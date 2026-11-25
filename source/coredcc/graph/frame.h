#pragma once

/** \file
 * An instance's state, per group: frames, input sockets and kernels. Internal to graph/.
 */

#include "coredcc/graph/instance.h"
#include "coredcc/graph/topology.h"
#include "coredcc/graph/trace.h"

#include "litestl/util/map.h"
#include "litestl/util/vector.h"

#include <memory>

namespace coredcc::graph {

using ErrorPtr = std::shared_ptr<const GraphError>;

struct Frame;
struct Foreach;

/** A vertex of the walk: an input socket, or a kernel node with its outputs. Outputs of
 * groups and interface inputs need none: each leads to exactly one other vertex. */
struct Vertex {
  Frame *frame = nullptr;
  /** In GraphTopology::inputs, or ::nodes for a kernel. */
  int index = -1;
  bool kernel = false;
  bool operator==(const Vertex &) const = default;
};

/** Tarjan's numbers, valid in pass `pass`. */
struct WalkState {
  uint64_t pass = 0;
  int number = 0;
  int low = 0;
  bool onStack = false;
  bool done = false;
};

/** An input socket: the value that reaches it, after conversion to its type. Valid in pass
 * `walk.pass` once done. */
struct InputState {
  WalkState walk;
  /** What feeds it, and that vertex's output if it is a kernel. */
  Vertex source;
  int sourceOutput = 0;
  const SocketType *sourceType = nullptr;
  ErrorPtr error;
  TraceInput origin;
  /** In a kernel's outputs, a registry, or `own`. */
  const GraphValue *value = nullptr;
  /** A value made here (a literal, a conversion), and what it was made from. */
  struct Own {
    GraphValue value;
    TraceInput origin;
  };
  std::unique_ptr<Own> own;
};

/** A kernel or foreach node. Kept across edits of the def, by node id and type. */
struct KernelState {
  KernelState();
  ~KernelState();

  struct Output {
    GraphValue value;
    uint64_t changedAt = 0;
  };
  WalkState walk;
  const NodeType *type = nullptr;
  litestl::util::Vector<Output, 1> outputs;
  /** The last run. */
  bool ran = false;
  Trace trace;
  ErrorPtr runError;
  uint64_t runs = 0;
  double seconds = 0;
  /** Foreach nodes. */
  std::unique_ptr<Foreach> foreach;
  /** The outcome of the last visit: the run's error, or an input's. */
  bool visited = false;
  bool inputFailed = false;
  ErrorPtr error;
};

/** An element of a foreach, or the state its uncached elements share. */
struct Element {
  struct Input {
    /** The interface input's. */
    int64_t id = 0;
    /** In a column of the table; else the value is the foreach node's. */
    bool column = false;
    GraphValue value;
    uint64_t version = 0;
  };
  Element(int64_t key, int *counter);
  ~Element();

  int64_t key = 0;
  /** How many there are. Null if it isn't counted. */
  int *counter = nullptr;
  /** Owned. */
  Frame *frame = nullptr;
  /** By index in the def's interface. */
  litestl::util::Vector<Input, 1> inputs;
  /** By index too. Those of `inputs`, while `world` is the Foreach's. */
  litestl::util::Vector<GraphValue, 1> outputs;
  uint64_t world = 0;
  /** The last run it was in. */
  uint64_t run = 0;
};

/** A foreach node's elements, and what they read in its last run. KernelState's trace has
 * the node's inputs, and the blocks of all its elements. */
struct Foreach {
  ~Foreach();
  void clear();

  uint64_t defSerial = 0;
  litestl::util::Map<int64_t, Element *> cached;
  Element *shared = nullptr;
  /** Moves when anything but the table does. */
  uint64_t world = 0;
  /** What the table's check gave. */
  Status table;
  /** The evaluate call that pinned the def. */
  uint64_t pinned = 0;

  /** Every def the elements may evaluate, with its version and its groups, flattened. */
  litestl::util::Vector<uint64_t> defs;
  /** The targets of their nodes' references, as of `epoch`. */
  litestl::util::Vector<BlockId> refs;
  uint64_t epoch = 0;
  uint64_t nodeTypes = 0;
  uint64_t socketTypes = 0;
  /** Of the blocks and queries in the trace. */
  DepSet deps;
};

/** One evaluated def: the root, a group, or an element of a foreach. */
struct Frame {
  ~Frame();

  GraphDef *def = nullptr;
  uint64_t defSerial = 0;
  Frame *parent = nullptr;
  /** The root of its tree: groups are in their parent's, an element is one. */
  Frame *top = nullptr;
  /** A top's: how many are below it. */
  int frames = 0;
  /** Null unless it is an element's. */
  const Element *element = nullptr;
  /** The group or foreach node in the parent. */
  NodeId groupNode;
  /** Its index in the parent's topology. */
  int groupIndex = -1;
  int depth = 0;
  /** Unique. */
  uint64_t serial = 0;
  /** The pass its topology was checked in. */
  uint64_t pass = 0;
  /** The evaluate call it was made in, and the last one that pinned its def. */
  uint64_t call = 0;
  uint64_t pinned = 0;

  std::shared_ptr<const GraphTopology> topo;
  litestl::util::Vector<InputState, 1> inputs;
  /** By node index; null until visited, and for nodes that are neither kernels nor
   * foreach. Owned. */
  litestl::util::Vector<KernelState *, 2> kernels;

  /** A group node's child, checked once per pass. */
  struct Group {
    uint64_t pass = 0;
    /** Owned; null until visited. Usable only while there is no error. */
    Frame *child = nullptr;
    ErrorPtr error;
  };
  /** By index in topo->groups. */
  litestl::util::Vector<Group, 1> groups;

  /** Calls fn(Element &) for every element of its foreach nodes. */
  template <class Fn> void forEachElement(Fn &&fn)
  {
    for (KernelState *k : kernels) {
      if (!k || !k->foreach) {
        continue;
      }
      for (auto &pair : k->foreach->cached) {
        fn(*pair.value);
      }
      if (k->foreach->shared) {
        fn(*k->foreach->shared);
      }
    }
  }
  NodePath pathOf(NodeId node) const;
  /** This frame and the groups below it. */
  int count() const;
};

} // namespace coredcc::graph
