/* AnyBvh over native sources, and over scripts through the fake foreign host. */
#include "bvh_fixture.h"

#include "../bind/fake_host.h"

#include "coredcc/base/iface/registry.h"
#include "coredcc/bind/foreign_command.h"
#include "coredcc/bvh/any_source.h"

#include "test_util.h"

#include <algorithm>
#include <new>
#include <thread>
#include <type_traits>

test_init;

using namespace bvhtest;
using litestl::util::Vector;
namespace lbt = litestl::binding::types;
namespace sc = coredcc::script;

namespace {

/* Spheres' boxes, with the calls it got. */
struct NativeSource {
  Vector<Box3d> boxes;
  int wrongLengthAt = -1; /* the chunk to answer one value short */
  int sizeAnswer = -1;    /* instead of the real one, unless -1 */
  mutable int calls = 0;
  mutable bool otherThread = false;
  std::thread::id home = std::this_thread::get_id();

  int size() const
  {
    return sizeAnswer == -1 ? int(boxes.size()) : sizeAnswer;
  }
  Vector<double> bounds(int first, int count) const
  {
    otherThread = otherThread || std::this_thread::get_id() != home;
    Vector<double> out;
    for (int i = first; i < first + count; i++) {
      for (int a = 0; a < 3; a++) {
        out.append(boxes[i].min[a]);
      }
      for (int a = 0; a < 3; a++) {
        out.append(boxes[i].max[a]);
      }
    }
    if (calls++ == wrongLengthAt) {
      out.pop_back();
    }
    return out;
  }
  static lbt::Struct<NativeSource> *defineBindings()
  {
    litestl::alloc::PermanentGuard guard;
    auto *st = new lbt::Struct<NativeSource>("test::NativeSource", sizeof(NativeSource));
    BIND_STRUCT_METHOD(st, size, MARGS());
    BIND_STRUCT_METHOD(st, bounds, MARGS("first", "count"));
    return st;
  }
};

void fill(NativeSource &src, int n)
{
  Rand r(5);
  const BoxSource boxes = bvhtest::boxes(r, Layout::Uniform, n);
  for (int i = 0; i < n; i++) {
    src.boxes.append(boxes.box(i));
  }
}

BoxSource direct(const NativeSource &src)
{
  BoxSource out;
  for (const Box3d &b : src.boxes) {
    out.append(b);
  }
  return out;
}

AnyBvhSource ref(Runtime &rt, NativeSource &src)
{
  return query<AnyBvhSource>(rt, TypeId::of("test.NativeSource"), &src);
}

void testNative()
{
  Runtime rt;
  test_assert((registerImpl<AnyBvhSource, NativeSource>(
                   rt, "test.NativeSource", IfaceFlags::ThreadAffine)
                   .ok()));
  NativeSource src;
  fill(src, 20000); /* enough for a parallel build */
  src.boxes[3].min[1] = std::numeric_limits<double>::quiet_NaN();
  src.boxes[4].max[0] = std::numeric_limits<double>::infinity();
  src.boxes[5] = Box3d();

  AnyBvh bvh;
  test_assert(bvh.build(rt, ref(rt, src)).ok());
  test_assert(src.calls == 5 && !src.otherThread);
  GeometryBvh<BoxSource> want;
  test_assert(want.build(direct(src)).ok());
  test_assert(sameTree(bvh.boxes().bvh(), want.bvh()));
  test_assert(bvh.boxes().bvh().skipped() == 3);

  /* Failures keep the old tree. */
  const auto unchanged = [&] { return sameTree(bvh.boxes().bvh(), want.bvh()); };
  src.calls = 0;
  src.wrongLengthAt = 2;
  test_assert(bvh.build(rt, ref(rt, src)).code() == StatusCode::InvalidArgument);
  test_assert(src.calls == 3 && unchanged());
  src.wrongLengthAt = -1;
  src.sizeAnswer = -7;
  test_assert(bvh.build(rt, ref(rt, src)).code() == StatusCode::InvalidArgument && unchanged());
  src.sizeAnswer = -1;
  test_assert(bvh.build(rt, AnyBvhSource()).code() == StatusCode::InvalidArgument && unchanged());
  BvhOptions bad;
  bad.leafSize = 0;
  test_assert(bvh.build(rt, ref(rt, src), bad).code() == StatusCode::InvalidArgument &&
              unchanged());
  rt.setStickyError(Status::error(StatusCode::Internal, "earlier"));
  src.calls = 0;
  test_assert(bvh.build(rt, ref(rt, src)).code() == StatusCode::FailedPrecondition);
  test_assert(src.calls == 0 && unchanged() && rt.takeStickyError().code() == StatusCode::Internal);

  src.boxes.clear();
  test_assert(bvh.build(rt, ref(rt, src)).ok() && bvh.boxes().bvh().size() == 0);
}

void testRefit()
{
  Runtime rt;
  test_assert((registerImpl<AnyBvhSource, NativeSource>(rt, "test.NativeSource").ok()));
  NativeSource src;
  fill(src, 9000);
  AnyBvh bvh;
  GeometryBvh<BoxSource> want;
  test_assert(bvh.build(rt, ref(rt, src)).ok() && want.build(direct(src)).ok());
  const auto same = [&] { return sameTree(bvh.boxes().bvh(), want.bvh()); };
  Rand r(6);
  const auto move = [&](int first, int count) {
    for (int i = first; i < first + count; i++) {
      const double by = r.range(-3, 3);
      src.boxes[i].min[2] += by;
      src.boxes[i].max[2] += by + 0.1;
    }
  };
  move(0, 9000);
  src.calls = 0;
  test_assert(bvh.refit(rt, ref(rt, src)).ok() && src.calls == 3);
  test_assert(want.refit(direct(src)).ok() && same() && bvh.growth() > 1.0);
  /* Some. */
  move(100, 5000);
  src.calls = 0;
  test_assert(bvh.refit(rt, ref(rt, src), 100, 5000).ok() && src.calls == 2);
  test_assert(want.refit(direct(src)).ok() && same());
  test_assert(bvh.refit(rt, ref(rt, src), 9000, 0).ok() && src.calls == 2 && same());
  for (const auto &[first, count] :
       {std::pair{-1, 2}, std::pair{0, -1}, std::pair{8999, 2}, std::pair{9001, 0}})
  {
    test_assert(bvh.refit(rt, ref(rt, src), first, count).code() == StatusCode::OutOfRange);
  }
  test_assert(src.calls == 2 && same());

  /* Failures change nothing. */
  move(0, 9000);
  src.wrongLengthAt = 3;
  test_assert(bvh.refit(rt, ref(rt, src)).code() == StatusCode::InvalidArgument);
  src.wrongLengthAt = 4;
  test_assert(bvh.refit(rt, ref(rt, src), 0, 100).code() == StatusCode::InvalidArgument);
  src.wrongLengthAt = -1;
  test_assert(bvh.refit(rt, AnyBvhSource()).code() == StatusCode::InvalidArgument);
  test_assert(bvh.refit(rt, AnyBvhSource(), 0, 1).code() == StatusCode::InvalidArgument);
  rt.setStickyError(Status::error(StatusCode::Internal, "earlier"));
  test_assert(bvh.refit(rt, ref(rt, src), 0, 1).code() == StatusCode::FailedPrecondition);
  test_assert(rt.takeStickyError().code() == StatusCode::Internal && same());

  /* What has to be done. */
  src.calls = 0;
  test_assert(bvh.onInvalidate(rt, ref(rt, src), InvalidateKinds::Selection).ok() && same());
  test_assert(src.calls == 0);
  test_assert(bvh.onInvalidate(rt, ref(rt, src), InvalidateKinds::Positions).ok());
  test_assert(src.calls == 3 && want.refit(direct(src)).ok() && same());
  test_assert(bvh.onInvalidate(rt, ref(rt, src), InvalidateKinds::Topology).ok());
  test_assert(src.calls == 6 && want.build(direct(src)).ok() && same());
  src.boxes.pop_back();
  test_assert(bvh.refit(rt, ref(rt, src)).code() == StatusCode::FailedPrecondition && same());
  src.calls = 0;
  test_assert(bvh.onInvalidate(rt, ref(rt, src), InvalidateKinds::Positions).ok());
  test_assert(src.calls == 3 && want.build(direct(src)).ok() && same());
}

/* What scripts call. */
void testNumbers()
{
  NativeSource src;
  fill(src, 500);
  AnyBvh bvh;
  GeometryBvh<BoxSource> want;
  const auto same = [&] { return sameTree(bvh.boxes().bvh(), want.bvh()); };
  Vector<double> all = src.bounds(0, 500);
  test_assert(bvh.buildFrom(all).ok() && want.build(direct(src)).ok() && same());
  test_assert(bvh.size() == 500 && bvh.growth() == 1.0);
  Rand r(7);
  for (int i = 0; i < 500; i++) {
    src.boxes[i].max[0] += r.range(0.0, 1.0);
  }
  test_assert(bvh.refitFrom(src.bounds(0, 500)).ok() && want.refit(direct(src)).ok() && same());
  Vector<int32_t> some;
  Vector<double> values;
  for (int i = 490; i > 0; i -= 7) {
    src.boxes[i].min[1] -= 2.0;
    some.append(i);
    for (double v : src.bounds(i, 1)) {
      values.append(v);
    }
  }
  test_assert(bvh.refitSome(some, values).ok() && want.refit(direct(src)).ok() && same());
  /* Wrong lengths. */
  values.pop_back();
  all.pop_back();
  test_assert(bvh.refitSome(some, values).code() == StatusCode::InvalidArgument && same());
  test_assert(bvh.refitFrom(all).code() == StatusCode::InvalidArgument && same());
  test_assert(bvh.buildFrom(all).code() == StatusCode::InvalidArgument && same());

  int rays = 0, found = 0;
  for (int k = 0; k < 300; k++) {
    Ray ray;
    ray.origin = r.vec(12.0);
    ray.dir = r.vec(1.0);
    ray.tMin = r.range(0.0, 2.0);
    ray.tMax = r.range(2.0, 40.0);
    const RayHit a = bvh.rayClosest(ray.origin, ray.dir, ray.tMin, ray.tMax);
    const RayHit b = want.rayClosest(ray);
    test_assert(a.primitive == b.primitive && a.t == b.t);
    const NearestHit n = bvh.nearest(ray.origin, 3.0), m = want.nearest(ray.origin, 3.0);
    test_assert(n.primitive == m.primitive && n.distance == m.distance);
    std::vector<int> hits[3];
    const auto into = [&](int i) {
      return [&hits, i](const auto &h) {
        if constexpr (std::is_same_v<std::decay_t<decltype(h)>, int>) {
          hits[i].push_back(h);
        } else {
          hits[i].push_back(h.primitive);
        }
        return true;
      };
    };
    BvhPlane planes[2];
    Vector<double> numbers;
    for (BvhPlane &p : planes) {
      p.n = r.vec(1.0);
      p.w = r.range(-5.0, 5.0);
      for (int i = 0; i < 4; i++) {
        numbers.append(i < 3 ? p.n[i] : p.w);
      }
    }
    test_assert(want.rayAll(ray, into(0)).ok());
    test_assert(want.sphereOverlap(ray.origin, 3.0, into(1)).ok());
    test_assert(want.region(planes, into(2)).ok());
    const Vector<int32_t> got[3] = {bvh.rayAll(ray.origin, ray.dir, ray.tMin, ray.tMax),
                                    bvh.sphereOverlap(ray.origin, 3.0),
                                    bvh.region(numbers)};
    for (int i = 0; i < 3; i++) {
      std::sort(hits[i].begin(), hits[i].end());
      test_assert(got[i].size() == hits[i].size() &&
                  std::equal(hits[i].begin(), hits[i].end(), got[i].data()));
      found += int(hits[i].size());
    }
    rays += bool(a);
    numbers.pop_back();
    test_assert(bvh.region(numbers).size() == 0);
  }
  test_assert(rays > 20 && found > 10000);
  const double nan = std::numeric_limits<double>::quiet_NaN();
  test_assert(bvh.sphereOverlap(double3(), nan).size() == 0);
  test_assert(bvh.rayAll(double3(nan, 0, 0), double3(0, 0, 1), 0, 1).size() == 0);
  test_assert(AnyBvh::defineBindings() == AnyBvh::defineBindings());
  test_assert(RayHit::defineBindings() == RayHit::defineBindings());
  test_assert(NearestHit::defineBindings() == NearestHit::defineBindings());
}

void testForeign()
{
  fakehost::Host host;
  {
    Runtime rt;
    test_assert(sc::foreignImplementable(detail::ifaceDesc<AnyBvhSource>()).ok());
    sc::ForeignBindings &fb = rt.ext<sc::ForeignBindings>();
    const char *slots[] = {"size", "bounds"};
    test_assert(
        (registerForeign<AnyBvhSource>(rt, "js.source", fb.dispatcher(), nullptr, slots).ok()));

    NativeSource data;
    fill(data, 9000);
    int failAt = -1, calls = 0;
    bool failSize = false, otherThread = false;
    const std::thread::id home = std::this_thread::get_id();
    fakehost::Obj obj;
    obj.methods["size"] = [&](void **, void *ret, litestl::util::string *err) {
      if (failSize) {
        *err = "size: Error: no size";
        return false;
      }
      *static_cast<int *>(ret) = data.size();
      return true;
    };
    obj.methods["bounds"] = [&](void **args, void *ret, litestl::util::string *err) {
      otherThread = otherThread || std::this_thread::get_id() != home;
      if (calls++ == failAt) {
        *err = "bounds: Error: boom";
        return false;
      }
      new (ret) Vector<double>(
          data.bounds(*static_cast<int *>(args[0]), *static_cast<int *>(args[1])));
      return true;
    };
    const fakehost::lf::Handle h = host.add(std::move(obj));
    const AnyBvhSource src =
        query<AnyBvhSource>(rt, TypeId::of("js.source"), reinterpret_cast<void *>(uintptr_t(h)));
    test_assert(bool(src));

    AnyBvh bvh;
    test_assert(bvh.build(rt, src).ok() && calls == 3 && !otherThread);
    GeometryBvh<BoxSource> want;
    test_assert(want.build(direct(data)).ok());
    test_assert(sameTree(bvh.boxes().bvh(), want.bvh()));

    failAt = 4; /* the second chunk of the next build */
    const Status s = bvh.build(rt, src);
    test_assert(s.code() == StatusCode::Internal && strstr(s.message(), "boom"));
    test_assert(calls == 5 && !rt.hasStickyError());
    test_assert(sameTree(bvh.boxes().bvh(), want.bvh()));
    failSize = true;
    test_assert(bvh.build(rt, src).code() == StatusCode::Internal && calls == 5);
    test_assert(!rt.hasStickyError() && sameTree(bvh.boxes().bvh(), want.bvh()));
    fakehost::lf::release(h);
  }
  test_assert(host.live() == 0 && host.allReleasedOnce());
}

void testBoxBinding()
{
  const auto *st = Box3d::defineBindings();
  test_assert(st == Box3d::defineBindings() && strcmp(st->name.c_str(), "coredcc::Box3d") == 0);
  Box3d b;
  test_assert(b.empty());
  for (int a = 0; a < 3; a++) {
    b.put(a, -1.0 - a, 2.0 + a);
  }
  b.put(3, 9, 9);
  b.put(-1, 9, 9);
  test_assert(!b.empty() && b.minAt(2) == -3.0 && b.maxAt(0) == 2.0);
  test_assert(b.minAt(3) == 0.0 && b.maxAt(-1) == 0.0);
}

} // namespace

int main()
{
  testBoxBinding();
  testNative();
  testRefit();
  testNumbers();
  testForeign();
  return test_end();
}
