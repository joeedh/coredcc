#pragma once

#include "coredcc/scene/transform.h"

namespace coredcc::solve {

/** Three columns. */
struct Linear {
  double3 col[3];

  static Linear identity();
  /** The linear part of an affine matrix. */
  static Linear of(const mat4d &m);
  static Linear of(const quatd &q);
  double3 apply(const double3 &v) const
  {
    return col[0] * v[0] + col[1] * v[1] + col[2] * v[2];
  }
  Linear transposed() const;
  /** Columns times the components of `s`. */
  Linear scaled(const double3 &s) const;
  /** The Frobenius norm. */
  double norm() const;
};
Linear operator*(const Linear &a, const Linear &b);

mat4d affine(const Linear &linear, const double3 &translation);
double3 translationOf(const mat4d &m);

/** Two rotations that are equally near to another, but for rounding: the first one
 * counts. */
inline constexpr double kSameNearness = 1e-9;

struct RotationScale {
  quatd rotation;
  double3 scale;
  /** The largest relative error of a column of `R A S` against `X`, zero columns left
   * out. */
  double residual = 0.0;
};

/**
 * Finds a rotation R and a diagonal S with `R A S = X`. Without an exact solution (X has
 * shear that A doesn't) the rotation fits the two most independent columns.
 *
 * Several sign patterns of S can be as good: the one with the fewest sign changes from
 * `scale` is taken, then the rotation nearest to `rotation`. A zero column of X has scale
 * 0; the rotation is then completed from the other columns, or from `rotation`.
 *
 * False when an input isn't finite or a column of A is zero.
 */
bool rotationScale(const Linear &x,
                   const Linear &a,
                   const quatd &rotation,
                   const double3 &scale,
                   RotationScale &out);

/**
 * A parent's linear part, for solving `P y = b`: the inverse, and two steps of iterative
 * refinement, so that the residual is a few ulps of |P| |y| for parents of any condition.
 */
class ParentSolver {
public:
  /** False when `world` is singular, or so nearly that a step in the world is a huge one
   * in its space: when its largest scale is about 1e12 times its smallest. */
  bool set(const mat4d &world);
  /** y with `P y = b` (a direction). */
  double3 vector(const double3 &b) const;
  /** The local point of a world point. */
  double3 point(const double3 &p) const;
  Linear linear(const Linear &b) const;

private:
  Linear p_, inverse_;
  double3 translation_;
};

/** Whether a state's linear part is `target`'s: each column within 1e-9 of its length,
 * and zero columns within 1e-12 of the longest. */
bool sameLinear(const mat4d &world, const mat4d &target);

} // namespace coredcc::solve
