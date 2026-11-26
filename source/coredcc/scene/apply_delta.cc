/* core.object.applyDelta and dragDelta. */
#include "coredcc/command/executor.h"
#include "coredcc/scene/object_command.h"
#include "coredcc/scene/state_cache.h"

#include "litestl/util/map.h"

#include <string>

namespace coredcc {

namespace lbt = litestl::binding::types;
using detail::HandleSet;
using detail::SceneOf;
using litestl::util::Vector;

namespace {

Status failed(const std::string &what)
{
  return Status::error(StatusCode::FailedPrecondition, what.c_str());
}

TransformDelta deltaInputs(Context &ctx)
{
  const PropertyBag &in = *ctx.inputs();
  const auto vec = [&in](const char *name) {
    const PropValue v = *in.get(name);
    return double3(v.component(0), v.component(1), v.component(2));
  };
  TransformDelta d;
  d.kind = DeltaKind(in.get("kind")->asEnum());
  const PropValue basis = *in.get("basis");
  for (int i = 0; i < 4; i++) {
    d.basis[i] = basis.component(i);
  }
  d.pivot = vec("pivot");
  d.vector = vec("vector");
  d.axis = vec("axis");
  d.angle = in.get("angle")->asFloat();
  return d;
}

CommandDef deltaDef(const char *name, const char *uiName)
{
  CommandDef def(name);
  const double z[] = {0, 0, 1};
  def.uiName(uiName)
      .input(PropertyDef::enumProp("kind")
                 .item("TRANSLATE", int64_t(DeltaKind::Translate))
                 .item("ROTATE", int64_t(DeltaKind::Rotate))
                 .item("SCALE", int64_t(DeltaKind::Scale)))
      .input(PropertyDef::quatProp("basis").defaultValue(PropValue::ofQuat(0, 0, 0, 1)))
      .input(PropertyDef::vecProp("pivot", 3)
                 .subtype(PropSubtype::Distance)
                 .unit(UnitFamily::Length))
      .input(PropertyDef::vecProp("vector", 3))
      .input(PropertyDef::vecProp("axis", 3).defaultValue(
          PropValue::ofComponents(PropValue::Type::Vec3, z)))
      .input(PropertyDef::floatProp("angle")
                 .subtype(PropSubtype::Angle)
                 .unit(UnitFamily::Angle))
      .input(detail::objectsInput())
      .input(PropertyDef::intProp("gesture").access(PropAccess::Optional |
                                                    PropAccess::NotRemembered))
      .output(PropertyDef::intProp("skipped"))
      .output(PropertyDef::intProp("locked"))
      .output(PropertyDef::intProp("driven"));
  return def;
}

} // namespace

/* ---- applyDelta ---- */

Status ApplyDelta::canRun(Context &ctx) const
{
  return detail::targetObjects(ctx).status();
}

Status ApplyDelta::exec(Context &ctx)
{
  const SceneOf s = *detail::sceneOf(ctx);
  const Vector<BlockHandle> objects = *detail::targetObjects(ctx);
  const TransformDelta delta = deltaInputs(ctx);
  time_ = s.scene->time();
  gesture_ = ctx.inputs()->get("gesture")->tryInt().value_or(0);
  detail::Skips skips;
  Vector<Base> usable;
  HandleSet moved;
  detail::StateCache states(*s.lib, time_);
  for (BlockHandle h : objects) {
    Result<Base> b = baseOf(*s.lib, states, h, delta);
    if (!b.ok()) {
      CDCC_TRY(skips.add(*s.lib, h, b.status()));
      continue;
    }
    usable.append(std::move(*b));
    moved.add(h, 0);
  }
  detail::Ancestry ancestry(*s.lib, moved);
  for (Base &b : usable) {
    Result<bool> below = ancestry.below(b.object);
    if (!below.ok()) {
      return detail::about(*s.lib, b.object, below.status());
    }
    if (!*below) {
      bases_.append(std::move(b));
    }
  }
  unusable_ = skips.count();
  CDCC_TRY(skips.outcome(int(bases_.size())));
  Status done = update(ctx, delta);
  skips.output(ctx);
  ctx.outputs()->set("locked", PropValue::ofInt(locked_)).ignore();
  ctx.outputs()->set("driven", PropValue::ofInt(driven_)).ignore();
  return done;
}

/* An object that the delta can be applied to. */
Result<ApplyDelta::Base> ApplyDelta::baseOf(const Library &lib,
                                            detail::StateCache &states,
                                            BlockHandle h,
                                            const TransformDelta &delta) const
{
  Result<ObjectRead> r = readObjectAt(lib, h, time_);
  Result<TransformState> parent = r.ok() ? states.stateAbove(h, *r) : r.status();
  if (!parent.ok()) {
    return parent.status();
  }
  if (!r->type) {
    return failed("its transform type isn't registered");
  }
  Base b;
  b.overrides = r->overrides;
  b.locks = r->info.locks;
  Result<ChannelBlock> c = solveDelta(
      *r->type, b.ctx(r->channels, time_), r->channels, delta, *parent, &b.fixed);
  if (!c.ok()) {
    return c.status();
  }
  b.object = h;
  b.type = r->info.transformType;
  b.parent = *parent;
  b.solved = std::move(*c);
  return b;
}

Status ApplyDelta::update(Context &ctx, const TransformDelta &delta)
{
  return updateWith(ctx, std::span<const TransformDelta>(&delta, 1));
}

Status ApplyDelta::update(Context &ctx, std::span<const TransformDelta> deltas)
{
  return deltas.size() == size_t(count())
             ? updateWith(ctx, deltas)
             : Status::error(
                   StatusCode::InvalidArgument,
                   "ApplyDelta::update: there must be one delta for each object");
}

Result<TransformState> ApplyDelta::startState(const Context &ctx, int i) const
{
  const TransformType *type = ctx.runtime().ext<TransformRegistry>().find(bases_[i].type);
  return evaluateTransform(
      *type, bases_[i].ctx(changes_.before(i), time_), bases_[i].parent);
}

Result<double3> ApplyDelta::startPivot(const Context &ctx, int i) const
{
  const TransformType *type = ctx.runtime().ext<TransformRegistry>().find(bases_[i].type);
  return transformPivot(
      *type, bases_[i].ctx(changes_.before(i), time_), bases_[i].parent);
}

uint64_t ApplyDelta::foldKey() const
{
  return uint64_t(gesture_) * 0x9E3779B97F4A7C15ull; /* 0 for 0 alone */
}

/* One that skipped others has the same objects and is folded too. */
bool ApplyDelta::foldFrom(Command next)
{
  const ApplyDelta *n = next.as<ApplyDelta>();
  return n && n->gesture_ == gesture_ && changes_.foldFrom(n->changes_);
}

Status ApplyDelta::updateWith(Context &ctx, std::span<const TransformDelta> deltas)
{
  const bool first = changes_.size() == 0;
  const Library &lib = *ctx.library();
  detail::Skips skips;
  Vector<ChannelBlock> channels;
  for (int i = 0; i < int(bases_.size()); i++) {
    Base &b = bases_[i];
    if (first) { /* exec's delta */
      channels.append(std::move(b.solved));
      b.solved = ChannelBlock();
      continue;
    }
    const TransformType *type = ctx.runtime().ext<TransformRegistry>().find(b.type);
    const TransformDelta &delta = deltas[deltas.size() == 1 ? 0 : size_t(i)];
    const ChannelBlock &last = changes_.after(i);
    Result<ChannelBlock> c = solveDelta(
        *type, b.ctx(changes_.before(i), time_), last, delta, b.parent, &b.fixed);
    if (!c.ok()) {
      CDCC_TRY(skips.add(lib, b.object, c.status()));
    }
    channels.append(c.ok() ? std::move(*c) : last);
  }
  CDCC_TRY(skips.outcome(int(bases_.size()) - skips.count()));
  Vector<ChannelBlock> previous;
  for (int i = 0; i < int(bases_.size()); i++) {
    if (first) {
      ObjectChanges::Values v;
      v.channels = &channels[i];
      CDCC_TRY(changes_.write(ctx, bases_[i].object, v));
      continue;
    }
    previous.append(changes_.after(i));
    if (Status s = changes_.rewrite(ctx, i, channels[i]); !s.ok()) {
      for (int k = 0; k <= i; k++) {
        changes_.rewrite(ctx, k, previous[k]).ignore();
      }
      return detail::about(lib, bases_[i].object, s);
    }
  }
  skipped_ = unusable_ + skips.count();
  locked_ = driven_ = 0;
  for (const Base &b : bases_) {
    locked_ += b.fixed.locked ? 1 : 0;
    driven_ += b.fixed.driven ? 1 : 0;
  }
  return Status();
}

lbt::Struct<ApplyDelta> *ApplyDelta::defineBindings()
{
  static lbt::Struct<ApplyDelta> *st = nullptr;
  if (!st) {
    st = new lbt::Struct<ApplyDelta>("coredcc::ApplyDeltaCommand", sizeof(ApplyDelta));
    BIND_OBJECT_COMMAND(st);
    BIND_STRUCT_METHOD(st, foldKey, MARGS());
    BIND_STRUCT_METHOD(st, foldFrom, MARGS("next"));
  }
  return st;
}

Status detail::registerDeltaCommands(Runtime &rt)
{
  CDCC_TRY(
      addCommandOnce<ApplyDelta>(rt, deltaDef(kApplyObjectDeltaCommand, "Transform")));
  CommandDef drag = deltaDef(kDragObjectDeltaCommand, "Transform (Drag)");
  drag.flags(CommandFlags::Interactive | CommandFlags::Private);
  return addCommandOnce<ApplyDelta>(rt, std::move(drag));
}

} // namespace coredcc
