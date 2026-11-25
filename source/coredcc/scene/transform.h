#pragma once

#include "coredcc/base/iface/iface.h"
#include "coredcc/base/status.h"
#include "coredcc/scene/channel_entries.h"
#include "coredcc/scene/eval_ctx.h"

#include "litestl/math/matrix_bindings.h"
#include "litestl/math/quat.h"
#include "litestl/math/transform.h"

#include <cstdint>

namespace coredcc {

using litestl::math::double3;
using litestl::math::mat4d;
using litestl::math::quatd;

/** What an object's transform passes on to its children. */
struct TransformState {
  /** Object to world. Column-major, affine. */
  mat4d world;
  /** The product of the local rotations along the chain: the world's rotation without scale,
   * shear and reflection. FBX inherit types need it carried, not decomposed. */
  quatd rotation;
  /** The local scale, signed. */
  double3 localScale;

  /** The identity: a root's parent. */
  TransformState() : localScale(1.0, 1.0, 1.0)
  {
    world.identity();
  }
  /** Whether every number is finite. */
  bool finite() const;

  static litestl::binding::types::Struct<TransformState> *defineBindings();
};

enum class SetWorldMode : int32_t {
  /** The target exactly (within the tolerances of solveWorld), or an error. */
  Exact = 0,
  /** Without what the model can't represent (shear, usually). */
  Closest = 1,
};

enum class DeltaKind : int32_t { Translate = 0, Rotate = 1, Scale = 2 };

/** A transformation in world space, as a tool applies it. */
struct TransformDelta {
  DeltaKind kind = DeltaKind::Translate;
  /** The orientation that `vector` and `axis` are in, as a rotation to world space: the
   * world's, an object's, the view's, or a custom one. */
  quatd basis;
  /** The fixed point of a rotation or a scale, in world space. */
  double3 pivot;
  /** The translation, or the factors of a scale along the basis's axes. */
  double3 vector;
  /** Of a rotation; needn't be unit length. */
  double3 axis;
  /** Of a rotation, in radians. */
  double angle = 0.0;

  /** The delta as a matrix. A scale by 0 is singular. */
  mat4d matrix() const;
  /** The rotation of a Rotate delta, in world space; the identity for the others. */
  quatd rotation() const;

  /* For scripts. */
  int32_t kindValue() const
  {
    return int32_t(kind);
  }
  void setKind(int32_t k)
  {
    kind = DeltaKind(k);
  }
  static litestl::binding::types::Struct<TransformDelta> *defineBindings();
};

/**
 * A kind of transform: how channels and the parent's state give an object's. Stateless: one
 * instance serves every object of its type. Native or foreign, and never called from a worker
 * when it is thread-affine.
 *
 * - `evaluate` is pure. It must accept any channel values, and may give a state that isn't
 *   finite.
 * - `setWorld` finds channels that put the object at `target`. `out` starts as a copy of
 *   `current`; channels that don't matter are left as they are. The caller checks the result
 *   (solveWorld), so a model may return its nearest solution without measuring it.
 * - `applyDelta` (optional) applies `delta` to `base`. `nearTo` is the previous result of the
 *   same interaction (or `base`): where several channel values mean the same, such as Euler
 *   angles, the ones nearest to it are wanted. `out` starts as a copy of `nearTo`. Without the
 *   slot, setWorld in Closest mode is used.
 * - `upgrade` (optional) changes what a file has of version `version` to what this build's
 *   channels are (TransformTypeInfo::version). After an error the object keeps the file's channels
 *   unread, and its status is Invalid.
 */
struct TransformModel : IfaceRef {
  TransformState evaluate(const EvalCtx &ctx, const TransformState &parent) const
  {
    return CDCC_CALL(evaluate, ctx, parent);
  }
  Status setWorld(const EvalCtx &current,
                  const mat4d &target,
                  const TransformState &parent,
                  SetWorldMode mode,
                  ChannelBlock &out) const
  {
    return CDCC_CALL(setWorld, current, target, parent, mode, out);
  }
  bool hasApplyDelta() const
  {
    return CDCC_HAS(applyDelta);
  }
  Status applyDelta(const EvalCtx &base,
                    const ChannelBlock &nearTo,
                    const TransformDelta &delta,
                    const TransformState &parent,
                    ChannelBlock &out) const
  {
    return CDCC_CALL(applyDelta, base, nearTo, delta, parent, out);
  }

  bool hasUpgrade() const
  {
    return CDCC_HAS(upgrade);
  }
  Status upgrade(uint32_t version, ChannelEntries &channels) const
  {
    return CDCC_CALL(upgrade, version, channels);
  }

  CDCC_INTERFACE(TransformModel, "coredcc::TransformModel")
  {
    BIND_STRUCT_METHOD(st, evaluate, MARGS("ctx", "parent"));
    BIND_STRUCT_METHOD(st, setWorld, MARGS("current", "target", "parent", "mode", "out"));
    BIND_OPTIONAL_METHOD(
        st, applyDelta, MARGS("base", "nearTo", "delta", "parent", "out"));
    BIND_OPTIONAL_METHOD(st, upgrade, MARGS("version", "channels"));
  }
};

} // namespace coredcc

namespace litestl::binding {
/** SetWorldMode, bound as an enum (scene/bindings.cc). */
template <> struct Binder<coredcc::SetWorldMode> {
  static const BindingBase *bind();
};
} // namespace litestl::binding
