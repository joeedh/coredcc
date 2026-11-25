/* Which of the things near a pixel comes first: pickScene with any options, against the rule applied to
 * every shape on its own, and cases where it is known what a user means. */
#include "pick_doc.h"

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

/* ---- against the rule ---- */

struct Shape1 {
  PickHit hit;
  int place;
  int dimension;
  int group = 0;
};

/* Every shape in a sink of its own, for all domains. */
std::vector<Shape1> everyShape(PickDoc &d,
                               const std::vector<BlockHandle> &objects,
                               const ViewMath &view,
                               double x,
                               double y,
                               PickOptions options)
{
  options.domains = ~0u;
  const PickCursor cursor(view, x, y, options);
  std::vector<Shape1> out;
  for (int place = 0; place < int(objects.size()); place++) {
    const Geo &g = d.geo(d.lib->find(d.obj(objects[size_t(place)]).data.target()));
    const PickQuery q(cursor, d.result(objects[size_t(place)]).state.world);
    for (int i = 0; i < int(g.prims.size()); i++) {
      PickSink sink(q);
      g.give(sink, i);
      if (sink.kept().size() == 0) {
        continue;
      }
      const Prim &p = g.prims[size_t(i)];
      Shape1 s;
      s.hit = sink.kept()[0].hit;
      s.hit.object = objects[size_t(place)];
      s.place = place;
      s.dimension = p.kind == Kind::Point                            ? 0 :
                    p.kind == Kind::Segment                          ? 1 :
                    p.kind == Kind::Triangle || !s.hit.thin          ? 2 :
                    lengthOf(p.q[0] - p.q[1]) > 0.0                  ? 1 :
                                                                       0;
      out.push_back(s);
    }
  }
  return out;
}

/* The rule of detail::rankPicks, by looking for the next one again and again. */
std::vector<Shape1> ranked(std::vector<Shape1> all, const ViewMath &view, const PickOptions &o)
{
  double depth = std::numeric_limits<double>::infinity(), behind = depth;
  if (o.occlusion) {
    for (const Shape1 &s : all) {
      if (!s.hit.thin && s.hit.distance == 0.0 && s.hit.depth < depth) {
        depth = s.hit.depth;
        const double reach = std::fmax(o.radius + o.shapeRadius, 1.0);
        behind = depth + o.slope * reach * view.pixelSizeAt(s.hit.point);
      }
    }
  }
  std::vector<Shape1> left, out;
  for (Shape1 s : all) {
    if ((o.domains >> s.hit.domain & 1) != 0) {
      s.group = s.hit.thin && s.hit.depth <= behind ? 0 : s.hit.distance == 0.0 ? 1 : 2;
      left.push_back(s);
    }
  }
  const auto before = [](const Shape1 &a, const Shape1 &b) {
    if (a.group != b.group) {
      return a.group < b.group;
    }
    if (a.group != 1 && a.hit.distance != b.hit.distance) {
      return a.hit.distance < b.hit.distance;
    }
    if (a.group == 0 && a.dimension != b.dimension) {
      return a.dimension < b.dimension;
    }
    return a.hit.depth != b.hit.depth ? a.hit.depth < b.hit.depth : a.place < b.place;
  };
  while (!left.empty() && (o.maxHits == 0 || int(out.size()) < o.maxHits)) {
    size_t best = 0;
    for (size_t i = 1; i < left.size(); i++) {
      best = before(left[i], left[best]) ? i : best;
    }
    const Shape1 s = left[best];
    out.push_back(s);
    std::vector<Shape1> rest;
    for (const Shape1 &r : left) {
      if (r.place != s.place || (o.everyDomain && r.hit.domain != s.hit.domain)) {
        rest.push_back(r);
      }
    }
    left = rest;
  }
  return out;
}

bool same(const Vector<PickHit> &hits, const std::vector<Shape1> &want)
{
  if (hits.size() != want.size()) {
    return false;
  }
  for (int i = 0; i < int(want.size()); i++) {
    const PickHit &a = hits[i], &b = want[size_t(i)].hit;
    if (!(a.object == b.object) || a.domain != b.domain || a.distance != b.distance ||
        a.depth != b.depth || a.thin != b.thin)
    {
      return false;
    }
  }
  return true;
}

void testRule()
{
  Rand r(51);
  int groups[3] = {}, hidden = 0, several = 0, singles = 0, rays = 0, fewer = 0;
  for (int round = 0; round < 80 * coredcc_test::testScale(); round++) {
    PickDoc d;
    std::vector<BlockHandle> objects;
    const double size = r.coin() ? 1.0 : 0.3;
    for (int i = 0; i < 25; i++) {
      std::vector<Prim> prims;
      for (int k = 0; k < 12; k++) {
        Prim p;
        p.kind = Kind(r.below(4));
        const double3 c = r.vec(size);
        for (double3 &q : p.q) {
          q = c + r.vec(0.3 * size);
        }
        if (p.kind == Kind::Capsule) {
          p.radius = r.range(0.0, r.coin() ? 0.15 : 0.02);
          p.q[1] = r.below(3) == 0 ? p.q[0] : p.q[1];
        }
        p.pixels = r.coin() ? 0.0 : r.range(0.0, 6.0);
        prims.push_back(p);
      }
      const BlockHandle data = d.geoBlock("geo", prims);
      const mat4d world = solve::affine(randomLinear(r, int(r.below(3))), r.vec(1.5));
      objects.push_back(d.placed("object", world, data));
    }
    d.evaluate();
    for (int k = 0; k < 40; k++) {
      ViewCase c;
      c.ortho = r.coin();
      c.eye = r.vec(3.0);
      c.target = r.vec(0.3);
      c.zNear = c.ortho ? r.range(-5.0, 1.0) : 0.1;
      c.zFar = r.coin() ? 1000.0 : c.zNear + r.range(1.0, 8.0);
      c.dpiScale = r.coin() ? 1.0 : 2.0;
      const ViewMath view = viewOf(c);
      const double x = std::round(r.range(600.0, 1400.0)), y = std::round(r.range(300.0, 800.0));
      PickOptions o;
      o.radius = double(r.below(4)) * 10.0;
      o.shapeRadius = 6.0;
      o.domains = r.coin() ? ~0u : uint32_t(r.below(8));
      o.maxHits = r.below(3) == 0 ? 0 : int32_t(1 + r.below(3));
      o.everyDomain = r.coin();
      o.occlusion = r.below(4) != 0;
      o.thinRadius = r.coin() ? 4.0 : r.range(0.0, 40.0);
      o.slope = r.coin() ? 2.0 : r.range(0.0, 20.0);
      const std::vector<Shape1> all = everyShape(d, objects, view, x, y, o);
      const std::vector<Shape1> want = ranked(all, view, o);
      for (int mode = 0; mode < 3; mode++) {
        int given = 0;
        for (BlockHandle h : objects) {
          Geo &g = d.geo(d.lib->find(d.obj(h).data.target()));
          g.useBvh = mode > 0;
          g.useRay = mode == 2;
          g.given = 0;
        }
        const Vector<PickHit> hits = d.picks(view, x, y, o);
        for (BlockHandle h : objects) {
          given += d.geo(d.lib->find(d.obj(h).data.target())).given;
        }
        expect(same(hits, want), mode == 0 ? "all shapes" : mode == 1 ? "from the BVH" : "from the ray");
        singles += o.maxHits == 1 && hits.size() == 1;
        rays += mode == 2 && o.maxHits == 1 && hits.size() == 1 && !hits[0].thin;
        fewer += mode == 0 && o.maxHits == 1 && given < 12 * 25 / 2;
      }
      for (const Shape1 &s : want) {
        groups[s.group]++;
        hidden += s.group != 0 && s.hit.thin;
      }
      several += o.everyDomain && want.size() > 1 && want[0].hit.object == want[1].hit.object;
    }
  }
  printf("%d thin, %d covering, %d near; %d thin hidden; %d with two domains first; %d single, %d of "
         "them surfaces found by rays, %d that asked half of the objects\n",
         groups[0],
         groups[1],
         groups[2],
         hidden,
         several,
         singles,
         rays,
         fewer);
  test_assert(groups[0] > 500 && groups[1] > 100 && groups[2] > 100 && hidden > 50);
  test_assert(several > 20 && singles > 200 && rays > 20 && fewer > 20);
}

/* ---- what a user means ---- */

void quad(std::vector<Prim> &prims, const double3 &center, double half, double tilt)
{
  const double3 u(half, 0.0, 0.0), v(0.0, half * std::cos(tilt), half * std::sin(tilt));
  prims.push_back(face(center - u - v, center + u - v, center + u + v));
  prims.push_back(face(center - u - v, center + u + v, center - u + v));
}

Prim vertex(const double3 &p)
{
  Prim v;
  v.kind = Kind::Point;
  v.q[0] = p;
  return v;
}

Prim edge(const double3 &a, const double3 &b)
{
  Prim e;
  e.kind = Kind::Segment;
  e.q[0] = a;
  e.q[1] = b;
  return e;
}

void testMeant()
{
  const ViewMath view = viewOf(ViewCase());
  const ViewPoint p = view.project(double3());
  const double pixel = view.pixelSizeAt(double3());
  /* A mesh seen from the front: at a vertex, its faces cover the pixel, and the vertex is meant. A
   * second vertex is on the other side of it, right under the cursor. */
  for (double tilt : {0.0, 0.7, 1.0}) {
    PickDoc d;
    std::vector<Prim> prims;
    quad(prims, double3(), 2.0, tilt);
    prims.push_back(vertex(double3(4.0 * pixel, 0.0, 0.0)));
    const double3 up(0.0, 5.0 * pixel, 5.0 * pixel * std::tan(tilt));
    prims.push_back(edge(up, up - double3(1.0, 0.0, 0.0)));
    prims.push_back(vertex(double3(0.0, 0.0, -3.0)));
    const BlockHandle h = d.placed("mesh", identity(), d.geoBlock("mesh", prims));
    d.evaluate();
    PickOptions o;
    o.radius = 10.0;
    Vector<PickHit> hits = d.picks(view, p.x, p.y, o);
    test_assert(hits.size() == 1 && hits[0].object == h && hits[0].element == 2);
    test_assert(hits[0].thin && std::fabs(hits[0].distance - 4.0) < 0.1);
    /* By domain. */
    o.domains = 1u << int(ElementDomain::Edge);
    hits = d.picks(view, p.x, p.y, o);
    test_assert(hits.size() == 1 && hits[0].element == 3 && std::fabs(hits[0].distance - 5.0) < 0.1);
    o.domains = 1u << int(ElementDomain::Face);
    hits = d.picks(view, p.x, p.y, o);
    test_assert(hits.size() == 1 && hits[0].element < 2 && hits[0].covers());
    o.domains = ~0u;
    o.everyDomain = true;
    hits = d.picks(view, p.x, p.y, o);
    test_assert(hits.size() == 3 && hits[0].element == 2 && hits[1].element == 3 &&
                hits[2].covers());
    /* The vertex behind the faces, only without occlusion: also if faces aren't wanted. */
    o.everyDomain = false;
    o.domains = 1u << int(ElementDomain::Point);
    o.radius = 2.0;
    hits = d.picks(view, p.x, p.y, o);
    test_assert(hits.size() == 1 && hits[0].element == 4 && hits[0].covers());
    o.domains = ~0u;
    hits = d.picks(view, p.x, p.y, o);
    test_assert(hits.size() == 1 && hits[0].element < 2);
    o.occlusion = false;
    hits = d.picks(view, p.x, p.y, o);
    test_assert(hits.size() == 1 && hits[0].element == 4);
    /* A steep surface hides its own edges if the slope says so. */
    if (tilt == 1.0) {
      o = PickOptions();
      o.radius = 10.0;
      o.domains = 1u << int(ElementDomain::Point) | 1u << int(ElementDomain::Face);
      test_assert(d.picks(view, p.x, p.y - 1.0, o)[0].element == 2);
      o.slope = 0.0;
      hits = d.picks(view, p.x, p.y - 1.0, o);
      test_assert(hits.size() == 1 && hits[0].element < 2);
    }
  }
}

/* Pixel radii. */
void testPixels()
{
  for (double dpi : {1.0, 2.0}) {
    PickDoc d;
    ViewCase c;
    c.dpiScale = dpi;
    const ViewMath view = viewOf(c);
    const double pixel = view.pixelSizeAt(double3()); /* logical */
    Prim handle = vertex(double3());
    handle.pixels = 8.0;
    d.placed("handle", identity(), d.geoBlock("handle", {handle}));
    Prim axis = edge(double3(-1.0, 20.0 * pixel, 0.0), double3(1.0, 20.0 * pixel, 0.0));
    axis.pixels = 3.0;
    const BlockHandle line = d.placed("axis", identity(), d.geoBlock("axis", {axis}));
    d.evaluate();
    const ViewPoint p = view.project(double3());
    PickOptions o;
    o.shapeRadius = 8.0;
    test_assert(d.picks(view, p.x + 7.9 * dpi, p.y, o).size() == 1);
    test_assert(d.picks(view, p.x + 7.9 * dpi, p.y, o)[0].covers());
    test_assert(d.picks(view, p.x + 8.1 * dpi, p.y, o).size() == 0);
    o.radius = 2.0;
    Vector<PickHit> hits = d.picks(view, p.x + 9.0 * dpi, p.y, o);
    test_assert(hits.size() == 1 && std::fabs(hits[0].distance - 1.0) < 0.01);
    test_assert(lengthOf(hits[0].point) < 1e-9);
    hits = d.picks(view, p.x, p.y - 15.5 * dpi, o);
    test_assert(hits.size() == 1 && hits[0].object == line && std::fabs(hits[0].distance - 1.5) < 0.01);
    test_assert(std::fabs(hits[0].u - 0.5) < 1e-6 && lengthOf(hits[0].normal) == 0.0);
    test_assert(d.picks(view, p.x, p.y - 14.5 * dpi, o).size() == 0);
  }
}

/* pixelSizeAt: a ball of that radius covers the pixels it should, in any direction. */
void testPixelSize()
{
  Rand r(52);
  int known = 0;
  for (int i = 0; i < 3000; i++) {
    const ViewCase c = randomView(r);
    const ViewMath view = viewOf(c);
    const mat4d world = solve::affine(randomLinear(r, int(r.below(3))), r.vec(2.0));
    const double3 local = r.vec(1.0);
    const ViewPoint at = view.project(transformPoint(world, local));
    const PickCursor cursor(view, at.x, at.y, within(5.0));
    const PickQuery q(cursor, world);
    const double size = q.pixelSizeAt(local);
    if (at.behind) {
      expect(size == 0.0, "nothing behind the eye has a size");
      continue;
    }
    if (!at.visible || std::fabs(at.x) > 1e4 || std::fabs(at.y) > 1e4) {
      continue;
    }
    known++;
    /* The shadow of a small ball: every direction in the image is at least a pixel. */
    double least = 1e300, most = 0.0;
    for (int k = 0; k < 720; k++) {
      const double angle = k * 3.14159265358979 / 720.0;
      double reach = 0.0;
      double3 g;
      for (int axis = 0; axis < 3; axis++) {
        double3 moved = local;
        const double step = size * 1e-6;
        moved[axis] += step;
        const ViewPoint to = view.project(transformPoint(world, moved));
        g[axis] = ((to.x - at.x) * std::cos(angle) + (to.y - at.y) * std::sin(angle)) / step;
      }
      reach = lengthOf(g) * size / c.dpiScale;
      least = std::fmin(least, reach);
      most = std::fmax(most, reach);
    }
    expect(least > 0.999 && (least < 1.01 || most > 100.0), "the shadow of a ball with a pixel's size");
  }
  test_assert(known > 300);
  /* Flattened. */
  Linear flat = Linear::identity();
  flat.col[0] = double3();
  const PickCursor cursor(viewOf(ViewCase()), 1000.0, 500.0, within(5.0));
  const PickQuery q(cursor, solve::affine(flat, double3()));
  test_assert(q.pixelSizeAt(double3()) == std::numeric_limits<double>::infinity() && !q.hasRay());
}

/* ---- what a hit says ---- */

void testHit()
{
  Rand r(53);
  int faces = 0, solids = 0, back = 0, mirrored = 0;
  for (int i = 0; i < 4000; i++) {
    const ViewCase c = randomView(r);
    const ViewMath view = viewOf(c);
    const int kind = int(r.below(3));
    const mat4d world = solve::affine(randomLinear(r, kind), c.eye + (c.target - c.eye) * 3.0);
    const double *m = world;
    const double3 x(m[0], m[1], m[2]), y(m[4], m[5], m[6]), z(m[8], m[9], m[10]);
    const bool mirror = x.cross(y).dot(z) < 0.0;
    const bool triangle = r.coin();
    const double3 a = r.vec(0.7), b = r.vec(0.7), e = r.vec(0.7);
    const double u = r.range(0.05, 0.6), v = r.range(0.05, 0.35);
    const double3 inside = triangle ? a + (b - a) * u + (e - a) * v : a + (b - a) * u;
    const ViewPoint at = view.project(transformPoint(world, inside));
    if (!at.visible || std::fabs(at.x) > 1e4 || std::fabs(at.y) > 1e4) {
      continue;
    }
    const PickCursor cursor(view, at.x, at.y, within(5.0, false));
    const PickQuery q(cursor, world);
    PickSink sink(q);
    if (triangle) {
      sink.triangle(a, b, e);
    }
    else {
      sink.capsule(a, b, 0.1);
    }
    if (!sink.any()) {
      continue;
    }
    const PickHit hit = sink.best();
    const double3 ray = view.rayFromPixel(at.x, at.y).dir;
    if (triangle) {
      const double3 wa = transformPoint(world, a);
      const double3 ab = transformPoint(world, b) - wa, ac = transformPoint(world, e) - wa;
      double3 n = ab.cross(ac);
      if (lengthOf(n) < 1e-3 || std::fabs(n.dot(ray)) < 1e-2 * lengthOf(n)) {
        continue; /* edge on */
      }
      faces++;
      mirrored += mirror;
      n = n * ((mirror ? -1.0 : 1.0) / lengthOf(n));
      expect(lengthOf(hit.normal - n) < 1e-9, "the normal of a triangle");
      expect(std::fabs(hit.u - u) < 1e-5 && std::fabs(hit.v - v) < 1e-5, "u and v of a triangle");
      expect(hit.backFacing == (n.dot(ray) > 0.0), "which side of a triangle");
      back += hit.backFacing;
      continue;
    }
    expect(hit.u >= 0.0 && hit.u <= 1.0, "u of a capsule");
    if (!hit.covers() || lengthOf(hit.normal) == 0.0 || hit.depth < 1e-6) {
      continue;
    }
    solids++;
    expect(!hit.backFacing && hit.normal.dot(ray) < 1e-6, "a solid is entered from outside");
    /* Nothing of the solid is past the tangent plane at the point. */
    const double3 axis[2] = {transformPoint(world, a), transformPoint(world, b)};
    const double3 local(hit.normal.dot(x), hit.normal.dot(y), hit.normal.dot(z));
    for (const double3 &end : axis) {
      expect(hit.normal.dot(end - hit.point) + 0.1 * lengthOf(local) < 1e-6, "the normal of a solid");
    }
  }
  test_assert(faces > 300 && solids > 100 && back > 50 && mirrored > 50);
  /* Nothing is hit, or from inside: no normal. */
  const ViewMath view = viewOf(ViewCase());
  const PickCursor cursor(view, 1000.0, 560.0, within(50.0, false));
  const PickQuery q(cursor, identity());
  PickSink around(q), beside(q);
  around.sphere(double3(0, 0, 10), 1.0);
  test_assert(around.any() && lengthOf(around.best().normal) == 0.0 && !around.best().backFacing);
  beside.sphere(double3(0.2, 0, 0), 0.1);
  test_assert(beside.any() && !beside.best().covers() && lengthOf(beside.best().normal) == 0.0);
}

/* ---- cases of the rule ---- */

Prim ball(const double3 &center, double radius)
{
  Prim p;
  p.kind = Kind::Capsule;
  p.q[0] = p.q[1] = center;
  p.radius = radius;
  return p;
}

uint32_t bit(ElementDomain d)
{
  return 1u << int(d);
}

struct OrthoDoc : PickDoc {
  ViewMath view;
  double pixel;

  OrthoDoc()
  {
    ViewCase c;
    c.ortho = true;
    view = viewOf(c);
    pixel = view.pixelSizeAt(double3());
  }
  BlockHandle add(std::vector<Prim> prims)
  {
    return placed("object", identity(), geoBlock("geo", prims));
  }
  Vector<PickHit> at(const PickOptions &o)
  {
    evaluate();
    const ViewPoint p = view.project(double3());
    return picks(view, p.x, p.y, o);
  }
};

void testOptions()
{
  OrthoDoc d;
  d.evaluate();
  Vector<PickHit> hits;
  test_assert(d.tryPick(d.view, 0, 0, PickOptions(), hits).ok());
  const double bad[] = {-1.0, std::numeric_limits<double>::infinity(),
                        std::numeric_limits<double>::quiet_NaN()};
  for (double v : bad) {
    for (double PickOptions::*field : {&PickOptions::radius, &PickOptions::shapeRadius,
                                       &PickOptions::thinRadius, &PickOptions::slope})
    {
      PickOptions o;
      o.*field = v;
      test_assert(d.tryPick(d.view, 0, 0, o, hits).code() == StatusCode::InvalidArgument);
    }
  }
  PickOptions o;
  o.maxHits = -1;
  test_assert(d.tryPick(d.view, 0, 0, o, hits).code() == StatusCode::InvalidArgument);
}

void testCases()
{
  {
    /* The least reach is a pixel. */
    OrthoDoc d;
    std::vector<Prim> prims;
    quad(prims, double3(), 1.0, 0.0);
    prims.push_back(vertex(double3(0.0, 0.0, -0.5 * d.pixel)));
    d.add(prims);
    test_assert(d.at(PickOptions())[0].element == 2);
  }
  for (double radius : {3.9, 4.1}) {
    OrthoDoc d;
    d.add({ball(double3(), radius * d.pixel)});
    test_assert(d.at(PickOptions())[0].thin == (radius < 4.0));
  }
  {
    /* Equal depths, when the second object is asked first. */
    OrthoDoc d;
    std::vector<Prim> prims;
    quad(prims, double3(), 1.0, 0.0);
    const BlockHandle first = d.add(prims);
    prims.push_back(face(double3(2, 2, 3), double3(2.5, 2, 3), double3(2, 2.5, 3)));
    d.add(prims);
    for (int max : {0, 1}) {
      PickOptions o;
      o.maxHits = max;
      test_assert(d.at(o)[0].object == first);
    }
  }
  {
    /* What isn't wanted is in front of the first object's hit, but the second one's comes before. */
    OrthoDoc d;
    std::vector<Prim> prims;
    quad(prims, double3(0, 0, 5), 1.0, 0.0);
    prims.push_back(ball(double3(0, 0, -5), 0.5));
    d.add(prims);
    const BlockHandle second = d.add({ball(double3(), 0.5)});
    PickOptions o;
    o.domains = bit(ElementDomain::Edge);
    o.maxHits = 1;
    const Vector<PickHit> hits = d.at(o);
    test_assert(hits.size() == 1 && hits[0].object == second);
  }
  {
    /* A thin shape is hidden by what isn't wanted and is found later: a surface that is nearer to the
     * cursor comes first then. */
    OrthoDoc d;
    d.add({vertex(double3(-2.0 * d.pixel, 0, 0)), vertex(double3(2, 2, 6))});
    std::vector<Prim> prims;
    quad(prims, double3(0, 0, 5), 1.0, 0.0);
    d.add(prims);
    const BlockHandle third = d.add({ball(double3(0.5 + d.pixel, 0, -3), 0.5)});
    PickOptions o;
    o.domains = bit(ElementDomain::Edge) | bit(ElementDomain::Point);
    o.maxHits = 1;
    o.radius = 5.0;
    const Vector<PickHit> hits = d.at(o);
    test_assert(hits.size() == 1 && hits[0].object == third);
  }
  for (bool swapped : {false, true}) {
    /* Two thin ones of a domain at one distance: the better one is hidden. */
    OrthoDoc d;
    std::vector<Prim> prims;
    quad(prims, double3(), 1.0, 0.0);
    Prim capsule = ball(double3(0, 0, 3), d.pixel);
    capsule.q[1] = double3(0.5, 0, 3);
    prims.push_back(ball(double3(0, 0, -3), d.pixel));
    prims.push_back(capsule);
    if (swapped) {
      std::swap(prims[2], prims[3]);
    }
    d.add(prims);
    test_assert(d.at(PickOptions())[0].element == (swapped ? 2 : 3));
  }
  for (bool up : {false, true}) {
    /* With maxHits 1, what can't be the first isn't asked. */
    OrthoDoc d;
    std::vector<BlockHandle> objects;
    for (int i = 0; i < 20; i++) {
      std::vector<Prim> prims;
      quad(prims, double3(0, 0, 0.2 * (up ? i : -i)), 1.0, 0.0);
      objects.push_back(d.add(prims));
    }
    for (int max : {0, 1}) {
      PickOptions o;
      o.maxHits = max;
      const Vector<PickHit> hits = d.at(o);
      test_assert(int(hits.size()) == (max ? 1 : 20) && hits[0].object == objects[up ? 19 : 0]);
      int given = 0;
      for (BlockHandle h : objects) {
        Geo &g = d.geo(d.lib->find(d.obj(h).data.target()));
        given += g.given;
        g.given = 0;
      }
      test_assert(given == (max ? 2 : 40));
    }
  }
}

/* What a query says is wanted. */
void testLimits()
{
  const auto inside = [](const PickQuery &q, double z) {
    for (const BvhPlane &p : q.planes()) {
      if (p.side(double3(0, 0, z)) < 0.0) {
        return false;
      }
    }
    return true;
  };
  for (bool infinite : {false, true}) {
    ViewCase c;
    c.infinite = infinite;
    c.zFar = 20.0; /* z = -10 */
    c.dpiScale = 2.0;
    const ViewMath view = viewOf(c);
    const ViewPoint at = view.project(double3());
    const PickCursor cursor(view, at.x, at.y, within(10.0));
    const PickQuery all(cursor, identity());
    test_assert(inside(all, -9.99) && inside(all, -10.01) == infinite && !inside(all, 9.95));
    test_assert(all.limitDepth() > 1e300 && all.surfaceRadiusDevicePixels() == 20.0);
    test_assert(infinite ? all.ray().tMax > 1e300 : std::fabs(all.ray().tMax - 19.9) < 1e-6);
    /* The near plane is at z = 9.9. */
    const PickQuery some(cursor, identity(), 5.0, false);
    test_assert(inside(some, 4.91) && !inside(some, 4.89) && std::fabs(some.ray().tMax - 5.0) < 1e-6);
    test_assert(some.radiusDevicePixels() == 20.0 && some.surfaceRadiusDevicePixels() == 0.0);
    PickSink sink(some);
    sink.point(double3(0, 0, 4.89));
    const double beside = 3.0 * view.pixelSizeAt(double3(0, 0, 5));
    sink.triangle(double3(beside, -1, 5), double3(beside, 1, 5), double3(1, 0, 5));
    test_assert(!sink.any());
    sink.point(double3(beside, 0, 5));
    test_assert(sink.any() && std::fabs(sink.best().distance - 3.0) < 1e-6);
    PickSink near(all);
    near.triangle(double3(beside, -1, 5), double3(beside, 1, 5), double3(1, 0, 5));
    test_assert(near.any());
    const PickQuery late(cursor, identity(), 25.0);
    test_assert(inside(late, -10.01) == infinite && !inside(late, -15.2));
    test_assert(infinite ? std::fabs(late.ray().tMax - 25.0) < 1e-6 :
                           std::fabs(late.ray().tMax - 19.9) < 1e-6);
  }
}

/* The sides of a box face outward. */
void testBox()
{
  Box3d box;
  box.include(-0.5, -0.75, -0.25);
  box.include(0.5, 0.75, 0.25);
  for (int axis = 0; axis < 3; axis++) {
    for (double side : {-1.0, 1.0}) {
      ViewCase c;
      c.eye = double3();
      c.eye[axis] = side * 10.0;
      c.up = axis == 1 ? double3(0, 0, 1) : double3(0, 1, 0);
      const ViewMath view = viewOf(c);
      for (int k = 0; k < 4; k++) {
        const ViewPoint at = view.project(double3());
        const PickCursor cursor(
            view, at.x + (k & 1 ? 7.0 : -7.0), at.y + (k & 2 ? 5.0 : -5.0), PickOptions());
        const PickQuery q(cursor, identity());
        PickSink sink(q);
        sink.box(box);
        double3 normal(0, 0, 0);
        normal[axis] = side;
        test_assert(sink.any() && lengthOf(sink.best().normal - normal) < 1e-12);
        test_assert(!sink.best().backFacing);
      }
    }
  }
}

} // namespace

int main()
{
  testOptions();
  testCases();
  testLimits();
  testBox();
  testRule();
  testMeant();
  testPixels();
  testPixelSize();
  testHit();
  if (g_failures > 0) {
    fprintf(stderr, "%d failures\n", g_failures);
  }
  test_assert(g_failures == 0);
  return test_end();
}
