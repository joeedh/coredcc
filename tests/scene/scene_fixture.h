#pragma once

/* A seeded generator, and a registry with the built-in transform types. */

#include "coredcc/api/context.h"
#include "coredcc/bvh/box.h"
#include "coredcc/scene/builtins.h"
#include "coredcc/scene/solve.h"
#include "coredcc/scene/transform_registry.h"

#include "litestl/math/euler.h"

#include "fuzz.h"
#include "litestl/tests/test_util.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

namespace scene_test {

using namespace coredcc;
using litestl::math::EulerOrder;
using solve::Linear;

struct Rand : coredcc_test::SplitMix64 {
  using SplitMix64::SplitMix64;
  double range(double a, double b)
  {
    return a + (b - a) * (double(next() >> 11) * 0x1p-53);
  }
  bool coin()
  {
    return (next() & 1) != 0;
  }
  double3 vec(double size)
  {
    const double x = range(-size, size), y = range(-size, size);
    return double3(x, y, range(-size, size));
  }
  /** Magnitudes from 1/span to span, with random signs. */
  double3 scale(double span, bool signs)
  {
    double3 s;
    for (int i = 0; i < 3; i++) {
      s[i] = std::exp(range(-std::log(span), std::log(span))) *
             (signs && coin() ? -1.0 : 1.0);
    }
    return s;
  }
  quatd rotation()
  {
    const double x = range(-1, 1), y = range(-1, 1), z = range(-1, 1);
    return quatd(x, y, z, range(-1, 1)).normalized();
  }
};

struct Types {
  Runtime rt;
  const TransformType *trs, *fbx, *matrix;
  Types()
  {
    const Status s = registerTransformTypes(rt);
    if (!s.ok()) {
      fprintf(stderr, "%s\n", s.message());
      abort();
    }
    const TransformRegistry &reg = rt.ext<TransformRegistry>();
    trs = reg.find(kTrsTransform);
    fbx = reg.find(kFbxTransform);
    matrix = reg.find(kMatrixTransform);
  }
};

inline double lengthOf(const double3 &v)
{
  return std::hypot(v[0], v[1], v[2]);
}

inline void put(ChannelBlock &b, int first, const double3 &v)
{
  for (int i = 0; i < 3; i++) {
    b.set(first + i, v[i]);
  }
}
inline double3 get(const ChannelBlock &b, int first)
{
  return double3(b.get(first), b.get(first + 1), b.get(first + 2));
}

inline TransformState
eval(const TransformType &t, const ChannelBlock &c, const TransformState &p)
{
  return *evaluateTransform(t, EvalCtx(c), p);
}

/** The larger; NaN counts as huge. */
inline double worst(double a, double b)
{
  return std::isnan(a) || std::isnan(b) ? 1e300 : std::fmax(a, b);
}

/** The largest difference of two matrices' elements. */
inline double difference(const mat4d &a, const mat4d &b)
{
  const double *x = a, *y = b;
  double e = 0.0;
  for (int i = 0; i < 16; i++) {
    e = worst(e, std::fabs(x[i] - y[i]));
  }
  return e;
}

inline ChannelBlock
trsChannels(Rand &r, const TransformType &t, const double3 &scale, double size)
{
  ChannelBlock c = t.defaults();
  put(c, trs::kLocation, r.vec(size));
  c.set(trs::kRotationMode, double(r.below(7)));
  put(c, trs::kRotationEuler, r.vec(3.0));
  const double length = r.range(0.1, 5.0);
  const quatd q = r.rotation();
  for (int i = 0; i < 4; i++) {
    c.set(trs::kRotationQuaternion + i, q[i] * length);
  }
  put(c, trs::kScale, scale);
  return c;
}

inline ChannelBlock fbxChannels(
    Rand &r, const TransformType &t, int inherit, const double3 &scale, double pivots)
{
  ChannelBlock c = t.defaults();
  put(c, fbx::kTranslation, r.vec(5.0 + pivots));
  put(c, fbx::kRotation, r.vec(3.0));
  c.set(fbx::kRotationOrder, double(r.below(6)));
  if (pivots > 0.0) {
    put(c, fbx::kPreRotation, r.vec(3.0));
    put(c, fbx::kPostRotation, r.vec(3.0));
    for (int f = fbx::kRotationOffset; f < fbx::kScaling; f += 3) {
      put(c, f, r.vec(pivots));
    }
  }
  put(c, fbx::kScaling, scale);
  c.set(fbx::kInheritType, double(inherit));
  return c;
}

/** A state some levels down, of mixed types. */
inline TransformState chain(Rand &r, const Types &t, int depth, double span, bool signs)
{
  TransformState p;
  for (int i = 0; i < depth; i++) {
    const double3 s = r.scale(span, signs);
    p = r.coin() ? eval(*t.trs, trsChannels(r, *t.trs, s, 3.0), p)
                 : eval(*t.fbx, fbxChannels(r, *t.fbx, int(r.below(3)), s, 0.0), p);
  }
  return p;
}

inline bool sameBits(const TransformState &a, const TransformState &b)
{
  bool same = !memcmp(
      static_cast<const double *>(a.world), static_cast<const double *>(b.world), 128);
  for (int i = 0; i < 4; i++) {
    same &=
        a.rotation[i] == b.rotation[i] && (i == 3 || a.localScale[i] == b.localScale[i]);
  }
  return same;
}

} // namespace scene_test
