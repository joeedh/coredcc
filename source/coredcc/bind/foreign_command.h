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
#include <initializer_list>

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
 * shares, the foreign calls in progress, and the handles it owns.
 *
 * It counts the live script command instances. Destroying it (with the Runtime) while
 * any is alive aborts, naming the cause: a history or executor outlived the Runtime.
 */
class ForeignBindings {
 public:
  static constexpr const char *kExtName = "coredcc.script.ForeignBindings";

  explicit ForeignBindings(Runtime *rt);
  /** Releases the handles it keeps. Aborts if script command instances are alive. */
  ~ForeignBindings();
  ForeignBindings(const ForeignBindings &) = delete;
  ForeignBindings &operator=(const ForeignBindings &) = delete;

  ForeignDispatcher *dispatcher();
  /** foreign::call with method `m` of `iface` on the slot stack; Internal with the host's
   * message on failure. */
  Status call(litestl::binding::foreign::Handle h,
              const lbt::_StructBase *iface,
              const lbt::Method *m,
              void **args,
              void *ret);
  /** Foreign calls in progress. While it is above 0, facades that would reenter the
   * executor or the history are Busy, per slot (see facades.h). */
  int depth() const
  {
    return int(frames_.size());
  }
  /** A foreign call: a method of an interface, or of ForeignFactory. */
  struct Frame {
    const lbt::_StructBase *iface = nullptr;
    const lbt::Method *method = nullptr;
  };
  /** The innermost foreign call; null when none is in progress. */
  const Frame *currentFrame() const
  {
    return frames_.size() > 0 ? &frames_[int(frames_.size()) - 1] : nullptr;
  }
  /** Live script command instances, made by a factory and not yet destroyed. */
  int liveInstances() const
  {
    return live_;
  }
  void instanceMade()
  {
    live_++;
  }
  void instanceGone()
  {
    live_--;
  }
  /** Takes the count of a handle (a factory's, a kernel's), released when the Runtime
   * goes. */
  void keepHandle(litestl::binding::foreign::Handle h);
  /** Releases a kept handle now. */
  void dropHandle(litestl::binding::foreign::Handle h);

 private:
  class Dispatcher;
  Dispatcher *dispatcher_;
  litestl::util::Vector<Frame> frames_;
  int live_ = 0;
  litestl::util::Vector<litestl::binding::foreign::Handle> handles_;
};

/** Whether a foreign call is in progress on `rt`. */
bool inForeignCall(const Runtime &rt);
/** Whether the innermost foreign call on `rt` is one of `iface`'s methods; with `slots`, one
 * of those. False when none is in progress. */
bool inForeignSlot(const Runtime &rt,
                   const lbt::_StructBase *iface,
                   std::initializer_list<const char *> slots = {});

/**
 * Ok if every slot of `iface` can be implemented by a script. Else Unsupported, naming the
 * slots and why:
 * - a `void *` or a QueryAnswer, taken or returned;
 * - a reference returned, or ownership of a returned pointer;
 * - a struct returned by value without bound default and copy constructors (the script makes
 *   one, litestl copies it);
 * - a Vector returned whose elements aren't numbers, booleans or enums.
 */
Status foreignImplementable(const lbt::_StructBase *iface);

/* ---- CommandDef builder facades ---- */

/** A new def for command `id` (dotted). Owned by the caller, who disposes of it:
 * registration copies it. */
CommandDef *newCommandDef(const string &id);
Status defUiName(CommandDef &def, const string &s);
Status defDescription(CommandDef &def, const string &s);
Status defIcon(CommandDef &def, const string &s);
Status defFlags(CommandDef &def, uint32_t flags);
/** An input whose kind and default come from `defaultValue`: Bool, Int, Float, String
 * or Vec2-4. Other types are InvalidArgument, and so is a name that isn't an
 * identifier. A name the def's inputs already have is AlreadyExists. */
Status defInput(CommandDef &def, const string &name, const PropValue &defaultValue);
/** An output, typed and checked as for defInput. */
Status defOutput(CommandDef &def, const string &name, const PropValue &defaultValue);

/**
 * Registers a copy of `def` as a script command, made by the factory `factory`, whose
 * instances implement the space-separated Command slots in `slots`.
 *
 * - Each call takes one count of `factory`, whatever the outcome, and releases it on
 *   failure. Registering two commands with one factory needs two counts (two
 *   `foreignHandle` calls in TS).
 * - Folding (`foldKey`, `foldFrom`) isn't supported from scripts yet, and is refused.
 * - The `core.` namespace is reserved for coredcc's own commands.
 * - A refused registration leaves no table and no def behind.
 */
Status registerForeignCommand(Context &ctx,
                              const CommandDef &def,
                              uint32_t factory,
                              const string &slots);

} // namespace coredcc::script
