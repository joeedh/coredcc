#pragma once

/** \file
 * The library section's layout, version 1. Little-endian.
 *
 *   "CDCCLIB\n"  u32 version  u32 minReader  u64 totalBytes  u32 chunkCount
 *   chunk:       u32 tag  u32 flags  u64 raw  u64 stored  bytes
 *   u64 fnv1a64 of everything before it
 *
 *   NAME  u32 count, then: u8 form, and str text (form 0) or the u64 id (form 1)
 *   BLCK  u32 count, then records, each u32-length-prefixed:
 *           client, u64 hi, u64 lo, type, str name, u32 flags, u8 storage,
 *           u32 payloadVersion, source, rows,
 *           u32 count of slots (namespace, i64 key, row),
 *           u32 count of attachments (type, u32 version, blob, rows), blob
 *   DATA  u32 count, then blobs: u8 lz4, u64 raw, u64 stored, bytes
 *
 *   rows: u32 count, then rows; a row: u8 kind, client, u64 hi, u64 lo
 *   client, type, source, namespace: u32, 0 for none, else an index into NAME plus 1
 *   blob: u32, 0 for no bytes, else an index into DATA plus 1; each is used at most once
 *
 * Integer and tag values here are the file format: they never change meaning.
 */

#include "coredcc/base/bytes.h"
#include "coredcc/lib/block.h"

#include "litestl/util/vector.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace coredcc::detail::libio {

inline constexpr uint8_t kMagic[8] = {'C', 'D', 'C', 'C', 'L', 'I', 'B', '\n'};
/** What this build writes, and the newest it knows everything about. */
inline constexpr uint32_t kVersion = 1;
/** The oldest reader that can read what this build writes. */
inline constexpr uint32_t kMinReader = 1;
inline constexpr size_t kHeaderBytes = 28;
inline constexpr uint32_t kMaxChunks = 16;

constexpr uint32_t chunkTag(char a, char b, char c, char d)
{
  return uint32_t(uint8_t(a)) | uint32_t(uint8_t(b)) << 8 | uint32_t(uint8_t(c)) << 16 |
         uint32_t(uint8_t(d)) << 24;
}
inline constexpr uint32_t kTagName = chunkTag('N', 'A', 'M', 'E');
inline constexpr uint32_t kTagBlocks = chunkTag('B', 'L', 'C', 'K');
inline constexpr uint32_t kTagData = chunkTag('D', 'A', 'T', 'A');

inline constexpr uint32_t kChunkCritical = 1;
inline constexpr uint32_t kChunkLz4 = 2;

enum class NameForm : uint8_t { Text = 0, RawId = 1 };
/** How a record's data is stored. */
enum class StoredAs : uint8_t {
  /** A placeholder's payload too, so a build without the type writes what it read. */
  Payload = 0,
  /** No data: host-owned, or never there. */
  NoPayload = 1,
};

inline constexpr uint8_t kRowStrong = 0;
inline constexpr uint8_t kRowOptStrong = 1;
inline constexpr uint8_t kRowWeak = 2;

inline constexpr uint32_t kFlagRoot = 1;
inline constexpr uint32_t kFlagFakeUser = 2;

inline constexpr size_t kRowBytes = 21;
inline constexpr size_t kSlotBytes = 12 + kRowBytes;
inline constexpr size_t kAttachmentBytes = 16;
/** The least a record takes, its length included. */
inline constexpr size_t kRecordBytes = 61;
inline constexpr size_t kNameBytes = 5;
inline constexpr size_t kBlobBytes = 17;

/** Blobs and chunks at least this large are compressed, when that makes them smaller. */
inline constexpr size_t kCompressFrom = 512;
/** Whether lz4 could make `raw` bytes out of `stored`. */
constexpr bool plausibleLz4(uint64_t raw, uint64_t stored)
{
  return stored <= UINT64_MAX / 256 && raw <= stored * 255 + 64;
}
/** The largest raw size of one blob or chunk (lz4's limit). */
inline constexpr uint64_t kMaxRaw = 0x7E000000;

/** What a load charges per item against LoadLimits::maxBytes, on top of the item's bytes. */
inline constexpr uint64_t kRecordCost = sizeof(Datablock) + 640;
inline constexpr uint64_t kAttachmentCost = sizeof(OpaqueAttachment) * 2 + 128;
inline constexpr uint64_t kRowCost = sizeof(RefCell) * 7;
inline constexpr uint64_t kSlotCost = sizeof(RefCell) * 8;
inline constexpr uint64_t kNameCost = 128;
inline constexpr uint64_t kBlobCost = 64;

/** A Vector's items as pointers, which std's algorithms need. */
template <class T, int N> T *first(litestl::util::Vector<T, N> &v)
{
  return v.data();
}
template <class T, int N> T *last(litestl::util::Vector<T, N> &v)
{
  return v.data() + v.size();
}

/** The memory a load may still use. */
struct Budget {
  uint64_t left = 0;

  bool take(uint64_t n)
  {
    if (n > left) {
      return false;
    }
    left -= n;
    return true;
  }
};

/** Bytes as stored: compressed or not. */
struct Stored {
  bool lz4 = false;
  uint64_t raw = 0;
  litestl::util::Vector<uint8_t> bytes;
};

/** Compresses when that is smaller. `fast` trades size for a much faster lz4. */
Stored pack(std::span<const uint8_t> bytes, bool fast = false);
/**
 * Decompresses or copies `stored` into `out`. Charges the budget, and checks that the memory
 * is there before litestl's Vector, which doesn't check, asks for it. InvalidArgument for bytes
 * that aren't `raw` bytes' worth, OutOfRange past a limit.
 */
Status unpack(std::span<const uint8_t> stored,
              bool lz4,
              uint64_t raw,
              Budget &budget,
              litestl::util::Vector<uint8_t> &out);
/** OutOfRange when `bytes` can't be allocated now. */
Status probeMemory(uint64_t bytes);

} // namespace coredcc::detail::libio
