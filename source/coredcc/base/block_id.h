#pragma once

#include "coredcc/base/id.h"

#include <compare>
#include <cstdint>

namespace coredcc {

/**
 * The client-local half of a BlockId: 128 random bits, made once when the block is
 * created and kept for its whole life, through undo, save and load (architecture §8a).
 */
struct LocalId {
  uint64_t hi = 0;
  uint64_t lo = 0;

  constexpr bool isNull() const
  {
    return hi == 0 && lo == 0;
  }
  constexpr auto operator<=>(const LocalId &) const = default;
  uint64_t computeHash() const
  {
    return hi * 0x9E3779B97F4A7C15ull ^ lo;
  }
  static const litestl::binding::types::Struct<LocalId> *defineBindings();
};

/** A datablock's persistent identity: the client that made it, plus a LocalId. */
struct BlockId {
  ClientId client;
  LocalId local;

  constexpr bool isNull() const
  {
    return local.isNull();
  }
  constexpr auto operator<=>(const BlockId &) const = default;
  uint64_t computeHash() const
  {
    return client.value * 0xC2B2AE3D27D4EB4Full ^ local.computeHash();
  }
  static const litestl::binding::types::Struct<BlockId> *defineBindings();
};

} // namespace coredcc
