#pragma once

/* Views, matrices, and a proof of distances on the screen that shares nothing with the search. */

#include "scene_fixture.h"

#include "coredcc/draw/view.h"
#include "coredcc/scene/outline.h"

#include <algorithm>
#include <vector>

namespace scene_test {

using detail::dotPoint;
using detail::ScreenMap;
using detail::ScreenPoint;
using litestl::math::planeToLocal;

struct ViewCase {
  bool ortho = false;
  /** Orthographic: may be negative, so behind the eye. */
  double zNear = 0.1;
  double zFar = 1000.0;
  double3 eye = double3(0.0, 0.0, 10.0);
  double3 target = double3(0.0, 0.0, 0.0);
  double3 up = double3(0.0, 1.0, 0.0);
  bool minusOne = false;
  bool reversed = false;
  bool infinite = false;
  double dpiScale = 1.0;
};

/** 1920 by 1080 pixels, at (40, 20). The vertical field of view is 1 (perspective), the height 6. */
inline ViewMath viewOf(const ViewCase &c)
{
  View v;
  v.viewport[0] = 40.0;
  v.viewport[1] = 20.0;
  v.viewport[2] = 1920.0;
  v.viewport[3] = 1080.0;
  v.dpiScale = c.dpiScale;
  v.depth = c.minusOne ? DepthRange::MinusOneToOne : DepthRange::ZeroToOne;
  v.reversedZ = c.reversed;
  v.ortho = c.ortho;
  v.infiniteFar = c.infinite && !c.ortho;
  v.view = litestl::math::lookAt(c.eye, c.target, c.up);
  const double aspect = 16.0 / 9.0, far = c.zFar;
  v.proj = c.ortho ? litestl::math::orthographic(
                         -3.0 * aspect, 3.0 * aspect, -3.0, 3.0, c.zNear, far, v.depth, c.reversed) :
                     litestl::math::perspective(1.0,
                                                aspect,
                                                c.zNear,
                                                v.infiniteFar ?
                                                    std::numeric_limits<double>::infinity() :
                                                    far,
                                                v.depth,
                                                c.reversed);
  ViewMath m;
  const Status s = m.set(v);
  if (!s.ok()) {
    fprintf(stderr, "%s\n", s.message());
    abort();
  }
  return m;
}

inline ViewCase randomView(Rand &r)
{
  const double nears[] = {0.001, 0.01, 0.1, 1.0};
  ViewCase c;
  c.ortho = r.below(3) == 0;
  c.zNear = c.ortho ? r.range(-8.0, 5.0) : nears[r.below(4)];
  c.zFar = r.coin() ? 1000.0 : c.zNear + r.range(0.5, 8.0);
  c.eye = r.vec(4.0);
  c.target = c.eye + r.vec(1.0) + double3(0.0, 0.0, 0.1);
  c.up = r.vec(1.0);
  c.minusOne = r.coin();
  c.reversed = r.coin();
  c.infinite = r.coin();
  c.dpiScale = r.coin() ? 1.0 : 2.0;
  return c;
}

inline mat4d identity()
{
  return solve::affine(Linear::identity(), double3());
}

/** A rotation and scale, a shear, a reflection, a matrix with one, two or three zero columns, or
 * one with two that almost are. */
inline Linear randomLinear(Rand &r, int kind)
{
  Linear m;
  const quatd q = r.rotation();
  const double3 scale = r.scale(4.0, false);
  for (int c = 0; c < 3; c++) {
    double3 axis;
    axis[c] = kind == 2 && c == 0 ? -scale[c] : scale[c];
    m.col[c] = q.rotate(axis);
    if (kind == 1) {
      m.col[c] = r.vec(1.5);
    }
  }
  for (int c = 0; c < kind - 2 && kind < 6; c++) {
    m.col[(c + int(r.below(3))) % 3] = double3();
  }
  if (kind == 6) {
    m.col[1] = m.col[1] * r.range(1e-11, 1e-9);
    m.col[2] = m.col[2] * r.range(1e-11, 1e-9);
  }
  return m;
}
constexpr int kLinearKinds = 7;

inline ScreenMap mapOf(const ViewMath &v, const mat4d &world, double x, double y)
{
  const double4 *p = v.pixelRows();
  const double4 &front = v.nearPlane();
  ScreenMap m;
  m.x = planeToLocal(world, p[0] - p[2] * x);
  m.y = planeToLocal(world, p[1] - p[2] * y);
  m.w = planeToLocal(world, p[2]);
  m.front = planeToLocal(world, front * (1.0 / std::hypot(front[0], front[1], front[2])));
  const double4 &back = v.farPlane();
  const double length = std::hypot(back[0], back[1], back[2]);
  m.back = length > 0.0 ? planeToLocal(world, back * (1.0 / length)) : double4(0, 0, 0, 1);
  return m;
}

inline double normalLength(const double4 &row)
{
  return std::hypot(row[0], row[1], row[2]);
}

/**
 * How certainly all of a solid's visible part (the points within `r` of the hull of `ends`) is at least
 * `distance` from the cursor, beyond the line across `n`: whether `L = n.(x, y) - distance w` is
 * never negative there. Proven if the answer isn't negative.
 *
 * For any k >= 0, the smallest `L - k front` of the whole solid is a lower bound, and so is that of
 * `L - k back`. The best one is exact: `front + back` is a positive constant, so of both factors only
 * what is left of the larger one counts. Each is concave in k, so a search finds it.
 */
inline double provenDistance(const ScreenMap &map,
                           const std::vector<double3> &ends,
                           double r,
                           double nx,
                           double ny,
                           double distance)
{
  const double4 line = map.x * nx + map.y * ny - map.w * distance;
  double terms = 0.0;
  for (const double3 &q : ends) {
    for (int c = 0; c < 4; c++) {
      const double k = c < 3 ? q[c] : 1.0;
      terms = std::fmax(terms, std::fabs(map.x[c] * k) + std::fabs(map.y[c] * k) +
                                   std::fabs(map.w[c] * k) * distance);
    }
  }
  terms += r * (normalLength(map.x) + normalLength(map.y) + normalLength(map.w) * distance);
  const double tolerance = 1e-10 * terms + 1e-300;
  const auto bound = [&](const double4 &plane, double k) {
    const double4 row = line - plane * k;
    double low = std::numeric_limits<double>::infinity();
    for (const double3 &q : ends) {
      low = std::fmin(low, dotPoint(row, q));
    }
    return low - r * normalLength(row);
  };
  double best = bound(map.front, 0.0);
  for (double scale = 1e-9; scale < 1e15 && best < -tolerance; scale *= 1e3) {
    for (const double4 *plane : {&map.front, &map.back}) {
      double lo = 0.0, hi = scale;
      for (int i = 0; i < 160 && best < -tolerance; i++) {
        const double a = lo + (hi - lo) / 3.0, b = hi - (hi - lo) / 3.0;
        const double va = bound(*plane, a), vb = bound(*plane, b);
        best = std::fmax(best, std::fmax(va, vb));
        (va < vb ? lo : hi) = va < vb ? a : b;
      }
    }
  }
  return best + tolerance;
}

/** Whether it is proven for a line across `n`, or almost: the edge of a shape may be a million
 * pixels long. */
inline bool provenNear(const ScreenMap &map,
                       const std::vector<double3> &ends,
                       double r,
                       double nx,
                       double ny,
                       double distance)
{
  if (provenDistance(map, ends, r, nx, ny, distance) >= 0.0) {
    return true;
  }
  double lo = -1e-5, hi = 1e-5;
  for (int i = 0; i < 100; i++) {
    const double a = lo + (hi - lo) / 3.0, b = hi - (hi - lo) / 3.0;
    const double va = provenDistance(map, ends, r, nx - ny * a, ny + nx * a, distance);
    const double vb = provenDistance(map, ends, r, nx - ny * b, ny + nx * b, distance);
    if (va >= 0.0 || vb >= 0.0) {
      return true;
    }
    (va < vb ? lo : hi) = va < vb ? a : b;
  }
  return false;
}

/** Whether a point is where it should be: visible, and at `distance` from the cursor. */
inline bool atDistance(const ScreenMap &map, const ScreenPoint &p, double &nx, double &ny)
{
  const double w = dotPoint(map.w, p.point);
  nx = dotPoint(map.x, p.point) / w;
  ny = dotPoint(map.y, p.point) / w;
  const double length = std::hypot(nx, ny);
  const bool ok = w > 0.0 && dotPoint(map.front, p.point) >= -1e-9 &&
                  dotPoint(map.back, p.point) >= -1e-9 &&
                  std::fabs(length - p.distance) <= 1.1e-4 + 1e-9 * length;
  if (!ok) {
    fprintf(stderr,
            "at %g, not %g; front %g\n",
            length,
            p.distance,
            dotPoint(map.front, p.point));
  }
  nx /= length;
  ny /= length;
  return ok;
}

} // namespace scene_test
