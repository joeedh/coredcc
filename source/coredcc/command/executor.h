#pragma once

/** \file
 * Running commands: CommandRecord (one run and its sub-commands), UndoHistory and its
 * built-in LinearHistory, and CommandExecutor (architecture §6, "Executor and pluggable
 * history", "Interactive commands").
 */

#include "coredcc/command/command.h"

#include <memory>
#include <string_view>

namespace coredcc {

class LastUsedStore;

/**
 * One run of a command: the instance, its bags, and the sub-commands its exec ran
 * (undone in reverse before its own undo). Owned by the executor while running, then by
 * the history.
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
   * sub-commands are dropped and `exec` runs again (it makes new ones). */
  Status redo(Context &ctx);
  /** Undoes and drops the sub-commands only (a failed exec). */
  void rollBackSubs(Context &ctx);
  /** Tells the command and its sub-commands they leave the history. */
  void discard();

 private:
  friend class CommandExecutor;
  const CommandDef *def_;
  Owned<Command> cmd_;
  PropertyBag inputs_;
  PropertyBag outputs_;
  litestl::util::Vector<CommandRecord *> subs_;
};

struct RecordDeleter {
  void operator()(CommandRecord *r) const;
};
using RecordPtr = std::unique_ptr<CommandRecord, RecordDeleter>;

/**
 * Where finished top-level commands go. Native hosts with their own undo (Unreal,
 * Maya) implement this as an adaptor; LinearHistory is the built-in one.
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
  /** Removes head() without undoing or discarding it. */
  virtual RecordPtr popHead() = 0;
  virtual void clear() = 0;
};

/**
 * A cursor over a list of commands.
 * - Pushing truncates the redo side, discarding it.
 * - A push folds into the head only when the head is at the cursor and nothing has been
 *   undone or redone since it was pushed.
 * - Past the memory budget the oldest commands are discarded; the newest always stays.
 * - Barriers stop undo.
 * - A failed redo drops that command and everything after it.
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

  litestl::util::Vector<CommandRecord *> entries_;
  int cursor_ = 0;
  /** Undo stops here (a barrier). */
  int floor_ = 0;
  bool headFoldable_ = false;
  uint64_t budget_ = uint64_t(256) << 20;
};

/**
 * Runs commands: canRun, exec with sub-command grouping, LastUsed values, and the hand
 * off to an UndoHistory (none: nothing is undoable).
 *
 * - A failed exec, or a sticky error left by it, undoes its sub-commands in reverse and
 *   returns the error. Nothing reaches the history.
 * - While an interactive command is open, run is Busy, undo and redo only cancel it,
 *   and library collection is Busy.
 * - Calls from inside a command's slots: runSub only.
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
   * again. If the new run fails, the old inputs run again and the error is returned; if
   * that fails too, the command is dropped and both errors are reported.
   */
  Status rerunHead(Context &ctx, PropertyBag newInputs);

  /** Starts an Interactive command: exec captures state, then the tool edits live. */
  Status beginInteractive(Context &ctx, const CommandDef &def, PropertyBag inputs);
  bool interactiveOpen() const
  {
    return open_ != nullptr;
  }
  /** The open command, for the tool driving it; null when none is open. */
  Command *interactiveCommand();
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

 private:
  Result<RecordPtr> start(Context &ctx, const CommandDef &def, Owned<Command> cmd,
                          PropertyBag inputs);
  Status execRecord(Context &ctx, CommandRecord &rec);
  Result<PropertyBag> finish(Context &ctx, RecordPtr rec);
  Status busy(const char *what) const;
  Result<PropertyBag> subRun(Context &ctx, const CommandDef &def, Owned<Command> cmd,
                             PropertyBag inputs);

  /* A record's redo-by-exec pushes itself so the exec's runSub calls land in it. */
  friend class CommandRecord;

  Runtime &rt_;
  UndoHistory *history_;
  litestl::util::Vector<CommandRecord *> stack_;
  RecordPtr open_;
};

/**
 * Runs `ctx`'s executor on an invocation string, `mesh.extrude(dist=1.0 mode='X')`:
 * arguments are parsed against the inputs' defs, and inputs not given take their
 * LastUsed values, then their defaults.
 */
Result<PropertyBag> invoke(Context &ctx, std::string_view invocation);

/** Builds the input bag for `def` from an invocation's arguments (unknown names are
 * NotFound with suggestions). */
Result<PropertyBag> inputsFromInvocation(Runtime &rt, const CommandDef &def,
                                         const struct Invocation &inv);

} // namespace coredcc
