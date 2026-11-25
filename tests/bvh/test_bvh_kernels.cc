/* The kernels against independent references, and the triangle test's watertightness. */
#include "bvh_fixture.h"

#include "test_util.h"

test_init;

using namespace bvhtest;

namespace {

int failures = 0;
void expect(bool ok, const char *what)
{
  if (!ok && failures++ < 20) {
    fprintf(stderr, "  %s\n", what);
  }
}

/* ---- a closed, bumpy sphere ---- */

struct Mesh {
  std::vector<double3> verts;
  std::vector<int> tris;

  int triCount() const
  {
    return int(tris.size() / 3);
  }
  double3 corner(int t, int k) const
  {
    return verts[size_t(tris[size_t(t * 3 + k)])];
  }
};

template <class S> Mesh bumpySphere(Rand &r, int n, int m, double3 offset)
{
  Mesh mesh;
  const auto id = [=](int i, int j) {
    return j == 0 ? 0 : j == m ? 1 : 2 + (j - 1) * n + (i % n);
  };
  const auto put = [&](double3 p) {
    p = p + offset;
    mesh.verts.push_back(double3(double(S(p[0])), double(S(p[1])), double(S(p[2]))));
  };
  put(double3(0, 0, 1));
  put(double3(0, 0, -1));
  for (int j = 1; j < m; j++) {
    for (int i = 0; i < n; i++) {
      const double th = 3.14159265 * j / m, ph = 6.2831853 * i / n, s = 1 + r.range(-0.02, 0.02);
      put(double3(std::sin(th) * std::cos(ph), std::sin(th) * std::sin(ph), std::cos(th)) * s);
    }
  }
  for (int j = 0; j < m; j++) {
    for (int i = 0; i < n; i++) {
      const int a = id(i, j), b = id(i + 1, j), c = id(i, j + 1), d = id(i + 1, j + 1);
      if (j > 0) {
        mesh.tris.insert(mesh.tris.end(), {a, c, b});
      }
      if (j < m - 1) {
        mesh.tris.insert(mesh.tris.end(), {b, c, d});
      }
    }
  }
  return mesh;
}

template <class S> TriangleSource<S> source(const Mesh &mesh)
{
  TriangleSource<S> src;
  for (int t = 0; t < mesh.triCount(); t++) {
    src.append(mesh.corner(t, 0), mesh.corner(t, 1), mesh.corner(t, 2));
  }
  return src;
}

enum class Aim { Random, Edges, Vertices, AxisThroughEdges, Outside, Count };

/* Rays that must hit the mesh, aimed at the places where a test that isn't watertight
 * lets them through. Returns how many missed. */
template <class S> int leaks(Aim aim, double3 offset, int leafSize, int rays)
{
  Rand r(uint64_t(aim) + 17);
  const Mesh mesh = bumpySphere<S>(r, 32, 16, offset);
  GeometryBvh<TriangleSource<S>> bvh;
  BvhOptions opts;
  opts.leafSize = leafSize;
  test_assert(bvh.build(source<S>(mesh), opts).ok());
  int missed = 0;
  for (int k = 0; k < rays; k++) {
    const int t = r.below(mesh.triCount()), e = r.below(3);
    const double3 a = mesh.corner(t, e), b = mesh.corner(t, (e + 1) % 3);
    const double s = r.unit();
    const double3 onEdge = a * (1.0 - s) + b * s;
    double3 origin = offset + r.vec(0.3), target = onEdge;
    switch (aim) {
      case Aim::Random:
        target = offset + r.vec(1.0);
        break;
      case Aim::Vertices:
        target = a;
        break;
      case Aim::AxisThroughEdges:
        /* Straight along an axis through the edge point, from the sphere's middle. */
        if (std::fabs(onEdge[2] - offset[2]) < 0.3) {
          continue;
        }
        origin = double3(onEdge[0], onEdge[1], offset[2]);
        break;
      case Aim::Outside: {
        const double3 p = r.below(2) ? a : onEdge;
        origin = offset + (p - offset) * r.range(2.0, 4.0) + r.vec(0.1);
        target = p;
        break;
      }
      default:
        break;
    }
    Ray ray;
    ray.origin = origin;
    ray.dir = target - origin;
    if (!bvh.rayAny(ray)) {
      missed++;
    }
  }
  return missed;
}

/* Rays exactly through the edges and vertices of an integer grid, in both windings: the
 * edge functions are exactly 0 there. */
void testLattice()
{
  const int n = 4;
  for (int axis = 0; axis < 3; axis++) {
    const auto point = [axis](double x, double y, double z) {
      double3 p;
      p[(axis + 1) % 3] = x;
      p[(axis + 2) % 3] = y;
      p[axis] = z;
      return p;
    };
    for (bool flip : {false, true}) {
      TriangleSource<float> src;
      for (int i = 0; i < n * n; i++) {
        const double x = i % n, y = i / n;
        const double3 a = point(x, y, 0), b = point(x + 1, y, 0);
        const double3 c = point(x, y + 1, 0), d = point(x + 1, y + 1, 0);
        src.append(a, flip ? c : b, flip ? b : c);
        src.append(b, flip ? c : d, flip ? d : c);
      }
      GeometryBvh<TriangleSource<float>> bvh;
      BvhOptions opts;
      opts.leafSize = 1;
      test_assert(bvh.build(src, opts).ok());
      for (int i = 0; i <= 4 * n; i++) {
        for (int j = 0; j <= 4 * n; j++) {
          const double3 p = point(i * 0.25, j * 0.25, 0);
          for (const double3 &d : {point(0, 0, 1), point(1, 2, 4), point(-3, 1, 4), point(2, 0, 4)}) {
            for (double side : {-1.0, 1.0}) {
              Ray ray;
              ray.origin = p + d * (2.0 * side);
              ray.dir = d * -side;
              const RayHit hit = bvh.rayClosest(ray);
              expect(hit && hit.t == 2.0, "a ray through a lattice point missed");
            }
          }
        }
      }
    }
  }
}

void testWatertight()
{
  const int rays = 4000 * coredcc_test::testScale();
  for (int aim = 0; aim < int(Aim::Count); aim++) {
    for (int leafSize : {1, 16, 1024}) {
      const double3 none, far(1000.0, -2000.0, 500.0);
      int n = leaks<float>(Aim(aim), none, leafSize, rays);
      n += leaks<double>(Aim(aim), none, leafSize, rays);
      n += leaks<float>(Aim(aim), far, leafSize, rays);
      n += leaks<double>(Aim(aim), far * 1000.0, leafSize, rays);
      if (n > 0) {
        fprintf(stderr, "  aim %d, leaf size %d: %d leaks\n", aim, leafSize, n);
      }
      expect(n == 0, "a ray leaked through a closed mesh");
    }
  }
}

/* ---- triangles against Moeller-Trumbore ---- */

void testTriangles()
{
  Rand r(1);
  const int n = 20000 * coredcc_test::testScale();
  for (int k = 0; k < n; k++) {
    /* Every size: the test must not have an absolute tolerance. */
    const double size = std::pow(10.0, r.range(-30, 15));
    const double3 a = r.vec(size), b = r.vec(size), c = r.vec(size);
    TriangleSource<double> src;
    src.append(a, b, c);
    Ray ray;
    const double3 dir = r.vec(1.0);
    const double dirScale = std::pow(10.0, r.range(-200, 200));
    ray.dir = dir * dirScale;
    const double u = r.range(-0.5, 1.5), v = r.range(-0.5, 1.5);
    const double3 p = a + (b - a) * u + (c - a) * v;
    const double3 unit = dir * (1.0 / length(dir));
    const double back = r.range(0.5, 3.0) * size;
    ray.origin = p - unit * back;
    const double want = back / length(dir) / dirScale;

    const double3 e1 = b - a, e2 = c - a, normal = e1.cross(e2);
    const double grazing = std::fabs(dot(normal, unit)) / (length(e1) * length(e2));
    const double edge = (std::min)((std::min)(std::fabs(u), std::fabs(v)), std::fabs(1 - u - v));
    if (grazing < 1e-3 || edge < 1e-6) {
      continue; /* the reference itself isn't sure */
    }
    const bool hit = u > 0 && v > 0 && u + v < 1;
    GeometryBvh<TriangleSource<double>> bvh;
    test_assert(bvh.build(src).ok());
    const RayHit got = bvh.rayClosest(ray);
    expect(bool(got) == hit, "a triangle hit or miss is wrong");
    if (got && hit) {
      expect(std::fabs(got.t - want) < 1e-9 * want, "t is wrong");
      expect(std::fabs(got.u - u) < 1e-9 && std::fabs(got.v - v) < 1e-9, "u or v is wrong");
      expect(!got.inside, "a triangle has no inside");
      /* The limits are inclusive, and nothing outside them is hit. */
      Ray clipped = ray;
      clipped.tMax = got.t;
      expect(bool(bvh.rayClosest(clipped)), "tMax is inclusive");
      clipped.tMax = got.t * (1.0 - 1e-12);
      expect(!bvh.rayClosest(clipped), "past tMax");
      clipped.tMax = ray.tMax;
      clipped.tMin = got.t * (1.0 + 1e-12);
      expect(!bvh.rayClosest(clipped), "before tMin");
    }
  }
}

/* ---- solids: the distance along the ray says whether an entry is right ---- */

template <class Distance, class Cast>
void checkSolid(const Ray &ray, double scale, Distance distance, Cast cast)
{
  /* distance(p): how far outside the solid p is, 0 or less inside. */
  const double dirLen = length(ray.dir);
  const auto at = [&](double t) { return ray.origin + ray.dir * t; };
  const bvh::RayQuery q(ray);
  double t = -1.0;
  bool inside = false;
  const bool hit = cast(q, q.tMax, t, inside);
  const double scaled = t;
  t = q.toRayT(t);
  const double eps = 1e-7 * scale;
  const double start = distance(at(ray.tMin));
  if (std::fabs(start) < eps) {
    return; /* on the surface: either answer */
  }
  expect((hit && inside) == (start < 0), "inside is wrong");
  if (start < 0) {
    expect(hit && t == ray.tMin, "a start inside hits at tMin");
    return;
  }
  /* The distance to a convex solid is convex along a line: ternary search for the least. */
  double lo = ray.tMin, hi = ray.tMin + 100.0 * scale / dirLen;
  for (int i = 0; i < 200; i++) {
    const double m1 = lo + (hi - lo) / 3, m2 = hi - (hi - lo) / 3;
    (distance(at(m1)) < distance(at(m2)) ? hi : lo) = distance(at(m1)) < distance(at(m2)) ? m2 : m1;
  }
  const double least = distance(at(lo));
  if (std::fabs(least) < eps) {
    return; /* grazing */
  }
  expect(hit == (least < 0), "a solid's hit or miss is wrong");
  if (!hit || least >= 0) {
    return;
  }
  expect(std::fabs(distance(at(t))) < eps, "the hit isn't on the surface");
  double a = ray.tMin, b = lo; /* outside at a, inside at b: bisect for the entry */
  for (int i = 0; i < 200; i++) {
    const double m = 0.5 * (a + b);
    (distance(at(m)) > 0 ? a : b) = m;
  }
  expect(std::fabs(a - t) * dirLen < 1e-5 * scale, "not the first entry");
  expect(!cast(q, q.tMin + 0.999 * (scaled - q.tMin), t, inside), "past the limit");
}

void testSolids()
{
  Rand r(2);
  const int n = 4000 * coredcc_test::testScale();
  for (int k = 0; k < n; k++) {
    const double scale = std::pow(10.0, r.range(-10, 10));
    Ray ray;
    ray.origin = r.vec(3.0 * scale);
    ray.dir = r.vec(1.0) * std::pow(10.0, r.range(-3, 3));
    if (r.below(4) == 0) {
      ray.dir[r.below(3)] = 0.0;
    }
    if (length(ray.dir) == 0.0) {
      continue;
    }
    const double3 a = r.vec(scale);
    double3 b = r.vec(scale);
    if (r.below(8) == 0) {
      b = a;
    }
    if (r.below(8) == 0) { /* along the axis */
      ray.dir = (b - a) * (1.0 / scale) + double3(r.below(2) ? 0.0 : 1e-9, 0, 0);
      if (length(ray.dir) == 0.0) {
        continue;
      }
    }
    const double radius = r.range(0.01, 0.8) * scale;
    const auto seg = [&](const double3 &p) {
      return length(p - bvh::closestOnSegment(p, a, b)) - radius;
    };
    checkSolid(ray, scale, seg, [&](const bvh::RayQuery &q, double limit, double &t, bool &in) {
      return bvh::rayCapsule(q, a, b, radius, limit, t, in);
    });
    checkSolid(
        ray,
        scale,
        [&](const double3 &p) { return length(p - a) - radius; },
        [&](const bvh::RayQuery &q, double limit, double &t, bool &in) {
          return bvh::raySphere(q, a, radius, limit, t, in);
        });
    Box3d box;
    box.include(a[0], a[1], a[2]);
    box.include(a[0] + radius, a[1] + 2 * radius, a[2] + radius * 0.5);
    checkSolid(
        ray,
        scale,
        [&](const double3 &p) {
          if (!box.contains(p)) {
            return std::sqrt(box.distanceSq(p));
          }
          double d = -1e300;
          for (int i = 0; i < 3; i++) {
            d = (std::max)(d, (std::max)(box.min[i] - p[i], p[i] - box.max[i]));
          }
          return d;
        },
        [&](const bvh::RayQuery &q, double limit, double &t, bool &in) {
          return bvh::rayBox(q, box, limit, t, in);
        });
  }
  double t;
  bool in;
  Ray ray;
  ray.dir = double3(1, 0, 0);
  const bvh::RayQuery q(ray);
  const double nan = std::numeric_limits<double>::quiet_NaN();
  expect(!bvh::raySphere(q, double3(5, 0, 0), -1.0, q.tMax, t, in), "negative radius");
  expect(!bvh::raySphere(q, double3(5, 0, 0), nan, q.tMax, t, in), "NaN radius");
  expect(!bvh::rayCapsule(q, double3(5, 0, 0), double3(6, 0, 0), nan, q.tMax, t, in), "NaN radius");
  expect(bvh::raySphere(q, double3(5, 0, 0), 1.0, 8.0 * (1 + 1e-12), t, in) && q.toRayT(t) == 4.0, "entry");
  expect(!bvh::raySphere(q, double3(5, 0, 0), 1.0, 7.9, t, in), "a limit before the entry");
  expect(!bvh::raySphere(q, double3(-5, 0, 0), 1.0, q.tMax, t, in), "behind the ray");
}

/* ---- closest points: the optimality condition of a convex set ---- */

void testClosest()
{
  Rand r(3);
  const int n = 20000 * coredcc_test::testScale();
  for (int k = 0; k < n; k++) {
    const double scale = std::pow(10.0, r.range(-20, 20));
    double3 a = r.vec(scale), b = r.vec(scale), c = r.vec(scale);
    const int kind = r.below(8);
    if (kind == 0) {
      c = b;
    } else if (kind == 1) {
      b = c = a;
    } else if (kind == 2) {
      c = a + (b - a) * r.range(-1, 2); /* collinear, up to rounding */
    }
    /* Some on the line of a triangle without area, where its normal is noise. */
    const double3 p = kind == 2 && r.unit() < 0.5 ? a + (b - a) * r.range(-1, 2) : r.vec(2.0 * scale);
    const double tol = 1e-9 * scale * scale;
    const double3 t = bvh::closestOnTriangle(p, a, b, c);
    /* (p - t).(x - t) <= 0 for every x of the set: its corners are enough. */
    for (const double3 &x : {a, b, c}) {
      expect(dot(p - t, x - t) <= tol, "closestOnTriangle isn't the closest");
    }
    /* t is in the triangle: closer to none of its edges' outside. */
    const double3 normal = (b - a).cross(c - a);
    if (length(normal) > 1e-3 * scale * scale) {
      const double area = dot(normal, normal);
      const double u = dot((b - a).cross(t - a), normal) / area;
      const double v = dot((t - a).cross(c - a), normal) / area;
      expect(u > -1e-9 && v > -1e-9 && u + v < 1 + 1e-9, "outside the triangle");
      expect(std::fabs(dot(t - a, normal)) < 1e-9 * length(normal) * scale, "off the plane");
    }
    /* A point of a thin triangle, not of its edges. */
    const double3 thin = a + (b - a) * 0.4 + (b - a).cross(r.vec(1.0)) * 1e-5;
    const double3 in = (a + b + thin) / 3.0;
    expect(length(bvh::closestOnTriangle(in, a, b, thin) - in) <= 1e-8 * scale, "a sliver");
    const double3 s = bvh::closestOnSegment(p, a, b);
    expect(dot(p - s, a - s) <= tol && dot(p - s, b - s) <= tol, "closestOnSegment");

    Box3d box;
    box.include(a[0], a[1], a[2]);
    box.include(b[0], b[1], b[2]);
    const double3 x = bvh::closestOnBox(p, box);
    expect(box.contains(x), "closestOnBox is outside");
    expect(std::fabs(dot(p - x, p - x) - box.distanceSq(p)) <= 1e-12 * dot(p - x, p - x),
           "closestOnBox and distanceSq disagree");
    const double radius = r.unit() * scale;
    const double3 y = bvh::closestOnSphere(p, a, radius);
    const double d = length(p - a);
    expect(std::fabs(length(p - y) - (std::max)(0.0, d - radius)) <= 1e-12 * scale, "sphere");
  }
}

/* ---- boxes ---- */

/* The sign of `k[0] + k[1] x[0] + k[2] x[1] + k[3] x[2] - bound`, in twice the precision of
 * double (two-sums and two-products). */
int sumSign(const double *k, const double *x, double bound)
{
  double hi = k[0], lo = 0.0;
  const auto add = [&](double v) {
    const double s = hi + v, b = s - hi;
    lo += (hi - (s - b)) + (v - b);
    hi = s;
  };
  add(-bound);
  for (int i = 0; i < 3; i++) {
    const double p = k[i + 1] * x[i];
    add(p);
    add(std::fma(k[i + 1], x[i], -p));
  }
  const double s = hi + lo;
  return s > 0.0 ? 1 : s < 0.0 ? -1 : 0;
}

void testBoxes()
{
  Rand r(4);
  const Box3d empty;
  double m[16] = {2, 0, 0, 0, 0, -3, 0, 0, 0, 0, 1, 0, 5, 6, 7, 1};
  expect(empty.empty() && transformBox(empty, m).empty(), "an empty box stays empty");
  expect(empty.allFinite() && !empty.contains(double3()), "an empty box is finite");
  for (int k = 0; k < 4000 * coredcc_test::testScale(); k++) {
    for (int i = 0; i < 16; i++) {
      m[i] = (i % 4 == 3) ? 0.0 : r.range(-1, 1) * std::pow(10.0, r.range(-8, 8));
    }
    Box3d b;
    for (int i = 0; i < 2; i++) {
      const double3 p = r.vec(std::pow(10.0, r.range(-5, 5)));
      b.include(p[0], p[1], p[2]);
    }
    const Box3d t = transformBox(b, m);
    for (int c = 0; c < 8; c++) {
      const double x[3] = {c & 1 ? b.max[0] : b.min[0],
                           c & 2 ? b.max[1] : b.min[1],
                           c & 4 ? b.max[2] : b.min[2]};
      for (int row = 0; row < 3; row++) {
        const double exact[4] = {m[12 + row], m[row], m[4 + row], m[8 + row]};
        expect(sumSign(exact, x, t.min[row]) >= 0 && sumSign(exact, x, t.max[row]) <= 0,
               "transformBox isn't outward");
      }
    }
  }
  m[0] = 1e200;
  Box3d big;
  big.include(1e200, 0, 0);
  big.include(-1e200, 0, 0);
  expect(!transformBox(big, m).allFinite(), "overflow isn't finite");

  const double inf = std::numeric_limits<double>::infinity();
  const float finf = std::numeric_limits<float>::infinity();
  const float fmax = (std::numeric_limits<float>::max)();
  expect(bvh::roundDown<float>(1e300) == fmax && bvh::roundUp<float>(1e300) == finf, "over");
  expect(bvh::roundDown<float>(-1e300) == -finf && bvh::roundUp<float>(-1e300) == -fmax, "under");
  expect(!bvh::finite(inf) && !bvh::finite(finf) && bvh::finite(fmax) &&
             !bvh::finite(std::numeric_limits<double>::quiet_NaN()),
         "finite");
  for (int k = 0; k < 20000; k++) {
    const double v = r.range(-1, 1) * std::pow(10.0, r.range(-44, 37));
    const float d = bvh::roundDown<float>(v), u = bvh::roundUp<float>(v);
    expect(double(d) <= v && double(u) >= v && (d == u || std::nextafter(d, finf) == u),
           "roundDown and roundUp are tight");
  }
}

} // namespace

int main()
{
  testBoxes();
  testTriangles();
  testSolids();
  testClosest();
  testLattice();
  testWatertight();
  test_assert(failures == 0);
  return test_end();
}
