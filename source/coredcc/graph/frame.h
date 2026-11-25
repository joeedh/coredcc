#pragma once

/** \file
 * An instance's state, per group: frames, input sockets and kernels. Internal to graph/.
 */

#include "coredcc/graph/array.h"
#include "coredcc/graph/dependents.h"
#include "coredcc/graph/instance.h"
#include "coredcc/graph/topology.h"
#include "coredcc/graph/trace.h"

#include "litestl/util/map.h"
#include "litestl/util/vector.h"

#include <memory>

namespace coredcc::graph {

using ErrorPtr = std::shared_ptr<const GraphError>;

struct Foreach;

/** Tarjan's numbers, valid in pass `pass`. A vertex that an edit may have touched has no
 * pass any more (Evaluator::markStale). */
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
  /** Whether `source` is an input around this frame that replaces this one. */
  bool replaced = false;
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
  KernelState(const KernelState &) = delete;
  KernelState &operator=(const KernelState &) = delete;

  struct Output {
    GraphValue value;
    uint64_t changedAt = 0;
  };
  WalkState walk;
  /** Where it is. */
  Frame *frame = nullptr;
  int index = -1;
  /** The last run: whether there was one, and whether it isn't to be used again. */
  bool ran = false;
  /** The outcome of the last visit: the run's error, or an input's. */
  bool visited = false;
  bool inputFailed = false;
  const NodeType *type = nullptr;
  litestl::util::Vector<Output, 1> outputs;
  Trace trace;
  ErrorPtr runError;
  ErrorPtr error;
  uint64_t runs = 0;
  double seconds = 0;
  double totalSeconds = 0;
  /** Foreach nodes. */
  std::unique_ptr<Foreach> foreach;
  /** What reads its outputs, and the blocks it used. In a frame with Dependents. */
  InputLink *consumers = nullptr;
  Array<BlockLink> blockLinks;
};

/** What cached elements take, over all foreach nodes of an instance. */
struct ElementCounts {
  int elements = 0;
  int kernels = 0;
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
  Element(int64_t key, ElementCounts *counts);
  ~Element();

  int64_t key = 0;
  /** How many there are. Null for a shared one. */
  ElementCounts *counts = nullptr;
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
  /** Calls fn(Element &). */
  template <class Fn> void forEachElement(Fn &&fn) const
  {
    for (auto &pair : cached) {
      fn(*pair.value);
    }
    if (shared) {
      fn(*shared);
    }
  }

  uint64_t defSerial = 0;
  litestl::util::Map<int64_t, Element *> cached;
  Element *shared = nullptr;
  /** Moves when anything but the table does. */
  uint64_t world = 0;
  /** What the table's check gave. */
  Status table;
  /** The table that the node's output was made from, and the world it was made in: rows that
   * are the same in both have the same output. */
  PayloadRef madeFrom;
  uint64_t madeIn = 0;
  /** Rows of the last run's table, and of those, how many had a state of their own. */
  int rows = 0;
  int cachedRows = 0;
  /** Whether the last run failed, or stopped. */
  bool failed = false;
  /** The evaluate call that pinned the def. */
  uint64_t pinned = 0;

  /** Every def the elements may evaluate, with its version and its groups, flattened. */
  litestl::util::Vector<uint64_t> defs;
  /** Their serials. */
  litestl::util::Map<uint64_t, bool> serials;
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
  Frame() = default;
  ~Frame();
  Frame(const Frame &) = delete;
  Frame &operator=(const Frame &) = delete;

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
  Array<InputState> inputs;
  /** By node index; null until visited, and for nodes that are neither kernels nor
   * foreach. Owned. */
  Array<KernelState *> kernels;
  /** A cached element's: where its kernels are counted. */
  ElementCounts *counts = nullptr;

  /** Null in elements, whose foreach node depends on things for them. */
  Dependents *deps = nullptr;
  /** By input, where there are `deps`. */
  Array<InputLink> links;
  /** Other frames of the def, in `deps`. */
  Frame *nextOfDef = nullptr;
  Frame *prevOfDef = nullptr;

  /** A group node's child, checked once per pass. */
  struct Group {
    uint64_t pass = 0;
    /** Owned; null until visited. Usable only while there is no error. */
    Frame *child = nullptr;
    ErrorPtr error;
  };
  /** By index in topo->groups. */
  Array<Group> groups;

  /** Interior inputs of frames around this one that concern groups in it: the key, how many
   * of its groups lead here, and the input that replaces it. Valid in pass `pass`. */
  struct Replacing {
    const InteriorKey *key = nullptr;
    int depth = 0;
    Vertex by;
  };
  std::unique_ptr<litestl::util::Vector<Replacing>> replacing;
  /** Inputs of this frame that are replaced, if any. Valid in pass `pass`. */
  std::unique_ptr<litestl::util::Map<int, Vertex>> replaced;
  const Vertex *replacedBy(int input) const
  {
    return replaced ? replaced->lookup_ptr(input) : nullptr;
  }

  /** Calls fn(Element &) for every element of its foreach nodes. */
  template <class Fn> void forEachElement(Fn &&fn) const
  {
    for (const KernelState *k : kernels) {
      if (k && k->foreach) {
        k->foreach->forEachElement(fn);
      }
    }
  }
  NodePath pathOf(NodeId node) const;
  /** This frame and the groups below it. */
  int count() const;
};

} // namespace coredcc::graph
