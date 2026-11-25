/* pickScene: against a loop over every shape, its order, filters and errors. */
#include "pick_doc.h"

#include "coredcc/api/resolve.h"
#include "coredcc/bind/foreign_command.h"

test_init;

using namespace scene_test;
using litestl::math::transformPoint;
namespace sc = coredcc::script;

namespace {

int g_failures = 0;
bool expect(bool ok, const char *what)
{
  if (!ok && g_failures++ < 20) {
    fprintf(stderr, "  %s\n", what);
  }
  return ok;
}

/* ---- against the loop ---- */

/* One object's best: the distance in device pixels, and a depth if that is 0. */
Expected expectObject(const ViewMath &view, const Geo &g, const mat4d &world, double x, double y)
{
  Expected best;
  for (const Prim &p : g.prims) {
    Expected e;
    if (p.triangle) {
      const double3 w[3] = {transformPoint(world, p.q[0]),
                            transformPoint(world, p.q[1]),
                            transformPoint(world, p.q[2])};
      e = expectTriangle(view, w, x, y);
    }
    else {
      ScreenPoint s;
      e.visible = detail::closestOnCapsule(
          mapOf(view, world, x, y), p.q[0], p.q[1], p.radius, 1e300, s);
      e.distance = s.distance;
      e.depthKnown = false;
    }
    if (!e.visible) {
      continue;
    }
    if (!best.visible || e.distance < best.distance) {
      const bool keep = best.visible && best.distance == 0.0;
      best = keep ? best : e;
    }
    else if (e.distance == 0.0 && best.distance == 0.0) {
      best.depthKnown = best.depthKnown && e.depthKnown;
      best.depth = std::fmin(best.depth, e.depth);
    }
  }
  return best;
}

bool insideOf(const Geo &g, const Prim &p, const double3 &q)
{
  (void)g;
  return lengthOf(q - bvh::closestOnSegment(q, p.q[0], p.q[1])) <= p.radius;
}

/* A hit that covers the cursor says where the ray enters. For a solid, a bit nearer is outside. */
void checkEntry(const ViewMath &view,
                const Geo &g,
                const mat4d &world,
                const PickHit &hit,
                double x,
                double y)
{
  const ViewPoint at = view.project(hit.point);
  expect(hit.depth >= 0.0, "no depth is negative");
  expect(std::hypot(at.x - x, at.y - y) < 0.02, "the point of a hit is under the cursor");
  expect(std::fabs(view.viewDistance(hit.point) - hit.depth) < 1e-6 * (1.0 + hit.depth), "depth");
  const Prim &p = g.prims[size_t(hit.element)];
  mat4d inverse;
  if (p.triangle || !litestl::math::tryInverse(world, inverse)) {
    return;
  }
  const double3 q = transformPoint(inverse, hit.point);
  const double off = lengthOf(q - bvh::closestOnSegment(q, p.q[0], p.q[1])) - p.radius;
  expect(off < 1e-7, "the point of a hit is in the solid");
  if (hit.depth > 1e-5) {
    const Ray ray = view.rayFromPixel(x, y);
    const double t = (hit.point - ray.origin).dot(ray.dir);
    expect(off > -1e-7, "the point of a hit is on the solid's surface");
    expect(!insideOf(g, p, transformPoint(inverse, ray.origin + ray.dir * (t - 1e-5))),
           "a hit is where the ray enters");
  }
}

void testLoop(bool ortho, bool behindEye)
{
  Rand r(uint64_t(21 + ortho * 2 + behindEye));
  int covered = 0, near = 0, hidden = 0, depths = 0, singular = 0;
  for (int round = 0; round < 12 * coredcc_test::testScale(); round++) {
    PickDoc d;
    std::vector<BlockHandle> objects;
    for (int i = 0; i < 40; i++) {
      std::vector<Prim> prims;
      const bool triangles = r.coin();
      for (int k = 0; k < 12; k++) {
        const double3 c = r.vec(0.5);
        if (triangles) {
          prims.push_back(face(c + r.vec(0.3), c + r.vec(0.3), c + r.vec(0.3)));
        }
        else {
          prims.push_back(curve(c, r.coin() ? c : c + r.vec(0.3), r.range(0.0, 0.1)));
        }
      }
      const BlockHandle data = d.geoBlock("geo", prims);
      const mat4d world = solve::affine(randomLinear(r, int(r.below(kLinearKinds))), r.vec(3.0));
      objects.push_back(d.placed("object", world, data));
    }
    d.evaluate();
    for (int v = 0; v < 4; v++) {
      ViewCase c = randomView(r);
      c.ortho = ortho;
      c.zNear = ortho ? (behindEye ? -6.0 : 0.5) : behindEye ? 0.001 : 0.5;
      const ViewMath view = viewOf(c);
      for (int k = 0; k < 20; k++) {
        /* Near some object, most of the time. */
        const BlockHandle aim = objects[r.below(40)];
        const ViewPoint p = view.project(solve::translationOf(d.result(aim).state.world));
        const double x = p.visible && k % 4 ? std::round(p.x) : r.range(0.0, 2000.0);
        const double y = p.visible && k % 4 ? std::round(p.y) : r.range(0.0, 1100.0);
        const double radius = double(r.below(3)) * 10.0;
        Vector<PickHit> hits[2];
        for (int mode = 0; mode < 2; mode++) {
          for (BlockHandle h : objects) {
            d.geo(d.lib->find(d.obj(h).data.target())).useBvh = mode == 1;
          }
          hits[mode] = d.picks(view, x, y, radius);
        }
        expect(hits[0].size() == hits[1].size(), "all shapes, and those from the BVH");
        for (size_t i = 0; i < std::min(hits[0].size(), hits[1].size()); i++) {
          const PickHit &a = hits[0][int(i)], &b = hits[1][int(i)];
          expect(a.object == b.object && a.distance == b.distance && a.depth == b.depth,
                 "the same hit from the BVH");
        }
        for (size_t i = 1; i < hits[0].size(); i++) {
          const PickHit &a = hits[0][int(i) - 1], &b = hits[0][int(i)];
          expect(a.distance < b.distance || (a.distance == b.distance && a.depth <= b.depth),
                 "the order");
        }
        for (BlockHandle h : objects) {
          const mat4d world = d.result(h).state.world;
          const Geo &g = d.geo(d.lib->find(d.obj(h).data.target()));
          const Expected e = expectObject(view, g, world, x, y);
          const PickHit *hit = nullptr;
          for (const PickHit &candidate : hits[0]) {
            hit = candidate.object == h ? &candidate : hit;
          }
          const double limit = radius * c.dpiScale;
          mat4d inverse;
          if (e.visible && e.distance > 0.0 && std::fabs(e.distance - limit) < 0.02) {
            continue;
          }
          if (!e.visible || e.distance > limit) {
            hidden++;
            expect(!hit, "what is too far or not visible isn't hit");
            continue;
          }
          if (!expect(hit != nullptr, "what is near is hit")) {
            continue;
          }
          singular += !litestl::math::tryInverse(world, inverse);
          expect(std::fabs(hit->distance * c.dpiScale - e.distance) < 1e-4, "the distance");
          if (e.distance == 0.0) {
            covered++;
            expect(hit->covers(), "covered");
            checkEntry(view, g, world, *hit, x, y);
            if (e.depthKnown) {
              depths++;
              expect(std::fabs(hit->depth - e.depth) < 1e-5 * (1.0 + e.depth), "the depth");
            }
          }
          else {
            near++;
            expect(hit->depth >= 0.0, "no depth is negative");
            const ViewPoint at = view.project(hit->point);
            expect(std::fabs(std::hypot(at.x - x, at.y - y) - e.distance) < 1e-3, "the point");
          }
        }
      }
    }
  }
  printf("%s%s: %d covered (%d depths), %d near, %d hidden, %d singular\n",
         ortho ? "orthographic" : "perspective",
         behindEye ? ", near plane at or behind the eye" : "",
         covered,
         depths,
         near,
         hidden,
         singular);
  test_assert(covered > 60 && near > 20 && depths > 30 && singular > 20);
}

/* ---- bounds ---- */

void testBounds()
{
  PickDoc d;
  const BlockHandle shape = d.shapeBlock("shape", 1.0);
  const BlockHandle a = d.object("a");
  d.setData(a, shape);
  ViewCase c;
  c.zNear = 0.5;
  const ViewMath view = viewOf(c);
  d.evaluate();
  const ViewPoint mid = view.project(double3(0, 0, 0)), edge = view.project(double3(1, 0, 1));
  Vector<PickHit> hits = d.picks(view, mid.x, mid.y, 0.0);
  test_assert(hits.size() == 1 && hits[0].object == a && hits[0].covers());
  test_assert(hits[0].element == -1 && hits[0].domain == -1);
  test_assert(std::fabs(hits[0].depth - 8.5) < 1e-9 && std::fabs(hits[0].point[2] - 1.0) < 1e-9);
  /* In the object's space. */
  put(d.obj(a).channels, trs::kRotationEuler, double3(0.0, 0.0, 0.7));
  put(d.obj(a).channels, trs::kScale, double3(0.5, 2.0, 1.0));
  put(d.obj(a).channels, trs::kLocation, double3(3.0, 0.0, 0.0));
  d.evaluate();
  const ObjectEval e = d.result(a);
  const ViewPoint corner = view.project(double3(e.bounds.max[0], e.bounds.max[1], 1.0));
  test_assert(d.picks(view, corner.x - 3.0, corner.y + 3.0, 0.0).size() == 0);
  const double3 tip = transformPoint(e.state.world, double3(0.97, 0.97, 1.0));
  const ViewPoint in = view.project(tip);
  hits = d.picks(view, in.x, in.y, 0.0);
  test_assert(hits.size() == 1 && lengthOf(hits[0].point - tip) < 1e-9);
  d.obj(a).channels = d.trs->defaults();
  d.evaluate();
  /* Beside it. */
  hits = d.picks(view, edge.x + 7.0, edge.y, 10.0);
  test_assert(hits.size() == 1 && !hits[0].covers() && hits[0].distance > 5.0 &&
              hits[0].distance <= 7.0);
  test_assert(d.picks(view, edge.x + 12.0, edge.y, 10.0).size() == 0);
  /* From inside: the surface behind the near plane isn't there, the one before it is. */
  c.eye = double3(0.0, 0.0, 0.3);
  c.target = double3(0.0, 0.0, -5.0);
  const ViewMath inside = viewOf(c);
  hits = d.picks(inside, 1000.0, 560.0, 0.0);
  test_assert(hits.size() == 1 && std::fabs(hits[0].depth - 0.8) < 1e-9);
  /* Flat, and a point. */
  for (int axes = 1; axes <= 3; axes++) {
    Box3d flat;
    for (int i = 0; i < 3; i++) {
      flat.min[i] = i < axes ? 0.0 : -1.0;
      flat.max[i] = i < axes ? 0.0 : 1.0;
    }
    d.shape(shape).box = flat;
    d.evaluate();
    hits = d.picks(view, mid.x + 3.0, mid.y, 5.0);
    test_assert(hits.size() == 1 && hits[0].distance <= 3.0 + 1e-9);
  }
  /* None. */
  d.shape(shape).box = Box3d();
  d.evaluate();
  test_assert(d.picks(view, mid.x, mid.y, 5.0).size() == 0);
}

/* ---- the order ---- */

/* A curve a thousand times longer than it is wide, whose matrix is as uneven: its width on the
 * screen isn't its radius times the largest scale. */
void testThin()
{
  PickDoc d;
  ViewCase c;
  c.zNear = 0.1;
  const ViewMath view = viewOf(c);
  Linear wide;
  wide.col[0] = double3(1000.0, 0.0, 0.0);
  wide.col[1] = double3(0.0, 1.0, 0.0);
  wide.col[2] = double3(0.0, 0.0, 1.0);
  /* Before the other one, 8 pixels above the cursor. */
  const double3 x(1.0, 0.0, 0.0);
  const double pixel = view.pixelSizeAt(double3(0, 0, 1));
  const BlockHandle thin = d.placed(
      "thin",
      solve::affine(wide, double3(0.0, 8.0 * pixel, 1.0)),
      d.geoBlock("thin", {curve(x * -0.002, x * 0.002, pixel)}));
  const BlockHandle thick = d.placed(
      "thick", identity(), d.geoBlock("thick", {curve(x * -2.0, x * 2.0, 0.1)}));
  d.evaluate();
  const ViewPoint at = view.project(double3(0.0, 0.0, 1.0));
  Vector<PickHit> hits = d.picks(view, at.x, at.y, 10.0);
  test_assert(hits.size() == 2 && hits[0].object == thick && hits[1].object == thin);
  test_assert(hits[0].covers() && std::fabs(hits[1].distance - 7.0) < 0.05);
  /* Covered by both, the nearer one wins. */
  hits = d.picks(view, at.x, at.y - 8.0, 10.0);
  test_assert(hits.size() == 2 && hits[0].object == thin && hits[0].covers() && hits[1].covers());
  test_assert(hits[0].depth < hits[1].depth);
  /* Its end is where the matrix says: a ball a thousand pixels long. */
  const ViewPoint end = view.project(double3(2.0 + 1000.0 * pixel, 8.0 * pixel, 1.0));
  hits = d.picks(view, end.x - 5.0, end.y, 0.0);
  test_assert(hits.size() == 1 && hits[0].object == thin);
  test_assert(d.picks(view, end.x + 5.0, end.y, 4.9).size() == 0);
  hits = d.picks(view, end.x + 5.0, end.y, 5.1);
  test_assert(hits.size() == 1 && std::fabs(hits[0].distance - 5.0) < 0.02);
}

void testOrder()
{
  PickDoc d;
  const ViewMath view = viewOf(ViewCase());
  const double pixel = view.pixelSizeAt(double3(0, 0, 0));
  const auto at = [&](const char *name, double x, double z) {
    return d.placed(name,
                    solve::affine(Linear::identity(), double3(x * pixel, 0.0, z)),
                    d.geoBlock(name, {ball(double3(), 2.0 * pixel)}));
  };
  const BlockHandle missNear = at("missNear", 6.0, 0.0), missFar = at("missFar", -8.0, 0.0);
  const BlockHandle hitBack = at("hitBack", 0.0, -1.0), hitFront = at("hitFront", 0.0, 1.0);
  const BlockHandle twin = at("twin", 0.0, 1.0);
  d.evaluate();
  const ViewPoint p = view.project(double3());
  const Vector<PickHit> hits = d.picks(view, p.x, p.y, 10.0);
  const BlockHandle order[] = {hitFront, twin, hitBack, missNear, missFar};
  test_assert(hits.size() == 5);
  for (int i = 0; i < int(hits.size()) && i < 5; i++) {
    test_assert(hits[i].object == order[i] && hits[i].covers() == (i < 3));
  }
  /* The scene's order decides between twins. */
  must(d.sc().remove(*d.lib, d.idOf(hitFront)).status());
  must(d.sc().add(*d.lib, hitFront));
  d.evaluate();
  test_assert(d.picks(view, p.x, p.y, 10.0)[0].object == twin);
  /* Logical pixels. */
  ViewCase c;
  c.dpiScale = 2.0;
  const Vector<PickHit> dense = d.picks(viewOf(c), p.x, p.y, 2.5);
  test_assert(dense.size() == 4 && std::fabs(dense[3].distance - 2.0) < 0.01);

  /* Of one object's shapes: what covers, then the front one. An element's domain. */
  const BlockHandle many = d.placed("many",
                                    identity(),
                                    d.geoBlock("many",
                                               {ball(double3(0, 0, -1), 0.1),
                                                ball(double3(0, 0, 3), 0.1),
                                                ball(double3(4.0 * pixel, 0, 4), 0.001)}));
  d.evaluate();
  const Vector<PickHit> best = d.picks(view, p.x, p.y, 10.0);
  test_assert(best[0].object == many && best[0].element == 1);
  test_assert(best[0].domain == int32_t(ElementDomain::Edge));
  test_assert(std::fabs(best[0].point[2] - 3.1) < 1e-6);
}

/* The same shape under a matrix without an inverse, and one that nearly has none. */
void testFlattened()
{
  PickDoc d;
  const ViewMath view = viewOf(ViewCase());
  const ViewPoint p = view.project(double3(0.1, 0.1, 0.5));
  for (int i = 0; i < 16; i++) {
    Linear flat = Linear::identity();
    if (i < 3) {
      flat.col[2] = double3(0.0, 0.0, i == 0 ? 0.0 : i == 1 ? 1e-13 : 1e-9);
    }
    else {
      /* Almost in the plane of the other two. */
      flat.col[0] = double3(1.0, 1.0 / 3.0, 0.0);
      flat.col[2] = double3(0.7, 0.3, std::pow(10.0, -3.0 - i));
    }
    const BlockHandle h = d.placed("flat",
                                   solve::affine(flat, double3(0.0, 0.0, 0.5)),
                                   d.geoBlock("flat", {ball(double3(), 1.0)}));
    d.evaluate();
    const Vector<PickHit> hits = d.picks(view, p.x, p.y, 0.0);
    test_assert(hits.size() == 1 && hits[0].object == h && hits[0].covers());
    test_assert(lengthOf(hits[0].point - double3(0.1, 0.1, 0.5)) < 1e-4);
    must(d.sc().remove(*d.lib, d.idOf(h)).status());
  }
}

/* ---- the sink and the query ---- */

void testSink()
{
  const ViewMath view = viewOf(ViewCase());
  const ViewPoint p = view.project(double3(0.2, 0.0, 0.0));
  const ViewRegion region = view.regionFromPixel(p.x, p.y, 4.0);
  const PickQuery q(view, region, p.x, p.y, 4.0, identity());
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const double inf = std::numeric_limits<double>::infinity();
  const double3 at(0.2, 0.0, 0.0), bad(0.2, nan, 0.0), worse(inf, 0.0, 0.0);
  {
    PickSink s(q);
    test_assert(!s.any());
    s.point(bad);
    s.segment(at, worse);
    s.triangle(at, at, bad);
    s.sphere(at, -1.0);
    s.sphere(at, nan);
    s.sphere(at, inf);
    s.sphere(bad, 1.0);
    s.capsule(at, worse, 1.0);
    s.box(Box3d());
    s.point(double3(0.2, 0.0, 11.0)); /* behind */
    test_assert(!s.any());
    s.point(at);
    test_assert(s.any() && s.best().covers() && s.best().element == -1);
  }
  PickSink s(q);
  const double pixel = view.pixelSizeAt(at);
  s.element(0, 7);
  s.point(at + double3(0.0, 3.0 * pixel, 0.0));
  test_assert(s.any() && std::fabs(s.best().distance - 3.0) < 0.01 && s.best().element == 7);
  s.element(1, 8);
  s.segment(at + double3(-1.0, 2.0 * pixel, 0.0), at + double3(1.0, 2.0 * pixel, 0.0));
  test_assert(std::fabs(s.best().distance - 2.0) < 0.01 && s.best().element == 8);
  s.element(2, 9);
  s.triangle(at + double3(-1, -1, -2), at + double3(1, -1, -2), at + double3(0, 1, -2));
  test_assert(s.best().covers() && s.best().element == 9 && s.best().domain == 2);
  s.element(0, 10);
  s.sphere(at, pixel);
  test_assert(s.best().element == 10);
  s.element(0, 11);
  s.capsule(at + double3(0, 0, -1), at + double3(0, 0, 2), pixel);
  test_assert(s.best().element == 11);
  s.element(0, 12);
  Box3d box;
  box.include(0.0, -1.0, 3.0);
  box.include(1.0, 1.0, 4.0);
  s.box(box);
  test_assert(s.best().element == 12 && std::fabs(s.best().point[2] - 4.0) < 1e-9);
  /* The first of equals, and no empty box. */
  s.element(0, 13);
  s.box(box);
  std::swap(box.min[2], box.max[2]);
  box.min[2] = 6.0;
  s.box(box);
  test_assert(s.best().element == 12);

  /* For scripts. */
  test_assert(q.planeCount() == 5 && q.hasRay() && q.radiusPixels() == 4.0);
  for (int i = 0; i < 5; i++) {
    test_assert(q.planeNormal(i)[0] == q.planes()[size_t(i)].n[0]);
    test_assert(q.planeOffset(i) == q.planes()[size_t(i)].w);
    test_assert(q.planeNormal(i).dot(at) + q.planeOffset(i) > 0.0);
  }
  test_assert(q.planeOffset(5) == 0.0 && q.planeOffset(-1) == 0.0 && q.planeNormal(5)[0] == 0.0);
  test_assert(q.rayOrigin()[2] == q.ray().origin[2] && q.rayDirection()[2] == q.ray().dir[2]);
  test_assert(sc::foreignImplementable(detail::ifaceDesc<Spatial>()).ok());
}

/* Object-space planes and rays, under any matrix, at an offset. */
void testQuery()
{
  Rand r(31);
  for (int i = 0; i < 6000; i++) {
    ViewCase c = randomView(r);
    const bool border = i % 2 == 1;
    if (border) {
      c.dpiScale = 1.0;
    }
    const ViewMath view = viewOf(c);
    /* Half of them with a pivot that is far away, and the point at the border. */
    const double3 pivot = border ? r.vec(1e6) : double3();
    const mat4d world = border ?
                            solve::affine(Linear::identity(), double3() - pivot) :
                            solve::affine(randomLinear(r, int(r.below(kLinearKinds))), r.vec(3.0));
    const double3 local = pivot + r.vec(1.0), at = transformPoint(world, local);
    const ViewPoint p = view.project(at);
    if (!p.visible || p.distance < 1e-9 || std::fabs(p.x) > 1e6 || std::fabs(p.y) > 1e6) {
      continue;
    }
    const double x = p.x + (border ? 3.0 : r.range(-3.0, 3.0)), y = p.y + r.range(-3.0, 3.0);
    const PickQuery q(view, view.regionFromPixel(x, y, 3.0), x, y, 3.0, world);
    for (const BvhPlane &plane : q.planes()) {
      expect(plane.n.dot(local) + plane.w >= 0.0, "a point within the radius is in the region");
    }
    const PickQuery far(view, view.regionFromPixel(x + 100.0, y, 3.0), x + 100.0, y, 3.0, world);
    bool outside = false;
    for (const BvhPlane &plane : far.planes()) {
      outside |= plane.n.dot(local) + plane.w < 0.0;
    }
    expect(outside, "a point that is far away isn't");
    if (q.hasRay()) {
      const Ray ray = view.rayFromPixel(x, y);
      const double3 a = transformPoint(world, q.ray().origin + q.ray().dir * 2.0);
      expect(lengthOf(a - (ray.origin + ray.dir * 2.0)) < 1e-6, "the ray");
    }
  }
}

/* ---- filters ---- */

void testFilter()
{
  PickDoc d;
  const ViewMath view = viewOf(ViewCase());
  const BlockHandle data = d.geoBlock("ball", {ball(double3(), 0.5)});
  const BlockHandle a = d.placed("a", identity(), data), b = d.placed("b", identity(), data);
  const BlockHandle c = d.placed("c", identity(), data);
  d.obj(a).flags = ObjectFlags::Hidden;
  d.obj(b).flags = ObjectFlags::Unselectable;
  d.evaluate();
  const ViewPoint p = view.project(double3());
  const auto found = [&](const PickFilter &f) {
    std::string names;
    for (const PickHit &h : d.picks(view, p.x, p.y, 0.0, f)) {
      names += d.lib->get(h.object)->name().c_str();
    }
    return names;
  };
  PickFilter f;
  test_assert(found(f) == "c");
  f.hidden = true;
  test_assert(found(f) == "ac");
  f.unselectable = true;
  test_assert(found(f) == "abc");
  f.hidden = false;
  test_assert(found(f) == "bc");
  f.accept = [&](BlockHandle h) { return h != c; };
  test_assert(found(f) == "b");
  /* Flags are read now; they don't need another evaluation. */
  d.obj(c).flags = ObjectFlags::Hidden;
  f.accept = nullptr;
  test_assert(found(f) == "b");
  (void)a;
}

/* ---- errors ---- */

void testErrors()
{
  PickDoc d;
  const ViewMath view = viewOf(ViewCase());
  const BlockHandle data = d.geoBlock("ball", {ball(double3(), 0.5)});
  const BlockHandle throwing = d.geoBlock("throwing", {ball(double3(), 0.5)});
  const BlockHandle a = d.placed("a", identity(), throwing), b = d.placed("b", identity(), data);
  const ViewPoint p = view.project(double3());
  Vector<PickHit> hits;
  hits.append(PickHit());
  test_assert(isError(d.tryPick(view, p.x, p.y, 5.0, hits), StatusCode::FailedPrecondition, "evaluated"));
  test_assert(hits.size() == 0);
  d.evaluate();
  const double nan = std::numeric_limits<double>::quiet_NaN();
  for (int i = 0; i < 5; i++) {
    test_assert(isError(d.tryPick(i == 0 ? ViewMath() : view,
                                  i == 1 ? nan : p.x,
                                  i == 2 ? nan : p.y,
                                  i == 3 ? nan : i == 4 ? -1.0 : 5.0,
                                  hits),
                        StatusCode::InvalidArgument));
  }
  test_assert(isError(pickScene(*d.ctx, a, view, p.x, p.y, 5.0, {}, hits), StatusCode::NotFound));
  Context none(&d.rt, nullptr, PointerRef());
  test_assert(isError(pickScene(none, d.scene, view, p.x, p.y, 5.0, {}, hits),
                      StatusCode::FailedPrecondition));
  d.rt.setStickyError(boom("pending"));
  test_assert(isError(d.tryPick(view, p.x, p.y, 5.0, hits), StatusCode::FailedPrecondition, "pending"));
  d.rt.takeStickyError().ignore();

  /* Two that throw: the first is returned, the others' hits are there. */
  int calls = 0;
  d.geo(throwing).onPick = [&](const PickQuery &, PickSink &sink) {
    sink.sphere(double3(), 0.5);
    d.rt.setStickyError(boom(calls++ ? "second" : "first"));
  };
  const BlockHandle c = d.placed("c", identity(), throwing);
  d.evaluate();
  test_assert(isError(d.tryPick(view, p.x, p.y, 5.0, hits), StatusCode::Internal, "first"));
  test_assert(hits.size() == 1 && hits[0].object == b && calls == 2 && !d.rt.hasStickyError());
  (void)c;
  /* A read that throws, and a query. */
  d.geo(throwing).onPick = nullptr;
  const BlockHandle host = d.hostObject("host");
  d.host(host).info.data = d.idOf(data);
  d.evaluate();
  test_assert(d.picks(view, p.x, p.y, 5.0).size() == 4);
  d.host(host).onRead = [&](ObjectInfo &) { d.rt.setStickyError(boom("read")); };
  test_assert(isError(d.tryPick(view, p.x, p.y, 5.0, hits), StatusCode::Internal, "read"));
  test_assert(hits.size() == 3);
  d.host(host).onRead = nullptr;
  d.host(host).info.data = BlockId();
  d.query = [&](const Datablock &block, InterfaceId iface) {
    if (!strcmp(block.name().c_str(), "host") && iface == InterfaceId::of(Spatial::ifaceName)) {
      d.rt.setStickyError(boom("query"));
    }
    return QueryAnswer();
  };
  test_assert(isError(d.tryPick(view, p.x, p.y, 5.0, hits), StatusCode::Internal, "query"));
  test_assert(hits.size() == 3 && !d.rt.hasStickyError());
  d.query = nullptr;

  /* Nothing can be changed while it runs, and it can't run while something is evaluated. */
  Status inside[3];
  d.geo(throwing).onPick = [&](const PickQuery &, PickSink &) {
    Vector<PickHit> more;
    inside[0] = evaluateScene(*d.ctx);
    inside[1] = d.tryPick(view, p.x, p.y, 5.0, more);
    inside[2] = setValue(*d.ctx, "scene.frame", PropValue::ofFloat(3.0));
  };
  must(d.tryPick(view, p.x, p.y, 5.0, hits));
  for (const Status &s : inside) {
    test_assert(s.code() == StatusCode::Busy);
  }
  test_assert(!d.rt.evaluating() && d.lib->collect().ok());
  const BlockHandle shape = d.shapeBlock("shape", 1.0);
  d.setData(b, shape);
  Status during;
  d.shape(shape).onBounds = [&] { during = d.tryPick(view, p.x, p.y, 5.0, hits); };
  d.evaluate();
  test_assert(during.code() == StatusCode::Busy);
  /* While blocks are destroyed. */
  static std::function<void()> whenFreed;
  const BlockHandle doomed = d.shapeBlock("doomed", 1.0);
  d.slot(doomed).free = [](void *s) {
    whenFreed();
    delete static_cast<Shape *>(s);
  };
  during = Status();
  whenFreed = [&] { during = d.tryPick(view, p.x, p.y, 5.0, hits); };
  must(d.lib->markDeleted(doomed));
  must(d.lib->destroyFromHistory(doomed));
  test_assert(during.code() == StatusCode::Busy);
}

} // namespace

int main()
{
  testLoop(false, false);
  testLoop(false, true);
  testLoop(true, false);
  testLoop(true, true);
  testBounds();
  testThin();
  testOrder();
  testFlattened();
  testSink();
  testQuery();
  testFilter();
  testErrors();
  if (g_failures > 0) {
    fprintf(stderr, "%d failures\n", g_failures);
  }
  test_assert(g_failures == 0);
  return test_end();
}
