#pragma once

/** \file
 * Snapshot undo: state a command can't describe, captured as bytes before and after it
 * (architecture §6).
 */

#include "coredcc/api/context.h"
#include "coredcc/base/iface/owned.h"
#include "coredcc/command/history_block.h"
#include "coredcc/command/undo_strategies.h"

#include "litestl/util/vector.h"

#include <string>
#include <string_view>

namespace coredcc {

/** Snapshot providers by name. A Runtime extension. */
class SnapshotRegistry {
public:
  static constexpr const char *kExtName = "coredcc.SnapshotRegistry";

  explicit SnapshotRegistry(Runtime *)
  {
  }
  ~SnapshotRegistry();
  SnapshotRegistry(const SnapshotRegistry &) = delete;
  SnapshotRegistry &operator=(const SnapshotRegistry &) = delete;

  /** Takes the provider. AlreadyExists for a name that is taken. */
  Status add(std::string_view name, Owned<SnapshotProvider> provider);
  /** Destroys the provider, when its host goes. Undo steps that need it fail then. */
  Status remove(std::string_view name);
  /** Null when there is none. */
  SnapshotProvider *find(std::string_view name);

  Status validate() const;

private:
  struct Entry {
    std::string name;
    Owned<SnapshotProvider> provider;
  };
  litestl::util::Vector<Entry *> entries_;
};

/**
 * The undo of a command whose effect only a snapshot can take back. A member of the
 * command:
 *
 * - exec calls `begin` first and `end` last, after its sub-commands. An Interactive
 * command's tool calls `end` (again, if it likes) before `commit`. When exec fails after
 * `begin`, a failed `end` included, it calls `rollback` and returns the error.
 * - Forward the command's undo, redo, memSize, discard and foldFrom to it. Without `end`,
 * undo works and redo is FailedPrecondition.
 * - Both blobs are there when the history asks for memSize, compressed with lz4.
 * - A restore is followed by Library::noteRefsChanged and ChangeBus::notifyAll,
 *   also when it failed: an opaque blob can't say what it changed.
 *
 * Blocks that a blob references are kept (History pins) while this does, if the provider
 * has `foreachRef`.
 */
class SnapshotUndo {
public:
  SnapshotUndo() = default;
  /** `provider` is a name in the SnapshotRegistry, looked up on each use. */
  explicit SnapshotUndo(std::string provider) : name_(std::move(provider))
  {
  }
  SnapshotUndo(SnapshotUndo &&) = default;
  SnapshotUndo &operator=(SnapshotUndo &&) = default;
  SnapshotUndo(const SnapshotUndo &) = delete;
  SnapshotUndo &operator=(const SnapshotUndo &) = delete;

  /** Captures before. Again, it starts over. */
  Status begin(Context &ctx);
  /** Captures after. FailedPrecondition without begin. */
  Status end(Context &ctx);
  /** Restores before. An error becomes the sticky error. */
  void undo(Context &ctx);
  /** Restores after. If it fails, the executor undoes the sub-commands that ran: one that
   * writes the captured state itself then leaves what it had in the middle of the step.
   */
  Status redo(Context &ctx);
  /** Restores before, now and once more at the discard, when the sub-commands that ran
   * have been undone. */
  Status rollback(Context &ctx);
  /** Takes what `next` captured after, so that redo does both steps. False, with nothing
   * done, if they use different providers or one has no `end`. */
  bool foldFrom(SnapshotUndo &next);
  uint64_t memSize() const;
  void discard();

private:
  /** A captured blob, compressed. */
  struct Blob {
    bool taken = false;
    bool lz4 = false;
    uint64_t raw = 0;
    litestl::util::Vector<uint8_t> bytes;
    litestl::util::Vector<HistoryBlock> targets;
  };

  Status capture(Context &ctx, Blob &to);
  Status restore(Context &ctx, const Blob &from);
  /** Whether `ctx` is of the Library that was captured. */
  Status sameLibrary(Context &ctx) const;

  std::string name_;
  Blob before_, after_;
  Runtime *rt_ = nullptr;
  LibraryRef lib_;
  bool inLibrary_ = false;
  PointerRef root_;
  bool restoreAtDiscard_ = false;
};

} // namespace coredcc
