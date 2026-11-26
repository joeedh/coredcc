#include "coredcc/scene/scene_bvh.h"

#include <algorithm>

namespace coredcc {

using litestl::util::Vector;

Status SceneBvh::update(const detail::EvaluatedStore &results, const BvhOptions &options)
{
  BoxSource source;
  source.ensureCapacity(int(results.objects.size()));
  for (const ObjectEval &e : results.objects) {
    source.append(e.member && e.status != EvalStatus::Invalid ? e.bounds : Box3d());
  }
  const bool sameObjects = index_ && index_ == results.index;
  index_ = results.index;
  if (sameObjects && boxes_.refit(source).ok() && boxes_.bvh().growth() <= 2.0) {
    counters_.refits++;
    sinceGrowth_ = 0;
    return Status();
  }
  if (Status s = boxes_.build(std::move(source), options); !s.ok()) {
    index_ = nullptr; /* what is there is of other results */
    return s;
  }
  counters_.builds++;
  return Status();
}

Status SceneBvh::update(const detail::EvaluatedStore &results,
                        const BvhOptions &options,
                        std::span<const int32_t> changed)
{
  /* Box by box only while they are few: a full refit costs about as much as 1% of the
   * boxes refitted one by one. growth() is measured once 10% were. */
  const int n = int(results.objects.size());
  if (!index_ || index_ != results.index || int(changed.size()) * 100 > n) {
    return update(results, options);
  }
  BoxSource moved;
  moved.ensureCapacity(int(changed.size()));
  Vector<int> primitives;
  for (int32_t place : changed) {
    const ObjectEval &e = results.objects[place];
    moved.append(e.member && e.status != EvalStatus::Invalid ? e.bounds : Box3d());
    primitives.append(place);
  }
  if (!boxes_.refit(moved, {primitives.data(), primitives.size()}).ok()) {
    return update(results, options);
  }
  counters_.refits++;
  sinceGrowth_ += int(changed.size());
  if (sinceGrowth_ * 10 >= n) {
    sinceGrowth_ = 0;
    if (boxes_.bvh().growth() > 2.0) {
      return update(results, options);
    }
  }
  return Status();
}

Status SceneBvh::objectsInRegion(std::span<const BvhPlane> planes,
                                 Vector<BlockHandle> &out) const
{
  out.clear();
  Vector<int> places;
  CDCC_TRY(boxes_.region(planes, [&](int place) {
    places.append(place);
    return true;
  }));
  std::sort(places.data(), places.data() + places.size());
  for (int place : places) {
    out.append(object(place));
  }
  return Status();
}

namespace {

struct Found {
  int place;
  double key;
};

void inOrder(const SceneBvh &bvh, Vector<Found> &found, Vector<BlockHandle> &out)
{
  std::sort(
      found.data(), found.data() + found.size(), [](const Found &a, const Found &b) {
        return a.key != b.key ? a.key < b.key : a.place < b.place;
      });
  for (const Found &f : found) {
    out.append(bvh.object(f.place));
  }
}

} // namespace

Status SceneBvh::objectsOnRay(const Ray &ray, Vector<BlockHandle> &out) const
{
  out.clear();
  Vector<Found> found;
  CDCC_TRY(boxes_.rayAll(ray, [&](const RayHit &h) {
    found.append(Found{h.primitive, h.t});
    return true;
  }));
  inOrder(*this, found, out);
  return Status();
}

Status
SceneBvh::objectsNear(const double3 &point, double radius, Vector<BlockHandle> &out) const
{
  out.clear();
  Vector<Found> found;
  CDCC_TRY(boxes_.sphereOverlap(point, radius, [&](const NearestHit &h) {
    found.append(Found{h.primitive, h.distance});
    return true;
  }));
  inOrder(*this, found, out);
  return Status();
}

} // namespace coredcc
