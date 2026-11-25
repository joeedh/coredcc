#pragma once

#include "coredcc/base/iface/iface.h"
#include "coredcc/base/runtime.h"
#include "coredcc/base/status.h"
#include "coredcc/bvh/geometry_bvh.h"

#include "litestl/util/vector.h"

namespace coredcc {

/** Primitives described at run time, by native code or by a script. */
struct AnyBvhSource : IfaceRef {
  int size() const
  {
    return CDCC_CALL(size);
  }
  /** Six values for each primitive: its minimum x, y, z, then its maximum. One with a
   * minimum above a maximum, or a value that isn't finite, is left out of the tree. */
  litestl::util::Vector<double> bounds(int first, int count) const
  {
    return CDCC_CALL(bounds, first, count);
  }

  CDCC_INTERFACE(AnyBvhSource, "coredcc::AnyBvhSource")
  {
    BIND_STRUCT_METHOD(st, size, MARGS());
    BIND_STRUCT_METHOD(st, bounds, MARGS("first", "count"));
  }
};

/**
 * A BVH over the bounds of an AnyBvhSource. Its queries answer with boxes: candidates for
 * the caller to test.
 */
class AnyBvh {
 public:
  /**
   * Asks `src` for all bounds on the calling thread, in chunks, then builds. The source
   * isn't called or kept after that, and never from a worker.
   *
   * A foreign call that fails, a negative size, or bounds of the wrong length fail the
   * build, and the old tree is kept. FailedPrecondition while a sticky error is pending.
   */
  Status build(Runtime &rt, AnyBvhSource src, BvhOptions opts = {});

  const GeometryBvh<BoxSource> &boxes() const
  {
    return boxes_;
  }

 private:
  GeometryBvh<BoxSource> boxes_;
};

} // namespace coredcc
