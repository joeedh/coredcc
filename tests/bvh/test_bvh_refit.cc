/* Refits, of everything and of a few primitives, and what they do to a tree. */
#include "bvh_fixture.h"

#include "test_util.h"

#include <algorithm>

test_init;

using namespace bvhtest;

namespace {

using litestl::util::Vector;

/* A source of bare boxes. */
struct Boxes {
  using Scalar = float;
  std::vector<Box3f> boxes;

  int size() const
  {
    return int(boxes.size());
  }
  Box3f bounds(int i) const
  {
    return boxes[size_t(i)];
  }
  void add(float x0, float y0, float z0, float x1, float y1, float z1)
  {
    Box3f b;
    b.include(x0, y0, z0);
    b.include(x1, y1, z1);
    boxes.push_back(b);
  }
};

/* Every ray and distance from the tree is what a loop over the source gives. */
template <class Src> void checkQueries(const GeometryBvh<Src> &bvh, const Src &src, Rand &r, int n)
{
  for (int k = 0; k < n; k++) {
    Ray ray;
    ray.origin = r.vec(12);
    const int aim = r.below(src.size());
    const Box3f box = src.bounds(aim);
    ray.dir = (box.empty() ? r.vec(1.0) : src.closest(aim, ray.origin)) - ray.origin;
    const bvh::RayQuery q(ray);
    RayHit want;
    NearestHit near_;
    int within = 0;
    const double radius = r.range(0.0, 3.0);
    for (int i = 0; i < src.size(); i++) {
      const Box3f b = src.bounds(i);
      if (b.empty() || !b.allFinite()) {
        continue;
      }
      double limit = q.tMax;
      src.rayRange(q, i, 1, limit, [&](int, const bvh::PrimHit &h) {
        const double t = q.toRayT(h.t);
        if (!want || t < want.t) {
          want.primitive = i;
          want.t = t;
        }
        return true;
      });
      const double d = length(ray.origin - src.closest(i, ray.origin));
      if (d <= radius && (!near_ || d < near_.distance)) {
        near_.primitive = i;
        near_.distance = d;
      }
      within += d <= radius;
    }
    const RayHit hit = bvh.rayClosest(ray);
    test_assert(hit.primitive == want.primitive && (!hit || hit.t == want.t));
    test_assert(bvh.nearest(ray.origin, radius).primitive == near_.primitive);
    test_assert(bvh.sphereOverlap(ray.origin, radius, [&](int) {
                     within--;
                     return true;
                   })
                    .ok());
    test_assert(within == 0);
  }
}

/* A node is exactly around what is in it. */
template <class S, class Src> bool tight(const Bvh<S> &bvh, const Src &src)
{
  const auto &nodes = bvh.nodes();
  for (int i = 0; i < int(nodes.size()); i++) {
    Box3<S> want;
    if (nodes[i].count > 0) {
      for (uint32_t k = 0; k < nodes[i].count; k++) {
        const Box3<S> b = src.bounds(bvh.order()[int(nodes[i].rightOrFirst + k)]);
        if (!b.empty() && b.allFinite()) {
          want.include(b);
        }
      }
    } else {
      for (int child : {i + 1, int(nodes[i].rightOrFirst)}) {
        for (int a = 0; a < 3; a++) {
          want.min[a] = (std::min)(want.min[a], nodes[child].min[a]);
          want.max[a] = (std::max)(want.max[a], nodes[child].max[a]);
        }
      }
    }
    if (memcmp(want.min, nodes[i].min, sizeof(want.min)) != 0 ||
        memcmp(want.max, nodes[i].max, sizeof(want.max)) != 0)
    {
      return false;
    }
  }
  return true;
}

void testFull(bool parallel)
{
  Rand r(41);
  const int n = 3000;
  TriangleSource<float> src = triangles<float>(r, Layout::Uniform, n);
  GeometryBvh<TriangleSource<float>> bvh;
  BvhOptions opts;
  opts.leafSize = 8;
  opts.parallel = parallel;
  test_assert(bvh.build(src, opts).ok() && bvh.options().leafSize == 8);
  const double before = bvh.bvh().cost();
  test_assert(before > 1.0 && bvh.bvh().growth() == 1.0);
  const Vector<int> order = bvh.bvh().order();

  /* The same geometry: the same tree. */
  Bvh<float> same;
  test_assert(same.build(src, opts).ok());
  test_assert(same.refit(src).ok() && sameTree(same, bvh.bvh()) && same.cost() == before);

  /* Moved: the shape is kept, the bounds are tight again, and it got slower. */
  TriangleSource<float> moved = triangles<float>(r, Layout::Uniform, n);
  test_assert(bvh.onInvalidate(InvalidateKinds::Positions, moved).ok());
  test_assert(treeProblem(bvh.bvh(), moved, 8) == nullptr && tight(bvh.bvh(), moved));
  test_assert(memcmp(order.data(), bvh.bvh().order().data(), order.size() * sizeof(int)) == 0);
  test_assert(bvh.bvh().cost() > before * 2 && bvh.bvh().growth() > 2.0);
  Box3f all;
  for (int i = 0; i < n; i++) {
    all.include(moved.bounds(i));
  }
  const Box3f got = bvh.bvh().bounds();
  test_assert(memcmp(&all, &got, sizeof(all)) == 0);
  checkQueries(bvh, moved, r, 200);

  /* Other changes: nothing. */
  for (InvalidateKinds kinds : {InvalidateKinds::None,
                                InvalidateKinds::Attributes | InvalidateKinds::Selection |
                                    InvalidateKinds::Materials})
  {
    const Bvh<float> was = bvh.bvh();
    test_assert(bvh.onInvalidate(kinds, src).ok() && sameTree(was, bvh.bvh()));
  }
  /* Topology: a new tree. */
  test_assert(
      bvh.onInvalidate(InvalidateKinds::Topology | InvalidateKinds::Positions, moved).ok());
  test_assert(bvh.bvh().cost() < before * 1.5 && bvh.bvh().growth() == 1.0);
  test_assert(treeProblem(bvh.bvh(), moved, 8) == nullptr);

  /* Another size is refused. onInvalidate builds instead. */
  TriangleSource<float> fewer = triangles<float>(r, Layout::Clustered, 10);
  test_assert(bvh.refit(fewer).code() == StatusCode::FailedPrecondition);
  test_assert(bvh.onInvalidate(InvalidateKinds::Positions, fewer).ok());
  test_assert(bvh.bvh().size() == 10 && treeProblem(bvh.bvh(), fewer, 8) == nullptr);
}

/* Built from a source that is given away. */
void testMoved()
{
  Rand r(42);
  for (int layout = 0; layout < int(Layout::Count); layout++) {
    CapsuleSource<float> src = capsules<float>(r, Layout(layout), 70000);
    src.append(double3(), double3(), -1.0);
    GeometryBvh<CapsuleSource<float>> copied, taken;
    test_assert(copied.build(src).ok());
    CapsuleSource<float> given = src;
    BvhOptions bad;
    bad.leafSize = 0;
    test_assert(!taken.build(std::move(given), bad).ok() && given.size() == 70001);
    test_assert(taken.build(std::move(given)).ok() && sameTree(taken.bvh(), copied.bvh()));
    test_assert(taken.source().size() == 70000 && copied.source().size() == 70000);
    for (int i = 0; i < 70000; i++) {
      const int id = taken.bvh().order()[i];
      for (int e = 0; e < 2; e++) {
        const double3 a = taken.source().end(i, e), b = copied.source().end(i, e);
        test_assert(length(a - b) == 0.0 && length(a - src.end(id, e)) == 0.0);
      }
      test_assert(taken.source().radius(i) == src.radius(id));
    }
  }
}

/* What growth() says: areas, and not of nodes that were almost nothing. */
void testAreas()
{
  Rand r(44);
  Boxes some;
  for (int i = 0; i < 4000; i++) {
    const double3 p = r.vec(10.0);
    some.add(float(p[0]), float(p[1]), float(p[2]), float(p[0]), float(p[1]), float(p[2]));
  }
  for (int i = 0; i < 1000; i++) {
    some.add(1, 2, 3, 1, 2, 3);
  }
  Bvh<float> tree;
  BvhOptions opts;
  opts.leafSize = 4;
  test_assert(tree.build(some, opts).ok() && tree.growth() == 1.0);
  Boxes moved = some;
  for (Box3f &b : moved.boxes) {
    for (int i = 0; i < 3; i++) {
      b.min[i] *= 1.5f;
      b.max[i] *= 1.5f;
    }
  }
  /* 1.5 times as long is 2.25 times as large, for all nodes but the ones at (1, 2, 3). */
  test_assert(tree.refit(moved).ok());
  test_assert(tree.growth() > 1.9 && tree.growth() < 2.2501);
  moved = some;
  for (int i = 4000; i < 5000; i++) {
    moved.boxes[i].max[1] += 1e-6f;
  }
  test_assert(tree.refit(moved).ok() && tree.growth() < 1.0001);
}

/* Primitives that lose their bounds, and get them back. */
void testAbsent()
{
  Boxes some;
  for (int i = 0; i < 100; i++) {
    some.add(float(i), 0, 0, float(i) + 0.5f, 1, 1);
  }
  some.boxes[7] = Box3f();
  Bvh<float> tree;
  BvhOptions two;
  two.leafSize = 2;
  test_assert(tree.build(some, two).ok() && tree.skipped() == 1 && tree.refit(some).ok());
  Bvh<float> copy = tree;

  /* One that the build left out can't come. */
  Boxes other = some;
  other.boxes[7] = other.boxes[6];
  test_assert(tree.refit(other).code() == StatusCode::FailedPrecondition);
  other.boxes[8] = Box3f();
  test_assert(tree.refit(other).code() == StatusCode::FailedPrecondition);
  other = some;
  other.add(0, 0, 0, 1, 1, 1);
  test_assert(tree.refit(other).code() == StatusCode::FailedPrecondition);
  test_assert(sameTree(tree, copy));
  for (int k = 0; k < 99; k++) {
    test_assert(tree.present(k));
  }

  other = some;
  other.boxes[8] = Box3f();
  other.boxes[9].max[1] = std::numeric_limits<float>::infinity();
  other.boxes[40].min[2] = std::numeric_limits<float>::quiet_NaN();
  test_assert(tree.refit(other).ok() && tight(tree, other));
  for (int k = 0; k < 99; k++) {
    const int id = tree.order()[k];
    test_assert(tree.present(k) == (id != 8 && id != 9 && id != 40));
  }
  test_assert(bvh::finite(tree.cost()) && tree.cost() > 0.0 && tree.growth() == 1.0);
  test_assert(tree.refit(some).ok() && sameTree(tree, copy));
  for (int k = 0; k < 99; k++) {
    test_assert(tree.present(k));
  }
  /* A leaf of which nothing is left costs nothing, and didn't grow. */
  {
    other = some;
    for (const auto &node : tree.nodes()) {
      if (node.count > 0) {
        for (uint32_t k = 0; k < node.count; k++) {
          other.boxes[tree.order()[int(node.rightOrFirst + k)]] = Box3f();
        }
        break;
      }
    }
    const double cost = tree.cost();
    test_assert(tree.refit(other).ok());
    test_assert(tree.cost() < cost * 1.1 && tree.growth() == 1.0);
    /* A build that replaces it has no memory of that. */
    test_assert(tree.build(some, two).ok() && sameTree(tree, copy) && tree.present(0));
    test_assert(tree.growth() == 1.0);
  }
  /* All of them. */
  for (Box3f &b : other.boxes) {
    b = Box3f();
  }
  test_assert(tree.refit(other).ok() && tree.bounds().empty());
  test_assert(bvh::finite(tree.cost()) && bvh::finite(tree.growth()));
  test_assert(tree.refit(some).ok() && sameTree(tree, copy));

  /* No query finds one, of any kind of source. */
  Rand r(43);
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const double inf = std::numeric_limits<double>::infinity();
  const double3 none(nan, nan, nan);
  TriangleSource<float> src = triangles<float>(r, Layout::Uniform, 400), half;
  CapsuleSource<float> caps = capsules<float>(r, Layout::Uniform, 400), capsHalf;
  SphereSource<float> balls = spheres<float>(r, Layout::Uniform, 400), ballsHalf;
  for (int i = 0; i < 400; i++) {
    const bool gone = i % 3 == 0;
    half.append(gone ? none : src.corner(i, 0), src.corner(i, 1), src.corner(i, 2));
    /* Bad radii, at places where rays and distances would find them. */
    capsHalf.append(caps.end(i, 0), caps.end(i, 1), gone ? inf : caps.radius(i));
    ballsHalf.append(balls.center(i), gone ? -1.0 : balls.radius(i));
  }
  const auto check = [&](const auto &all, const auto &some) {
    for (int leafSize : {1, 16, 1000}) {
      GeometryBvh<std::remove_cvref_t<decltype(all)>> bvh;
      BvhOptions opts;
      opts.leafSize = leafSize;
      test_assert(bvh.build(all, opts).ok() && bvh.refit(some).ok());
      checkQueries(bvh, some, r, 100);
      int found = 0;
      test_assert(bvh.region({}, [&](int i) {
                       test_assert(i % 3 != 0);
                       return ++found > 0;
                     })
                      .ok());
      test_assert(found == 266);
    }
  };
  check(src, half);
  check(caps, capsHalf);
  check(balls, ballsHalf);
}

/* A few at a time: exactly what a refit of everything gives. */
void testSome()
{
  Rand r(44);
  const double nan = std::numeric_limits<double>::quiet_NaN();
  for (int leafSize : {1, 8, 128, 5000}) {
    SphereSource<float> src = spheres<float>(r, Layout::Uniform, 4000);
    src.append(double3(), -1.0); /* left out */
    GeometryBvh<SphereSource<float>> some, full;
    BvhOptions opts;
    opts.leafSize = leafSize;
    test_assert(some.build(src, opts).ok() && full.build(src, opts).ok());
    for (int round = 0; round < 30; round++) {
      SphereSource<float> moved;
      std::vector<int> which;
      const int count = round == 0 ? 0 : 1 + r.below(round % 10 == 9 ? 6000 : 20);
      for (int k = 0; k < count; k++) {
        const int i = r.below(4000);
        if (std::find(which.begin(), which.end(), i) != which.end()) {
          continue;
        }
        which.push_back(i);
        const double radius = r.below(8) == 0 ? nan : r.range(0.0, 0.4);
        moved.append(r.vec(10.0), radius);
        src.set(i, moved, moved.size() - 1);
      }
      test_assert(some.refit(moved, which).ok());
      test_assert(full.refit(src).ok() && sameTree(some.bvh(), full.bvh()));
      for (int k = 0; k < 4000; k++) {
        test_assert(some.bvh().present(k) == full.bvh().present(k));
        test_assert(length(some.source().center(k) - full.source().center(k)) == 0.0);
      }
      checkQueries(some, src, r, 10);
    }
    /* A whole refit after that, and a build. */
    src = spheres<float>(r, Layout::Uniform, 4000);
    src.append(double3(), -1.0);
    test_assert(some.refit(src).ok() && full.refit(src).ok() && sameTree(some.bvh(), full.bvh()));
    test_assert(some.build(src, opts).ok());
    SphereSource<float> one;
    one.append(double3(1, 2, 3), 0.5);
    const int last = 3999;
    test_assert(some.refit(one, {&last, 1}).ok());
    src.set(last, one, 0);
    test_assert(tight(some.bvh(), src));

    /* Nothing changes on an error: too few, no such primitive, one that was left out. */
    const Bvh<float> was = some.bvh();
    one.append(double3(50, 0, 0), 1.0);
    const int bad[][2] = {{0, -1}, {0, 4001}, {0, 4000}};
    const StatusCode codes[] = {
        StatusCode::OutOfRange, StatusCode::OutOfRange, StatusCode::FailedPrecondition};
    for (int i = 0; i < 3; i++) {
      test_assert(some.refit(one, bad[i]).code() == codes[i]);
    }
    test_assert(some.refit(one, {&last, 1}).code() == StatusCode::InvalidArgument);
    test_assert(sameTree(was, some.bvh()));
    checkQueries(some, src, r, 10);
    /* Left out, and still without bounds. */
    SphereSource<float> none;
    none.append(double3(), nan);
    const int out = 4000;
    test_assert(some.refit(none, {&out, 1}).ok() && sameTree(was, some.bvh()));
  }
  /* The same primitive twice: the last one counts. Nothing at all. */
  SphereSource<float> src = spheres<float>(r, Layout::Uniform, 100), two;
  GeometryBvh<SphereSource<float>> bvh;
  const int twice[] = {5, 5};
  two.append(double3(90, 0, 0), 1.0);
  two.append(double3(0, 90, 0), 1.0);
  test_assert(bvh.build(src).ok() && bvh.refit(two, twice).ok());
  src.set(5, two, 1);
  test_assert(tight(bvh.bvh(), src));
  GeometryBvh<SphereSource<float>> empty;
  test_assert(empty.refit(SphereSource<float>(), {}).ok());
  test_assert(empty.refit(two, twice).code() == StatusCode::OutOfRange);
  test_assert(empty.bvh().growth() == 1.0);
}

/* Of the nodes' boxes. */
std::vector<double> areas(const Bvh<float> &bvh)
{
  std::vector<double> out;
  for (const auto &n : bvh.nodes()) {
    const double x = double(n.max[0]) - n.min[0], y = double(n.max[1]) - n.min[1],
                 z = double(n.max[2]) - n.min[2];
    out.push_back(x * y + y * z + z * x);
  }
  return out;
}

/* growth() sees a tree that got slow, whatever else is in it. */
void testGrowth()
{
  Rand r(45);
  for (float outlier : {0.0f, 1e5f, 1e9f}) {
    Boxes src;
    for (int i = 0; i < 1000; i++) {
      const double3 p = r.vec(10.0);
      src.add(float(p[0]), float(p[1]), float(p[2]), float(p[0]) + 0.2f, float(p[1]) + 0.2f,
              float(p[2]) + 0.2f);
    }
    if (outlier > 0.0f) {
      src.add(outlier, outlier, outlier, outlier * 1.01f, outlier * 1.01f, outlier * 1.01f);
    }
    Bvh<float> bvh;
    BvhOptions opts;
    opts.leafSize = 4;
    test_assert(bvh.build(src, opts).ok());
    const std::vector<double> built = areas(bvh);
    const auto move = [&](float dx, float dy, float dz, int from, int to) {
      for (int i = from; i < to; i++) {
        Box3f &b = src.boxes[size_t(i)];
        const float d[3] = {dx, dy, dz};
        for (int a = 0; a < 3; a++) {
          b.min[a] += d[a];
          b.max[a] += d[a];
        }
      }
    };
    /* Everything, and back. */
    move(100, -50, 20, 0, 1000);
    test_assert(bvh.refit(src).ok() && bvh.growth() < 1.2);
    move(-100, 50, -20, 0, 1000);
    test_assert(bvh.refit(src).ok() && bvh.growth() < 1.1);
    /* One leaves, very far: only its path grows. */
    move(1e7f, 0, 0, 3, 4);
    test_assert(bvh.refit(src).ok() && bvh.growth() > 1.0 && bvh.growth() < 1.3);
    move(-1e7f, 0, 0, 3, 4);
    /* A shuffle inside the same bounds. */
    for (int i = 0; i < 1000; i++) {
      const int j = r.below(1000);
      std::swap(src.boxes[size_t(i)], src.boxes[size_t(j)]);
    }
    test_assert(bvh.refit(src).ok() && bvh.growth() > 3.0 && bvh.growth() <= 8.0);
    const std::vector<double> now = areas(bvh);
    double sum = 0.0;
    for (size_t i = 0; i < now.size(); i++) {
      sum += std::fmin(std::fmax(now[i] / built[i], 1.0), 8.0);
    }
    test_assert(std::fabs(bvh.growth() * double(now.size()) / sum - 1.0) < 1e-6);
    const double before = bvh.cost();
    test_assert(bvh.build(src, opts).ok() && bvh.growth() == 1.0);
    /* The cost doesn't see it when something is far away. */
    test_assert(outlier > 0.0f ? before < bvh.cost() * 1.5 : before > bvh.cost() * 2.0);
  }
  /* Without area, on a line or as points; and of any size. */
  for (float size : {1e-30f, 1.0f, 1e30f}) {
    Boxes src;
    for (int i = 0; i < 100; i++) {
      src.add(size * float(i), 0, 0, size * float(i), 0, 0);
    }
    Bvh<float> bvh;
    BvhOptions opts;
    opts.leafSize = 1;
    test_assert(bvh.build(src, opts).ok() && bvh.growth() == 1.0);
    for (int i = 0; i < 100; i++) {
      src.boxes[size_t(i)].min[0] += size;
      src.boxes[size_t(i)].max[0] += size;
    }
    test_assert(bvh.refit(src).ok() && bvh.growth() == 1.0);
    for (int i = 0; i < 100; i++) {
      src.boxes[size_t(i)].max[1] = size;
    }
    test_assert(bvh.refit(src).ok() && bvh.growth() > 1.5 && bvh.growth() <= 8.0);
  }
}

} // namespace

int main()
{
  testFull(false);
  testFull(true);
  testMoved();
  testAreas();
  testAbsent();
  testSome();
  testGrowth();
  return test_end();
}
