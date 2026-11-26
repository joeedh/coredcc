/* Regression tests for the bind/ feature review
 * (docs/reports/feature-report-bind-2026-11-22.md). Each test names its finding. */
#include "script_fixture.h"

#include <cstring>

test_init;

using sc::ForeignBindings;

static ForeignBindings &bindingsOf(Setup &s)
{
  return s.w.rt.ext<ForeignBindings>();
}

/* H1: instances are counted, so a wrong teardown order can be caught. */
static void test_live_instances_are_counted()
{
  Host host;
  Script k;
  {
    Setup s;
    test_assert(registerScript(s, host, &k, "js.add", kCore).ok());
    test_assert(sc::invoke(s.ctx, "js.add()").status.ok());
    test_assert(sc::invoke(s.ctx, "js.add()").status.ok());
    test_assert(bindingsOf(s).liveInstances() == 2);
    s.history.clear();
    test_assert(bindingsOf(s).liveInstances() == 0);
  }
  test_assert(host.allReleasedOnce());
}

/* A native command whose exec runs js.add as a sub-command. */
struct RunsScript {
  Status canRun(Context &) const
  {
    return Status();
  }
  Status exec(Context &ctx)
  {
    const CommandDef *d = ctx.runtime().ext<CommandRegistry>().find("js.add");
    return ctx.executor()->runSub(ctx, *d, d->newInputs()).status();
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
  static lbt::Struct<RunsScript> *defineBindings()
  {
    auto *st = new lbt::Struct<RunsScript>("test::RunsScript", sizeof(RunsScript));
    BIND_COMMAND_CORE(st);
    return st;
  }
};

/* B1: a script can't take a core command id; setValue notices a squatted one. */
static void test_core_ids_are_reserved()
{
  Host host;
  Script k;
  {
    Setup s;
    fakehost::lf::Handle f = 0;
    Status st = registerScript(s, host, &k, "core.property.set", kCore, 0, &f);
    test_assert(st.code() == StatusCode::InvalidArgument && host.releasesOf(f) == 1);
    for (const char *id : {"Core.Property.set", "core"}) {
      fakehost::lf::Handle g = 0;
      test_assert(registerScript(s, host, &k, id, kCore, 0, &g).code() ==
                  StatusCode::InvalidArgument);
    }
    test_assert(sc::setValue(s.ctx, "tool.strength", sc::makeFloat(0.75)).ok());
  }
  {
    Setup s;
    /* Native code taking the id under another implementation. */
    Runtime &rt = s.w.rt;
    if (!rt.ext<CommandRegistry>().find(kSetPropertyCommand)) {
      Status impl = registerImpl<Command, AddCount>(rt, "test.squat#impl");
      test_assert(impl.ok());
      CommandDef d(kSetPropertyCommand);
      d.impl("test.squat#impl", [](Runtime &r) {
        return own<Command>(
            r, "test.squat#impl", litestl::alloc::New<AddCount>("AddCount"));
      });
      test_assert(rt.ext<CommandRegistry>().add(std::move(d)).ok());
      test_assert(setValueUndo(s.ctx, "tool.strength", PropValue::ofFloat(0.75)).code() ==
                  StatusCode::AlreadyExists);
    }
  }
  test_assert(host.allReleasedOnce());
}

/* B2: a failed undo drops the command, so redo can't re-apply it over the un-undone
 * state. */
static void test_failed_undo_drops_the_command()
{
  Host host;
  Script k;
  {
    Setup s;
    test_assert(registerScript(s, host, &k, "js.add", kCore).ok());
    const int64_t c0 = count(s.ctx);
    test_assert(sc::invoke(s.ctx, "js.add(amount=2)").status.ok());
    k.failUndo = true;
    Status u = sc::undo(s.ctx);
    k.failUndo = false;
    test_assert(!u.ok() && strstr(u.message(), "undo failed") != nullptr);
    test_assert(strstr(u.message(), "dropped") != nullptr);
    test_assert(!s.history.canRedo() && !s.history.canUndo() && s.history.size() == 0);
    test_assert(!sc::redo(s.ctx).ok() && count(s.ctx) == c0 + 2 && k.execs == 1);
    test_assert(s.history.validate().ok() && !s.w.rt.hasStickyError());
  }
  test_assert(k.discards == k.makes && host.allReleasedOnce());
}

/* B4: a script command's exec can invoke another command, as a sub-command. Its own
 * write is a sub-command too, as work that depends on a sub-command's result must be. */
static void test_invoke_from_exec_is_a_sub_command()
{
  Host host;
  Script k;
  {
    Setup s;
    k.useSetValue = true;
    test_assert(registerScript(s, host, &k, "js.add", kCore).ok());
    Status sub;
    k.onExec = [&sub](Context &ctx) {
      sub = sc::invoke(ctx, "test.add(amount=10)").status;
    };
    const int64_t c0 = count(s.ctx);
    test_assert(sc::invoke(s.ctx, "js.add(amount=1)").status.ok() && sub.ok());
    k.onExec = nullptr;
    /* The script read the count after its sub-command ran. */
    test_assert(count(s.ctx) == c0 + 11 && s.history.size() == 1);
    test_assert(s.history.at(0).subCount() == 2);
    test_assert(sc::undo(s.ctx).ok() && count(s.ctx) == c0);
    test_assert(s.history.validate().ok() && !s.w.rt.hasStickyError());
  }
  test_assert(host.allReleasedOnce());
}

/* B5: the guard is per slot, and covers writeValue. */
static void test_facades_are_gated_per_slot()
{
  Host host;
  Script k;
  {
    Setup s;
    k.ctx = &s.ctx;
    test_assert(registerScript(s, host, &k, "js.add", kCore).ok());
    test_assert(addNativeCommand<RunsScript>(s.w.rt, CommandDef("test.runsScript")).ok());
    const int64_t c0 = count(s.ctx);
    Status set, write, inv;
    auto probe = [&](Context &ctx) {
      set = sc::setValue(ctx, "tool.count", sc::makeInt(100));
      write = sc::writeValue(ctx, "tool.count", sc::makeInt(100));
      inv = sc::invoke(ctx, "test.add()").status;
    };
    auto allBusy = [&]() {
      return set.code() == StatusCode::Busy && write.code() == StatusCode::Busy &&
             inv.code() == StatusCode::Busy;
    };
    /* canRun, at top level and as a native parent's sub-command. */
    k.onCanRun = probe;
    test_assert(sc::invoke(s.ctx, "js.add()").status.ok() && allBusy());
    set = write = inv = Status();
    test_assert(sc::invoke(s.ctx, "test.runsScript()").status.ok() && allBusy());
    k.onCanRun = nullptr;
    /* make. */
    k.onMake = [&]() { probe(*k.ctx); };
    test_assert(sc::invoke(s.ctx, "js.add()").status.ok() && allBusy());
    k.onMake = nullptr;
    /* discard, run by the host's clear(). */
    k.onDiscard = [&]() { probe(*k.ctx); };
    s.history.clear();
    k.onDiscard = nullptr;
    test_assert(allBusy());
    test_assert(count(s.ctx) == c0 + 15 && !s.w.rt.hasStickyError());
  }
  test_assert(host.allReleasedOnce());
}

/* B6: Enum and Flags values can be read; Pointer values round-trip. */
static void test_enum_flags_and_pointer_values()
{
  Setup s;
  Context &ctx = s.ctx;
  test_assert(sc::readEnum(sc::getValue(ctx, "objects[0].mode").value).value == 0);
  test_assert(sc::writeValue(ctx, "objects[0].mode", sc::makeString("EDIT").value).ok());
  test_assert(sc::readEnum(sc::getValue(ctx, "objects[0].mode").value).value == 1);
  test_assert(sc::readInt(sc::getValue(ctx, "objects[0].mode").value).status.code() ==
              StatusCode::InvalidArgument);
  s.w.object(0).flags = 3;
  test_assert(sc::readFlags(sc::getValue(ctx, "objects[0].flags").value).value == 3);
  test_assert(sc::readFlags(sc::makeInt(3)).status.code() == StatusCode::InvalidArgument);
  test_assert(sc::readEnum(sc::makeInt(3)).status.code() == StatusCode::InvalidArgument);
  test_assert(strcmp(sc::valueText(sc::makeVec2(1, 2)).c_str(), "(1, 2)") == 0);
  sc::ValueResult brush = sc::getValue(ctx, "tool.activeBrush");
  test_assert(brush.status.ok() && brush.value.type() == PropValue::Type::Pointer);
  test_assert(sc::setValue(ctx, "tool.activeBrush", brush.value).ok());
}

/* B7, B8, B9, B11: registration and builder checks. */
static void test_registration_checks()
{
  Host host;
  Script k;
  {
    Setup s;
    /* B7: the def is copied, so registering it again is a clear AlreadyExists. */
    CommandDef *def = sc::newCommandDef("js.twice");
    test_assert(sc::commandDefInput(*def, "amount", sc::makeInt(5)).ok());
    test_assert(
        sc::registerForeignCommand(s.ctx, *def, host.add(factoryObj(host, &k)), kCore)
            .ok());
    test_assert(
        sc::registerForeignCommand(s.ctx, *def, host.add(factoryObj(host, &k)), kCore)
            .code() == StatusCode::AlreadyExists);
    test_assert(sc::commandDefInput(*def, "extra", sc::makeInt(1)).ok());
    test_assert(s.w.rt.ext<CommandRegistry>().find("js.twice")->inputCount() == 1);
    /* B9: names are checked when they are added. */
    test_assert(sc::commandDefInput(*def, "", sc::makeInt(1)).code() ==
                StatusCode::InvalidArgument);
    test_assert(sc::commandDefInput(*def, "a b", sc::makeInt(1)).code() ==
                StatusCode::InvalidArgument);
    test_assert(sc::commandDefInput(*def, "amount", sc::makeInt(1)).code() ==
                StatusCode::AlreadyExists);
    test_assert(sc::commandDefOutput(*def, "r", sc::makeInt(1)).ok());
    test_assert(sc::commandDefOutput(*def, "r", sc::makeInt(1)).code() ==
                StatusCode::AlreadyExists);
    litestl::alloc::Delete(def);

    /* B8: ids must be dotted identifiers, so invoke can call them. */
    for (const char *bad : {"Not An Id!", "a..b", ".a", "a.", "a.1b"}) {
      fakehost::lf::Handle f = 0;
      test_assert(registerScript(s, host, &k, bad, kCore, 0, &f).code() ==
                  StatusCode::InvalidArgument);
      test_assert(host.releasesOf(f) == 1);
    }
    test_assert(registerScript(s, host, &k, "js.b_c.d2", kCore).ok());
    test_assert(sc::invoke(s.ctx, "js.b_c.d2()").status.ok());
  }
  test_assert(host.allReleasedOnce());
  /* B11. */
  test_assert(sc::newErrorStatus(StatusCode(99), "x").code() ==
              StatusCode::InvalidArgument);
  test_assert(sc::newErrorStatus(StatusCode::Busy, "x").code() == StatusCode::Busy);
}

static void test_can_undo_and_redo()
{
  Setup s;
  test_assert(s.ex.run(s.ctx, *s.add, s.add->newInputs()).ok());
  test_assert(sc::canUndo(s.ctx).value && !sc::canRedo(s.ctx).value);
  test_assert(sc::undo(s.ctx).ok());
  test_assert(!sc::canUndo(s.ctx).value && sc::canRedo(s.ctx).value);
  World w;
  Context bare = w.context();
  test_assert(sc::canUndo(bare).status.code() == StatusCode::FailedPrecondition);
  test_assert(sc::canRedo(bare).status.code() == StatusCode::FailedPrecondition);
}

/* Test gaps: canRun and byteSize failures, and releases on the history's other paths. */
static void test_slot_failures_and_history_paths()
{
  Host host;
  Script k;
  {
    Setup s;
    test_assert(registerScript(s, host, &k, "js.add", kCore).ok());
    test_assert(registerScript(s,
                               host,
                               &k,
                               "js.drag",
                               "canRun exec undo redo byteSize discard",
                               uint32_t(CommandFlags::Interactive))
                    .ok());
    ForeignBindings &fb = bindingsOf(s);
    const int64_t c0 = count(s.ctx);

    k.failCanRun = true;
    sc::BagResult r = sc::invoke(s.ctx, "js.add()");
    k.failCanRun = false;
    test_assert(strstr(r.status.message(), "canRun: Error: no") != nullptr);
    k.refuseCanRun = true;
    r = sc::invoke(s.ctx, "js.add()");
    k.refuseCanRun = false;
    test_assert(r.status.code() == StatusCode::FailedPrecondition);
    test_assert(count(s.ctx) == c0 && s.history.size() == 0 && fb.liveInstances() == 0);

    /* byteSize failing: applied and recorded, and reported (left to the TS adapter, B3).
     */
    k.failMemSize = true;
    r = sc::invoke(s.ctx, "js.add(amount=1)");
    k.failMemSize = false;
    test_assert(!r.status.ok() && s.history.size() == 1 && count(s.ctx) == c0 + 1);
    test_assert(s.history.validate().ok() && !s.w.rt.hasStickyError());

    /* Redo truncation and budget eviction each release an instance. */
    test_assert(sc::undo(s.ctx).ok() &&
                sc::invoke(s.ctx, "js.add(amount=1)").status.ok());
    test_assert(fb.liveInstances() == 1);
    s.history.setMemoryBudget(20); /* 16 bytes each: the newest stays */
    test_assert(sc::invoke(s.ctx, "js.add(amount=1)").status.ok());
    test_assert(s.history.size() == 1 && fb.liveInstances() == 1);

    /* rerunHead replaces the head's instance. */
    PropertyBag in = s.w.rt.ext<CommandRegistry>().find("js.add")->newInputs();
    test_assert(in.set("amount", PropValue::ofInt(3)).ok());
    test_assert(s.ex.rerunHead(s.ctx, std::move(in)).ok() && fb.liveInstances() == 1);

    /* An interactive script command, cancelled and committed. */
    s.history.setMemoryBudget(uint64_t(1) << 20);
    const CommandDef *drag = s.w.rt.ext<CommandRegistry>().find("js.drag");
    test_assert(s.ex.beginInteractive(s.ctx, *drag, drag->newInputs()).ok());
    test_assert(fb.liveInstances() == 2 && s.ex.cancel(s.ctx).ok());
    test_assert(fb.liveInstances() == 1);
    test_assert(s.ex.beginInteractive(s.ctx, *drag, drag->newInputs()).ok());
    test_assert(s.ex.commit(s.ctx).ok() && fb.liveInstances() == 2);
    test_assert(s.history.validate().ok() && !s.w.rt.hasStickyError());
  }
  test_assert(k.discards == k.makes && host.allReleasedOnce());
}

int main()
{
  test_live_instances_are_counted();
  test_core_ids_are_reserved();
  test_failed_undo_drops_the_command();
  test_invoke_from_exec_is_a_sub_command();
  test_facades_are_gated_per_slot();
  test_enum_flags_and_pointer_values();
  test_registration_checks();
  test_can_undo_and_redo();
  test_slot_failures_and_history_paths();
  return test_end();
}
