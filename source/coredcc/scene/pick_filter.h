#pragma once

#include "coredcc/base/runtime.h"
#include "coredcc/lib/library.h"
#include "coredcc/lib/pin.h"

#include <functional>
#include <memory>

namespace coredcc {

class Context;
class Scene;
struct Spatial;

/** Which objects picking and the other queries of a scene look at. */
struct PickFilter {
  /** Also those with ObjectFlags::Hidden. */
  bool hidden = false;
  /** Also those with ObjectFlags::Unselectable. */
  bool unselectable = false;
  /** If set, only those it accepts. Called while nothing can be changed, as in
   * evaluateScene. */
  std::function<bool(BlockHandle)> accept;
};

namespace detail {

/**
 * What the queries of a scene have in common. They are for its last evaluation;
 * FailedPrecondition before the first one. While one runs, setValue, commands and
 * evaluateScene are Busy, and so is another query. The first exception of a foreign
 * capability is kept.
 */
class SceneQuery {
public:
  SceneQuery(Context &ctx, BlockHandle scene, const char *what);
  ~SceneQuery();
  SceneQuery(const SceneQuery &) = delete;
  SceneQuery &operator=(const SceneQuery &) = delete;

  /** Why it can't run. */
  const Status &status() const
  {
    return status_;
  }
  const Scene &scene() const
  {
    return *scene_;
  }
  /** An object's Spatial as it is now: its data block's, or its own. Null if the filter
   * leaves it out, it has none, or something threw. */
  Spatial spatial(BlockHandle object, const PickFilter &filter);
  /** Whether a foreign call threw since the last time. */
  bool thrown();
  /** The first exception. */
  const Status &foreign() const
  {
    return foreign_;
  }

private:
  Status begin(Context &ctx, BlockHandle scene, const char *what);

  Status status_;
  Status foreign_;
  Runtime &rt_;
  Library *lib_ = nullptr;
  const Scene *scene_ = nullptr;
  std::unique_ptr<EvaluationScope> evaluating_;
  LibraryPin pin_;
};

} // namespace detail
} // namespace coredcc
