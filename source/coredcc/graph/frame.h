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

/** A kernel node. Kept across edits of the def, by node id and type. */
struct KernelState {
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
  /** The outcome of the last visit: the run's error, or an input's. */
  bool visited = false;
  bool inputFailed = false;
  ErrorPtr error;
};

/** One evaluated def: the root, or a group. */
struct Frame {
  ~Frame();

  GraphDef *def = nullptr;
  uint64_t defSerial = 0;
  Frame *parent = nullptr;
  /** The group node in the parent. */
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
  /** By node index; null until visited, and for nodes that aren't kernels. Owned. */
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

  NodePath pathOf(NodeId node) const;
  /** This frame and those below it. */
  int count() const;
};

} // namespace coredcc::graph
