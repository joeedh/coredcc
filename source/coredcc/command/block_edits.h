#pragma once

/** \file
 * Commands that edit a block's record (registered by registerBlockCommands):
 * - `core.block.rename(block=<Pointer> name=<String>)`. Posts a change of the owner's
 *   `name`.
 * - `core.block.unlinkRef(block=<Pointer> index=<Int> attachment="")` nulls a row of the ref
 *   table of a placeholder, or of the block's opaque attachment of the named type, so the block
 *   it references can be deleted.
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

/** Registers a command unless one of its name exists. */
template <class Cmd> Status addCommandOnce(Runtime &rt, CommandDef def)
{
  if (rt.ext<CommandRegistry>().find(def.name().c_str())) {
    return Status();
  }
  return addNativeCommand<Cmd>(rt, std::move(def)).status();
}
} // namespace detail

} // namespace coredcc
