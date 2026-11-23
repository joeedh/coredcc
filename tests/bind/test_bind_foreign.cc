/* Script commands through a fake foreign host: registration, the executor and history
 * paths, failures, handle releases and the reentrancy guard (P1f step 4). */
#include "script_fixture.h"

#include "coredcc/lib/provider.h"

#include <cstring>

test_init;

static void test_registration_refusals()
{
  Host host;
  {
    Setup s;
    Script k;
    Runtime &rt = s.w.rt;
    const int tables = rt.ifaces().tableCount();
    struct Case {
      const char *slots;
      uint32_t flags;
      StatusCode code;
    };
    const std::string core = kCore;
    const Case cases[] = {
        {"canRun exec memSize discard", 0, StatusCode::TypeMismatch}, /* no undo */
        {"canRun exec undo memSize discard foldKey foldFrom", 0, StatusCode::Unsupported},
        {"canRun exec undo memSize discard foldKey", 0, StatusCode::InvalidArgument},
        {kCore, uint32_t(CommandFlags::Interactive), StatusCode::InvalidArgument},
        {"canRun exec undo memSize discard bogus", 0, StatusCode::TypeMismatch},
    };
    for (const Case &c : cases) {
      fakehost::lf::Handle f = 0;
      Status st = registerScript(s, host, &k, "js.a", c.slots, c.flags, &f);
      test_assert(!st.ok() && st.code() == c.code);
      test_assert(rt.ifaces().tableCount() == tables);
      test_assert(rt.ext<CommandRegistry>().find("js.a") == nullptr);
      test_assert(host.releasesOf(f) == 1);
    }
    /* No factory: nothing to release. */
    CommandDef *def = sc::newCommandDef("js.a");
    test_assert(sc::registerForeignCommand(s.ctx, *def, 0, kCore).code() ==
                StatusCode::InvalidArgument);
    litestl::alloc::Delete(def);
    /* The corrected retry succeeds; the factory is kept. */
    fakehost::lf::Handle f = 0;
    test_assert(registerScript(s, host, &k, "js.a", kCore, 0, &f).ok());
    test_assert(rt.ext<CommandRegistry>().find("js.a") != nullptr && host.releasesOf(f) == 0);
    /* A second registration of the id is refused, and its factory released. */
    fakehost::lf::Handle f2 = 0;
    test_assert(registerScript(s, host, &k, "js.a", kCore, 0, &f2).code() ==
                StatusCode::AlreadyExists);
    test_assert(host.releasesOf(f2) == 1);
    test_assert(rt.validateAll().ok() && !rt.hasStickyError());
  }
  test_assert(host.live() == 0 && host.allReleasedOnce());
}

static void test_def_facades()
{
  CommandDef *def = sc::newCommandDef("js.def");
  test_assert(sc::defUiName(*def, "Script Def").ok());
  test_assert(strcmp(def->uiNameText().c_str(), "Script Def") == 0);
  test_assert(sc::defFlags(*def, 1u << 10).code() == StatusCode::InvalidArgument);
  test_assert(sc::defFlags(*def, uint32_t(CommandFlags::NoUndo)).ok());
  test_assert(def->has(CommandFlags::NoUndo));
  test_assert(sc::defInput(*def, "m", PropValue::identityMat4()).code() ==
              StatusCode::InvalidArgument);
  test_assert(sc::defOutput(*def, "e", PropValue::ofEnum(1)).code() ==
              StatusCode::InvalidArgument);
  test_assert(sc::defInput(*def, "b", sc::makeBool(true)).ok());
  test_assert(sc::defInput(*def, "f", sc::makeFloat(0.5)).ok());
  test_assert(sc::defInput(*def, "s", sc::makeString("x")).ok());
  test_assert(sc::defInput(*def, "v", sc::makeVec3(1, 2, 3)).ok());
  test_assert(def->inputCount() == 4 && def->inputDef(3).kind() == PropKind::Vec3);
  test_assert(def->inputDef(3).defaultValue() == sc::makeVec3(1, 2, 3));
  litestl::alloc::Delete(def);
}

static void test_run_undo_redo()
{
  Host host;
  Script k;
  {
    Setup s;
    test_assert(registerScript(s, host, &k, "js.add", kCore).ok());
    const int64_t c0 = count(s.ctx);
    sc::BagResult out = sc::invoke(s.ctx, "js.add(amount=3)");
    test_assert(out.status.ok() && count(s.ctx) == c0 + 3);
    test_assert(*sc::bagGet(out.value, "result").value.tryInt() == c0 + 3);
    test_assert(sc::undo(s.ctx).ok() && count(s.ctx) == c0 && k.undos == 1);
    /* No redo slot: redo runs exec again. */
    test_assert(sc::redo(s.ctx).ok() && count(s.ctx) == c0 + 3 && k.execs == 2);
    test_assert(s.history.validate().ok() && !s.w.rt.hasStickyError());
    test_assert(!sc::inForeignCall(s.w.rt));

    /* With a redo slot, redo calls it. */
    test_assert(registerScript(s, host, &k, "js.addr", "canRun exec undo redo memSize discard")
                    .ok());
    test_assert(sc::invoke(s.ctx, "js.addr(amount=2)").status.ok());
    test_assert(sc::undo(s.ctx).ok() && sc::redo(s.ctx).ok());
    test_assert(k.redos == 1 && k.execs == 3 && count(s.ctx) == c0 + 5);
    test_assert(s.history.validate().ok());
  }
  /* History teardown discarded every instance, then the Runtime released the factories. */
  test_assert(k.discards == k.makes && host.live() == 0 && host.allReleasedOnce());
}

static void test_failing_slots()
{
  Host host;
  Script k;
  {
    Setup s;
    test_assert(registerScript(s, host, &k, "js.add", kCore).ok());
    test_assert(registerScript(s, host, &k, "js.addr", "canRun exec undo redo memSize discard")
                    .ok());
    const int64_t c0 = count(s.ctx);
    const int liveBefore = host.live();

    /* A throwing exec is the run's error, with the host's message. */
    k.failExecAt = k.execs + 1;
    sc::BagResult r = sc::invoke(s.ctx, "js.add()");
    test_assert(!r.status.ok() && strstr(r.status.message(), "boom") != nullptr);
    test_assert(count(s.ctx) == c0 && s.history.size() == 0 && host.live() == liveBefore);
    test_assert(!s.w.rt.hasStickyError());

    /* A throwing redo-by-exec leaves the cursor where it was. */
    test_assert(sc::invoke(s.ctx, "js.add(amount=4)").status.ok() && sc::undo(s.ctx).ok());
    k.failExecAt = k.execs + 1;
    Status rd = sc::redo(s.ctx);
    test_assert(!rd.ok() && strstr(rd.message(), "boom") != nullptr);
    test_assert(count(s.ctx) == c0 && !s.history.canUndo() && s.history.validate().ok());
    test_assert(!sc::undo(s.ctx).ok() && count(s.ctx) == c0 && !s.w.rt.hasStickyError());

    /* And so does a throwing redo slot. */
    test_assert(sc::invoke(s.ctx, "js.addr(amount=4)").status.ok() && sc::undo(s.ctx).ok());
    k.failRedo = true;
    rd = sc::redo(s.ctx);
    k.failRedo = false;
    test_assert(!rd.ok() && strstr(rd.message(), "no redo") != nullptr);
    test_assert(count(s.ctx) == c0 && !s.history.canUndo() && s.history.validate().ok());
    test_assert(!s.w.rt.hasStickyError());
  }
  test_assert(k.discards == k.makes && host.live() == 0 && host.allReleasedOnce());
}

static void test_factory_failures()
{
  Host host;
  Script k;
  {
    Setup s;
    test_assert(registerScript(s, host, &k, "js.add", kCore).ok());
    k.failMake = true;
    sc::BagResult r = sc::invoke(s.ctx, "js.add()");
    k.failMake = false;
    test_assert(!r.status.ok() && strstr(r.status.message(), "no instances today") != nullptr);
    k.makeZero = true;
    r = sc::invoke(s.ctx, "js.add()");
    k.makeZero = false;
    test_assert(!r.status.ok() && strstr(r.status.message(), "no instance") != nullptr);
    test_assert(s.history.size() == 0 && !s.w.rt.hasStickyError());
    test_assert(sc::invoke(s.ctx, "js.add()").status.ok());
  }
  test_assert(host.releasesOf(0) == 0 && host.live() == 0 && host.allReleasedOnce());
}

static void test_reentrancy_guard()
{
  Host host;
  Script k;
  {
    Setup s;
    k.ctx = &s.ctx;
    test_assert(registerScript(s, host, &k, "js.add", kCore).ok());
    const int64_t c0 = count(s.ctx);
    test_assert(sc::invoke(s.ctx, "js.add(amount=1)").status.ok());

    /* setValue from undo would push into the history the undo runs from. */
    k.probeUndo = true;
    test_assert(sc::undo(s.ctx).ok() && count(s.ctx) == c0);
    k.probeUndo = false;
    test_assert(k.undoSetValue.code() == StatusCode::Busy);
    test_assert(sc::redo(s.ctx).ok());

    /* A discard run by the host's clear() calls back in: Busy, not an abort. */
    Status inv, und, red, set;
    k.onDiscard = [&]() {
      inv = sc::invoke(*k.ctx, "test.add(amount=1)").status;
      und = sc::undo(*k.ctx);
      red = sc::redo(*k.ctx);
      set = sc::setValue(*k.ctx, "tool.count", sc::makeInt(0));
    };
    s.history.clear();
    k.onDiscard = nullptr;
    test_assert(inv.code() == StatusCode::Busy && und.code() == StatusCode::Busy);
    test_assert(red.code() == StatusCode::Busy && set.code() == StatusCode::Busy);
    test_assert(count(s.ctx) == c0 + 1 && !sc::inForeignCall(s.w.rt));
    test_assert(!s.w.rt.hasStickyError());
  }
  test_assert(host.live() == 0 && host.allReleasedOnce());
}

static void test_set_value_in_exec()
{
  Host host;
  Script k;
  {
    Setup s;
    k.useSetValue = true;
    test_assert(registerScript(s, host, &k, "js.set", kCore).ok());
    const int64_t c0 = count(s.ctx);
    test_assert(sc::invoke(s.ctx, "js.set(amount=7)").status.ok() && count(s.ctx) == c0 + 7);
    test_assert(s.history.size() == 1 && s.history.at(0).subCount() == 1);
    /* The sub-command is undone before the script's own undo. */
    test_assert(sc::undo(s.ctx).ok() && count(s.ctx) == c0);
    test_assert(sc::redo(s.ctx).ok() && count(s.ctx) == c0 + 7);
    test_assert(s.history.validate().ok() && !s.w.rt.hasStickyError());
  }
  test_assert(host.live() == 0 && host.allReleasedOnce());
}

static void test_foreign_implementable()
{
  test_assert(sc::foreignImplementable(detail::ifaceDesc<Command>()).ok());
  Status bp = sc::foreignImplementable(detail::ifaceDesc<BlockProvider>());
  test_assert(bp.code() == StatusCode::Unsupported && strstr(bp.message(), "data") != nullptr);
}

int main()
{
  test_registration_refusals();
  test_def_facades();
  test_run_undo_redo();
  test_failing_slots();
  test_factory_failures();
  test_reentrancy_guard();
  test_set_value_in_exec();
  test_foreign_implementable();
  return test_end();
}
