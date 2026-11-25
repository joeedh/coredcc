#include "coredcc/scene/models.h"

#include "litestl/binding/binding.h"
#include "litestl/util/alloc.h"

namespace coredcc::models {

using litestl::math::EulerOrder;
using solve::Linear;

namespace {

struct Node {
  double3 location, scale;
  quatd rotation;
};

quatd readRotation(const EvalCtx &ctx)
{
  const int32_t mode = ctx.enumChannel(trs::kRotationMode, trs::kQuaternionMode + 1);
  if (mode != trs::kQuaternionMode) {
    return litestl::math::eulerToQuat(readVec3(ctx, trs::kRotationEuler), EulerOrder(mode));
  }
  quatd q;
  for (int32_t i = 0; i < 4; i++) {
    q[i] = ctx.channel(trs::kRotationQuaternion + i);
  }
  return q.normalized();
}

Node readNode(const EvalCtx &ctx)
{
  Node n;
  n.location = readVec3(ctx, trs::kLocation);
  n.scale = readVec3(ctx, trs::kScale);
  n.rotation = readRotation(ctx);
  return n;
}

/* Into the channels of `ctx`'s mode, nearest to what they are in `hint`. */
void writeRotation(const EvalCtx &ctx, const EvalCtx &hint, const quatd &q, ChannelBlock &out)
{
  const int32_t mode = ctx.enumChannel(trs::kRotationMode, trs::kQuaternionMode + 1);
  if (mode != trs::kQuaternionMode) {
    writeVec3(out,
              trs::kRotationEuler,
              nearestEuler(q, EulerOrder(mode), readVec3(hint, trs::kRotationEuler)));
    return;
  }
  double side = 0.0;
  for (int32_t i = 0; i < 4; i++) {
    side += q[i] * hint.channel(trs::kRotationQuaternion + i);
  }
  for (int32_t i = 0; i < 4; i++) {
    out.set(trs::kRotationQuaternion + i, side < 0.0 ? -q[i] : q[i]);
  }
}

} // namespace

ChannelBlock withRotationMode(const ChannelBlock &channels, int32_t mode)
{
  ChannelBlock out = channels;
  out.set(trs::kRotationMode, mode);
  writeRotation(EvalCtx(out), EvalCtx(channels), readRotation(EvalCtx(channels)), out);
  return out;
}

TransformState Trs::evaluate(const EvalCtx &ctx, const TransformState &parent) const
{
  const Node n = readNode(ctx);
  const Linear world = (Linear::of(parent.world) * Linear::of(n.rotation)).scaled(n.scale);
  return childState(parent, world, n.location, n.rotation, n.scale);
}

Status Trs::setWorld(const EvalCtx &current,
                     const mat4d &target,
                     const TransformState &parent,
                     SetWorldMode,
                     ChannelBlock &out) const
{
  solve::ParentSolver solver;
  if (!solver.set(parent.world)) {
    return singularParent();
  }
  const Node n = readNode(current);
  solve::RotationScale rs;
  if (!solve::rotationScale(
          solver.linear(Linear::of(target)), Linear::identity(), n.rotation, n.scale, rs))
  {
    return unsolvable();
  }
  writeVec3(out, trs::kLocation, solver.point(solve::translationOf(target)));
  writeVec3(out, trs::kScale, rs.scale);
  writeRotation(current, current, rs.rotation, out);
  return Status();
}

Status Trs::applyDelta(const EvalCtx &base,
                       const ChannelBlock &nearTo,
                       const TransformDelta &delta,
                       const TransformState &parent,
                       ChannelBlock &out) const
{
  solve::ParentSolver solver;
  if (!solver.set(parent.world)) {
    return singularParent();
  }
  const Node n = readNode(base);
  const Linear p = Linear::of(parent.world);
  const ChannelBlock hint = nearTo; /* `out` may be the same block */
  out = base.snapshot();
  const double3 origin = litestl::math::transformPoint(parent.world, n.location);
  writeVec3(out, trs::kLocation, n.location + solver.vector(shiftOf(delta, origin)));
  if (delta.kind == DeltaKind::Rotate) {
    const quatd q = (turnUnder(delta, solver, p) * n.rotation).normalized();
    writeRotation(base, EvalCtx(hint), q, out);
  }
  else if (delta.kind == DeltaKind::Scale) {
    quatd rotation = n.rotation;
    double3 scale = n.scale;
    if (mirrors(delta)) {
      const Linear x = solver.linear(mirrored(delta, p * Linear::of(rotation)));
      bool turned = false;
      CDCC_TRY(takeMirror(x, rotation, scale, turned));
      if (turned) {
        writeRotation(base, EvalCtx(hint), rotation, out);
      }
    }
    writeVec3(out, trs::kScale, scale * factorsAlong(delta, p * Linear::of(rotation)));
  }
  return Status();
}

TransformTypeInfo trsInfo()
{
  TransformTypeInfo info;
  info.name = kTrsTransform;
  info.uiName = "Location, Rotation, Scale";
  info.channels.append(
      PropertyDef::vecProp("location", 3).subtype(PropSubtype::Distance).unit(UnitFamily::Length));
  info.channels.append(orderEnum("rotationMode").item("QUATERNION", trs::kQuaternionMode));
  info.channels.append(
      PropertyDef::vecProp("rotationEuler", 3).subtype(PropSubtype::Euler).unit(UnitFamily::Angle));
  info.channels.append(
      PropertyDef::quatProp("rotationQuaternion").defaultValue(PropValue::ofQuat(0, 0, 0, 1)));
  info.channels.append(PropertyDef::vecProp("scale", 3).defaultValue(PropValue::ofVec3(1, 1, 1)));
  return info;
}

litestl::binding::types::Struct<Trs> *Trs::defineBindings()
{
  namespace lbt = litestl::binding::types;
  static lbt::Struct<Trs> *st = nullptr;
  if (!st) {
    litestl::alloc::PermanentGuard guard;
    st = new lbt::Struct<Trs>("coredcc::models::Trs", sizeof(Trs));
    BIND_STRUCT_METHOD(st, evaluate, MARGS("ctx", "parent"));
    BIND_STRUCT_METHOD(st, setWorld, MARGS("current", "target", "parent", "mode", "out"));
    BIND_STRUCT_METHOD(st, applyDelta, MARGS("base", "nearTo", "delta", "parent", "out"));
  }
  return st;
}

} // namespace coredcc::models
