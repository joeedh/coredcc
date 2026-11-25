#pragma once

#include "coredcc/bvh/geometry_bvh.h"
#include "coredcc/scene/evaluated.h"

#include <memory>

namespace coredcc {

namespace detail {
class SceneEvaluator;
}

/** A BVH over the world bounds that evaluateScene found, which keeps it up to date. Members only,
 * without those that have no bounds or whose status is Invalid. */
class SceneBvh {
 public:
  /** Whether the scene has been evaluated. */
  bool valid() const
  {
    return index_ != nullptr;
  }
  const GeometryBvh<BoxSource> &boxes() const
  {
    return boxes_;
  }
  /** The object of a primitive of `boxes()`: its place in Scene::evaluatedObjects. */
  BlockHandle object(int primitive) const
  {
    return index_->handles()[primitive];
  }

  /** How often it was built, and how often refitted. */
  struct Counters {
    int builds = 0;
    int refits = 0;
  };
  const Counters &counters() const
  {
    return counters_;
  }

 private:
  friend class detail::SceneEvaluator;
  /** A refit if the objects, and those that have bounds, are the same, unless that makes the tree
   * twice as slow as when it was built. */
  Status update(const detail::EvaluatedStore &results);

  GeometryBvh<BoxSource> boxes_;
  std::shared_ptr<const detail::HandleIndex> index_;
  double builtCost_ = 0.0;
  Counters counters_;
};

} // namespace coredcc
