#pragma once

/* What commands ask about many objects at once. Internal. */

#include "coredcc/scene/object.h"

#include "litestl/util/map.h"
#include "litestl/util/vector.h"

namespace coredcc::detail {

using HandleSet = litestl::util::Map<BlockHandle, int>;

/** The Live parent that was read, or null. */
BlockHandle liveParent(const Library &lib, const BlockId &parent);

/** The states of objects and their parents, each computed once. For as long as nothing changes. */
class StateCache {
 public:
  StateCache(const Library &lib, double time) : lib_(lib), time_(time) {}
  /** As computeState. */
  Result<TransformState> state(BlockHandle object);
  /** Of a parent: the origin's, for one that isn't Transformable. */
  Result<TransformState> parentState(BlockHandle parent);
  /** Of the parent of an object, as computeParentState. */
  Result<TransformState> stateAbove(BlockHandle object, const ObjectRead &read);
  /** Of an object, under the state of its parent. */
  Result<TransformState> stateUnder(const ObjectRead &read, const TransformState &parent) const;

 private:
  /** `cycle` if one is above it: nothing is remembered then. */
  Status walk(BlockHandle object, bool &cycle, TransformState &out);

  const Library &lib_;
  double time_;
  litestl::util::Map<BlockHandle, TransformState> states_;
};

/** Where objects are relative to a set of them. Each is read once: for as long as nothing changes. */
class Ancestry {
 public:
  Ancestry(const Library &lib, const HandleSet &set) : lib_(lib), set_(set) {}

  struct Place {
    BlockHandle parent;
    /** It is in the set, or below one that is. */
    bool within = false;
    /** It is its own ancestor. */
    bool cycle = false;
    /** How many are above it. 0 on a cycle, which counts as one for those below it. */
    int depth = 0;
    /** It, if it isn't in the set, or else its nearest ancestor that isn't. */
    BlockHandle outside;
  };
  Result<Place> place(BlockHandle object);
  /** Whether an ancestor is in the set. False on a cycle. */
  Result<bool> below(BlockHandle object);

 private:
  Result<BlockHandle> parentOf(BlockHandle object);
  Place under(BlockHandle object, BlockHandle parent) const;

  const Library &lib_;
  const HandleSet &set_;
  litestl::util::Map<BlockHandle, Place> places_;
  ChannelBlock scratch_;
};

} // namespace coredcc::detail
