#pragma once

/** \file
 * Interior sockets: sockets of nodes inside a group, as sockets of the group node.
 *
 * Their key is `@<node>.<socket>` for a node of the group's def, or `@<group>/<node>.<socket>`
 * for one in a group in it, and so on (not in a foreach). As an output, it reads the socket.
 * As an input, its link replaces the interior input's own link or literal, for that group
 * node only; where several do for one input, the outermost counts. A link with a broken
 * other end replaces nothing.
 *
 * GraphDef::link accepts such keys; GraphDef::sockets doesn't list them. They are for graphs
 * that a program builds around the user's (a scene evaluator). Commands refuse them.
 */

#include "coredcc/graph/def.h"

#include "litestl/util/string.h"
#include "litestl/util/vector.h"

#include <optional>
#include <string>
#include <string_view>

namespace coredcc {

struct InteriorKey {
  /** The group nodes on the way in, in the group's def, the def of the first, and so on. */
  litestl::util::Vector<NodeId, 1> groups;
  NodeId node;
  litestl::util::string key;

  /** None if it isn't one. Each has only one text. */
  static std::optional<InteriorKey> parse(std::string_view text);
  /** The text, as sockets have it. */
  std::string toString() const;
  /** This key as seen from inside its first group. */
  InteriorKey inner() const;
  bool operator==(const InteriorKey &b) const;
  uint64_t computeHash() const;
};

inline bool isInteriorKey(std::string_view key)
{
  return !key.empty() && key[0] == '@';
}

/** The socket of `group`, a group node of `def`; none if it isn't there. */
std::optional<SocketInfo> findInterior(const GraphDef &def,
                                       const GraphNode &group,
                                       SocketSide side,
                                       const InteriorKey &key);

} // namespace coredcc
