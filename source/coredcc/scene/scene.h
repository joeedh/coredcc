#pragma once

#include "coredcc/props/unit_settings.h"
#include "coredcc/scene/evaluated.h"
#include "coredcc/scene/object.h"
#include "coredcc/scene/scene_bvh.h"

#include "litestl/util/map.h"
#include "litestl/util/vector.h"

#include <memory>

namespace coredcc {

/** coredcc's own scene: a block of type kSceneType. */
inline constexpr const char *kSceneType = "coredcc.Scene";

namespace detail {
class SceneEvaluator;
}

/**
 * The data of a `coredcc.Scene`: which objects are in it, in order, which one is active, which
 * are selected, and the time.
 *
 * - A block is a member once. Cells that were written behind the scene's back may name one twice: it
 *   counts once, and is listed where it is first.
 * - Objects are referenced with OptStrong cells. One that isn't Live (deleted, or its host
 *   took it away) stays in its place and isn't listed. A cell whose object has no record
 *   any more goes with the next call of `prune`, which the membership commands make.
 * - A scene is a GC root (newScene sets BlockFlags::Root).
 * - The objects of a scene, not their parents, are what is evaluated for it. A parent that isn't
 *   in the scene is evaluated too.
 */
class Scene {
 public:
  Scene();
  ~Scene();
  Scene(const Scene &) = delete;
  Scene &operator=(const Scene &) = delete;

  /** The scene of a block, or null unless it is a Live coredcc.Scene. */
  static Scene *of(const Library &lib, BlockHandle h);

  /* ---- members ---- */

  /** The Live objects, in order. Valid until something changes. */
  const litestl::util::Vector<BlockHandle> &objects(const Library &lib) const;
  /** Whether a block is a member, Live or not. */
  bool contains(const Library &lib, BlockHandle h) const
  {
    return indexOf(lib, h) >= 0;
  }
  /** Its place among all members, or -1. */
  int indexOf(const Library &lib, BlockHandle h) const;
  int indexOf(const Library &lib, const BlockId &id) const;
  /** Adds a block; at the end for `index` -1. Not checked to be Transformable: what isn't is at the
   * origin. AlreadyExists for a member. (One that cells written behind the scene's back name is found
   * out after `contains`, or the like, was asked since: it is in two cells then, which is the same.) */
  Status add(Library &lib, BlockHandle h, int index = -1);
  /** Removes a member: the place it had. NotFound otherwise. */
  Result<int> remove(Library &lib, const BlockId &id);
  /** Drops cells that are null or double, or whose block has no record. Returns how many. */
  int prune(Library &lib);
  /** All cells, for reading. */
  const litestl::util::Vector<RefCell> &cells() const
  {
    return cells_;
  }

  /* ---- active and selection ---- */

  /** The active object, or null unless it is Live and a member. */
  BlockHandle active(const Library &lib) const
  {
    const BlockHandle h = lib.resolveRef(active_.cell());
    return h && contains(lib, h) ? h : BlockHandle();
  }
  RefCell &activeCell()
  {
    return active_.cell();
  }
  /** The selected Live objects that are members, in the order they were selected. */
  litestl::util::Vector<BlockHandle> selected(const Library &lib) const;
  /** The cells, with those that aren't Live. */
  const litestl::util::Vector<RefCell> &selection() const
  {
    return selection_;
  }
  /** Replaces the selection. Duplicates are dropped. */
  void setSelection(Library &lib, const litestl::util::Vector<BlockId> &ids);

  /* ---- time ---- */

  double frame = 0.0;
  /** Display units, the frame rate and the frame at time 0. */
  UnitSettings units;
  /** In seconds. */
  double time() const
  {
    return units.timeOf(frame);
  }

  /* ---- evaluation ---- */

  /** What the last evaluateScene found for an object of this scene, or a parent of one. False,
   * with `out` unchanged, if it wasn't there. */
  bool evaluated(BlockHandle h, ObjectEval &out) const;
  /** Objects evaluated last time, in order: the members that were Live, then parents that
   * aren't members. */
  litestl::util::Vector<BlockHandle> evaluatedObjects() const;

  /** How often evaluateScene ran, and to the end (a foreign exception doesn't stop it); how often
   * it made its lookups again, and how often they had changed. */
  struct Counters {
    int evaluations = 0;
    int completed = 0;
    int revalidations = 0;
    int rebuilds = 0;
  };
  const Counters &counters() const;
  /** Over the bounds of the last evaluation. */
  const SceneBvh &bvh() const;

  /* ---- files: what this build doesn't know is kept, and written again ---- */

  void write(BlockWriter &out) const;
  /** Fails the reader for a payload that is wrong. */
  void readFrom(BlockReader &in);
  /** A copy for another block, not evaluated. */
  Scene *duplicate() const;
  uint64_t memSize() const;
  std::span<const BlockEntry> unreadFields() const
  {
    return std::span<const BlockEntry>(fields_.data(), size_t(fields_.size()));
  }

  /** Every reference, for the provider. */
  template <class Fn> void foreachCell(Fn &&fn)
  {
    for (RefCell &c : cells_) {
      fn(c);
    }
    fn(active_.cell());
    for (RefCell &c : selection_) {
      fn(c);
    }
    for (BlockEntry &e : fields_) {
      for (RefCell &c : e.refs) {
        fn(c);
      }
    }
  }

 private:
  friend class detail::SceneEvaluator;
  friend Status evaluateScene(Context &ctx, BlockHandle scene);
  void ensureIndex(const Library &lib) const;
  /** The same, as of the Library's epoch. */
  void ensureFreshIndex(const Library &lib) const;
  /** As indexOf, but for the epoch. */
  int lookup(const Library &lib, const BlockId &id) const;

  litestl::util::Vector<RefCell> cells_;
  litestl::util::Vector<RefCell> selection_;
  WeakRef<Object> active_;
  litestl::util::Vector<BlockEntry> fields_;

  /* Canonical id to its first place. Remapping, sweeps, a host's undo and whoever calls writeRef write
   * cells behind its back. So it is made again when the number of cells is another one, when a place
   * isn't of the id it is for, and when an id isn't there and the Library's epoch moved. */
  mutable litestl::util::Map<BlockId, int> index_;
  mutable bool indexValid_ = false;
  mutable bool indexDoubles_ = false;
  mutable int indexCount_ = 0;
  mutable uint64_t indexEpoch_ = 0;
  mutable uint64_t indexGeneration_ = 0;

  mutable litestl::util::Vector<BlockHandle> live_;
  mutable uint64_t liveEpoch_ = 0;
  mutable uint64_t liveGeneration_ = 0;

  std::unique_ptr<detail::SceneEvaluator> evaluator_;
};

/** A new, empty Scene. A GC root. */
Result<BlockHandle> newScene(Library &lib, std::string_view name);

/** The Context's scene, or null unless it is a Live coredcc.Scene. */
Scene *activeScene(const Context &ctx);

/**
 * Computes the states and bounds of a scene's objects, from scratch; Scene::evaluated gives
 * them. Hosts call it once a frame, before drawing.
 *
 * - Nothing in the document changes. setValue, commands, graph edits, computeState and this
 *   are Busy until it ends; capabilities that read results see the previous ones, complete.
 * - A transform that fails has a status (ObjectEval), and its parent's state stands in. An
 *   exception of a foreign capability or model is returned, the first one, with everything
 *   else computed.
 * - FailedPrecondition, with the last results kept, when a sticky error is pending, or when an
 *   object's `read` answers otherwise each time it is asked.
 * - After it, watchers of `evaluated` hear of it if any result differs.
 */
Status evaluateScene(Context &ctx, BlockHandle scene);
/** The Context's scene. */
Status evaluateScene(Context &ctx);

} // namespace coredcc
