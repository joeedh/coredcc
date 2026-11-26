#pragma once

/** \file
 * Graphs for scripts: node types with script kernels, editing, and evaluating. The rules
 * of facades.h hold here too.
 *
 * **Inside a script kernel** (`evaluate` of a node type) only the calls that take its
 * node, and those of tables, do anything. Reading datapaths or the running command's bags
 * there is Busy, as is every edit: the graph couldn't see what the kernel depends on.
 */

#include "coredcc/bind/facades.h"
#include "coredcc/bind/graph_instance_facades.h"
#include "coredcc/bind/script_table.h"
#include "coredcc/graph/node_type.h"

namespace coredcc::script {

/* ---- node types ---- */

/** A new def for node type `id` (dotted). Owned by the caller, who disposes of it:
 * registration copies it. Names and types are checked when it is registered. */
NodeTypeDef *newNodeType(const string &id);
Status nodeTypeUiName(NodeTypeDef &def, const string &s);
/** An input whose socket type and default come from `defaultValue`: Bool, Int, Float,
 * String, Vec2-4, Quat or Mat4. */
Status nodeTypeInput(NodeTypeDef &def, const string &name, const PropValue &defaultValue);
/** An input of a payload type: a table (`elements`), or one that scripts can only pass
 * on. */
Status
nodeTypePayloadInput(NodeTypeDef &def, const string &name, const string &socketType);
Status nodeTypeOutput(NodeTypeDef &def, const string &name, const string &socketType);
/** A param, typed as for nodeTypeInput. */
Status nodeTypeParam(NodeTypeDef &def, const string &name, const PropValue &defaultValue);

/** An Enum param, or a Flags one; its items come with nodeTypeItem. */
Status
nodeTypeItemParam(NodeTypeDef &def, const string &name, bool flags, int64_t defaultValue);
Status nodeTypeItem(NodeTypeDef &def,
                    const string &param,
                    const string &key,
                    int64_t value,
                    const string &uiName);
/** A Pointer param to blocks of the api struct `target`. `weak` ones don't keep them. */
Status nodeTypePointerParam(NodeTypeDef &def,
                            const string &name,
                            const string &target,
                            bool weak);
/** What an input or output is shown as. NotFound for one that wasn't added. */
Status nodeTypeSocketName(NodeTypeDef &def,
                          bool output,
                          const string &name,
                          const string &uiName);
/** The range of a number: of a param, else of an input. NotFound, or TypeMismatch for
 * others. */
Status nodeTypeRange(NodeTypeDef &def, const string &name, double min, double max);
/** NodeTypeFlags::OutputAdapter. The other flag, ThreadSafe, isn't for scripts. */
Status nodeTypeOutputAdapter(NodeTypeDef &def);

/**
 * Registers a copy of `def` with the script object `kernel`, which implements the
 * space-separated GraphKernel slots in `slots` (that is, `evaluate`).
 *
 * - Takes one count of `kernel`, whatever the outcome, and releases it on failure, else
 *   with the Runtime.
 * - The `core.`, `graph.`, `scene.`, `math.`, `vec.`, `mat4.`, `value.` and `elements.`
 *   namespaces are reserved.
 * - A refused registration leaves nothing behind.
 */
Status registerForeignNodeType(Context &ctx,
                               const NodeTypeDef &def,
                               uint32_t kernel,
                               const string &slots);

/**
 * Replaces the kernel of a script's node type, when code is reloaded. All nodes of the
 * type run again when they are needed. `def` must say the same as the type
 * (NodeTypeRegistry's replaceKernel). FailedPrecondition for a type that isn't a
 * script's. Takes one count of `kernel` in any case, and releases the old one.
 */
Status replaceForeignNodeType(Context &ctx,
                              const NodeTypeDef &def,
                              uint32_t kernel,
                              const string &slots);

/* ---- in a kernel ---- */

/** An input's value. NotFound for an unknown name; Unsupported for a payload. */
ValueResult nodeInput(NodeExecContext &node, const string &name);
/** A param's value; None for an unset Optional one, and for a Pointer. */
ValueResult nodeParam(NodeExecContext &node, const string &name);
/** Sets an output, converting `value` as a datapath write would (an Int to a Float, say).
 */
Status nodeSetOutput(NodeExecContext &node, const string &name, const PropValue &value);
/** Sets an output to an input's value, which may be a payload. */
Status nodePass(NodeExecContext &node, const string &input, const string &output);

BoolResult nodeIsLinked(NodeExecContext &node, const string &name);
/** Makes the node run again when the block that a Pointer param points to changes, in one
 * of `aspects` (ChangeAspects; 0 for all). The block's id as text (api/ref_text.h's
 * parseBlockId), or "" when there is none. */
StringResult nodeUseBlock(NodeExecContext &node, const string &param, uint32_t aspects);
/** Whether the evaluation should end: a kernel that takes long asks, and then throws or
 * returns Cancelled. */
bool nodeCancelled(NodeExecContext &node);
Status nodeProgress(NodeExecContext &node, double fraction, const string &text);
/** An input that is a table. TypeMismatch for others. */
TableResult nodeInputTable(NodeExecContext &node, const string &name);
/** Sets an output to a table, which has to pass ScriptTable::check. */
Status
nodeSetOutputTable(NodeExecContext &node, const string &name, const ScriptTable &table);

/* ---- editing: undoable commands, allowed where `invoke` is ---- */

/** Makes a graph. Its value is a pointer to it, for a Pointer member or input. */
ValueResult newGraph(Context &ctx, const string &name);
/** Adds a node to the graph at `graphPath`; its id. */
IntResult graphAddNode(
    Context &ctx, const string &graphPath, const string &type, double x, double y);
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

/* ---- looking at a graph ---- */

/** An id that no node, link or socket of the graph has, for pasting. */
IntResult graphNewId(Context &ctx, const string &graphPath);
/** What is wrong with its links, as JSON: an array of `link`, `code`, `message`, and
 * `interior`: whether it leads to a node inside a group. */
StringResult graphDiagnostics(Context &ctx, const string &graphPath);

} // namespace coredcc::script
