#pragma once

/** \file
 * The evaluator behind GraphInstance. Internal to graph/.
 */

#include "coredcc/graph/frame.h"
#include "coredcc/graph/system.h"

#include "litestl/util/map.h"
#include "litestl/util/vector.h"

#include <memory>
#include <optional>
#include <span>
#include <string>

namespace coredcc::graph {

/** Keeps a Library from collecting, and from destroying Doomed blocks until it ends. */
struct NoCollectScope {
  explicit NoCollectScope(Library *l) : lib(l)
  {
    if (lib) {
      lib->beginNoCollect();
    }
  }
  ~NoCollectScope()
  {
    if (lib) {
      lib->endNoCollect();
    }
  }
  NoCollectScope(const NoCollectScope &) = delete;
  NoCollectScope &operator=(const NoCollectScope &) = delete;
  Library *lib;
};

struct ElementsMap;

/** The error of a socket whose type isn't known. */
Status unknownType();

/**
 * Pulls sockets. A call walks the vertices the requested sockets depend on, depth first on
 * an explicit stack, and computes each when everything below it is done. Vertices on a
 * cycle (strongly connected components of more than one, by Tarjan's algorithm) fail
 * instead. What was computed under one key (see `Key`) stands until the key moves.
 *
 * When it moves, the GraphSystem's journal says which edits moved it. If it knows them all,
 * the vertices they touch and everything that depends on those lose their state, and the
 * rest stands; else nothing does (incremental.cc).
 */
class Evaluator {
 public:
  /** Groups and elements nested deeper than this fail. The root is at depth 0. */
  static constexpr int kMaxDepth = 64;

  Evaluator(Runtime *rt, Library *lib, GraphSource source);
  ~Evaluator();

  /** What a pass depends on: the document, and this instance's own inputs. */
  struct Key {
    GraphPassKey document;
    uint64_t own = 0;
    bool operator==(const Key &) const = default;
  };
  /** What a requested socket had. */
  struct Pulled {
    /** The pass and the number of `keyMoves_` it was pulled at. */
    uint64_t at = 0;
    ErrorPtr error;
    /** The value's source, so an unchanged one needn't be compared. */
    TraceInput origin;
    GraphValue value;
    bool hasValue = false;
    /** There is no such socket. */
    bool missing = false;
    uint64_t changedAt = 0;
    /** The vertex it led to, while `layout` is the Evaluator's. */
    Vertex vertex;
    int output = 0;
    uint64_t layout = 0;
  };

  Status evaluate(std::span<const SocketPath> sockets, litestl::util::Vector<Status> &results);
  const Pulled *result(const SocketPath &socket) const;
  /** Whether nothing `p` depends on can have moved since. */
  bool isCurrent(const Pulled &p) const;
  Key currentKey() const;

  /* prepared sockets */
  Result<SocketHandle> prepare(const SocketPath &socket);
  Status release(SocketHandle h);
  Status evaluate(std::span<const SocketHandle> sockets, litestl::util::Vector<int> &moved);
  /** Null for a handle that isn't valid. */
  const Pulled *result(SocketHandle h) const;
  Status setInput(std::string_view name, GraphValue v);
  NodeReport report(const NodePath &node) const;
  Status retry();
  Status clear();
  Status forget(const SocketPath &socket);

  GraphMemory memory() const;
  void setMaxFrames(int n)
  {
    maxFrames = n;
    failedKey_ = Key();
  }

  void *userData = nullptr;
  int maxFrames = 262144;
  int maxCachedKernels = 131072;
  bool incremental = true;
  /** Vertices visited, and passes that kept nothing, for tests and profiling. */
  uint64_t visits = 0;
  uint64_t fullPasses = 0;
  int frameCount() const
  {
    return root_ ? root_->frames : 0;
  }
  const ElementCounts &elementCounts() const
  {
    return elements_;
  }
  uint64_t totalRuns = 0;

 private:
  /** What a request leads to: a vertex, or a result without one. */
  struct Request {
    Vertex vertex;
    int output = 0;
    bool done = false;
    /** When done: NotFound, or a value or error made on the spot. */
    Status status;
    ErrorPtr error;
    TraceInput origin;
    GraphValue value;
  };

  /** What an evaluate call needs. */
  Status begin();
  /** Sets up the pass. */
  Status prepareCall();
  /** Not ok if the call went over the budget. */
  Status endCall();
  /** Pulls a socket into `p`, unless it goes over the budget. */
  void pull(const SocketPath &socket, Pulled &p, Status &status);

  /* incremental.cc */
  /** Marks what moved between passKey_ and `key` as stale. False if that isn't known, or
   * there is nothing to keep: then everything is. */
  bool absorb(const Key &key);
  bool absorb(const GraphEditNote &e);
  /** Whether nothing of this instance moved between passKey_ and `key`. */
  bool untouched(const Key &key) const;
  bool anyConcerns(const Key &key) const;
  /** Whether an edit concerns this instance. */
  bool concerns(const GraphEditNote &e) const;
  /** Whether a def is one that the root can reach through groups. */
  bool reaches(uint64_t defSerial) const;
  void findReachable() const;
  void markStale(const Vertex &v);
  void markFrame(Frame &f);

  /** The def, if it is this Runtime's and Library's. */
  GraphDef *findDef(Status &status) const;
  Status prepareRoot();
  /** Drops the frames made in this call, which went over the budget. */
  void dropNewFrames();
  void dropFrame(Frame *f);

  /* frames */
  void sync(Frame &f);
  /** Keeps the frame's def in memory until this call ends, whatever a kernel destroys. */
  void pinDef(Frame &f);
  Frame *child(Frame &f, int group, ErrorPtr &error);
  /** Whether a node of `f` can evaluate `def`, its target. */
  Status checkGroup(const Frame &f, const GraphNode &node, const GraphDef *def) const;
  /** Notes which inputs of `c`, and of the groups in it, others replace (graph/interior.h). */
  void setReplaced(Frame &f, int group, Frame &c);
  Frame *newFrame(Frame &parent, const GraphNode &node, GraphDef *def);
  void setOverBudget(const Frame &f, NodeId node);
  KernelState &kernel(Frame &f, int node);
  ErrorPtr makeError(const Frame &f, NodeId node, Status status, std::string_view socket = {}) const;

  /* the walk */
  Request find(const SocketPath &socket);
  bool walk(Vertex root);
  WalkState &walkState(const Vertex &v);
  void enter(const Vertex &v);
  /** What an output of a node leads to: a vertex, an error, or neither for a constant. */
  struct Feed {
    Vertex vertex;
    int output = 0;
    ErrorPtr error;
  };
  Feed feed(Frame &f, int node, int output);
  /** The vertex an input waits for; none when it can be computed. */
  std::optional<Vertex> follow(Frame &f, int input);
  /** A value that doesn't come from a kernel: where from, and the value. */
  struct Constant {
    Origin origin;
    GraphValue value;
  };
  /** The value of an element's interface input. */
  const GraphValue *elementInput(const Frame &f,
                                 int interfaceIndex,
                                 const SocketType &type,
                                 TraceInput &origin);
  void computeInput(Frame &f, int input);
  Constant unlinked(const Frame &f, int input) const;
  Constant external(int interfaceIndex, const SocketType &type) const;
  Constant typeDefault(const SocketType &type) const;
  void computeKernel(Frame &f, int node);
  void run(Frame &f, int node, KernelState &k, Trace trace);
  /* foreach.cc */
  void computeForeach(Frame &f, int node);
  /** Whether anything but the table moved since the last run; notes the new state. */
  bool worldMoved(Frame &f, int node, KernelState &k, const GraphDef &def);
  ErrorPtr runElements(Frame &f, int node, KernelState &k, GraphDef &def, bool newTable);
  /** Sets an element's inputs to a row of the table. `same` becomes false if any moved. */
  Status setRow(Element &e, const ElementsMap &map, int row, bool &same);
  /** Evaluates an element's outputs. */
  ErrorPtr runElement(Frame &f, int node, KernelState &k, Element &e);
  void settle(KernelState &k, ErrorPtr error, bool inputFailed);
  void failCycle(const litestl::util::Vector<Vertex> &members);

  void store(Pulled &p, const Request &r, Status &status);
  /** Whether the defs have the socket. True where a group fails, so that it can't be known. */
  bool exists(const SocketPath &socket) const;
  /** Drops results of sockets that are gone. */
  void pruneResults();

  Runtime &rt_;
  LibraryRef lib_;
  GraphSource source_;
  ElementCounts elements_;
  Dependents deps_;
  std::unique_ptr<Frame> root_;
  BlockReader blocks_;
  TypeChains chains_;

  struct External {
    GraphValue value;
    uint64_t version = 0;
  };
  litestl::util::Map<int64_t, External> inputs_;
  /** Moves with setInput and retry. */
  uint64_t own_ = 1;
  /** The inputs (by interface id) it moved for since the last evaluation; `ownAll_` if it
   * wasn't for those only. */
  litestl::util::Vector<int64_t> ownInputs_;
  bool ownAll_ = false;
  litestl::util::Map<SocketPath, Pulled> results_;
  struct Prepared {
    SocketPath path;
    Pulled pulled;
    /** The changedAt that was last in a `moved` list. */
    uint64_t reported = 0;
    uint32_t generation = 0;
    bool used = false;
  };
  litestl::util::Vector<Prepared> prepared_;
  litestl::util::Vector<uint32_t> freePrepared_;
  /** GraphSystem::structureVersion, and the Library's epoch (a write of a group's cell moves
   * only that), when results_ was last pruned. */
  uint64_t prunedStructure_ = 0;
  uint64_t prunedEpoch_ = 0;

  /** The current pass, and the last one's number. Each run of an element is a pass of its
   * own, over its frames only. */
  uint64_t pass_ = 0;
  uint64_t passes_ = 0;
  /** Where the innermost foreach that runs collects its blocks. */
  struct Sink {
    DepSet *set = nullptr;
    Trace *trace = nullptr;
  };
  Sink sink_;
  Key passKey_;
  /** What `untouched` said last, as isCurrent may be asked of many sockets. */
  mutable Key askedKey_;
  mutable bool answer_ = false;
  /** Counts the moves of passKey_. */
  uint64_t keyMoves_ = 0;
  /** The first edit that passKey_ doesn't include. */
  uint64_t seen_ = 0;
  /** Moves when frames may have moved or gone. */
  uint64_t layout_ = 1;
  /** The defs that the root reaches, and whether one uses interior sockets. Valid while
   * reachablePass_ is pass_. */
  mutable litestl::util::Map<uint64_t, bool> reachable_;
  mutable bool interior_ = false;
  mutable uint64_t reachablePass_ = 0;
  litestl::util::Vector<Vertex> marks_;
  uint64_t call_ = 0;
  litestl::util::Vector<LibraryPin, 4> pins_;
  bool running_ = false;
  bool overBudget_ = false;
  std::string overBudgetAt_;
  /** The last call that went over the budget: its key, and its sockets up to the one that
   * did. Another like it would too. */
  Key failedKey_;
  Status failed_;
  litestl::util::Vector<SocketPath> failedPaths_;
  litestl::util::Vector<SocketHandle> failedHandles_;

  struct Item {
    Vertex vertex;
    int next = 0;
  };
  litestl::util::Vector<Item> stack_;
  /** Tarjan's stack: visited vertices whose component isn't known yet. */
  litestl::util::Vector<Vertex> component_;
  int numbers_ = 0;
};

} // namespace coredcc::graph
