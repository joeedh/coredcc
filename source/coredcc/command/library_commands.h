#pragma once

/** \file
 * Undoable ways to add blocks to a library:
 * - `core.block.duplicate(block=<Pointer> blocks="")` copies the block and those of `blocks`
 *   (formatBlockIds text) together (lib/duplicate.h). Outputs the copies: `block` is the first,
 *   and `blocks` all of them, in order. Registered by registerBlockCommands.
 * - appendUndo, for a file's blocks.
 * Undo moves what they made to the history; redo brings it back with its ids. A block that filled
 * a MissingBlock is one again after the undo.
 */

#include "coredcc/api/library_io.h"
#include "coredcc/command/command.h"
#include "coredcc/lib/library.h"

namespace coredcc {

inline constexpr const char *kDuplicateBlockCommand = "core.block.duplicate";
/** The Private command behind appendUndo. */
inline constexpr const char *kAppendCommand = "core.library.append";

/**
 * appendLibrary as an undo step, or as a sub-command inside a command's exec. Undo leaves the
 * MissingBlock records it made, which nothing references then: the host may have filled them.
 */
Result<AppendResult> appendUndo(Context &ctx,
                                std::span<const uint8_t> bytes,
                                const AppendOptions &options = {});

namespace detail {
Status registerLibraryCommands(Runtime &rt);
} // namespace detail

} // namespace coredcc
