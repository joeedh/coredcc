#include "coredcc/scene/pick_scene.h"

#include "coredcc/scene/capabilities.h"
#include "coredcc/scene/scene.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>

namespace coredcc {

using litestl::util::Vector;

namespace {

constexpr double kNoLimit = (std::numeric_limits<double>::infinity)();

/* An object whose bounds are near the pixel. */
struct Near {
  int place = 0;
  /* Nothing of it is nearer to the near plane. */
  double viewDistance = 0.0;
};

double leastViewDistance(const double4 &front, const Box3d &box)
{
  double sum = front[3], size = std::fabs(front[3]);
  for (int i = 0; i < 3; i++) {
    const double a = front[i] * box.min[i], b = front[i] * box.max[i];
    sum += std::fmin(a, b);
    size += std::fmax(std::fabs(a), std::fabs(b));
  }
  return sum - size * 1e-12;
}

/* What has been found so far, if only the first hit is wanted. */
struct Best {
  const PickOptions &options;
  detail::Occluder occluder;
  /* The frontmost hit that covers the cursor. Something else may be in front of it. */
  double covered = kNoLimit;
  bool thin = false;

  /* Behind this is nothing that comes first: a thin shape is behind the occluder there,
   * and what covers comes after the frontmost that does. Without an occluder there is no
   * such view distance. */
  double maxViewDistance() const
  {
    return options.maxHits == 1 ? std::fmax(occluder.behind, covered) : kNoLimit;
  }
  /* Surfaces that only are near come after a hit that covers, and after a thin shape,
   * unless that is hidden by something that isn't a hit. */
  bool nearSurfaces() const
  {
    const bool thinIsFirst = thin && (options.domains == ~0u || !options.occlusion);
    return options.maxHits != 1 || !(covered < kNoLimit || thinIsFirst);
  }
  void include(const PickSink &sink)
  {
    occluder.include(sink.occluder());
    for (const detail::PickCandidate &c : sink.kept()) {
      thin = thin || c.hit.thin;
      if (c.hit.covers()) {
        covered = std::fmin(covered, c.hit.viewDistance);
      }
    }
  }
};

} // namespace

Status pickScene(Context &ctx,
                 BlockHandle h,
                 const ViewMath &view,
                 double x,
                 double y,
                 const PickOptions &options,
                 const PickFilter &filter,
                 Vector<PickHit> &hits)
{
  hits.clear();
  detail::SceneQuery scope(ctx, h, "pickScene");
  CDCC_TRY(scope.status());
  if (!view.valid() || !bvh::finite(x) || !bvh::finite(y) || !options.valid()) {
    return Status::error(StatusCode::InvalidArgument,
                         "pickScene: the view, the pixel or the options aren't valid");
  }
  const Scene &scene = scope.scene();
  const PickCursor cursor(view, x, y, options);
  const double4 &plane = view.nearPlane();
  const double4 front = plane * (1.0 / std::hypot(plane[0], plane[1], plane[2]));
  Vector<Near> candidates;
  CDCC_TRY(scene.bvh().boxes().region(cursor.region().planes, [&](int place) {
    ObjectEval e;
    if (scene.evaluated(scene.bvh().object(place), e)) {
      Near n;
      n.place = place;
      n.viewDistance = leastViewDistance(front, e.bounds);
      candidates.append(n);
    }
    return true;
  }));
  std::sort(candidates.data(),
            candidates.data() + candidates.size(),
            [](const Near &a, const Near &b) {
              return a.viewDistance != b.viewDistance ? a.viewDistance < b.viewDistance
                                                      : a.place < b.place;
            });

  Best best{options, {}};
  Vector<detail::PickCandidate> found;
  for (const Near &n : candidates) {
    if (n.viewDistance > best.maxViewDistance()) {
      break; /* so are those after it */
    }
    const BlockHandle object = scene.bvh().object(n.place);
    ObjectEval e;
    const Spatial s = scope.spatial(object, filter);
    if (!s || !scene.evaluated(object, e)) {
      continue;
    }
    const PickQuery query(
        cursor, e.dataWorld(), best.maxViewDistance(), best.nearSurfaces());
    PickSink sink(query);
    if (s.hasPick()) {
      s.pick(query, sink);
      if (scope.thrown()) {
        continue;
      }
    } else {
      sink.box(e.localBounds);
    }
    for (detail::PickCandidate c : sink.kept()) {
      c.place = n.place;
      c.hit.object = object;
      found.append(std::move(c));
    }
    best.include(sink);
  }
  detail::rankPicks(options, best.occluder, found);
  hits.ensure_capacity(found.size());
  for (const detail::PickCandidate &c : found) {
    hits.append(c.hit);
  }
  return scope.foreign();
}

Status pickRegion(Context &ctx,
                  BlockHandle h,
                  const ViewRegion &region,
                  ElementDomain domain,
                  const PickFilter &filter,
                  Vector<RegionHit> &hits)
{
  hits.clear();
  detail::SceneQuery scope(ctx, h, "pickRegion");
  CDCC_TRY(scope.status());
  const int32_t d = int32_t(domain);
  if (d > 31) {
    return Status::error(StatusCode::InvalidArgument, "pickRegion: no such domain");
  }
  const Scene &scene = scope.scene();
  Vector<int> places;
  CDCC_TRY(scene.bvh().boxes().region(region.planes, [&](int place) {
    places.append(place);
    return true;
  }));
  std::sort(places.data(), places.data() + places.size());
  for (int place : places) {
    RegionHit hit;
    hit.object = scene.bvh().object(place);
    ObjectEval e;
    const Spatial s = scope.spatial(hit.object, filter);
    if (!s || !scene.evaluated(hit.object, e)) {
      continue;
    }
    bool take = d < 0 && !s.hasElementsInRegion();
    if (s.hasElementsInRegion()) {
      const RegionQuery query(region, e.dataWorld());
      uint32_t domains = s.domains();
      if (scope.thrown()) {
        continue;
      }
      domains &= d < 0 ? ~0u : 1u << d;
      /* One is enough if objects are asked for. */
      for (; domains != 0 && !take; domains &= domains - 1) {
        Vector<int32_t> elements =
            s.elementsInRegion(query, ElementDomain(std::countr_zero(domains)));
        if (scope.thrown()) {
          break;
        }
        take = elements.size() > 0;
        if (d >= 0) {
          hit.elements = std::move(elements);
        }
      }
    }
    if (take) {
      hits.append(std::move(hit));
    }
  }
  return scope.foreign();
}

} // namespace coredcc
