#include "coredcc/scene/pick.h"

#include "coredcc/api/context.h"
#include "coredcc/bvh/kernels.h"
#include "coredcc/lib/pin.h"
#include "coredcc/scene/capabilities.h"
#include "coredcc/scene/scene.h"

#include "litestl/math/transform.h"

#include <algorithm>
#include <cmath>

namespace coredcc {

using litestl::math::planeToLocal;
using litestl::math::transformPoint;
using litestl::math::transformVector;
using litestl::math::tryInverse;
using litestl::util::Vector;

/* ---- the query ---- */

PickQuery::PickQuery(const ViewMath &view,
                     const ViewRegion &region,
                     double x,
                     double y,
                     double radius,
                     const mat4d &world)
    : world_(world), radius_(radius * view.view().dpiScale), dpiScale_(view.view().dpiScale)
{
  const double *m = world;
  for (int i = 0; i < ViewRegion::kPlanes; i++) {
    const BvhPlane &p = region.planes[i];
    const double4 local = planeToLocal(world, double4(p.n[0], p.n[1], p.n[2], p.w));
    double terms = 0.0;
    for (int k = 0; k < 3; k++) {
      terms += std::fabs(m[12 + k] * p.n[k]);
    }
    planes_[i].n = double3(local[0], local[1], local[2]);
    planes_[i].w = local[3] + terms * 1e-12; /* as the view's: rounding excludes nothing */
  }
  const double4 *pixels = view.pixelRows();
  const double4 &front = view.nearPlane();
  map_.x = planeToLocal(world, pixels[0] - pixels[2] * x);
  map_.y = planeToLocal(world, pixels[1] - pixels[2] * y);
  map_.w = planeToLocal(world, pixels[2]);
  map_.front = planeToLocal(world, front * (1.0 / std::hypot(front[0], front[1], front[2])));

  mat4d inverse;
  if (tryInverse(world, inverse)) {
    const Ray ray = view.rayFromPixel(x, y);
    ray_.origin = transformPoint(inverse, ray.origin);
    ray_.dir = transformVector(inverse, ray.dir);
    hasRay_ = ray_.valid();
  }
}

double3 PickQuery::planeNormal(int32_t i) const
{
  return i >= 0 && i < planeCount() ? planes_[i].n : double3();
}

double PickQuery::planeOffset(int32_t i) const
{
  return i >= 0 && i < planeCount() ? planes_[i].w : 0.0;
}

/* ---- the sink ---- */

namespace {

bool finite(const double3 &p)
{
  return bvh::finite(p[0]) && bvh::finite(p[1]) && bvh::finite(p[2]);
}

} // namespace

void PickSink::take(const detail::ScreenPoint &p)
{
  if (!(p.distance <= query_.radius_)) {
    return;
  }
  PickHit hit;
  hit.domain = domain_;
  hit.element = element_;
  hit.distance = p.distance / query_.dpiScale_;
  hit.depth = std::fmax(0.0, detail::dotPoint(query_.map_.front, p.point));
  hit.point = transformPoint(query_.world_, p.point);
  if (!finite(hit.point)) {
    return;
  }
  if (!any_ || hit.distance < best_.distance ||
      (hit.distance == best_.distance && hit.depth < best_.depth))
  {
    any_ = true;
    best_ = hit;
  }
}

void PickSink::corners(const double3 *p, int count)
{
  for (int i = 0; i < count; i++) {
    if (!finite(p[i])) {
      return;
    }
  }
  detail::ScreenPoint s;
  if (detail::closestOnCorners(query_.map_, p, count, s)) {
    take(s);
  }
}

void PickSink::point(const double3 &p)
{
  corners(&p, 1);
}

void PickSink::segment(const double3 &a, const double3 &b)
{
  const double3 p[2] = {a, b};
  corners(p, 2);
}

void PickSink::triangle(const double3 &a, const double3 &b, const double3 &c)
{
  const double3 p[3] = {a, b, c};
  corners(p, 3);
}

void PickSink::sphere(const double3 &center, double radius)
{
  capsule(center, center, radius);
}

void PickSink::capsule(const double3 &a, const double3 &b, double radius)
{
  if (!finite(a) || !finite(b) || !(radius >= 0.0) || !bvh::finite(radius)) {
    return;
  }
  detail::ScreenPoint s;
  const detail::ScreenMap &map = query_.map_;
  if (!detail::closestOnCapsule(map, a, b, radius, query_.radius_, s)) {
    return;
  }
  if (s.distance == 0.0 && query_.hasRay_) {
    const bvh::RayQuery q(query_.ray_);
    double t;
    bool inside;
    if (bvh::rayCapsule(q, a, b, radius, q.tMax, t, inside)) {
      /* A matrix that is nearly singular has an inverse that is nearly noise. */
      const double3 p = q.at(t);
      const double w = 0.01 * detail::dotPoint(map.w, p);
      if (std::fabs(detail::dotPoint(map.x, p)) <= w &&
          std::fabs(detail::dotPoint(map.y, p)) <= w)
      {
        s.point = p;
      }
    }
  }
  take(s);
}

void PickSink::box(const Box3d &box)
{
  if (box.empty()) {
    return;
  }
  double3 c[8];
  for (int i = 0; i < 8; i++) {
    for (int k = 0; k < 3; k++) {
      c[i][k] = (i >> k & 1) != 0 ? box.max[k] : box.min[k];
    }
  }
  for (int axis = 0; axis < 3; axis++) {
    const int u = 1 << (axis + 1) % 3, v = 1 << (axis + 2) % 3;
    for (int side : {0, 1 << axis}) {
      triangle(c[side], c[side | u], c[side | u | v]);
      triangle(c[side], c[side | u | v], c[side | v]);
    }
  }
}

/* ---- picking ---- */

namespace {

struct Candidate {
  /* In the scene's order. */
  int place = 0;
  PickHit hit;
};

bool before(const Candidate &a, const Candidate &b)
{
  return a.hit.distance != b.hit.distance ? a.hit.distance < b.hit.distance :
         a.hit.depth != b.hit.depth       ? a.hit.depth < b.hit.depth :
                                            a.place < b.place;
}

/* Whether a sticky error is there: kept, if it is the first. */
bool thrown(Runtime &rt, Status &foreign)
{
  if (!rt.hasStickyError()) {
    return false;
  }
  Status s = rt.takeStickyError();
  if (foreign.ok()) {
    foreign = std::move(s);
  }
  return true;
}

/* An object's Spatial as it is now. Null if it can't be picked, or something threw. */
Spatial spatialNow(const Library &lib, BlockHandle h, const PickFilter &filter, Status &foreign)
{
  Runtime &rt = lib.runtime();
  if (filter.accept && !filter.accept(h)) {
    return Spatial();
  }
  ObjectInfo info;
  const Transformable t = lib.query<Transformable>(h);
  if (thrown(rt, foreign)) {
    return Spatial();
  }
  if (t) {
    ChannelBlock channels;
    t.read(info, channels);
    if (thrown(rt, foreign)) {
      return Spatial();
    }
  }
  const ObjectFlags flags = ObjectFlags(info.flags);
  if ((hasFlag(flags, ObjectFlags::Hidden) && !filter.hidden) ||
      (hasFlag(flags, ObjectFlags::Unselectable) && !filter.unselectable))
  {
    return Spatial();
  }
  Spatial s = info.data.isNull() ? Spatial() : lib.query<Spatial>(lib.find(info.data));
  if (thrown(rt, foreign)) {
    return Spatial();
  }
  if (!s) {
    s = lib.query<Spatial>(h);
  }
  return thrown(rt, foreign) ? Spatial() : s;
}

} // namespace

Status pickScene(Context &ctx,
                 BlockHandle h,
                 const ViewMath &view,
                 double x,
                 double y,
                 double radius,
                 const PickFilter &filter,
                 Vector<PickHit> &hits)
{
  hits.clear();
  Library *lib = ctx.library();
  if (!lib) {
    return Status::error(StatusCode::FailedPrecondition, "the Context has no Library");
  }
  Runtime &rt = ctx.runtime();
  CDCC_TRY(rt.busyIfEvaluating("pickScene"));
  if (lib->busy()) {
    return Status::error(StatusCode::Busy, "pickScene: the library is busy");
  }
  if (rt.hasStickyError()) {
    return Status::error(StatusCode::FailedPrecondition, "pickScene: an error is pending");
  }
  if (!view.valid() || !bvh::finite(x) || !bvh::finite(y) || !(radius >= 0.0) ||
      !bvh::finite(radius))
  {
    return Status::error(StatusCode::InvalidArgument,
                         "pickScene: the view, the pixel or the radius isn't valid");
  }
  const Scene *scene = Scene::of(*lib, h);
  if (!scene) {
    return Status::error(StatusCode::NotFound, "pickScene: no such scene");
  }
  if (!scene->bvh().valid()) {
    return Status::error(StatusCode::FailedPrecondition,
                         "pickScene: the scene hasn't been evaluated");
  }

  /* As evaluateScene does, so that what a capability does can't take the scene away. */
  const EvaluationScope scope(rt);
  const LibraryPin pin(*lib, h);
  lib->beginNoCollect();
  Status foreign;
  Vector<Candidate> found;
  Vector<int> places;
  const ViewRegion region = view.regionFromPixel(x, y, radius);
  const Status searched = scene->bvh().boxes().region(region.planes, [&](int place) {
    places.append(place);
    return true;
  });
  for (int place : places) {
    const BlockHandle object = scene->bvh().object(place);
    ObjectEval e;
    const Spatial s = spatialNow(*lib, object, filter, foreign);
    if (!s || !scene->evaluated(object, e)) {
      continue;
    }
    const PickQuery query(view, region, x, y, radius, e.state.world);
    PickSink sink(query);
    if (s.hasPick()) {
      s.pick(query, sink);
      if (thrown(rt, foreign)) {
        continue;
      }
    }
    else {
      sink.box(e.localBounds);
    }
    if (sink.any()) {
      Candidate c;
      c.place = place;
      c.hit = sink.best();
      c.hit.object = object;
      found.append(c);
    }
  }
  lib->endNoCollect();
  std::sort(found.data(), found.data() + found.size(), before);
  hits.ensure_capacity(found.size());
  for (const Candidate &c : found) {
    hits.append(c.hit);
  }
  return searched.ok() ? foreign : searched;
}

} // namespace coredcc
