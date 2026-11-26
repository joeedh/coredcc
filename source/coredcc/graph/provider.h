#pragma once

/** \file
 * The `coredcc.GraphDef` block type: graphs as datablocks.
 */

#include "coredcc/graph/def.h"
#include "coredcc/lib/library.h"

#include <string_view>

namespace coredcc {

/** The block type's name; its client is the built-in one. */
inline constexpr const char *kGraphDefType = "coredcc.GraphDef";

/** Registers the block type and its provider. Idempotent. */
Status registerGraphBlocks(Runtime &rt);

/** A new graph block, with an empty GraphDef. */
Result<BlockHandle> newGraph(Library &lib, std::string_view name);

/** The GraphDef of a graph block; null for another type, or while it isn't Live. */
GraphDef *graphDefOf(const Library &lib, BlockHandle h);

} // namespace coredcc
