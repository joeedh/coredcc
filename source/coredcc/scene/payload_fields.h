#pragma once

/** \file
 * A block's payload as named entries, so that a newer build can add some without making the payload
 * unreadable. An entry that a build doesn't know, or whose value it can't use, is kept as it is and
 * written again. Of two with one name, the later counts: kept ones are written first.
 */

#include "coredcc/lib/block_io.h"

#include "litestl/util/vector.h"

#include <span>
#include <utility>

namespace coredcc::detail {

/** An entry with a value. */
BlockEntry valueField(const char *name, PropValue v);
/** An entry with references and no value. */
BlockEntry refField(const char *name, std::span<const RefCell> cells);

/** Whether the entry has this name and a value of this type, and nothing more. */
bool isValueField(const BlockEntry &e, const char *name, PropValue::Type type);
/** Whether the entry has this name and references, and nothing more. */
bool isRefField(const BlockEntry &e, const char *name);
/** Sets the cell's target from an entry with one reference. The cell keeps its own kind. */
bool takeRef(const BlockEntry &e, const char *name, RefCell &cell);

/** Reads the entries that one BlockWriter::entries() call wrote, gives each to `take`, and keeps
 * those for which it is false. */
template <class Take>
void readFields(BlockReader &in, litestl::util::Vector<BlockEntry> &kept, Take &&take)
{
  in.entries([&kept, &take](BlockEntry &e) {
    const bool taken = take(e);
    if (!taken) {
      kept.append(std::move(e));
    }
    return !taken;
  });
}

bool hasField(const litestl::util::Vector<BlockEntry> &entries, const char *name);
uint64_t fieldsMemSize(const litestl::util::Vector<BlockEntry> &entries);

} // namespace coredcc::detail
