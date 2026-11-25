#pragma once

#include "coredcc/base/block_id.h"

#include <cstdint>

namespace coredcc {

/** A BlockId with a keyed hash, for maps over ids that came from files. BlockId's own hash
 * is easy to invert, so a file could give every block the same one. */
struct BlockIdKey {
  BlockId id;
  uint64_t hash = 0;

  uint64_t computeHash() const
  {
    return hash;
  }
  bool operator==(const BlockIdKey &b) const
  {
    return id == b.id;
  }
};

/** Makes BlockIdKeys with a secret seed (SipHash-1-3). Keys of different hashers don't mix. */
class BlockIdHasher {
 public:
  explicit BlockIdHasher(LocalId seed) : seed_(seed) {}
  BlockIdKey key(const BlockId &id) const;

 private:
  LocalId seed_;
};

} // namespace coredcc
