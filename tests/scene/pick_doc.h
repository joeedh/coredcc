#pragma once

/* A scene document with a data block type that has shapes (test.Geo), and the answer to a
 * pick by looping over all of them. */

#include "pick_fixture.h"
#include "scene_doc.h"

#include "coredcc/bvh/kernels.h"
#include "coredcc/scene/capabilities.h"
#include "coredcc/scene/pick_scene.h"

#include <utility>

namespace scene_test {

enum class Kind { Triangle, Capsule, Point, Segment };

/* Coordinates of one shape: a triangle's corners, a capsule's or a segment's ends, or a
 * point. */
struct Prim {
  double3 q[3];
  double radius = 0.0;
  Kind kind = Kind::Triangle;
  double pixels = 0.0;

  int corners() const
  {
    return kind == Kind::Triangle ? 3 : kind == Kind::Point ? 1 : 2;
  }
  ElementDomain domain() const
  {
    return kind == Kind::Triangle ? ElementDomain::Face
           : kind == Kind::Point  ? ElementDomain::Point
                                  : ElementDomain::Edge;
  }
};

struct Geo {
  std::vector<Prim> prims;
  Box3d box;
  /* Gives the sinks only what is in the query's planes or radius, and what the ray hits
   * first. */
  bool useBvh = false;
  /* With it: no triangles but what the ray hits first, if no others are wanted. */
  bool useRay = false;
  GeometryBvh<TriangleSource<double>> triangles;
  GeometryBvh<CapsuleSource<double>> capsules;
  std::function<void(const PickQuery &, PickSink &)> onPick;
  /* closestElements and elementsInRegion. */
  std::function<void()> onNear;
  std::function<void()> onDomains;
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
      const bool triangle = p.kind == Kind::Triangle;
      t.append(triangle ? p.q[0] : nan, p.q[1], p.q[2]);
      c.append(triangle ? nan : p.q[0], p.q[p.kind == Kind::Point ? 0 : 1], p.radius);
      for (int i = 0; i < p.corners(); i++) {
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
    sink.element(p.domain(), i);
    switch (p.kind) {
    case Kind::Triangle:
      sink.triangle(p.q[0], p.q[1], p.q[2]);
      break;
    case Kind::Capsule:
      sink.capsule(p.q[0], p.q[1], p.radius);
      break;
    case Kind::Point:
      sink.point(p.q[0], p.pixels);
      break;
    case Kind::Segment:
      sink.segment(p.q[0], p.q[1], p.pixels);
      break;
    }
  }

  Box3d localBounds() const
  {
    return box;
  }
  uint32_t domains() const
  {
    if (onDomains) {
      onDomains();
    }
    return 7;
  }
  /* Exact for what moves and turns, and for points. */
  void closestElements(const NearQuery &query, NearSink &sink) const
  {
    if (onNear) {
      onNear();
    }
    const auto near = [&](int i) {
      const Prim &p = prims[size_t(i)];
      if (p.domain() != query.domain()) {
        return true;
      }
      const double3 &a = p.q[0];
      const double3 at = p.kind == Kind::Triangle
                             ? bvh::closestOnTriangle(query.point(), a, p.q[1], p.q[2])
                         : p.kind == Kind::Point
                             ? a
                             : bvh::closestOnSegment(query.point(), a, p.q[1]);
      if (useBvh && lengthOf(at - query.point()) > sink.radius()) {
        return true;
      }
      given++;
      sink.add(i, at);
      return true;
    };
    if (!useBvh || !bvh::finite(query.radius())) {
      for (int i = 0; i < int(prims.size()); i++) {
        near(i);
      }
      return;
    }
    must(triangles.sphereOverlap(query.point(), query.radius(), near));
    must(capsules.sphereOverlap(query.point(), query.radius(), near));
  }
  Vector<int32_t> elementsInRegion(const RegionQuery &query, ElementDomain domain) const
  {
    if (onNear) {
      onNear();
    }
    Vector<int32_t> out;
    const auto in = [&](int i) {
      if (prims[size_t(i)].domain() == domain) {
        out.append(i);
      }
      return true;
    };
    must(triangles.region(query.planes(), in));
    must(capsules.region(query.planes(), in));
    return out;
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
    if (!useRay || !query.hasRay() || query.surfaceRadiusDevicePixels() > 0.0) {
      must(triangles.region(query.planes(), near));
    }
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
    BIND_STRUCT_METHOD(st, closestElements, MARGS("query", "sink"));
    BIND_STRUCT_METHOD(st, pick, MARGS("query", "sink"));
    BIND_STRUCT_METHOD(st, elementsInRegion, MARGS("query", "domain"));
    return st;
  }
};

/** Without `thin`, no solid is. */
inline PickOptions within(double radius, bool thin = true)
{
  PickOptions o;
  o.radius = radius;
  o.thinRadius = thin ? o.thinRadius : 0.0;
  return o;
}

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
                 const PickOptions &options,
                 Vector<PickHit> &hits,
                 const PickFilter &filter = {})
  {
    return pickScene(*ctx, scene, view, x, y, options, filter, hits);
  }
  Vector<PickHit> picks(const ViewMath &view,
                        double x,
                        double y,
                        const PickOptions &options,
                        const PickFilter &filter = {})
  {
    Vector<PickHit> hits;
    must(tryPick(view, x, y, options, hits, filter));
    return hits;
  }
};

inline Prim ball(const double3 &center, double radius)
{
  Prim p;
  p.q[0] = p.q[1] = center;
  p.radius = radius;
  p.kind = Kind::Capsule;
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
  double viewDistance = 0.0;
  /** Whether the depth is worth comparing: not of triangles seen edge on. */
  bool depthKnown = true;
};

/** A triangle in the world: clipped to the near and the far plane, projected, its outline
 * walked. */
inline Expected
expectTriangle(const ViewMath &view, const double3 (&w)[3], double x, double y)
{
  Expected e;
  std::vector<std::pair<long double, long double>> outline;
  std::vector<double3> polygon(w, w + 3);
  const double4 &far = view.farPlane();
  const double length = std::hypot(far[0], far[1], far[2]);
  for (int plane = 0; plane < (length > 0.0 ? 2 : 1); plane++) {
    const auto side = [&](const double3 &p) {
      return plane == 0 ? view.viewDistance(p) : dotPoint(far, p) / length;
    };
    std::vector<double3> in;
    for (size_t i = 0; i < polygon.size(); i++) {
      const double3 a = polygon[i], b = polygon[(i + 1) % polygon.size()];
      const double da = side(a), db = side(b);
      if (da >= 0.0) {
        in.push_back(a);
      }
      if ((da >= 0.0) != (db >= 0.0)) {
        in.push_back(da >= 0.0 ? a + (b - a) * (da / (da - db))
                               : b + (a - b) * (db / (db - da)));
      }
    }
    polygon = in;
  }
  for (const double3 &p : polygon) {
    const ViewPoint v = view.project(p);
    outline.push_back({(long double)(v.x) - x, (long double)(v.y) - y});
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
    e.viewDistance = view.viewDistance(ray.origin + ray.dir * t);
  }
  return e;
}

} // namespace scene_test
