/* Regression tests for the command feature review
 * (docs/reports/feature-report-command-2026-11-21.md). Each test names its finding. */
#include "command_fixture.h"

#include "coredcc/parse/invocation.h"

#include <climits>
#include <cstring>
#include <cmath>
#include <string>

test_init;

static bool g_flag = false;
static Runtime *g_rt = nullptr;

#define BIND_ALL(T, name, extra)                                                         \
  static lbt::Struct<T> *defineBindings()                                                \
  {                                                                                      \
    auto *st = new lbt::Struct<T>(name, sizeof(T));                                      \
    BIND_COMMAND_CORE(st);                                                               \
    extra;                                                                               \
    return st;                                                                           \
  }

/* Relative increment of tool.count: dropped or doubled work shows in the count. */
static int g_memCalls = 0;
struct Inc {
  Status canRun(Context &) const
  {
    return Status();
  }
  Status exec(Context &ctx)
  {
    setCount(ctx, count(ctx) + 1);
    return Status();
  }
  void undo(Context &ctx)
  {
    setCount(ctx, count(ctx) - 1);
  }
  uint64_t memSize() const
  {
    g_memCalls++;
    return 0;
  }
  void discard() {}
  BIND_ALL(Inc, "review::Inc", )
};

static Status runSubInc(Context &ctx)
{
  const CommandDef *inc = ctx.runtime().ext<CommandRegistry>().find("review.inc");
  return ctx.executor()->runSub(ctx, *inc, inc->newInputs()).status();
}

/* B1: slots that call back into the executor. */
static std::string g_inner;
struct UndoRuns {
  Status canRun(Context &) const
  {
    return Status();
  }
  Status exec(Context &)
  {
    return Status();
  }
  void undo(Context &ctx)
  {
    g_inner = ctx.executor()->run(ctx, "review.inc", PropertyBag()).status().message();
  }
  Status redo(Context &ctx)
  {
    return ctx.executor()->run(ctx, "review.inc", PropertyBag()).status();
  }
  uint64_t memSize() const
  {
    return 0;
  }
  void discard() {}
  BIND_ALL(UndoRuns, "review::UndoRuns", BIND_STRUCT_METHOD(st, redo, MARGS("ctx")))
};

/* B4: a sub, then a failure on the second exec (redo by re-exec). */
struct SubThenFail {
  Status canRun(Context &) const
  {
    return Status();
  }
  Status exec(Context &ctx)
  {
    CDCC_TRY(runSubInc(ctx));
    return g_flag ? Status::error(StatusCode::Internal, "exec failed on redo") : Status();
  }
  void undo(Context &) {}
  uint64_t memSize() const
  {
    return 0;
  }
  void discard() {}
  BIND_ALL(SubThenFail, "review::SubThenFail", )
};

/* B4: a sub, then the redo slot fails. */
struct SubRedoFail {
  Status canRun(Context &) const
  {
    return Status();
  }
  Status exec(Context &ctx)
  {
    return runSubInc(ctx);
  }
  void undo(Context &) {}
  Status redo(Context &)
  {
    return g_flag ? Status::error(StatusCode::Internal, "redo failed") : Status();
  }
  uint64_t memSize() const
  {
    return 0;
  }
  void discard() {}
  BIND_ALL(SubRedoFail, "review::SubRedoFail", BIND_STRUCT_METHOD(st, redo, MARGS("ctx")))
};

/* B9: foldable, with its work in a sub. */
struct FoldSub {
  Status canRun(Context &) const
  {
    return Status();
  }
  Status exec(Context &ctx)
  {
    return runSubInc(ctx);
  }
  void undo(Context &) {}
  uint64_t memSize() const
  {
    return 0;
  }
  void discard() {}
  uint64_t foldKey() const
  {
    return 78;
  }
  bool foldFrom(Command)
  {
    return true;
  }
  BIND_ALL(FoldSub, "review::FoldSub", BIND_STRUCT_METHOD(st, foldKey, MARGS());
           BIND_STRUCT_METHOD(st, foldFrom, MARGS("next")))
};

/* B8: sticky errors from exec and from memSize (as a throwing foreign slot leaves). */
struct StickyFail {
  Status canRun(Context &) const
  {
    return Status();
  }
  Status exec(Context &ctx)
  {
    ctx.runtime().setStickyError(Status::error(StatusCode::Internal, "late sticky"));
    return Status::error(StatusCode::InvalidArgument, "early");
  }
  void undo(Context &) {}
  uint64_t memSize() const
  {
    return 0;
  }
  void discard() {}
  BIND_ALL(StickyFail, "review::StickyFail", )
};
struct MemFail {
  Status canRun(Context &) const
  {
    return Status();
  }
  Status exec(Context &)
  {
    return Status();
  }
  void undo(Context &) {}
  uint64_t memSize() const
  {
    if (g_flag) {
      g_rt->setStickyError(Status::error(StatusCode::Internal, "foreign memSize threw"));
    }
    return 0;
  }
  void discard() {}
  BIND_ALL(MemFail, "review::MemFail", )
};

/* B13: foldKey without foldFrom. */
struct KeyOnly {
  Status canRun(Context &) const
  {
    return Status();
  }
  Status exec(Context &)
  {
    return Status();
  }
  void undo(Context &) {}
  uint64_t memSize() const
  {
    return 0;
  }
  void discard() {}
  uint64_t foldKey() const
  {
    return 5;
  }
  BIND_ALL(KeyOnly, "review::KeyOnly", BIND_STRUCT_METHOD(st, foldKey, MARGS()))
};

/* Design item 7: setValueUndo inside exec becomes a sub-command. */
struct SetsInside {
  Status canRun(Context &) const
  {
    return Status();
  }
  Status exec(Context &ctx)
  {
    return setValueUndo(ctx, "tool.count", PropValue::ofInt(99));
  }
  void undo(Context &) {}
  uint64_t memSize() const
  {
    return 0;
  }
  void discard() {}
  BIND_ALL(SetsInside, "review::SetsInside", )
};

/* TA1 review A1: a command that runs itself forever. */
static int g_depth = 0;
struct Recurse {
  Status canRun(Context &) const
  {
    return Status();
  }
  Status exec(Context &ctx)
  {
    g_depth++;
    const CommandDef *self = ctx.runtime().ext<CommandRegistry>().find("review.recurse");
    return ctx.executor()->runSub(ctx, *self, self->newInputs()).status();
  }
  void undo(Context &) {}
  uint64_t memSize() const
  {
    return 0;
  }
  void discard() {}
  BIND_ALL(Recurse, "review::Recurse", )
};

static void addInc(Setup &s)
{
  test_assert(addNativeCommand<Inc>(s.w.rt, CommandDef("review.inc")).ok());
}

static BlockHandle makeRig(Setup &s, BlockHandle target)
{
  Runtime &rt = s.w.rt;
  if (!rt.ext<BlockTypeRegistry>().find(kRigType)) {
    test_assert((registerImpl<BlockProvider, RigProvider>(rt, "test.Rig#provider").ok()));
    Result<Owned<BlockProvider>> p =
        own<BlockProvider>(rt, "test.Rig#provider", litestl::alloc::New<RigProvider>("provider"));
    BlockTypeInfo info;
    info.name = "test.Rig";
    info.client = ClientId::of(kCoredccClient);
    test_assert(p.ok() && rt.ext<BlockTypeRegistry>().add(info, std::move(p).value()).ok());
  }
  Rig *rig = litestl::alloc::New<Rig>("Rig");
  Result<BlockHandle> h = s.w.lib.create(kRigType, "rig", rig);
  test_assert(h.ok() && s.w.lib.writeRef(rig->target, s.w.lib.get(target)->id()).ok());
  return *h;
}

static PropertyBag deleteInputs(Setup &s, BlockHandle h, bool cascade)
{
  StableRef r;
  r.owner = OwnerKey::ofBlock(s.w.lib.get(h)->id());
  PropertyBag in = s.w.rt.ext<CommandRegistry>().find(kDeleteBlockCommand)->newInputs();
  test_assert(in.set("block", PropValue::ofPointer(r)).ok());
  test_assert(in.set("cascade", PropValue::ofBool(cascade)).ok());
  return in;
}

/* ---- tests ---- */

static void test_reentrancy()
{
  Setup s;
  addInc(s);
  test_assert(addNativeCommand<UndoRuns>(s.w.rt, CommandDef("review.undoRuns")).ok());
  test_assert(s.ex.run(s.ctx, "review.undoRuns", PropertyBag()).ok());
  g_inner.clear();
  test_assert(s.ex.undo(s.ctx).ok());
  test_assert(g_inner.find("inside a command") != std::string::npos);
  test_assert(s.history.size() == 1 && s.w.tool.count == 4);
  /* B1b: the redo slot's run is refused, so the redo fails and is dropped cleanly. */
  Status r = s.ex.redo(s.ctx);
  test_assert(hasMsg(r, "inside a command") && s.history.validate().ok());
  test_assert(s.history.size() == 0 && s.w.tool.count == 4);
}

static void test_failed_redo_rolls_back()
{
  {
    Setup s;
    addInc(s);
    test_assert(addNativeCommand<SubThenFail>(s.w.rt, CommandDef("review.subThenFail")).ok());
    g_flag = false;
    test_assert(s.ex.run(s.ctx, "review.subThenFail", PropertyBag()).ok() && s.w.tool.count == 5);
    test_assert(s.ex.undo(s.ctx).ok() && s.w.tool.count == 4);
    g_flag = true;
    test_assert(hasMsg(s.ex.redo(s.ctx), "exec failed on redo"));
    g_flag = false;
    test_assert(s.w.tool.count == 4 && s.history.size() == 0);
  }
  {
    Setup s;
    addInc(s);
    test_assert(addNativeCommand<SubRedoFail>(s.w.rt, CommandDef("review.subRedoFail")).ok());
    g_flag = false;
    test_assert(s.ex.run(s.ctx, "review.subRedoFail", PropertyBag()).ok());
    test_assert(s.ex.undo(s.ctx).ok() && s.w.tool.count == 4);
    g_flag = true;
    test_assert(hasMsg(s.ex.redo(s.ctx), "redo failed"));
    g_flag = false;
    test_assert(s.w.tool.count == 4 && s.history.size() == 0);
  }
}

static void test_input_bags_are_checked()
{
  Setup s;
  /* B2: an empty bag gets the def's defaults; a null block is refused, not a crash. */
  Status r = s.ex.run(s.ctx, kDeleteBlockCommand, PropertyBag()).status();
  test_assert(r.code() == StatusCode::FailedPrecondition);
  PropertyBag wrong;
  test_assert(wrong.add(PropertyDef::intProp("bogus")).ok());
  test_assert(s.ex.run(s.ctx, *s.add, wrong).status().code() == StatusCode::InvalidArgument);
  PropertyBag typed;
  test_assert(typed.add(PropertyDef::stringProp("amount")).ok());
  test_assert(typed.set("amount", PropValue::ofString("x")).ok());
  test_assert(s.ex.run(s.ctx, *s.add, typed).status().code() == StatusCode::InvalidArgument);
  test_assert(s.history.size() == 0 && s.w.tool.count == 4);
}

static void test_rerun_head_refusals()
{
  {
    Setup s; /* B3: a setValueUndo head */
    test_assert(setValueUndo(s.ctx, "tool.count", PropValue::ofInt(10)).ok());
    const CommandDef *def = s.w.rt.ext<CommandRegistry>().find(kSetPropertyCommand);
    test_assert(s.ex.rerunHead(s.ctx, def->newInputs()).code() == StatusCode::FailedPrecondition);
    test_assert(s.w.tool.count == 10 && s.history.size() == 1);
  }
  {
    Setup s; /* B3: an interactive head */
    test_assert(s.ex.beginInteractive(s.ctx, *s.drag, s.drag->newInputs()).ok());
    test_assert(setValue(s.ctx, "tool.strength", PropValue::ofFloat(0.9)).ok());
    test_assert(s.ex.commit(s.ctx).ok());
    test_assert(s.ex.rerunHead(s.ctx, s.drag->newInputs()).code() ==
                StatusCode::FailedPrecondition);
    test_assert(s.w.tool.strength == 0.9 && s.history.size() == 1);
  }
  {
    Setup s; /* a failed re-run puts the old record back, redone */
    test_assert(s.runAdd(2).ok() && s.w.tool.count == 6);
    PropertyBag in = s.add->newInputs();
    test_assert(in.set("amount", PropValue::ofInt(-1)).ok());
    test_assert(hasMsg(s.ex.rerunHead(s.ctx, std::move(in)), "negative amount"));
    test_assert(s.w.tool.count == 6 && s.history.size() == 1 && s.history.head());
    test_assert(s.history.head()->inputs().get("amount")->asInt() == 2);
  }
}

static void test_folding()
{
  {
    Setup s; /* B9: records with subs don't fold */
    addInc(s);
    test_assert(addNativeCommand<FoldSub>(s.w.rt, CommandDef("review.foldSub")).ok());
    test_assert(s.ex.run(s.ctx, "review.foldSub", PropertyBag()).ok());
    test_assert(s.ex.run(s.ctx, "review.foldSub", PropertyBag()).ok());
    test_assert(s.w.tool.count == 6 && s.history.size() == 2);
    test_assert(s.ex.undo(s.ctx).ok() && s.w.tool.count == 5);
  }
  {
    Setup s; /* B15: the head shows the folded run's inputs */
    const CommandDef *def = *addNativeCommand<Foldable>(
        s.w.rt,
        CommandDef("review.fold2").input(PropertyDef::intProp("key")).input(PropertyDef::intProp("amount")));
    for (int64_t a : {1, 2, 3}) {
      PropertyBag in = def->newInputs();
      test_assert(in.set("key", PropValue::ofInt(5)).ok());
      test_assert(in.set("amount", PropValue::ofInt(a)).ok());
      test_assert(s.ex.run(s.ctx, *def, std::move(in)).ok());
    }
    test_assert(s.history.size() == 1);
    test_assert(s.history.head()->inputs().get("amount")->asInt() == 3);
  }
  {
    Setup s; /* B13 */
    Result<const CommandDef *> d = addNativeCommand<KeyOnly>(s.w.rt, CommandDef("review.keyOnly"));
    test_assert(hasMsg(d.status(), "foldKey and foldFrom"));
  }
}

static void test_sticky_errors()
{
  {
    Setup s; /* B8a: both errors are reported, and nothing leaks */
    test_assert(addNativeCommand<StickyFail>(s.w.rt, CommandDef("review.stickyFail")).ok());
    Status a = s.ex.run(s.ctx, "review.stickyFail", PropertyBag()).status();
    test_assert(hasMsg(a, "early") && hasMsg(a, "late sticky"));
    test_assert(s.runAdd(1).ok());
  }
  {
    Setup s; /* B8b: a live-phase failure cancels the commit */
    test_assert(s.ex.beginInteractive(s.ctx, *s.drag, s.drag->newInputs()).ok());
    test_assert(setValue(s.ctx, "tool.strength", PropValue::ofFloat(0.9)).ok());
    s.w.rt.setStickyError(Status::error(StatusCode::Internal, "live-edit failure"));
    test_assert(hasMsg(s.ex.commit(s.ctx), "live-edit failure"));
    test_assert(!s.ex.interactiveOpen() && s.history.size() == 0 && s.w.tool.strength == 0.5);
    test_assert(s.w.lib.collect().ok() && s.runAdd(1).ok());
  }
  {
    Setup s; /* B8c: an upkeep failure is the pushing command's, and doesn't leak */
    g_rt = &s.w.rt;
    test_assert(addNativeCommand<MemFail>(s.w.rt, CommandDef("review.memFail")).ok());
    g_flag = true;
    Status a = s.ex.run(s.ctx, "review.memFail", PropertyBag()).status();
    g_flag = false;
    test_assert(hasMsg(a, "foreign memSize threw") && hasMsg(a, "upkeep"));
    test_assert(s.history.size() == 1 && s.runAdd(1).ok());
  }
  {
    Setup s; /* a stray one is named by the next top-level call, then gone */
    s.w.rt.setStickyError(Status::error(StatusCode::Internal, "from elsewhere"));
    Status a = s.runAdd(1).status();
    test_assert(a.code() == StatusCode::FailedPrecondition && hasMsg(a, "from elsewhere"));
    test_assert(s.runAdd(1).ok());
  }
}

static void test_executor_lifetime_and_binding()
{
  Setup s;
  {
    LinearHistory h2; /* B10: destroying the executor cancels the open command */
    CommandExecutor ex2(&s.w.rt, &h2);
    Context c2 = s.w.context();
    test_assert(ex2.beginInteractive(c2, *s.drag, s.drag->newInputs()).ok());
    test_assert(c2.executor() == nullptr); /* bound only for the call */
    test_assert(setValue(c2, "tool.strength", PropValue::ofFloat(0.9)).ok());
    /* Another executor's Context is refused, not an abort. */
    test_assert(ex2.run(s.ctx, *s.add, s.add->newInputs()).status().code() ==
                StatusCode::Busy);
    test_assert(ex2.cancel(c2).ok());
    test_assert(ex2.run(s.ctx, *s.add, s.add->newInputs()).status().code() ==
                StatusCode::FailedPrecondition);
    test_assert(ex2.beginInteractive(c2, *s.drag, s.drag->newInputs()).ok());
    test_assert(setValue(c2, "tool.strength", PropValue::ofFloat(0.9)).ok());
  }
  test_assert(s.w.lib.collect().ok() && s.w.tool.strength == 0.5);
}

static void test_history_pins()
{
  {
    Setup s; /* B5: a pointer set pins the old target */
    for (BlockHandle ob : s.w.scene.objects) {
      s.w.lib.setFlag(ob, BlockFlags::FakeUser, true);
    }
    Result<PropValue> cube = getValue(s.ctx, "objects[0].data");
    Result<PropValue> sphere = getValue(s.ctx, "objects[1].data");
    test_assert(setValueUndo(s.ctx, "objects[0].data", *sphere).ok());
    const Datablock *old = s.w.lib.get(s.w.meshes[0]);
    test_assert(old->pins(PinKind::History) == 1 && old->pins(PinKind::Host) == 0);
    test_assert(invoke(s.ctx, "core.block.purge").ok());
    test_assert(s.w.lib.get(s.w.meshes[0]) != nullptr);
    test_assert(s.ex.undo(s.ctx).ok());
    test_assert(*getValue(s.ctx, "objects[0].data") == *cube);
    s.history.clear(); /* unpins */
    test_assert(s.w.lib.validate().ok());
  }
  {
    Setup s; /* B5: created blocks are pinned */
    for (BlockHandle ob : s.w.scene.objects) {
      s.w.lib.setFlag(ob, BlockFlags::FakeUser, true);
    }
    test_assert(invoke(s.ctx, "test.createMesh").ok());
    int history = 0, host = 0;
    s.w.lib.forEach([&](BlockHandle, const Datablock &b) {
      history += b.pins(PinKind::History);
      host += b.pins(PinKind::Host);
    });
    test_assert(history == 1 && host == 0);
    test_assert(invoke(s.ctx, "core.block.purge").ok());
    test_assert(s.ex.undo(s.ctx).ok() && s.ex.redo(s.ctx).ok());
    s.history.clear();
    Result<int> freed = s.w.lib.collect();
    test_assert(freed.ok() && *freed == 1); /* unpinned: now it is garbage */
  }
}

static void test_delete_cascade_states()
{
  {
    Setup s; /* B6: a user already deleted doesn't block the delete */
    BlockHandle rig = makeRig(s, s.w.meshes[0]);
    test_assert(s.ex.run(s.ctx, kDeleteBlockCommand, deleteInputs(s, rig, true)).ok());
    test_assert(deletePreview(s.w.lib, s.w.meshes[0]).size() == 1);
    test_assert(s.ex.run(s.ctx, kDeleteBlockCommand, deleteInputs(s, s.w.meshes[0], false)).ok());
  }
  {
    Setup s; /* B7: redo takes a Strong user added since, with cascade */
    test_assert(s.ex.run(s.ctx, kDeleteBlockCommand, deleteInputs(s, s.w.meshes[0], true)).ok());
    test_assert(s.ex.undo(s.ctx).ok());
    BlockHandle rig = makeRig(s, s.w.meshes[0]);
    test_assert(s.ex.redo(s.ctx).ok());
    test_assert(s.w.lib.get(rig)->state() == BlockState::InHistory);
    s.history.clear();
    test_assert(!s.w.lib.get(s.w.meshes[0]) && !s.w.lib.get(rig) && !s.w.rt.hasStickyError());
  }
  {
    Setup s; /* B7: without cascade the redo fails and is dropped */
    test_assert(s.ex.run(s.ctx, kDeleteBlockCommand, deleteInputs(s, s.w.meshes[0], false)).ok());
    test_assert(s.ex.undo(s.ctx).ok());
    BlockHandle rig = makeRig(s, s.w.meshes[0]);
    test_assert(hasMsg(s.ex.redo(s.ctx), "would now also delete 'rig'"));
    test_assert(s.w.lib.get(s.w.meshes[0])->state() == BlockState::Live);
    test_assert(s.w.lib.get(rig)->state() == BlockState::Live && s.history.size() == 0);
  }
}

static void test_literal_edges()
{
  auto roundTrip = [](const PropertyDef &def, const PropValue &v) {
    Result<std::string> lit = formatLiteral(def, v);
    test_assert(lit.ok());
    Result<Invocation> inv = parseInvocation("x(a=" + *lit + ")");
    test_assert(inv.ok());
    Result<PropValue> back = parseLiteral(def, inv->args[0].value);
    test_assert(back.ok() && *back == v);
  };
  roundTrip(PropertyDef::floatProp("f"), PropValue::ofFloat(-INFINITY)); /* B11 */
  roundTrip(PropertyDef::vecProp("v", 2), PropValue::ofVec2(-INFINITY, 1));
  roundTrip(PropertyDef::flagsProp("g").item("AB", 3).item("A", 1).item("B", 2),
            PropValue::ofFlags(1)); /* B12 */
  roundTrip(PropertyDef::intProp("i"), PropValue::ofInt(INT64_MIN));
  for (const char *text : {"x(a=9223372036854775808)", "x(a=1e999)"}) {
    /* Rejected by the invocation lexer or by the literal, never clamped. */
    Result<Invocation> inv = parseInvocation(text);
    if (inv.ok()) {
      const PropertyDef def = text[4] == '1' ? PropertyDef::floatProp("f") : PropertyDef::intProp("i");
      test_assert(hasMsg(parseLiteral(def, inv->args[0].value).status(), "out of range"));
    }
  }
}

static void test_last_used_load_continues()
{
  Setup s; /* B11: a bad line is reported, and the lines after it still load */
  LastUsedStore &lu = s.w.rt.ext<LastUsedStore>();
  Status l = lu.load("test.add(amount=((\ntest.add(amount=7)\n");
  test_assert(hasMsg(l, "LastUsed line 1"));
  test_assert(lu.get(*s.add, "amount") && lu.get(*s.add, "amount")->asInt() == 7);
}

static void test_registration_retry()
{
  Setup s; /* B14 */
  Status a = addNativeCommand<AddCount>(
                 s.w.rt, CommandDef("review.dup").input(PropertyDef::intProp("a")).input(PropertyDef::intProp("a")))
                 .status();
  test_assert(!a.ok());
  test_assert(addNativeCommand<AddCount>(s.w.rt, CommandDef("review.dup")
                                                     .input(PropertyDef::intProp("amount"))
                                                     .input(PropertyDef::intProp("mem")))
                  .ok());
}

static void test_memsize_once_per_push()
{
  Setup s; /* design item 4 */
  addInc(s);
  g_memCalls = 0;
  for (int i = 0; i < 1000; i++) {
    test_assert(s.ex.run(s.ctx, "review.inc", PropertyBag()).ok());
  }
  test_assert(g_memCalls == 1000 && s.history.validate().ok());
}

static void test_set_value_undo_inside_exec()
{
  Setup s; /* design item 7 */
  test_assert(addNativeCommand<SetsInside>(s.w.rt, CommandDef("review.setsInside")).ok());
  test_assert(s.ex.run(s.ctx, "review.setsInside", PropertyBag()).ok());
  test_assert(s.w.tool.count == 99 && s.history.size() == 1 && s.history.at(0).subCount() == 1);
  test_assert(s.ex.undo(s.ctx).ok() && s.w.tool.count == 4);
}

/* ---- test gaps the review listed ---- */

/* Runs test.parent (itself two test.add subs) as a sub, then fails if g_flag. */
struct GrandParent {
  Status canRun(Context &) const
  {
    return Status();
  }
  Status exec(Context &ctx)
  {
    const CommandDef *parent = ctx.runtime().ext<CommandRegistry>().find("test.parent");
    CDCC_TRY(ctx.executor()->runSub(ctx, *parent, parent->newInputs()).status());
    return g_flag ? Status::error(StatusCode::Internal, "grandparent failed") : Status();
  }
  void undo(Context &)
  {
    g_log += "grandparent-undo;";
  }
  uint64_t memSize() const
  {
    return 0;
  }
  void discard() {}
  BIND_ALL(GrandParent, "review::GrandParent", )
};

/* Counts instances and discards; fails each one discarded twice. `refuse`, `fail`
 * and `key` (fold, 0 never) come from the inputs; `mem` is its memSize. */
static int g_made = 0;
static int g_discards = 0;
struct Counted {
  int64_t key = 0;
  int64_t mem = 0;
  bool discarded = false;
  Counted()
  {
    g_made++;
  }
  Status canRun(Context &ctx) const
  {
    return inputInt(ctx, "refuse") ? Status::error(StatusCode::FailedPrecondition, "refused")
                                   : Status();
  }
  Status exec(Context &ctx)
  {
    key = inputInt(ctx, "key");
    mem = inputInt(ctx, "mem");
    return inputInt(ctx, "fail") ? Status::error(StatusCode::Internal, "failed") : Status();
  }
  void undo(Context &) {}
  uint64_t memSize() const
  {
    return uint64_t(mem);
  }
  void discard()
  {
    test_assert(!discarded);
    discarded = true;
    g_discards++;
  }
  uint64_t foldKey() const
  {
    return uint64_t(key);
  }
  bool foldFrom(Command)
  {
    return true;
  }
  BIND_ALL(Counted, "review::Counted", BIND_STRUCT_METHOD(st, foldKey, MARGS());
           BIND_STRUCT_METHOD(st, foldFrom, MARGS("next")))
};

static void test_nested_sub_commands()
{
  Setup s;
  test_assert(addNativeCommand<GrandParent>(s.w.rt, CommandDef("review.grandparent")).ok());
  g_flag = false;
  test_assert(s.ex.run(s.ctx, "review.grandparent", PropertyBag()).ok());
  test_assert(s.w.tool.count == 15 && s.history.at(0).subCount() == 1);
  g_log.clear();
  test_assert(s.ex.undo(s.ctx).ok() && s.w.tool.count == 4);
  test_assert(g_log == "undo;undo;parent-undo;grandparent-undo;");
  g_log.clear();
  test_assert(s.ex.redo(s.ctx).ok() && s.w.tool.count == 15); /* re-exec at depth */
  test_assert(g_log == "discard;discard;exec1;exec10;" && s.history.at(0).subCount() == 1);
  /* A failure at the top rolls back every level, innermost last-run first. */
  test_assert(s.ex.undo(s.ctx).ok());
  g_flag = true;
  g_log.clear();
  test_assert(hasMsg(s.ex.run(s.ctx, "review.grandparent", PropertyBag()).status(),
                     "grandparent failed"));
  g_flag = false;
  test_assert(s.w.tool.count == 4);
  test_assert(g_log == "exec1;exec10;undo;undo;parent-undo;discard;discard;");
}

static void test_budget_across_a_barrier()
{
  Setup s;
  const CommandDef *barrier = *addNativeCommand<AddCount>(
      s.w.rt, CommandDef("review.barrier")
                  .flags(CommandFlags::UndoBarrier)
                  .input(PropertyDef::intProp("amount"))
                  .input(PropertyDef::intProp("mem")));
  test_assert(s.runAdd(1, 100).ok() && s.runAdd(1, 100).ok());
  test_assert(s.ex.run(s.ctx, *barrier, barrier->newInputs()).ok());
  test_assert(s.runAdd(1, 100).ok());
  test_assert(s.ex.undo(s.ctx).ok() && !s.history.canUndo()); /* the barrier holds */
  test_assert(s.ex.redo(s.ctx).ok());
  s.history.setMemoryBudget(150); /* the two before the barrier go */
  test_assert(s.history.size() == 1 && s.history.memoryUsed() == 100);
  test_assert(s.history.validate().ok() && s.history.canUndo());
  test_assert(s.ex.undo(s.ctx).ok() && !s.history.canUndo());
}

static void test_discard_exactly_once()
{
  g_made = g_discards = 0;
  {
    Setup s;
    CommandDef d("review.counted");
    for (const char *in : {"refuse", "fail", "key", "mem"}) {
      d.input(PropertyDef::intProp(in));
    }
    const CommandDef *def = *addNativeCommand<Counted>(s.w.rt, std::move(d));
    auto run = [&](int64_t refuse, int64_t fail, int64_t key, int64_t mem) {
      PropertyBag in = def->newInputs();
      test_assert(in.set("refuse", PropValue::ofInt(refuse)).ok());
      test_assert(in.set("fail", PropValue::ofInt(fail)).ok());
      test_assert(in.set("key", PropValue::ofInt(key)).ok());
      test_assert(in.set("mem", PropValue::ofInt(mem)).ok());
      return s.ex.run(s.ctx, *def, std::move(in)).status();
    };
    test_assert(!run(1, 0, 0, 0).ok() && g_discards == 1); /* canRun refused */
    test_assert(!run(0, 1, 0, 0).ok() && g_discards == 2); /* exec failed */
    test_assert(run(0, 0, 5, 0).ok() && run(0, 0, 5, 0).ok() && g_discards == 3); /* fold */
    test_assert(s.ex.undo(s.ctx).ok() && run(0, 0, 0, 0).ok() && g_discards == 4); /* truncation */
    PropertyBag again = def->newInputs();
    test_assert(s.ex.rerunHead(s.ctx, std::move(again)).ok() && g_discards == 5);
    PropertyBag bad = def->newInputs();
    test_assert(bad.set("fail", PropValue::ofInt(1)).ok());
    test_assert(!s.ex.rerunHead(s.ctx, std::move(bad)).ok() && g_discards == 6);
    test_assert(run(0, 0, 0, 1000).ok());
    s.history.setMemoryBudget(999); /* the older entry goes */
    test_assert(g_discards == 7 && s.history.size() == 1);
  } /* the history's destructor discards the rest */
  test_assert(g_made == g_discards);
}

/* ---- from the bind/ pressure test (plan, "P1f step 4") ---- */

/* Fails only through the sticky error, as a failed foreign slot does: exec when
 * g_stickyExec is set, redo (when bound) when g_stickyRedo is set. */
static bool g_stickyExec = false;
static bool g_stickyRedo = false;
struct StickyInc : Inc {
  Status exec(Context &ctx)
  {
    if (g_stickyExec) {
      ctx.runtime().setStickyError(Status::error(StatusCode::Internal, "exec threw"));
      return Status();
    }
    return Inc::exec(ctx);
  }
  Status redo(Context &ctx)
  {
    if (g_stickyRedo) {
      ctx.runtime().setStickyError(Status::error(StatusCode::Internal, "redo threw"));
      return Status();
    }
    return Inc::exec(ctx);
  }
  BIND_ALL(StickyInc, "review::StickyInc", )
};
struct StickyIncRedo : StickyInc {
  BIND_ALL(StickyIncRedo, "review::StickyIncRedo", BIND_STRUCT_METHOD(st, redo, MARGS("ctx")))
};
/* foldKey without foldFrom. */
struct HalfFold : Inc {
  uint64_t foldKey() const
  {
    return 1;
  }
  BIND_ALL(HalfFold, "review::HalfFold", BIND_OPTIONAL_METHOD(st, foldKey, MARGS()))
};

template <class T> static void checkStickyRedo(const char *id)
{
  Setup s;
  test_assert(addNativeCommand<T>(s.w.rt, CommandDef(id)).ok());
  const int64_t start = s.w.tool.count;
  g_stickyExec = g_stickyRedo = false;
  test_assert(s.ex.run(s.ctx, id, PropertyBag()).ok() && s.ex.undo(s.ctx).ok());
  g_stickyExec = g_stickyRedo = true;
  Status r = s.ex.redo(s.ctx);
  g_stickyExec = g_stickyRedo = false;
  test_assert(!r.ok() && strstr(r.message(), "threw") != nullptr);
  /* Not redone, so nothing to undo: the cursor stayed put. */
  test_assert(s.w.tool.count == start && !s.history.canUndo() && s.history.validate().ok());
  test_assert(!s.ex.undo(s.ctx).ok() && s.w.tool.count == start);
  test_assert(!s.w.rt.hasStickyError());
}

static void test_redo_sees_the_sticky_error()
{
  checkStickyRedo<StickyInc>("review.stickyExec");     /* redo by exec */
  checkStickyRedo<StickyIncRedo>("review.stickyRedo"); /* a redo slot */
}

static void test_failed_add_registers_nothing()
{
  Setup s;
  Runtime &rt = s.w.rt;
  const int tables = rt.ifaces().tableCount();
  /* Interactive without redo; then the same id with a redo slot. */
  test_assert(!addNativeCommand<Inc>(rt, CommandDef("review.live").flags(CommandFlags::Interactive)).ok());
  test_assert(rt.ifaces().tableCount() == tables);
  test_assert(addNativeCommand<StickyIncRedo>(rt, CommandDef("review.live").flags(CommandFlags::Interactive)).ok());
  /* A lone foldKey; then the same id without it. */
  const int tables2 = rt.ifaces().tableCount();
  test_assert(!addNativeCommand<HalfFold>(rt, CommandDef("review.half")).ok());
  test_assert(rt.ifaces().tableCount() == tables2);
  test_assert(addNativeCommand<Inc>(rt, CommandDef("review.half")).ok());
  /* An id collision is refused before the implementation is registered. */
  test_assert(rt.debugRegisterNameAs(IdDomain::Command, CommandId::of("review.collide").value, "review.other").ok());
  const int tables3 = rt.ifaces().tableCount();
  Status c = addNativeCommand<Inc>(rt, CommandDef("review.collide")).status();
  test_assert(c.code() == StatusCode::AlreadyExists && rt.ifaces().tableCount() == tables3);
}

/* A runaway command fails at the nesting cap and leaves the executor idle. */
static void test_nesting_cap()
{
  Setup s;
  test_assert(addNativeCommand<Recurse>(s.w.rt, CommandDef("review.recurse")).ok());
  g_depth = 0;
  Status r = s.ex.run(s.ctx, "review.recurse", PropertyBag()).status();
  test_assert(r.code() == StatusCode::OutOfRange && hasMsg(r, "nested deeper than 32"));
  test_assert(g_depth == CommandExecutor::kMaxNesting);
  test_assert(!s.ex.running() && s.history.size() == 0 && s.history.validate().ok());
  test_assert(s.ex.run(s.ctx, *s.add, s.add->newInputs()).ok() && s.history.size() == 1);
}

int main()
{
  test_nested_sub_commands();
  test_budget_across_a_barrier();
  test_discard_exactly_once();
  test_reentrancy();
  test_failed_redo_rolls_back();
  test_input_bags_are_checked();
  test_rerun_head_refusals();
  test_folding();
  test_sticky_errors();
  test_executor_lifetime_and_binding();
  test_history_pins();
  test_delete_cascade_states();
  test_literal_edges();
  test_last_used_load_continues();
  test_registration_retry();
  test_memsize_once_per_push();
  test_set_value_undo_inside_exec();
  test_redo_sees_the_sticky_error();
  test_failed_add_registers_nothing();
  test_nesting_cap();
  return test_end();
}
