#pragma once

/** \file
 * Graphs in datapaths.
 *
 * A graph block's data struct is `coredcc.GraphDef`: `name`, and the collections `nodes`
 * (by `[#id]`, or by a label only one node has), `links`, `inputs` and `outputs` (the
 * interface, by `[#id]` or name).
 *
 * A node is a `coredcc.GraphNode:<type>`: `id`, `type`, `label`, `position`, and
 * - for a type with a kernel, `params` and `inputs` (the literals of unlinked inputs),
 * with one member per param and per input that has a literal;
 * - for `graph.group`, the settable pointer `group`;
 * - for `graph.group` and `graph.output`, the collection `sockets` of inputs. A socket's
 *   `value` is read-only, and shows the default, until `isSet` is true. Setting `isSet`
 * to false keeps the value for the next time. Both are read-only while the group isn't
 * there.
 *
 * A node whose type isn't registered is a plain `coredcc.GraphNode`. Everything here goes
 * through GraphDef's setters. Nodes, links and the interface are edited by commands
 * (graph/commands.h).
 */

#include "coredcc/base/id.h"
#include "coredcc/base/status.h"

#include <string_view>

namespace coredcc {

class GraphNode;
class Runtime;
struct NodeType;

inline constexpr const char *kGraphNodeStruct = "coredcc.GraphNode";
inline constexpr const char *kGraphParamsStruct = "coredcc.GraphParams";
inline constexpr const char *kGraphInputsStruct = "coredcc.GraphInputs";

/** The struct a node shows as. */
StructId graphNodeStruct(const GraphNode &node);

namespace detail {
/** The structs that don't depend on node types. */
void addGraphStructs(Runtime &rt);
/** AlreadyExists if a struct that addNodeStructs would add is there. */
Status checkNodeStructs(Runtime &rt, std::string_view type);
Status addNodeStructs(Runtime &rt, const NodeType &type);
} // namespace detail

} // namespace coredcc
