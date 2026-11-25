#pragma once

#include "coredcc/api/struct.h"
#include "coredcc/scene/object.h"

#include "litestl/util/map.h"
#include "litestl/util/vector.h"

#include <memory>

namespace coredcc {

/** What evaluateScene found for one object. */
struct ObjectEval {
  TransformState state;
  /** In the data's space; empty for none. From its data block's Spatial, or its own. */
  Box3d localBounds;
  /** A box around localBounds in the world; empty when that isn't finite. */
  Box3d bounds;
  EvalStatus status = EvalStatus::Ok;
  /** ObjectFlags. */
  uint32_t flags = 0;
  /** Its Live parent's place in evaluatedObjects, or -1; -1 on a cycle. */
  int32_t parent = -1;
  /** Its Live data block, or null. */
  BlockHandle data;
  /** Whether it is a member of the scene, not only a parent of one. */
  bool member = false;

  /** From the data's space to the world: what to draw and to find its geometry with. The
   * same as `state.world` for now; objects will have an offset of the data that children
   * don't have. */
  const mat4d &dataWorld() const
  {
    return state.world;
  }
  /** Bitwise. */
  bool same(const ObjectEval &b) const;
  static litestl::binding::types::Struct<ObjectEval> *defineBindings();
};

namespace detail {

/** A list of handles, and the place of each in it. */
class HandleIndex {
public:
  /** Appends a handle that isn't in the list. Returns its place. */
  int add(BlockHandle h)
  {
    const int at = int(handles_.size());
    while (places_.size() <= h.index) {
      places_.append(-1);
    }
    places_[h.index] = at;
    handles_.append(h);
    return at;
  }
  /** -1 if it isn't there. */
  int find(BlockHandle h) const
  {
    const int at = h.index < places_.size() ? places_[h.index] : -1;
    return at >= 0 && handles_[at] == h ? at : -1;
  }
  const litestl::util::Vector<BlockHandle> &handles() const
  {
    return handles_;
  }
  bool same(const HandleIndex &b) const;

private:
  litestl::util::Vector<BlockHandle> handles_;
  /* By the handle's index. */
  litestl::util::Vector<int> places_;
};

/** One evaluation's results. Two of them take turns: a reader sees the last complete one.
 */
struct EvaluatedStore {
  litestl::util::Vector<ObjectEval> objects;
  /** Shared by the two stores while the objects are the same. */
  std::shared_ptr<const HandleIndex> index;

  const ObjectEval *find(BlockHandle h) const
  {
    const int i = index ? index->find(h) : -1;
    return i >= 0 ? &objects[i] : nullptr;
  }
};

} // namespace detail

/* Results by path: the runtime owner `coredcc.evaluated`, for the Context's scene.
 *
 *     evaluated.objects["coredcc,0123456789abcdef0123456789abcdef"].world (formatBlockId)
 *
 * NotFound without a scene, before its first evaluation, and for objects not in it. */

inline constexpr const char *kEvaluatedOwner = "coredcc.evaluated";

/** Adds a member `evaluated` to a host's root struct. */
void addEvaluated(ApiStruct &root);

} // namespace coredcc
