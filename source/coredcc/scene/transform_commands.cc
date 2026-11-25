#include "coredcc/scene/commands.h"

#include "coredcc/command/block_commands.h"
#include "coredcc/command/block_edits.h"
#include "coredcc/command/executor.h"
#include "coredcc/scene/models.h"

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

using detail::SceneOf;
using HandleSet = litestl::util::Map<BlockHandle, int>;

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
  /* Ancestors that were passed. */
  int passed = 0;
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
    w.passed += w.found ? 0 : 1;
    p = lib.query<Transformable>(*p) ? parentOf(lib, *p) : Result<BlockHandle>(BlockHandle());
  }
  return p.ok() ? Result<Walk>(w) : Result<Walk>(p.status());
}

#define BIND_OBJECT_COMMAND(st)                                                          \
  BIND_STRUCT_METHOD(st, canRun, MARGS("ctx"));                                          \
  BIND_STRUCT_METHOD(st, exec, MARGS("ctx"));                                            \
  BIND_STRUCT_METHOD(st, undo, MARGS("ctx"));                                            \
  BIND_STRUCT_METHOD(st, redo, MARGS("ctx"));                                            \
  BIND_STRUCT_METHOD(st, memSize, MARGS());                                              \
  BIND_STRUCT_METHOD(st, discard, MARGS())

/* The parts that all of them share. */
struct ChangesCommand {
  ObjectChanges changes;

  void undo(Context &ctx)
  {
    changes.undo(ctx);
  }
  Status redo(Context &ctx)
  {
    return changes.redo(ctx);
  }
  uint64_t memSize() const
  {
    return sizeof(*this) + changes.memSize();
  }
  void discard() {}
};

void setApproximate(Context &ctx, int n)
{
  ctx.outputs()->set("approximate", PropValue::ofInt(n)).ignore();
}

/* ---- setParent ---- */

struct SetParent : ChangesCommand {
  Status canRun(Context &ctx) const
  {
    Result<Vector<BlockHandle>> objects = detail::selectedObjects(ctx);
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
           up->found ? detail::about(*s.lib, *parent, failed("is below a selected object")) :
                       Status();
  }
  Status exec(Context &ctx)
  {
    const SceneOf s = *detail::sceneOf(ctx);
    const Vector<BlockHandle> objects = *detail::selectedObjects(ctx);
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
    Vector<ChannelBlock> channels;
    for (BlockHandle h : objects) {
      if (!keep) {
        continue;
      }
      Result<ObjectRead> r = readObject(*s.lib, h);
      Result<TransformState> world = computeState(*s.lib, h, time);
      if (!r.ok() || !world.ok()) {
        return detail::about(*s.lib, h, r.ok() ? world.status() : r.status());
      }
      if (!r->type) {
        return detail::about(*s.lib, h, failed("its transform type isn't registered"));
      }
      Result<ChannelBlock> c = detail::solveKeeping(
          *r->type, EvalCtx(r->channels, time), world->world, parentState, approximate);
      if (!c.ok()) {
        return detail::about(*s.lib, h, c.status());
      }
      channels.append(std::move(*c));
    }
    for (int i = 0; i < int(objects.size()); i++) {
      ObjectChanges::Values v;
      v.channels = keep ? &channels[i] : nullptr;
      v.parent = &id;
      CDCC_TRY(changes.write(ctx, objects[i], v));
    }
    setApproximate(ctx, approximate);
    return Status();
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
    CDCC_TRY(detail::selectedObjects(ctx).status());
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
    const Vector<BlockHandle> objects = *detail::selectedObjects(ctx);
    const TransformType &type = *typeInput(ctx);
    const TypeId id = type.id();
    const bool keep = ctx.inputs()->get("keepWorld")->asBool();
    const double time = s.scene->time();
    int approximate = 0;
    Vector<ChannelBlock> channels;
    for (BlockHandle h : objects) {
      channels.append(type.defaults());
      if (!keep) {
        continue;
      }
      Result<TransformState> world = computeState(*s.lib, h, time);
      Result<TransformState> parent = computeParentState(*s.lib, h, time);
      if (!world.ok() || !parent.ok()) {
        return detail::about(*s.lib, h, world.ok() ? parent.status() : world.status());
      }
      Result<ChannelBlock> c = detail::solveKeeping(
          type, EvalCtx(type.defaults(), time), world->world, *parent, approximate);
      if (!c.ok()) {
        return detail::about(*s.lib, h, c.status());
      }
      channels[int(channels.size()) - 1] = std::move(*c);
    }
    for (int i = 0; i < int(objects.size()); i++) {
      ObjectChanges::Values v;
      v.channels = &channels[i];
      v.type = &id;
      CDCC_TRY(changes.write(ctx, objects[i], v));
    }
    setApproximate(ctx, approximate);
    return Status();
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
    Result<Vector<BlockHandle>> objects = detail::selectedObjects(ctx);
    if (!objects.ok()) {
      return objects.status();
    }
    for (BlockHandle h : *objects) {
      Result<ObjectRead> r = readObject(*ctx.library(), h);
      if (!r.ok()) {
        return r.status();
      }
      if (r->info.transformType != TypeId::of(kTrsTransform) ||
          r->channels.size() != trs::kCount)
      {
        return detail::about(*ctx.library(), h, failed("has no rotation mode"));
      }
    }
    return Status();
  }
  Status exec(Context &ctx)
  {
    const int32_t mode = int32_t(ctx.inputs()->get("mode")->asEnum());
    const Vector<BlockHandle> objects = *detail::selectedObjects(ctx);
    for (BlockHandle h : objects) {
      Result<ObjectRead> r = readObject(*ctx.library(), h);
      const ChannelBlock c = r.ok() ? models::withRotationMode(r->channels, mode) : ChannelBlock();
      ObjectChanges::Values v;
      v.channels = &c;
      CDCC_TRY(changes.write(ctx, h, v));
    }
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
    return detail::selectedObjects(ctx).status();
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
      Result<Walk> up =
          walkUp(lib, h, [&](BlockHandle x) { return gone.lookup_ptr(x) == nullptr; });
      Result<Walk> root = walkUp(lib, h, [](BlockHandle) { return false; });
      Result<TransformState> world = computeState(lib, h, time);
      if (!up.ok() || !root.ok() || !world.ok()) {
        return up.ok() ? (root.ok() ? world.status() : root.status()) : up.status();
      }
      Child c;
      c.object = h;
      c.parent = up->found;
      c.world = *world;
      c.depth = root->passed;
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
    const Vector<BlockHandle> objects = *detail::selectedObjects(ctx);
    const double time = s.scene->time();
    int approximate = 0;
    if (ctx.inputs()->get("keepChildrenWorld")->asBool()) {
      HandleSet gone;
      for (BlockHandle h : objects) {
        for (BlockHandle x : deletePreview(*s.lib, h)) {
          gone.add(x, 0);
        }
      }
      Vector<Child> found;
      CDCC_TRY(children(*s.lib, gone, time, found));
      for (const Child &c : found) {
        CDCC_TRY(reparent(ctx, c, time, approximate));
      }
    }
    const CommandDef *del = ctx.runtime().ext<CommandRegistry>().find(kDeleteBlockCommand);
    int64_t count = 0;
    for (BlockHandle h : objects) {
      const Datablock *b = s.lib->get(h);
      if (!b || b->state() != BlockState::Live) {
        continue; /* another one's deletion took it along */
      }
      PropertyBag in = del->newInputs();
      Status set = in.set("block", detail::pointerTo(*s.lib, h));
      Result<PropertyBag> out =
          set.ok() ? ctx.executor()->runSub(ctx, *del, std::move(in)) : Result<PropertyBag>(set);
      if (!out.ok()) {
        changes.undo(ctx); /* the executor undoes the deletions, not this */
        return out.status();
      }
      count += out->get("count")->asInt();
    }
    ctx.outputs()->set("count", PropValue::ofInt(count)).ignore();
    setApproximate(ctx, approximate);
    return Status();
  }
  /* Under the parents as they are by now: those before it have moved. */
  Status reparent(Context &ctx, const Child &c, double time, int &approximate)
  {
    const Library &lib = *ctx.library();
    Result<ObjectRead> r = readObject(lib, c.object);
    Result<TransformState> parent =
        c.parent ? computeState(lib, c.parent, time) : Result<TransformState>(TransformState());
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
          *r->type, EvalCtx(r->channels, time), c.world.world, *parent, approximate);
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
      .input(PropertyDef::vecProp("pivot", 3).subtype(PropSubtype::Distance))
      .input(PropertyDef::vecProp("vector", 3))
      .input(PropertyDef::vecProp("axis", 3).defaultValue(
          PropValue::ofComponents(PropValue::Type::Vec3, z)))
      .input(PropertyDef::floatProp("angle").subtype(PropSubtype::Angle).unit(UnitFamily::Angle));
  return def;
}

} // namespace

/* ---- applyDelta ---- */

Status ApplyDelta::canRun(Context &ctx) const
{
  return detail::selectedObjects(ctx).status();
}

Status ApplyDelta::exec(Context &ctx)
{
  const SceneOf s = *detail::sceneOf(ctx);
  const Vector<BlockHandle> objects = *detail::selectedObjects(ctx);
  const HandleSet selected = setOf(objects);
  time_ = s.scene->time();
  for (BlockHandle h : objects) {
    Result<Walk> up =
        walkUp(*s.lib, h, [&](BlockHandle p) { return selected.lookup_ptr(p) != nullptr; });
    if (!up.ok()) {
      return detail::about(*s.lib, h, up.status());
    }
    if (up->found) {
      continue;
    }
    Result<ObjectRead> r = readObject(*s.lib, h);
    Result<TransformState> parent = computeParentState(*s.lib, h, time_);
    if (!r.ok() || !parent.ok()) {
      return detail::about(*s.lib, h, r.ok() ? parent.status() : r.status());
    }
    if (!r->type) {
      return detail::about(*s.lib, h, failed("its transform type isn't registered"));
    }
    Base b;
    b.object = h;
    b.type = r->info.transformType;
    b.parent = *parent;
    bases_.append(b);
  }
  return update(ctx, deltaInputs(ctx));
}

Status ApplyDelta::update(Context &ctx, const TransformDelta &delta)
{
  const bool first = changes_.size() == 0;
  const Library &lib = *ctx.library();
  Vector<ChannelBlock> channels;
  for (int i = 0; i < int(bases_.size()); i++) {
    const Base &b = bases_[i];
    const TransformType *type = ctx.runtime().ext<TransformRegistry>().find(b.type);
    ChannelBlock base, last;
    if (first) {
      Result<ObjectRead> r = readObject(lib, b.object);
      if (!r.ok()) {
        return r.status();
      }
      base = r->channels;
      last = r->channels;
    }
    else {
      base = changes_.before(i);
      last = changes_.after(i);
    }
    Result<ChannelBlock> c = solveDelta(*type, EvalCtx(base, time_), last, delta, b.parent);
    if (!c.ok()) {
      return detail::about(lib, b.object, c.status());
    }
    channels.append(std::move(*c));
  }
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
  return Status();
}

lbt::Struct<ApplyDelta> *ApplyDelta::defineBindings()
{
  static lbt::Struct<ApplyDelta> *st = nullptr;
  if (!st) {
    st = new lbt::Struct<ApplyDelta>("coredcc::ApplyDeltaCommand", sizeof(ApplyDelta));
    BIND_OBJECT_COMMAND(st);
  }
  return st;
}

Status detail::registerTransformCommands(Runtime &rt)
{
  const auto keepWorld = [] {
    return PropertyDef::boolProp("keepWorld").defaultValue(PropValue::ofBool(true));
  };
  const auto approximate = [] { return PropertyDef::intProp("approximate"); };

  CommandDef parent(kSetParentCommand);
  parent.uiName("Set Parent")
      .input(PropertyDef::pointerProp("parent", StructId()))
      .input(keepWorld())
      .output(approximate());
  CDCC_TRY(addCommandOnce<SetParent>(rt, std::move(parent)));

  CommandDef type(kSetTransformTypeCommand);
  type.uiName("Set Transform Type")
      .input(PropertyDef::stringProp("type").defaultValue(PropValue::ofString(kTrsTransform)))
      .input(keepWorld())
      .output(approximate());
  CDCC_TRY(addCommandOnce<SetTransformType>(rt, std::move(type)));

  CommandDef mode(kSetRotationModeCommand);
  mode.uiName("Set Rotation Mode")
      .input(models::orderEnum("mode").item("QUATERNION", trs::kQuaternionMode));
  CDCC_TRY(addCommandOnce<SetRotationMode>(rt, std::move(mode)));

  CDCC_TRY(addCommandOnce<ApplyDelta>(rt, deltaDef(kApplyDeltaCommand, "Transform")));
  CommandDef drag = deltaDef(kDragDeltaCommand, "Transform (Drag)");
  drag.flags(CommandFlags::Interactive | CommandFlags::Private);
  CDCC_TRY(addCommandOnce<ApplyDelta>(rt, std::move(drag)));

  CommandDef del(kDeleteObjectsCommand);
  del.uiName("Delete Objects")
      .input(PropertyDef::boolProp("keepChildrenWorld").defaultValue(PropValue::ofBool(true)))
      .output(PropertyDef::intProp("count"))
      .output(approximate());
  return addCommandOnce<DeleteObjects>(rt, std::move(del));
}

} // namespace coredcc
