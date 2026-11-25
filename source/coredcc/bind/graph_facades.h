#pragma once

/** \file
 * Graphs for scripts: node types with script kernels, editing, and evaluating. The rules of
 * facades.h hold here too.
 *
 * **Inside a script kernel** (`evaluate` of a node type) only `nodeInput`, `nodeParam`,
 * `nodeOutput` and `nodePass` do anything. Reading datapaths or the running command's bags
 * there is Busy, as is every edit: the graph couldn't see what the kernel depends on.
 */

#include "coredcc/bind/facades.h"
#include "coredcc/graph/instance.h"
#include "coredcc/graph/node_type.h"

#include <memory>

namespace coredcc::script {

/* ---- node types ---- */

/** A new def for node type `id` (dotted). Owned by the caller, who disposes of it:
 * registration copies it. Names and types are checked when it is registered. */
NodeTypeDef *newNodeType(const string &id);
Status nodeTypeUiName(NodeTypeDef &def, const string &s);
/** An input whose socket type and default come from `defaultValue`: Bool, Int, Float,
 * String or Vec2-4. */
Status nodeTypeInput(NodeTypeDef &def, const string &name, const PropValue &defaultValue);
/** An input of a payload type, which scripts can only pass on. */
Status nodeTypePayloadInput(NodeTypeDef &def, const string &name, const string &socketType);
Status nodeTypeOutput(NodeTypeDef &def, const string &name, const string &socketType);
/** A param, typed as for nodeTypeInput. */
Status nodeTypeParam(NodeTypeDef &def, const string &name, const PropValue &defaultValue);

/**
 * Registers a copy of `def` with the script object `kernel`, which implements the
 * space-separated GraphKernel slots in `slots` (that is, `evaluate`).
 *
 * - Takes one count of `kernel`, whatever the outcome, and releases it on failure, else
 *   with the Runtime.
 * - The `core.` and `graph.` namespaces are reserved.
 * - A refused registration leaves nothing behind.
 */
Status registerForeignNodeType(Context &ctx,
                               const NodeTypeDef &def,
                               uint32_t kernel,
                               const string &slots);

/* ---- in a kernel ---- */

/** An input's value. NotFound for an unknown name; Unsupported for a payload. */
ValueResult nodeInput(NodeExecContext &node, const string &name);
/** A param's value; None for an unset Optional one, and for a Pointer. */
ValueResult nodeParam(NodeExecContext &node, const string &name);
/** Sets an output, converting `value` as a datapath write would (an Int to a Float, say). */
Status nodeOutput(NodeExecContext &node, const string &name, const PropValue &value);
/** Sets an output to an input's value, which may be a payload. */
Status nodePass(NodeExecContext &node, const string &input, const string &output);

/* ---- editing: undoable commands, allowed where `invoke` is ---- */

/** Makes a graph. Its value is a pointer to it, for a Pointer member or input. */
ValueResult graphCreate(Context &ctx, const string &name);
/** Adds a node to the graph at `graphPath`; its id. */
IntResult graphAddNode(Context &ctx, const string &graphPath, const string &type, double x, double y);
Status graphRemoveNode(Context &ctx, const string &graphPath, int64_t node);
/** The link's id. */
IntResult graphLink(Context &ctx,
                    const string &graphPath,
                    int64_t from,
                    const string &fromSocket,
                    int64_t to,
                    const string &toSocket);
Status graphUnlink(Context &ctx, const string &graphPath, int64_t link);
/** Adds a socket to the graph's interface; its id. Its key on nodes is `#<id>`. */
IntResult graphAddInterface(Context &ctx,
                            const string &graphPath,
                            bool output,
                            const string &name,
                            const string &type);

/* ---- evaluating ---- */

/**
 * A graph instance for scripts. Copies share it; it goes with the last one, or with the
 * Runtime, after which calls are FailedPrecondition. One dropped during its own evaluation
 * lasts until that is done.
 */
class ScriptGraph {
 public:
  struct Core;
  ScriptGraph() = default;
  explicit ScriptGraph(std::shared_ptr<Core> core) : core_(std::move(core)) {}
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
Status graphSetInput(const ScriptGraph &graph, const string &name, const PropValue &value);
/** Back to the interface's default. */
Status graphClearInput(const ScriptGraph &graph, const string &name);
/** Evaluates an interface output, by name. Unsupported for a payload that has a value. */
ValueResult graphEvaluate(const ScriptGraph &graph, const string &output);
/** Why the output has no value as of its last evaluation, with the node at fault; "" when
 * it has one. */
string graphErrorText(const ScriptGraph &graph, const string &output);
/** Drops the instance now, for all copies. */
void graphClose(const ScriptGraph &graph);

/** How many instances are alive, for tests of leaks. */
int32_t graphInstanceCount(Context &ctx);

} // namespace coredcc::script
