#pragma once

#include "coredcc/base/assert.h"
#include "coredcc/base/status.h"
#include "coredcc/bvh/box.h"
#include "coredcc/bvh/parallel.h"

#include "litestl/util/index_range.h"
#include "litestl/util/vector.h"

#include <algorithm>
#include <atomic>
#include <concepts>
#include <cstdint>
#include <limits>
#include <span>
#include <utility>

namespace coredcc {

/** Nodes are in depth-first order: an inner node's left child is the next node. */
template <class S> struct BvhNode {
  S min[3];
  /** Inner node: the right child. Leaf: its first position in order(). */
  uint32_t rightOrFirst;
  S max[3];
  /** The number of primitives of a leaf; 0 for an inner node. */
  uint32_t count;
};
static_assert(sizeof(BvhNode<float>) == 32);

/**
 * What a Bvh is built over: primitives with bounds. An empty or non-finite box leaves the
 * primitive out. bounds() is called from worker threads when the build is parallel. At most
 * 2^31 - 1 primitives.
 */
template <class T>
concept BvhSource = requires(const T &s, int i) {
  typename T::Scalar;
  { s.size() } -> std::integral;
  { s.bounds(i) } -> std::same_as<Box3<typename T::Scalar>>;
};

struct BvhOptions {
  /** The most primitives in a leaf, at least 1. The default is measured: docs/bvh-leafsize.md. */
  int leafSize = 8;
  /** Candidate split planes per axis, 2 to 64. */
  int bins = 16;
  /** Use litestl's task pool. The result is the same either way, for any worker count. */
  bool parallel = true;
};

namespace bvh {

/** A traversal callback's answer for a node to skip. */
constexpr double kSkip = (std::numeric_limits<double>::max)();

/** The build stops using the SAH at this depth and halves what is left. */
constexpr int kMaxSahDepth = 48;
/** More than the deepest tree: kMaxSahDepth, then 31 halvings. */
constexpr int kStackSize = 96;

template <class S> struct PrimBox {
  Box3<S> box;
  int id;
};

/**
 * Builds the tree over `prims` (all finite, none empty), reordering them: leaf positions
 * index the result. Checks `opts`.
 */
template <class S>
Status buildTree(litestl::util::Vector<PrimBox<S>> &prims, const BvhOptions &opts, litestl::util::Vector<BvhNode<S>> &nodes);

/** The expected cost of a ray, relative to one box test (a primitive costs 1 too). */
template <class S> double treeCost(const litestl::util::Vector<BvhNode<S>> &nodes);

/** The square root of an eighth of each node's surface area. */
template <class S> litestl::util::Vector<double> nodeSizes(const litestl::util::Vector<BvhNode<S>> &nodes);
/** See Bvh::growth(). `built` is nodeSizes() of when they were built. */
template <class S>
double treeGrowth(const litestl::util::Vector<BvhNode<S>> &nodes,
                  const litestl::util::Vector<double> &built);

} // namespace bvh

/**
 * A bounding volume hierarchy over any BvhSource (binned SAH). It holds nodes and an
 * order, not the source: see GeometryBvh for one that owns its primitives and answers
 * queries. Queries may run on several threads, but not during build() or a refit.
 */
template <class S> class Bvh {
 public:
  using Scalar = S;
  using Node = BvhNode<S>;

  /** Replaces the tree. On an error the old one is kept. */
  template <BvhSource Src>
    requires std::same_as<typename Src::Scalar, S>
  Status build(const Src &src, BvhOptions opts = {})
  {
    const auto size = src.size();
    if (std::cmp_less(size, 0)) {
      return Status::error(StatusCode::InvalidArgument, "Bvh: a source of negative size");
    }
    if (std::cmp_greater(size, (std::numeric_limits<int>::max)())) {
      return Status::error(StatusCode::OutOfRange, "Bvh: more than 2^31 - 1 primitives");
    }
    const int n = int(size);
    litestl::util::Vector<bvh::PrimBox<S>> prims;
    prims.resize(size_t(n));
    bvh::forChunks(n, kChunk, opts.parallel, [&](litestl::util::IndexRange r) {
      for (int i : r) {
        prims[i].box = src.bounds(i);
        prims[i].id = i;
      }
    });
    litestl::util::Vector<int> leftOut;
    int kept = 0;
    for (int i = 0; i < n; i++) {
      if (usable(prims[i].box)) {
        prims[kept++] = prims[i];
      } else {
        leftOut.append(i);
      }
    }
    prims.resize(size_t(kept));

    litestl::util::Vector<Node> nodes;
    CDCC_TRY(bvh::buildTree(prims, opts, nodes));
    litestl::util::Vector<int> order;
    order.resize(size_t(kept));
    for (int i = 0; i < kept; i++) {
      order[i] = prims[i].id;
    }
    nodes_ = std::move(nodes);
    order_ = std::move(order);
    leftOut_ = std::move(leftOut);
    builtSizes_ = bvh::nodeSizes(nodes_);
    absent_.clear();
    parents_.clear();
    leaves_.clear();
    positions_.clear();
    stamps_.clear();
    size_ = n;
    parallel_ = opts.parallel;
    return Status();
  }

  /**
   * Takes new bounds for the same primitives, keeping the tree's shape.
   *
   * - A primitive whose box is empty or not finite now stays in the tree without bounds
   *   (present()), and can come back.
   * - FailedPrecondition, and nothing changes: for another number of primitives, or when one
   *   that the build left out is usable now.
   * - The tree gets slower as primitives move apart: see growth().
   */
  template <BvhSource Src>
    requires std::same_as<typename Src::Scalar, S>
  Status refit(const Src &src)
  {
    if (std::cmp_not_equal(src.size(), size_)) {
      return Status::error(StatusCode::FailedPrecondition,
                           "Bvh: refit with another number of primitives");
    }
    for (int i : leftOut_) {
      if (usable(src.bounds(i))) {
        return leftOutCame();
      }
    }
    const int count = int(order_.size());
    std::atomic<bool> anyAbsent{false};
    const int perNode = nodes_.size() > 0 ? count / int(nodes_.size()) + 1 : 1;
    bvh::forChunks(
        int(nodes_.size()), kChunk / perNode + 1, parallel_, [&](litestl::util::IndexRange r) {
          bool absent = false;
          for (int i : r) {
            absent = fitLeaf(nodes_[i], [&](int k) { return src.bounds(order_[k]); }) || absent;
          }
          if (absent) {
            anyAbsent = true;
          }
        });
    /* Children come after their parent, so backwards is bottom up. */
    for (int i = int(nodes_.size()) - 1; i >= 0; i--) {
      fitInner(i);
    }
    absent_.clear();
    if (anyAbsent) {
      absent_.resize(size_t(count));
      bvh::forChunks(count, kChunk, parallel_, [&](litestl::util::IndexRange r) {
        for (int k : r) {
          absent_[k] = usable(src.bounds(order_[k])) ? 0 : 1;
        }
      });
    }
    return Status();
  }

  /**
   * Takes new bounds of a few primitives, given by their positions in order(). `bounds(int) ->
   * Box3<S>` gives those of any position, and is called from workers. The time and the memory are
   * those of the leaves concerned. The first call also makes a table of parents.
   */
  template <class Bounds> void refitPositions(std::span<const int> positions, Bounds &&bounds)
  {
    if (positions.empty()) {
      return;
    }
    prepare();
    if (++stamp_ == 0) {
      stamps_.clear();
      stamps_.resize(nodes_.size());
      stamp_ = 1;
    }
    litestl::util::Vector<uint32_t> leaves, inner;
    for (int position : positions) {
      const bool absent = !usable(bounds(position));
      if (absent && absent_.size() == 0) {
        absent_.resize(order_.size());
        for (uint8_t &a : absent_) {
          a = 0;
        }
      }
      if (absent_.size() > 0) {
        absent_[position] = absent ? 1 : 0;
      }
      const uint32_t leaf = leafOf(position);
      if (stamps_[leaf] == stamp_) {
        continue;
      }
      stamps_[leaf] = stamp_;
      leaves.append(leaf);
      for (uint32_t i = leaf; i != 0;) {
        i = parents_[i];
        if (stamps_[i] == stamp_) {
          break;
        }
        stamps_[i] = stamp_;
        inner.append(i);
      }
    }
    const int perLeaf = int(order_.size() / leaves_.size()) + 1;
    bvh::forChunks(int(leaves.size()),
                   kChunk / perLeaf + 1,
                   parallel_,
                   [&](litestl::util::IndexRange r) {
                     for (int i : r) {
                       fitLeaf(nodes_[leaves[i]], bounds);
                     }
                   });
    std::sort(inner.data(), inner.data() + inner.size(), [](uint32_t a, uint32_t b) {
      return a > b;
    });
    for (uint32_t i : inner) {
      fitInner(int(i));
    }
  }

  /** A primitive's position in order(), or -1 if the build left it out. The first call makes a
   * table. */
  int position(int primitive)
  {
    if (positions_.size() == 0 && size_ > 0) {
      positions_.resize(size_t(size_));
      for (int &p : positions_) {
        p = -1;
      }
      for (int k = 0; k < int(order_.size()); k++) {
        positions_[order_[k]] = k;
      }
    }
    return positions_[primitive];
  }

  /** The source's size at the last build. */
  int size() const
  {
    return size_;
  }
  /** How many primitives the build left out: empty or non-finite bounds. */
  int skipped() const
  {
    return int(leftOut_.size());
  }
  /** The source's primitive at each leaf position. */
  const litestl::util::Vector<int> &order() const
  {
    return order_;
  }
  /** Whether the primitive at a leaf position has bounds. One that hasn't, since a refit, must be
   * ignored by whoever traverses the tree. */
  bool present(int position) const
  {
    return absent_.size() == 0 || absent_[position] == 0;
  }
  const litestl::util::Vector<Node> &nodes() const
  {
    return nodes_;
  }
  Box3<S> bounds() const
  {
    return nodes_.size() == 0 ? Box3<S>() : boxOf(nodes_[0]);
  }
  /** The SAH cost of the tree, for a ray anywhere. 0 when empty. For comparing trees over the same
   * boxes: when one box is huge, everything else hardly counts. */
  double cost() const
  {
    return bvh::treeCost(nodes_);
  }
  /**
   * How much looser refits have made the tree: the mean over its nodes of their surface area
   * divided by what it was when the tree was built, each counted as 1 to 8. 1 after a build.
   * Rebuild when it is about 2. It takes a pass over the nodes.
   */
  double growth() const
  {
    return bvh::treeGrowth(nodes_, builtSizes_);
  }

  /**
   * Visits leaves in no particular order. `test(node) -> bool` says whether to enter a
   * node; `leaf(first, count) -> bool` gets positions in order(), and false stops.
   */
  template <class NodeTest, class Leaf> void traverse(NodeTest &&test, Leaf &&leaf) const
  {
    if (nodes_.size() == 0) {
      return;
    }
    uint32_t stack[bvh::kStackSize];
    int top = 0;
    stack[top++] = 0;
    while (top > 0) {
      const uint32_t i = stack[--top];
      const Node &n = nodes_[i];
      if (!test(n)) {
        continue;
      }
      if (n.count > 0) {
        if (!leaf(int(n.rightOrFirst), int(n.count))) {
          return;
        }
        continue;
      }
      CDCC_ASSERT(top + 2 <= bvh::kStackSize, "Bvh: deeper than the build allows");
      stack[top++] = n.rightOrFirst;
      stack[top++] = i + 1;
    }
  }

  /**
   * Visits leaves nearest first. `key(node) -> double` is a lower bound for everything in
   * the node, or bvh::kSkip. `limit() -> double` is read again before each node: those
   * with a larger key are skipped. `leaf` is as in traverse().
   */
  template <class Key, class Limit, class Leaf>
  void traverseOrdered(Key &&key, Limit &&limit, Leaf &&leaf) const
  {
    if (nodes_.size() == 0) {
      return;
    }
    struct Entry {
      uint32_t node;
      double key;
    };
    Entry stack[bvh::kStackSize];
    int top = 0;
    stack[top++] = {0, key(nodes_[0])};
    while (top > 0) {
      const Entry e = stack[--top];
      if (e.key == bvh::kSkip || e.key > limit()) {
        continue;
      }
      const Node &n = nodes_[e.node];
      if (n.count > 0) {
        if (!leaf(int(n.rightOrFirst), int(n.count))) {
          return;
        }
        continue;
      }
      CDCC_ASSERT(top + 2 <= bvh::kStackSize, "Bvh: deeper than the build allows");
      Entry l{e.node + 1, key(nodes_[e.node + 1])};
      Entry r{n.rightOrFirst, key(nodes_[n.rightOrFirst])};
      const bool leftFirst = l.key <= r.key;
      stack[top++] = leftFirst ? r : l;
      stack[top++] = leftFirst ? l : r;
    }
  }

 private:
  static constexpr int kChunk = 1 << 14;

  static bool usable(const Box3<S> &b)
  {
    return b.allFinite() && !b.empty();
  }
  static Box3<S> boxOf(const Node &n)
  {
    Box3<S> b;
    for (int a = 0; a < 3; a++) {
      b.min[a] = n.min[a];
      b.max[a] = n.max[a];
    }
    return b;
  }
  static void setBox(Node &n, const Box3<S> &box)
  {
    for (int a = 0; a < 3; a++) {
      n.min[a] = box.min[a];
      n.max[a] = box.max[a];
    }
  }
  static Status leftOutCame()
  {
    return Status::error(StatusCode::FailedPrecondition,
                         "Bvh: refit with a primitive that the build left out");
  }

  /* Whether a primitive has no bounds. Nothing for an inner node. */
  template <class Bounds> static bool fitLeaf(Node &n, Bounds &&bounds)
  {
    if (n.count == 0) {
      return false;
    }
    Box3<S> box;
    bool absent = false;
    for (uint32_t k = 0; k < n.count; k++) {
      const Box3<S> b = bounds(int(n.rightOrFirst + k));
      if (usable(b)) {
        box.include(b);
      } else {
        absent = true;
      }
    }
    setBox(n, box);
    return absent;
  }
  void fitInner(int i)
  {
    Node &n = nodes_[i];
    if (n.count == 0) {
      Box3<S> box = boxOf(nodes_[i + 1]);
      box.include(boxOf(nodes_[n.rightOrFirst]));
      setBox(n, box);
    }
  }

  void prepare()
  {
    if (parents_.size() == nodes_.size()) {
      return;
    }
    parents_.resize(nodes_.size());
    stamps_.resize(nodes_.size());
    for (int i = 0; i < int(nodes_.size()); i++) {
      stamps_[i] = 0;
      const Node &n = nodes_[i];
      if (n.count > 0) {
        leaves_.append(uint32_t(i));
      } else {
        parents_[i + 1] = uint32_t(i);
        parents_[n.rightOrFirst] = uint32_t(i);
      }
    }
    stamp_ = 0;
  }
  /* Leaves are in the order of their positions. */
  uint32_t leafOf(int position) const
  {
    const uint32_t *end = leaves_.data() + leaves_.size();
    const uint32_t *after =
        std::upper_bound(leaves_.data(), end, position, [this](int p, uint32_t leaf) {
          return uint32_t(p) < nodes_[leaf].rightOrFirst;
        });
    return *(after - 1);
  }

  litestl::util::Vector<Node> nodes_;
  litestl::util::Vector<int> order_;
  litestl::util::Vector<int> leftOut_;
  litestl::util::Vector<double> builtSizes_;
  /* By position; empty if none is. */
  litestl::util::Vector<uint8_t> absent_;
  /* For partial refits, made when they are first needed. The root's parent is unused. */
  litestl::util::Vector<uint32_t> parents_, leaves_, stamps_;
  litestl::util::Vector<int> positions_;
  uint32_t stamp_ = 0;
  int size_ = 0;
  bool parallel_ = true;
};

} // namespace coredcc
