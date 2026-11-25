#pragma once

/** \file
 * The graph commands. Each but the first takes a Pointer `graph` (into a graph block):
 * - `graph.create(name=<String>)`, output `graph`.
 * - `graph.node.add(graph type=<String> position=<Vec2> label=<String>)`, output `node`.
 * - `graph.node.remove(graph node=<Int>)`
 * - `graph.link(graph from=<Int> fromSocket=<String> to=<Int> toSocket=<String>)`, output
 *   `link`.
 * - `graph.unlink(graph link=<Int>)`
 * - `graph.interface.add(graph side=<INPUT|OUTPUT> name=<String> type=<String>
 * index=<Int>)`, output `socket`. `index` -1, the default, adds it last.
 * - `graph.interface.remove(graph socket=<Int>)`
 * - `graph.interface.move(graph socket=<Int> index=<Int>)`
 * - `graph.interface.setType(graph socket=<Int> type=<String>)`
 *
 * Nodes, links and interface sockets are named by their ids. Params, literals, labels,
 * positions, the group of a node, and the names and defaults of the interface are set
 * through datapaths (graph/api.h). runGraphEdit (graph/ops.h) makes many edits in one
 * step.
 */

#include "coredcc/graph/ops.h"

namespace coredcc {

inline constexpr const char *kGraphCreateCommand = "graph.create";
inline constexpr const char *kGraphNodeAddCommand = "graph.node.add";
inline constexpr const char *kGraphNodeRemoveCommand = "graph.node.remove";
inline constexpr const char *kGraphLinkCommand = "graph.link";
inline constexpr const char *kGraphUnlinkCommand = "graph.unlink";
inline constexpr const char *kGraphInterfaceAddCommand = "graph.interface.add";
inline constexpr const char *kGraphInterfaceRemoveCommand = "graph.interface.remove";
inline constexpr const char *kGraphInterfaceMoveCommand = "graph.interface.move";
inline constexpr const char *kGraphInterfaceSetTypeCommand = "graph.interface.setType";

/** Registers the graph block type and these commands. Idempotent. */
Status registerGraphCommands(Runtime &rt);

} // namespace coredcc
