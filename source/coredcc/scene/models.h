#pragma once

/* The built-in models, and what they share. Internal. */

#include "coredcc/scene/builtins.h"
#include "coredcc/scene/solve.h"
#include "coredcc/scene/transform_registry.h"

#include "litestl/math/euler.h"

namespace coredcc::models {

struct Trs {
  TransformState evaluate(const EvalCtx &ctx, const TransformState &parent) const;
  Status setWorld(const EvalCtx &current,
                  const mat4d &target,
                  const TransformState &parent,
                  SetWorldMode mode,
                  ChannelBlock &out) const;
  Status applyDelta(const EvalCtx &base,
                    const ChannelBlock &nearTo,
                    const TransformDelta &delta,
                    const TransformState &parent,
                    ChannelBlock &out) const;
  static litestl::binding::types::Struct<Trs> *defineBindings();
};

struct Fbx {
  TransformState evaluate(const EvalCtx &ctx, const TransformState &parent) const;
  Status setWorld(const EvalCtx &current,
                  const mat4d &target,
                  const TransformState &parent,
                  SetWorldMode mode,
                  ChannelBlock &out) const;
  Status applyDelta(const EvalCtx &base,
                    const ChannelBlock &nearTo,
                    const TransformDelta &delta,
                    const TransformState &parent,
                    ChannelBlock &out) const;
  static litestl::binding::types::Struct<Fbx> *defineBindings();
};

struct Matrix {
  TransformState evaluate(const EvalCtx &ctx, const TransformState &parent) const;
  Status setWorld(const EvalCtx &current,
                  const mat4d &target,
                  const TransformState &parent,
                  SetWorldMode mode,
                  ChannelBlock &out) const;
  static litestl::binding::types::Struct<Matrix> *defineBindings();
};

TransformTypeInfo trsInfo();
TransformTypeInfo fbxInfo();
TransformTypeInfo matrixInfo();

double3 readVec3(const EvalCtx &ctx, int32_t first);
void writeVec3(ChannelBlock &out, int32_t first, const double3 &v);
/** The six Euler orders, as the items of an enum. */
PropertyDef orderEnum(const char *name);
/** The Euler angles of `q` nearest to `hint`; non-finite hints count as 0. */
double3 nearestEuler(const quatd &q, litestl::math::EulerOrder order, double3 hint);
/** The state of a node from its parent, its linear part in the world and its local translation. */
TransformState childState(const TransformState &parent,
                          const solve::Linear &world,
                          const double3 &localTranslation,
                          const quatd &localRotation,
                          const double3 &localScale);
Status singularParent();
Status unsolvable();

/** How far a delta moves a point. Exactly 0 for a rotation's or a scale's pivot. */
double3 shiftOf(const TransformDelta &delta, const double3 &point);
/** A scale delta's factor along each of the node's own world axes. */
double3 factorsAlong(const TransformDelta &delta, const solve::Linear &axes);
/** The local rotation that turns points under `parent` around the same world axis as `delta`, by
 * the same angle: a conjugation, where that is a rotation. */
quatd turnUnder(const TransformDelta &delta,
                const solve::ParentSolver &solver,
                const solve::Linear &parent);

} // namespace coredcc::models
