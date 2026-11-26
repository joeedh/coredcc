#pragma once

/** \file
 * Undoable attachments (registered by registerBlockCommands):
 * - `core.block.attach(block=<Pointer> type=<String>)` attaches data made by the type's
 *   `create`.
 * - `core.block.detach(block=<Pointer> type=<String>)`, also of an attachment that this
 * build couldn't read. The command owns detached data until it leaves the history, and
 * keeps what the data references from being purged. Both post a change of the block's
 * `attachments`.
 */

#include "coredcc/command/command.h"
#include "coredcc/lib/library.h"

namespace coredcc {

inline constexpr const char *kAttachBlockCommand = "core.block.attach";
inline constexpr const char *kDetachBlockCommand = "core.block.detach";
/** The Private command behind attachUndo. */
inline constexpr const char *kAttachBlockDataCommand = "core.block.attachData";

/**
 * Attaches `data` to `h` as an undo step, or as a sub-command inside a command's exec.
 * Always takes `data`: on an error the type's provider has destroyed it. The one
 * exception is InvalidArgument, for data that is null, or of a type that isn't
 * registered, which nobody here could destroy.
 */
Status attachUndo(Context &ctx, BlockHandle h, TypeId type, void *data);

namespace detail {
Status registerAttachmentCommands(Runtime &rt);
} // namespace detail

} // namespace coredcc
