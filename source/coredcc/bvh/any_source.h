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
  /** Asks for all bounds in the same way, and refits (GeometryBvh::refit). */
  Status refit(Runtime &rt, AnyBvhSource src);
  /** The same, but only asks for `count` primitives, from `first`. OutOfRange past the end of the
   * tree. */
  Status refit(Runtime &rt, AnyBvhSource src, int first, int count);
  /** GeometryBvh::onInvalidate. The source is asked once, or not at all if nothing has to be done. */
  Status onInvalidate(Runtime &rt, AnyBvhSource src, InvalidateKinds kinds);

  const GeometryBvh<BoxSource> &boxes() const
  {
    return boxes_;
  }

  /* For scripts. Six numbers for each primitive, as AnyBvhSource::bounds gives; InvalidArgument for
   * another length. */
  Status buildFrom(const litestl::util::Vector<double> &bounds);
  Status refitFrom(const litestl::util::Vector<double> &bounds);
  /** `bounds` has those of `primitives`, in that order. */
  Status refitSome(const litestl::util::Vector<int32_t> &primitives,
                   const litestl::util::Vector<double> &bounds);
  int32_t size() const
  {
    return int32_t(boxes_.bvh().size());
  }
  /** Bvh::growth: how much slower refits made it, from 1. */
  double growth() const
  {
    return boxes_.bvh().growth();
  }
  RayHit rayClosest(const double3 &origin, const double3 &dir, double tMin, double tMax) const;
  NearestHit nearest(const double3 &p, double radius) const;
  /* What a ray, a sphere or half-spaces (four numbers each: a normal, then `w`) meet, in ascending
   * order. None for input that isn't valid. */
  litestl::util::Vector<int32_t> rayAll(const double3 &origin,
                                        const double3 &dir,
                                        double tMin,
                                        double tMax) const;
  litestl::util::Vector<int32_t> sphereOverlap(const double3 &p, double radius) const;
  litestl::util::Vector<int32_t> region(const litestl::util::Vector<double> &planes) const;
  static litestl::binding::types::Struct<AnyBvh> *defineBindings();

 private:
  GeometryBvh<BoxSource> boxes_;
};

} // namespace coredcc
