#pragma once

/** \file
 * Commands that edit a block's record (registered by registerBlockCommands):
 * - `core.block.rename(block=<Pointer> name=<String>)`. Posts a change of the owner's
 *   `name`.
 * - `core.block.unlinkRef(block=<Pointer to a placeholder> index=<Int>)` nulls a row of a
 *   placeholder's ref table, so the block it references can be deleted.
 */

#include "coredcc/command/command.h"
#include "coredcc/lib/library.h"

namespace coredcc {

inline constexpr const char *kRenameBlockCommand = "core.block.rename";
inline constexpr const char *kUnlinkRefCommand = "core.block.unlinkRef";

namespace detail {
/** The block a command's `block` input points into. FailedPrecondition when there is no
 * Library, or no such block. */
Result<BlockHandle> blockInput(Context &ctx);
Status registerBlockEdits(Runtime &rt);
} // namespace detail

} // namespace coredcc
