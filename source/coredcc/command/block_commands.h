#pragma once

/** \file
 * Undo support for block lifetimes: the delete cascade and its preview, the built-in
 * delete and purge commands, and CreatedBlocks for commands that create blocks
 * (architecture §8a, "Lifetime and garbage collection").
 */

#include "coredcc/command/command.h"
#include "coredcc/command/history_block.h"
#include "coredcc/lib/library.h"

#include <span>

namespace coredcc {

/**
 * The blocks deleting `target` takes with it: Live and Placeholder users whose non-null `Ref`
 * (RefKind::Strong) to a deleted block would break, transitively, comparing canonical
 * ids. `target` comes first. Nullable refs aren't followed: they resolve to null while
 * their target is deleted.
 */
litestl::util::Vector<BlockHandle> deletePreview(const Library &lib, BlockHandle target);
/** The same for several, which come first, each once; null handles are skipped. The relations are
 * built once, however many there are. */
litestl::util::Vector<BlockHandle> deletePreview(const Library &lib,
                                                 std::span<const BlockHandle> targets);

/**
 * Blocks a command created, for its undo: undo moves them to the history, redo brings
 * them back with their ids, and a discard while undone destroys them. Call `add` from
 * exec after each create. Each block is pinned until the discard (a history pin, §8a),
 * so a purge can't free a block that redo would bring back. Blocks are found by id, so a
 * block recreated under its id in between is still the one that is undone.
 *
 * A block that filled a MissingBlock (createWithId, an append) is one again after the undo, with
 * the references, slots and attachments it had, and a discard then destroys the data only.
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
  /** What the history holds for it: the blocks' data too, while they are undone. */
  uint64_t memSize() const;

 private:
  struct Made {
    HistoryBlock block;
    /** It filled a MissingBlock. */
    bool adopted = false;
    /** Undone: the data that did. */
    LibraryPin kept;
  };
  static Status undoOne(Library &lib, Made &m);
  static Status redoOne(Library &lib, Made &m);

  litestl::util::Vector<Made> blocks_;
  bool undone_ = false;
};

/**
 * Registers the built-in block commands (idempotent):
 * - `core.block.delete(block=<Pointer to a block> blocks="" cascade=true)` deletes the block,
 *   those of `blocks` (formatBlockIds text) and their preview, in one step. With `cascade=false`
 *   a non-empty cascade is FailedPrecondition naming the users. Undo restores them all with
 *   their ids. Outputs `count`.
 * - `core.block.rename` and `core.block.unlinkRef` (block_edits.h).
 * - `core.block.attach` and `core.block.detach` (attachment_commands.h).
 * - `core.block.duplicate` (library_commands.h).
 * - `core.block.purge()` collects garbage (NoUndo). Outputs `count`, the blocks freed.
 */
Status registerBlockCommands(Runtime &rt);

inline constexpr const char *kDeleteBlockCommand = "core.block.delete";
inline constexpr const char *kPurgeCommand = "core.block.purge";

} // namespace coredcc
