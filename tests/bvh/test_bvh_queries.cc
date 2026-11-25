/* Every query against a loop over the source, for every source, layout and leaf size. */
#include "bvh_fixture.h"

#include "test_util.h"

#include <algorithm>

test_init;

using namespace bvhtest;

namespace {

int failures = 0;
void expect(bool ok, const char *source, int layout, int leafSize, const char *what)
{
  if (!ok && failures++ < 20) {
    fprintf(stderr, "  %s, layout %d, leaf size %d: %s\n", source, layout, leafSize, what);
  }
}

/* The same primitive tests without a tree. */
template <class Src> std::vector<RayHit> bruteRay(const Src &src, const Ray &ray)
{
  std::vector<RayHit> hits;
  const bvh::RayQuery q(ray);
  double limit = q.tMax;
  for (int i = 0; i < src.size(); i++) {
    const auto b = src.bounds(i);
    if (b.empty() || !b.allFinite()) {
      continue;
    }
    src.rayRange(q, i, 1, limit, [&](int k, const bvh::PrimHit &h) {
      hits.push_back({k, q.toRayT(h.t), h.u, h.v, h.inside, h.backFacing});
      return true;
    });
  }
  return hits;
}

/* The first of some hits that `accept` takes. */
template <class Accept> RayHit firstOf(const std::vector<RayHit> &hits, Accept accept)
{
  RayHit want;
  for (const RayHit &h : hits) {
    if (accept(h) && (!want || h.t < want.t || (h.t == want.t && h.primitive < want.primitive))) {
      want = h;
    }
  }
  return want;
}

bool sameHit(const RayHit &a, const RayHit &b)
{
  return a.primitive == b.primitive && a.t == b.t && a.u == b.u && a.v == b.v &&
         a.inside == b.inside && a.backFacing == b.backFacing;
}

double3 farthest(const Box3d &b, Rand &r)
{
  return double3(r.range(b.min[0], b.max[0]), r.range(b.min[1], b.max[1]),
                 r.range(b.min[2], b.max[2]));
}

template <class Src>
void checkQueries(const char *name, const Src &src, int layout, int leafSize, Rand &r, int queries)
{
  GeometryBvh<Src> bvh;
  BvhOptions opts;
  opts.leafSize = leafSize;
  opts.bins = 2 + r.below(63);
  test_assert(bvh.build(src, opts).ok());
  const char *problem = treeProblem(bvh.bvh(), src, leafSize);
  expect(problem == nullptr, name, layout, leafSize, problem ? problem : "");

  Box3d all;
  for (int i = 0; i < src.size(); i++) {
    const auto b = src.bounds(i);
    if (!b.empty() && b.allFinite()) {
      all.include(b.min[0], b.min[1], b.min[2]);
      all.include(b.max[0], b.max[1], b.max[2]);
    }
  }
  const double diagonal =
      all.empty() ? 1.0
                  : length(double3(all.max[0] - all.min[0], all.max[1] - all.min[1],
                                   all.max[2] - all.min[2])) + 1e-3;
  const auto fail = [&](const char *what) { expect(false, name, layout, leafSize, what); };

  for (int k = 0; k < queries; k++) {
    /* Aim at a primitive, or anywhere. */
    const int target = src.size() > 0 ? r.below(src.size()) : 0;
    double3 at = all.empty() ? r.vec(1.0) : farthest(all, r);
    if (src.size() > 0 && r.below(4) != 0) {
      const auto b = src.bounds(target);
      if (!b.empty() && b.allFinite()) {
        at = src.closest(target, at);
      }
    }
    Ray ray;
    ray.origin = at + r.vec(1.0) * (r.below(3) == 0 ? diagonal : r.unit());
    ray.dir = at - ray.origin;
    if (r.below(6) == 0) {
      ray.dir[r.below(3)] = 0.0;
    }
    if (length(ray.dir) == 0.0) {
      ray.dir = double3(0, 0, 1);
    }
    if (r.below(4) == 0) {
      ray.tMin = r.range(-1, 1);
      ray.tMax = ray.tMin + r.range(0, 2);
    }

    if (layout == int(Layout::Lattice) && r.below(2) == 0) {
      /* From the lattice, along it or across. */
      for (int i = 0; i < 3; i++) {
        ray.origin[i] = lattice(r, 20);
        ray.dir[i] = r.below(5) - 2;
      }
      ray.dir[r.below(3)] = r.below(2) ? 1.0 : 0.1;
    }

    if (r.below(5) == 0) {
      /* Starting next to what it hits, which is large compared to the distance. */
      ray.dir = r.vec(1.0);
      ray.origin = at - ray.dir * std::pow(10.0, r.range(-9, -3));
    }

    const auto any = [](const RayHit &) { return true; };
    /* The same ray three times: as it is, ending at a hit and starting at one. */
    std::vector<int> a, b;
    const RayHit aimed = firstOf(bruteRay(src, ray), any);
    for (int way = 0; way < (aimed ? 3 : 1); way++) {
      Ray cut = ray;
      if (way == 1) {
        cut.tMax = aimed.t;
      } else if (way == 2) {
        cut.tMin = aimed.t;
      }
      const std::vector<RayHit> brute = bruteRay(src, cut);
      const RayHit want = firstOf(brute, any), got = bvh.rayClosest(cut);
      if (bool(got) != bool(want)) {
        fail(way == 0   ? "rayClosest: hit or miss"
             : way == 1 ? "rayClosest: a ray that ends at a hit"
                        : "rayClosest: a ray that starts at a hit");
      } else if (got && !sameHit(got, want)) {
        fail("rayClosest: another hit");
      }
      if (bvh.rayAny(cut) != !brute.empty()) {
        fail("rayAny");
      }
      a.clear();
      b.clear();
      test_assert(bvh.rayAll(cut, [&](const RayHit &h) {
                       a.push_back(h.primitive);
                       return true;
                     })
                      .ok());
      for (const RayHit &h : brute) {
        b.push_back(h.primitive);
      }
      std::sort(a.begin(), a.end());
      if (a != b) {
        fail("rayAll");
      }
      int calls = 0;
      test_assert(bvh.rayAll(cut, [&](const RayHit &) {
                       calls++;
                       return false;
                     })
                      .ok());
      if (calls != (b.empty() ? 0 : 1)) {
        fail("rayAll: stop");
      }
      /* Filtered. */
      const auto odd = [](const RayHit &h) { return (h.primitive & 1) != 0 && !h.backFacing; };
      const RayHit wantOdd = firstOf(brute, odd), gotOdd = bvh.rayClosest(cut, odd);
      if (bool(gotOdd) != bool(wantOdd) || (gotOdd && !sameHit(gotOdd, wantOdd))) {
        fail("rayClosest: filtered");
      }
      if (bvh.rayAny(cut, odd) != bool(wantOdd)) {
        fail("rayAny: filtered");
      }
    }

    /* Distances. */
    const double3 p = r.below(2) ? at : at + r.vec(0.3 * diagonal);
    const double radius = r.below(4) == 0 ? 0.0 : r.unit() * (r.below(2) ? diagonal : 0.5);
    NearestHit near_, nearOdd;
    double best = 0.0, bestOdd = 0.0; /* squares: roots can be equal where they differ */
    std::vector<int> inside;
    for (int i = 0; i < src.size(); i++) {
      const auto box = src.bounds(i);
      if (box.empty() || !box.allFinite()) {
        continue;
      }
      const double d2 = dot(p - src.closest(i, p), p - src.closest(i, p));
      if (d2 <= radius * radius) {
        inside.push_back(i);
        if (!near_ || d2 < best) {
          best = d2;
          near_.primitive = i;
          near_.distance = std::sqrt(d2);
        }
        if ((i & 1) != 0 && (!nearOdd || d2 < bestOdd)) {
          bestOdd = d2;
          nearOdd.primitive = i;
          nearOdd.distance = std::sqrt(d2);
        }
      }
    }
    const NearestHit n = bvh.nearest(p, radius);
    if (n.primitive != near_.primitive || n.distance != near_.distance) {
      fail("nearest");
    }
    if (n && length(p - n.point) != n.distance) {
      fail("nearest: point");
    }
    const NearestHit odd = bvh.nearest(p, radius, [](int i) { return (i & 1) != 0; });
    if (odd.primitive != nearOdd.primitive || odd.distance != nearOdd.distance) {
      fail("nearest: filtered");
    }
    /* Exactly at the radius. */
    if (near_) {
      const double dd = dot(p - n.point, p - n.point);
      const double edge = std::sqrt(dd);
      if (edge * edge >= dd && bvh.nearest(p, edge).primitive != near_.primitive) {
        fail("nearest: at the radius");
      }
    }
    a.clear();
    test_assert(bvh.sphereOverlap(p, radius, [&](int i) {
                     a.push_back(i);
                     return true;
                   })
                    .ok());
    std::sort(a.begin(), a.end());
    if (a != inside) {
      fail("sphereOverlap");
    }

    /* Regions: a random box or wedge around a point. */
    BvhPlane planes[8];
    const int count = r.below(9);
    for (int i = 0; i < count; i++) {
      planes[i].n = r.vec(1.0) * std::pow(10.0, r.range(-3, 3));
      planes[i].w = -dot(planes[i].n, at + r.vec(0.2));
    }
    b.clear();
    for (int i = 0; i < src.size(); i++) {
      const auto box = src.bounds(i);
      bool out = box.empty() || !box.allFinite();
      for (int q = 0; q < count && !out; q++) {
        out = src.outside(i, planes[q]);
      }
      if (!out) {
        b.push_back(i);
      }
    }
    a.clear();
    test_assert(bvh.region({planes, size_t(count)}, [&](int i) {
                     a.push_back(i);
                     return true;
                   })
                    .ok());
    std::sort(a.begin(), a.end());
    if (a != b) {
      fail("region");
    }
    /* A primitive with a point inside every plane must be a candidate. */
    if (src.size() > 0 && !src.bounds(target).empty() && src.bounds(target).allFinite()) {
      const double3 x = src.closest(target, at);
      bool in = true;
      for (int i = 0; i < count; i++) {
        in = in && dot(planes[i].n, x) + planes[i].w >= 0.0;
      }
      if (in && !std::binary_search(a.begin(), a.end(), target)) {
        fail("region: a primitive that overlaps it is missing");
      }
    }
  }
}

template <class S> void allSources(const char *scalar, int queries)
{
  Rand r(11);
  const bool isFloat = sizeof(S) == 4;
  for (int layout = 0; layout < int(Layout::Count); layout++) {
    for (int leafSize : {1, 2, 16, 128, 1024}) {
      for (int n : {1, 3, 40, 700}) {
        const Layout l = Layout(layout);
        checkQueries(scalar, triangles<S>(r, l, n), layout, leafSize, r, queries);
        checkQueries(scalar, capsules<S>(r, l, n), layout, leafSize, r, queries);
        checkQueries(scalar, spheres<S>(r, l, n), layout, leafSize, r, queries);
        if (!isFloat) {
          checkQueries("boxes", boxes(r, l, n), layout, leafSize, r, queries);
        }
      }
    }
  }
}

/* A source whose points are outside of their bounds, by rounding that isn't small compared to the
 * distance. */
struct RoundedSource : SphereSource<double> {
  double3 closest(int i, const double3 &p) const
  {
    double3 c = SphereSource<double>::closest(i, p);
    for (int k = 0; k < 3; k++) {
      c[k] += (p[k] - c[k] > 0.0 ? 4.0 : -4.0) * 2.2e-16 * std::fabs(c[k]);
    }
    return c;
  }
};

void testRounded()
{
  Rand r(19);
  for (int k = 0; k < 2000; k++) {
    RoundedSource src;
    const double3 c = place(r, r.below(2) ? Layout::Far : Layout::Uniform, k);
    const double radius = r.range(0.01, 0.5);
    src.append(c, radius);
    GeometryBvh<RoundedSource> bvh;
    test_assert(bvh.build(src).ok());
    double3 p = c;
    const int axis = r.below(3);
    p[axis] += (radius + r.range(1e-3, 0.1)) * (r.below(2) ? 1.0 : -1.0);
    const NearestHit n = bvh.nearest(p, 10.0);
    test_assert(bool(n));
    const double3 d = p - n.point;
    if (n.distance * n.distance < dot(d, d)) {
      continue; /* the root was rounded down */
    }
    int calls = 0;
    test_assert(bvh.sphereOverlap(p, n.distance, [&](int) { return ++calls > 0; }).ok());
    if (!bvh.nearest(p, n.distance) || calls != 1) {
      expect(false, "rounded", int(Layout::Far), 1, "nearest: at the radius");
    }
  }
}

void testOddInput()
{
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const double inf = std::numeric_limits<double>::infinity();
  GeometryBvh<TriangleSource<float>> bvh;

  /* Before any build, and of nothing. */
  Ray ray;
  ray.dir = double3(0, 0, 1);
  for (int k = 0; k < 2; k++) {
    test_assert(!bvh.rayClosest(ray) && !bvh.rayAny(ray) && !bvh.nearest(double3(), 1e9));
    test_assert(bvh.bvh().bounds().empty() && bvh.bvh().cost() == 0.0);
    test_assert(bvh.build(TriangleSource<float>()).ok());
  }

  TriangleSource<float> src;
  src.append(double3(-1, -1, 5), double3(1, -1, 5), double3(0, 1, 5));
  src.append(double3(nan, -1, 4), double3(1, -1, 4), double3(0, 1, 4));
  src.append(double3(-1, -1, 3), double3(1, inf, 3), double3(0, 1, 3));
  src.append(double3(-1, -1, 2), double3(1, -1, 2), double3(1e300, 1, 2)); /* inf as a float */
  src.append(double3(0, 0, 1), double3(0, 0, 1), double3(0, 0, 1));
  test_assert(bvh.build(src).ok());
  test_assert(bvh.bvh().size() == 5 && bvh.bvh().skipped() == 3);
  const RayHit h = bvh.rayClosest(ray);
  test_assert(h && h.primitive == 0 && h.t == 5.0); /* the point at z = 1 can't be hit */
  const NearestHit n = bvh.nearest(double3(0, 0, 0), 10.0);
  test_assert(n && n.primitive == 4 && n.distance == 1.0);

  /* Rays that aren't. */
  Ray bad = ray;
  bad.dir = double3();
  test_assert(!bvh.rayClosest(bad) && !bvh.rayAny(bad));
  bad.dir = double3(0, nan, 1);
  test_assert(!bvh.rayClosest(bad));
  bad = ray;
  bad.origin = double3(inf, 0, 0);
  test_assert(!bvh.rayClosest(bad));
  bad = ray;
  bad.tMin = 2.0;
  bad.tMax = 1.0;
  test_assert(!bvh.rayClosest(bad));
  bad.tMin = nan;
  test_assert(!bvh.rayClosest(bad));
  test_assert(!bvh.nearest(double3(nan, 0, 0), 1.0) && !bvh.nearest(double3(), nan) &&
              !bvh.nearest(double3(), -1.0));
  test_assert(bool(bvh.nearest(double3(), inf)));

  const auto none = [](const auto &) {
    test_assert(false);
    return true;
  };
  test_assert(bvh.rayAll(bad, none).code() == StatusCode::InvalidArgument);
  test_assert(bvh.sphereOverlap(double3(nan, 0, 0), 1.0, none).code() ==
              StatusCode::InvalidArgument);
  for (double radius : {nan, -1.0}) {
    test_assert(bvh.sphereOverlap(double3(), radius, none).code() == StatusCode::InvalidArgument);
  }
  for (int i = 0; i < 4; i++) {
    BvhPlane plane;
    plane.n = double3(0, 0, 1);
    (i < 3 ? plane.n[i] : plane.w) = i == 1 ? inf : nan;
    test_assert(bvh.region({&plane, 1}, none).code() == StatusCode::InvalidArgument);
  }

  /* Directions of any size. */
  for (double size : {1e-320, 1e-300, 1e-30, 1e30, 1e300}) {
    Ray s = ray;
    s.dir = double3(0, 0, size);
    const RayHit a = bvh.rayClosest(s);
    if (size < 1e-307) {
      test_assert(!a); /* t = 5 / size is past any tMax */
      continue;
    }
    test_assert(a && a.primitive == 0 && std::fabs(a.t * size - 5.0) < 1e-12);
  }

  /* Too many planes. */
  BvhPlane planes[9];
  test_assert(bvh.region({planes, 9}, [](int) { return true; }).code() ==
              StatusCode::InvalidArgument);

  /* Equal hits go to the lowest primitive, whatever the order in the tree. */
  TriangleSource<double> same;
  for (int i = 0; i < 300; i++) {
    same.append(double3(-1, -1, 5), double3(1, -1, 5), double3(0, 1, 5));
  }
  GeometryBvh<TriangleSource<double>> ties;
  BvhOptions opts;
  opts.leafSize = 4;
  test_assert(ties.build(same, opts).ok());
  test_assert(ties.rayClosest(ray).primitive == 0);
  test_assert(ties.nearest(double3(), 10.0).primitive == 0);

  /* Bad radii leave a primitive out. */
  CapsuleSource<float> caps;
  caps.append(double3(0, 0, 1), double3(0, 0, 2), -1.0);
  caps.append(double3(0, 0, 1), double3(0, 0, 2), nan);
  caps.append(double3(0, 0, 1), double3(0, 0, 2), inf);
  caps.append(double3(0, 0, 6), double3(0, 0, 7), 0.5);
  GeometryBvh<CapsuleSource<float>> cb;
  test_assert(cb.build(caps).ok() && cb.bvh().skipped() == 3);
  test_assert(cb.rayClosest(ray).primitive == 3 && cb.rayClosest(ray).t == 5.5);
  ray.origin = double3(0, 0, 6.5);
  test_assert(cb.rayClosest(ray).inside && cb.rayClosest(ray).t == 0.0);
}

} // namespace

int main()
{
  const int queries = 12 * coredcc_test::testScale();
  testOddInput();
  testRounded();
  allSources<float>("float", queries);
  allSources<double>("double", queries);
  test_assert(failures == 0);
  return test_end();
}
