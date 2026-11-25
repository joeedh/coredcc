#pragma once

/* Shapes on the screen: how far a cursor is from what they cover. Internal. */

#include "coredcc/bvh/box.h"

#include "litestl/math/vector.h"

namespace coredcc::detail {

using litestl::math::double4;

/**
 * From an object's space to the screen around a cursor. The point q, as (q, 1), is at
 * (x.q / w.q, y.q / w.q) device pixels from the cursor. It is visible if front.q >= 0, and then
 * w.q > 0, and back.q >= 0. These are its distances from the near and to the far plane, in world
 * units, so their normals are opposite; back is (0, 0, 0, 1) when there is no far plane.
 */
struct ScreenMap {
  double4 x, y, w, front, back;
};

inline double dotPoint(const double4 &row, const double3 &q)
{
  return row[0] * q[0] + row[1] * q[1] + row[2] * q[2] + row[3];
}

/** The point of a shape that is nearest to the cursor on the screen. */
struct ScreenPoint {
  /** In device pixels. 0 when the shape covers the cursor. */
  double distance = 0.0;
  /** In the object's space. */
  double3 point;
};

/** Distances up to this, in device pixels, are 0: rounding is smaller, also for solids that are
 * thinner on the screen. */
constexpr double kCoveredPixels = 1e-4;

/**
 * Of the visible part of a solid capsule (a sphere when a == b), under any affine matrix. The
 * distance is at most 0.01 pixels too large, and 0 if the cursor is covered; the point is then one
 * of those there, not the frontmost. False when nothing of the capsule is visible, or all of it is
 * farther than `limit`.
 */
bool closestOnCapsule(const ScreenMap &map,
                      const double3 &a,
                      const double3 &b,
                      double r,
                      double limit,
                      ScreenPoint &out);

/** Of the visible part of a point, a segment or a triangle: 1 to 3 corners. False when nothing of
 * it is visible. */
bool closestOnCorners(const ScreenMap &map,
                      const double3 *corners,
                      int count,
                      ScreenPoint &out);

} // namespace coredcc::detail
