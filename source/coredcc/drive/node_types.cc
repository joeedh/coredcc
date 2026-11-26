#include "coredcc/drive/node_types.h"

#include "coredcc/api/ref_text.h"
#include "coredcc/base/invalidate.h"
#include "coredcc/scene/capabilities.h"
#include "coredcc/scene/object.h"
#include "coredcc/scene/overrides.h"
#include "coredcc/scene/transform_registry.h"

#include <string>

namespace coredcc::drive {

namespace {

constexpr DriveKind kKinds[] = {DriveKind::Bool,
                                DriveKind::Int,
                                DriveKind::Float,
                                DriveKind::Vec2,
                                DriveKind::Vec3,
                                DriveKind::Vec4,
                                DriveKind::Quat};

GraphValue::Type graphType(DriveKind k)
{
  switch (k) {
  case DriveKind::Bool:
    return GraphValue::Type::Bool;
  case DriveKind::Int:
    return GraphValue::Type::Int;
  case DriveKind::Float:
    return GraphValue::Type::Float;
  case DriveKind::Vec2:
    return GraphValue::Type::Vec2;
  case DriveKind::Vec3:
    return GraphValue::Type::Vec3;
  case DriveKind::Vec4:
    return GraphValue::Type::Vec4;
  case DriveKind::Quat:
    return GraphValue::Type::Quat;
  default:
    return GraphValue::Type::None;
  }
}

/* An input of the kind, without a meaningful literal: unlinked, a transform node's
 * channel inputs mean the stored value. */
PropertyDef inputOf(DriveKind k, std::string_view name)
{
  switch (k) {
  case DriveKind::Bool:
    return PropertyDef::boolProp(name);
  case DriveKind::Int:
    return PropertyDef::intProp(name);
  case DriveKind::Vec2:
    return PropertyDef::vecProp(name, 2);
  case DriveKind::Vec3:
    return PropertyDef::vecProp(name, 3);
  case DriveKind::Vec4:
    return PropertyDef::vecProp(name, 4);
  case DriveKind::Quat:
    return PropertyDef::quatProp(name);
  default:
    return PropertyDef::floatProp(name);
  }
}

PropertyDef objectParam()
{
  return PropertyDef::pointerProp("object", StructId::of(kObjectStruct));
}

template <Status (*Fn)(NodeExecContext &)>
Status addType(Runtime &rt, NodeTypeDef def, const char *kernelName)
{
  Result<Owned<GraphKernel>> k = makeKernel<FnKernel<Fn>>(rt, kernelName);
  CDCC_TRY(k.status());
  def.kernel(std::move(k).value());
  return rt.ext<NodeTypeRegistry>().add(std::move(def)).status();
}

/* ---- kernels ---- */

Status timeKernel(NodeExecContext &ctx)
{
  return ctx.setOutput(0, GraphValue::ofFloat(ctx.input(0).asFloat()));
}

Status passKernel(NodeExecContext &ctx)
{
  return ctx.setOutput(0, ctx.input(0));
}

Status stateKernel(NodeExecContext &ctx)
{
  const TransformState *s = ctx.input(0).payload<TransformState>();
  if (!s) {
    return Status::error(StatusCode::NotFound,
                         "scene.object.state: the object isn't evaluated here (it isn't "
                         "there or isn't Transformable, or no SceneDrivers evaluates the "
                         "graph)");
  }
  const double *m = s->world;
  const quatd &q = s->rotation;
  CDCC_TRY(ctx.setOutput("world", GraphValue::ofComponents(GraphValue::Type::Mat4, m)));
  CDCC_TRY(ctx.setOutput("location", GraphValue::ofVec3(m[12], m[13], m[14])));
  CDCC_TRY(ctx.setOutput("rotation", GraphValue::ofQuat(q[0], q[1], q[2], q[3])));
  CDCC_TRY(ctx.setOutput(
      "scale", GraphValue::ofVec3(s->localScale[0], s->localScale[1], s->localScale[2])));
  return ctx.setOutput("status", GraphValue::ofInt(ctx.input(1).asInt()));
}

Status setState(NodeExecContext &ctx, const TransformState &s, int64_t status)
{
  CDCC_TRY(ctx.setOutput(0, GraphValue::ofPayload(makePayload(s))));
  return ctx.setOutput(1, GraphValue::ofInt(status));
}

/* What the direct pass computes for the object (evaluateObject), from the same channels,
 * overrides, time and parent: the two agree bitwise. */
Status transformKernel(NodeExecContext &ctx)
{
  Runtime &rt = ctx.runtime();
  const std::string_view name(ctx.type().name.c_str());
  const bool missing = name == kMissingTransform;
  const BlockUse use = ctx.paramBlock("object", kAllAspects & ~kDrivenAspect);
  const Transformable tr =
      use.handle ? ctx.queryBlock<Transformable>(use.handle) : Transformable();
  if (!use.data || !tr) {
    return Status::error(StatusCode::NotFound,
                         "the object isn't there, or isn't "
                         "Transformable");
  }
  /* Why SceneDrivers fails the object: what reads it fails too. */
  if (const PropValue *why = ctx.param("error"); why && why->asString().size() > 0) {
    return Status::error(StatusCode::FailedPrecondition, why->asString().c_str());
  }
  const TransformState *given = ctx.input(kParentInput).payload<TransformState>();
  const TransformState parent = given ? *given : TransformState();
  ObjectInfo info;
  ChannelBlock channels;
  tr.read(info, channels);
  if (rt.hasStickyError()) {
    (void)rt.takeStickyError(); /* the direct pass reports it */
    return setState(ctx, parent, int64_t(EvalStatus::ModelFailed));
  }
  const PropValue *expected = ctx.param("parentId");
  const TransformType *type = rt.ext<TransformRegistry>().find(info.transformType);
  const bool sameType =
      missing ? !type
              : type && name.substr(strlen(kTransformPrefix)) == type->name().c_str();
  if (!sameType || !expected ||
      formatBlockId(rt, info.parent) != expected->asString().c_str())
  {
    return setState(ctx, parent, kStaleStatus);
  }

  const double time = ctx.input(kTimeInput).asFloat();
  TransformState out;
  Status error;
  EvalStatus status;
  if (missing) {
    status = evaluateObject(rt, nullptr, EvalCtx(channels, time), parent, out, error);
  } else {
    ChannelOverrides overrides;
    overrides.reset(type->channelCount());
    /* SceneDrivers' inner provider, asked first as overridesOf does; it notifies the
     * objects whose answers change, so it needn't be a dependency. */
    if (auto *inner = static_cast<OverrideProvider *>(ctx.userData())) {
      const Status s =
          ctx.library()
              ? inner->overridesOf(*ctx.library(), use.handle, *type, time, overrides)
              : Status::error(StatusCode::Internal, "no Library");
      if (!s.ok()) { /* a failed driver: what reads it fails too */
        return Status::error(StatusCode::FailedPrecondition,
                             (std::string("its drivers: ") + s.message()).c_str());
      }
    }
    int32_t at = 0, input = kFirstChannelInput;
    double flat[4];
    for (const PropertyDef &def : type->channelDefs()) {
      const DriveKind k = kindOf(def.kind());
      if (ctx.isLinked(input) && flatten(ctx.input(input), k, flat)) {
        for (int c = 0; c < kindChannels(k); c++) {
          overrides.set(at + c, flat[c]);
        }
      }
      at += kindChannels(k);
      input++;
    }
    const EvalCtx ectx = overrides.any()
                             ? EvalCtx(channels, time, overrides.values, overrides.mask)
                             : EvalCtx(channels, time);
    status = evaluateObject(rt, type, ectx, parent, out, error);
  }
  const PropValue *cycle = ctx.param("cycle");
  if (status == EvalStatus::Ok && cycle && cycle->asBool()) {
    status = EvalStatus::Cycle;
  }
  return setState(ctx, out, int64_t(status));
}

NodeTypeDef transformDef(std::string_view name)
{
  NodeTypeDef d(name);
  d.payloadInput("parent", kStatePayload)
      .input(PropertyDef::floatProp("time"))
      .param(objectParam(), true)
      .param(PropertyDef::stringProp("parentId"))
      .param(PropertyDef::boolProp("cycle"))
      .param(PropertyDef::stringProp("error"))
      .output("state", kStatePayload)
      .output("status", "int");
  return d;
}

constexpr const char *kTransformKernel = "scene.transform#kernel";

} // namespace

/* ---- kinds ---- */

DriveKind kindOf(PropKind kind)
{
  switch (kind) {
  case PropKind::Bool:
    return DriveKind::Bool;
  case PropKind::Int:
  case PropKind::Enum:
    return DriveKind::Int;
  case PropKind::Float:
    return DriveKind::Float;
  case PropKind::Vec2:
    return DriveKind::Vec2;
  case PropKind::Vec3:
    return DriveKind::Vec3;
  case PropKind::Vec4:
    return DriveKind::Vec4;
  case PropKind::Quat:
    return DriveKind::Quat;
  default:
    return DriveKind::None;
  }
}

const char *kindName(DriveKind kind)
{
  switch (kind) {
  case DriveKind::Bool:
    return "bool";
  case DriveKind::Int:
    return "int";
  case DriveKind::Float:
    return "float";
  case DriveKind::Vec2:
    return "vec2";
  case DriveKind::Vec3:
    return "vec3";
  case DriveKind::Vec4:
    return "vec4";
  case DriveKind::Quat:
    return "quat";
  default:
    return "";
  }
}

DriveKind kindOfDriveType(std::string_view typeName)
{
  const std::string_view prefix(kDrivePrefix);
  if (typeName.substr(0, prefix.size()) != prefix) {
    return DriveKind::None;
  }
  for (DriveKind k : kKinds) {
    if (typeName.substr(prefix.size()) == kindName(k)) {
      return k;
    }
  }
  return DriveKind::None;
}

int kindChannels(DriveKind kind)
{
  switch (kind) {
  case DriveKind::Vec2:
    return 2;
  case DriveKind::Vec3:
    return 3;
  case DriveKind::Vec4:
  case DriveKind::Quat:
    return 4;
  case DriveKind::None:
    return 0;
  default:
    return 1;
  }
}

bool flatten(const GraphValue &v, DriveKind kind, double *out)
{
  if (kind == DriveKind::None || v.type() != graphType(kind)) {
    return false;
  }
  switch (kind) {
  case DriveKind::Bool:
    out[0] = v.asBool() ? 1.0 : 0.0;
    return true;
  case DriveKind::Int:
    out[0] = double(v.asInt());
    return true;
  case DriveKind::Float:
    out[0] = v.asFloat();
    return true;
  default:
    for (int c = 0; c < kindChannels(kind); c++) {
      out[c] = v.components()[c];
    }
    return true;
  }
}

/* ---- registering ---- */

Status registerNodeTypes(Runtime &rt)
{
  CDCC_TRY(rt.ext<SocketTypeRegistry>().addPayloadType<TransformState>().status());

  NodeTypeDef time(kTimeNode);
  time.uiName("Scene Time")
      .flags(NodeTypeFlags::InputAdapter)
      .input(PropertyDef::floatProp("time"))
      .output("seconds", "float");
  CDCC_TRY(addType<timeKernel>(rt, std::move(time), "scene.time#kernel"));

  NodeTypeDef state(kStateNode);
  state.uiName("Object State")
      .flags(NodeTypeFlags::InputAdapter)
      .payloadInput("state", kStatePayload)
      .input(PropertyDef::intProp("status"))
      .param(objectParam(), true)
      .output("world", "mat4")
      .output("location", "vec3")
      .output("rotation", "quat")
      .output("scale", "vec3")
      .output("status", "int");
  CDCC_TRY(addType<stateKernel>(rt, std::move(state), "scene.object.state#kernel"));

  for (DriveKind k : kKinds) {
    const std::string name = std::string(kDrivePrefix) + kindName(k);
    NodeTypeDef d(name);
    d.uiName("Drive Channel")
        .flags(NodeTypeFlags::OutputAdapter)
        .input(inputOf(k, "value"))
        .param(objectParam(), true)
        .param(PropertyDef::stringProp("channel"))
        .output("value", kindName(k));
    CDCC_TRY(addType<passKernel>(rt, std::move(d), "scene.drive#kernel"));
  }

  NodeTypeDef missing = transformDef(kMissingTransform);
  missing.uiName("Transform (missing type)");
  return addType<transformKernel>(rt, std::move(missing), kTransformKernel);
}

Result<const NodeType *> transformNodeType(Runtime &rt, const TransformType *type)
{
  NodeTypeRegistry &reg = rt.ext<NodeTypeRegistry>();
  const std::string name =
      type ? std::string(kTransformPrefix) + type->name().c_str() : kMissingTransform;
  if (const NodeType *t = reg.find(name)) {
    return t;
  }
  NodeTypeDef d = transformDef(name);
  d.uiName("Transform");
  for (const PropertyDef &def : type->channelDefs()) {
    const DriveKind k = kindOf(def.kind());
    if (k == DriveKind::None) {
      return Status::error(
          StatusCode::Unsupported,
          ("a channel of '" + std::string(type->name().c_str()) + "' can't be an input")
              .c_str());
    }
    d.input(inputOf(k, std::string(kChannelPrefix) + def.apiName().c_str()));
  }
  Result<Owned<GraphKernel>> k =
      makeKernel<FnKernel<transformKernel>>(rt, kTransformKernel);
  CDCC_TRY(k.status());
  d.kernel(std::move(k).value());
  return reg.add(std::move(d));
}

} // namespace coredcc::drive
