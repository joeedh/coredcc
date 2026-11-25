#pragma once

/** \file
 * GraphDef: a graph's nodes, links and interface.
 */

#include "coredcc/base/block_id.h"
#include "coredcc/base/runtime.h"
#include "coredcc/base/status.h"
#include "coredcc/graph/node_type.h"
#include "coredcc/graph/socket_type.h"
#include "coredcc/lib/block.h"
#include "coredcc/props/value.h"

#include "litestl/util/map.h"
#include "litestl/util/string.h"
#include "litestl/util/vector.h"

#include <compare>
#include <cstdint>
#include <memory>
#include <string_view>

namespace coredcc {

class GraphDef;
class Library;
struct GraphTopology;
struct GraphReach;

/** Ids of nodes and links are random, 1 to 2^52-1 (so they fit a JS number), and unique in
 * their def. They never change, through undo, save and load. */
struct NodeId {
  int64_t value = 0;
  bool isNull() const
  {
    return value == 0;
  }
  explicit operator bool() const
  {
    return value != 0;
  }
  auto operator<=>(const NodeId &) const = default;
  uint64_t computeHash() const
  {
    return uint64_t(value) * 0x9E3779B97F4A7C15ull;
  }
};
struct LinkId {
  int64_t value = 0;
  bool isNull() const
  {
    return value == 0;
  }
  explicit operator bool() const
  {
    return value != 0;
  }
  auto operator<=>(const LinkId &) const = default;
  uint64_t computeHash() const
  {
    return uint64_t(value) * 0x9E3779B97F4A7C15ull;
  }
};

/** A socket of a node. The key is the socket's identifier, or `#<id>` for a socket that
 * comes from an interface (on group, input and output nodes). */
struct SocketRef {
  NodeId node;
  std::string_view key;
};

enum class SocketSide : uint8_t { Input, Output };

/** A socket of a def's interface. */
struct InterfaceSocket {
  /** In the same id space as nodes and links; random too, so it is unique across defs. */
  int64_t id = 0;
  litestl::util::string name;
  litestl::util::string typeName;
  /** An input's value when nothing feeds it; None for the socket type's default. Ignored
   * while its type isn't the socket's. */
  PropValue defaultValue;
  /** Moves when defaultValue does. */
  uint64_t defaultVersion = 0;

  /** The key of its sockets on nodes. */
  litestl::util::string key() const;
};

/** `#<id>`. */
litestl::util::string interfaceKey(int64_t id);

/** A value set on a node: a param or a literal. */
struct NodeValue {
  litestl::util::string name;
  PropValue value;
  /** Moves when the value does. */
  uint64_t version = 0;
};

using NodeValues = litestl::util::Vector<NodeValue, 1>;

/** A block a node's Pointer param refers to. */
struct NodeBlockRef {
  litestl::util::string name;
  RefCell cell;
};

class GraphNode;

/** A link between two nodes' sockets. */
struct GraphLink {
  LinkId id;
  GraphNode *from = nullptr;
  litestl::util::string fromKey;
  GraphNode *to = nullptr;
  litestl::util::string toKey;
};

/** A node. Read it here, change it through its GraphDef. Its address is stable while it
 * exists. */
class GraphNode {
 public:
  NodeId id() const
  {
    return id_;
  }
  const litestl::util::string &typeName() const
  {
    return typeName_;
  }
  /** The registered type; null while there is none of that name. */
  const NodeType *type() const
  {
    return type_;
  }
  const litestl::util::string &label() const
  {
    return label_;
  }
  double x() const
  {
    return pos_[0];
  }
  double y() const
  {
    return pos_[1];
  }
  /** Params that were set, pointers excepted. An unset param has its def's default. */
  const NodeValues &params() const
  {
    return params_;
  }
  /** Literals that were set, by socket key. */
  const NodeValues &literals() const
  {
    return literals_;
  }
  /** Pointer params. */
  const litestl::util::Vector<NodeBlockRef, 1> &blockRefs() const
  {
    return refs_;
  }
  /** What a Pointer param refers to; null if it isn't set. */
  BlockId blockTarget(std::string_view param) const;
  /** Group nodes: the def's block. */
  const RefCell &group() const
  {
    return group_;
  }
  /** Moves when a param or the group does. */
  uint64_t paramVersion() const
  {
    return paramVersion_;
  }
  /** Links into this node, at most one per input. */
  const litestl::util::Vector<GraphLink *, 2> &linksIn() const
  {
    return in_;
  }
  const litestl::util::Vector<GraphLink *, 2> &linksOut() const
  {
    return out_;
  }
  /** The link into input `key`, or null. */
  const GraphLink *linkInto(std::string_view key) const;
  /** The set literal of input `key`, or null. */
  const NodeValue *literal(std::string_view key) const;
  const NodeValue *param(std::string_view name) const;

 private:
  friend class GraphDef;

  NodeId id_;
  litestl::util::string typeName_;
  mutable const NodeType *type_ = nullptr;
  litestl::util::string label_;
  double pos_[2] = {0, 0};
  NodeValues params_;
  NodeValues literals_;
  litestl::util::Vector<NodeBlockRef, 1> refs_;
  RefCell group_{RefKind::OptStrong};
  uint64_t paramVersion_ = 0;
  litestl::util::Vector<GraphLink *, 2> in_;
  litestl::util::Vector<GraphLink *, 2> out_;
  /** The sockets when they were last known, unless the type says; not undoable. */
  mutable SocketInfos inputs_;
  mutable SocketInfos outputs_;
  /** The interface version the sockets are from. */
  mutable uint64_t socketsVersion_ = 0;
  /** Its place in GraphDef::nodes_. */
  int slot_ = 0;
};

/** Everything a removed node needs to come back: undo, and reading files. */
struct NodeState {
  struct Ref {
    litestl::util::string name;
    BlockId target;
    bool weak = false;
  };
  NodeId id;
  litestl::util::string typeName;
  litestl::util::string label;
  double pos[2] = {0, 0};
  /** Versions are ignored: an inserted node is stamped anew. */
  NodeValues params;
  NodeValues literals;
  litestl::util::Vector<Ref> refs;
  BlockId group;
  /** Its sockets when last known, for a type or a group that isn't there. */
  SocketInfos inputs;
  SocketInfos outputs;
};

struct LinkState {
  LinkId id;
  NodeId from;
  litestl::util::string fromKey;
  NodeId to;
  litestl::util::string toKey;
};

/** A link that is ignored, or whose types don't convert. */
struct LinkDiagnostic {
  LinkId link;
  Status status;
};

/** What a cached answer about a document depends on. */
struct GraphPassKey {
  uint64_t edit = 0;
  uint64_t epoch = 0;
  uint64_t clients = 0;
  int ifaceTables = 0;
  bool operator==(const GraphPassKey &) const = default;
  static GraphPassKey of(Runtime &rt, const Library *lib);
};

/** How graphs hold their Library. A raw pointer for now: the client destroys graphs and
 * instances before it. P5's step 5 makes it a LibraryRef. */
class GraphLibrary {
 public:
  explicit GraphLibrary(Library *lib = nullptr) : lib_(lib) {}
  Library *get() const
  {
    return lib_;
  }

 private:
  Library *lib_;
};

/**
 * A graph: nodes, links, and an interface of typed sockets.
 *
 * **Its home.** A def is usually the data of a block of type `coredcc.GraphDef`
 * (graph/provider.h), and is made with its Library and BlockId. A standalone def has no
 * block. It may have a Library, which group nodes and Pointer params need; its references
 * don't keep blocks alive, because no block reports them. Edits of a block's def notify
 * the ChangeBus.
 *
 * **Links** name sockets by key. A link to a socket that doesn't exist (an interface input
 * was removed, say) is kept and ignored, and listed in diagnostics(). An input has at most
 * one link.
 *
 * **Edits** are Busy while a graph evaluates. They stamp versions, which is how instances
 * see them; nothing is pushed to instances. Several defs may use each other through group
 * nodes; a def can't reach itself.
 */
class GraphDef {
 public:
  explicit GraphDef(Runtime *rt, Library *lib = nullptr, BlockId self = {});
  ~GraphDef();
  GraphDef(const GraphDef &) = delete;
  GraphDef &operator=(const GraphDef &) = delete;

  Runtime &runtime() const
  {
    return rt_;
  }
  Library *library() const
  {
    return lib_.get();
  }
  /** The block this is the data of; null for a standalone def. */
  BlockId self() const
  {
    return self_;
  }
  /** Unique per GraphDef object. */
  uint64_t serial() const
  {
    return serial_;
  }
  /** Moves when nodes, links, groups or the interface change. */
  uint64_t structureVersion() const
  {
    return structureVersion_;
  }
  /** Moves with every edit except labels and positions. */
  uint64_t contentVersion() const
  {
    return contentVersion_;
  }
  uint64_t interfaceVersion() const
  {
    return interfaceVersion_;
  }

  /* ---- reading ---- */

  int nodeCount() const
  {
    return int(byId_.size());
  }
  int linkCount() const
  {
    return int(links_.size());
  }
  const GraphNode *node(NodeId id) const;
  const GraphLink *link(LinkId id) const;
  /** Calls fn(const GraphNode &), in the order nodes were added. */
  template <class Fn> void forEachNode(Fn &&fn) const
  {
    for (GraphNode *n : nodes_) {
      if (n) {
        fn(static_cast<const GraphNode &>(*n));
      }
    }
  }
  /** Calls fn(const GraphLink &), in no particular order. */
  template <class Fn> void forEachLink(Fn &&fn) const
  {
    for (auto &pair : links_) {
      fn(static_cast<const GraphLink &>(*pair.value));
    }
  }
  const litestl::util::Vector<InterfaceSocket> &interfaceSockets(SocketSide side) const
  {
    return side == SocketSide::Input ? ifaceIn_ : ifaceOut_;
  }
  /** Null if there is none. */
  const InterfaceSocket *interfaceSocket(int64_t id) const;
  /** The first of that name; null if there is none. */
  const InterfaceSocket *interfaceSocket(SocketSide side, std::string_view name) const;
  /** A node's sockets: its type's, the interface's, or the last known ones when the type
   * or the group isn't there. */
  const SocketInfos &sockets(const GraphNode &node, SocketSide side) const;
  /** The def a group node evaluates; null if it has none or its block isn't Live. */
  GraphDef *groupTarget(const GraphNode &node) const;
  /** The effective value of a param: the one set, or the default. None for a pointer or an
   * unknown param. */
  PropValue paramValue(const GraphNode &node, std::string_view name) const;
  /** Links that don't work. */
  litestl::util::Vector<LinkDiagnostic> diagnostics() const;

  /** A node's state, for insertNode. */
  NodeState stateOf(const GraphNode &node) const;
  static LinkState stateOf(const GraphLink &link);

  /* ---- nodes ---- */

  /** A new node. NotFound (with suggestions) for an unknown type; AlreadyExists for a
   * second `graph.output`. */
  Result<NodeId> addNode(std::string_view type);
  /** A node with a given id and content, whether or not its type is registered.
   * AlreadyExists if the id is taken. Refused if its group would make the def recursive. */
  Status insertNode(const NodeState &state);
  /** Removes a node and its links. */
  Status removeNode(NodeId id);
  /** Labels and positions don't affect evaluation. */
  Status setLabel(NodeId id, std::string_view label);
  Status setPosition(NodeId id, double x, double y);
  /** Sets a param, coerced to its def. None resets it to its default. A Pointer param takes
   * a pointer to a block, other than this def's own. */
  Status setParam(NodeId id, std::string_view name, const PropValue &value);
  /** Sets the value an input has while it isn't linked, coerced to the socket's type. None
   * resets it. Payload inputs have no literal (Unsupported). */
  Status setLiteral(NodeId id, std::string_view input, const PropValue &value);
  /** Sets the def a group node evaluates; null for none. InvalidArgument if the def would
   * reach itself. */
  Status setGroup(NodeId id, BlockId def);

  /* ---- links ---- */

  /** Links an output to an input, replacing the input's link. NotFound if a socket doesn't
   * exist, TypeMismatch without a conversion, InvalidArgument if it would make a cycle. */
  Result<LinkId> link(SocketRef from, SocketRef to);
  /** A link with a given id. The nodes must exist and the input must be free. Sockets,
   * types and cycles aren't checked: the evaluator reports them. */
  Status insertLink(const LinkState &state);
  Status unlink(LinkId id);

  /* ---- interface ---- */

  /** A new interface socket, at `index` (-1: last), and its id. Pass `id` only to bring a
   * removed one back. Names are unique per side. */
  Result<int64_t> addInterface(SocketSide side,
                               std::string_view name,
                               std::string_view socketType,
                               int index = -1,
                               int64_t id = 0);
  /** Links to it stay, and are ignored. */
  Status removeInterface(int64_t id);
  /** Moves it to `index` on its side. */
  Status moveInterface(int64_t id, int index);
  Status setInterfaceType(int64_t id, std::string_view socketType);
  Status setInterfaceName(int64_t id, std::string_view name);
  /** Sets an interface input's default, coerced to its type; None resets it. */
  Status setInterfaceDefault(int64_t id, const PropValue &value);

  /* ---- references and evaluation ---- */

  /** Every block reference: group defs and Pointer params. */
  void foreachRef(RefVisitor &visitor);
  /** The compiled form, rebuilt when it is out of date. */
  std::shared_ptr<const GraphTopology> topology() const;

  /** Checks that nodes, links and indices agree. */
  Status validate() const;

 private:
  friend class GraphReachBuilder;

  Status busy(const char *what) const;
  Result<GraphNode *> find(NodeId id, const char *what) const;
  /** Stamps an edit; `announce` follows once the data is changed. */
  uint64_t stamp(bool structural);
  /** Tells the ChangeBus about an edit of a block's def. */
  void announce(bool structural);
  int64_t newId();
  bool idTaken(int64_t id) const;
  void attachNode(GraphNode *n);
  void attachLink(const LinkState &state, GraphNode *from, GraphNode *to);
  void detachLink(GraphLink *link);
  void refreshSockets(const GraphNode &node) const;
  const NodeType *typeOf(const GraphNode &node) const;
  bool topologyCurrent() const;
  Status setParamRef(GraphNode *n, const NodeParamDef &param, const PropValue &value);
  InterfaceSocket *findInterface(int64_t id, SocketSide *side = nullptr);
  Status checkInterfaceName(SocketSide side, std::string_view name, int64_t except) const;

  /* reach.cc */
  bool wouldCycle(const GraphNode &from,
                  std::string_view fromKey,
                  const GraphNode &to,
                  std::string_view toKey) const;
  Status checkRecursion(BlockId def) const;

  Runtime &rt_;
  GraphLibrary lib_;
  BlockId self_;
  uint64_t serial_;
  uint64_t structureVersion_;
  uint64_t contentVersion_;
  uint64_t interfaceVersion_;
  /** In order, with nulls where nodes were removed. */
  litestl::util::Vector<GraphNode *> nodes_;
  litestl::util::Map<int64_t, GraphNode *> byId_;
  litestl::util::Map<int64_t, GraphLink *> links_;
  litestl::util::Vector<InterfaceSocket> ifaceIn_;
  litestl::util::Vector<InterfaceSocket> ifaceOut_;
  mutable std::shared_ptr<const GraphTopology> topo_;
  mutable GraphPassKey topoKey_;
  mutable std::shared_ptr<const GraphReach> reach_;
  mutable GraphPassKey reachKey_;
};

} // namespace coredcc
