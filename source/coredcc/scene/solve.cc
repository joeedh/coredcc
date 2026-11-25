#include "coredcc/scene/solve.h"

#include "coredcc/bvh/box.h"

#include <cmath>
#include <limits>

namespace coredcc::solve {

namespace {

constexpr double kZeroColumn = 1e-12;
constexpr double kSameResidual = 1e-9;
/* A world vector of length 1 may be one of this length in the space of a parent of size 1. */
constexpr double kLargestCondition = 1e12;

double lengthOf(const double3 &v)
{
  return std::hypot(v[0], v[1], v[2]);
}

bool finite3(const double3 &v)
{
  return bvh::finite(v[0]) && bvh::finite(v[1]) && bvh::finite(v[2]);
}

/* Scaled by the largest component first, so that no square leaves the range. `v` isn't 0. */
double3 unit(const double3 &v)
{
  const double big = std::fmax(std::fmax(std::fabs(v[0]), std::fabs(v[1])), std::fabs(v[2]));
  const double3 s = v / big;
  return s / lengthOf(s);
}

/* A rotation with its x axis along `u` and its y axis in the plane of `u` and `v`; not finite
 * when they are parallel. */
Linear frame(const double3 &u, const double3 &v)
{
  Linear f;
  f.col[0] = u;
  f.col[2] = unit(u.cross(v));
  f.col[1] = f.col[2].cross(u);
  return f;
}

/* The smallest rotation from one unit vector to another. */
quatd turn(const double3 &from, const double3 &to)
{
  const double c = from.dot(to);
  if (c < -1.0 + 1e-12) {
    int least = 0;
    for (int i = 1; i < 3; i++) {
      least = std::fabs(from[i]) < std::fabs(from[least]) ? i : least;
    }
    double3 other;
    other[least] = 1.0;
    const double3 axis = unit(from.cross(other));
    return quatd(axis[0], axis[1], axis[2], 0.0);
  }
  const double3 v = from.cross(to);
  return quatd(v[0], v[1], v[2], 1.0 + c).normalized();
}

quatd quatOf(const Linear &m)
{
  return quatd::fromMatrix(affine(m, double3()));
}

struct Columns {
  double3 unitX[3], unitA[3];
  double3 scale; /* |X| / |A| */
  bool zero[3] = {true, true, true}; /* a zero column of X */
  int nonzero[3];
  int count = 0;
};

/* The rotation for one sign pattern. */
quatd fitRotation(const Columns &c, const double *sign, const int *pair, const quatd &current)
{
  if (c.count >= 2 && pair[0] >= 0) {
    const Linear to = frame(c.unitX[pair[0]] * sign[pair[0]], c.unitX[pair[1]] * sign[pair[1]]);
    const Linear from = frame(c.unitA[pair[0]], c.unitA[pair[1]]);
    return quatOf(to * from.transposed());
  }
  if (c.count >= 1) {
    const int k = c.nonzero[0];
    return (turn(current.rotate(c.unitA[k]), c.unitX[k] * sign[k]) * current).normalized();
  }
  return current;
}

/* The two nonzero columns whose directions are the most independent, in both A and X. */
void choosePair(const Columns &c, int *pair)
{
  pair[0] = pair[1] = -1;
  double best = 0.0;
  for (int n = 0; n < c.count; n++) {
    for (int m = n + 1; m < c.count; m++) {
      const int i = c.nonzero[n], j = c.nonzero[m];
      const double s = std::fmin(lengthOf(c.unitA[i].cross(c.unitA[j])),
                                 lengthOf(c.unitX[i].cross(c.unitX[j])));
      if (s > best) {
        best = s;
        pair[0] = i;
        pair[1] = j;
      }
    }
  }
}

} // namespace

Linear Linear::identity()
{
  Linear m;
  for (int i = 0; i < 3; i++) {
    m.col[i][i] = 1.0;
  }
  return m;
}

Linear Linear::of(const mat4d &m)
{
  const double *d = m;
  Linear r;
  for (int c = 0; c < 3; c++) {
    r.col[c] = double3(d[c * 4], d[c * 4 + 1], d[c * 4 + 2]);
  }
  return r;
}

Linear Linear::of(const quatd &q)
{
  return of(q.toMatrix4());
}

Linear Linear::transposed() const
{
  Linear r;
  for (int i = 0; i < 3; i++) {
    for (int j = 0; j < 3; j++) {
      r.col[i][j] = col[j][i];
    }
  }
  return r;
}

Linear Linear::scaled(const double3 &s) const
{
  Linear r;
  for (int i = 0; i < 3; i++) {
    r.col[i] = col[i] * s[i];
  }
  return r;
}

double Linear::norm() const
{
  return std::hypot(lengthOf(col[0]), lengthOf(col[1]), lengthOf(col[2]));
}

Linear operator*(const Linear &a, const Linear &b)
{
  Linear r;
  for (int i = 0; i < 3; i++) {
    r.col[i] = a.apply(b.col[i]);
  }
  return r;
}

mat4d affine(const Linear &linear, const double3 &translation)
{
  mat4d m;
  for (int c = 0; c < 3; c++) {
    for (int r = 0; r < 3; r++) {
      m[c][r] = linear.col[c][r];
    }
    m[3][c] = translation[c];
  }
  m[3][3] = 1.0;
  return m;
}

double3 translationOf(const mat4d &m)
{
  const double *d = m;
  return double3(d[12], d[13], d[14]);
}

bool rotationScale(const Linear &x,
                   const Linear &a,
                   const quatd &rotation,
                   const double3 &scale,
                   RotationScale &out)
{
  double longest = 0.0, lenX[3], lenA[3];
  for (int i = 0; i < 3; i++) {
    lenX[i] = lengthOf(x.col[i]);
    lenA[i] = lengthOf(a.col[i]);
    longest = std::fmax(longest, lenX[i]);
    if (!(lenA[i] > 0.0) || !bvh::finite(lenA[i]) || !bvh::finite(lenX[i])) {
      return false;
    }
  }
  Columns c;
  for (int i = 0; i < 3; i++) {
    c.unitA[i] = unit(a.col[i]);
    if (lenX[i] > kZeroColumn * longest) {
      c.unitX[i] = unit(x.col[i]);
      c.scale[i] = lenX[i] / lenA[i];
      c.zero[i] = false;
      c.nonzero[c.count++] = i;
    }
  }
  int pair[2];
  choosePair(c, pair);
  const quatd current = rotation.normalized(); /* the identity if it isn't finite */

  struct Candidate {
    quatd rotation;
    double sign[3];
    double residual;
    int changes;
  } found[8];
  int count = 0;
  double smallest = std::numeric_limits<double>::infinity();
  for (int bits = 0; bits < 8; bits++) {
    Candidate k;
    k.changes = 0;
    bool skip = false;
    for (int i = 0; i < 3; i++) {
      k.sign[i] = (bits >> i) & 1 ? -1.0 : 1.0;
      skip = skip || (c.zero[i] && k.sign[i] < 0.0);
      k.changes += (k.sign[i] < 0.0) != (scale[i] < 0.0);
    }
    if (skip) {
      continue;
    }
    k.rotation = fitRotation(c, k.sign, pair, current);
    k.residual = 0.0;
    for (int n = 0; n < c.count; n++) {
      const int i = c.nonzero[n];
      const double3 e = k.rotation.rotate(c.unitA[i]) - c.unitX[i] * k.sign[i];
      k.residual = std::fmax(k.residual, lengthOf(e));
    }
    smallest = std::fmin(smallest, k.residual);
    found[count++] = k;
  }

  const Candidate *best = nullptr;
  for (int n = 0; n < count; n++) {
    const Candidate &k = found[n];
    if (!(k.residual <= smallest + kSameResidual)) {
      continue;
    }
    const auto nearness = [&](const Candidate &q) { return std::fabs(q.rotation.dot(current)); };
    if (!best || k.changes < best->changes ||
        (k.changes == best->changes && nearness(k) > nearness(*best) + kSameNearness))
    {
      best = &k;
    }
  }
  out.rotation = best->rotation;
  out.residual = best->residual;
  for (int i = 0; i < 3; i++) {
    out.scale[i] = c.scale[i] * best->sign[i];
  }
  return finite3(out.scale);
}

bool ParentSolver::set(const mat4d &world)
{
  mat4d inverse;
  if (!litestl::math::tryInverse(affine(Linear::of(world), double3()), inverse)) {
    return false;
  }
  p_ = Linear::of(world);
  inverse_ = Linear::of(inverse);
  translation_ = translationOf(world);
  return finite3(translation_) && p_.norm() * inverse_.norm() <= kLargestCondition;
}

double3 ParentSolver::vector(const double3 &b) const
{
  double3 y = inverse_.apply(b);
  for (int i = 0; i < 2; i++) {
    y = y + inverse_.apply(b - p_.apply(y));
  }
  return y;
}

double3 ParentSolver::point(const double3 &p) const
{
  return vector(p - translation_);
}

Linear ParentSolver::linear(const Linear &b) const
{
  Linear r;
  for (int i = 0; i < 3; i++) {
    r.col[i] = vector(b.col[i]);
  }
  return r;
}

bool sameLinear(const mat4d &world, const mat4d &target)
{
  const Linear w = Linear::of(world), t = Linear::of(target);
  double longest = 0.0, len[3];
  for (int i = 0; i < 3; i++) {
    len[i] = lengthOf(t.col[i]);
    longest = std::fmax(longest, len[i]);
  }
  for (int i = 0; i < 3; i++) {
    const double e = lengthOf(w.col[i] - t.col[i]);
    const double limit = len[i] > kZeroColumn * longest ? kSameResidual * len[i] :
                                                          kZeroColumn * longest;
    if (!(e <= limit)) {
      return false;
    }
  }
  return true;
}

} // namespace coredcc::solve
