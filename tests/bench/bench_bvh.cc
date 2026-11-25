/* The BVH leaf-size benchmark: one JSON object per line on stdout. Run it through
 * tools/bench-bvh.mjs, which builds it, decides and writes the report.
 *
 *   bench_bvh --source triangles|capsules|spheres --count N --leaf a,b,c [--queries N]
 *             [--reps N] */
#include "coredcc/bvh/geometry_bvh.h"

#include "litestl/util/vector.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace coredcc;
using litestl::util::Vector;

namespace {

struct Rand {
  uint64_t state;
  explicit Rand(uint64_t seed) : state(seed) {}
  uint64_t next()
  {
    state += 0x9E3779B97F4A7C15ull;
    uint64_t z = state;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
  }
  double unit()
  {
    return double(next() >> 11) * 0x1p-53;
  }
  double range(double a, double b)
  {
    return a + (b - a) * unit();
  }
  double3 vec(double size)
  {
    const double x = range(-size, size), y = range(-size, size);
    return double3(x, y, range(-size, size));
  }
};

using Clock = std::chrono::steady_clock;
double ms(Clock::time_point from)
{
  return std::chrono::duration<double, std::milli>(Clock::now() - from).count();
}

double3 onSphere(double u, double v, double wobble)
{
  const double ph = 6.283185307179586 * u, th = 3.141592653589793 * v;
  const double r = 1.0 + wobble * std::sin(9 * ph) * std::sin(11 * th);
  return double3(std::sin(th) * std::cos(ph), std::sin(th) * std::sin(ph), std::cos(th)) * r;
}

/* Every source is inside about [-1.1, 1.1] and in random order. `phase` moves it a
 * little, for refits. */

/* A bumpy sphere's surface. */
TriangleSource<float> makeTriangles(int count, double phase)
{
  const int rows = int(std::sqrt(count / 4.0)), cols = rows * 2;
  Vector<int> cells;
  for (int i = 0; i < rows * cols; i++) {
    cells.append(i);
  }
  Rand r(1);
  for (int i = int(cells.size()) - 1; i > 0; i--) {
    std::swap(cells[i], cells[int(r.next() % uint64_t(i + 1))]);
  }
  TriangleSource<float> src;
  src.ensureCapacity(rows * cols * 2);
  for (int cell : cells) {
    const double v0 = double(cell / cols) / rows, v1 = v0 + 1.0 / rows;
    const double u0 = double(cell % cols) / cols + phase, u1 = u0 + 1.0 / cols;
    const double3 a = onSphere(u0, v0, 0.05), b = onSphere(u1, v0, 0.05);
    const double3 c = onSphere(u0, v1, 0.05), d = onSphere(u1, v1, 0.05);
    src.append(a, c, b);
    src.append(b, c, d);
  }
  return src;
}

/* Strands of 16 segments growing out of a sphere. */
CapsuleSource<float> makeCapsules(int count, double phase)
{
  Rand r(2);
  CapsuleSource<float> src;
  src.ensureCapacity(count);
  const double step = 0.5 / std::sqrt(double(count));
  for (int made = 0; made < count;) {
    double3 p = onSphere(r.unit() + phase, r.unit(), 0.0);
    double3 dir = p;
    for (int k = 0; k < 16 && made < count; k++, made++) {
      dir = dir + r.vec(0.3);
      const double3 q = p + dir * step;
      src.append(p, q, step * 0.1);
      p = q;
    }
  }
  return src;
}

/* A solid cloud. */
SphereSource<float> makeSpheres(int count, double phase)
{
  Rand r(3);
  SphereSource<float> src;
  src.ensureCapacity(count);
  const double radius = 0.5 / std::cbrt(double(count));
  for (int i = 0; i < count; i++) {
    src.append(r.vec(1.0) + double3(phase, 0, 0), radius * r.unit());
  }
  return src;
}

Vector<Ray> coherentRays(int count)
{
  const int side = int(std::sqrt(double(count)));
  Vector<Ray> rays;
  for (int i = 0; i < side * side; i++) {
    Ray ray;
    ray.origin = double3(0.3, -3.5, 0.4);
    const double x = (double(i % side) + 0.5) / side - 0.5;
    const double y = (double(i / side) + 0.5) / side - 0.5;
    ray.dir = double3(x * 0.8, 1.0, y * 0.8);
    rays.append(ray);
  }
  return rays;
}

Vector<Ray> randomRays(int count)
{
  Rand r(4);
  Vector<Ray> rays;
  for (int i = 0; i < count; i++) {
    Ray ray;
    ray.origin = r.vec(2.0);
    ray.dir = r.vec(1.0) - ray.origin;
    rays.append(ray);
  }
  return rays;
}

/* Small pick frusta along rays: four planes through the eye, and a near plane. */
struct Region {
  BvhPlane planes[5];
};
Vector<Region> regions(const Vector<Ray> &rays, int count)
{
  Vector<Region> out;
  Rand r(6);
  for (int i = 0; i < count; i++) {
    const Ray &ray = rays[int(r.next() % rays.size())];
    const double3 d = ray.dir.normalized();
    const double3 u = d.cross(double3(0.31, 0.21, 0.92)).normalized(), v = d.cross(u);
    const double half = 0.006; /* about 8 of 1000 pixels */
    Region g;
    const double3 n[5] = {d * half + u, d * half - u, d * half + v, d * half - v, d};
    for (int k = 0; k < 5; k++) {
      g.planes[k].n = n[k];
      g.planes[k].w = -n[k].dot(ray.origin) - (k == 4 ? 0.1 : 0.0);
    }
    out.append(g);
  }
  return out;
}

struct Args {
  const char *source = "triangles";
  int count = 10000, queries = 1 << 16, reps = 5;
  Vector<int> leaves;
};

template <class Src, class Make> int run(const Args &args, Make make)
{
  const Src src = make(args.count, 0.0), moved = make(args.count, 0.003);
  const Vector<Ray> coherent = coherentRays(args.queries), random = randomRays(args.queries);
  const Vector<Region> picks = regions(coherent, args.queries / 16);
  for (int leaf : args.leaves) {
    GeometryBvh<Src> bvh;
    BvhOptions opts;
    opts.leafSize = leaf;
    double build = 1e300, buildSerial = 1e300, refit = 1e300;
    double tCoherent = 1e300, tRandom = 1e300, tRegion = 1e300;
    uint64_t sum = 0, found = 0;
    for (int rep = 0; rep < args.reps; rep++) {
      opts.parallel = false;
      auto t = Clock::now();
      Bvh<float> alone;
      if (!alone.build(src, opts).ok()) {
        return 1;
      }
      buildSerial = std::fmin(buildSerial, ms(t));
      opts.parallel = true;
      t = Clock::now();
      if (!bvh.build(src, opts).ok()) {
        return 1;
      }
      build = std::fmin(build, ms(t));
      t = Clock::now();
      if (!alone.refit(moved).ok()) {
        return 1;
      }
      refit = std::fmin(refit, ms(t));

      const auto rays = [&](const Vector<Ray> &set) {
        const auto start = Clock::now();
        for (const Ray &ray : set) {
          sum += uint64_t(bvh.rayClosest(ray).primitive + 1);
        }
        return ms(start) * 1e6 / double(set.size());
      };
      tCoherent = std::fmin(tCoherent, rays(coherent));
      tRandom = std::fmin(tRandom, rays(random));
      t = Clock::now();
      for (const Region &g : picks) {
        const Status s = bvh.region(g.planes, [&](int) {
          found++;
          return true;
        });
        if (!s.ok()) {
          return 1;
        }
      }
      tRegion = std::fmin(tRegion, ms(t) * 1e6 / double(picks.size()));
    }
    printf("{\"source\":\"%s\",\"count\":%d,\"leaf\":%d,\"buildMs\":%.4f,\"serialBuildMs\":%.4f,"
           "\"refitMs\":%.4f,\"coherentNs\":%.1f,\"randomNs\":%.1f,\"regionNs\":%.1f,"
           "\"nodes\":%d,\"nodeBytes\":%.0f,\"cost\":%.3f,\"check\":%llu}\n",
           args.source,
           src.size(),
           leaf,
           build,
           buildSerial,
           refit,
           tCoherent,
           tRandom,
           tRegion,
           int(bvh.bvh().nodes().size()),
           double(bvh.bvh().nodes().size() * sizeof(BvhNode<float>)),
           bvh.bvh().cost(),
           (unsigned long long)(sum + found) / (unsigned long long)args.reps);
    fflush(stdout);
  }
  return 0;
}

} // namespace

int main(int argc, char **argv)
{
  Args args;
  for (int i = 1; i + 1 < argc; i += 2) {
    const char *name = argv[i], *value = argv[i + 1];
    if (strcmp(name, "--source") == 0) {
      args.source = value;
    } else if (strcmp(name, "--count") == 0) {
      args.count = atoi(value);
    } else if (strcmp(name, "--queries") == 0) {
      args.queries = atoi(value);
    } else if (strcmp(name, "--reps") == 0) {
      args.reps = atoi(value);
    } else if (strcmp(name, "--leaf") == 0) {
      for (const char *p = value; *p;) {
        args.leaves.append(atoi(p));
        const char *comma = strchr(p, ',');
        p = comma ? comma + 1 : p + strlen(p);
      }
    } else {
      fprintf(stderr, "unknown option %s\n", name);
      return 2;
    }
  }
  if (args.count < 1 || args.queries < 16 || args.reps < 1 || args.leaves.size() == 0) {
    fprintf(stderr, "usage: bench_bvh --source S --count N --leaf a,b,c\n");
    return 2;
  }
  if (strcmp(args.source, "triangles") == 0) {
    return run<TriangleSource<float>>(args, makeTriangles);
  }
  if (strcmp(args.source, "capsules") == 0) {
    return run<CapsuleSource<float>>(args, makeCapsules);
  }
  if (strcmp(args.source, "spheres") == 0) {
    return run<SphereSource<float>>(args, makeSpheres);
  }
  fprintf(stderr, "unknown source %s\n", args.source);
  return 2;
}
