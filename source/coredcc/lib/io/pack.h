#pragma once

/** \file
 * Compressed blobs, for files and undo snapshots: lz4 when that is smaller.
 */

#include "coredcc/base/status.h"

#include "litestl/util/vector.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace coredcc::detail::libio {

/** Blobs and chunks at least this large are compressed, when that makes them smaller. */
inline constexpr size_t kCompressFrom = 512;
/** Whether lz4 could make `raw` bytes out of `stored`. */
constexpr bool plausibleLz4(uint64_t raw, uint64_t stored)
{
  return stored <= UINT64_MAX / 256 && raw <= stored * 255 + 64;
}
/** The largest raw size of one blob or chunk (lz4's limit). */
inline constexpr uint64_t kMaxRaw = 0x7E000000;

/** What a load charges for a blob, on top of its bytes. */
inline constexpr uint64_t kBlobCost = 64;

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

/** What `unpack` takes from the budget: a Vector allocates half as much again. */
constexpr uint64_t unpackCost(uint64_t raw)
{
  return raw + raw / 2 + kBlobCost;
}

/** Bytes as stored: compressed or not. */
struct Stored {
  bool lz4 = false;
  uint64_t raw = 0;
  litestl::util::Vector<uint8_t> bytes;
};

/** Compresses when that is smaller. `fast` trades size for a much faster lz4. */
Stored pack(std::span<const uint8_t> bytes, bool fast = false);
/**
 * Decompresses or copies `stored` into `out`. Charges the budget, and checks that the
 * memory is there before litestl's Vector, which doesn't check, asks for it.
 * InvalidArgument for bytes that aren't `raw` bytes' worth, OutOfRange past a limit.
 */
Status unpack(std::span<const uint8_t> stored,
              bool lz4,
              uint64_t raw,
              Budget &budget,
              litestl::util::Vector<uint8_t> &out);
/** OutOfRange when `bytes` can't be allocated now. */
Status probeMemory(uint64_t bytes);

} // namespace coredcc::detail::libio
