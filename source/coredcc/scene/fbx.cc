/* FBX's node transform, after Autodesk's "Transformations" SDK sample. The parent's global
 * rotation is carried in TransformState: the sample decomposes it, which loses reflections. */
#include "coredcc/scene/models.h"

#include "litestl/binding/binding.h"
#include "litestl/util/alloc.h"

namespace coredcc::models {

using litestl::math::EulerOrder;
using litestl::math::eulerToQuat;
using solve::Linear;

namespace {

struct Node {
  double3 translation, scale;
  double3 rotationOffset, rotationPivot, scalingOffset, scalingPivot;
  quatd pre, post;
  quatd rotation; /* Rpre R Rpost^-1 */
  EulerOrder order;
  int32_t inherit;
};

/* eulerToQuat. Most pre- and post-rotations are zero. */
quatd eulerOf(const double3 &v, EulerOrder order)
{
  return v[0] == 0.0 && v[1] == 0.0 && v[2] == 0.0 ? quatd() : eulerToQuat(v, order);
}

Node readNode(const EvalCtx &ctx)
{
  Node n;
  n.translation = readVec3(ctx, fbx::kTranslation);
  n.scale = readVec3(ctx, fbx::kScaling);
  n.rotationOffset = readVec3(ctx, fbx::kRotationOffset);
  n.rotationPivot = readVec3(ctx, fbx::kRotationPivot);
  n.scalingOffset = readVec3(ctx, fbx::kScalingOffset);
  n.scalingPivot = readVec3(ctx, fbx::kScalingPivot);
  n.pre = eulerOf(readVec3(ctx, fbx::kPreRotation), EulerOrder::XYZ);
  n.post = eulerOf(readVec3(ctx, fbx::kPostRotation), EulerOrder::XYZ);
  n.order = EulerOrder(ctx.enumChannel(fbx::kRotationOrder, 6));
  n.inherit = ctx.enumChannel(fbx::kInheritType, 3);
  n.rotation = n.pre * eulerOf(readVec3(ctx, fbx::kRotation), n.order) * n.post.conjugated();
  return n;
}

/* The local translation without `translation`, for a rotation and a scale. */
double3 pivotOffset(const Node &n, const quatd &rotation, const double3 &scale)
{
  const double3 inner = n.scalingOffset - n.rotationPivot + n.scalingPivot - n.scalingPivot * scale;
  return n.rotationOffset + n.rotationPivot + rotation.rotate(inner);
}

/* What comes between the node's rotation and its scale. */
Linear inherited(const Node &n, const TransformState &parent)
{
  if (n.inherit == fbx::kRSrs) {
    return Linear::identity();
  }
  Linear a = Linear::of(parent.rotation.normalized().conjugated()) * Linear::of(parent.world);
  if (n.inherit == fbx::kRrs) {
    for (int i = 0; i < 3; i++) {
      const double s = parent.localScale[i];
      a.col[i] = a.col[i] / (s == 0.0 ? 1.0 : s);
    }
  }
  return a;
}

/* What comes before the node's rotation. */
Linear before(const Node &n, const TransformState &parent)
{
  return n.inherit == fbx::kRSrs ? Linear::of(parent.world) :
                                   Linear::of(parent.rotation.normalized());
}

/* The node's world axes: its linear part without its scale. */
Linear axesOf(const Node &n, const quatd &rotation, const TransformState &parent)
{
  return before(n, parent) * Linear::of(rotation) * inherited(n, parent);
}

void writeRotation(const Node &n, const double3 &hint, const quatd &rotation, ChannelBlock &out)
{
  const quatd r = (n.pre.conjugated() * rotation * n.post).normalized();
  writeVec3(out, fbx::kRotation, nearestEuler(r, n.order, hint));
}

} // namespace

TransformState Fbx::evaluate(const EvalCtx &ctx, const TransformState &parent) const
{
  const Node n = readNode(ctx);
  return childState(parent,
                    axesOf(n, n.rotation, parent).scaled(n.scale),
                    n.translation + pivotOffset(n, n.rotation, n.scale),
                    n.rotation,
                    n.scale);
}

/* The rotation pivot. With the inherit types that don't turn within the parent's scale, its own
 * rotation channels turn the node about another point. */
double3 Fbx::pivot(const EvalCtx &ctx, const TransformState &parent) const
{
  const double3 local = readVec3(ctx, fbx::kTranslation) + readVec3(ctx, fbx::kRotationOffset) +
                        readVec3(ctx, fbx::kRotationPivot);
  return litestl::math::transformPoint(parent.world, local);
}

Status Fbx::setWorld(const EvalCtx &current,
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
  const Linear x = n.inherit == fbx::kRSrs ?
                       solver.linear(Linear::of(target)) :
                       Linear::of(parent.rotation.normalized().conjugated()) * Linear::of(target);
  solve::RotationScale rs;
  if (!solve::rotationScale(x, inherited(n, parent), n.rotation, n.scale, rs)) {
    return unsolvable();
  }
  writeVec3(out,
            fbx::kTranslation,
            solver.point(solve::translationOf(target)) - pivotOffset(n, rs.rotation, rs.scale));
  writeVec3(out, fbx::kScaling, rs.scale);
  writeRotation(n, readVec3(current, fbx::kRotation), rs.rotation, out);
  return Status();
}

Status Fbx::applyDelta(const EvalCtx &base,
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
  const double3 hint = readVec3(EvalCtx(nearTo), fbx::kRotation);
  out = base.snapshot();
  quatd rotation = n.rotation;
  double3 scale = n.scale;
  if (delta.kind == DeltaKind::Rotate) {
    /* Exact, except where the parent's scale applies after the node's rotation. */
    const quatd up = parent.rotation.normalized();
    const quatd turn = n.inherit == fbx::kRSrs ?
                           turnUnder(delta, solver, Linear::of(parent.world)) :
                           up.conjugated() * delta.rotation() * up;
    rotation = (turn * n.rotation).normalized();
    writeRotation(n, hint, rotation, out);
  }
  else if (delta.kind == DeltaKind::Scale) {
    if (mirrors(delta)) {
      const Linear m = mirrored(delta, before(n, parent) * Linear::of(rotation));
      const Linear x = n.inherit == fbx::kRSrs ?
                           solver.linear(m) :
                           Linear::of(parent.rotation.normalized().conjugated()) * m;
      bool turned = false;
      CDCC_TRY(takeMirror(x, rotation, scale, turned));
      if (turned) {
        writeRotation(n, hint, rotation, out);
      }
    }
    scale = scale * factorsAlong(delta, axesOf(n, rotation, parent));
    writeVec3(out, fbx::kScaling, scale);
  }
  const double3 offset = pivotOffset(n, n.rotation, n.scale);
  const double3 origin = litestl::math::transformPoint(parent.world, n.translation + offset);
  writeVec3(out,
            fbx::kTranslation,
            n.translation + solver.vector(shiftOf(delta, origin)) +
                (offset - pivotOffset(n, rotation, scale)));
  return Status();
}

TransformTypeInfo fbxInfo()
{
  TransformTypeInfo info;
  info.name = kFbxTransform;
  info.uiName = "FBX";
  const auto length = [&](const char *name) {
    info.channels.append(
        PropertyDef::vecProp(name, 3).subtype(PropSubtype::Distance).unit(UnitFamily::Length));
  };
  const auto euler = [&](const char *name) {
    info.channels.append(
        PropertyDef::vecProp(name, 3).subtype(PropSubtype::Euler).unit(UnitFamily::Angle));
  };
  length("lclTranslation");
  euler("lclRotation");
  info.channels.append(orderEnum("rotationOrder"));
  euler("preRotation");
  euler("postRotation");
  length("rotationOffset");
  length("rotationPivot");
  length("scalingOffset");
  length("scalingPivot");
  info.channels.append(
      PropertyDef::vecProp("lclScaling", 3).defaultValue(PropValue::ofVec3(1, 1, 1)));
  info.channels.append(PropertyDef::enumProp("inheritType")
                           .item("RRSS", fbx::kRrSs, "RrSs")
                           .item("RSRS", fbx::kRSrs, "RSrs")
                           .item("RRS", fbx::kRrs, "Rrs")
                           .defaultValue(PropValue::ofEnum(fbx::kRSrs)));
  return info;
}

litestl::binding::types::Struct<Fbx> *Fbx::defineBindings()
{
  namespace lbt = litestl::binding::types;
  static lbt::Struct<Fbx> *st = nullptr;
  if (!st) {
    litestl::alloc::PermanentGuard guard;
    st = new lbt::Struct<Fbx>("coredcc::models::Fbx", sizeof(Fbx));
    BIND_STRUCT_METHOD(st, evaluate, MARGS("ctx", "parent"));
    BIND_STRUCT_METHOD(st, setWorld, MARGS("current", "target", "parent", "mode", "out"));
    BIND_STRUCT_METHOD(st, applyDelta, MARGS("base", "nearTo", "delta", "parent", "out"));
    BIND_STRUCT_METHOD(st, pivot, MARGS("ctx", "parent"));
  }
  return st;
}

} // namespace coredcc::models
