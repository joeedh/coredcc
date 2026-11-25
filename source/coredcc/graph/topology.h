#pragma once

/** \file
 * GraphTopology: a def compiled for evaluation.
 */

#include "coredcc/graph/interior.h"

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
    /** How many of them are of the node itself. Interior ones that links use follow. */
    int interfaceIns = 0, interfaceOuts = 0;
    /** Index in `groups`, or -1. */
    int group = -1;
  };
  /** An interior key that a link of a group node uses. */
  struct Interior {
    InteriorKey key;
    SocketSide side = SocketSide::Input;
    /** Whether its socket is there, and the socket's type name. */
    bool found = false;
    litestl::util::string typeName;
    /** Index in the node's inputs or outputs. -1 if it isn't there; and for an input whose
     * link has no output, which replaces nothing. */
    int index = -1;
  };
  struct Group {
    const GraphNode *node = nullptr;
    /** Index in `nodes`. */
    int index = 0;
    /** The interfaceVersion of its def; 0 while it isn't there, when its sockets are the
     * last known ones. */
    uint64_t targetInterface = 0;
    /** GraphNode::socketsVersion: what is last known can change while nobody looks. */
    uint64_t sockets = 0;
    litestl::util::Vector<Interior, 1> interior;
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

 private:
  /** The interior sockets of a group node, the last one added, at `index`. */
  void addInterior(const GraphDef &def, Node &n, int index);
};

/** Interior keys that a reach table has, after the interface's sockets. An `in` replaces the
 * interior input's link, as it does in the group the table is for. */
struct ReachKeys {
  litestl::util::Vector<InteriorKey> outs;
  litestl::util::Vector<InteriorKey> ins;
  void add(SocketSide side, const InteriorKey &key);
  /** Its index, or -1. */
  int find(SocketSide side, const InteriorKey &key) const;
  std::string toString() const;
};

/** Which inputs each output of a def depends on. */
struct GraphReach {
  /** Per interface output and then per ReachKeys::outs, the inputs, ascending: interface
   * inputs by index, then ReachKeys::ins. */
  litestl::util::Vector<litestl::util::Vector<int>> inputs;
  /** The versions of its group defs' tables, in the order of GraphTopology::groups; 0 for
   * a group without one. */
  litestl::util::Vector<uint64_t> groups;
  uint64_t topology = 0;
  /** Unique per table. */
  uint64_t version = 0;
};

/** A reach table of `def`, computed or cached. Defs that (through a reference written
 * directly) reach themselves are handled by leaving out the recursive group. */
std::shared_ptr<const GraphReach> graphReach(const GraphDef &def, const ReachKeys &keys = {});

} // namespace coredcc
