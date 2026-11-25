#pragma once

/** \file
 * Payloads: the bytes of one block's data, and the table of the references in it
 * (architecture §8a). Files, duplication and snapshots all go through these.
 *
 * A payload holds no BlockId, by convention: it holds indices into its ref table, which
 * the library stores beside it. Remapping, garbage collection and placeholders then work
 * on the table without the client's code.
 */

#include "coredcc/base/bytes.h"
#include "coredcc/lib/bag_refs.h"
#include "coredcc/lib/block.h"
#include "coredcc/props/bag.h"
#include "coredcc/props/value_io.h"

#include "litestl/util/string.h"
#include "litestl/util/vector.h"

#include <cstdint>
#include <span>

namespace coredcc {

class Library;

/** A named value in a payload. */
struct BlockEntry {
  litestl::util::string name;
  /** None for an entry of a type this build doesn't know. */
  PropValue value;
  /** The blocks the entry references. For a Pointer into a block, the first is the truth
   * for which block; the value's id is a copy. Report these from foreachRef. */
  litestl::util::Vector<RefCell, 1> refs;
  /** Not 0: a type tag this build doesn't know, with its bytes in `raw`. */
  uint8_t unknownTag = 0;
  litestl::util::Vector<uint8_t> raw;
};

namespace detail {
/** Writes a block id as an index into the entry's own refs. */
class EntryCodec : public ValueCodec {
 public:
  using ValueCodec::ValueCodec;
  void writeBlockId(BytesWriter &out, const BlockId &id) override;
  void readBlockId(BytesReader &in, BlockId &id) override;
  void chargeKey(BytesReader &in, uint32_t n) override;
  const BlockEntry *entry = nullptr;
  /** Null: no bound. */
  uint64_t *budget = nullptr;
};
struct PayloadAccess;
} // namespace detail

/** What BlockProvider::serialize and AttachmentProvider::serialize write to. */
class BlockWriter {
 public:
  explicit BlockWriter(const Library &lib);

  /** The payload's own bytes, for anything but references. */
  BytesWriter &bytes()
  {
    return bytes_;
  }
  /** Adds the reference to the ref table and writes its index. Every reference foreachRef
   * reports must be written, also deleted targets (what a save nulls is the save's job).
   * Writing one twice is fine. */
  void ref(const RefCell &cell);
  /** Writes one entry. A Pointer into a block needs its ref in `e.refs` (Internal
   * otherwise). Reads back with BlockReader::entries. */
  void entries(std::span<const BlockEntry> all);
  /** entries() of one. */
  void entry(const BlockEntry &e)
  {
    entries(std::span<const BlockEntry>(&e, 1));
  }
  /**
   * The bag's set entries, with `unread` (what BlockReader::bag gave) before them. Which block a
   * pointer is into comes from `refs`, which foreachRef reports. A pointer into a block that has
   * no cell there is Internal.
   */
  void bag(const PropertyBag &bag, const BagRefs &refs, std::span<const BlockEntry> unread = {});

  /** Opaque. */
  static const litestl::binding::types::Struct<BlockWriter> *defineBindings();

 private:
  friend struct detail::PayloadAccess;
  Result<BlockPayload> take(uint32_t version);
  void writeEntry(const BlockEntry &e);

  BytesWriter bytes_;
  litestl::util::Vector<RefCell> refs_;
  detail::EntryCodec codec_;
};

/** What BlockProvider::deserialize and AttachmentProvider::deserialize read from. */
class BlockReader {
 public:
  /** Reads `payload`, which must outlive the reader. */
  BlockReader(Library &lib, const BlockPayload &payload);

  /** The payload version it was written with, at most the registered one. */
  uint32_t version() const
  {
    return payload_.version;
  }
  BytesReader &bytes()
  {
    return bytes_;
  }
  /** Sets the cell's target. The cell keeps its own kind. */
  void ref(RefCell &cell);
  /** Appends the entries one BlockWriter::entries() call wrote. */
  void entries(litestl::util::Vector<BlockEntry> &out);
  /** Sets the entries into the bag, and the cells of its pointers into blocks. Those no def
   * takes, or whose value it would change (a number out of its range), land in `unread`: keep
   * them, report their refs from foreachRef, and write them again, so a file survives a build
   * that doesn't know them. */
  void bag(PropertyBag &bag, BagRefs &refs, litestl::util::Vector<BlockEntry> &unread);
  /** Counts memory that is about to be allocated, so that the load's LoadLimits bound it.
   * False, and a failure, when there isn't that much left. */
  bool charge(uint64_t bytes);
  /** The new data, on success: what the record's clientData becomes. Not null. */
  void setData(void *data)
  {
    data_ = data;
  }
  /** For ids and names. Other blocks may not have their data yet. */
  Library &library() const
  {
    return lib_;
  }

  /** Opaque. */
  static const litestl::binding::types::Struct<BlockReader> *defineBindings();

 private:
  friend struct detail::PayloadAccess;
  /** A row of the table by a stored index; null (and a failure) for a bad index. */
  const RefCell *row();
  bool readEntry(BlockEntry &e);
  uint32_t entryCount();

  Library &lib_;
  const BlockPayload &payload_;
  BytesReader bytes_;
  detail::EntryCodec codec_;
  void *data_ = nullptr;
};

/** What BlockProvider::duplicate gives its copy to. */
class BlockCopy {
 public:
  /** The new data, on success. Not null. */
  void setData(void *data)
  {
    data_ = data;
  }

  /** Opaque. */
  static const litestl::binding::types::Struct<BlockCopy> *defineBindings();

 private:
  friend struct detail::PayloadAccess;
  void *data_ = nullptr;
};

/**
 * Serializes a Live or InHistory block's data. Unsupported for a type without serialize
 * and deserialize. With the library's relations audit on, also checkPayloadRefs.
 */
Result<BlockPayload> writePayload(const Library &lib, BlockHandle h);

/**
 * Internal unless the payload's ref table and the provider's foreachRef name the same
 * blocks with the same strengths. A save without the relations audit reports this as an
 * issue.
 */
Status checkPayloadRefs(const Library &lib, BlockHandle h, const BlockPayload &payload);

/**
 * Gives a Live block that has no data yet (a null clientData) the data in `payload`.
 * Unsupported for a payload newer than the registered type.
 *
 * On an error the block still has no data. A provider that returns an error has freed what it
 * made. One that returns ok after the reader failed, or without reading to the end (the next
 * save would lose the rest), gets its data back through `destroy`.
 */
Status readPayload(Library &lib, BlockHandle h, const BlockPayload &payload);

} // namespace coredcc
