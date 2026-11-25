#pragma once

/** \file
 * Reading the library section into plain structures: nothing in the Library or the Runtime is
 * touched, so a damaged file changes nothing.
 */

#include "coredcc/lib/id_map.h"
#include "coredcc/lib/io/format.h"
#include "coredcc/lib/io/library_io.h"
#include "coredcc/lib/provider.h"

#include "litestl/util/function.h"

#include <memory>

namespace coredcc::detail::libio {

/** A NAME entry. */
struct FileName {
  bool isRaw = false;
  std::string_view text;
  /** The domains it was used in. */
  bool asClient = false;
  bool asType = false;
  /** Its ids: computed once, since a long name can be used often. */
  ClientId client;
  TypeId type;
};

/** A run of items in one of ParsedFile's flat lists. */
struct Range {
  uint32_t begin = 0;
  uint32_t count = 0;
};

struct ParsedSlot {
  TypeId ns;
  int64_t key = 0;
  RefCell cell;
};

struct ParsedAttachment {
  std::string_view type;
  TypeId typeId;
  uint32_t version = 0;
  /** As stored: 0 for no bytes, else an index plus 1. */
  uint32_t blob = 0;
  Range rows;
};

struct ParsedRecord {
  /** As stored, then (after planIds) the id the record gets. */
  BlockId id;
  std::string_view type;
  TypeId typeId;
  std::string_view name;
  std::string_view source;
  uint32_t flags = 0;
  StoredAs storedAs = StoredAs::Payload;
  uint32_t version = 0;
  uint32_t blob = 0;
  std::span<const uint8_t> extension;
  Range rows;
  Range slots;
  Range attachments;
};

struct ParsedFile {
  /** The decompressed chunks the views below point into; empty for a chunk stored as it is. */
  litestl::util::Vector<uint8_t> nameBytes;
  litestl::util::Vector<uint8_t> blockBytes;
  litestl::util::Vector<FileName> names;
  litestl::util::Vector<ParsedRecord> records;
  litestl::util::Vector<RefCell> rows;
  litestl::util::Vector<ParsedSlot> slots;
  litestl::util::Vector<ParsedAttachment> attachments;
  litestl::util::Vector<litestl::util::Vector<uint8_t>> blobs;
  /** Strong targets that aren't records, here or in the library: each becomes one. Canonical,
   * sorted, distinct. */
  litestl::util::Vector<BlockId> missing;
  Budget budget;
};

/**
 * Parses and checks `bytes`, which must outlive the result. Corruption is InvalidArgument,
 * limits are OutOfRange, a file that needs a newer build is Unsupported.
 */
Result<std::unique_ptr<ParsedFile>> parseLibrary(std::span<const uint8_t> bytes,
                                                 const LoadLimits &limits,
                                                 IoIssues &issues);

/** Gives an imported block an id, or a null one. May leave a sticky error. */
using RekeyFn =
    litestl::util::function<BlockId(const ParsedRecord &, BlockId id, const RekeyContext &)>;

/**
 * Like planIds, for an append to a library that has records: every record of the coredcc client
 * gets a fresh id, and the others' come from `rekey`. FailedPrecondition, listing the types, when
 * it gives none, or one that can't be used. `ids` maps the stored ids to the new ones.
 */
Status planAppend(ParsedFile &file,
                  Library &lib,
                  const LoadLimits &limits,
                  RekeyFn rekey,
                  IdMap &ids,
                  IoIssues &issues);

/**
 * Gives every record its id in `lib` (client aliases applied; `rekey` is asked for one whose id
 * another record then has), and every reference its target.
 * Checks the names against the Runtime's and the library's, then adds those they don't know to
 * lib.fileNames().
 */
Status planIds(ParsedFile &file,
               Library &lib,
               const LoadLimits &limits,
               RekeyFn rekey,
               IoIssues &issues);

} // namespace coredcc::detail::libio
