/* Building and refitting: extreme coordinates, options, determinism, refit. */
#include "bvh_fixture.h"

#include "litestl/util/task.h"
#include "test_util.h"

test_init;

using namespace bvhtest;

namespace {

using litestl::util::Vector;

/* A source of bare boxes in either scalar. */
template <class S> struct Boxes {
  using Scalar = S;
  std::vector<Box3<S>> boxes;

  int size() const
  {
    return int(boxes.size());
  }
  Box3<S> bounds(int i) const
  {
    return boxes[size_t(i)];
  }
  void add(S x0, S y0, S z0, S x1, S y1, S z1)
  {
    Box3<S> b;
    b.include(x0, y0, z0);
    b.include(x1, y1, z1);
    boxes.push_back(b);
  }
};

template <class S> void build(const Boxes<S> &src, const char *what, int leafSize = 1)
{
  for (bool parallel : {false, true}) {
    Bvh<S> bvh;
    BvhOptions opts;
    opts.leafSize = leafSize;
    opts.parallel = parallel;
    const Status s = bvh.build(src, opts);
    const char *problem = s.ok() ? treeProblem(bvh, src, leafSize) : s.message();
    if (problem) {
      fprintf(stderr, "  %s: %s\n", what, problem);
    }
    test_assert(problem == nullptr);
    test_assert(bvh::finite(bvh.cost()) && bvh.cost() >= 0.0);
  }
}

template <class S> void testExtremes()
{
  const S big = (std::numeric_limits<S>::max)(), den = std::numeric_limits<S>::denorm_min();
  const S inf = std::numeric_limits<S>::infinity(), nan = std::numeric_limits<S>::quiet_NaN();
  Rand r(21);

  Boxes<S> src;
  build(src, "nothing");
  src.add(1, 2, 3, 1, 2, 3);
  build(src, "one point");
  src.add(1, 2, 3, 1, 2, 3);
  build(src, "two equal points");
  for (int i = 0; i < 5000; i++) {
    src.add(1, 2, 3, 1, 2, 3);
  }
  build(src, "many equal points");

  src = {};
  for (int i = 0; i < 300; i++) {
    const S s = S(r.range(-1, 1));
    src.add(big * s, big * S(r.range(-1, 1)), -big, big, big * S(r.unit()), big);
  }
  build(src, "the largest coordinates");
  src = {};
  for (int i = 0; i < 300; i++) {
    const S x = i % 2 ? big : -big;
    src.add(x, x, x, x, x, x);
  }
  build(src, "opposite corners of everything");
  src = {};
  for (int i = 0; i < 300; i++) {
    src.add(den * S(i % 7), 0, 0, den * S(i % 7 + 1), den, den);
  }
  build(src, "denormal extents");
  src = {};
  for (int i = 0; i < 300; i++) {
    const S x = std::ldexp(S(1), -140) * S(r.unit());
    src.add(x, x, x, x, x, x);
  }
  build(src, "tiny extents");
  src = {};
  for (int i = 0; i < 2000; i++) {
    const int e = i % (sizeof(S) == 4 ? 240 : 2000) - (sizeof(S) == 4 ? 120 : 1000);
    const S x = std::ldexp(S(1), e);
    src.add(x, 0, 0, x, 0, 0);
  }
  build(src, "every magnitude on a line");
  src = {};
  for (int i = 0; i < 4000; i++) {
    src.add(S(i), 0, 0, S(i), 0, 0);
  }
  build(src, "points on a line");
  Bvh<S> line;
  BvhOptions one;
  one.leafSize = 1;
  test_assert(line.build(src, one).ok());
  struct Depth {
    const Vector<BvhNode<S>> &nodes;
    int of(uint32_t i) const
    {
      const BvhNode<S> &n = nodes[int(i)];
      return n.count > 0 ? 1 : 1 + (std::max)(of(i + 1), of(n.rightOrFirst));
    }
  };
  /* Boxes without area split evenly: 4000 takes 12 halvings. */
  test_assert(Depth{line.nodes()}.of(0) <= 14);

  src = {};
  src.add(0, 0, 0, 1, 1, 1);
  src.add(nan, 0, 0, 1, 1, 1);
  src.boxes[1].min[0] = nan;
  src.add(0, 0, 0, 1, inf, 1);
  src.boxes.push_back(Box3<S>()); /* empty */
  src.add(2, 2, 2, 3, 3, 3);
  build(src, "some unusable");
  Bvh<S> bvh;
  test_assert(bvh.build(src).ok() && bvh.size() == 5 && bvh.skipped() == 3);
  src.boxes = {src.boxes[1], src.boxes[2], src.boxes[3]};
  build(src, "all unusable");
}

/* Scaling by a power of two doesn't change the tree: no absolute tolerance, and no
 * intermediate leaves the range. */
template <class S> void testScale()
{
  Rand r(61);
  const bool isFloat = sizeof(S) == 4;
  Boxes<S> one;
  for (int i = 0; i < 3000; i++) {
    const double3 a = r.vec(1.0), b = a + r.vec(0.05);
    one.add(S(a[0]), S(a[1]), S(a[2]), S(b[0]), S(b[1]), S(b[2]));
  }
  BvhOptions opts;
  opts.leafSize = 4;
  Bvh<S> tree;
  test_assert(tree.build(one, opts).ok());
  for (int e : {isFloat ? 127 : 1023, isFloat ? -60 : -500, isFloat ? 80 : 700}) {
    Boxes<S> scaled;
    for (const Box3<S> &b : one.boxes) {
      Box3<S> s;
      for (int a = 0; a < 3; a++) {
        s.min[a] = std::ldexp(b.min[a], e);
        s.max[a] = std::ldexp(b.max[a], e);
      }
      scaled.boxes.push_back(s);
    }
    Bvh<S> other;
    test_assert(other.build(scaled, opts).ok());
    test_assert(memcmp(tree.order().data(), other.order().data(),
                       tree.order().size() * sizeof(int)) == 0);
    test_assert(tree.nodes().size() == other.nodes().size() && tree.cost() == other.cost());
  }
}

void testOptions()
{
  Boxes<float> src;
  for (int i = 0; i < 10; i++) {
    src.add(float(i), 0, 0, float(i), 1, 1);
  }
  Bvh<float> bvh;
  test_assert(bvh.build(src).ok());
  const int kept = bvh.size();
  for (int leafSize : {0, -1}) {
    BvhOptions opts;
    opts.leafSize = leafSize;
    test_assert(bvh.build(Boxes<float>(), opts).code() == StatusCode::InvalidArgument);
  }
  for (int bins : {-1, 0, 1, 65, 1 << 30}) {
    BvhOptions opts;
    opts.bins = bins;
    test_assert(bvh.build(Boxes<float>(), opts).code() == StatusCode::InvalidArgument);
  }
  test_assert(bvh.size() == kept && treeProblem(bvh, src, 128) == nullptr);
  BvhOptions opts;
  opts.leafSize = (std::numeric_limits<int>::max)();
  test_assert(bvh.build(src, opts).ok() && bvh.nodes().size() == 1);
}

/* Bit-identical trees for any number of bands, and without the pool. */
template <class S> void testDeterminism()
{
  Rand r(31);
  for (int layout = 0; layout < int(Layout::Count); layout++) {
    for (int n : {5000, 30000}) {
      const TriangleSource<S> src = triangles<S>(r, Layout(layout), n);
      BvhOptions opts;
      opts.leafSize = 16;
      opts.parallel = false;
      Bvh<S> serial;
      test_assert(serial.build(src, opts).ok());
      test_assert(treeProblem(serial, src, 16) == nullptr);
      opts.parallel = true;
      for (int bands : {1, 2, 5, 12}) {
        litestl::task::ScopedBandCount scoped(bands);
        Bvh<S> tree;
        test_assert(tree.build(src, opts).ok());
        test_assert(sameTree(serial, tree));
      }
    }
  }
  /* Large enough for the parallel scans at the top. */
  const TriangleSource<S> src = triangles<S>(r, Layout::Uniform, 200000);
  BvhOptions opts;
  opts.parallel = false;
  Bvh<S> serial, parallel;
  test_assert(serial.build(src, opts).ok());
  opts.parallel = true;
  test_assert(parallel.build(src, opts).ok());
  test_assert(sameTree(serial, parallel));
}

void testRefit()
{
  Rand r(41);
  const int n = 3000;
  TriangleSource<float> src = triangles<float>(r, Layout::Uniform, n);
  GeometryBvh<TriangleSource<float>> bvh;
  BvhOptions opts;
  opts.leafSize = 8;
  test_assert(bvh.build(src, opts).ok());
  const double before = bvh.bvh().cost();
  test_assert(before > 1.0);
  const Vector<int> order = bvh.bvh().order();

  /* The same geometry: the same tree. */
  Bvh<float> same;
  test_assert(same.build(src, opts).ok());
  test_assert(same.refit(src).ok() && sameTree(same, bvh.bvh()) && same.cost() == before);

  /* Moved: the shape is kept, the bounds are tight again, and the cost grew. */
  TriangleSource<float> moved = triangles<float>(r, Layout::Uniform, n);
  test_assert(bvh.onInvalidate(GeometryChange::Positions, moved).ok());
  test_assert(treeProblem(bvh.bvh(), moved, 8) == nullptr);
  test_assert(memcmp(order.data(), bvh.bvh().order().data(), order.size() * sizeof(int)) == 0);
  test_assert(bvh.bvh().cost() > before * 2);
  Box3f all;
  for (int i = 0; i < n; i++) {
    all.include(moved.bounds(i));
  }
  const Box3f got = bvh.bvh().bounds();
  test_assert(memcmp(&all, &got, sizeof(all)) == 0);
  for (int k = 0; k < 200; k++) {
    Ray ray;
    ray.origin = r.vec(12);
    ray.dir = moved.corner(r.below(n), 0) - ray.origin;
    const bvh::RayQuery q(ray);
    double limit = q.tMax, best = -1.0;
    moved.rayRange(q, 0, n, limit, [&](int, double t, double, double, bool) {
      best = t;
      limit = t;
      return true;
    });
    const RayHit h = bvh.rayClosest(ray);
    test_assert(bool(h) == (best >= 0.0) && (!h || h.t == q.toRayT(best)));
  }

  /* Topology: a new tree. */
  test_assert(
      bvh.onInvalidate(GeometryChange::Topology | GeometryChange::Positions, moved).ok());
  test_assert(bvh.bvh().cost() < before * 1.5 && treeProblem(bvh.bvh(), moved, 8) == nullptr);

  /* Refused: another size, another set left out. onInvalidate builds instead. */
  Boxes<float> some;
  for (int i = 0; i < 100; i++) {
    some.add(float(i), 0, 0, float(i) + 0.5f, 1, 1);
  }
  some.boxes[7] = Box3f();
  Bvh<float> tree;
  BvhOptions two;
  two.leafSize = 2;
  test_assert(tree.build(some, two).ok() && tree.skipped() == 1 && tree.refit(some).ok());
  Bvh<float> copy = tree;
  Boxes<float> other = some;
  other.boxes[7] = other.boxes[6]; /* one more usable */
  test_assert(tree.refit(other).code() == StatusCode::FailedPrecondition);
  other.boxes[8] = Box3f(); /* the same number, another one */
  test_assert(tree.refit(other).code() == StatusCode::FailedPrecondition);
  other = some;
  other.boxes[8].max[1] = std::numeric_limits<float>::infinity();
  test_assert(tree.refit(other).code() == StatusCode::FailedPrecondition);
  other = some;
  other.add(0, 0, 0, 1, 1, 1);
  test_assert(tree.refit(other).code() == StatusCode::FailedPrecondition);
  test_assert(sameTree(tree, copy));

  TriangleSource<float> fewer = triangles<float>(r, Layout::Clustered, 10);
  test_assert(bvh.onInvalidate(GeometryChange::Positions, fewer).ok());
  test_assert(bvh.bvh().size() == 10 && treeProblem(bvh.bvh(), fewer, 8) == nullptr);
}

void testFromWorker()
{
  /* A build on a pool worker is serial, and the same. */
  Rand r(51);
  const TriangleSource<float> src = triangles<float>(r, Layout::Uniform, 20000);
  Bvh<float> here;
  test_assert(here.build(src).ok());
  Bvh<float> there[4];
  litestl::task::parallel_for_chunks(
      litestl::util::IndexRange(0, 4), 1, [&](int k, litestl::util::IndexRange) {
        there[k].build(src).ignore();
      });
  for (const Bvh<float> &t : there) {
    test_assert(sameTree(here, t));
  }
}

} // namespace

int main()
{
  testOptions();
  testExtremes<float>();
  testExtremes<double>();
  testDeterminism<float>();
  testDeterminism<double>();
  testScale<float>();
  testScale<double>();
  testRefit();
  testFromWorker();
  return test_end();
}
