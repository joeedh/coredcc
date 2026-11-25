#pragma once

/** \file
 * Undo support for block lifetimes: the delete cascade and its preview, the built-in
 * delete and purge commands, and CreatedBlocks for commands that create blocks
 * (architecture §8a, "Lifetime and garbage collection").
 */

#include "coredcc/command/command.h"
#include "coredcc/lib/library.h"

namespace coredcc {

/**
 * The blocks deleting `target` takes with it: Live and Placeholder users whose non-null `Ref`
 * (RefKind::Strong) to a deleted block would break, transitively, comparing canonical
 * ids. `target` comes first. Nullable refs aren't followed: they resolve to null while
 * their target is deleted.
 */
litestl::util::Vector<BlockHandle> deletePreview(const Library &lib, BlockHandle target);

/**
 * Blocks a command created, for its undo: undo moves them to the history, redo brings
 * them back with their ids, and a discard while undone destroys them. Call `add` from
 * exec after each create. Each block is pinned until the discard (a history pin, §8a),
 * so a purge can't free a block that redo would bring back.
 */
class CreatedBlocks {
 public:
  void add(Library &lib, BlockHandle h);
  int size() const
  {
    return int(blocks_.size());
  }
  void undo(Library &lib);
  Status redo(Library &lib);
  void discard(Library &lib);

 private:
  litestl::util::Vector<BlockHandle> blocks_;
  bool undone_ = false;
};

/**
 * Registers the built-in block commands (idempotent):
 * - `core.block.delete(block=<Pointer to a block> cascade=true)` deletes the block and
 *   its preview. With `cascade=false` a non-empty cascade is FailedPrecondition naming
 *   the users. Undo restores them all with their ids. Outputs `count`.
 * - `core.block.rename` and `core.block.unlinkRef` (block_edits.h).
 * - `core.block.purge()` collects garbage (NoUndo). Outputs `count`, the blocks freed.
 */
Status registerBlockCommands(Runtime &rt);

inline constexpr const char *kDeleteBlockCommand = "core.block.delete";
inline constexpr const char *kPurgeCommand = "core.block.purge";

} // namespace coredcc
