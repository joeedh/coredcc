#pragma once

/** ile
 * Evaluating graphs from scripts. The rules of facades.h hold here too. All calls are
 * Busy while a graph evaluates.
 *
 * Nodes are named by path, as NodePath::toString gives it: `12`, `12/34` for one in a
 * group, and `12/34[7]` for one in element 7 of a foreach.
 */

#include "coredcc/bind/facades.h"
#include "coredcc/bind/script_table.h"
#include "coredcc/graph/instance.h"

#include <memory>

namespace coredcc::script {

/* ---- evaluating ---- */

/**
 * A graph instance for scripts. Copies share it; it goes with the last one, or with the
 * Runtime, after which calls are FailedPrecondition. One dropped during its own
 * evaluation lasts until that is done.
 */
class ScriptGraph {
public:
  struct Core;
  ScriptGraph() = default;
  explicit ScriptGraph(std::shared_ptr<Core> core) : core_(std::move(core))
  {
  }
  const std::shared_ptr<Core> &core() const
  {
    return core_;
  }
  static const litestl::binding::types::Struct<ScriptGraph> *defineBindings();

private:
  std::shared_ptr<Core> core_;
};

struct GraphResult {
  Status status;
  ScriptGraph value;
  static const litestl::binding::types::Struct<GraphResult> *defineBindings();
};

/** A new instance of the graph block at `graphPath`. */
GraphResult graphInstance(Context &ctx, const string &graphPath);
/** Sets an interface input by name, converting `value` as nodeOutput does. */
Status
graphSetInput(const ScriptGraph &graph, const string &name, const PropValue &value);
/** Back to the interface's default. */
Status graphClearInput(const ScriptGraph &graph, const string &name);
/** Evaluates an interface output, by name. Unsupported for a payload that has a value. */
ValueResult graphEvaluate(const ScriptGraph &graph, const string &output);
/** Why the output has no value as of its last evaluation, with the node at fault; "" when
 * it has one. */
string graphErrorText(const ScriptGraph &graph, const string &output);
/** Sets an interface input that takes tables. */
Status graphSetInputTable(const ScriptGraph &graph,
                          const string &name,
                          const ScriptTable &table);
/** As graphEvaluate, for an output that is a table. TypeMismatch for another. */
TableResult graphEvaluateTable(const ScriptGraph &graph, const string &output);

/* ---- watched sockets: evaluated together, and only what moved is listed ---- */

/** Watches a socket of a node, or with an empty `node` an interface output; its id. A
 * socket that is gone is NotFound (see graphWatchStatus) until it is back. */
IntResult graphWatch(const ScriptGraph &graph, const string &node, const string &socket);
Status graphUnwatch(const ScriptGraph &graph, int64_t watch);
/** Evaluates what is watched. Its value is a JSON array: the ids of those whose value or
 * error moved since it last listed them. */
StringResult graphUpdate(const ScriptGraph &graph);
/** What the last update gave: ok, or the error. FailedPrecondition before one. */
Status graphWatchStatus(const ScriptGraph &graph, int64_t watch);
/** Its value; the error if it has none. */
ValueResult graphWatchValue(const ScriptGraph &graph, int64_t watch);
TableResult graphWatchTable(const ScriptGraph &graph, int64_t watch);
/** The last value it had, for display while it has an error; None if it never had one. */
ValueResult graphWatchLastGood(const ScriptGraph &graph, int64_t watch);
/** Moves with its value and its error. 0 before an update. */
IntResult graphWatchChangedAt(const ScriptGraph &graph, int64_t watch);
/** False if an update might give something new. Cheap. */
BoolResult graphWatchIsCurrent(const ScriptGraph &graph, int64_t watch);
/** As graphErrorText. */
string graphWatchErrorText(const ScriptGraph &graph, int64_t watch);

/* ---- looking inside, without evaluating ---- */

/** GraphInstance::peek; None for what wasn't computed. */
ValueResult
graphPeek(const ScriptGraph &graph, const string &node, const string &socket, bool input);
TableResult graphPeekTable(const ScriptGraph &graph,
                           const string &node,
                           const string &socket,
                           bool input);
/**
 * GraphInstance::report, as JSON: `state` ("NotEvaluated", "Ok", "Failed",
 * "InputFailed"), `current`, `runs`, `seconds`, `totalSeconds`, `elements` and
 * `cachedElements` of a foreach, `memory` (`frames`, `kernels`, `elements`, `bytes`,
 * `payloadBytes`), and for the states that failed `error`: `code`, `message`, `node`, and
 * `socket` if an input is at fault.
 */
StringResult graphReport(const ScriptGraph &graph, const string &node);
/** GraphInstance::retry, clear and drop. */
Status graphRetry(const ScriptGraph &graph);
Status graphClear(const ScriptGraph &graph);
Status graphDrop(const ScriptGraph &graph, const string &node);
/** Evaluations that take longer than that are Cancelled, between kernels and where
 * kernels ask (nodeCancelled). What was computed stays, so that the next one goes on. 0
 * for no limit. */
Status graphSetTimeLimit(const ScriptGraph &graph, double milliseconds);
/** Drops the instance now, for all copies. */
void graphClose(const ScriptGraph &graph);

/** How many instances are alive, for tests of leaks. */
int32_t graphInstanceCount(Context &ctx);

} // namespace coredcc::script
