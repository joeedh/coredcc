#pragma once

/* Random primitives and trees for the BVH tests. */

#include "coredcc/bvh/geometry_bvh.h"

#include "fuzz.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace bvhtest {

using namespace coredcc;
using litestl::math::double3;

struct Rand {
  coredcc_test::SplitMix64 gen;
  explicit Rand(uint64_t seed) : gen(seed) {}
  double unit()
  {
    return double(gen.next() >> 11) * 0x1p-53;
  }
  double range(double a, double b)
  {
    return a + (b - a) * unit();
  }
  int below(int n)
  {
    return int(gen.below(uint32_t(n)));
  }
  double3 vec(double size)
  {
    const double x = range(-size, size), y = range(-size, size);
    return double3(x, y, range(-size, size));
  }
};

inline double dot(const double3 &a, const double3 &b)
{
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
inline double length(const double3 &a)
{
  return std::sqrt(dot(a, a));
}

/* Where primitives are put. */
enum class Layout {
  Uniform,
  Clustered,
  Coplanar,
  /* Every primitive the same. */
  AllEqual,
  /* Thin primitives across the whole scene. */
  Long,
  /* Centers on a line, doubling in distance. */
  Exponential,
  Count,
};

inline double3 place(Rand &r, Layout layout, int i)
{
  switch (layout) {
    case Layout::Clustered: {
      Rand c(uint64_t(i % 7) + 99);
      const double3 center = c.vec(100.0);
      return center + r.vec(0.5);
    }
    case Layout::Coplanar:
      return double3(r.range(-10, 10), r.range(-10, 10), 2.0);
    case Layout::AllEqual:
      return double3(1.0, 2.0, 3.0);
    case Layout::Exponential:
      return double3(std::ldexp(1.0, i % 30), 0.0, 0.0);
    default:
      return r.vec(10.0);
  }
}

template <class S> TriangleSource<S> triangles(Rand &r, Layout layout, int n)
{
  TriangleSource<S> src;
  Rand fixed(5);
  const double3 e1 = fixed.vec(1.0), e2 = fixed.vec(1.0);
  for (int i = 0; i < n; i++) {
    double3 a = place(r, layout, i), b, c;
    if (layout == Layout::AllEqual) {
      b = a + e1;
      c = a + e2;
    } else if (layout == Layout::Long) {
      a = r.vec(10.0);
      b = a * -1.0;
      c = b + r.vec(0.01);
    } else {
      const double size = layout == Layout::Exponential ? a[0] : 1.0;
      b = a + r.vec(0.3 * size);
      c = a + r.vec(0.3 * size);
      if (layout == Layout::Coplanar) {
        b[2] = c[2] = a[2];
      }
      if (r.below(20) == 0) {
        c = b; /* a segment */
      }
      if (r.below(40) == 0) {
        b = c = a; /* a point */
      }
    }
    src.append(a, b, c);
  }
  return src;
}

template <class S> CapsuleSource<S> capsules(Rand &r, Layout layout, int n)
{
  CapsuleSource<S> src;
  for (int i = 0; i < n; i++) {
    const double3 a = place(r, layout, i);
    double3 b = layout == Layout::AllEqual ? a + double3(1, 0, 0)
                : layout == Layout::Long   ? r.vec(10.0)
                                           : a + r.vec(0.5);
    if (r.below(10) == 0) {
      b = a;
    }
    const bool thin = r.below(10) == 0;
    src.append(a, b, layout == Layout::AllEqual ? 0.2 : thin ? 0.0 : r.range(0.0, 0.2));
  }
  return src;
}

template <class S> SphereSource<S> spheres(Rand &r, Layout layout, int n)
{
  SphereSource<S> src;
  for (int i = 0; i < n; i++) {
    src.append(place(r, layout, i), layout == Layout::AllEqual ? 0.5 : r.range(0.0, 0.4));
  }
  return src;
}

inline BoxSource boxes(Rand &r, Layout layout, int n)
{
  BoxSource src;
  for (int i = 0; i < n; i++) {
    const double3 p = place(r, layout, i);
    Box3d b;
    b.include(p[0], p[1], p[2]);
    const double3 q = layout == Layout::AllEqual ? p + double3(1, 1, 1) : p + r.vec(0.5);
    b.include(q[0], q[1], q[2]);
    src.append(b);
  }
  return src;
}

/** Problems with the tree over `src`, or null. */
template <class S, class Src>
const char *treeProblem(const Bvh<S> &bvh, const Src &src, int leafSize)
{
  const auto &nodes = bvh.nodes();
  const auto &order = bvh.order();
  std::vector<int> seen(size_t(src.size()), 0);
  for (int id : order) {
    if (id < 0 || id >= src.size() || seen[size_t(id)]++) {
      return "order isn't a subset permutation";
    }
    const Box3<S> b = src.bounds(id);
    if (b.empty() || !b.allFinite()) {
      return "an unusable primitive is in the tree";
    }
  }
  for (int i = 0; i < src.size(); i++) {
    const Box3<S> b = src.bounds(i);
    if (!seen[size_t(i)] && !b.empty() && b.allFinite()) {
      return "a usable primitive is missing";
    }
  }
  if (order.size() == 0) {
    return nodes.size() == 0 ? nullptr : "nodes without primitives";
  }
  const auto contains = [](const BvhNode<S> &n, const S *lo, const S *hi) {
    for (int a = 0; a < 3; a++) {
      if (lo[a] < n.min[a] || hi[a] > n.max[a]) {
        return false;
      }
    }
    return true;
  };
  struct Item {
    uint32_t node;
    int depth;
  };
  std::vector<Item> stack{{0, 0}};
  uint32_t nextNode = 0, nextPrim = 0;
  while (!stack.empty()) {
    const Item it = stack.back();
    stack.pop_back();
    if (it.node != nextNode++) {
      return "nodes aren't in depth-first order";
    }
    if (it.depth >= bvh::kStackSize - 1) {
      return "too deep";
    }
    const BvhNode<S> &n = nodes[int(it.node)];
    if (n.count > 0) {
      if (int(n.count) > leafSize || n.rightOrFirst != nextPrim) {
        return "a bad leaf";
      }
      nextPrim += n.count;
      for (uint32_t k = 0; k < n.count; k++) {
        const Box3<S> b = src.bounds(order[int(n.rightOrFirst + k)]);
        if (!contains(n, b.min, b.max)) {
          return "a leaf doesn't contain its primitives";
        }
      }
      continue;
    }
    for (uint32_t child : {it.node + 1, n.rightOrFirst}) {
      if (child <= it.node || child >= nodes.size()) {
        return "a bad child";
      }
      const BvhNode<S> &c = nodes[int(child)];
      if (!contains(n, c.min, c.max)) {
        return "a node doesn't contain its child";
      }
    }
    stack.push_back({n.rightOrFirst, it.depth + 1});
    stack.push_back({it.node + 1, it.depth + 1});
  }
  return nextNode == nodes.size() && nextPrim == order.size() ? nullptr : "unreachable nodes";
}

/** Whether two trees are the same, bit for bit. */
template <class S> bool sameTree(const Bvh<S> &a, const Bvh<S> &b)
{
  return a.nodes().size() == b.nodes().size() && a.order().size() == b.order().size() &&
         memcmp(a.nodes().data(), b.nodes().data(), a.nodes().size() * sizeof(BvhNode<S>)) == 0 &&
         memcmp(a.order().data(), b.order().data(), a.order().size() * sizeof(int)) == 0;
}

} // namespace bvhtest
