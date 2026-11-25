#pragma once

/** \file
 * NodeExecContext: what a kernel sees while it runs.
 */

#include "coredcc/api/change_bus.h"
#include "coredcc/base/runtime.h"
#include "coredcc/graph/value.h"
#include "coredcc/lib/block.h"

#include "litestl/util/vector.h"

#include <span>
#include <string_view>

namespace coredcc {

struct NodeType;

/** A block a kernel uses. */
struct BlockUse {
  /** Null when there is no such block. */
  BlockHandle handle;
  /** Its data, or null unless the block is Live. */
  void *data = nullptr;
};

/** Where a NodeExecContext gets blocks from; it records that it did. The evaluator's, or a
 * test's. */
class ExecBlocks {
 public:
  virtual BlockUse useBlock(BlockId id, ChangeAspects aspects) = 0;
  virtual IfaceRef queryBlock(BlockHandle h, InterfaceId iface) = 0;

 protected:
  ~ExecBlocks() = default;
};

/** What a kernel may ask of, and tell, the evaluation it is in. The evaluator's, or a test's. */
class ExecMonitor {
 public:
  virtual bool cancelled() = 0;
  virtual void progress(double fraction, std::string_view text) = 0;

 protected:
  ~ExecMonitor() = default;
};

/**
 * A kernel's inputs, params and outputs. Valid only during its `evaluate`.
 *
 * A kernel must be a pure function of what it reads here: the evaluator runs it again only
 * when some of that changed. It reads blocks with `useBlock` and `paramBlock` (and
 * `queryBlock`), which record them as dependencies, and writes only its outputs.
 *
 * An error that a kernel returns is kept as a value is, until something it read changes. Only
 * Cancelled isn't: the node runs again in the next evaluation that needs it.
 */
class NodeExecContext {
 public:
  struct Input {
    const GraphValue *value = nullptr;
    bool linked = false;
  };
  /** For the evaluator and tests. `inputs`, `params` and `paramBlocks` follow the node
   * type's inputs and params; `paramBlocks` is null for params that aren't pointers. */
  struct Setup {
    Runtime *rt = nullptr;
    const NodeType *type = nullptr;
    std::span<const Input> inputs;
    std::span<const PropValue> params;
    std::span<const BlockId> paramBlocks;
    Library *library = nullptr;
    ExecBlocks *blocks = nullptr;
    ExecMonitor *monitor = nullptr;
    void *userData = nullptr;
  };
  explicit NodeExecContext(const Setup &setup);
  NodeExecContext(const NodeExecContext &) = delete;
  NodeExecContext &operator=(const NodeExecContext &) = delete;

  const NodeType &type() const
  {
    return *s_.type;
  }
  Runtime &runtime() const
  {
    return *s_.rt;
  }
  /** The Library, or null. Blocks read through it aren't dependencies: use useBlock. */
  const Library *library() const
  {
    return s_.library;
  }
  /** Whether the client wants the evaluation to end (GraphInstance::setMonitor). A kernel that
   * takes long asks now and then, and returns Cancelled. */
  bool cancelled() const
  {
    return s_.monitor && s_.monitor->cancelled();
  }
  /** Tells the client how far the kernel is, 0 to 1, and what it is doing. */
  void progress(double fraction, std::string_view text = {})
  {
    if (s_.monitor) {
      s_.monitor->progress(fraction, text);
    }
  }
  /** What the client gave GraphInstance::setUserData. Changing it runs no kernel. The client
   * changes nothing there that kernels read while it evaluates. */
  void *userData() const
  {
    return s_.userData;
  }

  int inputCount() const
  {
    return int(s_.inputs.size());
  }
  /** The input's value, of its socket's type. Aborts on a bad index. */
  const GraphValue &input(int i) const;
  /** Null when there is no such input. */
  const GraphValue *input(std::string_view name) const;
  /** Whether the input is linked; false when there is no such input. */
  bool isLinked(int i) const;
  bool isLinked(std::string_view name) const;

  /** A param's value; null when there is no such param. A Pointer param's is None: use
   * paramBlock. */
  const PropValue *param(std::string_view name) const;
  /** useBlock of a Pointer param's target. Nothing is recorded for a null pointer. */
  BlockUse paramBlock(std::string_view name, ChangeAspects aspects = kAllAspects);

  /** A block, recorded as a dependency: the node runs again when it changes, appears or
   * goes away. The handle is null if there is none.
   *
   * With `aspects`, only changes of those count (ChangeBus::notify), as if the kernel read
   * nothing else of it: one that reads a mesh's positions doesn't run when its selection
   * changes. Appearing, going away and being renamed always count. */
  BlockUse useBlock(BlockId id, ChangeAspects aspects = kAllAspects);
  /** An interface of a block; the answer is recorded as a dependency. Null if it has none.
   * What a block answers may change only with a notification of the block or a move of the
   * Library's epoch (an override does move it): without either, the node isn't looked at
   * again. */
  template <class I> I queryBlock(BlockHandle h)
  {
    I out;
    static_cast<IfaceRef &>(out) = queryBlockRaw(h, InterfaceId::of(I::ifaceName));
    return out;
  }
  IfaceRef queryBlockRaw(BlockHandle h, InterfaceId iface);
  /** makeAlias for views into the data of blocks this kernel got from useBlock. */
  template <class T> Result<PayloadRef> alias(T view, std::span<const BlockHandle> blocks)
  {
    if (!s_.library) {
      return Status::error(StatusCode::NotFound, "alias: there is no Library");
    }
    return makeAlias<T>(std::move(view), *s_.library, blocks);
  }
  template <class T> Result<PayloadRef> alias(T view, BlockHandle block)
  {
    return alias<T>(std::move(view), std::span<const BlockHandle>(&block, 1));
  }

  /** Sets an output. A value of another type is TypeMismatch, and fails the node even if
   * the kernel then returns ok. */
  Status setOutput(int i, GraphValue v);
  Status setOutput(std::string_view name, GraphValue v);

  /* ---- for the evaluator ---- */

  /** Internal if an output isn't set, or a setOutput failed. */
  Status checkOutputs() const;
  /** The outputs, None where not set. */
  litestl::util::Vector<GraphValue> &outputs()
  {
    return outputs_;
  }

  /** Bound without members: scripts use bind/'s facades. */
  static const litestl::binding::types::Struct<NodeExecContext> *defineBindings();

 private:
  Setup s_;
  litestl::util::Vector<GraphValue> outputs_;
  Status outputError_;
};

} // namespace coredcc
