#pragma once

/** \file
 * What is near a point of the world.
 */

#include "coredcc/base/status.h"
#include "coredcc/bvh/box.h"
#include "coredcc/lib/library.h"
#include "coredcc/scene/element_domain.h"
#include "coredcc/scene/pick_filter.h"

#include "litestl/math/matrix.h"
#include "litestl/util/vector.h"

#include <cstdint>

namespace litestl::binding::types {
template <typename T> struct Struct;
}

namespace coredcc {

using litestl::math::mat4d;

struct NearestOptions {
  /** Objects that don't have it aren't asked. */
  ElementDomain domain = ElementDomain::Point;
  /** The most hits, of all objects together, at least 1. */
  int32_t maxCount = 1;
};

/** What a Spatial is asked for: the elements of a domain that are near a point. In its
 * object's space. */
class NearQuery {
public:
  NearQuery() = default;
  /** For an object with the matrix `world`, and a point and a radius in the world.
   * Without an inverse, the radius in the object's space is infinite. */
  NearQuery(const double3 &point,
            double radius,
            const NearestOptions &options,
            const mat4d &world);

  const double3 &point() const
  {
    return point_;
  }
  /** What is farther away than this isn't near enough in the world. What is within it may
   * be: the object's matrix may stretch it. May be infinite. */
  double radius() const
  {
    return radius_;
  }
  ElementDomain domain() const
  {
    return options_.domain;
  }
  /** How many are wanted. It is enough to give those that are nearest in the world. */
  int32_t maxCount() const
  {
    return options_.maxCount;
  }
  /** Object to world, for what can't be found exactly in an object that is stretched
   * unevenly. */
  const mat4d &world() const
  {
    return world_;
  }
  static litestl::binding::types::Struct<NearQuery> *defineBindings();

private:
  friend class NearSink;
  double3 point_;
  double radius_ = 0.0;
  NearestOptions options_;
  mat4d world_;
  double3 worldPoint_;
  double worldRadius_ = 0.0;
  /* How much longer than in the world a distance can be in the object. */
  double stretch_ = 0.0;
};

struct NearHit {
  BlockHandle object;
  ElementDomain domain = ElementDomain::None;
  int32_t element = -1;
  /** In the world. */
  double distance = 0.0;
  double3 point;
};

/** Takes elements, and keeps the nearest in the world. */
class NearSink {
public:
  /** `query` must outlive it. */
  explicit NearSink(const NearQuery &query) : query_(query)
  {
  }
  NearSink(const NearSink &) = delete;
  NearSink &operator=(const NearSink &) = delete;

  /** An element, and a point of it, in the object's space: the one that is nearest in the
   * world. The object's own nearest is the same unless the matrix stretches unevenly, and
   * isn't wrong then, but says that the element is farther away. Those that are too far
   * away, or not finite, are ignored. */
  void add(int32_t element, const double3 &position);
  /** Many at once: three numbers for each. InvalidArgument if the sizes don't match. */
  Status addAll(const litestl::util::Vector<int32_t> &elements,
                const litestl::util::Vector<double> &positions);
  /** As NearQuery::radius, but less once there are maxCount nearer ones. */
  double radius() const;

  /** Nearest first; without `object`. Equally near ones are in the order of their
   * elements. */
  const litestl::util::Vector<NearHit> &hits() const
  {
    return hits_;
  }
  static litestl::binding::types::Struct<NearSink> *defineBindings();

private:
  const NearQuery &query_;
  litestl::util::Vector<NearHit> hits_;
};

/**
 * The elements within a radius of a point, in the world: the nearest ones of all objects.
 * Equally near ones are in the scene's order, then in that of their elements. `radius`
 * may be infinite.
 *
 * The answer is for the last evaluateScene, as pickScene's, and the same rules hold. An
 * object's bounds must contain its elements. An object with a Spatial that throws gives
 * no hits.
 */
Status nearestInScene(Context &ctx,
                      BlockHandle scene,
                      const double3 &point,
                      double radius,
                      const NearestOptions &options,
                      const PickFilter &filter,
                      litestl::util::Vector<NearHit> &hits);

} // namespace coredcc
