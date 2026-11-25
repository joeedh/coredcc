#include "coredcc/scene/kept_worlds.h"

#include "coredcc/scene/solve.h"

#include <cmath>

namespace coredcc::detail {

using litestl::util::Vector;

namespace {

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

Status KeptWorlds::remember(const Library &lib, const HandleSet &changing, double time)
{
  time_ = time;
  Vector<BlockHandle> all;
  lib.forEach([&](BlockHandle h, const Datablock &b) {
    if (b.state() == BlockState::Live) {
      all.append(h);
    }
  });
  StateCache states(lib, time);
  Ancestry ancestry(lib, changing);
  for (BlockHandle h : all) {
    if (!lib.query<Transformable>(h) || changing.lookup_ptr(h)) {
      continue;
    }
    Result<Ancestry::Place> place = ancestry.place(h);
    if (!place.ok()) {
      return place.status();
    }
    if (place->within) {
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
