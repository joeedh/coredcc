#pragma once

#include "coredcc/scene/object.h"

#include "litestl/util/map.h"
#include "litestl/util/vector.h"

namespace coredcc::detail {

/** The states of objects, each computed once. For as long as nothing changes. */
class StateCache {
 public:
  StateCache(const Library &lib, double time) : lib_(lib), time_(time) {}
  /** As computeState. */
  Result<TransformState> state(BlockHandle object);

 private:
  const Library &lib_;
  double time_;
  litestl::util::Map<BlockHandle, TransformState> states_;
};

/** Whether children can be elsewhere under `b` than under `a`, with the same matrix. */
bool inheritsOtherwise(const TransformState &a, const TransformState &b);

/**
 * The objects below some that a command keeps in place. They may move all the same: an object can
 * depend on its parent's rotation and scale, besides its matrix (FBX's inherit types).
 */
class KeptWorlds {
 public:
  /** Remembers where the objects below `changing` are, without those in it. */
  Status remember(const Library &lib,
                  const litestl::util::Map<BlockHandle, int> &changing,
                  double time);
  /** How many of them are elsewhere now. */
  int moved(const Library &lib) const;

 private:
  struct Place {
    BlockHandle object;
    mat4d world;
  };
  litestl::util::Vector<Place> places_;
  double time_ = 0.0;
};

} // namespace coredcc::detail
