#pragma once

/** \file
 * The graph commands. Each but the first takes a Pointer `graph` (into a graph block):
 * - `core.graph.new(name=<String>)`, output `graph`.
 * - `core.graph.node.add(graph type=<String> position=<Vec2> label=<String>)`, output
 * `node`.
 * - `core.graph.node.remove(graph node=<Int>)`
 * - `core.graph.link.add(graph from=<Int> fromSocket=<String> to=<Int>
 * toSocket=<String>)`, output `link`.
 * - `core.graph.link.remove(graph link=<Int>)`
 * - `core.graph.interface.add(graph side=<INPUT|OUTPUT> name=<String> type=<String>
 * index=<Int>)`, output `socket`. `index` -1, the default, adds it last.
 * - `core.graph.interface.remove(graph socket=<Int>)`
 * - `core.graph.interface.move(graph socket=<Int> index=<Int>)`
 * - `core.graph.interface.setType(graph socket=<Int> type=<String>)`
 *
 * Nodes, links and interface sockets are named by their ids. Params, literals, labels,
 * positions, the group of a node, and the names and defaults of the interface are set
 * through datapaths (graph/api.h). runGraphEdit (graph/ops.h) makes many edits in one
 * step.
 */

#include "coredcc/graph/ops.h"

namespace coredcc {

inline constexpr const char *kNewGraphCommand = "core.graph.new";
inline constexpr const char *kAddGraphNodeCommand = "core.graph.node.add";
inline constexpr const char *kRemoveGraphNodeCommand = "core.graph.node.remove";
inline constexpr const char *kAddGraphLinkCommand = "core.graph.link.add";
inline constexpr const char *kRemoveGraphLinkCommand = "core.graph.link.remove";
inline constexpr const char *kAddGraphInterfaceCommand = "core.graph.interface.add";
inline constexpr const char *kRemoveGraphInterfaceCommand = "core.graph.interface.remove";
inline constexpr const char *kMoveGraphInterfaceCommand = "core.graph.interface.move";
inline constexpr const char *kSetGraphInterfaceTypeCommand =
    "core.graph.interface.setType";

/** Registers the graph block type and these commands. Idempotent. */
Status registerGraphCommands(Runtime &rt);

} // namespace coredcc
