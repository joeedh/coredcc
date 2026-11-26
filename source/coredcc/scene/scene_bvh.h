#pragma once

#include "coredcc/bvh/geometry_bvh.h"
#include "coredcc/draw/view.h"
#include "coredcc/scene/evaluated.h"

#include "litestl/util/vector.h"

#include <memory>

namespace coredcc {

namespace detail {
class SceneEvaluator;
}

/** A host's choices about scenes: `rt.ext<SceneSettings>()`. */
struct SceneSettings {
  static constexpr const char *kExtName = "coredcc.SceneSettings";

  /** Of every SceneBvh, from its next build on. Without `parallel`, evaluateScene doesn't
   * use litestl's task pool. */
  BvhOptions bvh;
  /** evaluateScene evaluates again only what changed: the objects whose blocks were
   * notified, what is below them, and the bounds that it can't keep. False evaluates
   * everything each time, to find a change that wasn't notified. */
  bool incremental = true;

  explicit SceneSettings(Runtime *)
  {
  }
};

/** A BVH over the world bounds that evaluateScene found, which keeps it up to date.
 * Members only, without those that have no bounds or whose status is Invalid. */
class SceneBvh {
public:
  /** Whether the scene has been evaluated (and the last build didn't fail). */
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

  /* Objects, by their bounds in the world: those that a region may contain a part of, in
   * the scene's order; that a ray hits, in the order it enters them; and whose bounds are
   * within a distance of a point, nearest first. InvalidArgument, and none, for a region,
   * a ray, a point or a radius that isn't valid. */
  Status objectsInRegion(std::span<const BvhPlane> planes,
                         litestl::util::Vector<BlockHandle> &out) const;
  Status objectsOnRay(const Ray &ray, litestl::util::Vector<BlockHandle> &out) const;
  Status objectsNear(const double3 &point,
                     double radius,
                     litestl::util::Vector<BlockHandle> &out) const;

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
  /** A refit if the objects are the same and none appears that had no bounds when it was
   * built, unless that makes the tree too loose. */
  Status update(const detail::EvaluatedStore &results, const BvhOptions &options);
  /** The same, when only the places `changed` may differ from the last call's results. */
  Status update(const detail::EvaluatedStore &results,
                const BvhOptions &options,
                std::span<const int32_t> changed);

  GeometryBvh<BoxSource> boxes_;
  std::shared_ptr<const detail::HandleIndex> index_;
  Counters counters_;
  /** Boxes refitted one by one since growth() was last measured. */
  int sinceGrowth_ = 0;
};

} // namespace coredcc
