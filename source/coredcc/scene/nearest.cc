#include "coredcc/scene/nearest.h"

#include "coredcc/scene/capabilities.h"
#include "coredcc/scene/scene.h"

#include "litestl/math/transform.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace coredcc {

using litestl::math::transformPoint;
using litestl::math::tryInverse;
using litestl::util::Vector;

namespace {

constexpr double kInfinite = (std::numeric_limits<double>::infinity)();

bool finite(const double3 &p)
{
  return bvh::finite(p[0]) && bvh::finite(p[1]) && bvh::finite(p[2]);
}

/* A hit that takes part in the sorting of all objects' hits. */
struct Ranked {
  NearHit hit;
  int place = 0;
};

bool nearer(const NearHit &a, const NearHit &b)
{
  return a.distance != b.distance ? a.distance < b.distance : a.element < b.element;
}

} // namespace

NearQuery::NearQuery(const double3 &point,
                     double radius,
                     const NearestOptions &options,
                     const mat4d &world)
    : radius_(kInfinite), options_(options), world_(world), worldPoint_(point),
      worldRadius_(radius), stretch_(kInfinite)
{
  mat4d inverse;
  if (!tryInverse(world, inverse)) {
    return;
  }
  point_ = transformPoint(inverse, point);
  /* Its Frobenius norm: at least the largest factor that it stretches anything by. */
  const double *m = inverse;
  double sum = 0.0;
  for (int c = 0; c < 3; c++) {
    sum += m[c * 4] * m[c * 4] + m[c * 4 + 1] * m[c * 4 + 1] + m[c * 4 + 2] * m[c * 4 + 2];
  }
  stretch_ = std::sqrt(sum) * (1.0 + 1e-9);
  /* The inverse's rounding, and the point's. */
  double size = 0.0;
  for (int i = 0; i < 3; i++) {
    size = std::fmax(size, std::fabs(point[i]) + std::fabs(static_cast<const double *>(world)[12 + i]));
  }
  radius_ = (radius + size * 1e-12) * stretch_;
  if (!finite(point_) || !bvh::finite(stretch_)) {
    point_ = double3();
    radius_ = stretch_ = kInfinite;
  }
}

void NearSink::add(int32_t element, const double3 &position)
{
  NearHit hit;
  hit.domain = query_.options_.domain;
  hit.element = element;
  hit.point = transformPoint(query_.world_, position);
  hit.distance = double3(hit.point - query_.worldPoint_).length();
  if (!bvh::finite(hit.distance) || hit.distance > query_.worldRadius_) {
    return;
  }
  const int limit = query_.options_.maxCount;
  if (int(hits_.size()) == limit) {
    if (!nearer(hit, hits_[limit - 1])) {
      return;
    }
    hits_.pop_back();
  }
  hits_.append(hit);
  for (int i = int(hits_.size()) - 1; i > 0 && nearer(hits_[i], hits_[i - 1]); i--) {
    std::swap(hits_[i], hits_[i - 1]);
  }
}

Status NearSink::addAll(const Vector<int32_t> &elements, const Vector<double> &positions)
{
  if (positions.size() != elements.size() * 3) {
    return Status::error(StatusCode::InvalidArgument,
                         "NearSink: three numbers are needed for each element");
  }
  for (int i = 0; i < int(elements.size()); i++) {
    add(elements[i], double3(positions[i * 3], positions[i * 3 + 1], positions[i * 3 + 2]));
  }
  return Status();
}

double NearSink::radius() const
{
  const int limit = query_.options_.maxCount;
  if (int(hits_.size()) < limit || !bvh::finite(query_.stretch_)) {
    return query_.radius_;
  }
  /* The query's radius has room for rounding. */
  const double room = query_.radius_ - query_.worldRadius_ * query_.stretch_;
  const double less = hits_[limit - 1].distance * query_.stretch_ * (1.0 + 1e-12);
  return std::fmin(query_.radius_, bvh::finite(room) ? less + std::fmax(room, 0.0) : query_.radius_);
}

Status nearestInScene(Context &ctx,
                      BlockHandle h,
                      const double3 &point,
                      double radius,
                      const NearestOptions &options,
                      const PickFilter &filter,
                      Vector<NearHit> &hits)
{
  hits.clear();
  detail::SceneQuery scope(ctx, h, "nearestInScene");
  CDCC_TRY(scope.status());
  if (options.domain < 0 || options.domain > 31 || options.maxCount < 1) {
    return Status::error(StatusCode::InvalidArgument, "nearestInScene: the options aren't valid");
  }
  const Scene &scene = scope.scene();
  struct Candidate {
    int place;
    double distance;
  };
  Vector<Candidate> candidates;
  CDCC_TRY(scene.bvh().boxes().sphereOverlap(point, radius, [&](const NearestHit &n) {
    candidates.append(Candidate{n.primitive, n.distance});
    return true;
  }));
  std::sort(candidates.data(),
            candidates.data() + candidates.size(),
            [](const Candidate &a, const Candidate &b) {
              return a.distance != b.distance ? a.distance < b.distance : a.place < b.place;
            });
  Vector<Ranked> found;
  for (const Candidate &c : candidates) {
    if (c.distance > radius) {
      break; /* so are those after it */
    }
    const BlockHandle object = scene.bvh().object(c.place);
    ObjectEval e;
    const Spatial s = scope.spatial(object, filter);
    if (!s || !scene.evaluated(object, e)) {
      continue;
    }
    const uint32_t domains = s.domains();
    if (scope.thrown() || (domains >> options.domain & 1) == 0) {
      continue;
    }
    const NearQuery query(point, radius, options, e.dataWorld());
    NearSink sink(query);
    s.closestElements(query, sink);
    if (scope.thrown()) {
      continue;
    }
    for (const NearHit &n : sink.hits()) {
      Ranked r;
      r.hit = n;
      r.hit.object = object;
      r.place = c.place;
      found.append(r);
    }
    std::sort(found.data(), found.data() + found.size(), [](const Ranked &a, const Ranked &b) {
      return a.hit.distance != b.hit.distance ? a.hit.distance < b.hit.distance :
             a.place != b.place               ? a.place < b.place :
                                                a.hit.element < b.hit.element;
    });
    if (int(found.size()) >= options.maxCount) {
      found.resize(size_t(options.maxCount));
      radius = found[options.maxCount - 1].hit.distance; /* equal ones may still come first */
    }
  }
  for (const Ranked &r : found) {
    hits.append(r.hit);
  }
  return scope.foreign();
}

} // namespace coredcc
