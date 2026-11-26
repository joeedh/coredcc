/* View and ViewMath: every convention, at scene offsets up to 1e9. */
#include "coredcc/draw/view.h"

#include "coredcc/bvh/geometry_bvh.h"

#include "litestl/binding/binding.h"
#include "litestl/math/matrix_bindings.h"

#include "fuzz.h"
#include "litestl/tests/test_util.h"

#include <cmath>
#include <cstring>
#include <functional>
#include <limits>

test_init;

using namespace coredcc;
using litestl::math::lookAt;
using litestl::math::orthographic;
using litestl::math::perspective;

namespace {

struct Rand {
  uint64_t state;
  explicit Rand(uint64_t seed) : state(seed)
  {
  }
  uint64_t next()
  {
    state += 0x9E3779B97F4A7C15ull;
    uint64_t z = state;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
  }
  double range(double a, double b)
  {
    return a + (b - a) * (double(next() >> 11) * 0x1p-53);
  }
  double3 vec(double size)
  {
    const double x = range(-size, size), y = range(-size, size);
    return double3(x, y, range(-size, size));
  }
  bool coin()
  {
    return (next() & 1) != 0;
  }
};

int g_failures = 0;
int g_past = 0; /* points past a far plane */
void expect(bool ok, const char *what)
{
  if (!ok && g_failures++ < 20) {
    fprintf(stderr, "  %s\n", what);
  }
}

mat4d diagonal(double x, double y, double z)
{
  mat4d m;
  m[0][0] = x;
  m[1][1] = y;
  m[2][2] = z;
  m[3][3] = 1.0;
  return m;
}

/* A view and what it was made from. */
struct Case {
  View v;
  double zNear = 0.1, zFar = 1000;
  double3 eye, offset;
  mat4d camera; /* world (after the mirroring of a left-handed one) to view, unscaled */
  double scale = 1.0;
};

struct Convention {
  bool minusOne, reversed, infinite, ortho, left, leftViewSpace;
};

Case makeCase(const Convention &c, Rand &r, double offset)
{
  Case k;
  k.offset = offset > 0.0 ? r.vec(1.0) * offset : double3();
  for (int i = 0; i < 3; i++) {
    k.offset[i] =
        std::round(k.offset[i]); /* so that adding it to a point is nearly exact */
  }
  k.eye = r.vec(3.0);
  k.scale = r.coin() ? 1.0 : r.range(0.1, 10.0);
  k.zNear = c.ortho ? r.range(-5.0, 5.0) : r.range(0.01, 1.0);
  k.zFar = c.infinite ? std::numeric_limits<double>::infinity() : r.range(20.0, 1000.0);

  View &v = k.v;
  v.viewport[0] = std::round(r.range(-50, 200));
  v.viewport[1] = std::round(r.range(-50, 200));
  v.viewport[2] = std::round(r.range(50, 4000));
  v.viewport[3] = std::round(r.range(50, 3000));
  v.dpiScale = r.coin() ? 1.0 : 2.0;
  v.depth = c.minusOne ? DepthRange::MinusOneToOne : DepthRange::ZeroToOne;
  v.reversedZ = c.reversed;
  v.infiniteFar = c.infinite;
  v.ortho = c.ortho;
  v.handedness = c.left ? Handedness::Left : Handedness::Right;

  /* A left-handed world is a mirrored right-handed one. */
  const double3 mirror(1.0, 1.0, c.left ? -1.0 : 1.0);
  const mat4d cam = lookAt(double3((k.eye + k.offset) * mirror),
                           double3((k.eye + k.offset + r.vec(1.0)) * mirror),
                           r.vec(1.0));
  k.camera = diagonal(k.scale, k.scale, k.scale) * cam * diagonal(1, 1, mirror[2]);
  const double aspect = v.viewport[2] / v.viewport[3];
  const double n = k.zNear * k.scale, f = k.zFar * k.scale;
  const double h = r.range(0.5, 5.0) * k.scale;
  mat4d proj =
      c.ortho ? orthographic(-h * aspect, h * aspect, -h, h, n, f, v.depth, c.reversed)
              : perspective(r.range(0.2, 2.0), aspect, n, f, v.depth, c.reversed);
  if (r.coin()) { /* off-center, and scaled as a whole (w isn't -z then) */
    if (c.ortho) {
      proj[3][0] = r.range(-1.0, 1.0);
    } else {
      proj[2][0] = r.range(-1.0, 1.0);
      proj[2][1] = r.range(-1.0, 1.0);
      double *d = proj;
      for (int i = 0; i < 16; i++) {
        d[i] *= 3.0;
      }
    }
  }
  /* View space itself may be left-handed: forward is then +z. */
  const mat4d flip = diagonal(1, 1, c.leftViewSpace ? -1.0 : 1.0);
  v.view = flip * k.camera;
  v.proj = proj * flip;
  return k;
}

/* A world point in front of the camera: `x` and `y` in normalized device coordinates, at
 * a distance from the near plane. */
double3 pointAt(const Case &k, const ViewMath &m, double x, double y, double distance)
{
  const double *vp = k.v.viewport;
  Ray ray =
      m.rayFromPixel(vp[0] + (0.5 + 0.5 * x) * vp[2], vp[1] + (0.5 - 0.5 * y) * vp[3]);
  /* Along the ray to a distance along the view direction. */
  const double step = m.viewDistance(ray.origin + ray.dir) - m.viewDistance(ray.origin);
  return ray.origin + ray.dir * (distance / step);
}

void testCase(const Convention &c, Rand &r, double offset)
{
  Case k = makeCase(c, r, offset);
  const Status valid = k.v.validate();
  expect(valid.ok(), "a valid view is refused");
  ViewMath m;
  if (!valid.ok() || !m.set(k.v).ok()) {
    fprintf(stderr, "  %s\n", valid.message());
    return;
  }
  const double *vp = k.v.viewport;
  const double px = 1e-3; /* pixels */
  const double rel = 1e-9 + offset * 1e-14;
  const double maxDistance = (std::min)(k.zFar - k.zNear, 50.0);
  const bool exact = offset <= 1e6;

  /* The camera's axes in the world, from the camera alone. */
  const mat4d toCamera = k.camera;
  for (int i = 0; i < 20; i++) {
    const double x = r.range(-1.2, 1.2), y = r.range(-1.2, 1.2);
    const double distance = r.range(0.01, maxDistance);
    const double3 p = pointAt(k, m, x, y, distance);

    /* Camera space: z is minus the distance from the eye. */
    const double3 q = litestl::math::transformPoint(toCamera, p) * (1.0 / k.scale);
    const ViewPoint at = m.project(p);
    expect(!at.behind && at.visible, "a point in front is hidden");
    if (exact) {
      expect(std::fabs(-q[2] - k.zNear - distance) <
                 1e-6 * (1 + distance + offset * 1e-3),
             "the distance is wrong");
      expect(std::fabs(at.distance - distance) < 1e-6 * (1 + distance + offset * 1e-3),
             "project's distance is wrong");
      expect(std::fabs(m.viewDistance(p) - at.distance) == 0.0, "viewDistance differs");
      const double wantX = vp[0] + (0.5 + 0.5 * x) * vp[2];
      const double wantY = vp[1] + (0.5 - 0.5 * y) * vp[3];
      expect(std::fabs(at.x - wantX) < px && std::fabs(at.y - wantY) < px,
             "the pixel is wrong");
    }
    /* Depth against the convention's own formula. */
    if (offset == 0.0) {
      const double n = k.zNear, f = k.zFar, d = distance + n;
      double t = c.ortho      ? (d - n) / (f - n)
                 : c.infinite ? 1.0 - n / d
                              : (f / (f - n)) * (1.0 - n / d);
      t = c.reversed ? 1.0 - t : t;
      const double want = c.minusOne ? 2.0 * t - 1.0 : t;
      expect(std::fabs(at.depth - want) < 1e-8, "the depth is wrong");
      const Result<double3> back = m.unproject(at.x, at.y, at.depth);
      expect(back.ok(), "unproject failed");
      if (back.ok()) {
        /* The depth's rounding is worth about d * d / n. */
        expect(double3(*back - p).length() <
                   1e-13 * (1.0 + d * d / std::fabs(c.ortho ? 1.0 : n)) * 100,
               "unproject isn't the inverse of project");
      }
      double3 out;
      expect(m.unprojectTo(at.x, at.y, at.depth, out).ok() &&
                 memcmp(&out, &*back, sizeof(out)) == 0,
             "unprojectTo differs");
    }

    /* The ray starts on the near plane, goes forward, and has unit length. */
    const Ray ray = m.rayFromPixel(at.x, at.y);
    expect(ray.valid() && ray.tMin == 0.0, "the ray is invalid");
    expect(std::fabs(ray.dir.length() - 1.0) < 1e-12,
           "the ray's direction isn't unit length");
    expect(std::fabs(m.viewDistance(ray.origin)) < rel * 100 + offset * 1e-14,
           "the ray doesn't start on the near plane");
    expect(m.viewDistance(ray.origin + ray.dir) > 0.0 || offset > 1e6,
           "the ray goes backward");
    expect(memcmp(&ray.origin,
                  &static_cast<const double3 &>(m.rayOrigin(at.x, at.y)),
                  sizeof(double3)) == 0,
           "rayOrigin differs");

    /* The rows and the near plane agree with project. */
    const double4 *rows = m.pixelRows();
    const auto dot = [&](const double4 &a) {
      return a[0] * p[0] + a[1] * p[1] + a[2] * p[2] + a[3];
    };
    if (offset == 0.0) {
      expect(std::fabs(dot(rows[0]) / dot(rows[2]) - at.x) < 1e-6 &&
                 std::fabs(dot(rows[1]) / dot(rows[2]) - at.y) < 1e-6,
             "pixelRows is wrong");
      expect(dot(m.nearPlane()) > 0.0, "nearPlane is wrong");
      const double3 n(m.nearPlane()[0], m.nearPlane()[1], m.nearPlane()[2]);
      const double3 before = p - n * ((distance + 0.005) / n.length());
      const double side = n.dot(before) + m.nearPlane()[3];
      expect(side < 0.0 && !m.project(before).visible,
             "a point before the near plane is visible");
      const double4 &far = m.farPlane();
      expect(dot(far) > 0.0, "farPlane is wrong");
      if (c.infinite) {
        expect(far[0] == 0.0 && far[1] == 0.0 && far[2] == 0.0 && far[3] == 1.0,
               "no far plane");
      } else {
        const double3 past = p + n * ((k.zFar - k.zNear - distance) * 1.001 / n.length());
        const double there =
            far[0] * past[0] + far[1] * past[1] + far[2] * past[2] + far[3];
        expect(there < 0.0 && !m.project(past).visible,
               "a point past the far plane is visible");
        expect(!m.project(past).behind, "a point past the far plane is behind");
      }
      for (int e = 0; e < 4; e++) {
        expect(m.nearPlaneAt(e) == m.nearPlane()[e] && m.farPlaneAt(e) == far[e],
               "elements");
        for (int row = 0; row < 3; row++) {
          expect(m.pixelRowAt(row, e) == rows[row][e], "pixelRowAt");
        }
      }
    }

    /* One logical pixel along the image's y is dpiScale device pixels. */
    if (offset == 0.0) {
      const double size = m.pixelSizeAt(p);
      /* Camera up in the world: the camera's second row over its scale. */
      const double *d = k.camera;
      const double3 up = double3(d[1], d[5], d[9]) * (1.0 / k.scale);
      const ViewPoint moved = m.project(p + up * size);
      expect(std::fabs((at.y - moved.y) - k.v.dpiScale) < 1e-4, "pixelSizeAt is wrong");
    }
  }

  if (!c.ortho) {
    /* Behind the eye. */
    const double *d = k.camera;
    const double3 backward = double3(d[2], d[6], d[10]) * (1.0 / k.scale);
    if (offset == 0.0) {
      const ViewPoint b = m.project(k.eye + backward * 2.0);
      expect(b.behind && !b.visible && b.distance < 0.0, "a point behind the eye");
      expect(m.pixelSizeAt(k.eye + backward * 2.0) == 0.0,
             "the pixel size behind the eye");
    }
  }
  if (c.infinite) {
    const double farDepth = c.reversed ? (c.minusOne ? -1.0 : 0.0) : 1.0;
    expect(m.unproject(10, 10, farDepth).status().code() ==
               StatusCode::FailedPrecondition,
           "the infinite far plane has a point");
  } else if (!exact) {
    /* nothing more */
  } else {
    const double farDepth = c.reversed ? (c.minusOne ? -1.0 : 0.0) : 1.0;
    const Result<double3> f = m.unproject(vp[0] + vp[2] / 2, vp[1] + vp[3] / 2, farDepth);
    expect(f.ok() && std::fabs(m.viewDistance(*f) - (k.zFar - k.zNear)) < 1e-6 * k.zFar,
           "the far plane");
  }
}

/* Every point that projects into the square, past the near plane, is inside every plane;
 * a point well outside is outside of some plane. */
void testRegion(const Convention &c, Rand &r, double offset)
{
  Case k = makeCase(c, r, offset);
  if (offset == 0.0 &&
      r.coin()) { /* the eye at the origin: the planes have no constant */
    for (int i = 0; i < 3; i++) {
      k.v.view[3][i] = 0.0;
    }
  }
  ViewMath m;
  if (!m.set(k.v).ok()) {
    expect(false, "a valid view is refused");
    return;
  }
  const double *vp = k.v.viewport;
  for (int i = 0; i < 20; i++) {
    const double radius = i % 4 == 0 ? r.range(0.0, 0.2) : r.range(0.0, 20.0);
    double wantX = (std::max)(radius * k.v.dpiScale, 0.5), wantY = wantX;
    const double cx = vp[0] + r.range(0, 1) * vp[2], cy = vp[1] + r.range(0, 1) * vp[3];
    ViewRegion g = m.regionFromPixel(cx, cy, radius);
    if (i % 3 == 0) {
      /* A rectangle, from any of its corners to the opposite one. */
      const double hx = r.range(-100.0, 100.0),
                   hy = r.coin() ? r.range(-100.0, 100.0) : 0.1;
      g = m.regionFromRect(cx - hx, cy - hy, cx + hx, cy + hy);
      wantX = (std::max)(std::fabs(hx), 0.5);
      wantY = (std::max)(std::fabs(hy), 0.5);
    }
    for (int p = 0; p < ViewRegion::kPlanes; p++) {
      expect(g.planeAt(p, 0) == g.planes[p].n[0] && g.planeAt(p, 2) == g.planes[p].n[2] &&
                 g.planeAt(p, 3) == g.planes[p].w,
             "planeAt");
    }
    for (int j = 0; j < 30; j++) {
      /* Near the square's border. */
      const double reach = j % 3 == 0   ? r.range(0.0, 1.0)
                           : j % 3 == 1 ? 1.0
                                        : r.range(0.999, 1.001);
      const bool flip = r.coin();
      const double edge = reach * (r.coin() ? 1 : -1), along = r.range(-1.0, 1.0);
      const double dx = (flip ? along : edge) * wantX, dy = (flip ? edge : along) * wantY;
      const double ndcX = (cx + dx - vp[0]) / vp[2] * 2.0 - 1.0;
      const double ndcY = 1.0 - (cy + dy - vp[1]) / vp[3] * 2.0;
      const double3 p = pointAt(
          k, m, ndcX, ndcY, j % 5 == 0 ? r.range(-0.01, 0.01) : std::exp(r.range(-5, 8)));
      const ViewPoint at = m.project(p);
      const bool in =
          at.visible && std::fabs(at.x - cx) <= wantX && std::fabs(at.y - cy) <= wantY;
      bool inside = true;
      for (const BvhPlane &plane : g.planes) {
        inside = inside && plane.n.dot(p) + plane.w >= 0.0;
      }
      expect(!in || inside, "the region excludes a point in the square");
      if (offset <= 1e3 && !at.behind) {
        const bool farOut = std::fabs(at.x - cx) > wantX * 1.01 + 0.01 ||
                            std::fabs(at.y - cy) > wantY * 1.01 + 0.01 ||
                            at.distance < -1e-6 ||
                            at.distance > (k.zFar - k.zNear) * (1.0 + 1e-6);
        expect(!(farOut && inside), "the region is larger than the square");
        g_past += at.distance > k.zFar - k.zNear;
      }
    }
  }
}

void testConventions()
{
  Rand r(11);
  const int reps = 6 * coredcc_test::testScale();
  for (int bits = 0; bits < 64; bits++) {
    Convention c{(bits & 1) != 0,
                 (bits & 2) != 0,
                 (bits & 4) != 0,
                 (bits & 8) != 0,
                 (bits & 16) != 0,
                 (bits & 32) != 0};
    if (c.ortho && c.infinite) {
      continue;
    }
    for (int i = 0; i < reps; i++) {
      for (double offset : {0.0, 1e3, 1e6}) {
        testCase(c, r, offset);
      }
      for (double offset : {0.0, 1e3, 1e6, 1e9}) {
        testRegion(c, r, offset);
      }
    }
  }
  test_assert(g_failures == 0 && g_past > 1000);
}

/* ---- matrices that aren't quite what they should be ---- */

/* As three.js and gl-matrix invert: by cofactors, without knowing that the matrix is
 * affine. */
mat4d generalInverse(const mat4d &m)
{
  const double *a = m;
  mat4d out;
  double *o = out;
  const double b00 = a[0] * a[5] - a[1] * a[4], b01 = a[0] * a[6] - a[2] * a[4],
               b02 = a[0] * a[7] - a[3] * a[4], b03 = a[1] * a[6] - a[2] * a[5],
               b04 = a[1] * a[7] - a[3] * a[5], b05 = a[2] * a[7] - a[3] * a[6],
               b06 = a[8] * a[13] - a[9] * a[12], b07 = a[8] * a[14] - a[10] * a[12],
               b08 = a[8] * a[15] - a[11] * a[12], b09 = a[9] * a[14] - a[10] * a[13],
               b10 = a[9] * a[15] - a[11] * a[13], b11 = a[10] * a[15] - a[11] * a[14];
  const double det =
      1.0 / (b00 * b11 - b01 * b10 + b02 * b09 + b03 * b08 - b04 * b07 + b05 * b06);
  o[0] = (a[5] * b11 - a[6] * b10 + a[7] * b09) * det;
  o[1] = (a[2] * b10 - a[1] * b11 - a[3] * b09) * det;
  o[2] = (a[13] * b05 - a[14] * b04 + a[15] * b03) * det;
  o[3] = (a[10] * b04 - a[9] * b05 - a[11] * b03) * det;
  o[4] = (a[6] * b08 - a[4] * b11 - a[7] * b07) * det;
  o[5] = (a[0] * b11 - a[2] * b08 + a[3] * b07) * det;
  o[6] = (a[14] * b02 - a[12] * b05 - a[15] * b01) * det;
  o[7] = (a[8] * b05 - a[10] * b02 + a[11] * b01) * det;
  o[8] = (a[4] * b10 - a[5] * b08 + a[7] * b06) * det;
  o[9] = (a[1] * b08 - a[0] * b10 - a[3] * b06) * det;
  o[10] = (a[12] * b04 - a[13] * b02 + a[15] * b00) * det;
  o[11] = (a[9] * b02 - a[8] * b04 - a[11] * b00) * det;
  o[12] = (a[5] * b07 - a[4] * b09 - a[6] * b06) * det;
  o[13] = (a[0] * b09 - a[1] * b07 + a[2] * b06) * det;
  o[14] = (a[13] * b01 - a[12] * b03 - a[14] * b00) * det;
  o[15] = (a[8] * b03 - a[9] * b01 + a[10] * b00) * det;
  return out;
}

mat4d asFloat(const mat4d &m)
{
  mat4d out;
  const double *a = m;
  double *o = out;
  for (int i = 0; i < 16; i++) {
    o[i] = float(a[i]);
  }
  return out;
}

/* The length of a matrix's translation. */
double length3(const mat4d &m)
{
  const double *e = m;
  return std::hypot(e[12], e[13], e[14]);
}

void testRounded()
{
  Rand r(12);
  int inexact = 0, inexactProj = 0;
  for (int i = 0; i < 4000 * coredcc_test::testScale(); i++) {
    const int bits = int(r.next() % 64);
    Convention c{(bits & 1) != 0,
                 (bits & 2) != 0,
                 (bits & 4) != 0,
                 (bits & 8) != 0,
                 (bits & 16) != 0,
                 (bits & 32) != 0};
    c.infinite = c.infinite && !c.ortho;
    Case k = makeCase(c, r, i % 2 ? 1e3 : 0.0);
    View v = k.v;
    mat4d inverse;
    test_assert(litestl::math::tryInverse(k.v.view, inverse));
    v.view = generalInverse(inverse);
    double *m = v.view;
    const bool scaled = i % 5 == 0;
    if (i % 5 < 2) {
      /* Scaled a bit, or with rounding in the last row. */
      const double by = r.range(-9e-7, 9e-7);
      for (int col = 0; col < 4; col++) {
        for (int row = 0; scaled && row < 4; row++) {
          m[col * 4 + row] *= 1.0 + by;
        }
        if (!scaled && col < 3) {
          m[col * 4 + 3] = by * std::fabs(m[col * 4 + r.next() % 3]);
        }
      }
    }
    inexact += m[3] != 0.0 || m[7] != 0.0 || m[11] != 0.0 || m[15] != 1.0;
    v.proj = generalInverse(generalInverse(k.v.proj));
    double *p = v.proj;
    if (i % 2) {
      /* What a product of matrices, or a float one, leaves. */
      const double noise = (i % 4 == 1 ? 1e-15 : 5e-7) * r.range(-1.0, 1.0);
      for (int col = 0; col < 2; col++) {
        const double big = (std::max)(std::fabs(p[col * 4]), std::fabs(p[col * 4 + 1]));
        p[col * 4 + 2] = big * noise;
        p[col * 4 + 3] = -big * noise;
      }
      (c.ortho ? p[11] : p[15]) = noise * std::fabs(c.ortho ? p[10] : p[14]);
    }
    inexactProj += p[2] != 0.0 || p[6] != 0.0 || p[3] != 0.0 || p[7] != 0.0 ||
                   (c.ortho ? p[11] != 0.0 : p[15] != 0.0);
    if (i % 3 == 0) {
      v.view = asFloat(v.view);
      v.proj = asFloat(v.proj);
    }
    const Status s = v.validate();
    ViewMath exact, rounded;
    if (!s.ok() || !rounded.set(v).ok() || !exact.set(k.v).ok()) {
      fprintf(stderr, "  %s\n", s.message());
      expect(false, "a rounded view is refused");
      continue;
    }
    /* What it stores is exact. */
    const double *w = rounded.view().view;
    expect(w[3] == 0.0 && w[7] == 0.0 && w[11] == 0.0 && w[15] == 1.0,
           "the stored view isn't affine");
    const double3 at =
        pointAt(k, exact, r.range(-1, 1), r.range(-1, 1), r.range(1.0, 15.0));
    const ViewPoint a = exact.project(at), b = rounded.project(at);
    const double px = i % 3 == 0 ? (i % 2 ? 100.0 : 1.0) : 1e-4;
    expect(b.visible && std::fabs(a.x - b.x) < px && std::fabs(a.y - b.y) < px,
           "a rounded view projects elsewhere");
    if (i % 3 != 0) {
      const double far = length3(rounded.view().view);
      expect(std::fabs(exact.viewDistance(at) - rounded.viewDistance(at)) <
                 1e-8 * (1.0 + far),
             "a rounded view has other distances");
    }
  }
  printf("inexact inverses: %d views, %d projections\n", inexact, inexactProj);
  test_assert(inexact > 400 && inexactProj > 100 && g_failures == 0);
}

View baseView()
{
  View v;
  v.viewport[2] = 800;
  v.viewport[3] = 600;
  v.view = lookAt(double3(1, 2, 3), double3(), double3(0, 0, 1));
  v.proj = perspective(1.0, 800.0 / 600.0, 0.1, 100.0);
  return v;
}

void testValidate()
{
  test_assert(baseView().validate().ok());
  const double nan = std::numeric_limits<double>::quiet_NaN();
  struct Bad {
    const char *needle;
    std::function<void(View &)> change;
  };
  const mat4d orthoProj = orthographic(-1.0, 1.0, -1.0, 1.0, 0.1, 100.0);
  const Bad cases[] = {
      {"viewport isn't finite", [&](View &v) { v.viewport[1] = nan; }},
      {"viewport is empty", [](View &v) { v.viewport[2] = 0; }},
      {"viewport is empty", [](View &v) { v.viewport[3] = -1; }},
      {"dpiScale", [](View &v) { v.dpiScale = 0; }},
      {"dpiScale", [&](View &v) { v.dpiScale = nan; }},
      {"no such value", [](View &v) { v.setConventions(2); }},
      {"no such value", [](View &v) { v.handedness = Handedness(7); }},
      {"no such value", [](View &v) { v.up = UpAxis(-1); }},
      {"isn't finite", [&](View &v) { v.view[3][0] = nan; }},
      {"isn't finite",
       [](View &v) { v.proj[0][0] = std::numeric_limits<double>::infinity(); }},
      {"isn't affine", [](View &v) { v.view[1][3] = 0.1; }},
      {"isn't affine", [](View &v) { v.view[3][3] = 2; }},
      {"isn't affine", [](View &v) { v.view[0][3] = 1e-5; }},
      {"isn't affine", [](View &v) { v.view[2][3] = -1e-5; }},
      {"isn't affine", [](View &v) { v.view[3][3] = 1.00001; }},
      {"oblique", [](View &v) { v.proj[0][2] = 1e-4; }},
      {"oblique", [](View &v) { v.proj[1][2] = -1e-4; }},
      {"oblique", [](View &v) { v.proj[0][3] = -1e-4; }},
      {"ortho is missing", [](View &v) { v.proj[3][3] = 1e-4; }},
      {"isn't orthographic",
       [&](View &v) {
         v.proj = orthoProj;
         v.ortho = true;
         v.proj[2][3] = 1e-4;
       }},
      {"isn't invertible", [](View &v) { v.view[0] = v.view[1]; }},
      {"isn't invertible", [](View &v) { v.proj[1][1] = 0; }},
      {"oblique", [](View &v) { v.proj[0][2] = 0.1; }},
      {"oblique", [](View &v) { v.proj[1][3] = 0.1; }},
      {"ortho is missing", [&](View &v) { v.proj = orthoProj; }},
      {"isn't orthographic", [](View &v) { v.ortho = true; }},
      {"isn't orthographic",
       [&](View &v) {
         v.ortho = true;
         const double *d = orthoProj;
         for (int k = 0; k < 16; k++) {
           static_cast<double *>(v.proj)[k] = -d[k];
         }
       }},
      /* Depths fall from infinity to 2: there is no near plane. */
      {"near plane isn't in front",
       [](View &v) {
         v.reversedZ = true;
         v.proj[2][2] = -2.0;
         v.proj[3][2] = 0.1;
       }},
      {"no infinite far",
       [&](View &v) {
         v.proj = orthoProj;
         v.ortho = v.infiniteFar = true;
       }},
      {"reversedZ, but", [](View &v) { v.reversedZ = true; }},
      {"reversedZ is missing",
       [](View &v) {
         v.proj = perspective(1.0, 1.0, 0.1, 100.0, DepthRange::ZeroToOne, true);
       }},
      {"infiniteFar, but", [](View &v) { v.infiniteFar = true; }},
      /* [-1, 1] and reversed ends at -1, which the matrix never reaches from 0's side. */
      {"depth range is wrong",
       [](View &v) {
         v.proj = perspective(1.0, 1.0, 0.1, 100.0, DepthRange::ZeroToOne, true);
         v.depth = DepthRange::MinusOneToOne;
         v.reversedZ = true;
       }},
      {"contradict handedness", [](View &v) { v.handedness = Handedness::Left; }},
      {"contradict handedness", [](View &v) { v.proj[0][0] = -v.proj[0][0]; }},
      {"contradict handedness", [](View &v) { v.view = diagonal(1, -1, 1) * v.view; }},
      {"contradict handedness",
       [&](View &v) {
         v.proj = orthoProj;
         v.ortho = v.reversedZ = true;
       }},
  };
  int i = 0;
  for (const Bad &b : cases) {
    View v = baseView();
    b.change(v);
    const Status s = v.validate();
    if (s.code() != StatusCode::InvalidArgument || !strstr(s.message(), b.needle)) {
      fprintf(stderr, "case %d: '%s' instead of '%s'\n", i, s.message(), b.needle);
      test_assert(false);
    }
    /* set() keeps the old view. */
    ViewMath m;
    test_assert(m.set(baseView()).ok());
    const ViewPoint before = m.project(double3());
    test_assert(!m.set(v).ok() && m.valid() && m.project(double3()).x == before.x);
    i++;
  }
  /* A distant far plane in float precision looks infinite; both flags are accepted. */
  View v = baseView();
  v.proj = perspective(1.0, 1.0, 0.01, 1e8);
  double *d = v.proj;
  for (int k = 0; k < 16; k++) {
    d[k] = float(d[k]);
  }
  test_assert(v.validate().ok());
  v.infiniteFar = true;
  test_assert(v.validate().ok());
  v.proj[2][2] = -(1.0 - 6e-8); /* rounded the other way */
  test_assert(v.validate().ok());
  v.infiniteFar = false;
  test_assert(v.validate().ok());
  /* And there is no far plane then: it would be noise. */
  ViewMath m;
  test_assert(m.set(v).ok() && m.farPlane()[3] == 1.0 && m.farPlane()[2] == 0.0);
  test_assert(m.project(double3(1, 2, 3) * -1e7).visible);
}

void testUnset()
{
  ViewMath m;
  test_assert(!m.valid() && m.project(double3(1, 2, 3)).behind);
  test_assert(!m.unproject(0, 0, 0.5).ok() && !m.rayFromPixel(0, 0).valid());
  const ViewRegion g = m.regionFromPixel(0, 0, 5);
  test_assert(m.regionFromRect(0, 0, 9, 9).planes[0].w < 0.0);
  TriangleSource<double> triangles;
  triangles.append(double3(), double3(1, 0, 0), double3(0, 1, 0));
  GeometryBvh<TriangleSource<double>> bvh;
  int found = 0;
  test_assert(bvh.build(triangles).ok());
  test_assert(bvh.region(g.planes, [&](int) { return ++found > 0; }).ok() && found == 0);
}

void testScripts()
{
  const auto *view = View::defineBindings();
  const auto *math = ViewMath::defineBindings();
  test_assert(strcmp(view->name.c_str(), "coredcc::View") == 0 &&
              view == View::defineBindings());
  test_assert(math == ViewMath::defineBindings());
  test_assert(view->constructors.size() == 2 &&
              ViewPoint::defineBindings()->constructors.size() == 2);
  View v;
  v.putViewport(2, 640);
  v.putViewport(4, 1);
  v.putViewport(-1, 1);
  test_assert(v.depthRange() == 0);
  v.setConventions(1);
  test_assert(v.depthRange() == 1 && v.depth == DepthRange::MinusOneToOne);
  ViewMath m;
  for (int i : {-1, 4}) {
    test_assert(m.nearPlaneAt(i) == 0.0 && m.farPlaneAt(i) == 0.0 &&
                m.pixelRowAt(0, i) == 0.0);
    test_assert(ViewRegion().planeAt(0, i) == 0.0);
  }
  test_assert(m.pixelRowAt(3, 0) == 0.0 && m.pixelRowAt(-1, 0) == 0.0);
  test_assert(ViewRegion().planeAt(6, 3) == 0.0 && ViewRegion().planeAt(-1, 3) == 0.0);
  test_assert(ViewRegion().planeCount() == 6);
  test_assert(ViewRegion::defineBindings() == ViewRegion::defineBindings());
  test_assert(v.viewportAt(2) == 640 && v.viewportAt(0) == 0 && v.viewportAt(4) == 0 &&
              v.viewportAt(-1) == 0);
}

} // namespace

int main()
{
  testConventions();
  testRounded();
  testValidate();
  testUnset();
  testScripts();
  return test_end();
}
