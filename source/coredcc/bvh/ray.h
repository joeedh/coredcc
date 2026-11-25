#pragma once

#include "coredcc/bvh/box.h"

#include <cmath>
#include <limits>

namespace litestl::binding::types {
template <typename T> struct Struct;
}

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
  /** The ray hits a triangle's back: it goes the way of `(b - a) x (c - a)`. */
  bool backFacing = false;

  explicit operator bool() const
  {
    return primitive >= 0;
  }
  static litestl::binding::types::Struct<RayHit> *defineBindings();
};

/** A half-space: the points with `n.p + w >= 0`. */
struct BvhPlane {
  double3 n;
  double w = 0.0;

  double side(const double3 &p) const
  {
    return n[0] * p[0] + n[1] * p[1] + n[2] * p[2] + w;
  }
};

namespace bvh {

/** How much larger (relative to its reach) a node's box is made for rays. */
constexpr double kNodeSlack = 1e-12;

/** A hit on one primitive, in a RayQuery's scaled t. */
struct PrimHit {
  double t = 0.0;
  double u = 0.0, v = 0.0;
  bool inside = false;
  bool backFacing = false;
};

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
   * Whether the ray meets a box that stands for what is in it, between tMin and `limit`,
   * and where it enters (tMin, if it starts inside). The box is made larger, by
   * kNodeSlack of how far from the origin it reaches: more than the kernels' rounding.
   */
  template <class S>
  bool nodeSlab(const S *lo, const S *hi, double limit, double &enter) const
  {
    double leave = limit;
    enter = tMin;
    return meets(lo, hi, reach(lo, hi) * kNodeSlack, 0.0, enter, leave);
  }
  /** The same for a box that is the primitive. Never misses one the exact ray meets. */
  template <class S>
  bool boxSlab(const S *lo, const S *hi, double limit, double &enter) const
  {
    const double late = reach(lo, hi) * (16.0 * std::numeric_limits<double>::epsilon());
    double leave = limit;
    enter = tMin;
    return meets(lo, hi, 0.0, late, enter, leave);
  }
  /**
   * Moves a triangle's t into its own box, or is false if the ray misses that. The box is
   * made larger, but less than nodeSlab does. A triangle without an area has a t that is
   * noise.
   */
  bool keep(const double *lo, const double *hi, double &t) const
  {
    double leave = (std::numeric_limits<double>::max)(), enter = -leave;
    if (!meets(lo, hi, reach(lo, hi) * (0.1 * kNodeSlack), 0.0, enter, leave)) {
      return false;
    }
    t = t < enter ? enter : t > leave ? leave : t;
    return true;
  }

private:
  /* How far from the origin a box reaches, along any axis. */
  template <class S> double reach(const S *lo, const S *hi) const
  {
    double most = 0.0;
    for (int i = 0; i < 3; i++) {
      const double a = std::fabs(double(lo[i]) - origin[i]);
      const double b = std::fabs(double(hi[i]) - origin[i]);
      const double c = a > b ? a : b;
      most = c > most ? c : most;
    }
    return most;
  }

  /* Narrows [t0, t1] to where the ray is in the box, which is larger by `pad` on each
   * side, and left later by `late`. A NaN (infinity minus infinity) narrows nothing. */
  template <class S>
  bool
  meets(const S *lo, const S *hi, double pad, double late, double &t0, double &t1) const
  {
    double enter = t0, leave = t1;
    for (int i = 0; i < 3; i++) {
      if (!regular_) {
        /* A zero component, or one too small to invert. */
        if (dir[i] == 0.0) {
          if (origin[i] < double(lo[i]) - pad || origin[i] > double(hi[i]) + pad) {
            return false;
          }
          continue;
        }
        if (!finite(inv_[i])) {
          continue; /* conservative: this axis rejects nothing */
        }
      }
      const bool neg = inv_[i] < 0.0;
      const double grow = pad * std::fabs(inv_[i]);
      const double a = (double(neg ? hi[i] : lo[i]) - origin[i]) * inv_[i] - grow;
      const double b =
          (double(neg ? lo[i] : hi[i]) - origin[i]) * inv_[i] + (grow + late);
      enter = a > enter ? a : enter;
      leave = b < leave ? b : leave;
    }
    t0 = enter;
    t1 = leave;
    return enter <= leave;
  }

  double scaleT(double t) const
  {
    const double s = std::ldexp(t, exponent_);
    const double big = (std::numeric_limits<double>::max)();
    return s > big ? big : s < -big ? -big : s;
  }

  int exponent_ = 0;
  bool regular_ = true;
  double inv_[3];
};

} // namespace bvh
} // namespace coredcc
