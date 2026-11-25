#pragma once

#include "litestl/math/vector.h"

#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>

namespace litestl::binding::types {
template <typename T> struct Struct;
}

namespace coredcc {

using litestl::math::double3;

namespace bvh {

/* Tested on the exponent bits: a host built with fast-math may assume std::isfinite. */
inline bool finite(double v)
{
  return (std::bit_cast<uint64_t>(v) & 0x7FF0000000000000ull) != 0x7FF0000000000000ull;
}
inline bool finite(float v)
{
  return (std::bit_cast<uint32_t>(v) & 0x7F800000u) != 0x7F800000u;
}

/** The largest S not above `v`. Values below S's range give -infinity. */
template <class S> S roundDown(double v)
{
  if constexpr (sizeof(S) == sizeof(double)) {
    return v;
  } else {
    if (v > double((std::numeric_limits<S>::max)())) {
      return (std::numeric_limits<S>::max)();
    }
    if (v < double(std::numeric_limits<S>::lowest())) {
      return -std::numeric_limits<S>::infinity();
    }
    const S f = S(v);
    return double(f) > v ? std::nextafter(f, -std::numeric_limits<S>::infinity()) : f;
  }
}
/** The smallest S not below `v`. Values above S's range give infinity. */
template <class S> S roundUp(double v)
{
  return -roundDown<S>(-v);
}

/** Bounds, rounded to S, of the exact value of a rounded double sum or difference. */
template <class S> S sumDown(double rounded)
{
  return roundDown<S>(std::nextafter(rounded, -std::numeric_limits<double>::infinity()));
}
template <class S> S sumUp(double rounded)
{
  return roundUp<S>(std::nextafter(rounded, std::numeric_limits<double>::infinity()));
}

} // namespace bvh

/** An axis-aligned box. Empty by default, so it can be grown with include(). */
template <class S> struct Box3 {
  using Scalar = S;

  S min[3];
  S max[3];

  constexpr Box3()
      : min{(std::numeric_limits<S>::max)(), (std::numeric_limits<S>::max)(),
            (std::numeric_limits<S>::max)()},
        max{std::numeric_limits<S>::lowest(), std::numeric_limits<S>::lowest(),
            std::numeric_limits<S>::lowest()}
  {
  }

  /** Whether the box contains no point: some min is above its max. */
  constexpr bool empty() const
  {
    return min[0] > max[0] || min[1] > max[1] || min[2] > max[2];
  }
  /** Whether every bound is finite. An empty box's are. */
  bool allFinite() const
  {
    return bvh::finite(min[0]) && bvh::finite(min[1]) && bvh::finite(min[2]) &&
           bvh::finite(max[0]) && bvh::finite(max[1]) && bvh::finite(max[2]);
  }

  constexpr void include(S x, S y, S z)
  {
    const S p[3] = {x, y, z};
    for (int i = 0; i < 3; i++) {
      min[i] = p[i] < min[i] ? p[i] : min[i];
      max[i] = p[i] > max[i] ? p[i] : max[i];
    }
  }
  constexpr void include(const Box3 &b)
  {
    for (int i = 0; i < 3; i++) {
      min[i] = b.min[i] < min[i] ? b.min[i] : min[i];
      max[i] = b.max[i] > max[i] ? b.max[i] : max[i];
    }
  }
  constexpr bool contains(const double3 &p) const
  {
    return p[0] >= double(min[0]) && p[0] <= double(max[0]) && p[1] >= double(min[1]) &&
           p[1] <= double(max[1]) && p[2] >= double(min[2]) && p[2] <= double(max[2]);
  }

  /** The squared distance from `p` to the box; 0 inside. */
  double distanceSq(const double3 &p) const
  {
    double sum = 0.0;
    for (int i = 0; i < 3; i++) {
      const double lo = double(min[i]) - p[i], hi = p[i] - double(max[i]);
      const double d = lo > hi ? lo : hi;
      sum += d > 0.0 ? d * d : 0.0;
    }
    return sum;
  }

  /* For scripts: one axis at a time. An axis out of range reads 0 and writes nothing. */
  double minAt(int32_t axis) const
  {
    return axis >= 0 && axis < 3 ? double(min[axis]) : 0.0;
  }
  double maxAt(int32_t axis) const
  {
    return axis >= 0 && axis < 3 ? double(max[axis]) : 0.0;
  }
  void put(int32_t axis, double lo, double hi)
  {
    if (axis >= 0 && axis < 3) {
      min[axis] = bvh::roundDown<S>(lo);
      max[axis] = bvh::roundUp<S>(hi);
    }
  }
  /** Only Box3<double> is bound. */
  static litestl::binding::types::Struct<Box3> *defineBindings();
};

using Box3f = Box3<float>;
using Box3d = Box3<double>;

template <> litestl::binding::types::Struct<Box3d> *Box3d::defineBindings();

/**
 * A box around the image of `b` under the affine column-major matrix `m` (16 values),
 * rounded outward. An empty box stays empty. Overflow gives bounds that aren't finite.
 */
inline Box3d transformBox(const Box3d &b, const double *m)
{
  Box3d r;
  if (b.empty()) {
    return r;
  }
  for (int row = 0; row < 3; row++) {
    double lo = m[12 + row], hi = lo;
    double size = std::fabs(lo);
    for (int c = 0; c < 3; c++) {
      const double x = m[c * 4 + row] * b.min[c], y = m[c * 4 + row] * b.max[c];
      lo += x < y ? x : y;
      hi += x < y ? y : x;
      const double ax = std::fabs(x), ay = std::fabs(y);
      size += ax > ay ? ax : ay;
    }
    /* Three products and three sums: their error is under 4 eps of the terms' size. */
    const double pad = size * (8.0 * std::numeric_limits<double>::epsilon()) +
                       (std::numeric_limits<double>::min)();
    r.min[row] = lo - pad;
    r.max[row] = hi + pad;
  }
  return r;
}

} // namespace coredcc
