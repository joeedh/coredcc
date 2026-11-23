#pragma once

/** \file
 * Command implementations written in a script (JS): the foreign dispatcher, the
 * factory protocol, and registration (P1f step 4 in
 * docs/plans/2026-11-20-p1-application-spine.md).
 *
 * A script command reaches C++ as handles (litestl/binding/foreign.h): one for its
 * factory, and one per instance the factory makes. C++ holds one count of each and
 * releases it exactly once.
 *
 * **Teardown:** destroy the executor and the history before the Runtime. Their records
 * hold instances whose tables and dispatcher belong to the Runtime. A script's
 * `discard` or `undo` that fails while the host clears the history leaves a sticky
 * error, which the host takes (`rt.takeStickyError()`).
 */

#include "coredcc/base/iface/iface.h"
#include "coredcc/command/command.h"

#include "litestl/binding/foreign.h"
#include "litestl/util/string.h"
#include "litestl/util/vector.h"

#include <cstdint>

namespace coredcc::script {

using litestl::util::string;

/**
 * Makes the instances of one script command class. Implemented in TS (the generated
 * `ForeignFactoryImpl`); C++ only calls it through litestl::binding::foreign::call.
 * `make` returns a new instance's handle, carrying a count for C++.
 */
struct ForeignFactory {
  uint32_t make()
  {
    return 0;
  }
  static const litestl::binding::types::Struct<ForeignFactory> *defineBindings();
};

/**
 * The Runtime extension behind script commands: the dispatcher every foreign table
 * shares, the depth of foreign calls in progress, and the factory handles it owns.
 */
class ForeignBindings {
 public:
  static constexpr const char *kExtName = "coredcc.script.ForeignBindings";

  explicit ForeignBindings(Runtime *rt);
  /** Releases the factory handles. */
  ~ForeignBindings();
  ForeignBindings(const ForeignBindings &) = delete;
  ForeignBindings &operator=(const ForeignBindings &) = delete;

  ForeignDispatcher *dispatcher();
  /** foreign::call with the depth raised; Internal with the host's message on failure. */
  Status call(litestl::binding::foreign::Handle h,
              const litestl::binding::types::Method *m,
              void **args,
              void *ret);
  /** Foreign calls in progress. While it is above 0, facades that would reenter the
   * executor or the history are Busy. */
  int depth() const
  {
    return depth_;
  }
  /** Takes the count of a factory handle, released when the Runtime goes. */
  void keepFactory(litestl::binding::foreign::Handle h);

 private:
  class Dispatcher;
  Dispatcher *dispatcher_;
  int depth_ = 0;
  litestl::util::Vector<litestl::binding::foreign::Handle> factories_;
};

/** Whether a foreign call is in progress on `rt`. */
bool inForeignCall(const Runtime &rt);

/**
 * Ok if every slot of `iface` can be implemented by a script: none takes or returns a
 * `void *` or a QueryAnswer. Else Unsupported, naming the slots.
 */
Status foreignImplementable(const lbt::_StructBase *iface);

/* ---- CommandDef builder facades ---- */

/** A new def for command `id` (dotted). Owned by the caller until registered. */
CommandDef *newCommandDef(const string &id);
Status defUiName(CommandDef &def, const string &s);
Status defFlags(CommandDef &def, uint32_t flags);
/** An input whose kind and default come from `defaultValue`: Bool, Int, Float, String
 * or Vec2-4. Other types are InvalidArgument. */
Status defInput(CommandDef &def, const string &name, const PropValue &defaultValue);
/** An output, typed by `defaultValue` as for defInput. */
Status defOutput(CommandDef &def, const string &name, const PropValue &defaultValue);

/**
 * Registers a script command: `def` (moved from, so register it once), made by the
 * factory `factory`, whose instances implement the space-separated Command slots in
 * `slots`.
 *
 * C++ takes the factory's count whatever the outcome, and releases it on failure.
 * Folding (`foldKey`, `foldFrom`) isn't supported from scripts yet, and is refused.
 * A refused registration leaves no table and no def behind.
 */
Status registerForeignCommand(Context &ctx,
                              CommandDef &def,
                              uint32_t factory,
                              const string &slots);

} // namespace coredcc::script
