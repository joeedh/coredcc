#pragma once

#include "coredcc/lib/block_io.h"

namespace coredcc::detail {

/** The library's side of the payload classes: providers have no use for it. */
struct PayloadAccess {
  /** Takes what was written. The writer's failure, if it has one. */
  static Result<BlockPayload> take(BlockWriter &out, uint32_t version)
  {
    return out.take(version);
  }
  /** Bounds what entries() and bag() allocate: they take from `*left` (null: no bound), and
   * fail with OutOfRange when it runs out. */
  static void setBudget(BlockReader &in, uint64_t *left)
  {
    in.codec_.budget = left;
  }
  static void *data(const BlockReader &in)
  {
    return in.data_;
  }
  static void *data(const BlockCopy &copy)
  {
    return copy.data_;
  }
};

} // namespace coredcc::detail
