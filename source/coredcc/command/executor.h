#pragma once

/** \file
 * Running commands: CommandRecord (one run and its sub-commands), UndoHistory and its
 * built-in LinearHistory, and CommandExecutor (architecture §6, "Executor and pluggable
 * history", "Interactive commands").
 */

#include "coredcc/command/command.h"

#include "litestl/util/function.h"

#include <memory>
#include <optional>
#include <string_view>

namespace coredcc {

class LastUsedStore;

/**
 * One run of a command: the instance, its bags, and the sub-commands its exec ran
 * (undone in reverse before its own undo). Owned by the executor while running, then by
 * the history.
 *
 * Because the sub-commands are undone first, a command whose own work depends on a
 * sub-command's result (it sets a property on a block a sub-command created) should do
 * that work in a sub-command too.
 */
class CommandRecord {
 public:
  CommandRecord(const CommandDef *def, Owned<Command> cmd, PropertyBag inputs);
  ~CommandRecord();
  CommandRecord(const CommandRecord &) = delete;
  CommandRecord &operator=(const CommandRecord &) = delete;

  const CommandDef &def() const
  {
    return *def_;
  }
  Command &command()
  {
    return cmd_.get();
  }
  const PropertyBag &inputs() const
  {
    return inputs_;
  }
  const PropertyBag &outputs() const
  {
    return outputs_;
  }
  PropertyBag &outputs()
  {
    return outputs_;
  }
  int subCount() const
  {
    return int(subs_.size());
  }

  /** Bytes held for undo, sub-commands included. */
  uint64_t memSize() const;
  Status exec(Context &ctx);
  /** Sub-commands in reverse, then the command. */
  void undo(Context &ctx);
  /** The `redo` slot after redoing the sub-commands in order; without one, the
   * sub-commands are dropped and `exec` runs again (it makes new ones). A failure undoes
   * what this redo applied before returning. */
  Status redo(Context &ctx);
  /** Undoes and drops the sub-commands only (a failed exec). */
  void rollBackSubs(Context &ctx);
  /** Tells the command and its sub-commands they leave the history. */
  void discard();
  /** Whether the def's factory made the command (so the inputs alone can re-run it),
   * rather than runInstance. */
  bool fromFactory() const
  {
    return fromFactory_;
  }
  /** The address of the struct that its path prefix named when it started, which is the prefix
   * of every slot, undo and redo too; null if it had none. */
  const StableRef *prefix() const
  {
    return prefix_ ? &*prefix_ : nullptr;
  }
  /** After a fold: the head shows the folded run's inputs and outputs. */
  void adoptBags(CommandRecord &from);

 private:
  friend class CommandExecutor;
  friend class LinearHistory;
  const CommandDef *def_;
  Owned<Command> cmd_;
  PropertyBag inputs_;
  PropertyBag outputs_;
  litestl::util::Vector<CommandRecord *> subs_;
  bool fromFactory_ = false;
  std::optional<StableRef> prefix_;
  /** memSize() when the history took it, for the budget's running total. */
  uint64_t cachedSize_ = 0;
};

struct RecordDeleter {
  void operator()(CommandRecord *r) const;
};
using RecordPtr = std::unique_ptr<CommandRecord, RecordDeleter>;

/**
 * Where finished top-level commands go. Native hosts with their own undo (Unreal,
 * Maya) implement this as an adaptor; LinearHistory is the built-in one.
 *
 * Records call `discard` without a Context, so commands that need the Library keep a
 * pointer to it: destroy (or clear) the history before the Library.
 */
class UndoHistory {
 public:
  virtual ~UndoHistory() = default;
  /** Takes a finished command, folding it into the head when they agree to. */
  virtual void push(RecordPtr rec) = 0;
  /** Nothing before this point can be undone. */
  virtual void pushBarrier() = 0;
  virtual bool canUndo() const = 0;
  virtual bool canRedo() const = 0;
  virtual Status undo(Context &ctx) = 0;
  virtual Status redo(Context &ctx) = 0;
  /** The command an undo would undo, if nothing has been undone since it was pushed
   * (the last-command panel's target); else null. */
  virtual CommandRecord *head() = 0;
  /** Removes head() without undoing or discarding it. The one before it is then head(), but
   * nothing folds into it. */
  virtual RecordPtr popHead() = 0;
  virtual void clear() = 0;
};

/**
 * A cursor over a list of commands.
 * - Pushing truncates the redo side, discarding it.
 * - A push folds into the head only when the head is at the cursor, nothing has been
 *   undone or redone since it was pushed, neither record has sub-commands, and both ran with
 *   the same path prefix.
 * - Past the memory budget the oldest commands are discarded; the newest always stays.
 *   A record's memSize is read when it is pushed or folded into, and after its undo and redo
 *   (what it holds changes); the budget is applied at the next push.
 * - Barriers stop undo.
 * - A failed redo has undone its own partial work; that command and everything after it
 *   are dropped.
 * - A failed undo (one that leaves a sticky error) drops that command and everything
 *   after it too: what it left behind is unknown, so redoing over it isn't safe.
 * - Not reentrant: a slot that calls back into the history is refused (Busy) or aborts.
 */
class LinearHistory final : public UndoHistory {
 public:
  LinearHistory() = default;
  ~LinearHistory() override;

  void push(RecordPtr rec) override;
  void pushBarrier() override;
  bool canUndo() const override;
  bool canRedo() const override;
  Status undo(Context &ctx) override;
  Status redo(Context &ctx) override;
  CommandRecord *head() override;
  RecordPtr popHead() override;
  void clear() override;

  void setMemoryBudget(uint64_t bytes);
  uint64_t memoryUsed() const;
  int size() const
  {
    return int(entries_.size());
  }
  int cursor() const
  {
    return cursor_;
  }
  const CommandRecord &at(int i) const
  {
    return *entries_[i];
  }
  Status validate() const;

 private:
  void truncateRedo();
  void enforceBudget();
  /** An undone command may hold more, or less. */
  void measure(CommandRecord &rec);
  void drop(CommandRecord *r);

  litestl::util::Vector<CommandRecord *> entries_;
  int cursor_ = 0;
  uint64_t used_ = 0;
  bool inCall_ = false;
  /** Undo stops here (a barrier). */
  int floor_ = 0;
  /** Nothing was undone or redone since the last push. */
  bool headValid_ = false;
  bool headFoldable_ = false;
  uint64_t budget_ = uint64_t(256) << 20;
};

/**
 * Runs commands: canRun, exec with sub-command grouping, LastUsed values, and the hand
 * off to an UndoHistory (none: nothing is undoable).
 *
 * - Input bags are checked against the def: a run gets `def.newInputs()` with the
 *   caller's set values copied in. Unknown names and values that don't coerce are
 *   InvalidArgument.
 * - A failed exec, or a sticky error left by it, undoes its sub-commands in reverse and
 *   returns the error. Nothing reaches the history.
 * - Every instance the executor makes or takes is discarded exactly once, including
 *   one whose canRun refused.
 * - Sticky errors are collected after every slot call the executor makes and returned
 *   from that call. One pending before a top-level call came from outside the executor:
 *   the call takes it and fails with FailedPrecondition, naming it.
 * - While an interactive command is open, run is Busy, undo and redo only cancel it,
 *   and library collection is Busy.
 * - From inside a command's slots, only `runSub` is allowed, and only from `exec`. Every
 *   other call (run, undo, redo, rerunHead, the interactive calls) is refused.
 * - While a graph evaluates (Runtime::evaluating) every call is Busy, before any other
 *   check and with nothing changed: an open interactive command stays open, and a pending
 *   sticky error stays pending. LinearHistory's undo and redo are too.
 * - A Context with no executor is bound to this one for the length of each call, then
 *   unbound. One bound to another executor is FailedPrecondition. `invoke` and
 *   `setValueUndo` need `ctx.setExecutor` first.
 */
class CommandExecutor {
 public:
  CommandExecutor(Runtime *rt, UndoHistory *history);
  ~CommandExecutor();
  CommandExecutor(const CommandExecutor &) = delete;
  CommandExecutor &operator=(const CommandExecutor &) = delete;

  /** Runs a command with `inputs` (from def.newInputs()); returns its outputs. */
  Result<PropertyBag> run(Context &ctx, const CommandDef &def, PropertyBag inputs);
  Result<PropertyBag> run(Context &ctx, std::string_view id, PropertyBag inputs);
  /** Runs an instance made elsewhere (SetPropertyCommand), as `def`. */
  Result<PropertyBag> runInstance(Context &ctx,
                                  const CommandDef &def,
                                  Owned<Command> cmd,
                                  PropertyBag inputs);
  /** From inside exec: runs a command into the running command's group. */
  Result<PropertyBag> runSub(Context &ctx, const CommandDef &def, PropertyBag inputs);
  Result<PropertyBag> runSub(Context &ctx, Owned<Command> cmd, const CommandDef &def);

  /** Undo or redo; while an interactive command is open, only cancels it. */
  Status undo(Context &ctx);
  Status redo(Context &ctx);

  /**
   * Re-runs the last command with new inputs (the last-command panel): undo it, run it
   * again. If the new run fails, the old run is redone and put back, and the error is
   * returned; if that fails too, the command is dropped and both errors are reported.
   *
   * Refused before anything is undone for Interactive commands (their values come from
   * the drag) and for commands made by runInstance (setValueUndo: their state isn't in
   * the inputs).
   */
  Status rerunHead(Context &ctx, PropertyBag newInputs);

  /**
   * Undoes and drops the commands at the history's head that `which` is true for, the newest
   * first, until one isn't: a drag that is cancelled. They can't be redone. If undoing one leaves
   * an error, it is dropped, and that is the end.
   */
  Status revertHead(Context &ctx, litestl::util::function_ref<bool(CommandRecord &rec)> which);

  /** Starts an Interactive command: exec captures state, then the tool edits live.
   * `ctx` must outlive the open command: destroying the executor cancels through it. */
  Status beginInteractive(Context &ctx, const CommandDef &def, PropertyBag inputs);
  bool interactiveOpen() const
  {
    return open_ != nullptr;
  }
  /** The open command, for the tool driving it; null when none is open. */
  Command *interactiveCommand();
  /** Runs `fn` on the open command with its bags and its path prefix in `ctx`, as in its slots,
   * and returns what it returns. For live edits; commands can't be run from it. A sticky error
   * that it leaves stays for commit. */
  Status updateInteractive(Context &ctx,
                           litestl::util::function_ref<Status(Context &ctx, Command &cmd)> fn);
  /** Records the open command. A sticky error left by the live phase cancels it
   * instead, and is returned. */
  Status commit(Context &ctx);
  Status cancel(Context &ctx);

  UndoHistory *history() const
  {
    return history_;
  }
  bool running() const
  {
    return int(stack_.size()) > 0;
  }

  /** How deep commands may nest through runSub: a command that runs itself (directly or
   * through others) fails with OutOfRange here instead of overflowing the stack. Debug
   * builds on Windows' 1 MB main-thread stack overflowed at 52 levels (2026-11-23). */
  static constexpr int kMaxNesting = 32;

 private:
  Result<RecordPtr> start(Context &ctx, const CommandDef &def, Owned<Command> cmd,
                          PropertyBag inputs);
  Status execRecord(Context &ctx, CommandRecord &rec);
  /** exec with `rec` on the sub-command stack, where runSub is allowed. */
  Status execFrame(Context &ctx, CommandRecord &rec);
  Result<PropertyBag> finish(Context &ctx, RecordPtr rec);
  Status busy(const char *what) const;
  /** Refuses a top-level call from inside a slot, or with a stray sticky error. */
  Status refuseNested(const char *what);
  /** `s`, with a pending sticky error taken and folded in. */
  Status collect(Status s);
  void discardRecord(CommandRecord &rec);
  Result<PropertyBag> subRun(Context &ctx, const CommandDef &def, Owned<Command> cmd,
                             PropertyBag inputs);

  /* A record's redo-by-exec pushes itself so the exec's runSub calls land in it. */
  friend class CommandRecord;

  Runtime &rt_;
  UndoHistory *history_;
  litestl::util::Vector<CommandRecord *> stack_;
  /** Slot calls other than exec in progress (exec frames reset it to 0). */
  int slotDepth_ = 0;
  RecordPtr open_;
  Context *openCtx_ = nullptr;
  class Library *openLib_ = nullptr;
};

/**
 * Runs `ctx`'s executor on an invocation string, `mesh.extrude(dist=1.0 mode='X')`:
 * arguments are parsed against the inputs' defs, and inputs not given take their
 * LastUsed values, then their defaults.
 */
Result<PropertyBag> invoke(Context &ctx, std::string_view invocation);

/** Builds the input bag for `def` from an invocation's arguments (unknown names are
 * NotFound with suggestions). A Pointer's datapath is resolved now, in `ctx`. */
Result<PropertyBag> inputsFromInvocation(Context &ctx, const CommandDef &def,
                                         const struct Invocation &inv);

} // namespace coredcc
