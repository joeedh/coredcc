#pragma once

/** \file
 * The stroke's curve math: a centripetal Catmull-Rom segment as a cubic Bezier, a part of a
 * cubic, and a walk that gives evenly spaced parameters by arc length, carrying the remainder
 * from one segment to the next.
 *
 * A port of sculptcore's `brush/stroke_curve.h`, which is itself a port of its TypeScript
 * `stroke_math.ts`. EPS and the chord count are the same as theirs, so that the samples agree
 * (test_tool_stroke's goldens). All in double: the carry accumulates over a whole stroke.
 */

#include "litestl/math/vector.h"
#include "litestl/util/vector.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace coredcc::stroke {

template <int N> using Point = litestl::math::Vec<double, N>;

/** A cubic Bezier: its four control points. */
template <int N> struct Cubic {
  Point<N> p[4];

  Point<N> &operator[](int i)
  {
    return p[i];
  }
  const Point<N> &operator[](int i) const
  {
    return p[i];
  }
};

inline constexpr double kEps = 1e-7;

template <int N> inline double dist(const Point<N> &a, const Point<N> &b)
{
  double sum = 0.0;
  for (int i = 0; i < N; i++) {
    const double d = a[i] - b[i];
    sum += d * d;
  }
  return std::sqrt(sum);
}

template <int N> inline Point<N> lerp(const Point<N> &a, const Point<N> &b, double t)
{
  Point<N> out;
  for (int i = 0; i < N; i++) {
    out[i] = a[i] + (b[i] - a[i]) * t;
  }
  return out;
}

/**
 * The Catmull-Rom segment from P1 to P2 as a cubic Bezier. `alpha` is the knot spacing: 0
 * uniform, 0.5 centripetal, 1 chordal. A neighbour on its endpoint makes that end's tangent
 * one-sided, which is how the caller clamps the ends.
 *
 * The P1..P2 interval is floored at kEps wherever it divides: a pointer that reports one
 * position twice makes the segment degenerate, and 0/0 would give a NaN curve that poisons
 * the walk's carry for the rest of the stroke.
 */
template <int N>
inline Cubic<N> catmullRomToBezier(const Point<N> &P0,
                                   const Point<N> &P1,
                                   const Point<N> &P2,
                                   const Point<N> &P3,
                                   double alpha = 0.5)
{
  const double t01 = std::pow(dist(P0, P1), alpha);
  const double t12 = std::pow(dist(P1, P2), alpha);
  const double t23 = std::pow(dist(P2, P3), alpha);
  const double span = t12 > kEps ? t12 : kEps;

  Point<N> m1, m2;
  for (int i = 0; i < N; i++) {
    m1[i] = t01 < kEps ? (P2[i] - P1[i]) / span
                       : (P2[i] - P1[i]) / span - (P2[i] - P0[i]) / (t01 + span) +
                             (P1[i] - P0[i]) / t01;
    m2[i] = t23 < kEps ? (P2[i] - P1[i]) / span
                       : (P3[i] - P2[i]) / t23 - (P3[i] - P1[i]) / (span + t23) +
                             (P2[i] - P1[i]) / span;
  }
  Cubic<N> out;
  out[0] = P1;
  out[3] = P2;
  for (int i = 0; i < N; i++) {
    out[1][i] = P1[i] + m1[i] * (span / 3.0);
    out[2][i] = P2[i] + m2[i] * (-span / 3.0);
  }
  return out;
}

template <int N> inline Point<N> evalCubic(const Cubic<N> &B, double s)
{
  const double mt = 1.0 - s;
  const double a = mt * mt * mt, b = 3.0 * mt * mt * s, c = 3.0 * mt * s * s, d = s * s * s;
  Point<N> out;
  for (int i = 0; i < N; i++) {
    out[i] = a * B[0][i] + b * B[1][i] + c * B[2][i] + d * B[3][i];
  }
  return out;
}

/** The cubic split at `t`: [0, t] and [t, 1]. */
template <int N> inline void splitCubic(const Cubic<N> &B, double t, Cubic<N> &left, Cubic<N> &right)
{
  const Point<N> a = lerp(B[0], B[1], t), b = lerp(B[1], B[2], t), c = lerp(B[2], B[3], t);
  const Point<N> d = lerp(a, b, t), e = lerp(b, c, t);
  const Point<N> f = lerp(d, e, t);
  left = Cubic<N>{{B[0], a, d, f}};
  right = Cubic<N>{{f, e, c, B[3]}};
}

/** The part over [t0, t1], clamped to [0, 1], as a cubic of its own. */
template <int N> inline Cubic<N> subCubic(const Cubic<N> &B, double t0, double t1)
{
  t0 = std::clamp(t0, 0.0, 1.0);
  t1 = std::clamp(t1, 0.0, 1.0);
  if (t1 <= kEps) {
    return Cubic<N>{{B[0], B[0], B[0], B[0]}};
  }
  Cubic<N> head, tail, unused;
  splitCubic(B, t1, head, unused);
  splitCubic(head, t0 / t1, unused, tail);
  return tail;
}

struct Walk {
  /** Parameters in [0, 1], one every `spacing` of arc length. */
  litestl::util::Vector<double> ts;
  /** The arc length since the last one, for the next segment. */
  double carry = 0.0;
  /** Those past `maxSamples`, which aren't in `ts`. */
  int64_t dropped = 0;
};

/**
 * Parameters every `spacing` of arc length along a cubic, continuing from `carry` (the length
 * since the previous segment's last one). The length is that of `chords` straight chords,
 * with the parameter linear inside each.
 */
template <int N>
inline Walk arcLengthWalk(const Cubic<N> &B,
                          double spacing,
                          double carry = 0.0,
                          int chords = 32,
                          int32_t maxSamples = INT32_MAX)
{
  Walk res;
  if (spacing <= kEps) {
    res.carry = carry;
    return res;
  }
  double acc = carry;
  Point<N> prev = evalCubic(B, 0.0);
  for (int k = 1; k <= chords; k++) {
    const double sA = double(k - 1) / double(chords), sB = double(k) / double(chords);
    const Point<N> cur = evalCubic(B, sB);
    const double seg = dist(prev, cur);
    prev = cur;
    if (seg <= kEps) {
      continue;
    }
    double local = 0.0;
    while (acc + seg * (1.0 - local) >= spacing) {
      if (int32_t(res.ts.size()) >= maxSamples) {
        /* Counted, not walked: the rest of this chord at once. */
        const double rest = acc + seg * (1.0 - local);
        const double n = std::floor(rest / spacing);
        res.dropped += int64_t(n);
        acc = rest - n * spacing;
        local = 1.0;
        break;
      }
      local += (spacing - acc) / seg;
      res.ts.append(sA + (sB - sA) * local);
      acc = 0.0;
    }
    acc += seg * (1.0 - local);
  }
  res.carry = acc;
  return res;
}

} // namespace coredcc::stroke
