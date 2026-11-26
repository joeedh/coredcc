#pragma once

/* Views of every convention, at scene offsets, for the tool tests: a copy of P3's
 * generator in tests/draw/test_view.cc (kept there unchanged). */

#include "coredcc/draw/view.h"

#include "litestl/math/transform.h"

#include <cmath>
#include <cstdint>
#include <limits>

namespace view_cases {

using namespace coredcc;
using litestl::math::lookAt;
using litestl::math::orthographic;
using litestl::math::perspective;

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

inline mat4d diagonal(double x, double y, double z)
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

inline Case makeCase(const Convention &c, Rand &r, double offset)
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
  v.depthRange = c.minusOne ? DepthRange::MinusOneToOne : DepthRange::ZeroToOne;
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
      c.ortho
          ? orthographic(-h * aspect, h * aspect, -h, h, n, f, v.depthRange, c.reversed)
          : perspective(r.range(0.2, 2.0), aspect, n, f, v.depthRange, c.reversed);
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
inline double3
pointAt(const Case &k, const ViewMath &m, double x, double y, double distance)
{
  const double *vp = k.v.viewport;
  Ray ray =
      m.rayFromPixel(vp[0] + (0.5 + 0.5 * x) * vp[2], vp[1] + (0.5 - 0.5 * y) * vp[3]);
  /* Along the ray to a distance along the view direction. */
  const double step = m.viewDistance(ray.origin + ray.dir) - m.viewDistance(ray.origin);
  return ray.origin + ray.dir * (distance / step);
}

/* Every convention but ortho with an infinite far plane, as bits. */
inline bool conventionOf(int bits, Convention &c)
{
  c = Convention{(bits & 1) != 0,
                 (bits & 2) != 0,
                 (bits & 4) != 0,
                 (bits & 8) != 0,
                 (bits & 16) != 0,
                 (bits & 32) != 0};
  return !(c.ortho && c.infinite);
}

} // namespace view_cases
