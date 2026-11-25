#pragma once

/** \file
 * Undo strategy helpers a command composes (architecture §6, "Undo strategies"). The
 * Custom strategy is the command's own undo; CreatedBlocks (block_commands.h) covers
 * block creation.
 */

#include "coredcc/api/context.h"
#include "coredcc/base/iface/iface.h"
#include "coredcc/base/status.h"
#include "coredcc/lib/block.h"

#include "litestl/util/function.h"
#include "litestl/util/vector.h"

#include <cstdint>
#include <utility>

namespace coredcc {

/**
 * Delegated undo: a step in a domain journal (sculptcore's MeshLog, an ubertopo
 * savepoint), driven through three callbacks. Forward the command's undo, redo and
 * discard slots to it. `release` runs once, at discard or destruction.
 */
class DelegatedUndo {
public:
  using UndoFn = litestl::util::function<void()>;
  using RedoFn = litestl::util::function<Status()>;
  using ReleaseFn = litestl::util::function<void()>;

  DelegatedUndo() = default;
  DelegatedUndo(UndoFn undo, RedoFn redo, ReleaseFn release, uint64_t memSize = 0)
      : undo_(std::move(undo)), redo_(std::move(redo)), release_(std::move(release)),
        memSize_(memSize)
  {
  }
  DelegatedUndo(const DelegatedUndo &) = delete;
  DelegatedUndo &operator=(const DelegatedUndo &) = delete;
  /* Moves clear the source explicitly: a moved-from litestl function may still hold its
   * target, and the source's destructor would release the step a second time. */
  DelegatedUndo(DelegatedUndo &&b)
      : undo_(std::exchange(b.undo_, UndoFn())), redo_(std::exchange(b.redo_, RedoFn())),
        release_(std::exchange(b.release_, ReleaseFn())), memSize_(b.memSize_)
  {
  }
  DelegatedUndo &operator=(DelegatedUndo &&b)
  {
    if (this != &b) {
      discard();
      undo_ = std::exchange(b.undo_, UndoFn());
      redo_ = std::exchange(b.redo_, RedoFn());
      release_ = std::exchange(b.release_, ReleaseFn());
      memSize_ = b.memSize_;
    }
    return *this;
  }
  ~DelegatedUndo()
  {
    discard();
  }

  void undo()
  {
    if (undo_) {
      undo_();
    }
  }
  Status redo()
  {
    return redo_ ? redo_() : Status();
  }
  void discard()
  {
    if (release_) {
      ReleaseFn r = std::move(release_);
      release_ = ReleaseFn();
      r();
    }
  }
  uint64_t memSize() const
  {
    return memSize_;
  }

private:
  UndoFn undo_;
  RedoFn redo_;
  ReleaseFn release_;
  uint64_t memSize_ = 0;
};

/**
 * Snapshot undo: a host-registered provider that captures and restores state as bytes.
 * Synchronous. SnapshotUndo (snapshot_undo.h) is the command's side.
 *
 * - `ctx` says which document. When a failed command is rolled back it has only the
 * Runtime, the Library and the root.
 * - `capture` appends the state's bytes to `blob`, which is empty.
 * - `foreachRef` (optional): visits a cell for each block that `blob` references, so undo
 *   keeps those blocks. The cells are the provider's own; writes to them are ignored.
 */
struct SnapshotProvider : IfaceRef {
  Status capture(Context &ctx, litestl::util::Vector<uint8_t> &blob) const
  {
    return CDCC_CALL(capture, ctx, blob);
  }
  Status restore(Context &ctx, const litestl::util::Vector<uint8_t> &blob)
  {
    return CDCC_CALL(restore, ctx, blob);
  }
  bool hasForeachRef() const
  {
    return CDCC_HAS(foreachRef);
  }
  void foreachRef(Context &ctx,
                  const litestl::util::Vector<uint8_t> &blob,
                  RefVisitor &v) const
  {
    CDCC_CALL(foreachRef, ctx, blob, v);
  }

  CDCC_INTERFACE(SnapshotProvider, "coredcc::SnapshotProvider")
  {
    BIND_STRUCT_METHOD(st, capture, MARGS("ctx", "blob"));
    BIND_STRUCT_METHOD(st, restore, MARGS("ctx", "blob"));
    BIND_OPTIONAL_METHOD(st, foreachRef, MARGS("ctx", "blob", "v"));
  }
};

} // namespace coredcc
