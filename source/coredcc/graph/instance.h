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

/** A node of an instance: the group nodes on the way down from its root, then the node. */
struct NodePath {
  litestl::util::Vector<NodeId, 2> groups;
  NodeId node;
  bool operator==(const NodePath &b) const;
  /** Groups first, as for nested file names; a shorter path before its extensions. */
  bool operator<(const NodePath &b) const;
  /** `12/34/56`. */
  std::string toString() const;
};

/** A socket of an instance: an interface output, or any socket of any node. */
struct SocketPath {
  NodePath node;
  SocketSide side = SocketSide::Output;
  /** A socket key; for an interface output (a null node), its name or key. */
  litestl::util::string key;

  /** An interface output of the root, by name or by key (`#<id>`). */
  static SocketPath output(std::string_view nameOrKey);
  /** A node's socket. */
  static SocketPath of(NodePath node, std::string_view key, SocketSide side = SocketSide::Output);
  static SocketPath of(NodeId node, std::string_view key, SocketSide side = SocketSide::Output);

  bool operator==(const SocketPath &b) const;
  uint64_t computeHash() const;
};

/** Why a socket has no value. */
struct GraphError {
  /** The error as the node that failed gave it, and where that was. */
  Status status;
  NodePath origin;
  /** The input at fault (a conversion, a cycle), or "" for the node. */
  litestl::util::string socket;
  /** If it is in elements of foreach nodes: their keys, the outermost first. The message
   * has them too. */
  litestl::util::Vector<int64_t, 1> elements;
  bool operator==(const GraphError &b) const;
};

struct NodeReport {
  enum class State : uint8_t {
    /** Never reached by an evaluation, or not a node that can fail. */
    NotEvaluated,
    Ok,
    /** Its kernel failed, or it couldn't run. */
    Failed,
    /** Something it depends on failed. */
    InputFailed,
  };
  State state = State::NotEvaluated;
  /** Failed and InputFailed. */
  GraphError error;
  /** How often its kernel ran. */
  uint64_t runs = 0;
  /** How long its last run took. */
  double seconds = 0;
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
 * edits it costs almost nothing. See docs/plans/2026-11-24-p5-graph.md.
 */
class GraphInstance {
 public:
  /** The def is found on every call, so it may be deleted and come back. `lib` may be null
   * for a def without a Library. */
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
   * `evaluate`; the pointers are valid until the next call of it or of `clear`. */

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

  /** Sets an interface input. None unsets it: the interface's default then applies.
   * NotFound for an unknown input (or def), TypeMismatch for a value of another type. */
  Status setInput(std::string_view name, GraphValue v);
  NodeReport report(const NodePath &node) const;
  /** Makes the next evaluation run failed kernels again, though nothing changed. */
  Status retry();
  /** Drops everything evaluated. Inputs stay. */
  Status clear();
  /** For kernels (NodeExecContext::userData). Not a dependency. */
  void setUserData(void *data);
  /** Groups an instance may hold (default 262,144), so a graph that doubles at each level
   * fails instead of running out of memory. Each element of a foreach may have as many
   * inside. It is also how many elements, of all its foreach nodes, have a state of their
   * own: more than that share one, as those over `maxCached` do. */
  void setMaxFrames(int n);
  /** Groups it holds, not counting those inside elements. */
  int frameCount() const;
  /** Elements of foreach nodes that have a state of their own. */
  int elementCount() const;
  /** Kernel runs since it was made, for tests and profiling. */
  uint64_t totalRuns() const;

 private:
  std::unique_ptr<graph::Evaluator> e_;
};

} // namespace coredcc
