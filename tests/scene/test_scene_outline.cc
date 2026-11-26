/* The distance from a cursor to what capsules, spheres, triangles, segments and points
 * cover. Each answer is proven: a point of the solid is that near, and no point of it is
 * nearer. */
#include "pick_fixture.h"

#include "coredcc/bvh/kernels.h"

test_init;

using namespace scene_test;

namespace {

struct Tally {
  int shapes = 0, covered = 0, hidden = 0, crossing = 0, eye = 0, behind = 0,
      undecided = 0;
  int past = 0, cutFar = 0, both = 0;

  void print(const char *name) const
  {
    printf("%s: %d, %d covered, %d hidden, %d across the near plane, %d across the eye "
           "plane, "
           "%d ends behind the near plane, %d undecided; %d past the far plane, %d "
           "across it, %d across "
           "both\n",
           name,
           shapes,
           covered,
           hidden,
           crossing,
           eye,
           behind,
           undecided,
           past,
           cutFar,
           both);
  }
};

int g_failures = 0;
bool expect(bool ok, const char *what)
{
  if (!ok && g_failures++ < 20) {
    fprintf(stderr, "  %s\n", what);
  }
  return ok;
}

/* `gap` is how much too large a distance may be. */
void check(const ScreenMap &map,
           const std::vector<double3> &ends,
           double r,
           bool found,
           const ScreenPoint &p,
           double gap,
           Tally &tally)
{
  double front = -std::numeric_limits<double>::infinity(), back = -front, w = back;
  double before = front, after = back; /* of the far plane */
  double size = std::fabs(map.front[3]);
  for (const double3 &q : ends) {
    front = std::fmax(front, dotPoint(map.front, q) + r * normalLength(map.front));
    back = std::fmin(back, dotPoint(map.front, q) - r * normalLength(map.front));
    w = std::fmin(w, dotPoint(map.w, q) - r * normalLength(map.w));
    size = std::fmax(size, std::fabs(dotPoint(map.front, q)));
    before = std::fmax(before, dotPoint(map.back, q) + r * normalLength(map.back));
    after = std::fmin(after, dotPoint(map.back, q) - r * normalLength(map.back));
  }
  tally.shapes++;
  const double slack = 1e-9 * (1.0 + size);
  if (std::fabs(front) < slack || std::fabs(before) < slack) {
    tally.undecided++;
    return;
  }
  const bool there = front > 0.0 && before > 0.0;
  tally.hidden += front < 0.0;
  tally.past += before < 0.0;
  tally.cutFar += before > 0.0 && after < 0.0;
  tally.both += before > 0.0 && after < 0.0 && front > 0.0 && back < 0.0;
  tally.crossing += front > 0.0 && back < 0.0;
  tally.eye += front > 0.0 && w < 0.0;
  for (const double3 &q : ends) {
    tally.behind += front > 0.0 && dotPoint(map.front, q) < 0.0;
  }
  if (!expect(found == there, "found, or not")) {
    return;
  }
  if (!found) {
    return;
  }
  double nx, ny;
  expect(atDistance(map, p, nx, ny), "the point is at the distance");
  /* In the solid. */
  const double3 nearest =
      ends.size() == 1   ? ends[0]
      : ends.size() == 2 ? bvh::closestOnSegment(p.point, ends[0], ends[1])
                         : bvh::closestOnTriangle(p.point, ends[0], ends[1], ends[2]);
  double extent = r + lengthOf(nearest);
  for (const double3 &q : ends) {
    extent = std::fmax(extent, lengthOf(q));
  }
  expect(lengthOf(p.point - nearest) <= r + 1e-9 * extent, "the point is in the solid");
  if (p.distance == 0.0) {
    tally.covered++;
    return;
  }
  const double least = std::fmax(0.0, p.distance - gap - 1e-8 * p.distance);
  if (!expect(provenNear(map, ends, r, nx, ny, least), "nothing is nearer") &&
      g_failures < 5)
  {
    fprintf(stderr, "    distance %.9g, radius %g\n", p.distance, r);
  }
}

struct Setup {
  ViewMath view;
  mat4d world;
};

Setup randomSetup(Rand &r)
{
  Setup s;
  const ViewCase c = randomView(r);
  s.view = viewOf(c);
  const double3 ahead =
      (c.target - c.eye) * (r.range(-0.5, 4.0) / lengthOf(c.target - c.eye));
  s.world = solve::affine(randomLinear(r, int(r.below(kLinearKinds))),
                          c.eye + ahead + r.vec(r.coin() ? 0.1 : 2.0));
  return s;
}

/* A cursor near what a point of the shape projects to, if that is on the screen. */
void cursorFor(Rand &r, const Setup &s, const double3 &q, double &x, double &y)
{
  const ViewPoint p = s.view.project(litestl::math::transformPoint(s.world, q));
  const double spread = r.below(4) == 0 ? 0.0 : r.coin() ? 5.0 : 300.0;
  const bool use = p.visible && std::fabs(p.x) < 1e5 && std::fabs(p.y) < 1e5;
  x = (use ? p.x : r.range(0.0, 2000.0)) + r.range(-spread, spread);
  y = (use ? p.y : r.range(0.0, 1100.0)) + r.range(-spread, spread);
}

void testCapsules()
{
  Rand r(11);
  Tally tally;
  const int count = 30000 * coredcc_test::testScale();
  for (int i = 0; i < count; i++) {
    const Setup s = randomSetup(r);
    const double size = r.coin() ? 1.0 : 0.05;
    double3 a = r.vec(size),
            b = r.below(4) == 0 ? a : r.vec(size * (r.coin() ? 1.0 : 5.0));
    const double radius = r.below(5) == 0 ? 0.0 : r.range(0.001, 0.8) * size;
    double x, y;
    cursorFor(r, s, a + (b - a) * r.range(0.0, 1.0) + r.vec(radius), x, y);
    const ScreenMap map = mapOf(s.view, s.world, x, y);
    ScreenPoint p;
    const double infinity = std::numeric_limits<double>::infinity();
    const bool found = detail::closestOnCapsule(map, a, b, radius, infinity, p);
    check(map, {a, b}, radius, found, p, 0.01001, tally);

    /* With a limit: the same if it is nearer, else the same or not found. */
    const double limit = r.coin() ? 12.0 : 0.0;
    ScreenPoint limited;
    const bool within = detail::closestOnCapsule(map, a, b, radius, limit, limited);
    if (found && p.distance <= limit) {
      expect(within && limited.distance == p.distance, "a limit changes nothing nearer");
    } else {
      expect(!within || (found && limited.distance == p.distance),
             "a limit drops what is farther");
      expect(!within || limited.distance <= limit + 0.011, "what is within the limit");
    }
  }
  tally.print("capsules");
  test_assert(tally.covered > count / 20 && tally.crossing > count / 20);
  test_assert(tally.eye > count / 50 && tally.hidden > count / 20 &&
              tally.undecided < count / 50);
  test_assert(tally.past > count / 50 && tally.cutFar > count / 100 &&
              tally.both > count / 500);
}

/* Spheres and capsules on a grid of pixels around them, from 1 to 100 pixels large. */
void testGrid()
{
  Tally tally;
  for (int kind = 0; kind < 6; kind++) {
    ViewCase c;
    c.ortho = kind % 2 == 1;
    c.zNear = c.ortho ? -20.0 : 0.001;
    c.eye = double3(0.0, 0.0, 10.0);
    const ViewMath view = viewOf(c);
    Linear m;
    m.col[0] = double3(1.0, 0.0, 0.3);
    m.col[1] = double3(kind < 2 ? 0.0 : 0.8, 1.0, 0.0);
    m.col[2] = kind < 4 ? double3(0.0, 0.0, 1.0) : double3();
    const mat4d world = solve::affine(m, double3(0.2, 0.1, 0.0));
    const double3 a(-0.1, 0.0, 0.0), b = kind % 3 == 0 ? a : double3(0.1, 0.05, 0.3);
    const double radius = kind % 2 == 0 ? 0.004 : 0.1;
    const ViewPoint center = view.project(litestl::math::transformPoint(world, a));
    for (int py = -40; py <= 40; py += 2) {
      for (int px = -40; px <= 40; px += 2) {
        const ScreenMap map =
            mapOf(view, world, std::round(center.x) + px, std::round(center.y) + py);
        ScreenPoint p;
        const bool found = detail::closestOnCapsule(map, a, b, radius, 1e9, p);
        check(map, {a, b}, radius, found, p, 0.01001, tally);
      }
    }
  }
  tally.print("grid");
  test_assert(tally.covered > 300);
}

void testCorners()
{
  Rand r(12);
  Tally tally;
  const int count = 30000 * coredcc_test::testScale();
  for (int i = 0; i < count; i++) {
    const Setup s = randomSetup(r);
    const double size = r.coin() ? 1.0 : 0.05;
    double3 q[3] = {r.vec(size), r.vec(size), r.vec(size * (r.coin() ? 1.0 : 5.0))};
    if (r.below(10) == 0) {
      q[2] = q[0] + (q[1] - q[0]) * r.range(-1.0, 2.0); /* no area */
    }
    const int n = 1 + int(r.below(3));
    const double u = r.range(0.0, 1.0), v = r.range(0.0, 1.0 - u);
    const double3 inside = n == 1   ? q[0]
                           : n == 2 ? q[0] + (q[1] - q[0]) * u
                                    : q[0] + (q[1] - q[0]) * u + (q[2] - q[0]) * v;
    double x, y;
    cursorFor(r, s, inside, x, y);
    const ScreenMap map = mapOf(s.view, s.world, x, y);
    ScreenPoint p;
    const bool found = detail::closestOnCorners(map, q, n, p);
    check(map, std::vector<double3>(q, q + n), 0.0, found, p, 1e-5, tally);
  }
  tally.print("corners");
  test_assert(tally.covered > count / 20 && tally.crossing > count / 20);
  test_assert(tally.eye > count / 50 && tally.hidden > count / 20 &&
              tally.undecided < count / 50);
  test_assert(tally.past > count / 50 && tally.cutFar > count / 100 &&
              tally.both > count / 500);
}

/* Cursors on the shared edges and vertices of a fan are covered by a triangle of it,
 * whether the near or the far plane cuts the fan or not. */
void testWatertight()
{
  Rand r(13);
  int cut = 0, whole = 0;
  for (int i = 0; i < 4000 * coredcc_test::testScale(); i++) {
    ViewCase c;
    c.ortho = r.coin();
    c.zNear = c.ortho ? r.range(-1.0, 1.0) : 0.01;
    c.eye = double3(0.0, 0.0, r.range(0.0, 3.0));
    c.zFar = r.coin() ? 1000.0 : c.zNear + r.range(0.5, 4.0);
    const ViewMath view = viewOf(c);
    const mat4d world = solve::affine(randomLinear(r, int(r.below(3))), r.vec(0.5));
    const int n = 5;
    double3 rim[n];
    const double3 hub = r.vec(0.1);
    for (int k = 0; k < n; k++) {
      const double angle = k * 2.0 * 3.14159265358979 / n;
      rim[k] = double3(std::cos(angle), std::sin(angle), 0.0) + r.vec(0.2);
    }
    /* A point of a spoke, not at the rim. */
    const double3 on =
        hub + (rim[r.below(n)] - hub) * (r.coin() ? 0.0 : r.range(0.0, 0.9));
    const ViewPoint at = view.project(litestl::math::transformPoint(world, on));
    if (!at.visible || at.viewDistance < 1e-6) {
      continue;
    }
    const ScreenMap map = mapOf(view, world, at.x, at.y);
    bool covered = false, crosses = false;
    for (int k = 0; k < n; k++) {
      const double3 t[3] = {hub, rim[k], rim[(k + 1) % n]};
      ScreenPoint p;
      covered |= detail::closestOnCorners(map, t, 3, p) && p.distance == 0.0;
      crosses |= dotPoint(map.front, rim[k]) < 0.0 || dotPoint(map.back, rim[k]) < 0.0;
    }
    expect(covered, "an edge leaks");
    (crosses ? cut : whole)++;
  }
  printf("watertight: %d whole, %d cut\n", whole, cut);
  test_assert(cut > 300 && whole > 300);
}

/* What can't be in a view. */
void testExtremes()
{
  const ViewMath view = viewOf(ViewCase());
  const ScreenMap map = mapOf(view, identity(), 1000.0, 500.0);
  const double3 z(0.0, 0.0, 0.0);
  ScreenPoint p;
  /* Balls that are nearer than what counts, found also without a radius. */
  ViewCase flat;
  flat.ortho = true;
  const ViewMath ortho = viewOf(flat);
  const ViewPoint mid = ortho.project(z);
  Rand r(14);
  for (int i = 0; i < 2000; i++) {
    const double pixels = r.range(5.0, 500.0), angle = r.range(0.0, 6.28);
    for (double off : {5e-5, 5e-4}) {
      const ScreenMap m = mapOf(ortho,
                                identity(),
                                mid.x + (pixels + off) * std::cos(angle),
                                mid.y + (pixels + off) * std::sin(angle));
      const bool found = detail::closestOnCapsule(m, z, z, pixels / 180.0, 0.0, p);
      expect(found == (off < 1e-4) && (!found || p.distance == 0.0),
             "what counts as covered");
    }
  }
  /* All of it at one point, behind the eye or before it. */
  mat4d zero = solve::affine(Linear{{z, z, z}}, z);
  const ViewPoint at = view.project(z);
  test_assert(detail::closestOnCapsule(
      mapOf(view, zero, at.x + 3, at.y), z, double3(1, 2, 3), 5.0, 1e9, p));
  test_assert(std::fabs(p.distance - 3.0) < 1e-6);
  zero = solve::affine(Linear{{z, z, z}}, double3(0, 0, 11));
  test_assert(!detail::closestOnCapsule(
      mapOf(view, zero, 0, 0), z, double3(1, 2, 3), 5.0, 1e9, p));
  const double3 far[3] = {
      double3(1e300, 0, 0), double3(0, 1e300, 0), double3(0, 0, 1e300)};
  detail::closestOnCorners(map, far, 3, p);
  detail::closestOnCapsule(map, far[0], far[1], 1e300, 1e9, p);
  detail::closestOnCapsule(map, z, z, 1e200, 1e9, p);
}

} // namespace

int main()
{
  testCapsules();
  testGrid();
  testCorners();
  testWatertight();
  testExtremes();
  if (g_failures > 0) {
    fprintf(stderr, "%d failures\n", g_failures);
  }
  test_assert(g_failures == 0);
  return test_end();
}
