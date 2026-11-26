#pragma once

/* A fake script command class for the bind tests, run through fakehost::Host: shared
 * counters, switches that make a slot throw, and hooks that call facades from a slot. */

#include "../command/command_fixture.h"
#include "fake_host.h"

#include "coredcc/bind/facades.h"
#include "coredcc/bind/foreign_command.h"

#include <functional>
#include <memory>
#include <new>

namespace sc = coredcc::script;
using fakehost::Fn;
using fakehost::Host;
using fakehost::Obj;
using litestl::util::string;

/* The "JS class": shared counters and switches for every instance one factory makes. */
struct Script {
  int makes = 0, execs = 0, undos = 0, redos = 0, discards = 0;
  int failExecAt = 0; /* the n-th exec (1-based) throws */
  bool failRedo = false;
  bool failMake = false;
  bool makeZero = false;
  bool useSetValue = false; /* exec sets through setValue (a sub-command) */
  Status undoSetValue;      /* what setValue returned inside undo, when probed */
  bool probeUndo = false;
  Context *ctx = nullptr; /* for discard, which gets none */
  std::function<void()> onDiscard;
  /* From the review (tests/bind/test_bind_review.cc). */
  bool failCanRun = false;   /* canRun throws */
  bool refuseCanRun = false; /* canRun returns FailedPrecondition */
  bool failUndo = false;
  bool failMemSize = false;
  std::function<void(Context &)> onCanRun, onExec;
  std::function<void()> onMake;
};

inline bool ok(void *ret, Status s = Status())
{
  new (ret) Status(std::move(s));
  return true;
}

inline Obj commandObj(Script *s)
{
  auto before = std::make_shared<int64_t>(0);
  Obj o;
  o.methods["canRun"] = [s](void **args, void *ret, string *err) {
    if (s->onCanRun) {
      s->onCanRun(*static_cast<Context *>(args[0]));
    }
    if (s->failCanRun) {
      *err = "canRun: Error: no";
      return false;
    }
    return ok(ret,
              s->refuseCanRun ? Status::error(StatusCode::FailedPrecondition, "refused")
                              : Status());
  };
  o.methods["exec"] = [s, before](void **args, void *ret, string *err) {
    Context &ctx = *static_cast<Context *>(args[0]);
    if (++s->execs == s->failExecAt) {
      *err = "exec: Error: boom";
      return false;
    }
    if (s->onExec) {
      s->onExec(ctx);
    }
    const int64_t amount = *sc::commandInput(ctx, "amount").value.tryInt();
    *before = count(ctx);
    const PropValue v = sc::makeInt(*before + amount);
    Status st = s->useSetValue ? sc::setValue(ctx, "tool.count", v)
                               : sc::writeValue(ctx, "tool.count", v);
    test_assert(sc::setCommandOutput(ctx, "result", v).ok());
    return ok(ret, std::move(st));
  };
  o.methods["undo"] = [s, before](void **args, void *, string *err) {
    Context &ctx = *static_cast<Context *>(args[0]);
    s->undos++;
    if (s->failUndo) {
      *err = "undo: Error: undo failed";
      return false;
    }
    if (s->probeUndo) {
      s->undoSetValue = sc::setValue(ctx, "tool.count", sc::makeInt(-1));
    }
    if (!s->useSetValue) { /* else the executor undoes the sub-command */
      test_assert(sc::writeValue(ctx, "tool.count", sc::makeInt(*before)).ok());
    }
    return true;
  };
  o.methods["redo"] = [s, before](void **args, void *ret, string *err) {
    Context &ctx = *static_cast<Context *>(args[0]);
    s->redos++;
    if (s->failRedo) {
      *err = "redo: Error: no redo";
      return false;
    }
    const int64_t amount = *sc::commandInput(ctx, "amount").value.tryInt();
    return ok(ret, sc::writeValue(ctx, "tool.count", sc::makeInt(*before + amount)));
  };
  o.methods["memSize"] = [s](void **, void *ret, string *err) {
    if (s->failMemSize) {
      *err = "memSize: TypeError: expected a number or bigint";
      return false;
    }
    *static_cast<uint64_t *>(ret) = 16;
    return true;
  };
  o.methods["discard"] = [s](void **, void *, string *) {
    s->discards++;
    if (s->onDiscard) {
      s->onDiscard();
    }
    return true;
  };
  return o;
}

inline Obj factoryObj(Host &host, Script *s)
{
  Obj f;
  f.methods["make"] = [&host, s](void **, void *ret, string *err) {
    s->makes++;
    if (s->onMake) {
      s->onMake();
    }
    if (s->failMake) {
      *err = "make: Error: no instances today";
      return false;
    }
    *static_cast<uint32_t *>(ret) = s->makeZero ? 0 : host.add(commandObj(s));
    return true;
  };
  return f;
}

inline constexpr const char *kCore = "canRun exec undo memSize discard";

/* Registers command `id` (input `amount`, output `result`) with a new factory. */
inline Status registerScript(Setup &s,
                             Host &host,
                             Script *sc_,
                             const char *id,
                             const char *slots,
                             uint32_t flags = 0,
                             fakehost::lf::Handle *factory = nullptr)
{
  CommandDef *def = sc::newCommandDef(id);
  test_assert(sc::commandDefInput(*def, "amount", sc::makeInt(5)).ok());
  test_assert(sc::commandDefOutput(*def, "result", sc::makeInt(0)).ok());
  test_assert(sc::commandDefFlags(*def, flags).ok());
  const fakehost::lf::Handle f = host.add(factoryObj(host, sc_));
  if (factory) {
    *factory = f;
  }
  Status st = sc::registerForeignCommand(s.ctx, *def, f, slots);
  litestl::alloc::Delete(def);
  return st;
}
