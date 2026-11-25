#pragma once

#include "coredcc/base/status.h"
#include "coredcc/bvh/geometry_bvh.h"
#include "coredcc/bvh/ray.h"

#include "litestl/math/transform.h"

#include <cstdint>

namespace litestl::binding::types {
template <typename T> struct Struct;
}

namespace coredcc {

using litestl::math::double4;
using litestl::math::mat4d;

/** The range of depths in normalized device coordinates. */
using DepthRange = litestl::math::ClipDepth;
enum class Handedness : int32_t { Right, Left };
/** The world's up axis, for navigation and for new objects. Nothing checks it. */
enum class UpAxis : int32_t { Y, Z };

/**
 * A viewport and its camera, as the host gives them. Matrices are column-major and apply to
 * column vectors. Normalized device coordinates have x to the right and y up, so a host whose
 * projection flips y (Vulkan) gives the unflipped one.
 */
struct View {
  /** x, y, width and height in device pixels, y down. */
  double viewport[4] = {0, 0, 0, 0};
  /** World to view space, affine. */
  mat4d view;
  /** View to clip space. The depth of a point may depend on its view z only. */
  mat4d proj;
  DepthRange depth = DepthRange::ZeroToOne;
  /** The near plane has the largest depth, 1. */
  bool reversedZ = false;
  /** The far plane is at infinity (perspective only). */
  bool infiniteFar = false;
  bool ortho = false;
  /** Of the world. */
  Handedness handedness = Handedness::Right;
  UpAxis up = UpAxis::Z;
  /** Device pixels per logical pixel. */
  double dpiScale = 1.0;

  /**
   * InvalidArgument when something isn't finite or invertible, or when a flag contradicts the
   * matrices. A depth range can't always be told from a matrix: a projection to [0, 1] is also
   * one to [-1, 1] with another near plane.
   */
  Status validate() const;

  /* For scripts. */
  double viewportAt(int32_t i) const;
  void putViewport(int32_t i, double value);
  void setConventions(int32_t depthRange, int32_t hand, int32_t upAxis);
  static litestl::binding::types::Struct<View> *defineBindings();
};

/** Where a world point is in a view. */
struct ViewPoint {
  /** Device pixels, y down. Meaningless when `behind`. */
  double x = 0.0, y = 0.0;
  /** In normalized device coordinates. Meaningless when `behind`. */
  double depth = 0.0;
  /** ViewMath::viewDistance. */
  double distance = 0.0;
  /** Perspective: on the eye's plane or behind it. */
  bool behind = false;
  /** On the near plane or past it. A point that isn't can't be seen. */
  bool visible = false;

  static litestl::binding::types::Struct<ViewPoint> *defineBindings();
};

/** The half-spaces of a small square around a pixel: four sides and the near plane. */
struct ViewRegion {
  static constexpr int kPlanes = 5;
  BvhPlane planes[kPlanes];
};

/**
 * Conversions between the world and the pixels of a valid View. Pixels are device pixels, y
 * down; radii and sizes are in logical pixels, which are `dpiScale` device pixels.
 */
class ViewMath {
 public:
  ViewMath() = default;

  /** Takes a view. On an error the old one is kept. */
  Status set(const View &view);
  bool valid() const
  {
    return valid_;
  }
  const View &view() const
  {
    return view_;
  }

  ViewPoint project(const double3 &p) const;
  /** The world point at a pixel and a depth (normalized device coordinates).
   * FailedPrecondition at infinity (the far plane when it is infinite). */
  Result<double3> unproject(double x, double y, double depth) const;
  /**
   * Through a pixel, starting on the near plane, in the direction of increasing distance. The
   * direction has unit length, so t is a world distance. In an orthographic view the near plane
   * may be behind the eye.
   */
  Ray rayFromPixel(double x, double y) const;
  /**
   * Planes around every point that projects to within `radius` logical pixels of a pixel, in x
   * and in y, and is past the near plane. The radius is at least half a device pixel. Each
   * plane is moved out by 1e-12 of the size of its terms, so rounding doesn't exclude a point.
   */
  ViewRegion regionFromPixel(double x, double y, double radius) const;
  /** The world length of a logical pixel at `p`, along the image. 0 when `p` is behind. */
  double pixelSizeAt(const double3 &p) const;
  /** The distance along the view direction from the near plane, in world units: negative
   * before it. */
  double viewDistance(const double3 &p) const;

  /** World to device pixels, as three rows: a point is at (X / W, Y / W), where W > 0. */
  const double4 *pixelRows() const
  {
    return pixelRows_;
  }
  /** The near plane in world space; visible points have `n.p + w >= 0`. Not normalized. */
  const double4 &nearPlane() const
  {
    return nearPlane_;
  }

  /* For scripts. */
  Status unprojectTo(double x, double y, double depth, double3 &out) const;
  double3 rayOrigin(double x, double y) const;
  double3 rayDirection(double x, double y) const;
  static litestl::binding::types::Struct<ViewMath> *defineBindings();

 private:
  double4 unprojectToView(double x, double y, double depth) const;

  View view_;
  mat4d invView_, invProj_;
  /** Clip-space rows, in view space. */
  double4 clip_[4];
  /** The near plane in view space. */
  double4 nearView_;
  double4 nearPlane_;
  double4 pixelRows_[3];
  /** 1 / the length of nearPlane_'s normal. */
  double nearScale_ = 0.0;
  /** The depth of the near plane, and one between it and the far plane. */
  double nearDepth_ = 0.0, midDepth_ = 0.0;
  bool valid_ = false;
};

} // namespace coredcc
