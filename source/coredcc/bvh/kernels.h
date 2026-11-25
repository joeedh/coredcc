#pragma once

/* The geometric tests behind the BVH queries, in double. Hits use RayQuery's scaled t. */

#include "coredcc/bvh/ray.h"

#include <cstdint>

namespace coredcc::bvh {

/** How many triangles rayTriangles() tests at once. */
constexpr int kTriangleBlock = 16;

struct TriangleBlockHits {
  double t[kTriangleBlock], u[kTriangleBlock], v[kTriangleBlock];
};

/**
 * Tests `count <= kTriangleBlock` triangles from index `first`. `coords` are nine arrays:
 * the x, y and z of corner a, then b, then c. Bit k of the result is set when triangle
 * `first + k` is hit at a t between q.tMin and `limit`.
 *
 * Watertight (Woop, Benthin and Wald): a ray through an edge or a vertex shared by
 * triangles hits at least one of them, so it can't leak through a closed mesh. Zero-area
 * triangles are never hit.
 */
template <class S>
uint32_t rayTriangles(const RayQuery &q,
                      const S *const coords[9],
                      int first,
                      int count,
                      double limit,
                      TriangleBlockHits &out);

/** The ray's entry into a solid sphere, capsule or box: its t, and whether it starts
 * inside (t is then q.tMin). `limit` is the largest t that counts. */
bool raySphere(
    const RayQuery &q, const double3 &c, double r, double limit, double &t, bool &inside);
bool rayCapsule(const RayQuery &q,
                const double3 &a,
                const double3 &b,
                double r,
                double limit,
                double &t,
                bool &inside);
bool rayBox(const RayQuery &q, const Box3d &box, double limit, double &t, bool &inside);

/** The closest point of a segment or triangle to `p`. Degenerate ones are segments or
 * points. */
double3 closestOnSegment(const double3 &p, const double3 &a, const double3 &b);
double3
closestOnTriangle(const double3 &p, const double3 &a, const double3 &b, const double3 &c);

/** The same for solids: `p` itself when it is inside. */
double3 closestOnBox(const double3 &p, const Box3d &box);
double3 closestOnSphere(const double3 &p, const double3 &c, double r);

/**
 * Whether a point set is strictly outside the half-space `n.p + w >= 0`. `best` is the
 * largest `n.p + w` over the set as computed, and `size` at least the sum of its terms'
 * magnitudes. Rounding can only make the answer false.
 */
inline bool outsidePlane(double best, double size)
{
  return best < -size * (8.0 * std::numeric_limits<double>::epsilon());
}

/** Whether a box is certainly outside the half-space; see outsidePlane(). */
template <class S> bool boxOutside(const S *lo, const S *hi, const double *plane)
{
  double best = plane[3], size = std::fabs(plane[3]);
  for (int a = 0; a < 3; a++) {
    const double x = plane[a] * double(lo[a]), y = plane[a] * double(hi[a]);
    best += x > y ? x : y;
    const double ax = std::fabs(x), ay = std::fabs(y);
    size += ax > ay ? ax : ay;
  }
  return outsidePlane(best, size);
}

} // namespace coredcc::bvh
