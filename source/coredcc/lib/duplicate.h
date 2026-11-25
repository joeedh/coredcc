#pragma once

#include "coredcc/base/status.h"
#include "coredcc/lib/block.h"
#include "coredcc/lib/id_map.h"

#include <span>

namespace coredcc {

/**
 * Copies blocks, under fresh ids of the same clients. A failure undoes the whole call.
 *
 * - Live blocks are copied with their type's `duplicate`, or else by writing and reading
 * their payload; a type with neither is Unsupported. MissingType placeholders are copied
 * as they are. Other blocks are FailedPrecondition. A repeated handle is copied once.
 * - References to blocks in the set (from data, slots and attachments) point at the
 * copies; others are shared.
 * - Slots, names and the FakeUser flag are copied. Attachments follow their type's
 *   DuplicatePolicy; opaque ones are copied. Root, pins and overrides aren't.
 *
 * Returns the copies' ids by the sources'.
 */
Result<IdMap> duplicateBlocks(Library &lib, std::span<const BlockHandle> blocks);

} // namespace coredcc
