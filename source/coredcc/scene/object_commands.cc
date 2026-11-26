/* Commands that write what an object has: flags, locks, and world matrices. */
#include "coredcc/api/ref_text.h"
#include "coredcc/command/executor.h"
#include "coredcc/scene/channel_io.h"
#include "coredcc/scene/object_command.h"
#include "coredcc/scene/state_cache.h"

#include <algorithm>
#include <cstdlib>
#include <string>

namespace coredcc {

namespace lbt = litestl::binding::types;
using detail::ChangesCommand;
using detail::HandleSet;
using detail::SceneOf;
using litestl::util::Vector;

namespace {

/* Those two write one mask, of each object. */
struct MaskCommand : ChangesCommand {
  Status canRun(Context &ctx) const
  {
    return detail::targetObjects(ctx).status();
  }
  /* `values` says what to write of an object, or why nothing. */
  template <class Fn> Status run(Context &ctx, Fn &&values)
  {
    const Library &lib = *ctx.library();
    detail::Skips skips;
    int done = 0;
    const Vector<BlockHandle> objects = *detail::targetObjects(ctx);
    for (BlockHandle h : objects) {
      Result<ObjectRead> r = readObject(lib, h);
      ObjectChanges::Values v;
      const Status why = r.ok() ? values(*r, v) : r.status();
      if (!why.ok()) {
        CDCC_TRY(skips.add(lib, h, why));
        continue;
      }
      done++;
      if (v.flags || v.locks) {
        CDCC_TRY(changes.write(ctx, h, v));
      }
    }
    skips.output(ctx);
    return skips.outcome(done);
  }
};

/* ---- setFlags ---- */

struct SetFlags : MaskCommand {
  Status exec(Context &ctx)
  {
    const uint32_t flags = uint32_t(ctx.inputs()->get("flags")->asFlags());
    const bool on = ctx.inputs()->get("on")->asBool();
    ObjectFlags out = ObjectFlags::None;
    return run(ctx, [&](const ObjectRead &r, ObjectChanges::Values &v) {
      if (!r.iface.hasSetFlags()) {
        return Status::error(StatusCode::Unsupported,
                             "this object's flags can't be changed");
      }
      out = ObjectFlags(on ? r.info.flags | flags : r.info.flags & ~flags);
      v.flags = uint32_t(out) == r.info.flags ? nullptr : &out;
      return Status();
    });
  }
  static lbt::Struct<SetFlags> *defineBindings()
  {
    auto *st = new lbt::Struct<SetFlags>("coredcc::SetFlagsCommand", sizeof(SetFlags));
    BIND_OBJECT_COMMAND(st);
    return st;
  }
};

/* ---- setLocks ---- */

struct SetLocks : MaskCommand {
  Status exec(Context &ctx)
  {
    const litestl::util::string names = ctx.inputs()->get("channels")->asString();
    const bool on = ctx.inputs()->get("on")->asBool();
    uint64_t out = 0;
    return run(ctx, [&](const ObjectRead &r, ObjectChanges::Values &v) {
      if (!r.iface.hasSetLocks()) {
        return Status::error(StatusCode::Unsupported,
                             "this object's channels can't be locked");
      }
      if (!r.type) {
        return Status::error(StatusCode::FailedPrecondition,
                             "its transform type isn't registered");
      }
      std::string unknown;
      uint64_t mask = detail::lockMask(*r.type, names.c_str(), &unknown);
      if (names.size() == 0) {
        const int32_t n = r.type->channelCount();
        mask = n < EvalContext::kMaxLocks ? (uint64_t(1) << n) - 1 : ~uint64_t(0);
      }
      if (!unknown.empty()) {
        return Status::error(StatusCode::FailedPrecondition,
                             ("no channel '" + unknown + "' that can be locked").c_str());
      }
      out = on ? r.info.locks | mask : r.info.locks & ~mask;
      v.locks = out == r.info.locks ? nullptr : &out;
      return Status();
    });
  }
  static lbt::Struct<SetLocks> *defineBindings()
  {
    auto *st = new lbt::Struct<SetLocks>("coredcc::SetLocksCommand", sizeof(SetLocks));
    BIND_OBJECT_COMMAND(st);
    return st;
  }
};

/* ---- setWorld ---- */

/* The numbers of a text, with spaces, commas or semicolons between them. */
bool parseNumbers(const char *text, Vector<double> &out)
{
  const auto between = [](char c) {
    return c == ' ' || c == ',' || c == ';' || c == '\n';
  };
  while (*text) {
    if (between(*text)) {
      text++;
      continue;
    }
    char *end = nullptr;
    out.append(std::strtod(text, &end));
    if (end == text || (*end && !between(*end))) {
      return false;
    }
    text = end;
  }
  return true;
}

struct SetWorld : ChangesCommand {
  struct Target {
    BlockHandle object;
    mat4d matrix;
    int depth = 0;
  };

  Status canRun(Context &ctx) const
  {
    return targets(ctx).status();
  }
  static Status invalid(const char *what)
  {
    return Status::error(StatusCode::InvalidArgument, what);
  }
  /* Parents first. */
  static Result<Vector<Target>> targets(Context &ctx)
  {
    Result<Vector<BlockHandle>> objects = detail::objectsOf(ctx);
    if (!objects.ok()) {
      return objects.status();
    }
    Vector<double> numbers;
    const litestl::util::string text = ctx.inputs()->get("matrices")->asString();
    if (!parseNumbers(text.c_str(), numbers)) {
      return invalid("matrices: not numbers");
    }
    const litestl::util::string names = ctx.inputs()->get("objects")->asString();
    Result<Vector<BlockId>> ids = parseBlockIds(ctx.runtime(), names.c_str());
    if (!ids.ok() || ids->size() != objects->size()) {
      return invalid("objects: one is named twice");
    }
    if (objects->size() == 0 || numbers.size() != objects->size() * 16) {
      return invalid("matrices: there must be 16 numbers for each of the objects");
    }
    const Library &lib = *ctx.library();
    HandleSet set;
    for (BlockHandle h : *objects) {
      set.add(h, 0);
    }
    detail::Ancestry ancestry(lib, set);
    Vector<Target> out;
    for (int i = 0; i < int(objects->size()); i++) {
      Target t;
      t.object = (*objects)[i];
      double *m = t.matrix;
      for (int k = 0; k < 16; k++) {
        m[k] = numbers[i * 16 + k];
      }
      Result<detail::Ancestry::Place> place = ancestry.place(t.object);
      if (!place.ok()) {
        return detail::about(lib, t.object, place.status());
      }
      t.depth = place->parentDepth;
      out.append(t);
    }
    std::stable_sort(out.data(),
                     out.data() + out.size(),
                     [](const Target &a, const Target &b) { return a.depth < b.depth; });
    return out;
  }
  Status exec(Context &ctx)
  {
    const SceneOf s = *detail::sceneOf(ctx);
    Result<Vector<Target>> list = targets(ctx);
    if (!list.ok()) {
      return list.status();
    }
    const bool closest =
        ctx.inputs()->get("mode")->asEnum() == int64_t(SetWorldMode::Closest);
    const double time = s.scene->time();
    detail::Skips skips;
    int done = 0, approximate = 0;
    detail::StateCache parents(*s.lib, time);
    for (const Target &t : *list) {
      Result<ChannelBlock> c = solved(*s.lib, t, closest, time, parents, approximate);
      if (!c.ok()) {
        if (Status why = skips.add(*s.lib, t.object, c.status()); !why.ok()) {
          changes.undo(ctx);
          return why;
        }
        continue;
      }
      ObjectChanges::Values v;
      v.channels = &*c;
      CDCC_TRY(changes.write(ctx, t.object, v));
      done++;
    }
    ctx.outputs()->set("approximate", PropValue::ofInt(approximate)).ignore();
    skips.output(ctx);
    return skips.outcome(done);
  }
  /* Its parent was written before. */
  static Result<ChannelBlock> solved(const Library &lib,
                                     const Target &t,
                                     bool closest,
                                     double time,
                                     detail::StateCache &parents,
                                     int &approximate)
  {
    Result<ObjectRead> r = readObjectAt(lib, t.object, time);
    Result<TransformState> parent =
        r.ok() ? parents.stateAbove(t.object, *r) : r.status();
    if (!parent.ok()) {
      return parent.status();
    }
    if (!r->type) {
      return Status::error(StatusCode::FailedPrecondition,
                           "its transform type isn't registered");
    }
    Result<ChannelBlock> c =
        solveWorld(*r->type, r->solveContext(), t.matrix, *parent, SetWorldMode::Exact);
    if (!c.ok() && closest && c.status().code() == StatusCode::FailedPrecondition) {
      c = solveWorld(
          *r->type, r->solveContext(), t.matrix, *parent, SetWorldMode::Closest);
      approximate += c.ok() ? 1 : 0;
    }
    return c;
  }
  static lbt::Struct<SetWorld> *defineBindings()
  {
    auto *st = new lbt::Struct<SetWorld>("coredcc::SetWorldCommand", sizeof(SetWorld));
    BIND_OBJECT_COMMAND(st);
    return st;
  }
};

} // namespace

Status detail::registerObjectCommands(Runtime &rt)
{
  const auto on = [] {
    return PropertyDef::boolProp("on").defaultValue(PropValue::ofBool(true));
  };
  CommandDef flags(kSetObjectFlagsCommand);
  flags.uiName("Set Object Flags")
      .input(PropertyDef::flagsProp("flags")
                 .item("HIDDEN", int64_t(ObjectFlags::Hidden))
                 .item("UNSELECTABLE", int64_t(ObjectFlags::Unselectable)))
      .input(on())
      .input(objectsInput())
      .output(PropertyDef::intProp("skipped"));
  CDCC_TRY(addCommandOnce<SetFlags>(rt, std::move(flags)));

  CommandDef locks(kSetObjectLocksCommand);
  locks.uiName("Lock Channels")
      .input(PropertyDef::stringProp("channels"))
      .input(on())
      .input(objectsInput())
      .output(PropertyDef::intProp("skipped"));
  CDCC_TRY(addCommandOnce<SetLocks>(rt, std::move(locks)));

  CommandDef world(kSetObjectWorldCommand);
  world.uiName("Set World Matrix")
      .input(PropertyDef::stringProp("objects").access(PropAccess::NotRemembered))
      .input(PropertyDef::stringProp("matrices").access(PropAccess::NotRemembered))
      .input(PropertyDef::enumProp("mode")
                 .item("CLOSEST", int64_t(SetWorldMode::Closest))
                 .item("EXACT", int64_t(SetWorldMode::Exact)))
      .output(PropertyDef::intProp("approximate"))
      .output(PropertyDef::intProp("skipped"));
  return addCommandOnce<SetWorld>(rt, std::move(world));
}

} // namespace coredcc
