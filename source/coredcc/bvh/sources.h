#pragma once

#include "coredcc/bvh/box.h"
#include "coredcc/bvh/kernels.h"
#include "coredcc/bvh/ray.h"

#include "litestl/util/vector.h"

#include <cstdint>
#include <utility>

/*
 * The built-in primitive sources: triangles, capsules, spheres and boxes, each a structure
 * of arrays. GeometryBvh stores one in leaf order. Every source has:
 *
 *   using Scalar;  int size();  Box3<Scalar> bounds(i);
 *   Source permuted(order);                        // a copy: element k is order[k]
 *   bool rayRange(q, first, count, limit, hit);    // see GeometryBvh
 *   double3 closest(i, p);                         // the point of primitive i nearest to p
 *   bool outside(i, plane);                        // certainly outside `n.p + w >= 0`
 *
 * Coordinates are object space, radii object units. A primitive whose bounds aren't
 * finite, or whose radius is negative or NaN, has an unusable box, so it is left out.
 */
namespace coredcc {

namespace bvh {

/** One growing array per coordinate. */
template <class S, int N> struct Columns {
  litestl::util::Vector<S> col[N];

  int size() const
  {
    return int(col[0].size());
  }
  void append(const double (&row)[N])
  {
    for (int c = 0; c < N; c++) {
      col[c].append(S(row[c]));
    }
  }
  Columns permuted(const litestl::util::Vector<int> &order) const
  {
    Columns r;
    for (int c = 0; c < N; c++) {
      r.col[c].resize(order.size());
      for (int k = 0; k < int(order.size()); k++) {
        r.col[c][k] = col[c][order[k]];
      }
    }
    return r;
  }
  double3 point(int firstColumn, int i) const
  {
    return double3(double(col[firstColumn][i]),
                   double(col[firstColumn + 1][i]),
                   double(col[firstColumn + 2][i]));
  }
  void ensureCapacity(int n)
  {
    for (int c = 0; c < N; c++) {
      col[c].ensure_capacity(size_t(n));
    }
  }
};

/** An unusable box: the primitive is left out of the tree. */
template <class S> Box3<S> noBox()
{
  return Box3<S>();
}

/** A box around the spheres of radius r at two points. */
template <class S> Box3<S> boxAround(const double3 &a, const double3 &b, double r)
{
  if (!(r >= 0.0) || !finite(r)) {
    return noBox<S>();
  }
  Box3<S> box;
  for (int i = 0; i < 3; i++) {
    const double lo = a[i] < b[i] ? a[i] : b[i], hi = a[i] < b[i] ? b[i] : a[i];
    if (!finite(a[i]) || !finite(b[i])) {
      return noBox<S>();
    }
    /* Without a radius the bounds are exact, and stay so. */
    box.min[i] = r > 0.0 ? sumDown<S>(lo - r) : S(lo);
    box.max[i] = r > 0.0 ? sumUp<S>(hi + r) : S(hi);
  }
  return box;
}

/** Whether the ball of radius r around p is certainly outside the plane. */
inline bool ballOutside(const double3 &p, double r, const double *plane)
{
  double best = plane[3], size = std::fabs(plane[3]), normal = 0.0;
  for (int a = 0; a < 3; a++) {
    const double term = plane[a] * p[a];
    best += term;
    size += std::fabs(term);
    normal += plane[a] * plane[a];
  }
  const double reach = r * std::sqrt(normal);
  return outsidePlane(best + reach, size + reach);
}

} // namespace bvh

/** Triangles. One with no area can't be hit by a ray; to closest() it is a segment or a
 * point. */
template <class S> class TriangleSource {
 public:
  using Scalar = S;

  void append(const double3 &a, const double3 &b, const double3 &c)
  {
    c_.append({a[0], a[1], a[2], b[0], b[1], b[2], c[0], c[1], c[2]});
  }
  void ensureCapacity(int n)
  {
    c_.ensureCapacity(n);
  }
  int size() const
  {
    return c_.size();
  }
  double3 corner(int i, int which) const
  {
    return c_.point(which * 3, i);
  }

  Box3<S> bounds(int i) const
  {
    Box3<S> b;
    for (int k = 0; k < 9; k++) {
      if (!bvh::finite(c_.col[k][i])) {
        return bvh::noBox<S>();
      }
    }
    for (int k = 0; k < 9; k += 3) {
      b.include(c_.col[k][i], c_.col[k + 1][i], c_.col[k + 2][i]);
    }
    return b;
  }
  TriangleSource permuted(const litestl::util::Vector<int> &order) const
  {
    TriangleSource r;
    r.c_ = c_.permuted(order);
    return r;
  }

  template <class Hit>
  bool rayRange(const bvh::RayQuery &q, int first, int count, double &limit, Hit &&hit) const
  {
    const S *coords[9];
    for (int k = 0; k < 9; k++) {
      coords[k] = c_.col[k].data();
    }
    bvh::TriangleBlockHits block;
    for (int done = 0; done < count; done += bvh::kTriangleBlock) {
      const int n = count - done < bvh::kTriangleBlock ? count - done : bvh::kTriangleBlock;
      uint32_t mask = bvh::rayTriangles(q, coords, first + done, n, limit, block);
      for (int k = 0; mask != 0; k++, mask >>= 1) {
        if ((mask & 1) && block.t[k] <= limit &&
            !hit(first + done + k, block.t[k], block.u[k], block.v[k], false))
        {
          return false;
        }
      }
    }
    return true;
  }
  double3 closest(int i, const double3 &p) const
  {
    return bvh::closestOnTriangle(p, corner(i, 0), corner(i, 1), corner(i, 2));
  }
  bool outside(int i, const double *plane) const
  {
    return bvh::ballOutside(corner(i, 0), 0.0, plane) &&
           bvh::ballOutside(corner(i, 1), 0.0, plane) &&
           bvh::ballOutside(corner(i, 2), 0.0, plane);
  }

 private:
  bvh::Columns<S, 9> c_;
};

/** Solid capsules: the points within `radius` of a segment. A zero-length one is a sphere. */
template <class S> class CapsuleSource {
 public:
  using Scalar = S;

  void append(const double3 &a, const double3 &b, double radius)
  {
    c_.append({a[0], a[1], a[2], b[0], b[1], b[2], radius});
  }
  void ensureCapacity(int n)
  {
    c_.ensureCapacity(n);
  }
  int size() const
  {
    return c_.size();
  }
  double3 end(int i, int which) const
  {
    return c_.point(which * 3, i);
  }
  double radius(int i) const
  {
    return double(c_.col[6][i]);
  }

  Box3<S> bounds(int i) const
  {
    return bvh::boxAround<S>(end(i, 0), end(i, 1), radius(i));
  }
  CapsuleSource permuted(const litestl::util::Vector<int> &order) const
  {
    CapsuleSource r;
    r.c_ = c_.permuted(order);
    return r;
  }

  template <class Hit>
  bool rayRange(const bvh::RayQuery &q, int first, int count, double &limit, Hit &&hit) const
  {
    for (int i = first; i < first + count; i++) {
      double t;
      bool inside;
      if (bvh::rayCapsule(q, end(i, 0), end(i, 1), radius(i), limit, t, inside) &&
          !hit(i, t, 0.0, 0.0, inside))
      {
        return false;
      }
    }
    return true;
  }
  double3 closest(int i, const double3 &p) const
  {
    const double3 c = bvh::closestOnSegment(p, end(i, 0), end(i, 1));
    return bvh::closestOnSphere(p, c, radius(i));
  }
  bool outside(int i, const double *plane) const
  {
    return bvh::ballOutside(end(i, 0), radius(i), plane) &&
           bvh::ballOutside(end(i, 1), radius(i), plane);
  }

 private:
  bvh::Columns<S, 7> c_;
};

/** Solid spheres. */
template <class S> class SphereSource {
 public:
  using Scalar = S;

  void append(const double3 &center, double radius)
  {
    c_.append({center[0], center[1], center[2], radius});
  }
  void ensureCapacity(int n)
  {
    c_.ensureCapacity(n);
  }
  int size() const
  {
    return c_.size();
  }
  double3 center(int i) const
  {
    return c_.point(0, i);
  }
  double radius(int i) const
  {
    return double(c_.col[3][i]);
  }

  Box3<S> bounds(int i) const
  {
    return bvh::boxAround<S>(center(i), center(i), radius(i));
  }
  SphereSource permuted(const litestl::util::Vector<int> &order) const
  {
    SphereSource r;
    r.c_ = c_.permuted(order);
    return r;
  }

  template <class Hit>
  bool rayRange(const bvh::RayQuery &q, int first, int count, double &limit, Hit &&hit) const
  {
    for (int i = first; i < first + count; i++) {
      double t;
      bool inside;
      if (bvh::raySphere(q, center(i), radius(i), limit, t, inside) &&
          !hit(i, t, 0.0, 0.0, inside))
      {
        return false;
      }
    }
    return true;
  }
  double3 closest(int i, const double3 &p) const
  {
    return bvh::closestOnSphere(p, center(i), radius(i));
  }
  bool outside(int i, const double *plane) const
  {
    return bvh::ballOutside(center(i), radius(i), plane);
  }

 private:
  bvh::Columns<S, 4> c_;
};

/** Solid boxes, in double. */
class BoxSource {
 public:
  using Scalar = double;

  void append(const Box3d &box)
  {
    boxes_.append(box);
  }
  void ensureCapacity(int n)
  {
    boxes_.ensure_capacity(size_t(n));
  }
  int size() const
  {
    return int(boxes_.size());
  }
  const Box3d &box(int i) const
  {
    return boxes_[i];
  }

  Box3d bounds(int i) const
  {
    return boxes_[i];
  }
  BoxSource permuted(const litestl::util::Vector<int> &order) const
  {
    BoxSource r;
    r.boxes_.resize(order.size());
    for (int k = 0; k < int(order.size()); k++) {
      r.boxes_[k] = boxes_[order[k]];
    }
    return r;
  }

  template <class Hit>
  bool rayRange(const bvh::RayQuery &q, int first, int count, double &limit, Hit &&hit) const
  {
    for (int i = first; i < first + count; i++) {
      double t;
      bool inside;
      if (bvh::rayBox(q, boxes_[i], limit, t, inside) && !hit(i, t, 0.0, 0.0, inside)) {
        return false;
      }
    }
    return true;
  }
  double3 closest(int i, const double3 &p) const
  {
    return bvh::closestOnBox(p, boxes_[i]);
  }
  bool outside(int i, const double *plane) const
  {
    return bvh::boxOutside(boxes_[i].min, boxes_[i].max, plane);
  }

 private:
  litestl::util::Vector<Box3d> boxes_;
};

} // namespace coredcc
