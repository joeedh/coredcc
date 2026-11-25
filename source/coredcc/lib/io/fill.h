#pragma once

/** \file
 * Filling records with data read from payloads, for loads and for types that were registered
 * later.
 */

#include "coredcc/lib/attachment.h"
#include "coredcc/lib/io/library_io.h"
#include "coredcc/lib/library.h"
#include "coredcc/lib/registry.h"

namespace coredcc::detail {

/**
 * Turns MissingType placeholders into Live blocks, and opaque attachments into real ones, in
 * place. What can't be read stays as it is, with an issue.
 *
 * Providers run under the sweep's Busy rules (`Scope`). References are followed up afterwards
 * by makeMissing().
 */
class LibraryFiller {
 public:
  /** `budget` is what BlockReader::entries may still allocate. */
  LibraryFiller(Library &lib, LoadReport &report, uint64_t budget, uint32_t maxBlocks)
      : lib_(lib), report_(report), budget_(budget), maxBlocks_(maxBlocks)
  {
  }

  /** While one lives, the library is busy and void mutations are deferred. */
  class Scope {
   public:
    explicit Scope(Library &lib);
    ~Scope();
    Scope(const Scope &) = delete;
    Scope &operator=(const Scope &) = delete;

   private:
    Library &lib_;
  };

  /* ---- before the Scope ---- */

  /** A record with the given flags and slots. */
  Result<BlockHandle> addRecord(BlockId id, PlaceholderInfo info, BlockFlags flags);
  void addSlot(BlockHandle h, TypeId ns, int64_t key, const RefCell &cell);
  void addOpaque(BlockHandle h, OpaqueAttachment attachment);
  void addAttachment(BlockHandle h, TypeId type, void *data);
  /** Copies the extension of the block's file record. */
  void setExtension(BlockHandle h, std::span<const uint8_t> bytes);
  void setSource(BlockHandle h, const litestl::util::string &source);

  /* ---- inside it ---- */

  /** Fills `h` if it is a MissingType placeholder. False if it stays one. */
  bool fill(BlockHandle h);
  /** Returns how many opaque attachments became real ones. */
  int fillAttachments(BlockHandle h);
  /** Notes `h`'s strong references to blocks without a record. */
  void noteMissing(BlockHandle h);

  /* ---- after it ---- */

  /** Makes a MissingBlock record for each block noteMissing found. */
  void makeMissing();

  /** Ok when `lib` can be filled: not busy, not evaluating, no sticky error pending. */
  static Status ready(const Library &lib, const char *what);
  /** Destroys records that a failed call made. */
  void destroy(const litestl::util::Vector<BlockHandle> &blocks);
  /** What the type's `rekey` makes of a block that isn't a record yet: null without a type, or a
   * rekey. */
  static BlockId rekey(Library &lib,
                       BlockId id,
                       const PlaceholderInfo &info,
                       const RekeyContext &context);
  /** readPayload without its checks: `h` is Live and has no data. */
  static Status read(Library &lib,
                     BlockHandle h,
                     const BlockTypeRegistry::Entry &type,
                     const BlockPayload &payload,
                     uint64_t *budget = nullptr);

 private:
  Status why(const Datablock &b) const;
  Status readAttachment(Datablock &b, const OpaqueAttachment &from);

  Library &lib_;
  LoadReport &report_;
  uint64_t budget_;
  uint32_t maxBlocks_;
  litestl::util::Vector<BlockId> missing_;
};

} // namespace coredcc::detail
