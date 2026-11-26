#pragma once

/** \file
 * Graphs that drive the channels of objects, and read the states of others, in the same
 * evaluation (docs/plans/2026-11-25-p5c-scene-evaluator.md).
 *
 * - A scene lists graph blocks (Scene::graphs, `core.scene.addGraph`). The graphs of all
 * the Library's Live scenes drive the document's objects.
 * - In those graphs, `scene.drive.<kind>` nodes drive a channel of an object,
 * `scene.object.state` nodes read an object's evaluated state, and `scene.time` nodes the
 * time.
 * - SceneDrivers is the Library's OverrideProvider: evaluateScene, computeState and the
 * commands see the driven values through it. It owns a generated graph with a transform
 * node for each object whose state a graph reads, and for their ancestors, around the
 * listed graphs.
 */

#include "coredcc/graph/instance.h"
#include "coredcc/scene/overrides.h"
#include "coredcc/scene/transform.h"

#include "litestl/util/string.h"
#include "litestl/util/vector.h"

#include <memory>

namespace coredcc {

/** Registers the payload type, the node types and the commands `core.scene.addGraph` and
 * `core.scene.removeGraph`. After the scene's and the graph's types and commands. */
Status registerDriveTypes(Runtime &rt);

/** Something in the listed graphs that can't work as it is. */
struct DriverDiagnostic {
  /** The listed graph, and the node in it (with its groups); a null node for the graph.
   */
  BlockId graph;
  NodePath node;
  litestl::util::string message;
};

/**
 * The Library's OverrideProvider for graphs. The host makes one per Library that should
 * be driven, and destroys it before the Library (the graph's destroy order).
 *
 * - Its answers are those of the document as it is, at the time asked for. Outside an
 * evaluation it evaluates the graphs first, if anything moved; inside one it answers for
 * what `prepare` computed, and fails with FailedPrecondition for another time.
 * - Driven objects whose values move are notified with the aspect kDrivenAspect.
 * - A driver that fails makes `overridesOf` fail with FailedPrecondition, so that
 * commands leave its objects out. What a graph reads downstream of it fails too.
 */
class SceneDrivers final : public OverrideProvider {
public:
  /** Becomes the Library's provider; fails as setOverrideProvider does. */
  static Result<std::unique_ptr<SceneDrivers>> make(Library &lib);
  ~SceneDrivers() override;
  SceneDrivers(const SceneDrivers &) = delete;
  SceneDrivers &operator=(const SceneDrivers &) = delete;

  /** Another provider, asked first (animation, say); the graphs win where both drive a
   * channel. Null for none. It must outlive this, or be set to null before it goes. */
  void setInner(OverrideProvider *inner);
  /** Brings the generated graph up to date, and evaluates it at `time`. What
   * `overridesOf` does outside an evaluation. Busy while one runs. */
  Status update(double time);
  /** After `update`: what is wrong in the listed graphs. */
  const litestl::util::Vector<DriverDiagnostic> &diagnostics() const;

  /** For tests and tools: the generated graph, its instance, the transform node of an
   * object (null if it has none) and how many objects have one. */
  const GraphDef &generated() const;
  const GraphInstance &instance() const;
  NodeId transformNode(const BlockId &object) const;
  /** What the object's transform node gave last; null if it has none, or it failed. */
  const TransformState *generatedState(const BlockId &object) const;
  int transformCount() const;
  /** How often the generated graph was made again. */
  int rebuilds() const;

  Status prepare(const Library &lib, double time, Driven &driven) override;
  Status overridesOf(const Library &lib,
                     BlockHandle object,
                     const TransformType &type,
                     double time,
                     ChannelOverrides &out) override;

  struct State;

private:
  explicit SceneDrivers(Library &lib);
  std::unique_ptr<State> s_;
};

} // namespace coredcc
