#include "coredcc/scene/commands.h"

#include "coredcc/api/ref_text.h"
#include "coredcc/command/block_commands.h"
#include "coredcc/command/block_edits.h"
#include "coredcc/command/executor.h"
#include "coredcc/scene/kept_worlds.h"
#include "coredcc/scene/models.h"
#include "coredcc/scene/object_command.h"

#include "litestl/binding/binding.h"
#include "litestl/util/map.h"

#include <algorithm>
#include <string>

namespace coredcc {

namespace lbt = litestl::binding::types;
using litestl::util::Vector;

namespace detail {

Result<ChannelBlock> solveKeeping(const TransformType &type,
                                  const EvalCtx &current,
                                  const mat4d &world,
                                  const TransformState &parent,
                                  int &approximate)
{
  Result<ChannelBlock> c = solveWorld(type, current, world, parent, SetWorldMode::Exact);
  if (!c.ok() && c.status().code() == StatusCode::FailedPrecondition) {
    c = solveWorld(type, current, world, parent, SetWorldMode::Closest);
    approximate += c.ok() ? 1 : 0;
  }
  return c;
}

} // namespace detail

namespace {

using detail::ChangesCommand;
using detail::SceneOf;
using detail::HandleSet;

Status failed(const std::string &what)
{
  return Status::error(StatusCode::FailedPrecondition, what.c_str());
}

HandleSet setOf(const Vector<BlockHandle> &handles)
{
  HandleSet s;
  for (BlockHandle h : handles) {
    s.add(h, 0);
  }
  return s;
}

/* An object's Live parent, or null. */
Result<BlockHandle> parentOf(const Library &lib, BlockHandle h)
{
  Result<ObjectRead> r = readObject(lib, h);
  if (!r.ok()) {
    return r.status();
  }
  const BlockHandle p = r->info.parent.isNull() ? BlockHandle() : lib.find(r->info.parent);
  const Datablock *b = lib.get(p);
  return b && b->state() == BlockState::Live ? p : BlockHandle();
}

/* What an object's walk up came to. */
struct Walk {
  /* The nearest ancestor for which `stop` was true, or null. */
  BlockHandle found;
  /* The object is its own ancestor. */
  bool cycle = false;
};

/* An ancestor that isn't Transformable has no parent. */
template <class Stop> Result<Walk> walkUp(const Library &lib, BlockHandle h, Stop &&stop)
{
  Walk w;
  HandleSet seen;
  Result<BlockHandle> p = parentOf(lib, h);
  while (p.ok() && *p && seen.add(*p, 0)) {
    if (*p == h) {
      w.cycle = true;
      w.found = BlockHandle();
      return w;
    }
    if (!w.found && stop(*p)) {
      w.found = *p;
    }
    p = lib.query<Transformable>(*p) ? parentOf(lib, *p) : Result<BlockHandle>(BlockHandle());
  }
  return p.ok() ? Result<Walk>(w) : Result<Walk>(p.status());
}

void setApproximate(Context &ctx, int n)
{
  ctx.outputs()->set("approximate", PropValue::ofInt(n)).ignore();
}

/* Channels that keep an object at `state`; `otherwise` is set if what is below it may move. */
Result<ChannelBlock> solveKeeping(const TransformType &type,
                                  const EvalCtx &current,
                                  const TransformState &state,
                                  const TransformState &parent,
                                  int &approximate,
                                  bool &otherwise)
{
  const int before = approximate;
  Result<ChannelBlock> c =
      detail::solveKeeping(type, current, state.world, parent, approximate);
  otherwise = otherwise || approximate > before;
  if (c.ok()) {
    Result<TransformState> now = evaluateTransform(type, current.withChannels(*c), parent);
    otherwise = otherwise || !now.ok() || detail::inheritsOtherwise(state, *now);
  }
  return c;
}

/* ---- setParent ---- */

struct SetParent : ChangesCommand {
  Status canRun(Context &ctx) const
  {
    Result<Vector<BlockHandle>> objects = detail::targetObjects(ctx);
    if (!objects.ok()) {
      return objects.status();
    }
    const SceneOf s = *detail::sceneOf(ctx);
    Result<BlockHandle> parent = detail::blockOf(ctx, "parent");
    if (!parent.ok() || !*parent) {
      return parent.status();
    }
    if (!s.scene->contains(*s.lib, *parent)) {
      return detail::about(*s.lib, *parent, failed("isn't in the scene"));
    }
    const HandleSet selected = setOf(*objects);
    if (selected.lookup_ptr(*parent)) {
      return detail::about(*s.lib, *parent, failed("can't be its own parent"));
    }
    Result<Walk> up =
        walkUp(*s.lib, *parent, [&](BlockHandle p) { return selected.lookup_ptr(p) != nullptr; });
    return !up.ok()  ? up.status() :
           up->found ? detail::about(*s.lib, *parent, failed("is below one of the objects")) :
                       Status();
  }
  Status exec(Context &ctx)
  {
    const SceneOf s = *detail::sceneOf(ctx);
    const Vector<BlockHandle> objects = *detail::targetObjects(ctx);
    const BlockHandle parent = *detail::blockOf(ctx, "parent");
    const BlockId id = parent ? s.lib->get(parent)->id() : BlockId();
    const bool keep = ctx.inputs()->get("keepWorld")->asBool();
    const double time = s.scene->time();
    int approximate = 0;

    /* Everything is computed before anything is written. */
    TransformState parentState;
    if (keep && parent) {
      Result<TransformState> p = computeState(*s.lib, parent, time);
      if (!p.ok()) {
        return p.status();
      }
      parentState = *p;
    }
    detail::Skips skips;
    Vector<BlockHandle> usable;
    Vector<ChannelBlock> channels;
    bool otherwise = false;
    detail::StateCache states(*s.lib, time);
    for (BlockHandle h : objects) {
      Result<ChannelBlock> c =
          kept(*s.lib, states, h, keep, time, parentState, approximate, otherwise);
      if (!c.ok()) {
        CDCC_TRY(skips.add(*s.lib, h, c.status()));
        continue;
      }
      usable.append(h);
      channels.append(std::move(*c));
    }
    detail::KeptWorlds below;
    if (otherwise) {
      CDCC_TRY(below.remember(*s.lib, setOf(usable), time));
    }
    for (int i = 0; i < int(usable.size()); i++) {
      ObjectChanges::Values v;
      v.channels = keep ? &channels[i] : nullptr;
      v.parent = &id;
      CDCC_TRY(changes.write(ctx, usable[i], v));
    }
    setApproximate(ctx, approximate + below.moved(*s.lib));
    skips.output(ctx);
    return skips.outcome(int(usable.size()));
  }
  /* An object's channels under another parent, if its place is kept; an error if it can't have one. */
  static Result<ChannelBlock> kept(const Library &lib,
                                   detail::StateCache &states,
                                   BlockHandle h,
                                   bool keep,
                                   double time,
                                   const TransformState &parent,
                                   int &approximate,
                                   bool &otherwise)
  {
    Result<ObjectRead> r = readObjectAt(lib, h, time);
    if (!r.ok()) {
      return r.status();
    }
    if (!r->iface.hasSetParentId()) {
      return Status::error(StatusCode::Unsupported, "this object's parent can't be changed");
    }
    if (!keep) {
      return ChannelBlock();
    }
    Result<TransformState> above = states.stateAbove(h, *r);
    Result<TransformState> world = above.ok() ? states.stateUnder(*r, *above) : above;
    if (!world.ok()) {
      return world.status();
    }
    if (!r->type) {
      return failed("its transform type isn't registered");
    }
    return solveKeeping(
        *r->type, r->ctx(), *world, parent, approximate, otherwise);
  }
  static lbt::Struct<SetParent> *defineBindings()
  {
    auto *st = new lbt::Struct<SetParent>("coredcc::SetParentCommand", sizeof(SetParent));
    BIND_OBJECT_COMMAND(st);
    return st;
  }
};

/* ---- setTransformType ---- */

struct SetTransformType : ChangesCommand {
  static const TransformType *typeInput(Context &ctx)
  {
    const litestl::util::string name = ctx.inputs()->get("type")->asString();
    return ctx.runtime().ext<TransformRegistry>().find(std::string_view(name.c_str()));
  }
  Status canRun(Context &ctx) const
  {
    CDCC_TRY(detail::targetObjects(ctx).status());
    return typeInput(ctx) ?
               Status() :
               Status::error(StatusCode::NotFound,
                             ("no transform type is named '" +
                              std::string(ctx.inputs()->get("type")->asString().c_str()) + "'")
                                 .c_str());
  }
  Status exec(Context &ctx)
  {
    const SceneOf s = *detail::sceneOf(ctx);
    const Vector<BlockHandle> objects = *detail::targetObjects(ctx);
    const TransformType &type = *typeInput(ctx);
    const TypeId id = type.id();
    const bool keep = ctx.inputs()->get("keepWorld")->asBool();
    const double time = s.scene->time();
    int approximate = 0;
    detail::Skips skips;
    Vector<BlockHandle> usable;
    Vector<ChannelBlock> channels;
    bool otherwise = false;
    int done = 0;
    detail::StateCache states(*s.lib, time);
    for (BlockHandle h : objects) {
      Result<ObjectRead> r = readObjectAt(*s.lib, h, time);
      if (r.ok() && r->info.transformType == id && r->channels.size() == type.channelCount()) {
        done++; /* it has the type */
        continue;
      }
      Result<ChannelBlock> c =
          r.ok() ? converted(states, *r, h, type, keep, time, approximate, otherwise) : r.status();
      if (!c.ok()) {
        CDCC_TRY(skips.add(*s.lib, h, c.status()));
        continue;
      }
      usable.append(h);
      channels.append(std::move(*c));
    }
    detail::KeptWorlds below;
    if (otherwise) {
      CDCC_TRY(below.remember(*s.lib, setOf(usable), time));
    }
    for (int i = 0; i < int(usable.size()); i++) {
      ObjectChanges::Values v;
      v.channels = &channels[i];
      v.type = &id;
      CDCC_TRY(changes.write(ctx, usable[i], v));
    }
    setApproximate(ctx, approximate + below.moved(*s.lib));
    skips.output(ctx);
    return skips.outcome(done + int(usable.size()));
  }
  static Result<ChannelBlock> converted(detail::StateCache &states,
                                        const ObjectRead &r,
                                        BlockHandle h,
                                        const TransformType &type,
                                        bool keep,
                                        double time,
                                        int &approximate,
                                        bool &otherwise)
  {
    if (!r.iface.hasSetTransformTypeId()) {
      return Status::error(StatusCode::Unsupported, "this object's transform type can't be changed");
    }
    if (!keep) {
      return type.defaults();
    }
    Result<TransformState> parent = states.stateAbove(h, r);
    Result<TransformState> world = parent.ok() ? states.stateUnder(r, *parent) : parent;
    if (!world.ok()) {
      return world.status();
    }
    return solveKeeping(
        type, EvalCtx(type.defaults(), time), *world, *parent, approximate, otherwise);
  }
  static lbt::Struct<SetTransformType> *defineBindings()
  {
    auto *st = new lbt::Struct<SetTransformType>("coredcc::SetTransformTypeCommand",
                                                 sizeof(SetTransformType));
    BIND_OBJECT_COMMAND(st);
    return st;
  }
};

/* ---- setRotationMode ---- */

struct SetRotationMode : ChangesCommand {
  Status canRun(Context &ctx) const
  {
    Result<Vector<BlockHandle>> objects = detail::targetObjects(ctx);
    if (!objects.ok()) {
      return objects.status();
    }
    detail::Skips skips;
    int usable = 0;
    for (BlockHandle h : *objects) {
      Result<ObjectRead> r = readObject(*ctx.library(), h);
      const Status why = usableFor(r);
      usable += why.ok() ? 1 : 0;
      if (!why.ok()) {
        CDCC_TRY(skips.add(*ctx.library(), h, why));
      }
    }
    return skips.outcome(usable);
  }
  static Status usableFor(const Result<ObjectRead> &r)
  {
    return !r.ok() ? r.status() :
           r->info.transformType == TypeId::of(kTrsTransform) &&
                   r->channels.size() == trs::kCount ?
                     Status() :
                     failed("has no rotation mode");
  }
  Status exec(Context &ctx)
  {
    const int32_t mode = int32_t(ctx.inputs()->get("mode")->asEnum());
    const Vector<BlockHandle> objects = *detail::targetObjects(ctx);
    detail::Skips skips;
    for (BlockHandle h : objects) {
      Result<ObjectRead> r = readObject(*ctx.library(), h);
      if (const Status why = usableFor(r); !why.ok()) {
        CDCC_TRY(skips.add(*ctx.library(), h, why));
        continue;
      }
      const ChannelBlock c = models::withRotationMode(r->channels, mode);
      ObjectChanges::Values v;
      v.channels = &c;
      CDCC_TRY(changes.write(ctx, h, v));
    }
    skips.output(ctx);
    return Status();
  }
  static lbt::Struct<SetRotationMode> *defineBindings()
  {
    auto *st = new lbt::Struct<SetRotationMode>("coredcc::SetRotationModeCommand",
                                                sizeof(SetRotationMode));
    BIND_OBJECT_COMMAND(st);
    return st;
  }
};

/* ---- delete ---- */

struct DeleteObjects : ChangesCommand {
  /* A child that stays, and where it goes. */
  struct Child {
    BlockHandle object;
    BlockHandle parent;
    TransformState world;
    int depth = 0;
  };

  Status canRun(Context &ctx) const
  {
    return detail::targetObjects(ctx).status();
  }
  /* Children of the blocks in `gone` that aren't in it, parents first. */
  static Status children(const Library &lib,
                         const HandleSet &gone,
                         double time,
                         Vector<Child> &out)
  {
    Vector<BlockHandle> all;
    lib.forEach([&](BlockHandle h, const Datablock &b) {
      if (b.state() == BlockState::Live && !gone.lookup_ptr(h)) {
        all.append(h);
      }
    });
    detail::StateCache states(lib, time);
    detail::Ancestry ancestry(lib, gone);
    for (BlockHandle h : all) {
      const Transformable t = lib.query<Transformable>(h);
      if (!t || !t.hasSetParentId()) {
        continue;
      }
      Result<BlockHandle> p = parentOf(lib, h);
      if (!p.ok()) {
        return p.status();
      }
      if (!*p || !gone.lookup_ptr(*p)) {
        continue;
      }
      Result<detail::Ancestry::Place> place = ancestry.place(h);
      Result<detail::Ancestry::Place> above = ancestry.place(*p);
      Result<TransformState> world = states.state(h);
      if (!place.ok() || !above.ok() || !world.ok()) {
        return !place.ok() ? place.status() : !above.ok() ? above.status() : world.status();
      }
      Child c;
      c.object = h;
      c.parent = place->cycle ? BlockHandle() : above->outside;
      c.world = *world;
      c.depth = place->depth;
      out.append(c);
    }
    std::stable_sort(out.data(), out.data() + out.size(), [](const Child &a, const Child &b) {
      return a.depth < b.depth;
    });
    return Status();
  }
  Status exec(Context &ctx)
  {
    const SceneOf s = *detail::sceneOf(ctx);
    const Vector<BlockHandle> objects = *detail::targetObjects(ctx);
    const double time = s.scene->time();
    int approximate = 0;
    detail::KeptWorlds below;
    if (ctx.inputs()->get("keepChildrenWorld")->asBool()) {
      HandleSet gone;
      for (BlockHandle x : deletePreview(*s.lib, {objects.data(), objects.size()})) {
        gone.add(x, 0);
      }
      Vector<Child> found;
      CDCC_TRY(children(*s.lib, gone, time, found));
      HandleSet moving;
      for (const Child &c : found) {
        moving.add(c.object, 0);
      }
      if (found.size() > 0) {
        CDCC_TRY(below.remember(*s.lib, moving, time));
      }
      /* Parents first, so the state of a parent is final when it is asked for. Those on cycles come
       * before all others, and get no parent. */
      detail::StateCache parents(*s.lib, time);
      for (const Child &c : found) {
        CDCC_TRY(reparent(ctx, c, parents, time, approximate));
      }
    }
    const CommandDef *del = ctx.runtime().ext<CommandRegistry>().find(kDeleteBlockCommand);
    Vector<BlockId> ids;
    for (BlockHandle h : objects) {
      ids.append(s.lib->get(h)->id());
    }
    PropertyBag in = del->newInputs();
    Status set = in.set(
        "blocks",
        PropValue::ofString(formatBlockIds(ctx.runtime(), {ids.data(), ids.size()}).c_str()));
    Result<PropertyBag> out =
        set.ok() ? ctx.executor()->runSub(ctx, *del, std::move(in)) : Result<PropertyBag>(set);
    if (!out.ok()) {
      changes.undo(ctx); /* the executor undoes the deletion, not this */
      return out.status();
    }
    const int64_t count = out->get("count")->asInt();
    ctx.outputs()->set("count", PropValue::ofInt(count)).ignore();
    setApproximate(ctx, approximate + below.moved(*s.lib));
    return Status();
  }
  /* Under the parents as they are by now: those before it have moved. */
  Status reparent(Context &ctx,
                  const Child &c,
                  detail::StateCache &parents,
                  double time,
                  int &approximate)
  {
    const Library &lib = *ctx.library();
    Result<ObjectRead> r = readObjectAt(lib, c.object, time);
    Result<TransformState> parent =
        c.parent ? parents.parentState(c.parent) : Result<TransformState>(TransformState());
    const Status failure = r.ok() ? parent.status() : r.status();
    if (!failure.ok()) {
      changes.undo(ctx);
      return detail::about(lib, c.object, failure);
    }
    const BlockId id = c.parent ? lib.get(c.parent)->id() : BlockId();
    ObjectChanges::Values v;
    v.parent = &id;
    ChannelBlock channels;
    if (r->type) {
      Result<ChannelBlock> solved = detail::solveKeeping(
          *r->type, r->ctx(), c.world.world, *parent, approximate);
      if (solved.ok()) {
        channels = std::move(*solved);
        v.channels = &channels;
      }
      else if (solved.status().code() == StatusCode::FailedPrecondition ||
               solved.status().code() == StatusCode::Unsupported)
      {
        approximate++; /* a degenerate child doesn't stop the deletion */
      }
      else {
        changes.undo(ctx);
        return detail::about(lib, c.object, solved.status());
      }
    }
    return changes.write(ctx, c.object, v);
  }
  static lbt::Struct<DeleteObjects> *defineBindings()
  {
    auto *st = new lbt::Struct<DeleteObjects>("coredcc::DeleteObjectsCommand",
                                              sizeof(DeleteObjects));
    BIND_OBJECT_COMMAND(st);
    return st;
  }
};

} // namespace

Status detail::registerTransformCommands(Runtime &rt)
{
  const auto keepWorld = [] {
    return PropertyDef::boolProp("keepWorld").defaultValue(PropValue::ofBool(true));
  };
  const auto approximate = [] { return PropertyDef::intProp("approximate"); };
  const auto skipped = [] { return PropertyDef::intProp("skipped"); };

  CommandDef parent(kSetParentCommand);
  parent.uiName("Set Parent")
      .input(PropertyDef::pointerProp("parent", StructId()))
      .input(keepWorld())
      .input(detail::objectsInput())
      .output(approximate())
      .output(skipped());
  CDCC_TRY(addCommandOnce<SetParent>(rt, std::move(parent)));

  CommandDef type(kSetTransformTypeCommand);
  type.uiName("Set Transform Type")
      .input(PropertyDef::stringProp("type").defaultValue(PropValue::ofString(kTrsTransform)))
      .input(keepWorld())
      .input(detail::objectsInput())
      .output(approximate())
      .output(skipped());
  CDCC_TRY(addCommandOnce<SetTransformType>(rt, std::move(type)));

  CommandDef mode(kSetRotationModeCommand);
  mode.uiName("Set Rotation Mode")
      .input(models::orderEnum("mode").item("QUATERNION", trs::kQuaternionMode))
      .input(detail::objectsInput())
      .output(skipped());
  CDCC_TRY(addCommandOnce<SetRotationMode>(rt, std::move(mode)));

  CDCC_TRY(registerDeltaCommands(rt));
  CDCC_TRY(registerObjectCommands(rt));

  CommandDef del(kDeleteObjectsCommand);
  del.uiName("Delete Objects")
      .input(PropertyDef::boolProp("keepChildrenWorld").defaultValue(PropValue::ofBool(true)))
      .input(detail::objectsInput())
      .output(PropertyDef::intProp("count"))
      .output(approximate());
  return addCommandOnce<DeleteObjects>(rt, std::move(del));
}

} // namespace coredcc
