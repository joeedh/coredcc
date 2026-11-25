#pragma once

/** \file
 * GraphTopology: a def compiled for evaluation.
 */

#include "coredcc/graph/def.h"

#include <memory>

namespace coredcc {

/**
 * A def's nodes, sockets and links as arrays. Immutable and shared: instances keep one
 * while they walk it. `GraphDef::topology` rebuilds it when the structure, a registry, or
 * the interface or presence of a group's def changed; not when params or literals did.
 */
struct GraphTopology {
  struct Input {
    litestl::util::string key;
    /** Null while its type isn't registered. */
    const SocketType *type = nullptr;
    /** Where its link comes from, as indices in `nodes` and that node's outputs; -1
     * without a working link. */
    int fromNode = -1;
    int fromOutput = -1;
    /** The node it is on. */
    int node = -1;
  };
  struct Output {
    litestl::util::string key;
    const SocketType *type = nullptr;
  };
  struct Node {
    NodeId id;
    const GraphNode *node = nullptr;
    /** Null while its type isn't registered; then the role is Kernel. */
    const NodeType *type = nullptr;
    NodeRole role = NodeRole::Kernel;
    /** Ranges in `inputs` and `outputs`. For the sockets of an interface, the index in the
     * range is the index in the interface. */
    int inBegin = 0, inCount = 0;
    int outBegin = 0, outCount = 0;
    /** Index in `groups`, or -1. */
    int group = -1;
  };
  struct Group {
    const GraphNode *node = nullptr;
    /** Index in `nodes`. */
    int index = 0;
    /** The interfaceVersion of its def; 0 while it isn't there, when its sockets are the
     * last known ones. */
    uint64_t targetInterface = 0;
  };

  litestl::util::Vector<Node> nodes;
  litestl::util::Vector<Input> inputs;
  litestl::util::Vector<Output> outputs;
  litestl::util::Vector<Group> groups;
  litestl::util::Map<int64_t, int> indexOf;
  /** The graph.output node, or -1. */
  int outputNode = -1;
  /** Unique per build. */
  uint64_t serial = 0;
  /** What it was built from. */
  uint64_t structure = 0;
  uint64_t nodeTypes = 0;
  uint64_t socketTypes = 0;

  /** The index of a node, or -1. */
  int find(NodeId id) const
  {
    const int *i = indexOf.lookup_ptr(id.value);
    return i ? *i : -1;
  }
  /** Index in the node's inputs, or -1. */
  int inputIndex(const Node &n, std::string_view key) const;
  int outputIndex(const Node &n, std::string_view key) const;

  static std::shared_ptr<const GraphTopology> build(const GraphDef &def);
};

/** Which interface inputs each interface output of a def depends on. */
struct GraphReach {
  /** Per interface output, the indices of inputs, ascending. */
  litestl::util::Vector<litestl::util::Vector<int>> inputs;
  /** The versions of its group defs' tables, in the order of GraphTopology::groups; 0 for
   * a group without one. */
  litestl::util::Vector<uint64_t> groups;
  uint64_t topology = 0;
  /** Unique per table. */
  uint64_t version = 0;
};

/** The reach table of `def`, computed or cached. Defs that (through a reference written
 * directly) reach themselves are handled by leaving out the recursive group. */
std::shared_ptr<const GraphReach> graphReach(const GraphDef &def);

} // namespace coredcc
