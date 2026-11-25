#pragma once

/* A scene document with a data block type that has shapes (test.Geo), and the answer to a pick by
 * looping over all of them. */

#include "pick_fixture.h"
#include "scene_doc.h"

#include "coredcc/bvh/kernels.h"
#include "coredcc/scene/pick.h"

#include <utility>

namespace scene_test {

/* Coordinates of one shape: a triangle's corners, or a capsule's ends. */
struct Prim {
  double3 q[3];
  double radius = 0.0;
  bool triangle = true;
};

struct Geo {
  std::vector<Prim> prims;
  Box3d box;
  /* Gives the sink only what is in the query's planes, and what the ray hits first. */
  bool useBvh = false;
  GeometryBvh<TriangleSource<double>> triangles;
  GeometryBvh<CapsuleSource<double>> capsules;
  std::function<void(const PickQuery &, PickSink &)> onPick;
  mutable int given = 0;

  void build()
  {
    TriangleSource<double> t;
    CapsuleSource<double> c;
    box = Box3d();
    for (const Prim &p : prims) {
      /* Both have all: a shape of the other kind is one that is left out. */
      const double none = std::numeric_limits<double>::quiet_NaN();
      const double3 nan(none, none, none);
      t.append(p.triangle ? p.q[0] : nan, p.q[1], p.q[2]);
      c.append(p.triangle ? nan : p.q[0], p.q[1], p.radius);
      for (int i = 0; i < (p.triangle ? 3 : 2); i++) {
        box.include(p.q[i][0] - p.radius, p.q[i][1] - p.radius, p.q[i][2] - p.radius);
        box.include(p.q[i][0] + p.radius, p.q[i][1] + p.radius, p.q[i][2] + p.radius);
      }
    }
    BvhOptions small;
    small.leafSize = 2;
    must(triangles.build(t, small));
    must(capsules.build(c, small));
  }
  void give(PickSink &sink, int i) const
  {
    const Prim &p = prims[size_t(i)];
    given++;
    sink.element(int32_t(p.triangle ? ElementDomain::Face : ElementDomain::Edge), i);
    if (p.triangle) {
      sink.triangle(p.q[0], p.q[1], p.q[2]);
    }
    else {
      sink.capsule(p.q[0], p.q[1], p.radius);
    }
  }

  Box3d localBounds() const
  {
    return box;
  }
  uint32_t domains() const
  {
    return 4;
  }
  Vector<int32_t> closestElements(const double3 &, double, int32_t) const
  {
    return Vector<int32_t>();
  }
  void pick(const PickQuery &query, PickSink &sink) const
  {
    if (onPick) {
      onPick(query, sink);
      return;
    }
    if (!useBvh) {
      for (int i = 0; i < int(prims.size()); i++) {
        give(sink, i);
      }
      return;
    }
    const auto near = [&](int i) {
      give(sink, i);
      return true;
    };
    must(triangles.region(query.planes(), near));
    must(capsules.region(query.planes(), near));
    if (query.hasRay()) {
      for (const RayHit &hit :
           {triangles.rayClosest(query.ray()), capsules.rayClosest(query.ray())})
      {
        if (hit) {
          give(sink, hit.primitive);
        }
      }
    }
  }
  static lbt::Struct<Geo> *defineBindings()
  {
    auto *st = new lbt::Struct<Geo>("test::Geo", sizeof(Geo));
    BIND_STRUCT_METHOD(st, localBounds, MARGS());
    BIND_STRUCT_METHOD(st, domains, MARGS());
    BIND_STRUCT_METHOD(st, closestElements, MARGS("co", "radius", "domain"));
    BIND_STRUCT_METHOD(st, pick, MARGS("query", "sink"));
    return st;
  }
};

struct PickDoc : Doc {
  PickDoc()
  {
    addSlotType("test.Geo");
    must(registerImpl<Spatial, Geo>(rt, "test.Geo"));
  }

  Geo &geo(BlockHandle h)
  {
    return *static_cast<Geo *>(slot(h).data);
  }
  BlockHandle geoBlock(const char *name, std::vector<Prim> prims)
  {
    const BlockHandle h = addSlot<Geo>("test.Geo", name);
    geo(h).prims = std::move(prims);
    geo(h).build();
    return h;
  }
  /** In the scene, with any affine matrix. */
  BlockHandle placed(const char *name, const mat4d &world, BlockHandle data)
  {
    Result<BlockHandle> h = newObject(*lib, name, kMatrixTransform);
    must(h.status());
    const double *m = world;
    for (int c = 0; c < 4; c++) {
      put(obj(*h).channels, c * 3, double3(m[c * 4], m[c * 4 + 1], m[c * 4 + 2]));
    }
    setData(*h, data);
    must(sc().add(*lib, *h));
    return *h;
  }
  Status tryPick(const ViewMath &view,
                 double x,
                 double y,
                 double radius,
                 Vector<PickHit> &hits,
                 const PickFilter &filter = {})
  {
    return pickScene(*ctx, scene, view, x, y, radius, filter, hits);
  }
  Vector<PickHit> picks(const ViewMath &view,
                        double x,
                        double y,
                        double radius,
                        const PickFilter &filter = {})
  {
    Vector<PickHit> hits;
    must(tryPick(view, x, y, radius, hits, filter));
    return hits;
  }
};

inline Prim ball(const double3 &center, double radius)
{
  Prim p;
  p.q[0] = p.q[1] = center;
  p.radius = radius;
  p.triangle = false;
  return p;
}
inline Prim curve(const double3 &a, const double3 &b, double radius)
{
  Prim p = ball(a, radius);
  p.q[1] = b;
  return p;
}
inline Prim face(const double3 &a, const double3 &b, const double3 &c)
{
  Prim p;
  p.q[0] = a;
  p.q[1] = b;
  p.q[2] = c;
  return p;
}

/* ---- the loop ---- */

/** What is nearest of one shape: device pixels, and the depth there. */
struct Expected {
  bool visible = false;
  double distance = 0.0;
  double depth = 0.0;
  /** Whether the depth is worth comparing: not of triangles seen edge on. */
  bool depthKnown = true;
};

/** A triangle in the world: clipped to the near plane, projected, its outline walked. */
inline Expected expectTriangle(const ViewMath &view, const double3 (&w)[3], double x, double y)
{
  Expected e;
  std::vector<std::pair<long double, long double>> outline;
  for (int i = 0; i < 3; i++) {
    const double3 a = w[i], b = w[(i + 1) % 3];
    const double da = view.viewDistance(a), db = view.viewDistance(b);
    const auto add = [&](const double3 &p) {
      const ViewPoint v = view.project(p);
      outline.push_back({(long double)(v.x) - x, (long double)(v.y) - y});
    };
    if (da >= 0.0) {
      add(a);
    }
    if ((da >= 0.0) != (db >= 0.0)) {
      add(da >= 0.0 ? a + (b - a) * (da / (da - db)) : b + (a - b) * (db / (db - da)));
    }
  }
  if (outline.empty()) {
    return e;
  }
  e.visible = true;
  long double best = 1e300L, area = 0, around = 0;
  int left = 0, right = 0;
  const size_t n = outline.size();
  for (size_t i = 0; i < n; i++) {
    const auto [ax, ay] = outline[i];
    const auto [bx, by] = outline[(i + 1) % n];
    const long double dx = bx - ax, dy = by - ay, dd = dx * dx + dy * dy;
    long double t = dd > 0 ? -(ax * dx + ay * dy) / dd : 0;
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    best = std::min(best, std::hypot(ax + dx * t, ay + dy * t));
    const long double side = ax * by - ay * bx;
    area += side;
    around += std::hypot(dx, dy);
    left += side >= 0;
    right += side <= 0;
  }
  const bool flat = std::fabs(double(area)) <= 1e-6 * double(around);
  const bool inside = !flat && (left == int(n) || right == int(n));
  e.distance = inside ? 0.0 : double(best);
  /* The depth: Moeller and Trumbore's test. */
  const Ray ray = view.rayFromPixel(x, y);
  const double3 ab = w[1] - w[0], ac = w[2] - w[0], across = ray.dir.cross(ac);
  const double det = ab.dot(across);
  e.depthKnown = std::fabs(det) > 1e-4 * lengthOf(ab) * lengthOf(ac);
  if (inside && e.depthKnown) {
    const double t = ac.dot((ray.origin - w[0]).cross(ab)) / det;
    e.depth = view.viewDistance(ray.origin + ray.dir * t);
  }
  return e;
}

} // namespace scene_test
