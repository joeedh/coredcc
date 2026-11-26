#pragma once

/* Shared by the command tests: test commands, a Rig block type, and Setup (a World
 * with an executor, a LinearHistory and the test commands registered). */

#include "../api/scene_fixture.h"

#include "coredcc/command/block_commands.h"
#include "coredcc/command/executor.h"
#include "coredcc/command/last_used.h"
#include "coredcc/command/literal.h"
#include "coredcc/command/set_property.h"
#include "coredcc/command/undo_strategies.h"

#include <cmath>
#include <string>

using namespace fixture;

/* ---- test commands ---- */

inline std::string g_log;

#define BIND_COMMAND_CORE(st)                                                            \
  BIND_STRUCT_METHOD(st, canRun, MARGS("ctx"));                                          \
  BIND_STRUCT_METHOD(st, exec, MARGS("ctx"));                                            \
  BIND_STRUCT_METHOD(st, undo, MARGS("ctx"));                                            \
  BIND_STRUCT_METHOD(st, byteSize, MARGS());                                             \
  BIND_STRUCT_METHOD(st, discard, MARGS())

inline int64_t inputInt(Context &ctx, const char *name)
{
  return ctx.inputs()->get(name)->asInt();
}

inline int64_t count(Context &ctx)
{
  return getValue(ctx, "tool.count")->asInt();
}

inline void setCount(Context &ctx, int64_t v)
{
  test_assert(setValue(ctx, "tool.count", PropValue::ofInt(v)).ok());
}

/* Adds `amount` to tool.count; fails for a negative amount. Holds `mem` bytes. */
struct AddCount {
  int64_t before = 0;
  int64_t mem = 0;
  Status canRun(Context &) const
  {
    return Status();
  }
  Status exec(Context &ctx)
  {
    const int64_t amount = inputInt(ctx, "amount");
    if (amount < 0) {
      return Status::error(StatusCode::InvalidArgument, "negative amount");
    }
    mem = inputInt(ctx, "mem");
    before = count(ctx);
    setCount(ctx, before + amount);
    ctx.outputs()->set("result", PropValue::ofInt(before + amount)).ignore();
    g_log += "exec" + std::to_string(amount) + ";";
    return Status();
  }
  void undo(Context &ctx)
  {
    setCount(ctx, before);
    g_log += "undo;";
  }
  uint64_t byteSize() const
  {
    return uint64_t(mem);
  }
  void discard()
  {
    g_log += "discard;";
  }
  static lbt::Struct<AddCount> *defineBindings()
  {
    auto *st = new lbt::Struct<AddCount>("test::AddCount", sizeof(AddCount));
    BIND_COMMAND_CORE(st);
    return st;
  }
};

/* Runs two AddCount sub-commands, then fails if `fail` (or leaves a sticky error). */
struct Parent {
  Status canRun(Context &) const
  {
    return Status();
  }
  Status exec(Context &ctx)
  {
    const CommandDef *add = ctx.runtime().ext<CommandRegistry>().find("test.add");
    for (int64_t amount : {1, 10}) {
      PropertyBag in = add->newInputs();
      test_assert(in.set("amount", PropValue::ofInt(amount)).ok());
      Result<PropertyBag> out = ctx.executor()->runSub(ctx, *add, std::move(in));
      if (!out.ok()) {
        return out.status();
      }
    }
    const int64_t mode = inputInt(ctx, "fail");
    if (mode == 1) {
      return Status::error(StatusCode::Internal, "parent failed");
    }
    if (mode == 2) {
      ctx.runtime().setStickyError(Status::error(StatusCode::Internal, "sticky"));
    }
    return Status();
  }
  void undo(Context &)
  {
    g_log += "parent-undo;";
  }
  uint64_t byteSize() const
  {
    return 0;
  }
  void discard()
  {
  }
  static lbt::Struct<Parent> *defineBindings()
  {
    auto *st = new lbt::Struct<Parent>("test::Parent", sizeof(Parent));
    BIND_COMMAND_CORE(st);
    return st;
  }
};

/* Refuses to run. */
struct Refuse {
  Status canRun(Context &) const
  {
    return Status::error(StatusCode::FailedPrecondition, "nothing selected");
  }
  Status exec(Context &)
  {
    g_log += "refuse-exec;";
    return Status();
  }
  void undo(Context &)
  {
  }
  uint64_t byteSize() const
  {
    return 0;
  }
  void discard()
  {
  }
  static lbt::Struct<Refuse> *defineBindings()
  {
    auto *st = new lbt::Struct<Refuse>("test::Refuse", sizeof(Refuse));
    BIND_COMMAND_CORE(st);
    return st;
  }
};

/* Adds to tool.count, folding runs with the same `key` into one step. */
struct Foldable {
  int64_t before = 0;
  int64_t key = 0;
  Status canRun(Context &) const
  {
    return Status();
  }
  Status exec(Context &ctx)
  {
    key = inputInt(ctx, "key");
    before = count(ctx);
    setCount(ctx, before + 1);
    return Status();
  }
  void undo(Context &ctx)
  {
    setCount(ctx, before);
  }
  uint64_t byteSize() const
  {
    return 0;
  }
  void discard()
  {
  }
  uint64_t foldKey() const
  {
    return uint64_t(key);
  }
  bool foldFrom(Command next)
  {
    return next.as<Foldable>() != nullptr; /* keeps our `before` */
  }
  static lbt::Struct<Foldable> *defineBindings()
  {
    auto *st = new lbt::Struct<Foldable>("test::Foldable", sizeof(Foldable));
    BIND_COMMAND_CORE(st);
    BIND_STRUCT_METHOD(st, foldKey, MARGS());
    BIND_STRUCT_METHOD(st, foldFrom, MARGS("next"));
    return st;
  }
};

/* An interactive drag of tool.strength: exec captures, the tool edits, undo and redo
 * swap the two values. */
struct Drag {
  double before = 0;
  double after = 0;
  Status canRun(Context &) const
  {
    return Status();
  }
  Status exec(Context &ctx)
  {
    before = getValue(ctx, "tool.strength")->asFloat();
    return Status();
  }
  void undo(Context &ctx)
  {
    after = getValue(ctx, "tool.strength")->asFloat();
    test_assert(setValue(ctx, "tool.strength", PropValue::ofFloat(before)).ok());
  }
  Status redo(Context &ctx)
  {
    return setValue(ctx, "tool.strength", PropValue::ofFloat(after));
  }
  uint64_t byteSize() const
  {
    return 16;
  }
  void discard()
  {
  }
  static lbt::Struct<Drag> *defineBindings()
  {
    auto *st = new lbt::Struct<Drag>("test::Drag", sizeof(Drag));
    BIND_COMMAND_CORE(st);
    BIND_STRUCT_METHOD(st, redo, MARGS("ctx"));
    return st;
  }
};

/* Creates a Mesh block per run, undoably. */
struct CreateMesh {
  CreatedBlocks created;
  Library *lib = nullptr;
  Status canRun(Context &) const
  {
    return Status();
  }
  Status exec(Context &ctx)
  {
    lib = ctx.library();
    Result<BlockHandle> h =
        lib->create(kMeshType, "made", litestl::alloc::New<Mesh>("Mesh"));
    if (!h.ok()) {
      return h.status();
    }
    created.add(*lib, *h);
    ctx.outputs()
        ->set("block", PropValue::ofInt(int64_t(lib->get(*h)->id().local.lo)))
        .ignore();
    return Status();
  }
  void undo(Context &)
  {
    created.undo(*lib);
  }
  Status redo(Context &)
  {
    return created.redo(*lib);
  }
  uint64_t byteSize() const
  {
    return 8;
  }
  void discard()
  {
    created.discard(*lib);
  }
  static lbt::Struct<CreateMesh> *defineBindings()
  {
    auto *st = new lbt::Struct<CreateMesh>("test::CreateMesh", sizeof(CreateMesh));
    BIND_COMMAND_CORE(st);
    BIND_STRUCT_METHOD(st, redo, MARGS("ctx"));
    return st;
  }
};

/* A block type with one non-null strong Ref: deleting its target cascades to it. */
struct Rig {
  RefCell target{RefKind::Strong};
};
struct RigProvider {
  void *data(const Datablock &b) const
  {
    return b.clientData();
  }
  void forEachRef(const Datablock &b, RefVisitor &v) const
  {
    v.visit(static_cast<Rig *>(b.clientData())->target);
  }
  void destroy(const Datablock &b)
  {
    litestl::alloc::Delete(static_cast<Rig *>(b.clientData()));
  }
  static lbt::Struct<RigProvider> *defineBindings()
  {
    auto *st = new lbt::Struct<RigProvider>("test::RigProvider", sizeof(RigProvider));
    BIND_STRUCT_METHOD(st, data, MARGS("block"));
    BIND_STRUCT_METHOD(st, forEachRef, MARGS("block", "visitor"));
    BIND_STRUCT_METHOD(st, destroy, MARGS("block"));
    return st;
  }
};
inline const TypeId kRigType = TypeId::of("test.Rig");

struct Setup {
  World w;
  LinearHistory history;
  CommandExecutor ex{&w.rt, &history};
  Context ctx = w.context();
  const CommandDef *add = nullptr;
  const CommandDef *parent = nullptr;
  const CommandDef *refuse = nullptr;
  const CommandDef *fold = nullptr;
  const CommandDef *drag = nullptr;

  Setup()
  {
    ctx.setExecutor(&ex);
    Runtime &rt = w.rt;
    add = *addNativeCommand<AddCount>(
        rt,
        CommandDef("test.add")
            .input(PropertyDef::intProp("amount").defaultValue(PropValue::ofInt(1)))
            .input(PropertyDef::intProp("mem"))
            .input(PropertyDef::enumProp("mode").item("FAST", 0).item("SLOW", 1))
            .output(PropertyDef::intProp("result")));
    parent = *addNativeCommand<Parent>(
        rt, CommandDef("test.parent").input(PropertyDef::intProp("fail")));
    refuse = *addNativeCommand<Refuse>(rt, CommandDef("test.refuse"));
    fold = *addNativeCommand<Foldable>(
        rt, CommandDef("test.fold").input(PropertyDef::intProp("key")));
    drag = *addNativeCommand<Drag>(
        rt, CommandDef("test.drag").flags(CommandFlags::Interactive));
    test_assert(
        addNativeCommand<CreateMesh>(
            rt, CommandDef("test.createMesh").output(PropertyDef::intProp("block")))
            .ok());
    test_assert(registerBlockCommands(rt).ok() && registerBlockCommands(rt).ok());
    g_log.clear();
  }

  Result<PropertyBag> runAdd(int64_t amount, int64_t mem = 0)
  {
    PropertyBag in = add->newInputs();
    test_assert(in.set("amount", PropValue::ofInt(amount)).ok());
    test_assert(in.set("mem", PropValue::ofInt(mem)).ok());
    return ex.run(ctx, *add, std::move(in));
  }
  Status runFold(int64_t key)
  {
    PropertyBag in = fold->newInputs();
    test_assert(in.set("key", PropValue::ofInt(key)).ok());
    return ex.run(ctx, *fold, std::move(in)).status();
  }
};

/* ---- tests ---- */
