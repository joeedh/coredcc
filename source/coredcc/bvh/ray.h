#pragma once

#include "coredcc/bvh/box.h"

#include <cmath>
#include <limits>

namespace coredcc {

/** The points `origin + t * dir` with `tMin <= t <= tMax`. `dir` needn't be unit length;
 * t is in units of it. */
struct Ray {
  double3 origin;
  double3 dir;
  double tMin = 0.0;
  double tMax = (std::numeric_limits<double>::max)();

  /** Finite, with a direction. Other rays hit nothing. */
  bool valid() const
  {
    for (int i = 0; i < 3; i++) {
      if (!bvh::finite(origin[i]) || !bvh::finite(dir[i])) {
        return false;
      }
    }
    return (dir[0] != 0.0 || dir[1] != 0.0 || dir[2] != 0.0) && tMin <= tMax;
  }
};

struct RayHit {
  /** The source's primitive, or -1 for no hit. */
  int primitive = -1;
  double t = 0.0;
  /** On a triangle abc, the hit is `a + u (b - a) + v (c - a)`; 0 on other shapes. */
  double u = 0.0, v = 0.0;
  /** The ray starts inside the primitive (a solid one: not a triangle); t is tMin. */
  bool inside = false;

  explicit operator bool() const
  {
    return primitive >= 0;
  }
};

namespace bvh {

/**
 * A ray prepared for the kernels. Its direction is scaled by a power of two so that its
 * largest component is in [0.5, 1). The kernels work in that scaled t; toRayT() converts
 * to the ray's own.
 */
struct RayQuery {
  double3 origin, dir;
  double tMin, tMax;

  /** Ray space of the watertight triangle test: kz is dir's largest axis, and the shear
   * maps dir to the z axis. */
  int kx, ky, kz;
  double shearX, shearY, shearZ;

  explicit RayQuery(const Ray &ray) : origin(ray.origin), dir(ray.dir)
  {
    kz = 0;
    for (int i = 1; i < 3; i++) {
      kz = std::fabs(dir[i]) > std::fabs(dir[kz]) ? i : kz;
    }
    std::frexp(dir[kz], &exponent_);
    for (int i = 0; i < 3; i++) {
      dir[i] = std::ldexp(dir[i], -exponent_);
    }
    tMin = scaleT(ray.tMin);
    tMax = scaleT(ray.tMax);

    kx = (kz + 1) % 3;
    ky = (kx + 1) % 3;
    shearX = dir[kx] / dir[kz];
    shearY = dir[ky] / dir[kz];
    shearZ = 1.0 / dir[kz];

    for (int i = 0; i < 3; i++) {
      inv_[i] = dir[i] != 0.0 ? 1.0 / dir[i] : 0.0;
      regular_ = regular_ && dir[i] != 0.0 && finite(inv_[i]);
    }
  }

  double toRayT(double t) const
  {
    return std::ldexp(t, -exponent_);
  }
  double3 at(double t) const
  {
    return origin + dir * t;
  }

  /**
   * Whether the ray meets the box between tMin and `limit`, and where it enters it
   * (tMin, if it starts inside). Never misses a box the exact ray meets.
   */
  template <class S> bool slab(const S *lo, const S *hi, double limit, double &enter) const
  {
    double t0 = tMin, t1 = limit;
    if (regular_) {
      for (int i = 0; i < 3; i++) {
        const bool neg = inv_[i] < 0.0;
        const double a = (double(neg ? hi[i] : lo[i]) - origin[i]) * inv_[i];
        double b = (double(neg ? lo[i] : hi[i]) - origin[i]) * inv_[i];
        b += std::fabs(b) * kPad;
        t0 = a > t0 ? a : t0;
        t1 = b < t1 ? b : t1;
      }
    } else if (!slabAxes(lo, hi, t0, t1)) {
      return false;
    }
    enter = t0;
    return t0 <= t1;
  }

 private:
  /* Rounding in a slab's entry and exit is at most 2 ulp each. */
  static constexpr double kPad = 4.0 * std::numeric_limits<double>::epsilon();

  double scaleT(double t) const
  {
    const double s = std::ldexp(t, exponent_);
    const double big = (std::numeric_limits<double>::max)();
    return s > big ? big : s < -big ? -big : s;
  }

  /* For rays with a zero component, or one too small to invert. */
  template <class S> bool slabAxes(const S *lo, const S *hi, double &t0, double &t1) const
  {
    for (int i = 0; i < 3; i++) {
      if (dir[i] == 0.0) {
        if (origin[i] < double(lo[i]) || origin[i] > double(hi[i])) {
          return false;
        }
        continue;
      }
      if (!finite(inv_[i])) {
        continue; /* conservative: this axis rejects nothing */
      }
      const bool neg = inv_[i] < 0.0;
      const double a = (double(neg ? hi[i] : lo[i]) - origin[i]) * inv_[i];
      double b = (double(neg ? lo[i] : hi[i]) - origin[i]) * inv_[i];
      b += std::fabs(b) * kPad;
      t0 = a > t0 ? a : t0;
      t1 = b < t1 ? b : t1;
    }
    return true;
  }

  int exponent_ = 0;
  bool regular_ = true;
  double inv_[3];
};

} // namespace bvh
} // namespace coredcc
