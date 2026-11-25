#include "coredcc/scene/kept_worlds.h"

#include "coredcc/scene/solve.h"

#include <cmath>

namespace coredcc::detail {

using litestl::util::Map;
using litestl::util::Vector;

namespace {

BlockHandle liveParent(const Library &lib, const ObjectRead &r)
{
  const BlockHandle p = r.info.parent.isNull() ? BlockHandle() : lib.find(r.info.parent);
  const Datablock *b = lib.get(p);
  return b && b->state() == BlockState::Live ? p : BlockHandle();
}

bool samePlace(const mat4d &a, const mat4d &b)
{
  const double3 ta = solve::translationOf(a), tb = solve::translationOf(b), d = ta - tb;
  const double size = solve::Linear::of(b).norm() + std::hypot(tb[0], tb[1], tb[2]);
  return solve::sameLinear(a, b) && std::hypot(d[0], d[1], d[2]) <= 1e-9 * size;
}

} // namespace

bool inheritsOtherwise(const TransformState &a, const TransformState &b)
{
  const double side = a.rotation.dot(b.rotation) < 0.0 ? -1.0 : 1.0;
  bool same = true;
  for (int i = 0; i < 4; i++) {
    same = same && std::fabs(a.rotation[i] - side * b.rotation[i]) <= 1e-9;
  }
  for (int i = 0; i < 3; i++) {
    const double size = std::fmax(std::fabs(a.localScale[i]), std::fabs(b.localScale[i]));
    same = same && std::fabs(a.localScale[i] - b.localScale[i]) <= 1e-9 * size;
  }
  return !same;
}

Result<TransformState> StateCache::state(BlockHandle object)
{
  struct Step {
    BlockHandle object;
    ObjectRead read;
  };
  Vector<Step> steps;
  Map<BlockHandle, int> seen;
  TransformState state;
  for (BlockHandle h = object; h;) {
    if (const TransformState *known = states_.lookup_ptr(h)) {
      state = *known;
      break;
    }
    if (!seen.add(h, 0)) {
      return computeState(lib_, object, time_); /* a cycle is above it */
    }
    if (h != object && !lib_.query<Transformable>(h)) {
      break; /* the origin */
    }
    Result<ObjectRead> r = readObject(lib_, h);
    if (!r.ok()) {
      return r.status();
    }
    const BlockHandle parent = liveParent(lib_, *r);
    steps.append(Step{h, std::move(*r)});
    h = parent;
  }
  for (int i = int(steps.size()) - 1; i >= 0; i--) {
    const ObjectRead &r = steps[i].read;
    const TransformState parent = state;
    Status error;
    evaluateObject(lib_.runtime(), r.type, r.channels, time_, parent, state, error);
    if (!error.ok()) {
      return error;
    }
    states_.add(steps[i].object, state);
  }
  return state;
}

Status KeptWorlds::remember(const Library &lib, const Map<BlockHandle, int> &changing, double time)
{
  time_ = time;
  Map<BlockHandle, BlockHandle> parents;
  Vector<BlockHandle> all;
  lib.forEach([&](BlockHandle h, const Datablock &b) {
    if (b.state() == BlockState::Live) {
      all.append(h);
    }
  });
  for (BlockHandle h : all) {
    if (!lib.query<Transformable>(h)) {
      continue;
    }
    Result<ObjectRead> r = readObject(lib, h);
    if (!r.ok()) {
      return r.status();
    }
    parents.add(h, liveParent(lib, *r));
  }
  StateCache states(lib, time);
  Map<BlockHandle, int> below; /* 1: changes, or is below one that does */
  for (BlockHandle h : all) {
    Vector<BlockHandle> path;
    Map<BlockHandle, int> seen;
    int is = 0;
    for (BlockHandle x = h; x && seen.add(x, 0);) {
      if (changing.lookup_ptr(x)) {
        is = 1;
        break;
      }
      if (const int *known = below.lookup_ptr(x)) {
        is = *known;
        break;
      }
      path.append(x);
      const BlockHandle *p = parents.lookup_ptr(x);
      x = p ? *p : BlockHandle();
    }
    for (BlockHandle x : path) {
      below.add(x, is);
    }
    if (is && !changing.lookup_ptr(h)) {
      Result<TransformState> s = states.state(h);
      if (!s.ok()) {
        return s.status();
      }
      places_.append(Place{h, s->world});
    }
  }
  return Status();
}

int KeptWorlds::moved(const Library &lib) const
{
  StateCache states(lib, time_);
  int n = 0;
  for (const Place &p : places_) {
    Result<TransformState> s = states.state(p.object);
    n += s.ok() && !samePlace(s->world, p.world) ? 1 : 0;
  }
  return n;
}

} // namespace coredcc::detail
