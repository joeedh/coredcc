#include "coredcc/scene/state_cache.h"

#include <utility>

namespace coredcc::detail {

using litestl::util::Map;
using litestl::util::Vector;

BlockHandle liveParent(const Library &lib, const BlockId &parent)
{
  const BlockHandle p = parent.isNull() ? BlockHandle() : lib.find(parent);
  const Datablock *b = lib.get(p);
  return b && b->state() == BlockState::Live ? p : BlockHandle();
}

/* ---- states ---- */

Status StateCache::walk(BlockHandle object, bool &cycle, TransformState &out)
{
  struct Step {
    BlockHandle object;
    ObjectRead read;
  };
  Vector<Step> steps;
  Map<BlockHandle, int> seen;
  TransformState state;
  cycle = false;
  for (BlockHandle h = object; h;) {
    if (const TransformState *known = states_.lookup_ptr(h)) {
      state = *known;
      break;
    }
    if (!seen.add(h, 0)) {
      cycle = true;
      return Status();
    }
    if (!lib_.query<Transformable>(h)) {
      break; /* the origin */
    }
    Result<ObjectRead> r = readObjectAt(lib_, h, time_);
    if (!r.ok()) {
      return r.status();
    }
    const BlockHandle parent = liveParent(lib_, r->info.parent);
    steps.append(Step{h, std::move(*r)});
    h = parent;
  }
  for (int i = int(steps.size()) - 1; i >= 0; i--) {
    Result<TransformState> s = stateUnder(steps[i].read, state);
    if (!s.ok()) {
      return s.status();
    }
    state = *s;
    states_.add(steps[i].object, state);
  }
  out = state;
  return Status();
}

Result<TransformState> StateCache::stateUnder(const ObjectRead &read,
                                              const TransformState &parent) const
{
  TransformState state;
  Status error;
  evaluateObject(lib_.runtime(), read.type, read.ctx(), parent, state, error);
  return error.ok() ? Result<TransformState>(state)
                    : Result<TransformState>(std::move(error));
}

Result<TransformState> StateCache::state(BlockHandle object)
{
  if (!lib_.query<Transformable>(object)) {
    return computeState(lib_, object, time_); /* its error */
  }
  return parentState(object);
}

Result<TransformState> StateCache::parentState(BlockHandle parent)
{
  bool cycle = false;
  TransformState s;
  if (Status e = walk(parent, cycle, s); !e.ok()) {
    return e;
  }
  return cycle ? computeState(lib_, parent, time_) : Result<TransformState>(s);
}

Result<TransformState> StateCache::stateAbove(BlockHandle object, const ObjectRead &read)
{
  const BlockHandle parent = liveParent(lib_, read.info.parent);
  if (!parent) {
    return TransformState();
  }
  bool cycle = false;
  TransformState s;
  if (Status e = walk(parent, cycle, s); !e.ok()) {
    return e;
  }
  return cycle ? computeParentState(lib_, object, time_) : Result<TransformState>(s);
}

/* ---- ancestors ---- */

Result<BlockHandle> Ancestry::parentOf(BlockHandle object)
{
  Runtime &rt = lib_.runtime();
  ObjectInfo info;
  if (const Transformable t = lib_.query<Transformable>(object)) {
    t.read(info, scratch_);
  }
  if (rt.hasStickyError()) {
    return rt.takeStickyError();
  }
  return liveParent(lib_, info.parent);
}

Ancestry::Place Ancestry::under(BlockHandle object, BlockHandle parent) const
{
  const Place *above = parent ? places_.lookup_ptr(parent) : nullptr;
  Place p;
  p.parent = parent;
  const bool in = set_.lookup_ptr(object) != nullptr;
  p.within = in || (above && above->within);
  p.parentDepth = above ? above->parentDepth + 1 : 0;
  p.outside = !in ? object : above ? above->outside : BlockHandle();
  return p;
}

Result<Ancestry::Place> Ancestry::place(BlockHandle object)
{
  Vector<BlockHandle> path, parents;
  Map<BlockHandle, int> at;
  int cycle = -1;
  for (BlockHandle h = object; h && !places_.lookup_ptr(h);) {
    if (!at.add(h, int(path.size()))) {
      cycle = *at.lookup_ptr(h);
      break;
    }
    Result<BlockHandle> p = parentOf(h);
    if (!p.ok()) {
      return p.status();
    }
    path.append(h);
    parents.append(*p);
    h = *p;
  }
  int n = int(path.size());
  if (cycle >= 0) {
    bool within = false;
    for (int i = cycle; i < n; i++) {
      within = within || set_.lookup_ptr(path[i]);
    }
    for (int i = cycle; i < n; i++) {
      Place p;
      p.parent = parents[i];
      p.within = within;
      p.cycle = true;
      for (int k = 0; !p.outside && k < n - cycle; k++) {
        const BlockHandle h = path[cycle + (i - cycle + k) % (n - cycle)];
        p.outside = set_.lookup_ptr(h) ? BlockHandle() : h;
      }
      places_.add(path[i], p);
    }
    n = cycle;
  }
  for (int i = n - 1; i >= 0; i--) {
    places_.add(path[i], under(path[i], parents[i]));
  }
  return *places_.lookup_ptr(object);
}

Result<bool> Ancestry::below(BlockHandle object)
{
  Result<BlockHandle> parent = parentOf(object);
  if (!parent.ok() || !*parent) {
    return parent.ok() ? Result<bool>(false) : Result<bool>(parent.status());
  }
  Result<Place> above = place(*parent);
  if (!above.ok()) {
    return above.status();
  }
  if (!above->cycle) {
    return above->within;
  }
  /* It is below the cycle, or on it. */
  Result<Place> p = place(object);
  return p.ok() ? Result<bool>(!p->cycle && above->within) : Result<bool>(p.status());
}

} // namespace coredcc::detail
