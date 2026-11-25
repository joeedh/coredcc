#include "coredcc/bvh/bvh.h"

#include "litestl/util/task.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>

#if defined(__EMSCRIPTEN_PTHREADS__)
#include <emscripten/em_asm.h>
#include <emscripten/threading.h>
#endif

namespace coredcc::bvh {

using litestl::util::IndexRange;
using litestl::util::Vector;

bool poolUsable()
{
#if defined(__EMSCRIPTEN_PTHREADS__)
  return !emscripten_is_main_browser_thread() ||
         EM_ASM_INT({ return ENVIRONMENT_IS_NODE ? 1 : 0; }) != 0;
#elif defined(__EMSCRIPTEN__)
  return false; /* no threads */
#else
  return true;
#endif
}

namespace {

constexpr int kMaxBins = 64;
/** A subtree this small is built by one task. */
constexpr int kSubtreeSize = 4096;
constexpr int kMaxSubtrees = 64;
/** A range this large is scanned in parallel. */
constexpr int kScanChunk = 1 << 16;

/** What choosing a split needs to know about a range. */
template <class S> struct Extents {
  Box3<S> box;
  /** Of half the centroids' doubles: (min + max) / 2 can overflow. */
  double cmin[3], cmax[3];

  Extents()
  {
    for (int a = 0; a < 3; a++) {
      cmin[a] = (std::numeric_limits<double>::max)();
      cmax[a] = std::numeric_limits<double>::lowest();
    }
  }
  void include(const Extents &b)
  {
    box.include(b.box);
    for (int a = 0; a < 3; a++) {
      cmin[a] = b.cmin[a] < cmin[a] ? b.cmin[a] : cmin[a];
      cmax[a] = b.cmax[a] > cmax[a] ? b.cmax[a] : cmax[a];
    }
  }
};

template <class S> double centroid(const Box3<S> &b, int axis)
{
  return 0.5 * double(b.min[axis]) + 0.5 * double(b.max[axis]);
}

template <class S> struct Bin {
  Box3<S> box;
  int count = 0;
};
template <class S> struct Bins {
  Bin<S> bin[3][kMaxBins];
};

/** Maps centroids to bins along each axis. */
struct Binning {
  double base[3], scale[3];
  bool usable[3];
  int bins;

  template <class S> Binning(const Extents<S> &e, int bins) : bins(bins)
  {
    for (int a = 0; a < 3; a++) {
      /* Halved, so the widest range doesn't overflow. */
      base[a] = 0.5 * e.cmin[a];
      const double half = 0.5 * e.cmax[a] - base[a];
      scale[a] = half > 0.0 ? double(bins) / half : 0.0;
      usable[a] = half > 0.0;
    }
  }
  int of(int axis, double c) const
  {
    const double x = (0.5 * c - base[axis]) * scale[axis];
    /* Clamped before the cast. The scale is infinite for denormal extents, so x can be
     * infinite or NaN: the last bin or the first. */
    return x >= double(bins - 1) ? bins - 1 : x > 0.0 ? int(x) : 0;
  }
};

/** Half the surface area of a box whose extents are multiplied by `scale` / 2. */
template <class S> double scaledArea(const Box3<S> &b, double scale)
{
  if (b.empty()) {
    return 0.0;
  }
  double d[3];
  for (int a = 0; a < 3; a++) {
    d[a] = (0.5 * double(b.max[a]) - 0.5 * double(b.min[a])) * scale;
  }
  return d[0] * d[1] + d[1] * d[2] + d[2] * d[0];
}

/** 1 over the box's largest half extent: not finite when that is 0 or too small. */
template <class S> double areaScale(const Box3<S> &b)
{
  double big = 0.0;
  for (int a = 0; a < 3; a++) {
    const double d = 0.5 * double(b.max[a]) - 0.5 * double(b.min[a]);
    big = d > big ? d : big;
  }
  return big > 0.0 ? 1.0 / big : std::numeric_limits<double>::infinity();
}

/** A run of primitives still to be split, and how. */
struct Range {
  int first = 0, count = 0, depth = 0;
  /** In median order: halve it without looking further. */
  bool sorted = false;
};

template <class S> class Builder {
public:
  Builder(Vector<PrimBox<S>> &prims, const BvhOptions &opts, bool parallel)
      : prims_(prims), opts_(opts), parallel_(parallel)
  {
  }

  Box3<S> boundsOf(const Range &r) const
  {
    return extents(r).box;
  }

  /** Orders the range's primitives and returns where its right half starts. */
  int split(Range &r, Bins<S> &scratch)
  {
    if (!r.sorted) {
      const Extents<S> e = extents(r);
      if (r.depth < kMaxSahDepth) {
        const int mid = splitSah(r, e, scratch);
        if (mid > r.first) {
          return mid;
        }
      }
      sortMedian(r, e);
      r.sorted = true;
    }
    return r.first + r.count / 2;
  }

  /** Appends the range's subtree; returns its root's index. */
  uint32_t subtree(Range r, Vector<BvhNode<S>> &nodes, Bins<S> &scratch)
  {
    const uint32_t index = uint32_t(nodes.size());
    nodes.append(makeNode(boundsOf(r)));
    if (r.count <= opts_.leafSize) {
      nodes[index].rightOrFirst = uint32_t(r.first);
      nodes[index].count = uint32_t(r.count);
      return index;
    }
    const int mid = split(r, scratch);
    subtree({r.first, mid - r.first, r.depth + 1, r.sorted}, nodes, scratch);
    const uint32_t right =
        subtree({mid, r.first + r.count - mid, r.depth + 1, r.sorted}, nodes, scratch);
    nodes[index].rightOrFirst = right;
    return index;
  }

  static BvhNode<S> makeNode(const Box3<S> &b)
  {
    BvhNode<S> n{};
    for (int a = 0; a < 3; a++) {
      n.min[a] = b.min[a];
      n.max[a] = b.max[a];
    }
    return n;
  }

private:
  bool scanParallel(const Range &r) const
  {
    return parallel_ && r.count > kScanChunk;
  }

  Extents<S> extents(const Range &r) const
  {
    const PrimBox<S> *prims = &prims_[0];
    const auto scan = [prims](IndexRange range) {
      Extents<S> e;
      for (int i : range) {
        const Box3<S> &b = prims[i].box;
        e.box.include(b);
        for (int a = 0; a < 3; a++) {
          const double c = centroid(b, a);
          e.cmin[a] = c < e.cmin[a] ? c : e.cmin[a];
          e.cmax[a] = c > e.cmax[a] ? c : e.cmax[a];
        }
      }
      return e;
    };
    const IndexRange all(r.first, r.count);
    if (!scanParallel(r)) {
      return scan(all);
    }
    return litestl::task::parallel_reduce(
        all, kScanChunk, Extents<S>(), scan, [](Extents<S> acc, const Extents<S> &b) {
          acc.include(b);
          return acc;
        });
  }

  void fillBins(const Range &r, const Binning &binning, Bins<S> &out) const
  {
    const PrimBox<S> *prims = &prims_[0];
    const auto scan = [prims, &binning](IndexRange range, Bins<S> &bins) {
      for (int i : range) {
        const Box3<S> &b = prims[i].box;
        for (int a = 0; a < 3; a++) {
          if (binning.usable[a]) {
            Bin<S> &bin = bins.bin[a][binning.of(a, centroid(b, a))];
            bin.box.include(b);
            bin.count++;
          }
        }
      }
    };
    const IndexRange all(r.first, r.count);
    if (!scanParallel(r)) {
      clear(out);
      scan(all, out);
      return;
    }
    out = litestl::task::parallel_reduce(
        all,
        kScanChunk,
        Bins<S>(),
        [&scan](IndexRange range) {
          Bins<S> bins;
          scan(range, bins);
          return bins;
        },
        [this](Bins<S> acc, const Bins<S> &b) {
          for (int a = 0; a < 3; a++) {
            for (int k = 0; k < opts_.bins; k++) {
              acc.bin[a][k].box.include(b.bin[a][k].box);
              acc.bin[a][k].count += b.bin[a][k].count;
            }
          }
          return acc;
        });
  }

  void clear(Bins<S> &bins) const
  {
    for (int a = 0; a < 3; a++) {
      for (int k = 0; k < opts_.bins; k++) {
        bins.bin[a][k] = Bin<S>();
      }
    }
  }

  /** The start of the right half, or r.first when there is no finite best split. */
  int splitSah(const Range &r, const Extents<S> &e, Bins<S> &bins)
  {
    const Binning binning(e, opts_.bins);
    const double scale = areaScale(e.box); /* not finite: no cost is below `best` */
    fillBins(r, binning, bins);

    int bestAxis = -1, bestBin = 0, bestSmaller = 0;
    double best = (std::numeric_limits<double>::max)();
    double rightCost[kMaxBins];
    for (int a = 0; a < 3; a++) {
      if (!binning.usable[a]) {
        continue;
      }
      const Bin<S> *row = bins.bin[a];
      Bin<S> acc;
      for (int k = opts_.bins - 1; k > 0; k--) {
        acc.box.include(row[k].box);
        acc.count += row[k].count;
        rightCost[k] = acc.count > 0 ? scaledArea(acc.box, scale) * acc.count : -1.0;
      }
      acc = Bin<S>();
      for (int k = 0; k + 1 < opts_.bins; k++) {
        acc.box.include(row[k].box);
        acc.count += row[k].count;
        if (acc.count == 0 || rightCost[k + 1] < 0.0) {
          continue;
        }
        const double cost = scaledArea(acc.box, scale) * acc.count + rightCost[k + 1];
        /* Boxes without area (primitives on a line) all cost 0: split those evenly. */
        const int smaller = (std::min)(acc.count, r.count - acc.count);
        if (cost < best || (cost == best && smaller > bestSmaller)) {
          best = cost;
          bestSmaller = smaller;
          bestAxis = a;
          bestBin = k;
        }
      }
    }
    if (bestAxis < 0) {
      return r.first;
    }
    /* Not std::partition: its order isn't the same in every standard library. */
    int lo = r.first, hi = r.first + r.count - 1;
    while (true) {
      while (lo <= hi &&
             binning.of(bestAxis, centroid(prims_[lo].box, bestAxis)) <= bestBin)
      {
        lo++;
      }
      while (lo <= hi &&
             binning.of(bestAxis, centroid(prims_[hi].box, bestAxis)) > bestBin)
      {
        hi--;
      }
      if (lo >= hi) {
        return lo;
      }
      std::swap(prims_[lo], prims_[hi]);
    }
  }

  void sortMedian(const Range &r, const Extents<S> &e)
  {
    int axis = 0;
    double widest = -1.0;
    for (int a = 0; a < 3; a++) {
      const double half = 0.5 * e.cmax[a] - 0.5 * e.cmin[a];
      if (half > widest) {
        widest = half;
        axis = a;
      }
    }
    PrimBox<S> *p = &prims_[r.first];
    std::sort(p, p + r.count, [axis](const PrimBox<S> &a, const PrimBox<S> &b) {
      const double ca = centroid(a.box, axis), cb = centroid(b.box, axis);
      return ca != cb ? ca < cb : a.id < b.id;
    });
  }

  Vector<PrimBox<S>> &prims_;
  const BvhOptions &opts_;
  const bool parallel_;
};

/** The top of the tree, split until its leaves can be handed to tasks. */
struct TopNode {
  Range range;
  int left = -1, right = -1; /* -1: a subtree */
  int subtree = -1;
};

template <class S>
void buildParallel(Builder<S> &builder,
                   const BvhOptions &opts,
                   int count,
                   Vector<BvhNode<S>> &nodes)
{
  Vector<TopNode> top;
  Vector<int> open; /* top nodes without children */
  top.append(TopNode{{0, count, 0, false}});
  open.append(0);
  const int enough = (std::max)(kSubtreeSize, opts.leafSize);
  const auto scratch = std::make_unique<Bins<S>>();
  while (int(open.size()) < kMaxSubtrees) {
    int big = 0;
    for (int i = 1; i < int(open.size()); i++) {
      if (top[open[i]].range.count > top[open[big]].range.count) {
        big = i;
      }
    }
    const int index = open[big];
    Range r = top[index].range;
    if (r.count <= enough) {
      break;
    }
    const int mid = builder.split(r, *scratch);
    top[index].left = int(top.size());
    top[index].right = top[index].left + 1;
    open[big] = top[index].left;
    open.append(top[index].right);
    top.append(TopNode{{r.first, mid - r.first, r.depth + 1, r.sorted}});
    top.append(TopNode{{mid, r.first + r.count - mid, r.depth + 1, r.sorted}});
  }

  /* Largest first, each worker taking the next one: the order of the work changes only
   * how long it takes. */
  std::sort(open.data(), open.data() + open.size(), [&top](int a, int b) {
    const int ca = top[a].range.count, cb = top[b].range.count;
    return ca != cb ? ca > cb : a < b;
  });
  const int n = int(open.size());
  Vector<Vector<BvhNode<S>>> parts;
  parts.resize(size_t(n));
  std::atomic<int> next{0};
  litestl::task::parallel_for_chunks(IndexRange(0, n), 1, [&](int, IndexRange) {
    const auto bins = std::make_unique<Bins<S>>(); /* too large for a worker's stack */
    for (int i = next.fetch_add(1); i < n; i = next.fetch_add(1)) {
      top[open[i]].subtree = i;
      builder.subtree(top[open[i]].range, parts[i], *bins);
    }
  });

  /* Depth first, which is the order the serial build appends in. */
  struct Emit {
    const Vector<TopNode> &top;
    const Vector<Vector<BvhNode<S>>> &parts;
    Builder<S> &builder;
    Vector<BvhNode<S>> &nodes;

    uint32_t operator()(int index)
    {
      const TopNode &t = top[index];
      const uint32_t base = uint32_t(nodes.size());
      if (t.left < 0) {
        for (BvhNode<S> node : parts[t.subtree]) {
          node.rightOrFirst += node.count > 0 ? 0 : base;
          nodes.append(node);
        }
        return base;
      }
      nodes.append(Builder<S>::makeNode(builder.boundsOf(t.range)));
      (*this)(t.left);
      const uint32_t right = (*this)(t.right);
      nodes[base].rightOrFirst = right;
      return base;
    }
  };
  Emit{top, parts, builder, nodes}(0);
}

} // namespace

template <class S>
Status
buildTree(Vector<PrimBox<S>> &prims, const BvhOptions &opts, Vector<BvhNode<S>> &nodes)
{
  if (opts.leafSize < 1) {
    return Status::error(StatusCode::InvalidArgument, "Bvh: leafSize must be at least 1");
  }
  if (opts.bins < 2 || opts.bins > kMaxBins) {
    return Status::error(StatusCode::InvalidArgument, "Bvh: bins must be 2 to 64");
  }
  nodes.clear();
  const int count = int(prims.size());
  if (count == 0) {
    return Status();
  }
  const bool parallel = opts.parallel && poolUsable() && !litestl::task::in_pool_worker();
  Builder<S> builder(prims, opts, parallel);
  if (parallel && count > (std::max)(kSubtreeSize, opts.leafSize)) {
    buildParallel(builder, opts, count, nodes);
  } else {
    const auto scratch = std::make_unique<Bins<S>>();
    builder.subtree({0, count, 0, false}, nodes, *scratch);
  }
  return Status();
}

namespace {

template <class S> Box3<S> boxOf(const BvhNode<S> &n)
{
  Box3<S> b;
  for (int a = 0; a < 3; a++) {
    b.min[a] = n.min[a];
    b.max[a] = n.max[a];
  }
  return b;
}

/* The square root of an eighth of the surface area, without overflow. */
template <class S> double sizeOf(const BvhNode<S> &n)
{
  const Box3<S> box = boxOf(n); /* if it is empty, `big` stays 0 */
  double d[3], big = 0.0;
  for (int a = 0; a < 3; a++) {
    d[a] = 0.5 * double(box.max[a]) - 0.5 * double(box.min[a]);
    big = d[a] > big ? d[a] : big;
  }
  if (!(big > 0.0)) {
    return 0.0;
  }
  for (double &x : d) {
    x /= big;
  }
  return big * std::sqrt(d[0] * d[1] + d[1] * d[2] + d[2] * d[0]);
}

/* A node counts as at most this much larger: one that covers the scene now is bad, but
 * how far away the thing is that made it so doesn't matter. */
constexpr double kMaxGrowth = 8.0;
/* Nodes smaller than this, relative to their parents, count as this large: points and
 * flat boxes. */
constexpr double kSmallNode = 1e-6;

} // namespace

template <class S> Vector<double> nodeSizes(const Vector<BvhNode<S>> &nodes)
{
  Vector<double> sizes;
  sizes.resize(nodes.size());
  for (int i = 0; i < int(nodes.size()); i++) {
    sizes[i] = sizeOf(nodes[i]);
  }
  return sizes;
}

template <class S>
double treeGrowth(const Vector<BvhNode<S>> &nodes, const Vector<double> &built)
{
  if (nodes.size() == 0) {
    return 1.0;
  }
  const auto term = [&](int i, int parent) {
    const double was = (std::max)(built[i], built[parent] * kSmallNode),
                 now = sizeOf(nodes[i]);
    const double grown = now <= was ? 1.0 : was > 0.0 ? now / was : kMaxGrowth;
    return (std::min)(grown * grown, kMaxGrowth);
  };
  double sum = term(0, 0);
  for (int i = 0; i < int(nodes.size()); i++) {
    if (nodes[i].count == 0) {
      sum += term(i + 1, i) + term(int(nodes[i].rightOrFirst), i);
    }
  }
  return sum / double(nodes.size());
}

template <class S> double treeCost(const Vector<BvhNode<S>> &nodes)
{
  if (nodes.size() == 0) {
    return 0.0;
  }
  const double scale = areaScale(boxOf(nodes[0]));
  const double root = finite(scale) ? scaledArea(boxOf(nodes[0]), scale) : 0.0;
  double cost = 0.0;
  for (const BvhNode<S> &n : nodes) {
    /* A root without area (one point, or a line): every node counts as if always hit. */
    const double p = root > 0.0 ? scaledArea(boxOf(n), scale) / root : 1.0;
    cost += p * (n.count > 0 ? double(n.count) : 1.0);
  }
  return cost;
}

template Status
buildTree<float>(Vector<PrimBox<float>> &, const BvhOptions &, Vector<BvhNode<float>> &);
template Status buildTree<double>(Vector<PrimBox<double>> &,
                                  const BvhOptions &,
                                  Vector<BvhNode<double>> &);
template double treeCost<float>(const Vector<BvhNode<float>> &);
template double treeCost<double>(const Vector<BvhNode<double>> &);
template Vector<double> nodeSizes<float>(const Vector<BvhNode<float>> &);
template Vector<double> nodeSizes<double>(const Vector<BvhNode<double>> &);
template double treeGrowth<float>(const Vector<BvhNode<float>> &, const Vector<double> &);
template double treeGrowth<double>(const Vector<BvhNode<double>> &,
                                   const Vector<double> &);

} // namespace coredcc::bvh
