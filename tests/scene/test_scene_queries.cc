/* nearestInScene, pickRegion and SceneBvh's object queries, against loops over every
 * shape. */
#include "pick_doc.h"

#include "coredcc/bind/foreign_command.h"

test_init;

using namespace scene_test;
using litestl::math::transformPoint;

namespace {

int g_failures = 0;
bool expect(bool ok, const char *what)
{
  if (!ok && g_failures++ < 20) {
    fprintf(stderr, "  %s\n", what);
  }
  return ok;
}

double3 closestOn(const Prim &p, const mat4d &world, const double3 &to)
{
  const double3 a = transformPoint(world, p.q[0]);
  if (p.kind == Kind::Point) {
    return a;
  }
  const double3 b = transformPoint(world, p.q[1]);
  return p.kind == Kind::Triangle
             ? bvh::closestOnTriangle(to, a, b, transformPoint(world, p.q[2]))
             : bvh::closestOnSegment(to, a, b);
}

std::vector<Prim> randomPrims(Rand &r, int count)
{
  std::vector<Prim> prims;
  for (int k = 0; k < count; k++) {
    Prim p;
    const int kind = int(r.below(3));
    p.kind = kind == 0 ? Kind::Triangle : kind == 1 ? Kind::Point : Kind::Segment;
    const double3 c = r.vec(0.8);
    for (double3 &q : p.q) {
      q = c + r.vec(0.3);
    }
    prims.push_back(p);
  }
  return prims;
}

/* ---- nearestInScene ---- */

void testNearest()
{
  Rand r(61);
  int found = 0, full = 0, none = 0, given[2] = {}, singular = 0;
  for (int round = 0; round < 12 * coredcc_test::testScale(); round++) {
    PickDoc d;
    std::vector<BlockHandle> objects;
    /* Edges and faces are exact only in objects that aren't stretched. */
    const bool any = r.coin();
    for (int i = 0; i < 30; i++) {
      mat4d world =
          solve::affine(randomLinear(r, int(r.below(kLinearKinds))), r.vec(3.0));
      if (!any) {
        Linear m;
        const quatd q = r.rotation();
        const double size = r.range(0.2, 3.0);
        for (int c = 0; c < 3; c++) {
          double3 axis;
          axis[c] = size;
          m.col[c] = q.rotate(axis);
        }
        world = solve::affine(m, r.vec(3.0));
      }
      objects.push_back(d.placed("object", world, d.geoBlock("geo", randomPrims(r, 15))));
    }
    d.evaluate();
    for (int k = 0; k < 60; k++) {
      const double3 p = r.vec(4.0);
      const double infinity = std::numeric_limits<double>::infinity();
      const double radius = r.below(5) == 0   ? infinity
                            : r.below(5) == 0 ? 0.0
                                              : r.range(0.1, 3.0);
      NearestOptions o;
      o.domain = any ? ElementDomain::Point : ElementDomain(r.below(3));
      o.maxCount = 1 + int32_t(r.below(r.coin() ? 3 : 600));
      struct Want {
        double distance;
        int place, element;
      };
      std::vector<Want> want;
      for (int place = 0; place < 30; place++) {
        const mat4d world = d.result(objects[size_t(place)]).state.world;
        const Geo &g = d.geo(d.lib->find(d.obj(objects[size_t(place)]).data.target()));
        mat4d inverse;
        singular += !litestl::math::tryInverse(world, inverse);
        for (int i = 0; i < int(g.prims.size()); i++) {
          if (g.prims[size_t(i)].domain() == o.domain) {
            const double at = lengthOf(p - closestOn(g.prims[size_t(i)], world, p));
            if (at <= radius) {
              want.push_back({at, place, i});
            }
          }
        }
      }
      std::sort(want.begin(), want.end(), [](const Want &a, const Want &b) {
        return a.distance < b.distance;
      });
      if (int(want.size()) > o.maxCount) {
        want.resize(size_t(o.maxCount));
        full++;
      }
      none += want.empty();
      Vector<NearHit> hits[2];
      for (int mode = 0; mode < 2; mode++) {
        for (BlockHandle h : objects) {
          Geo &g = d.geo(d.lib->find(d.obj(h).data.target()));
          g.useBvh = mode == 1;
          g.given = 0;
        }
        must(nearestInScene(*d.ctx, d.scene, p, radius, o, {}, hits[mode]));
        for (BlockHandle h : objects) {
          given[mode] += d.geo(d.lib->find(d.obj(h).data.target())).given;
        }
        if (!expect(hits[mode].size() == want.size(), "how many")) {
          continue;
        }
        for (int i = 0; i < int(want.size()); i++) {
          const NearHit &h = hits[mode][i];
          found++;
          expect(std::fabs(h.distance - want[size_t(i)].distance) < 1e-7, "the distance");
          expect(std::fabs(lengthOf(h.point - p) - h.distance) < 1e-12, "the point");
          expect(h.domain == o.domain, "the domain");
          if (i > 0) {
            const NearHit &before = hits[mode][i - 1];
            expect(before.distance <= h.distance, "the order");
          }
        }
      }
    }
  }
  printf("nearest: %d hits, %d with more than wanted, %d without, %d singular, %d and %d "
         "given\n",
         found,
         full,
         none,
         singular,
         given[0],
         given[1]);
  test_assert(found > 5000 && full > 50 && none > 10 && singular > 100 &&
              given[1] * 3 < given[0] * 2);
}

void testNearestCases()
{
  PickDoc d;
  const double infinity = std::numeric_limits<double>::infinity();
  const double nan = std::numeric_limits<double>::quiet_NaN();
  Prim p;
  p.kind = Kind::Point;
  p.q[0] = double3(1.0, 0.0, 0.0);
  const BlockHandle data = d.geoBlock("point", {p, p});
  const BlockHandle a = d.placed("a", identity(), data),
                    b = d.placed("b", identity(), data);
  const BlockHandle hidden = d.placed("hidden", identity(), data);
  d.obj(hidden).flags = ObjectFlags::Hidden;
  NearestOptions o;
  Vector<NearHit> hits;
  const double3 zero;
  test_assert(isError(nearestInScene(*d.ctx, d.scene, zero, 1.0, o, {}, hits),
                      StatusCode::FailedPrecondition,
                      "nearestInScene: the scene hasn't been evaluated"));
  d.evaluate();
  /* Ties: the scene's order, then the elements'. Exactly at the radius. */
  o.maxCount = 3;
  must(nearestInScene(*d.ctx, d.scene, zero, 1.0, o, {}, hits));
  test_assert(hits.size() == 3 && hits[0].object == a && hits[0].element == 0);
  test_assert(hits[1].object == a && hits[1].element == 1 && hits[2].object == b);
  test_assert(hits[0].distance == 1.0 && hits[0].point[0] == 1.0 &&
              hits[0].domain == ElementDomain::Point);
  must(nearestInScene(*d.ctx, d.scene, zero, 0.999, o, {}, hits));
  test_assert(hits.size() == 0);
  PickFilter f;
  f.hidden = true;
  f.accept = [&](BlockHandle h) { return h != a; };
  o.maxCount = 10;
  must(nearestInScene(*d.ctx, d.scene, zero, infinity, o, f, hits));
  test_assert(hits.size() == 4 && hits[0].object == b && hits[2].object == hidden);
  /* Not asked for a domain it doesn't have. */
  o.domain = ElementDomain(5);
  int others = 0;
  d.geo(data).onNear = [&] { others++; };
  must(nearestInScene(*d.ctx, d.scene, zero, infinity, o, {}, hits));
  test_assert(hits.size() == 0 && others == 0);
  /* Bad input. */
  for (int i = 0; i < 6; i++) {
    NearestOptions bad;
    bad.domain = ElementDomain(i == 2 ? -1 : i == 3 ? 32 : 0);
    bad.maxCount = i == 4 ? 0 : 1;
    test_assert(isError(nearestInScene(*d.ctx,
                                       d.scene,
                                       i == 5 ? double3(0.0, infinity, 0.0) : zero,
                                       i == 0   ? nan
                                       : i == 1 ? -1.0
                                                : 1.0,
                                       bad,
                                       {},
                                       hits),
                        StatusCode::InvalidArgument));
  }
  /* What a Spatial gives that isn't valid, or is too much. */
  o = NearestOptions();
  o.maxCount = 2;
  const NearQuery q(zero, 2.0, o, identity());
  NearSink sink(q);
  sink.add(0, double3(nan, 0, 0));
  sink.add(1, double3(0, infinity, 0));
  sink.add(2, double3(2.1, 0, 0));
  test_assert(sink.hits().size() == 0 && sink.radius() == q.radius() &&
              q.radius() >= 2.0);
  sink.add(3, double3(2.0, 0, 0));
  test_assert(sink.radius() == q.radius());
  sink.add(4, double3(0.5, 0, 0));
  sink.add(5, double3(1.0, 0, 0));
  sink.add(6, double3(1.5, 0, 0));
  test_assert(sink.hits().size() == 2 && sink.hits()[0].element == 4 &&
              sink.hits()[1].element == 5);
  test_assert(sink.radius() >= 1.0 && sink.radius() < 2.0);
  /* Equally far. */
  sink.add(3, double3(0, 1.0, 0));
  test_assert(sink.hits()[1].element == 3);
  sink.add(7, double3(0, 0, 1.0));
  test_assert(sink.hits()[1].element == 3 && sink.hits().size() == 2);
  Vector<int32_t> elements;
  Vector<double> positions;
  elements.append(9);
  for (int i = 0; i < 3; i++) {
    positions.append(0.1);
  }
  must(sink.addAll(elements, positions));
  test_assert(sink.hits()[0].element == 9);
  elements.append(10);
  test_assert(
      isError(sink.addAll(elements, positions), StatusCode::InvalidArgument, "three"));
  test_assert(NearSink::defineBindings() == NearSink::defineBindings());
  test_assert(NearQuery::defineBindings() == NearQuery::defineBindings());
  /* One that throws. */
  d.evaluate();
  d.geo(data).onNear = [&] { d.rt.setStickyError(boom("near")); };
  test_assert(
      isError(nearestInScene(*d.ctx, d.scene, zero, 2.0, NearestOptions(), {}, hits),
              StatusCode::Internal,
              "near"));
  test_assert(hits.size() == 0 && !d.rt.hasStickyError());
  /* If it already throws when it is asked for its domains, it isn't asked for more. */
  int asked = 0;
  d.geo(data).onNear = [&] { asked++; };
  d.geo(data).onDomains = [&] { d.rt.setStickyError(boom("domains")); };
  test_assert(
      isError(nearestInScene(*d.ctx, d.scene, zero, 2.0, NearestOptions(), {}, hits),
              StatusCode::Internal,
              "domains"));
  test_assert(hits.size() == 0 && asked == 0);
  Vector<RegionHit> region;
  ViewRegion everywhere;
  for (BvhPlane &plane : everywhere.planes) {
    plane.w = 1.0;
  }
  test_assert(
      isError(pickRegion(*d.ctx, d.scene, everywhere, ElementDomain::None, {}, region),
              StatusCode::Internal,
              "domains"));
  test_assert(region.size() == 0 && asked == 0 && !d.rt.hasStickyError());
  d.geo(data).onDomains = nullptr;
  must(pickRegion(*d.ctx, d.scene, everywhere, ElementDomain::None, {}, region));
  test_assert(region.size() == 2 && asked > 0);
}

/* The order in which objects are asked doesn't matter, and what is too far away isn't. */
void testNearestOrder()
{
  PickDoc d;
  const auto at = [&](std::initializer_list<double3> points) {
    std::vector<Prim> prims;
    for (const double3 &q : points) {
      Prim p;
      p.kind = Kind::Point;
      p.q[0] = q;
      prims.push_back(p);
    }
    return d.placed("object", identity(), d.geoBlock("points", prims));
  };
  const BlockHandle first = at({double3(0, 1, 0)});
  at({double3(1, 0, 0), double3(-3, 0, 0)}); /* its bounds are nearer */
  const BlockHandle far = at({double3(0, 0, 1.5)});
  d.evaluate();
  Vector<NearHit> hits;
  must(nearestInScene(*d.ctx, d.scene, double3(), 2.0, NearestOptions(), {}, hits));
  test_assert(hits.size() == 1 && hits[0].object == first);
  test_assert(d.geo(d.lib->find(d.obj(far).data.target())).given == 0);
}

/* Far from the origin, with small radii: what is near enough in the world is within the
 * radius in the object, also when the sink has what it needs. */
void testNearRounding()
{
  Rand r(64);
  int beyond = 0;
  for (int k = 0; k < 20000; k++) {
    const mat4d world = solve::affine(randomLinear(r, int(r.below(3))), r.vec(1e7));
    const double3 local = r.vec(1.0);
    const double3 there = transformPoint(world, local);
    NearestOptions o;
    const double radius = std::pow(10.0, r.range(-10.0, -3.0));
    /* Around a point that is near the element. */
    const double3 offset = r.vec(1.0);
    const double3 from = there + offset * (radius / lengthOf(offset));
    const NearQuery q(from, double3(there - from).length(), o, world);
    const double reach = lengthOf(local - q.point());
    expect(reach <= q.radius(), "the radius in the object");
    NearSink sink(q);
    sink.add(1, local);
    expect(sink.hits().size() == 1 && reach <= sink.radius(), "the sink's radius");
    beyond += reach > radius;
    const NearQuery wide(from, 1e6, o, world);
    NearSink second(wide);
    second.add(1, local);
    expect(second.radius() < wide.radius() && reach <= second.radius(),
           "a smaller radius");
  }
  test_assert(beyond > 100);
}

/* ---- regions ---- */

void testRegion()
{
  Rand r(62);
  int in = 0, out = 0, elements = 0;
  for (int round = 0; round < 12 * coredcc_test::testScale(); round++) {
    PickDoc d;
    std::vector<BlockHandle> objects;
    for (int i = 0; i < 30; i++) {
      const mat4d world =
          solve::affine(randomLinear(r, int(r.below(kLinearKinds))), r.vec(3.0));
      if (i % 5 == 4) {
        objects.push_back(d.placed("box", world, d.shapeBlock("shape", 0.2)));
      } else {
        objects.push_back(
            d.placed("object", world, d.geoBlock("geo", randomPrims(r, 10))));
      }
    }
    d.evaluate();
    for (int k = 0; k < 30; k++) {
      ViewCase c = randomView(r);
      c.eye = r.vec(6.0);
      c.target = r.vec(0.5);
      const ViewMath view = viewOf(c);
      const double x0 = r.range(0.0, 2000.0), y0 = r.range(0.0, 1100.0);
      const double x1 = r.range(0.0, 2000.0), y1 = r.range(0.0, 1100.0);
      const ViewRegion region = view.regionFromRect(x0, y0, x1, y1);
      const double left = std::fmin(x0, x1), right = std::fmax(x0, x1);
      const double top = std::fmin(y0, y1), bottom = std::fmax(y0, y1);
      const int32_t domain = int32_t(r.below(4)) - 1;
      Vector<RegionHit> hits;
      must(pickRegion(*d.ctx, d.scene, region, ElementDomain(domain), {}, hits));
      int last = -1;
      for (const RegionHit &h : hits) {
        const int place =
            int(std::find(objects.begin(), objects.end(), h.object) - objects.begin());
        expect(place > last, "the order");
        last = place;
        expect((domain < 0) == (h.elements.size() == 0), "elements only if asked for");
        expect(domain < 0 || place % 5 != 4, "boxes have no elements");
      }
      for (int place = 0; place < 30; place++) {
        const BlockHandle h = objects[size_t(place)];
        const RegionHit *hit = nullptr;
        for (const RegionHit &candidate : hits) {
          hit = candidate.object == h ? &candidate : hit;
        }
        const mat4d world = d.result(h).state.world;
        /* Points of the shapes: surely in the rectangle, and all of them surely on one
         * side. */
        const auto judge =
            [&](const std::vector<double3> &points, bool &inside, bool &outside) {
              inside = false;
              bool sides[6] = {true, true, true, true, true, true};
              const double4 &far = view.farPlane();
              const double farLength = std::hypot(far[0], far[1], far[2]);
              for (const double3 &q : points) {
                const double3 w = transformPoint(world, q);
                const ViewPoint v = view.project(w);
                const double past = farLength > 0.0 ? dotPoint(far, w) / farLength : 1.0;
                inside = inside || (v.visible && v.x > left && v.x < right && v.y > top &&
                                    v.y < bottom);
                sides[0] = sides[0] && v.visible && v.x < left - 1.0;
                sides[1] = sides[1] && v.visible && v.x > right + 1.0;
                sides[2] = sides[2] && v.visible && v.y < top - 1.0;
                sides[3] = sides[3] && v.visible && v.y > bottom + 1.0;
                sides[4] = sides[4] && v.viewDistance < -1e-6;
                sides[5] = sides[5] && past < -1e-6;
              }
              outside = false;
              for (bool side : sides) {
                outside = outside || side;
              }
            };
        std::vector<std::vector<double3>> shapes;
        if (place % 5 == 4) {
          if (domain >= 0) {
            continue;
          }
          std::vector<double3> corners;
          for (int i = 0; i < 8; i++) {
            corners.push_back(
                double3(i & 1 ? 0.2 : -0.2, i & 2 ? 0.2 : -0.2, i & 4 ? 0.2 : -0.2));
          }
          shapes.push_back(corners);
        } else {
          for (const Prim &p : d.geo(d.lib->find(d.obj(h).data.target())).prims) {
            shapes.push_back(std::vector<double3>(p.q, p.q + p.corners()));
            if (domain >= 0 && int32_t(p.domain()) != domain) {
              shapes.back().clear();
            }
          }
        }
        for (size_t i = 0; i < shapes.size(); i++) {
          if (shapes[i].empty()) {
            continue;
          }
          bool inside, outside;
          judge(shapes[i], inside, outside);
          if (domain < 0) {
            in += inside;
            expect(!inside || hit, "an object in the region");
            continue;
          }
          bool has = false;
          for (int32_t e : hit ? hit->elements : Vector<int32_t>()) {
            has = has || size_t(e) == i;
          }
          elements += has;
          out += outside;
          expect(!inside || has, "an element in the region");
          expect(!outside || !has, "an element outside of the region");
        }
      }
    }
  }
  printf("regions: %d in, %d elements in, %d out\n", in, elements, out);
  test_assert(in > 300 && elements > 300 && out > 1000);

  PickDoc d;
  const BlockHandle data = d.geoBlock("geo", randomPrims(r, 10));
  const BlockHandle a = d.placed("a", identity(), data);
  const ViewMath view = viewOf(ViewCase());
  const ViewRegion all = view.regionFromRect(0.0, 0.0, 2000.0, 1100.0);
  Vector<RegionHit> hits;
  test_assert(isError(pickRegion(*d.ctx, d.scene, all, ElementDomain::None, {}, hits),
                      StatusCode::FailedPrecondition,
                      "pickRegion"));
  d.evaluate();
  test_assert(isError(pickRegion(*d.ctx, d.scene, all, ElementDomain(32), {}, hits),
                      StatusCode::InvalidArgument));
  ViewRegion bad = all;
  bad.planes[2].w = std::numeric_limits<double>::quiet_NaN();
  test_assert(isError(pickRegion(*d.ctx, d.scene, bad, ElementDomain::None, {}, hits),
                      StatusCode::InvalidArgument));
  must(pickRegion(*d.ctx, d.scene, all, ElementDomain::None, {}, hits));
  test_assert(hits.size() == 1 && hits[0].object == a);
  PickFilter none;
  none.accept = [](BlockHandle) { return false; };
  must(pickRegion(*d.ctx, d.scene, all, ElementDomain::None, none, hits));
  test_assert(hits.size() == 0);
  d.geo(data).onNear = [&] { d.rt.setStickyError(boom("region")); };
  for (int32_t domain : {-1, 0}) {
    test_assert(isError(pickRegion(*d.ctx, d.scene, all, ElementDomain(domain), {}, hits),
                        StatusCode::Internal,
                        "region"));
    test_assert(hits.size() == 0 && !d.rt.hasStickyError());
  }
}

/* ---- objects by their bounds ---- */

void testObjects()
{
  Rand r(63);
  int onRay = 0, near = 0, inRegion = 0;
  PickDoc d;
  std::vector<BlockHandle> objects;
  for (int i = 0; i < 200; i++) {
    const mat4d world = solve::affine(Linear::identity(), r.vec(5.0));
    objects.push_back(
        d.placed("box",
                 world,
                 i % 10 == 0 ? BlockHandle() : d.shapeBlock("shape", r.range(0.1, 0.6))));
  }
  d.evaluate();
  const SceneBvh &scene = d.sc().bvh();
  for (int k = 0; k < 500; k++) {
    Vector<BlockHandle> got;
    Ray ray;
    ray.origin = r.vec(8.0);
    ray.dir = r.vec(1.0);
    ray.tMax = r.coin() ? 5.0 : ray.tMax;
    const double3 p = r.vec(6.0);
    const double radius = r.range(0.0, 2.0);
    struct Want {
      double key;
      int place;
    };
    std::vector<Want> rays, nears;
    std::vector<int> regions;
    BvhPlane planes[2];
    for (BvhPlane &plane : planes) {
      plane.n = r.vec(1.0);
      plane.w = r.range(-3.0, 3.0);
    }
    for (int i = 0; i < 200; i++) {
      const Box3d box = d.result(objects[size_t(i)]).bounds;
      if (box.empty()) {
        continue;
      }
      double enter = 0.0, leave = ray.tMax, dd = 0.0;
      bool outside = false;
      for (int axis = 0; axis < 3; axis++) {
        const double a = (box.min[axis] - ray.origin[axis]) / ray.dir[axis];
        const double b = (box.max[axis] - ray.origin[axis]) / ray.dir[axis];
        enter = std::fmax(enter, std::fmin(a, b));
        leave = std::fmin(leave, std::fmax(a, b));
        const double off =
            std::fmax(0.0, std::fmax(box.min[axis] - p[axis], p[axis] - box.max[axis]));
        dd += off * off;
      }
      for (const BvhPlane &plane : planes) {
        double most = plane.w;
        for (int axis = 0; axis < 3; axis++) {
          most += std::fmax(plane.n[axis] * box.min[axis], plane.n[axis] * box.max[axis]);
        }
        outside = outside || most < 0.0;
      }
      if (enter <= leave) {
        rays.push_back({enter, i});
      }
      if (std::sqrt(dd) <= radius) {
        nears.push_back({std::sqrt(dd), i});
      }
      if (!outside) {
        regions.push_back(i);
      }
    }
    const auto sorted = [&](std::vector<Want> &want) {
      std::sort(want.begin(), want.end(), [](const Want &a, const Want &b) {
        return a.key != b.key ? a.key < b.key : a.place < b.place;
      });
      bool ok = got.size() == want.size();
      for (int i = 0; ok && i < int(want.size()); i++) {
        ok = got[i] == objects[size_t(want[size_t(i)].place)];
      }
      return ok;
    };
    must(scene.objectsOnRay(ray, got));
    expect(sorted(rays), "objectsOnRay");
    must(scene.objectsNear(p, radius, got));
    expect(sorted(nears), "objectsNear");
    must(scene.objectsInRegion(planes, got));
    bool ok = got.size() == regions.size();
    for (int i = 0; ok && i < int(regions.size()); i++) {
      ok = got[i] == objects[size_t(regions[size_t(i)])];
    }
    expect(ok, "objectsInRegion");
    onRay += int(rays.size());
    near += int(nears.size());
    inRegion += int(regions.size());
  }
  printf("objects: %d on rays, %d near, %d in regions%c", onRay, near, inRegion, 10);
  test_assert(onRay > 100 && near > 100 && inRegion > 3000);
  Vector<BlockHandle> got;
  got.append(BlockHandle());
  const double nan = std::numeric_limits<double>::quiet_NaN();
  test_assert(scene.objectsNear(double3(), nan, got).code() ==
              StatusCode::InvalidArgument);
  test_assert(got.size() == 0);
  Ray bad;
  bad.dir = double3(0, 0, nan);
  got.append(BlockHandle());
  test_assert(scene.objectsOnRay(bad, got).code() == StatusCode::InvalidArgument &&
              got.size() == 0);
  BvhPlane plane;
  plane.w = nan;
  got.append(BlockHandle());
  test_assert(scene.objectsInRegion({&plane, 1}, got).code() ==
              StatusCode::InvalidArgument);
  test_assert(got.size() == 0);
}

void testSettings()
{
  PickDoc d;
  d.rt.ext<SceneSettings>().bvh.leafSize = 3;
  const BlockHandle shape = d.shapeBlock("shape", 0.5);
  for (int i = 0; i < 100; i++) {
    d.setData(d.object("o", 2.0 * i), shape);
  }
  d.evaluate();
  int most = 0;
  for (const auto &node : d.sc().bvh().boxes().bvh().nodes()) {
    most = (std::max)(most, int(node.count));
  }
  test_assert(most <= 3 && most > 0);
  d.rt.ext<SceneSettings>().bvh.leafSize = 0;
  d.setData(d.object("one more"), shape);
  test_assert(isError(evaluateScene(*d.ctx), StatusCode::InvalidArgument));
  test_assert(isError(evaluateScene(*d.ctx), StatusCode::InvalidArgument) &&
              !d.sc().bvh().valid());
  d.rt.ext<SceneSettings>().bvh.leafSize = 4;
  d.evaluate();
  test_assert(d.sc().bvh().valid() && d.sc().bvh().boxes().bvh().size() == 101);
  Runtime other;
  const BvhOptions &bvh = other.ext<SceneSettings>().bvh;
  test_assert(bvh.leafSize == BvhOptions().leafSize && bvh.parallel);
}

} // namespace

int main()
{
  testNearest();
  testNearestCases();
  testNearestOrder();
  testNearRounding();
  testRegion();
  testObjects();
  testSettings();
  if (g_failures > 0) {
    fprintf(stderr, "%d failures\n", g_failures);
  }
  test_assert(g_failures == 0);
  return test_end();
}
