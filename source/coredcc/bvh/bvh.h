#pragma once

#include "coredcc/base/assert.h"
#include "coredcc/base/status.h"
#include "coredcc/bvh/box.h"
#include "coredcc/bvh/parallel.h"

#include "litestl/util/index_range.h"
#include "litestl/util/vector.h"

#include <concepts>
#include <cstdint>
#include <limits>

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
 * primitive out. bounds() is called from worker threads when the build is parallel.
 */
template <class T>
concept BvhSource = requires(const T &s, int i) {
  typename T::Scalar;
  { s.size() } -> std::convertible_to<int>;
  { s.bounds(i) } -> std::same_as<Box3<typename T::Scalar>>;
};

struct BvhOptions {
  /** The most primitives in a leaf, at least 1. */
  int leafSize = 128;
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

} // namespace bvh

/**
 * A bounding volume hierarchy over any BvhSource (binned SAH). It holds nodes and an
 * order, not the source: see GeometryBvh for one that owns its primitives and answers
 * queries. Immutable between build() and refit(); queries may run on several threads.
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
    const int n = int(src.size());
    if (n < 0) {
      return Status::error(StatusCode::InvalidArgument, "Bvh: a source of negative size");
    }
    litestl::util::Vector<bvh::PrimBox<S>> prims;
    prims.resize(size_t(n));
    bvh::forChunks(n, kChunk, opts.parallel, [&](litestl::util::IndexRange r) {
      for (int i : r) {
        prims[i].box = src.bounds(i);
        prims[i].id = i;
      }
    });
    int kept = 0;
    for (int i = 0; i < n; i++) {
      if (usable(prims[i].box)) {
        prims[kept++] = prims[i];
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
    size_ = n;
    parallel_ = opts.parallel;
    return Status();
  }

  /**
   * Takes new bounds for the same primitives, keeping the tree's shape. It must be the
   * same number of primitives, with the same ones left out (FailedPrecondition; nothing
   * changes). The tree gets slower as primitives move apart: see cost().
   */
  template <BvhSource Src>
    requires std::same_as<typename Src::Scalar, S>
  Status refit(const Src &src)
  {
    if (int(src.size()) != size_) {
      return Status::error(StatusCode::FailedPrecondition,
                           "Bvh: refit with another number of primitives");
    }
    litestl::util::Vector<Box3<S>> leaves;
    leaves.resize(size_t(order_.size()));
    bvh::forChunks(int(order_.size()), kChunk, parallel_, [&](litestl::util::IndexRange r) {
      for (int i : r) {
        leaves[i] = src.bounds(order_[i]);
      }
    });
    for (const Box3<S> &b : leaves) {
      if (!usable(b)) {
        return skippedChanged();
      }
    }
    if (skipped() > 0) {
      litestl::util::Vector<uint8_t> inTree;
      inTree.resize(size_t(size_));
      for (int i = 0; i < size_; i++) {
        inTree[i] = 0;
      }
      for (int i : order_) {
        inTree[i] = 1;
      }
      for (int i = 0; i < size_; i++) {
        if (!inTree[i] && usable(src.bounds(i))) {
          return skippedChanged();
        }
      }
    }
    /* Children come after their parent, so backwards is bottom up. */
    for (int i = int(nodes_.size()) - 1; i >= 0; i--) {
      Node &n = nodes_[i];
      Box3<S> box;
      if (n.count > 0) {
        for (uint32_t k = 0; k < n.count; k++) {
          box.include(leaves[n.rightOrFirst + k]);
        }
      } else {
        box.include(boxOf(nodes_[i + 1]));
        box.include(boxOf(nodes_[n.rightOrFirst]));
      }
      for (int a = 0; a < 3; a++) {
        n.min[a] = box.min[a];
        n.max[a] = box.max[a];
      }
    }
    return Status();
  }

  /** The source's size at the last build. */
  int size() const
  {
    return size_;
  }
  /** How many primitives were left out: empty or non-finite bounds. */
  int skipped() const
  {
    return size_ - int(order_.size());
  }
  /** The source's primitive at each leaf position. */
  const litestl::util::Vector<int> &order() const
  {
    return order_;
  }
  const litestl::util::Vector<Node> &nodes() const
  {
    return nodes_;
  }
  Box3<S> bounds() const
  {
    return nodes_.size() == 0 ? Box3<S>() : boxOf(nodes_[0]);
  }
  /** The SAH cost of the tree; grows as refits loosen it. 0 when empty. */
  double cost() const
  {
    return bvh::treeCost(nodes_);
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
  static Status skippedChanged()
  {
    return Status::error(StatusCode::FailedPrecondition,
                         "Bvh: refit with other primitives left out");
  }

  litestl::util::Vector<Node> nodes_;
  litestl::util::Vector<int> order_;
  int size_ = 0;
  bool parallel_ = true;
};

} // namespace coredcc
