#pragma once

/** \file
 * GraphInstance: a graph's evaluated state.
 */

#include "coredcc/base/status.h"
#include "coredcc/graph/def.h"
#include "coredcc/graph/value.h"

#include "litestl/util/string.h"
#include "litestl/util/vector.h"

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

namespace coredcc {

namespace graph {
class Evaluator;
}

/**
 * A node of an instance: the group nodes on the way down from its root, then the node. A
 * foreach node on the way leads into one of its elements, by key.
 *
 * Paths into elements are for `report` and `peek`, and where errors come from. They lead to
 * elements with a state of their own.
 */
struct NodePath {
  litestl::util::Vector<NodeId, 2> groups;
  NodeId node;
  /** Of each foreach node on the way, the outermost first. The origin of an error may have
   * one more: an element that its foreach node couldn't evaluate. */
  litestl::util::Vector<int64_t, 1> elements{};
  bool operator==(const NodePath &b) const;
  /** Groups first, as for nested file names; a shorter path before its extensions. */
  bool operator<(const NodePath &b) const;
  /** `12/34/56`, and with elements `12/34/56[7, 8]`. */
  std::string toString() const;
};

/** A socket of an instance: an interface output, or any socket of any node. */
struct SocketPath {
  NodePath node;
  SocketSide side = SocketSide::Output;
  /** A socket key; for an interface output (a null node), its name or key. */
  litestl::util::string key;

  /** An interface output of the root, by name or by key (`#<id>`). `peek` also takes one of
   * a graph inside, by key: with groups, and a null node. */
  static SocketPath output(std::string_view nameOrKey);
  /** A node's socket. */
  static SocketPath of(NodePath node, std::string_view key, SocketSide side = SocketSide::Output);
  static SocketPath of(NodeId node, std::string_view key, SocketSide side = SocketSide::Output);

  bool operator==(const SocketPath &b) const;
  uint64_t computeHash() const;
};

/** Why a socket has no value. */
struct GraphError {
  /** The error as the node that failed gave it, and where that was. The message has the
   * elements, too. */
  Status status;
  NodePath origin;
  /** The input at fault (a conversion, a cycle), or "" for the node. */
  litestl::util::string socket;
  bool operator==(const GraphError &b) const;
};

/** A socket that GraphInstance::prepare looked up. */
struct SocketHandle {
  uint32_t index = 0;
  /** 0 for a null handle. */
  uint32_t generation = 0;
  bool operator==(const SocketHandle &) const = default;
};

/** What an instance takes, besides its def and the values set on it. */
struct GraphMemory {
  /** Evaluated graphs: the root, its groups, and those in elements. */
  int frames = 0;
  int inputs = 0;
  int kernels = 0;
  /** Elements with a state of their own. */
  int elements = 0;
  /** All of it, about; without payloads. */
  size_t bytes = 0;
  /** Payloads in its outputs. One that several share counts for each. */
  size_t payloadBytes = 0;
};

struct NodeReport {
  enum class State : uint8_t {
    /** Not reached by an evaluation (or dropped), or not a node that can fail. */
    NotEvaluated,
    Ok,
    /** Its kernel failed, or it couldn't run. */
    Failed,
    /** Something it depends on failed. */
    InputFailed,
  };
  /** A group's is the worst of the nodes in it. */
  State state = State::NotEvaluated;
  /** Failed and InputFailed. A group's is of one of its nodes in that state. */
  GraphError error;
  /** Whether the state is that of the document as it is: evaluating what the node gives would
   * do nothing. A group's: whether it was looked at since anything moved. */
  bool current = false;
  /** How often its kernel ran. A group's numbers are the sums of its nodes'; a foreach's
   * elements aren't counted one by one. */
  uint64_t runs = 0;
  /** How long its last run took. A foreach's is with its elements. */
  double seconds = 0;
  /** How long all of its runs took. */
  double totalSeconds = 0;
  /** What it keeps: its outputs, and the frames of its group or elements. */
  GraphMemory memory;
  /** Foreach nodes: the elements of its last run, and how many of them have a state of their
   * own. The others share one: to run them again costs all of their kernels. */
  int elements = 0;
  int cachedElements = 0;
};

/** What an instance evaluates: a graph block, or a def the client keeps alive. */
struct GraphSource {
  GraphSource(BlockId id) : block(id) {}
  GraphSource(GraphDef *d) : def(d) {}
  BlockId block;
  GraphDef *def = nullptr;
};

/**
 * A graph's evaluated state: outputs, caches, errors. A plain object, not a block: the
 * client destroys it before its Runtime and Library. Several instances may evaluate one
 * def.
 *
 * Evaluation is pulled and incremental: `evaluate` visits what the requested sockets depend
 * on, and runs a node's kernel when something it read differs from its last run. Between
 * edits it costs almost nothing, and after an edit, what depends on what was edited is
 * looked at, not all of it. (All of it is when blocks were created, destroyed or renamed or
 * references written, an interface changed, or something called Runtime::touch.) See
 * docs/plans/2026-11-24-p5-graph.md.
 */
class GraphInstance {
 public:
  /** A block's def is found on every call, so it may be deleted and come back; `lib` is its
   * Library. Of a def that the client keeps, `lib` is the def's, or null to take the def's. A
   * def or Library of another Runtime, or a def of another Library, makes `evaluate` and
   * `setInput` InvalidArgument. */
  GraphInstance(Runtime *rt, Library *lib, GraphSource source);
  ~GraphInstance();
  GraphInstance(const GraphInstance &) = delete;
  GraphInstance &operator=(const GraphInstance &) = delete;

  /**
   * Evaluates sockets, giving one Status each: NotFound for a socket that isn't there, or
   * the error of the node that failed. The call itself fails, leaving `results` empty, when
   * - a graph is evaluating, or the Library is sweeping (Busy);
   * - a sticky error is pending (FailedPrecondition);
   * - the def isn't there (NotFound);
   * - the instance would need more than maxFrames groups (OutOfRange).
   */
  Status evaluate(std::span<const SocketPath> sockets, litestl::util::Vector<Status> &results);
  /** Evaluates one socket; its status, or the call's. */
  Status evaluate(const SocketPath &socket);

  /* What the last evaluation of a socket gave. These are about sockets passed to
   * `evaluate`; the pointers are valid until the next call of it, or of `clear` or `forget`.
   * `evaluate` drops what it has of sockets that are gone, whichever ones it was asked for. */

  /** Null if it has an error, or was never evaluated. */
  const GraphValue *value(const SocketPath &socket) const;
  /** The last value it had, for display while it has an error. */
  const GraphValue *lastGood(const SocketPath &socket) const;
  /** Moves when its value changes, when it gets or loses an error, and when the error
   * changes. 0 if never evaluated. */
  uint64_t changedAt(const SocketPath &socket) const;
  /** Its error, or null. */
  const GraphError *error(const SocketPath &socket) const;
  /** False if a pull might give something new: an edit, the Library or an input moved since
   * the socket was evaluated. Cheap. */
  bool isCurrent(const SocketPath &socket) const;

  /* Prepared sockets: for a client that evaluates the same ones again and again. They are
   * looked up once, and a batch of them says which moved. They aren't part of the results
   * above, and a socket may be prepared more than once. */

  /** A handle for a socket, there or not, until `release`. A socket that is gone is NotFound
   * until it is back. Busy while a graph evaluates, as `release`. */
  Result<SocketHandle> prepare(const SocketPath &socket);
  Status release(SocketHandle socket);
  /** Evaluates sockets, and gives the positions in `sockets` of those whose changedAt moved
   * since this last listed them (at first, all). What each gave is in `status`. The call
   * itself fails as with paths, or for a handle that isn't valid (InvalidArgument). */
  Status evaluate(std::span<const SocketHandle> sockets, litestl::util::Vector<int> &moved);
  /** What its last evaluation gave: ok, the error's, or NotFound for a socket that isn't
   * there. */
  Status status(SocketHandle socket) const;
  /* The same as for paths; the pointers are valid until the next call of `evaluate`,
   * `prepare` or `clear`, or the handle's `release`. */
  const GraphValue *value(SocketHandle socket) const;
  const GraphValue *lastGood(SocketHandle socket) const;
  uint64_t changedAt(SocketHandle socket) const;
  const GraphError *error(SocketHandle socket) const;
  bool isCurrent(SocketHandle socket) const;

  /** Sets an interface input, by name or by key (`#<id>`). None unsets it: the interface's
   * default then applies. NotFound for an unknown input (or def), TypeMismatch for a value of
   * another type. The value of an input that is removed stays, for when undo brings it back;
   * None by its key, or `clear`, drops it. */
  Status setInput(std::string_view nameOrKey, GraphValue v);
  /** What is known of a node, without evaluating. A group's looks at all in it. */
  NodeReport report(const NodePath &node) const;
  /** The value a socket of any node had when it was last computed, without evaluating, which
   * `report` may say isn't current. None if it wasn't computed or has an error. An input's, or a
   * group's output, is None too while an edit may have changed it. */
  GraphValue peek(const SocketPath &socket) const;
  /** Makes the next evaluation run failed kernels again, though nothing changed. */
  Status retry();
  /** Drops everything evaluated. Inputs stay, but for those the interface doesn't have. */
  Status clear();
  /** Drops what a node keeps: its outputs, or all in its group or elements. It is computed
   * again when something needs it. NotFound if nothing is kept of it, Unsupported for a path
   * into an element. */
  Status drop(const NodePath &node);
  /** Drops what is kept of a socket for `value` and the calls with it. Caches stay. */
  Status forget(const SocketPath &socket);
  /** For kernels (NodeExecContext::userData). Not a dependency. */
  void setUserData(void *data);
  /** Groups an instance may hold (default 262,144), so a graph that doubles at each level
   * fails instead of running out of memory. Each element of a foreach may have as many
   * inside. It is also how many elements, of all its foreach nodes, have a state of their
   * own: more than that share one, as those over `maxCached` do. */
  void setMaxFrames(int n);
  /** Kernels that elements with a state of their own may hold, over all foreach nodes
   * (default 131,072, of about 0.7 KB each with their inputs). Elements that come when there
   * are more share a state. */
  void setMaxCachedKernels(int n);
  /** Off, every edit makes it look at all that the sockets depend on: for tests, and to find
   * out whether a wrong result is an edit that nobody was notified of. */
  void setIncremental(bool on);
  /** What it takes. Walks all of it. */
  GraphMemory memory() const;
  /** Groups it holds, not counting those inside elements. */
  int frameCount() const;
  /** Elements of foreach nodes that have a state of their own. */
  int elementCount() const;
  /** Kernel runs since it was made, for tests and profiling. */
  uint64_t totalRuns() const;
  /** Sockets and nodes it looked at since it was made, whether or not they ran. */
  uint64_t totalVisits() const;
  /** How often everything had to be looked at again. */
  uint64_t totalFullPasses() const;

 private:
  std::unique_ptr<graph::Evaluator> e_;
};

} // namespace coredcc
