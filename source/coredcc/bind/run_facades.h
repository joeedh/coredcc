#pragma once

/** \file
 * Running commands with values, asking whether they would run, and driving interactive ones, for
 * scripts. The rules of facades.h hold here too.
 *
 * A `prefix` is a path prefix as invokeAt's: "" is none, so is "/".
 */

#include "coredcc/bind/facades.h"

#include <memory>

namespace coredcc::script {

/** A command's inputs by name, before it is known which command's. Copies share it. */
class RunInputs {
 public:
  struct Core;
  RunInputs();
  Core &core() const
  {
    return *core_;
  }
  static const litestl::binding::types::Struct<RunInputs> *defineBindings();

 private:
  std::shared_ptr<Core> core_;
};

RunInputs newRunInputs();
/** Over what it was. Checked, and converted as `setValue` does, when a command is run with it. */
Status runInputsSet(const RunInputs &inputs, const string &name, const PropValue &value);

/** Runs command `id`, returning its outputs; a sub-command from a script command's exec. Inputs
 * that aren't given have their last-used values, if `lastUsed`, or their defaults. */
BagResult runAt(Context &ctx,
                const string &prefix,
                const string &id,
                const RunInputs &inputs,
                bool lastUsed);
/** Why runAt or beginInteractiveAt would not start it: its canRun's answer, or what is wrong with
 * the command, the prefix or the inputs. Not from a script's slot. */
Status canRunAt(Context &ctx,
                const string &prefix,
                const string &id,
                const RunInputs &inputs,
                bool lastUsed);
/** canRunAt, for an invocation: a layout's tool. */
Status canInvokeAt(Context &ctx, const string &prefix, const string &invocation);

/** canInvokeAt of many at once. Copies share it. */
class ToolChecks {
 public:
  struct Core;
  ToolChecks();
  Core &core() const
  {
    return *core_;
  }
  static const litestl::binding::types::Struct<ToolChecks> *defineBindings();

 private:
  std::shared_ptr<Core> core_;
};

ToolChecks newToolChecks();
/** Its index. An error for a prefix or an invocation that doesn't parse. */
IntResult toolChecksAdd(const ToolChecks &tools, const string &prefix, const string &invocation);
/** Asks of each. A JSON array of `[index, reason]` of those whose answer isn't what it was at the
 * last call, or that are new; `reason` is the error's message, or null if it can run. */
StringResult toolChecksRun(Context &ctx, const ToolChecks &tools);

/** Starts an Interactive command. Not from a script's slot, as the calls below. */
Status beginInteractiveAt(Context &ctx,
                          const string &prefix,
                          const string &id,
                          const RunInputs &inputs,
                          bool lastUsed);
/** The open command's id; "" for none. */
StringResult interactiveId(Context &ctx);
/** A live edit: writeValue as if from the open command, so under its prefix. The command's undo
 * has to take it back. */
Status writeInteractive(Context &ctx, const string &path, const PropValue &value);
Status commitInteractive(Context &ctx);
Status cancelInteractive(Context &ctx);

/** The id of the last command in the history, if it can be run again (CommandExecutor::rerunHead);
 * "" for none. */
StringResult lastCommandId(Context &ctx);
/** Its inputs. FailedPrecondition if there is none. */
BagResult lastCommandInputs(Context &ctx);
/** Undoes it and runs it again, with `inputs` over those it had. */
Status rerunLast(Context &ctx, const RunInputs &inputs);

} // namespace coredcc::script
