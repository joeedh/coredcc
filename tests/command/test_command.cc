#include "../api/scene_fixture.h"

#include "coredcc/command/block_commands.h"
#include "coredcc/command/executor.h"
#include "coredcc/command/last_used.h"
#include "coredcc/command/literal.h"
#include "coredcc/command/set_property.h"
#include "coredcc/command/undo_strategies.h"

#include <cmath>
#include <string>

test_init;

using namespace fixture;

/* ---- test commands ---- */

static std::string g_log;

#define BIND_COMMAND_CORE(st)                                                            \
  BIND_STRUCT_METHOD(st, canRun, MARGS("ctx"));                                          \
  BIND_STRUCT_METHOD(st, exec, MARGS("ctx"));                                            \
  BIND_STRUCT_METHOD(st, undo, MARGS("ctx"));                                            \
  BIND_STRUCT_METHOD(st, memSize, MARGS());                                              \
  BIND_STRUCT_METHOD(st, discard, MARGS())

static int64_t inputInt(Context &ctx, const char *name)
{
  return ctx.inputs()->get(name)->asInt();
}

static int64_t count(Context &ctx)
{
  return getValue(ctx, "tool.count")->asInt();
}

static void setCount(Context &ctx, int64_t v)
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
  uint64_t memSize() const
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
  uint64_t memSize() const
  {
    return 0;
  }
  void discard() {}
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
  void undo(Context &) {}
  uint64_t memSize() const
  {
    return 0;
  }
  void discard() {}
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
  uint64_t memSize() const
  {
    return 0;
  }
  void discard() {}
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
  uint64_t memSize() const
  {
    return 16;
  }
  void discard() {}
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
    Result<BlockHandle> h = lib->create(kMeshType, "made", litestl::alloc::New<Mesh>("Mesh"));
    if (!h.ok()) {
      return h.status();
    }
    created.add(*h);
    ctx.outputs()->set("block", PropValue::ofInt(int64_t(lib->get(*h)->id().local.lo))).ignore();
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
  uint64_t memSize() const
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
  void foreachRef(const Datablock &b, RefVisitor &v) const
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
    BIND_STRUCT_METHOD(st, foreachRef, MARGS("block", "visitor"));
    BIND_STRUCT_METHOD(st, destroy, MARGS("block"));
    return st;
  }
};
static const TypeId kRigType = TypeId::of("test.Rig");

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
        rt, CommandDef("test.add")
                .input(PropertyDef::intProp("amount").defaultValue(PropValue::ofInt(1)))
                .input(PropertyDef::intProp("mem"))
                .input(PropertyDef::enumProp("mode").item("FAST", 0).item("SLOW", 1))
                .output(PropertyDef::intProp("result")));
    parent = *addNativeCommand<Parent>(
        rt, CommandDef("test.parent").input(PropertyDef::intProp("fail")));
    refuse = *addNativeCommand<Refuse>(rt, CommandDef("test.refuse"));
    fold = *addNativeCommand<Foldable>(rt, CommandDef("test.fold").input(PropertyDef::intProp("key")));
    drag = *addNativeCommand<Drag>(rt, CommandDef("test.drag").flags(CommandFlags::Interactive));
    test_assert(addNativeCommand<CreateMesh>(
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

static void test_undo_redo()
{
  Setup s;
  test_assert(s.w.tool.count == 4);
  Result<PropertyBag> out = s.runAdd(2);
  test_assert(out.ok() && *out->get("result") == PropValue::ofInt(6));
  test_assert(s.runAdd(3).ok() && s.w.tool.count == 9);
  test_assert(s.history.size() == 2 && s.history.cursor() == 2);

  test_assert(s.ex.undo(s.ctx).ok() && s.w.tool.count == 6);
  test_assert(s.ex.undo(s.ctx).ok() && s.w.tool.count == 4);
  test_assert(s.ex.undo(s.ctx).code() == StatusCode::FailedPrecondition);
  test_assert(s.ex.redo(s.ctx).ok() && s.w.tool.count == 6); /* re-exec: no redo slot */
  /* A new command truncates the redo side, discarding it. */
  g_log.clear();
  test_assert(s.runAdd(1).ok() && s.w.tool.count == 7);
  test_assert(g_log == "exec1;discard;"); /* truncated when the new one is pushed */
  test_assert(s.history.size() == 2 && !s.history.canRedo());
  test_assert(s.ex.redo(s.ctx).code() == StatusCode::FailedPrecondition);

  /* canRun refuses; nothing runs or reaches the history. */
  Result<PropertyBag> r = s.ex.run(s.ctx, *s.refuse, s.refuse->newInputs());
  test_assert(r.status().code() == StatusCode::FailedPrecondition);
  test_assert(g_log.find("refuse-exec") == std::string::npos && s.history.size() == 2);
  test_assert(s.ex.run(s.ctx, "test.ad", PropertyBag()).status().suggestions().size() == 1);
  test_assert(s.history.validate().ok() && s.w.rt.validateAll().ok());
}

static void test_sub_commands()
{
  Setup s;
  /* Sub-commands are undone in reverse before the parent's own undo. */
  test_assert(s.ex.run(s.ctx, *s.parent, s.parent->newInputs()).ok());
  test_assert(s.w.tool.count == 15 && s.history.size() == 1 && s.history.at(0).subCount() == 2);
  g_log.clear();
  test_assert(s.ex.undo(s.ctx).ok() && s.w.tool.count == 4);
  test_assert(g_log == "undo;undo;parent-undo;");
  /* Redo without a redo slot drops the old subs and runs exec, which makes new ones. */
  g_log.clear();
  test_assert(s.ex.redo(s.ctx).ok() && s.w.tool.count == 15);
  test_assert(g_log == "discard;discard;exec1;exec10;" && s.history.at(0).subCount() == 2);

  /* A failed exec undoes its sub-commands, and nothing reaches the history. */
  for (int64_t mode : {1, 2}) {
    PropertyBag in = s.parent->newInputs();
    test_assert(in.set("fail", PropValue::ofInt(mode)).ok());
    g_log.clear();
    Result<PropertyBag> r = s.ex.run(s.ctx, *s.parent, std::move(in));
    test_assert(!r.ok() && s.w.tool.count == 15 && s.history.size() == 1);
    test_assert(g_log == "exec1;exec10;undo;discard;undo;discard;");
    test_assert(!s.w.rt.hasStickyError());
  }
  /* runSub outside an exec, and run inside one, are refused. */
  test_assert(s.ex.runSub(s.ctx, *s.add, s.add->newInputs()).status().code() ==
              StatusCode::FailedPrecondition);
}

static void test_folding_and_barriers()
{
  Setup s;
  test_assert(s.runFold(7).ok() && s.runFold(7).ok() && s.runFold(7).ok());
  test_assert(s.history.size() == 1 && s.w.tool.count == 7);
  test_assert(s.runFold(8).ok() && s.history.size() == 2); /* another key */
  test_assert(s.runFold(0).ok() && s.runFold(0).ok() && s.history.size() == 4); /* 0 never folds */
  /* Nothing folds into a head that was undone and redone. */
  test_assert(s.runFold(9).ok() && s.history.size() == 5);
  test_assert(s.ex.undo(s.ctx).ok() && s.ex.redo(s.ctx).ok());
  test_assert(s.runFold(9).ok() && s.history.size() == 6);
  /* One undo reverses the folded run. */
  while (s.history.canUndo()) {
    test_assert(s.ex.undo(s.ctx).ok());
  }
  test_assert(s.w.tool.count == 4);

  /* A barrier stops undo. */
  test_assert(s.ex.redo(s.ctx).ok());
  test_assert(s.runAdd(1).ok());
  s.history.pushBarrier();
  test_assert(!s.history.canUndo() && s.ex.undo(s.ctx).code() == StatusCode::FailedPrecondition);
  test_assert(s.runAdd(1).ok() && s.ex.undo(s.ctx).ok() && !s.history.canUndo());
  test_assert(s.history.validate().ok());
}

static void test_memory_budget()
{
  Setup s;
  s.history.setMemoryBudget(250);
  test_assert(s.runAdd(1, 100).ok() && s.runAdd(1, 100).ok());
  g_log.clear();
  test_assert(s.runAdd(1, 100).ok());
  test_assert(s.history.size() == 2 && s.history.memoryUsed() == 200);
  test_assert(g_log == "exec1;discard;"); /* the oldest went */
  /* The newest always stays, even over budget. */
  test_assert(s.runAdd(1, 1000).ok() && s.history.size() == 1 && s.history.canUndo());
  test_assert(s.history.validate().ok());
}

static void test_rerun_head()
{
  Setup s;
  test_assert(s.runAdd(2).ok() && s.w.tool.count == 6);
  PropertyBag in = s.add->newInputs();
  test_assert(in.set("amount", PropValue::ofInt(5)).ok());
  test_assert(s.ex.rerunHead(s.ctx, std::move(in)).ok());
  test_assert(s.w.tool.count == 9 && s.history.size() == 1);
  /* New inputs that fail: the old ones run again and the error comes back. */
  in = s.add->newInputs();
  test_assert(in.set("amount", PropValue::ofInt(-1)).ok());
  Status st = s.ex.rerunHead(s.ctx, std::move(in));
  test_assert(hasMsg(st, "negative amount") && s.w.tool.count == 9 && s.history.size() == 1);
  test_assert(s.ex.undo(s.ctx).ok() && s.w.tool.count == 4);
  /* Nothing to re-run once undone. */
  test_assert(s.ex.rerunHead(s.ctx, s.add->newInputs()).code() == StatusCode::FailedPrecondition);
}

static void test_interactive()
{
  Setup s;
  test_assert(s.ex.beginInteractive(s.ctx, *s.drag, s.drag->newInputs()).ok());
  test_assert(s.ex.interactiveOpen() && s.ex.interactiveCommand());
  test_assert(setValue(s.ctx, "tool.strength", PropValue::ofFloat(0.9)).ok()); /* the tool */

  /* Other commands are Busy, and collection too. */
  test_assert(s.runAdd(1).status().code() == StatusCode::Busy);
  test_assert(setValueUndo(s.ctx, "tool.count", PropValue::ofInt(1)).code() == StatusCode::Busy);
  test_assert(s.w.lib.collect().status().code() == StatusCode::Busy);
  test_assert(s.ex.beginInteractive(s.ctx, *s.drag, s.drag->newInputs()).code() ==
              StatusCode::Busy);
  test_assert(s.ex.run(s.ctx, *s.drag, s.drag->newInputs()).status().code() == StatusCode::Busy);

  /* Undo while one is open only cancels it. */
  test_assert(s.runAdd(0).status().code() == StatusCode::Busy);
  test_assert(s.ex.undo(s.ctx).ok() && !s.ex.interactiveOpen());
  test_assert(s.w.tool.strength == 0.5 && s.history.size() == 0);
  test_assert(s.w.lib.collect().ok());

  /* Commit, then undo and redo. */
  test_assert(s.runAdd(1).ok());
  test_assert(s.ex.beginInteractive(s.ctx, *s.drag, s.drag->newInputs()).ok());
  test_assert(setValue(s.ctx, "tool.strength", PropValue::ofFloat(0.8)).ok());
  test_assert(s.ex.commit(s.ctx).ok() && s.history.size() == 2);
  test_assert(s.ex.undo(s.ctx).ok() && s.w.tool.strength == 0.5 && s.w.tool.count == 5);
  test_assert(s.ex.redo(s.ctx).ok() && s.w.tool.strength == 0.8);
  test_assert(s.ex.commit(s.ctx).code() == StatusCode::FailedPrecondition);
  /* Interactive defs need beginInteractive, and plain ones can't use it. */
  test_assert(s.ex.run(s.ctx, *s.drag, s.drag->newInputs()).status().code() ==
              StatusCode::FailedPrecondition);
  test_assert(s.ex.beginInteractive(s.ctx, *s.add, s.add->newInputs()).code() ==
              StatusCode::FailedPrecondition);
}

static void test_set_value_undo()
{
  Setup s;
  /* Through `active`; undo writes the same object after `active` moved. */
  test_assert(setValueUndo(s.ctx, "objects.active.location[1]", PropValue::ofFloat(3)).ok());
  test_assert(s.w.object(0).location[1] == 3 && s.history.size() == 1);
  s.w.scene.active = 1;
  test_assert(s.ex.undo(s.ctx).ok());
  test_assert(s.w.object(0).location[1] == 0 && s.w.object(1).location[1] == 0);
  test_assert(s.ex.redo(s.ctx).ok() && s.w.object(0).location[1] == 3);

  /* A gesture folds into one step; another gesture or member doesn't. */
  for (double v : {0.1, 0.2, 0.3}) {
    test_assert(setValueUndo(s.ctx, "tool.strength", PropValue::ofFloat(v), 42).ok());
  }
  test_assert(s.history.size() == 2 && s.w.tool.strength == 0.3);
  test_assert(setValueUndo(s.ctx, "tool.strength", PropValue::ofFloat(0.4), 43).ok());
  test_assert(setValueUndo(s.ctx, "tool.free", PropValue::ofFloat(1), 43).ok());
  test_assert(s.history.size() == 4);
  test_assert(s.ex.undo(s.ctx).ok() && s.ex.undo(s.ctx).ok() && s.w.tool.strength == 0.3);
  test_assert(s.ex.undo(s.ctx).ok() && s.w.tool.strength == 0.5);

  /* Errors come back without a history entry. */
  test_assert(setValueUndo(s.ctx, "tool.version", PropValue::ofInt(1)).code() ==
              StatusCode::FailedPrecondition);
  test_assert(setValueUndo(s.ctx, "tool.nope", PropValue::ofInt(1)).code() == StatusCode::NotFound);
  test_assert(s.history.size() == 4);

  /* Undo of a set whose target is gone leaves a sticky error. */
  Setup t;
  test_assert(setValueUndo(t.ctx, "objects[0].data.radius", PropValue::ofFloat(2)).ok());
  test_assert(t.w.lib.writeRef(t.w.object(0).mesh, t.w.meshId(1)).ok());
  test_assert(t.w.lib.markDeleted(t.w.meshes[0]).ok());
  test_assert(t.ex.undo(t.ctx).code() == StatusCode::NotFound);
}

static void test_invoke_and_last_used()
{
  Setup s;
  Result<PropertyBag> r = invoke(s.ctx, "test.add(amount=5 mode=SLOW)");
  test_assert(r.ok() && s.w.tool.count == 9);
  /* Inputs not given take their LastUsed values. */
  test_assert(invoke(s.ctx, "test.add").ok() && s.w.tool.count == 14);
  LastUsedStore &lu = s.w.rt.ext<LastUsedStore>();
  test_assert(*lu.get(*s.add, "mode") == PropValue::ofEnum(1));
  test_assert(lu.get(*s.add, "mem") == nullptr); /* never set */

  Result<PropertyBag> bad = invoke(s.ctx, "test.add(amonut=1)");
  test_assert(bad.status().code() == StatusCode::NotFound);
  test_assert(hasMsg(bad.status(), "no input 'amonut' (at offset 9)"));
  test_assert(bad.status().suggestions().size() == 1);
  test_assert(invoke(s.ctx, "test.add(amount=\"x\")").status().code() == StatusCode::InvalidArgument);
  test_assert(invoke(s.ctx, "test.nope").status().code() == StatusCode::NotFound);
  test_assert(!invoke(s.ctx, "test.add(").ok());

  /* serialize/load round trip, skipping what no longer exists. */
  const std::string text = lu.serialize();
  test_assert(text == "test.add(amount=5 mode=SLOW)\n");
  lu.clear();
  test_assert(lu.load("# comment\n\ntest.gone(x=1)\ntest.add(amount=7 nope=1 mode=FAST)\r\n").ok());
  test_assert(*lu.get(*s.add, "amount") == PropValue::ofInt(7));
  test_assert(*lu.get(*s.add, "mode") == PropValue::ofEnum(0));
  test_assert(hasMsg(lu.load("test.add(amount=1)\ntest.add(\n"), "line 2"));
  test_assert(*lu.get(*s.add, "amount") == PropValue::ofInt(1));
  lu.clear();
  test_assert(lu.load(text).ok() && lu.serialize() == text);
}

static void test_command_defaults_path()
{
  Setup s;
  Runtime &rt = s.w.rt;
  struct Prefs {
    int unused = 0;
  } prefs;
  ApiStruct root("Prefs");
  rt.ext<LastUsedStore>().addCollectionTo(root);
  test_assert(rt.ext<ApiRegistry>().add(std::move(root)).ok());
  test_assert(rt.ext<ApiRegistry>()
                  .addRuntimeOwner("prefs", StructId::of("Prefs"), [&prefs]() { return (void *)&prefs; })
                  .ok());
  Context pc(&rt, &s.w.lib, rt.ext<ApiRegistry>().runtimeOwner("prefs"));
  pc.setExecutor(&s.ex);

  test_assert(*getValue(pc, "commandDefaults[\"test.add\"].amount") == PropValue::ofInt(1));
  test_assert(setValue(pc, "commandDefaults[\"test.add\"].amount", PropValue::ofInt(3)).ok());
  test_assert(*rt.ext<LastUsedStore>().get(*s.add, "amount") == PropValue::ofInt(3));
  test_assert(invoke(s.ctx, "test.add").ok() && s.w.tool.count == 7);
  /* NoUndo: setValueUndo writes directly. */
  test_assert(setValueUndo(pc, "commandDefaults[\"test.add\"].amount", PropValue::ofInt(4)).ok());
  test_assert(s.history.size() == 1);
  test_assert(*getValue(pc, "commandDefaults.length") == PropValue::ofInt(rt.ext<CommandRegistry>().size()));
  Result<StableRef> c = canonicalize(pc, *parsePath("commandDefaults[\"test.add\"].mode"));
  test_assert(c.ok() && c->key.size() == 3);
  Result<PropValue> miss = getValue(pc, "commandDefaults[\"test.add\"].amonut");
  test_assert(miss.status().suggestions().size() == 1);
  test_assert(rt.validateAll().ok());
}

static void test_literals()
{
  struct Case {
    PropertyDef def;
    PropValue value;
    const char *text;
  };
  const double m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  Case cases[] = {
      {PropertyDef::boolProp("b"), PropValue::ofBool(true), "true"},
      {PropertyDef::intProp("i"), PropValue::ofInt(-12), "-12"},
      {PropertyDef::floatProp("f"), PropValue::ofFloat(0.1), "0.1"},
      {PropertyDef::floatProp("f"), PropValue::ofFloat(INFINITY), "inf"},
      {PropertyDef::stringProp("s"), PropValue::ofString("a\"b\n"), "\"a\\\"b\\n\""},
      {PropertyDef::enumProp("e").item("A", 0).item("B", 3), PropValue::ofEnum(3), "B"},
      {PropertyDef::flagsProp("g").item("X", 1).item("Y", 4), PropValue::ofFlags(5), "\"X|Y\""},
      {PropertyDef::flagsProp("g").item("X", 1), PropValue::ofFlags(0), "0"},
      {PropertyDef::vecProp("v", 3), PropValue::ofVec3(1, -0.5, 2), "(1, -0.5, 2)"},
      {PropertyDef::mat4Prop("m"), PropValue::ofComponents(PropValue::Type::Mat4, m), nullptr},
  };
  for (Case &c : cases) {
    Result<std::string> lit = formatLiteral(c.def, c.value);
    test_assert(lit.ok());
    if (c.text && *lit != c.text) {
      fprintf(stderr, "literal was %s, expected %s\n", lit->c_str(), c.text);
      test_assert(false);
    }
    Result<Invocation> inv = parseInvocation("x(a=" + *lit + ")");
    test_assert(inv.ok());
    Result<PropValue> back = parseLiteral(c.def, inv->args[0].value);
    test_assert(back.ok() && *back == c.value);
  }
  test_assert(formatLiteral(PropertyDef::enumProp("e").item("A", 0), PropValue::ofEnum(9))
                  .status()
                  .code() == StatusCode::InvalidArgument);
  test_assert(formatLiteral(PropertyDef::pointerProp("p", StructId()), PropValue::nullPointer())
                  .status()
                  .code() == StatusCode::Unsupported);
  /* Clamped on the way in. */
  Result<Invocation> inv = parseInvocation("x(a=5)");
  test_assert(*parseLiteral(PropertyDef::floatProp("f").range(0, 1), inv->args[0].value) ==
              PropValue::ofFloat(1));
  test_assert(!parseLiteral(PropertyDef::vecProp("v", 2), inv->args[0].value).ok());
}

static void test_registration()
{
  Setup s;
  Runtime &rt = s.w.rt;
  test_assert(addNativeCommand<AddCount>(rt, CommandDef("test.add")).status().code() ==
              StatusCode::AlreadyExists);
  /* Interactive needs a redo slot. */
  test_assert(hasMsg(addNativeCommand<AddCount>(
                         rt, CommandDef("test.dragNoRedo").flags(CommandFlags::Interactive))
                         .status(),
                     "redo slot"));
  /* No implementation, an unregistered one, duplicate inputs. */
  test_assert(hasMsg(rt.ext<CommandRegistry>().add(CommandDef("test.bare")).status(),
                     "no implementation"));
  CommandDef ghost("test.ghost");
  ghost.impl("test.ghost#impl", [](Runtime &) { return Result<Owned<Command>>(Owned<Command>()); });
  test_assert(rt.ext<CommandRegistry>().add(std::move(ghost)).status().code() == StatusCode::NotFound);
  test_assert(hasMsg(addNativeCommand<AddCount>(rt, CommandDef("test.dup")
                                                        .input(PropertyDef::intProp("a"))
                                                        .input(PropertyDef::intProp("a")))
                         .status(),
                     "input"));
  test_assert(rt.validateAll().ok());
}

static void test_block_commands()
{
  Setup s;
  Library &lib = s.w.lib;
  Runtime &rt = s.w.rt;
  const int before = lib.size();

  /* Create, undo, redo: the block keeps its id; a discard while undone destroys it. */
  Result<PropertyBag> made = invoke(s.ctx, "test.createMesh");
  test_assert(made.ok() && lib.size() == before + 1);
  const uint64_t lo = uint64_t(made->get("block")->asInt());
  BlockHandle h;
  lib.forEach([&](BlockHandle bh, const Datablock &b) {
    if (b.id().local.lo == lo) {
      h = bh;
    }
  });
  const BlockId id = lib.get(h)->id();
  test_assert(s.ex.undo(s.ctx).ok() && lib.get(h)->state() == BlockState::InHistory);
  test_assert(lib.data(h) == nullptr);
  test_assert(s.ex.redo(s.ctx).ok() && lib.get(h)->state() == BlockState::Live);
  test_assert(lib.find(id) == h);
  test_assert(s.ex.undo(s.ctx).ok());
  test_assert(s.runAdd(1).ok()); /* truncates the undone create */
  test_assert(!lib.get(h) && lib.size() == before);

  /* Delete through an OptStrong ref: no cascade, and the object's `data` reads null. */
  Result<PropValue> mesh = getValue(s.ctx, "objects[0].data");
  PropertyBag in = rt.ext<CommandRegistry>().find(kDeleteBlockCommand)->newInputs();
  test_assert(in.set("block", *mesh).ok());
  Result<PropertyBag> del = s.ex.run(s.ctx, kDeleteBlockCommand, std::move(in));
  test_assert(del.ok() && *del->get("count") == PropValue::ofInt(1));
  test_assert(*getValue(s.ctx, "objects[0].data") == PropValue::nullPointer());
  test_assert(s.ex.undo(s.ctx).ok());
  test_assert(getValue(s.ctx, "objects[0].data.radius").ok());

  /* A strong Ref cascades: preview, refuse without cascade, delete both, undo both. */
  test_assert((registerImpl<BlockProvider, RigProvider>(rt, "test.Rig#provider").ok()));
  Result<Owned<BlockProvider>> p =
      own<BlockProvider>(rt, "test.Rig#provider", litestl::alloc::New<RigProvider>("provider"));
  BlockTypeInfo info;
  info.name = "test.Rig";
  info.client = ClientId::of(kCoredccClient);
  test_assert(p.ok() && rt.ext<BlockTypeRegistry>().add(info, std::move(p).value()).ok());
  Rig *rig = litestl::alloc::New<Rig>("Rig");
  Result<BlockHandle> rigH = lib.create(kRigType, "rig", rig);
  test_assert(rigH.ok() && lib.writeRef(rig->target, s.w.meshId(0)).ok());
  Rig *rig2 = litestl::alloc::New<Rig>("Rig");
  Result<BlockHandle> rig2H = lib.create(kRigType, "rig2", rig2);
  test_assert(rig2H.ok() && lib.writeRef(rig2->target, lib.get(*rigH)->id()).ok());
  litestl::util::Vector<BlockHandle> preview = deletePreview(lib, s.w.meshes[0]);
  test_assert(preview.size() == 3 && preview[0] == s.w.meshes[0]);

  in = rt.ext<CommandRegistry>().find(kDeleteBlockCommand)->newInputs();
  test_assert(in.set("block", *mesh).ok() && in.set("cascade", PropValue::ofBool(false)).ok());
  Result<PropertyBag> refused = s.ex.run(s.ctx, kDeleteBlockCommand, std::move(in));
  test_assert(hasMsg(refused.status(), "would also delete rig, rig2"));
  test_assert(lib.get(*rigH)->state() == BlockState::Live);

  in = rt.ext<CommandRegistry>().find(kDeleteBlockCommand)->newInputs();
  test_assert(in.set("block", *mesh).ok());
  del = s.ex.run(s.ctx, kDeleteBlockCommand, std::move(in));
  test_assert(del.ok() && *del->get("count") == PropValue::ofInt(3));
  test_assert(lib.get(*rig2H)->state() == BlockState::InHistory);
  test_assert(s.ex.undo(s.ctx).ok());
  test_assert(lib.get(*rig2H)->state() == BlockState::Live && lib.resolveRef(rig2->target) == *rigH);
  test_assert(s.ex.redo(s.ctx).ok() && lib.get(*rigH)->state() == BlockState::InHistory);

  /* Purge: history-held blocks are roots, so nothing goes until the history drops them.
   * The scene's objects are rooted by hand (the fixture's scene is a runtime owner). */
  for (BlockHandle ob : s.w.scene.objects) {
    lib.setFlag(ob, BlockFlags::FakeUser, true);
  }
  Result<PropertyBag> purged = invoke(s.ctx, "core.block.purge");
  test_assert(purged.ok() && *purged->get("count") == PropValue::ofInt(0));
  s.history.clear(); /* discards the applied delete: the three are destroyed */
  test_assert(!lib.get(*rigH) && !lib.get(*rig2H) && !lib.get(s.w.meshes[0]));
  test_assert(lib.validate().ok() && rt.validateAll().ok());
  test_assert(s.ex.run(s.ctx, kDeleteBlockCommand, rt.ext<CommandRegistry>().find(kDeleteBlockCommand)->newInputs())
                  .status()
                  .code() == StatusCode::FailedPrecondition);
}

/* A snapshot provider over one int. */
struct IntSnapshot {
  int *value = nullptr;
  litestl::util::Vector<uint8_t> capture() const
  {
    litestl::util::Vector<uint8_t> out;
    for (int i = 0; i < 4; i++) {
      out.append(uint8_t((uint32_t(*value) >> (i * 8)) & 0xff));
    }
    return out;
  }
  Status restore(const litestl::util::Vector<uint8_t> &blob)
  {
    if (blob.size() != 4) {
      return Status::error(StatusCode::InvalidArgument, "bad snapshot");
    }
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
      v |= uint32_t(blob[i]) << (i * 8);
    }
    *value = int(v);
    return Status();
  }
  static lbt::Struct<IntSnapshot> *defineBindings()
  {
    auto *st = new lbt::Struct<IntSnapshot>("test::IntSnapshot", sizeof(IntSnapshot));
    BIND_STRUCT_METHOD(st, capture, MARGS());
    BIND_STRUCT_METHOD(st, restore, MARGS("blob"));
    return st;
  }
};

static void test_undo_strategies()
{
  std::string log;
  {
    DelegatedUndo d([&log]() { log += "u"; }, [&log]() { log += "r"; return Status(); },
                    [&log]() { log += "x"; }, 64);
    d.undo();
    test_assert(d.redo().ok() && d.memSize() == 64);
    DelegatedUndo moved = std::move(d);
    moved.discard();
    moved.discard(); /* once */
  }
  test_assert(log == "urx");

  Runtime rt;
  test_assert((registerImpl<SnapshotProvider, IntSnapshot>(rt, "test.IntSnapshot").ok()));
  int value = 1234567;
  IntSnapshot impl;
  impl.value = &value;
  SnapshotProvider snap = query<SnapshotProvider>(rt, TypeId::of("test.IntSnapshot"), &impl);
  test_assert(bool(snap));
  litestl::util::Vector<uint8_t> blob = snap.capture();
  value = 0;
  test_assert(snap.restore(blob).ok() && value == 1234567);
  test_assert(!snap.restore(litestl::util::Vector<uint8_t>()).ok());
}

int main()
{
  test_undo_strategies();
  test_block_commands();
  test_undo_redo();
  test_sub_commands();
  test_folding_and_barriers();
  test_memory_budget();
  test_rerun_head();
  test_interactive();
  test_set_value_undo();
  test_invoke_and_last_used();
  test_command_defaults_path();
  test_literals();
  test_registration();
  return test_end();
}
