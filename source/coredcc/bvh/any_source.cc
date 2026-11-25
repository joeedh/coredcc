#include "coredcc/bvh/any_source.h"

#include "litestl/binding/binding.h"
#include "litestl/util/alloc.h"

#include <algorithm>
#include <string>

namespace coredcc {

using litestl::util::Vector;

namespace {

constexpr int kChunk = 4096;

Box3d boxAt(const Vector<double> &values, int i)
{
  Box3d box;
  for (int a = 0; a < 3; a++) {
    box.min[a] = values[i * 6 + a];
    box.max[a] = values[i * 6 + 3 + a];
  }
  return box;
}

Status usable(Runtime &rt, const AnyBvhSource &src)
{
  if (!src) {
    return Status::error(StatusCode::InvalidArgument, "AnyBvh: a null source");
  }
  if (rt.hasStickyError()) {
    return Status::error(StatusCode::FailedPrecondition,
                         "AnyBvh: a sticky error is pending; take it first");
  }
  return Status();
}

Status
gatherRange(Runtime &rt, const AnyBvhSource &src, int begin, int end, BoxSource &out)
{
  for (int first = begin; first < end; first += kChunk) {
    const int count = end - first < kChunk ? end - first : kChunk;
    const Vector<double> values = src.bounds(first, count);
    if (rt.hasStickyError()) {
      return rt.takeStickyError();
    }
    if (values.size() != size_t(count) * 6) {
      return Status::error(StatusCode::InvalidArgument,
                           ("AnyBvh: bounds(" + std::to_string(first) + ", " +
                            std::to_string(count) + ") gave " +
                            std::to_string(values.size()) + " values instead of " +
                            std::to_string(count * 6))
                               .c_str());
    }
    for (int i = 0; i < count; i++) {
      out.append(boxAt(values, i));
    }
  }
  return Status();
}

Status gatherAll(Runtime &rt, const AnyBvhSource &src, BoxSource &out)
{
  CDCC_TRY(usable(rt, src));
  const int n = src.size();
  if (rt.hasStickyError()) {
    return rt.takeStickyError();
  }
  if (n < 0) {
    return Status::error(StatusCode::InvalidArgument,
                         "AnyBvh: a source of negative size");
  }
  return gatherRange(rt, src, 0, n, out);
}

Status boxesOf(const Vector<double> &bounds, BoxSource &out)
{
  if (bounds.size() % 6 != 0) {
    return Status::error(StatusCode::InvalidArgument,
                         "AnyBvh: six numbers for each primitive");
  }
  const int n = int(bounds.size() / 6);
  out.ensureCapacity(n);
  for (int i = 0; i < n; i++) {
    out.append(boxAt(bounds, i));
  }
  return Status();
}

Ray rayOf(const double3 &origin, const double3 &dir, double tMin, double tMax)
{
  Ray ray;
  ray.origin = origin;
  ray.dir = dir;
  ray.tMin = tMin;
  ray.tMax = tMax;
  return ray;
}

Vector<int32_t> sorted(Vector<int32_t> found)
{
  std::sort(found.data(), found.data() + found.size());
  return found;
}

} // namespace

Status AnyBvh::build(Runtime &rt, AnyBvhSource src, BvhOptions opts)
{
  BoxSource gathered;
  CDCC_TRY(gatherAll(rt, src, gathered));
  return boxes_.build(std::move(gathered), opts);
}

Status AnyBvh::refit(Runtime &rt, AnyBvhSource src)
{
  BoxSource gathered;
  CDCC_TRY(gatherAll(rt, src, gathered));
  return boxes_.refit(gathered);
}

Status AnyBvh::refit(Runtime &rt, AnyBvhSource src, int first, int count)
{
  CDCC_TRY(usable(rt, src));
  if (first < 0 || count < 0 || count > size() - first) {
    return Status::error(StatusCode::OutOfRange, "AnyBvh: refit of no such primitives");
  }
  BoxSource gathered;
  CDCC_TRY(gatherRange(rt, src, first, first + count, gathered));
  Vector<int> primitives;
  for (int i = 0; i < count; i++) {
    primitives.append(first + i);
  }
  return boxes_.refit(gathered, {primitives.data(), primitives.size()});
}

Status AnyBvh::onInvalidate(Runtime &rt, AnyBvhSource src, InvalidateKinds kinds)
{
  if (!hasFlag(kinds, InvalidateKinds::Topology) &&
      !hasFlag(kinds, InvalidateKinds::Positions))
  {
    return Status();
  }
  BoxSource gathered;
  CDCC_TRY(gatherAll(rt, src, gathered));
  return boxes_.onInvalidate(kinds, gathered);
}

/* ---- for scripts ---- */

Status AnyBvh::buildFrom(const Vector<double> &bounds)
{
  BoxSource boxes;
  CDCC_TRY(boxesOf(bounds, boxes));
  return boxes_.build(std::move(boxes));
}

Status AnyBvh::refitFrom(const Vector<double> &bounds)
{
  BoxSource boxes;
  CDCC_TRY(boxesOf(bounds, boxes));
  return boxes_.refit(boxes);
}

Status AnyBvh::refitSome(const Vector<int32_t> &primitives, const Vector<double> &bounds)
{
  BoxSource boxes;
  CDCC_TRY(boxesOf(bounds, boxes));
  return boxes_.refit(boxes, {primitives.data(), primitives.size()});
}

RayHit AnyBvh::rayClosest(const double3 &origin,
                          const double3 &dir,
                          double tMin,
                          double tMax) const
{
  return boxes_.rayClosest(rayOf(origin, dir, tMin, tMax));
}

NearestHit AnyBvh::nearest(const double3 &p, double radius) const
{
  return boxes_.nearest(p, radius);
}

Vector<int32_t>
AnyBvh::rayAll(const double3 &origin, const double3 &dir, double tMin, double tMax) const
{
  Vector<int32_t> found;
  const auto take = [&](const RayHit &h) {
    found.append(h.primitive);
    return true;
  };
  boxes_.rayAll(rayOf(origin, dir, tMin, tMax), take).ignore();
  return sorted(std::move(found));
}

Vector<int32_t> AnyBvh::sphereOverlap(const double3 &p, double radius) const
{
  Vector<int32_t> found;
  const auto take = [&](int i) {
    found.append(i);
    return true;
  };
  boxes_.sphereOverlap(p, radius, take).ignore();
  return sorted(std::move(found));
}

Vector<int32_t> AnyBvh::region(const Vector<double> &values) const
{
  Vector<int32_t> found;
  if (values.size() % 4 != 0) {
    return found;
  }
  Vector<BvhPlane> planes;
  for (int i = 0; i < int(values.size()); i += 4) {
    BvhPlane p;
    p.n = double3(values[i], values[i + 1], values[i + 2]);
    p.w = values[i + 3];
    planes.append(p);
  }
  const auto take = [&](int i) {
    found.append(i);
    return true;
  };
  boxes_.region({planes.data(), planes.size()}, take).ignore();
  return sorted(std::move(found));
}

litestl::binding::types::Struct<AnyBvh> *AnyBvh::defineBindings()
{
  static litestl::binding::types::Struct<AnyBvh> *st = nullptr;
  if (!st) {
    litestl::alloc::PermanentGuard guard;
    st = new litestl::binding::types::Struct<AnyBvh>("coredcc::AnyBvh", sizeof(AnyBvh));
    BIND_STRUCT_DEFAULT_CONSTRUCTOR(st);
    BIND_STRUCT_METHOD(st, buildFrom, MARGS("bounds"));
    BIND_STRUCT_METHOD(st, refitFrom, MARGS("bounds"));
    BIND_STRUCT_METHOD(st, refitSome, MARGS("primitives", "bounds"));
    BIND_STRUCT_METHOD(st, size, MARGS());
    BIND_STRUCT_METHOD(st, growth, MARGS());
    BIND_STRUCT_METHOD(st, rayClosest, MARGS("origin", "dir", "tMin", "tMax"));
    BIND_STRUCT_METHOD(st, nearest, MARGS("p", "radius"));
    BIND_STRUCT_METHOD(st, rayAll, MARGS("origin", "dir", "tMin", "tMax"));
    BIND_STRUCT_METHOD(st, sphereOverlap, MARGS("p", "radius"));
    BIND_STRUCT_METHOD(st, region, MARGS("planes"));
  }
  return st;
}

} // namespace coredcc
