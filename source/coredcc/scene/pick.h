#pragma once

/** \file
 * Picking: what is at or near a pixel.
 */

#include "coredcc/base/status.h"
#include "coredcc/draw/view.h"
#include "coredcc/lib/library.h"
#include "coredcc/scene/outline.h"

#include "litestl/util/vector.h"

#include <cstdint>
#include <functional>
#include <span>

namespace litestl::binding::types {
template <typename T> struct Struct;
}

namespace coredcc {

class Context;

/**
 * What a Spatial is asked for, in its object's space. To find candidates:
 *
 * - The half-spaces `planes()`. Shapes outside any of them are too far from the pixel, or not
 *   visible. It is always possible to use them.
 * - The ray through the pixel, if there is one. Whatever it hits first covers the pixel, but it
 *   misses what is only near it.
 */
class PickQuery {
 public:
  PickQuery() = default;
  /** `region` is the view's around the pixel. `radius` is in logical pixels. */
  PickQuery(const ViewMath &view,
            const ViewRegion &region,
            double x,
            double y,
            double radius,
            const mat4d &world);

  std::span<const BvhPlane> planes() const
  {
    return planes_;
  }
  /** False when the object's matrix has no inverse (a scale of 0). */
  bool hasRay() const
  {
    return hasRay_;
  }
  /** From the near plane. Its t is the distance in the world. */
  const Ray &ray() const
  {
    return ray_;
  }
  /** In device pixels. */
  double radiusPixels() const
  {
    return radius_;
  }

  /* For scripts. */
  int32_t planeCount() const
  {
    return ViewRegion::kPlanes;
  }
  double3 planeNormal(int32_t i) const;
  /** The plane's `w`: the half-space is `normal.p + w >= 0`. */
  double planeOffset(int32_t i) const;
  double3 rayOrigin() const
  {
    return ray_.origin;
  }
  double3 rayDirection() const
  {
    return ray_.dir;
  }
  static litestl::binding::types::Struct<PickQuery> *defineBindings();

 private:
  friend class PickSink;
  BvhPlane planes_[ViewRegion::kPlanes];
  Ray ray_;
  bool hasRay_ = false;
  detail::ScreenMap map_;
  mat4d world_;
  double radius_ = 0.0;
  double dpiScale_ = 1.0;
};

struct PickHit {
  BlockHandle object;
  /** An ElementDomain, and the element as the object reported it. -1 for its bounds. */
  int32_t domain = -1;
  int32_t element = -1;
  /** Logical pixels from the cursor to what is seen of the shape. 0 if it covers the cursor. */
  double distance = 0.0;
  /** Of `point` (ViewMath::viewDistance). */
  double depth = 0.0;
  /** In the world: where the ray through the cursor hits the shape, or the shape's point that is
   * nearest to the cursor on the screen. */
  double3 point;

  /** Whether the shape covers the cursor. */
  bool covers() const
  {
    return distance == 0.0;
  }
};

/**
 * Takes the shapes an object has near a pixel, and keeps the best: one that covers the pixel
 * before others, then the nearest on the screen, then the frontmost. Shapes that aren't near may
 * be given too.
 *
 * Coordinates and radii are in the object's space. Shapes with values that aren't finite, or a
 * negative radius, are ignored.
 */
class PickSink {
 public:
  explicit PickSink(const PickQuery &query) : query_(query) {}
  PickSink(const PickSink &) = delete;
  PickSink &operator=(const PickSink &) = delete;

  /** What the shapes after this belong to: an ElementDomain, and an element. */
  void element(int32_t domain, int32_t element)
  {
    domain_ = domain;
    element_ = element;
  }
  void point(const double3 &p);
  void segment(const double3 &a, const double3 &b);
  void triangle(const double3 &a, const double3 &b, const double3 &c);
  /** A solid. */
  void sphere(const double3 &center, double radius);
  /** A solid: the points within `radius` of a segment. */
  void capsule(const double3 &a, const double3 &b, double radius);
  /** The surface of a box. */
  void box(const Box3d &box);

  bool any() const
  {
    return any_;
  }
  /** The best, without `object`, if there is any. */
  const PickHit &best() const
  {
    return best_;
  }

  static litestl::binding::types::Struct<PickSink> *defineBindings();

 private:
  void corners(const double3 *p, int count);
  void take(const detail::ScreenPoint &p);

  const PickQuery &query_;
  int32_t domain_ = -1;
  int32_t element_ = -1;
  bool any_ = false;
  PickHit best_;
};

/** Which objects can be picked. */
struct PickFilter {
  /** Also those with ObjectFlags::Hidden. */
  bool hidden = false;
  /** Also those with ObjectFlags::Unselectable. */
  bool unselectable = false;
  /** If set, only those it accepts. Called while nothing can be changed, as evaluateScene. */
  std::function<bool(BlockHandle)> accept;
};

/**
 * The objects within `radius` logical pixels of a device pixel: the best hit of each. Hits that
 * cover the pixel come first, by depth; then the others by distance, then depth; then by the
 * scene's order.
 *
 * - The answer is for the last evaluateScene, which is what was drawn: its matrices and bounds,
 *   and no object that came later. FailedPrecondition before the first one.
 * - Each candidate's Spatial is asked now (its data block's, or its own). An object that isn't
 *   Live any more, or has none, is left out. Without a `pick` slot, its local bounds' surface
 *   answers. An object with empty bounds isn't found.
 * - Objects that are only parents of members, or whose channels don't fit, can't be picked.
 * - While it runs, setValue, commands and evaluateScene are Busy, and it is too.
 * - The first exception of a foreign capability is returned, with the hits of the other objects.
 *   With another error, there are none.
 */
Status pickScene(Context &ctx,
                 BlockHandle scene,
                 const ViewMath &view,
                 double x,
                 double y,
                 double radius,
                 const PickFilter &filter,
                 litestl::util::Vector<PickHit> &hits);

} // namespace coredcc
