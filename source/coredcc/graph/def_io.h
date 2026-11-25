#pragma once

/** \file
 * A GraphDef as a block payload (files, duplicating).
 */

#include "coredcc/graph/def.h"
#include "coredcc/lib/block_io.h"

namespace coredcc {

/** The payload version writeGraphDef writes. */
inline constexpr uint32_t kGraphDefPayload = 1;

/**
 * Writes nodes, links and the interface, with their ids. Versions aren't written.
 *
 * Nodes also have their sockets as they are now, and values their types, so a build that
 * doesn't know a node's type reads and writes it without loss.
 */
void writeGraphDef(const GraphDef &def, BlockWriter &out);

/**
 * A new def, the data of the block `self`; null when `in` has failed.
 *
 * What GraphDef's edits always keep is checked: ids, at most one link per input, links
 * between nodes that are there, at most one `graph.output`, unique names. Cycles and
 * recursions aren't: the evaluator reports them. A value of a type this build doesn't
 * know is Unsupported, and a type name with the id of another name is InvalidArgument.
 */
GraphDef *readGraphDef(BlockReader &in, BlockId self);

} // namespace coredcc
