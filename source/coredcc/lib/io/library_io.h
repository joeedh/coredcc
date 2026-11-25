#pragma once

/** \file
 * The library section of a file: every block that is kept, with its data, references, slots
 * and attachments. Saving is here; loading posts change notifications, so it is in
 * coredcc/api/library_io.h.
 *
 * Equal libraries give equal bytes, and a save that was loaded and saved again without
 * changes gives the same bytes.
 */

#include "coredcc/base/status.h"
#include "coredcc/lib/block.h"
#include "coredcc/lib/id_map.h"

#include "litestl/util/string.h"
#include "litestl/util/vector.h"

#include <cstdint>
#include <span>
#include <string_view>

namespace coredcc {

/** Something a save or a load went on without. */
struct IoIssue {
  /** The block it is about; null when it is about the file. */
  BlockId block;
  litestl::util::string blockName;
  Status why;
};

/** The issues of one save or load. */
struct IoIssues {
  static constexpr int kMaxKept = 1000;

  /** The first kMaxKept. */
  litestl::util::Vector<IoIssue> first;
  int64_t total = 0;

  void add(BlockId block, std::string_view blockName, Status why);
};

/** What a save did. */
struct SaveReport {
  IoIssues issues;
  /** Records written. */
  int64_t blocks = 0;
  /** Reachable Transient blocks: not written. */
  litestl::util::Vector<BlockHandle> transient;
  /** Written blocks that only Transient blocks lead to: orphans after a load, until the host
   * links them again. */
  litestl::util::Vector<BlockHandle> throughTransient;
  /** Detached blocks: written without their data. */
  litestl::util::Vector<BlockHandle> detached;
  /** Attachments whose type has no serialize: not written. */
  int64_t skippedAttachments = 0;
};

/**
 * Writes the blocks that are reachable from the save roots: Root and FakeUser blocks, blocks
 * with Host pins, Detached blocks, and placeholders that were records in a file. Frees
 * nothing. Deleted (InHistory) blocks aren't written, and OptStrong references to them, or to
 * ids without a record, are written null.
 *
 * - Fails with FailedPrecondition, naming the types, when a reachable block's type neither
 *   serializes nor says it is Transient. Fails with the provider's error when a `serialize`
 *   or `foreachRef` fails, naming the block. `report`, if given, is meaningful only for a
 *   save that worked.
 * - A MissingBlock placeholder that was never a record is written only if it has a name,
 *   flags, slots or attachments; a load makes it again from the references to it.
 * - Very large libraries need larger LoadLimits to load.
 */
Result<litestl::util::Vector<uint8_t>> saveLibrary(const Library &lib,
                                                   SaveReport *report = nullptr);

/** Bounds on what a load may take, so a hostile file can't exhaust memory. */
struct LoadLimits {
  /** Memory for the library's own structures and the decompressed payloads. What providers
   * allocate in `deserialize` isn't counted, except the entries they read through
   * BlockReader. */
  uint64_t maxBytes = sizeof(void *) == 4 ? uint64_t(128) << 20 : uint64_t(2) << 30;
  /** Records, counting the MissingBlock records the load makes. */
  uint32_t maxBlocks = 1u << 20;
};

struct LoadOptions {
  LoadLimits limits;
  /** What the new placeholders are said to come from, such as the file's name. A name the
   * file itself stored is kept instead. */
  litestl::util::string sourceName;
};

/** What a load did. */
struct LoadReport {
  IoIssues issues;
  /** The records that were made or changed: the file's, in its order, then MissingBlock
   * ones. */
  litestl::util::Vector<BlockHandle> blocks;
  /** How many of `blocks`, from its start, the file had. 0 from resolvePlaceholders. */
  int64_t records = 0;
  /** Records that stayed or became MissingType placeholders. */
  int64_t missingType = 0;
  /** MissingBlock records, those saved without data included. */
  int64_t missingBlock = 0;
};

/** What an append did. */
struct AppendResult {
  /** The new ids, by the ids the file has. */
  IdMap ids;
  LoadReport report;
};

namespace detail {
/* Without the notifications; see coredcc/api/library_io.h. */
Result<LoadReport> loadLibrary(Library &lib,
                               std::span<const uint8_t> bytes,
                               const LoadOptions &options);
Result<AppendResult> appendLibrary(Library &lib,
                                   std::span<const uint8_t> bytes,
                                   const LoadOptions &options);
Result<LoadReport> resolvePlaceholders(Library &lib);
} // namespace detail

} // namespace coredcc
