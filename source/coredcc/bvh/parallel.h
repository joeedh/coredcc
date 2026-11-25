#pragma once

#include "litestl/util/index_range.h"
#include "litestl/util/task.h"

namespace coredcc::bvh {

/**
 * Whether blocking on litestl's task pool is safe here. It isn't on a browser's main
 * thread: workers can't start while it blocks.
 */
bool poolUsable();

/** Calls `cb(range)` over [0, n) in chunks of `chunk`, on the task pool when `parallel`
 * and it is usable. Serial on a pool worker. */
template <class Cb> void forChunks(int n, int chunk, bool parallel, Cb &&cb)
{
  using litestl::util::IndexRange;
  if (n <= 0) {
    return;
  }
  if (!parallel || n <= chunk || !poolUsable()) {
    cb(IndexRange(0, n));
    return;
  }
  litestl::task::parallel_for_chunks(
      IndexRange(0, n), chunk, [&](int, IndexRange r) { cb(r); });
}

} // namespace coredcc::bvh
