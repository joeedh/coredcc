#pragma once

/** \file
 * The evaluator behind GraphInstance. Internal to graph/.
 */

#include "coredcc/graph/frame.h"

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
    Key key;
    ErrorPtr error;
    /** The value's source, so an unchanged one needn't be compared. */
    TraceInput origin;
    GraphValue value;
    bool hasValue = false;
    uint64_t changedAt = 0;
  };

  Status evaluate(std::span<const SocketPath> sockets, litestl::util::Vector<Status> &results);
  const Pulled *result(const SocketPath &socket) const;
  Key currentKey() const;
  Status setInput(std::string_view name, GraphValue v);
  NodeReport report(const NodePath &node) const;
  Status retry();
  Status clear();

  void *userData = nullptr;
  int maxFrames = 262144;
  int frameCount() const
  {
    return root_ ? root_->frames : 0;
  }
  int elementCount() const
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
  Frame *newFrame(Frame &parent, const GraphNode &node, GraphDef *def);
  KernelState &kernel(Frame &f, int node);
  ErrorPtr makeError(const Frame &f, NodeId node, Status status, std::string_view socket = {}) const;

  /* the walk */
  Request find(const SocketPath &socket);
  bool walk(Vertex root);
  WalkState &walkState(const Vertex &v);
  void enter(const Vertex &v);
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

  void store(const SocketPath &socket, const Request &r, Status &status);

  Runtime &rt_;
  LibraryRef lib_;
  GraphSource source_;
  /** Counted by the elements themselves. */
  int elements_ = 0;
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
  litestl::util::Map<SocketPath, Pulled> results_;

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
  uint64_t call_ = 0;
  litestl::util::Vector<LibraryPin, 4> pins_;
  bool running_ = false;
  bool overBudget_ = false;

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
