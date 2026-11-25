#include "coredcc/scene/pick.h"

#include "coredcc/bvh/kernels.h"

#include "litestl/math/transform.h"
#include "litestl/util/map.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace coredcc {

using litestl::math::planeToLocal;
using litestl::math::transformPoint;
using litestl::math::transformVector;
using litestl::math::tryInverse;
using litestl::util::Vector;

namespace {

bool finite(const double3 &p)
{
  return bvh::finite(p[0]) && bvh::finite(p[1]) && bvh::finite(p[2]);
}

/* Scaled so that the normal has unit length. */
double4 normalizedPlane(const double4 &p)
{
  const double length = std::hypot(p[0], p[1], p[2]);
  return length > 0.0 ? p * (1.0 / length) : double4(0.0, 0.0, 0.0, 1.0);
}

double3 unitOr0(const double3 &v)
{
  const double length = std::hypot(v[0], v[1], v[2]);
  return length > 0.0 && bvh::finite(length) ? v * (1.0 / length) : double3();
}

} // namespace

/* ---- regions ---- */

RegionQuery::RegionQuery(const ViewRegion &region, const mat4d &world)
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
    planes_[i].w =
        local[3] + terms * 1e-12; /* as the view's: rounding excludes nothing */
  }
}

double3 RegionQuery::planeNormal(int32_t i) const
{
  return i >= 0 && i < planeCount() ? planes_[i].n : double3();
}

double RegionQuery::planeOffset(int32_t i) const
{
  return i >= 0 && i < planeCount() ? planes_[i].w : 0.0;
}

Vector<double> RegionQuery::planeValues() const
{
  Vector<double> out;
  for (const BvhPlane &p : planes_) {
    for (int i = 0; i < 3; i++) {
      out.append(p.n[i]);
    }
    out.append(p.w);
  }
  return out;
}

/* ---- the cursor ---- */

bool PickOptions::valid() const
{
  for (double v : {radius, shapeRadius, thinRadius, slope}) {
    if (!(v >= 0.0) || !bvh::finite(v)) {
      return false;
    }
  }
  return maxHits >= 0;
}

PickCursor::PickCursor(const ViewMath &view,
                       double x,
                       double y,
                       const PickOptions &options)
    : view_(view), options_(options), x_(x), y_(y)
{
  region_ = view.regionFromPixel(x, y, options.radius + options.shapeRadius);
  ray_ = view.rayFromPixel(x, y);
  front_ = normalizedPlane(view.nearPlane());
  back_ = normalizedPlane(view.farPlane());
  along_ = front_[0] * ray_.dir[0] + front_[1] * ray_.dir[1] + front_[2] * ray_.dir[2];
  const double room = detail::dotPoint(back_, ray_.origin);
  if (along_ > 0.0 && (back_[0] != 0.0 || back_[1] != 0.0 || back_[2] != 0.0)) {
    ray_.tMax = std::fmax(0.0, room / along_) * (1.0 + 1e-9);
  }
}

double PickCursor::reachAt(const double3 &p) const
{
  return std::fmax(options_.radius + options_.shapeRadius, 1.0) * view_.pixelSizeAt(p);
}

/* ---- the query ---- */

PickQuery::PickQuery(const PickCursor &cursor,
                     const mat4d &world,
                     double limitDepth,
                     bool nearSurfaces)
    : world_(world), worldDir_(cursor.ray_.dir), options_(cursor.options_),
      cursor_(&cursor), radius_(cursor.options_.radius * cursor.view_.view().dpiScale),
      surfaceRadius_(nearSurfaces ? radius_ : 0.0), limitDepth_(limitDepth),
      dpiScale_(cursor.view_.view().dpiScale)
{
  const double4 *pixels = cursor.view_.pixelRows();
  const double4 &front = cursor.front_, &back = cursor.back_;
  ViewRegion region = cursor.region_;
  const bool limited = bvh::finite(limitDepth);
  if (limited) {
    BvhPlane nearer;
    nearer.n = double3(-front[0], -front[1], -front[2]);
    nearer.w = limitDepth * (1.0 + 1e-9) - front[3] + std::fabs(front[3]) * 1e-12;
    const bool noFar = back[0] == 0.0 && back[1] == 0.0 && back[2] == 0.0;
    if (noFar || nearer.w < back[3]) {
      region.planes[ViewRegion::kFar] = nearer;
    }
  }
  region_ = RegionQuery(region, world);
  map_.x = planeToLocal(world, pixels[0] - pixels[2] * cursor.x_);
  map_.y = planeToLocal(world, pixels[1] - pixels[2] * cursor.y_);
  map_.w = planeToLocal(world, pixels[2]);
  map_.front = planeToLocal(world, front);
  map_.back = planeToLocal(world, back);

  /* The cofactors, and the determinant's sign: a mirrored object's normals still point
   * outward. */
  const double *m = world;
  const double3 c[3] = {
      double3(m[0], m[1], m[2]), double3(m[4], m[5], m[6]), double3(m[8], m[9], m[10])};
  const double sign = c[0].dot(c[1].cross(c[2])) < 0.0 ? -1.0 : 1.0;
  for (int i = 0; i < 3; i++) {
    normalColumns_[i] = c[(i + 1) % 3].cross(c[(i + 2) % 3]) * sign;
  }

  mat4d inverse;
  if (tryInverse(world, inverse)) {
    ray_ = cursor.ray_;
    ray_.origin = transformPoint(inverse, cursor.ray_.origin);
    ray_.dir = transformVector(inverse, cursor.ray_.dir);
    if (limited && cursor.along_ > 0.0) {
      ray_.tMax = std::fmin(ray_.tMax, limitDepth / cursor.along_ * (1.0 + 1e-9));
    }
    hasRay_ = ray_.valid();
  }
}

double PickQuery::pixelSizeAt(const double3 &p) const
{
  const double w = detail::dotPoint(map_.w, p);
  if (!(w > 0.0)) {
    return 0.0;
  }
  /* The gradients of the pixel's x and y. */
  const double px = detail::dotPoint(map_.x, p) / w, py = detail::dotPoint(map_.y, p) / w;
  double3 gx, gy;
  for (int i = 0; i < 3; i++) {
    gx[i] = (map_.x[i] - px * map_.w[i]) / w;
    gy[i] = (map_.y[i] - py * map_.w[i]) / w;
  }
  /* The singular values' product, and the larger one. */
  const double area = gx.cross(gy).length();
  const double a = gx.dot(gx), b = gx.dot(gy), c = gy.dot(gy);
  const double large = std::sqrt(0.5 * (a + c) + std::hypot(0.5 * (a - c), b));
  return area > 0.0 ? dpiScale_ * large / area
                    : (std::numeric_limits<double>::infinity)();
}

/* ---- ranking ---- */

namespace detail {

void rankPicks(const PickOptions &options,
               const Occluder &occluder,
               Vector<PickCandidate> &hits)
{
  /* What covers comes before the rest, since its distance is 0. */
  const auto group = [&](const PickCandidate &c) {
    return c.hit.thin && c.hit.depth <= occluder.behind ? 0 : 1;
  };
  std::stable_sort(hits.data(),
                   hits.data() + hits.size(),
                   [&](const PickCandidate &a, const PickCandidate &b) {
                     const int ga = group(a), gb = group(b);
                     if (ga != gb) {
                       return ga < gb;
                     }
                     if (a.hit.distance != b.hit.distance) {
                       return a.hit.distance < b.hit.distance;
                     }
                     if (ga == 0 && a.dimension != b.dimension) {
                       return a.dimension < b.dimension;
                     }
                     return a.hit.depth != b.hit.depth ? a.hit.depth < b.hit.depth
                                                       : a.place < b.place;
                   });
  litestl::util::Map<uint64_t, char> seen;
  int n = 0;
  for (int i = 0; i < int(hits.size()); i++) {
    if (options.maxHits > 0 && n == options.maxHits) {
      break;
    }
    const uint32_t domain = options.everyDomain ? uint32_t(hits[i].hit.domain) : 0;
    const uint64_t key = uint64_t(domain) << 32 | uint32_t(hits[i].place);
    if (seen.lookup_ptr(key)) {
      continue;
    }
    seen.add(key, 0);
    if (n != i) {
      hits[n] = hits[i];
    }
    n++;
  }
  hits.resize(size_t(n));
}

} // namespace detail

/* ---- the sink ---- */

void PickSink::take(const Found &f)
{
  PickHit hit;
  const double distance = std::fmax(0.0, f.at.distance - f.pixels);
  if (!(distance <= (f.thin ? query_.radius_ : query_.surfaceRadius_))) {
    return;
  }
  hit.domain = domain_;
  hit.element = element_;
  hit.instance = instance_;
  hit.distance = distance / query_.dpiScale_;
  hit.depth = std::fmax(0.0, detail::dotPoint(query_.map_.front, f.at.point));
  hit.point = transformPoint(query_.world_, f.at.point);
  if (!finite(hit.point) || hit.depth > query_.limitDepth_) {
    return;
  }
  hit.thin = f.thin;
  hit.u = f.u;
  hit.v = f.v;
  const double3 n = unitOr0(f.normal);
  const double3 *columns = query_.normalColumns_;
  hit.normal = unitOr0(columns[0] * n[0] + columns[1] * n[1] + columns[2] * n[2]);
  hit.backFacing = hit.normal.dot(query_.worldDir_) > 0.0;
  if (query_.options_.occlusion && !hit.thin && hit.covers()) {
    detail::Occluder o;
    o.depth = hit.depth;
    o.behind = hit.depth + query_.options_.slope * query_.cursor_->reachAt(hit.point);
    occluder_.include(o);
  }
  if (query_.wants(domain_)) {
    detail::PickCandidate c;
    c.hit = hit;
    c.dimension = f.dimension;
    keep(c);
  }
}

/* Of each domain: the best surface, and the thin hits that no other one beats wherever
 * the occluder is. */
void PickSink::keep(const detail::PickCandidate &c)
{
  const auto beats = [](const detail::PickCandidate &a, const detail::PickCandidate &b) {
    if (a.hit.distance != b.hit.distance) {
      return a.hit.distance < b.hit.distance &&
             (!a.hit.thin || a.hit.depth <= b.hit.depth);
    }
    if (!a.hit.thin || a.dimension == b.dimension) {
      return a.hit.depth <= b.hit.depth;
    }
    return a.dimension < b.dimension && a.hit.depth <= b.hit.depth;
  };
  int n = 0;
  for (int i = 0; i < int(kept_.size()); i++) {
    const detail::PickCandidate &k = kept_[i];
    const bool alike = k.hit.domain == c.hit.domain && k.hit.thin == c.hit.thin;
    if (alike && beats(k, c)) {
      return; /* and then c beats none of those before it */
    }
    if (alike && beats(c, k)) {
      continue;
    }
    if (n != i) {
      kept_[n] = k;
    }
    n++;
  }
  kept_.resize(size_t(n));
  kept_.append(c);
}

PickHit PickSink::best() const
{
  Vector<detail::PickCandidate> all = kept_;
  PickOptions one = query_.options_;
  one.maxHits = 1;
  detail::rankPicks(one, occluder_, all);
  return all.size() > 0 ? all[0].hit : PickHit();
}

void PickSink::corners(const double3 *p, int count, double pixels)
{
  for (int i = 0; i < count; i++) {
    if (!finite(p[i])) {
      return;
    }
  }
  Found f;
  if (!(pixels >= 0.0) || !bvh::finite(pixels) ||
      !detail::closestOnCorners(query_.map_, p, count, f.at))
  {
    return;
  }
  f.pixels = pixels * query_.dpiScale_;
  f.dimension = count - 1;
  f.thin = count < 3;
  if (count > 1) {
    const double3 ab = p[1] - p[0], at = f.at.point - p[0];
    const double length = ab.dot(ab);
    f.u = length > 0.0 ? at.dot(ab) / length : 0.0;
    if (count > 2) {
      const double3 ac = p[2] - p[0];
      f.normal = ab.cross(ac);
      const double nn = f.normal.dot(f.normal);
      if (nn > 0.0) {
        f.u = at.cross(ac).dot(f.normal) / nn;
        f.v = ab.cross(at).dot(f.normal) / nn;
      }
    }
  }
  take(f);
}

void PickSink::point(const double3 &p, double pixels)
{
  corners(&p, 1, pixels);
}

void PickSink::segment(const double3 &a, const double3 &b, double pixels)
{
  const double3 p[2] = {a, b};
  corners(p, 2, pixels);
}

void PickSink::triangle(const double3 &a, const double3 &b, const double3 &c)
{
  const double3 p[3] = {a, b, c};
  corners(p, 3, 0.0);
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
  Found f;
  const detail::ScreenMap &map = query_.map_;
  if (!detail::closestOnCapsule(map, a, b, radius, query_.radius_, f.at)) {
    return;
  }
  if (f.at.distance == 0.0 && query_.hasRay_) {
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
        f.at.point = p;
        f.normal = inside ? double3() : p - bvh::closestOnSegment(p, a, b);
      }
    }
  }
  const double3 ab = b - a;
  const double length = ab.dot(ab);
  f.u = length > 0.0 ? std::clamp((f.at.point - a).dot(ab) / length, 0.0, 1.0) : 0.0;
  f.thin = radius <= query_.options_.thinRadius * query_.pixelSizeAt(f.at.point);
  f.dimension = f.thin ? (length > 0.0 ? 1 : 0) : 2;
  take(f);
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
      /* Both windings are outward. */
      if (side != 0) {
        triangle(c[side], c[side | u], c[side | u | v]);
        triangle(c[side], c[side | u | v], c[side | v]);
      } else {
        triangle(c[side], c[side | u | v], c[side | u]);
        triangle(c[side], c[side | v], c[side | u | v]);
      }
    }
  }
}

/* ---- many at once ---- */

namespace {

Status sizes(const char *what,
             const Vector<int32_t> &elements,
             const Vector<double> &values,
             int per)
{
  if (values.size() != elements.size() * size_t(per)) {
    return Status::error(StatusCode::InvalidArgument,
                         (std::string(what) + ": " + std::to_string(values.size()) +
                          " numbers for " + std::to_string(elements.size()) + " elements")
                             .c_str());
  }
  return Status();
}

double3 pointAt(const Vector<double> &values, int i)
{
  return double3(values[i], values[i + 1], values[i + 2]);
}

} // namespace

Status PickSink::points(int32_t domain,
                        const Vector<int32_t> &elements,
                        const Vector<double> &values,
                        double pixels)
{
  CDCC_TRY(sizes("points", elements, values, 3));
  for (int i = 0; i < int(elements.size()); i++) {
    element(domain, elements[i]);
    point(pointAt(values, i * 3), pixels);
  }
  return Status();
}

Status PickSink::segments(int32_t domain,
                          const Vector<int32_t> &elements,
                          const Vector<double> &values,
                          double pixels)
{
  CDCC_TRY(sizes("segments", elements, values, 6));
  for (int i = 0; i < int(elements.size()); i++) {
    element(domain, elements[i]);
    segment(pointAt(values, i * 6), pointAt(values, i * 6 + 3), pixels);
  }
  return Status();
}

Status PickSink::triangles(int32_t domain,
                           const Vector<int32_t> &elements,
                           const Vector<double> &values)
{
  CDCC_TRY(sizes("triangles", elements, values, 9));
  for (int i = 0; i < int(elements.size()); i++) {
    element(domain, elements[i]);
    triangle(
        pointAt(values, i * 9), pointAt(values, i * 9 + 3), pointAt(values, i * 9 + 6));
  }
  return Status();
}

Status PickSink::capsules(int32_t domain,
                          const Vector<int32_t> &elements,
                          const Vector<double> &values)
{
  CDCC_TRY(sizes("capsules", elements, values, 7));
  for (int i = 0; i < int(elements.size()); i++) {
    element(domain, elements[i]);
    capsule(pointAt(values, i * 7), pointAt(values, i * 7 + 3), values[i * 7 + 6]);
  }
  return Status();
}

} // namespace coredcc
