#pragma once

#include "coredcc/base/invalidate.h"
#include "coredcc/bvh/bvh.h"
#include "coredcc/bvh/kernels.h"
#include "coredcc/bvh/sources.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <type_traits>
#include <utility>

namespace coredcc {

struct NearestHit {
  /** The source's primitive, or -1 when nothing is within the radius. */
  int primitive = -1;
  double distance = 0.0;
  double3 point;

  explicit operator bool() const
  {
    return primitive >= 0;
  }
  static litestl::binding::types::Struct<NearestHit> *defineBindings();
};

namespace bvh {
/** The filter of queries that takes everything. */
struct AcceptAll {
  template <class T> constexpr bool operator()(const T &) const
  {
    return true;
  }
};
} // namespace bvh

/**
 * A Bvh with a leaf-ordered copy of its source's primitives, and the queries. Everything
 * is in the source's space and in double. Primitives are named by their index in the
 * source. Queries are const and may run on several threads, but not during build() or a refit.
 *
 * - Ties go to the lowest primitive, so answers don't depend on the tree.
 * - Bad input (a ray that isn't valid, a point or a plane that isn't finite, a radius that isn't a
 *   number of at least 0) finds nothing. Queries that call back for each primitive also say so:
 *   InvalidArgument.
 */
template <GeometrySource Source> class GeometryBvh {
 public:
  using Scalar = typename Source::Scalar;
  using Tree = Bvh<Scalar>;

  /** Replaces the geometry, with a copy. On an error the old one is kept. */
  Status build(const Source &src, BvhOptions opts = {})
  {
    CDCC_TRY(bvh_.build(src, opts));
    prims_.gather(src, bvh_.order(), opts.parallel);
    opts_ = opts;
    return Status();
  }
  /** The same without a second copy: `src` is taken. On an error it isn't. */
  Status build(Source &&src, BvhOptions opts = {})
  {
    CDCC_TRY(bvh_.build(src, opts));
    prims_ = std::move(src);
    prims_.reorder(bvh_.order(), opts.parallel);
    opts_ = opts;
    return Status();
  }

  /** Takes the same primitives at other positions, in place; see Bvh::refit. */
  Status refit(const Source &src)
  {
    CDCC_TRY(bvh_.refit(src));
    prims_.gather(src, bvh_.order(), opts_.parallel);
    return Status();
  }

  /**
   * Takes some primitives at other positions: `primitives[k]` is now element k of `moved`, which
   * has as many. The time is that of the leaves they are in, not of the whole tree.
   *
   * As for refit(), one that has no bounds any more stays without them, and one that the build left
   * out but that has bounds now is FailedPrecondition. On an error nothing changes.
   */
  Status refit(const Source &moved, std::span<const int> primitives)
  {
    if (std::cmp_not_equal(moved.size(), primitives.size())) {
      return Status::error(StatusCode::InvalidArgument,
                           "GeometryBvh: refit with another number of primitives");
    }
    litestl::util::Vector<int> positions;
    litestl::util::Vector<int> elements;
    for (int k = 0; k < int(primitives.size()); k++) {
      const int primitive = primitives[size_t(k)];
      if (primitive < 0 || primitive >= bvh_.size()) {
        return Status::error(StatusCode::OutOfRange, "GeometryBvh: refit of no such primitive");
      }
      const int position = bvh_.position(primitive);
      const auto box = moved.bounds(k);
      if (position >= 0) {
        positions.append(position);
        elements.append(k);
      } else if (box.allFinite() && !box.empty()) {
        return Status::error(StatusCode::FailedPrecondition,
                             "GeometryBvh: refit of a primitive that the build left out");
      }
    }
    for (int k = 0; k < int(positions.size()); k++) {
      prims_.set(positions[k], moved, elements[k]);
    }
    bvh_.refitPositions({positions.data(), positions.size()},
                        [this](int position) { return prims_.bounds(position); });
    return Status();
  }

  /**
   * Takes the changed geometry: a build for Topology, else a refit for Positions (or a build when
   * that is refused), else nothing. On an error the old one is kept.
   */
  Status onInvalidate(InvalidateKinds kinds, const Source &src)
  {
    if (hasFlag(kinds, InvalidateKinds::Topology)) {
      return build(src, opts_);
    }
    if (!hasFlag(kinds, InvalidateKinds::Positions) || refit(src).ok()) {
      return Status();
    }
    return build(src, opts_);
  }

  const Tree &bvh() const
  {
    return bvh_;
  }
  /** The primitives in leaf order: element i is the source's `bvh().order()[i]`. For traversals of
   * one's own, which have to ask bvh().present(i). */
  const Source &source() const
  {
    return prims_;
  }
  const BvhOptions &options() const
  {
    return opts_;
  }

  /** The first hit along the ray that `accept(const RayHit &) -> bool` takes. */
  template <class Accept = bvh::AcceptAll>
  RayHit rayClosest(const Ray &ray, Accept &&accept = {}) const
  {
    RayHit best;
    if (!ray.valid()) {
      return best;
    }
    const bvh::RayQuery q(ray);
    double limit = q.tMax;
    rayTraverse(q, limit, [&](int i, const bvh::PrimHit &h) {
      const int id = bvh_.order()[i];
      if (best.primitive < 0 || h.t < limit || (h.t == limit && id < best.primitive)) {
        const RayHit hit = toHit(q, id, h);
        if (accept(hit)) {
          best = hit;
          limit = h.t;
        }
      }
      return true;
    });
    return best;
  }

  /** Whether the ray hits anything that `accept(const RayHit &) -> bool` takes. */
  template <class Accept = bvh::AcceptAll>
  bool rayAny(const Ray &ray, Accept &&accept = {}) const
  {
    if (!ray.valid()) {
      return false;
    }
    const bvh::RayQuery q(ray);
    double limit = q.tMax;
    bool any = false;
    rayTraverse(q, limit, [&](int i, const bvh::PrimHit &h) {
      any = accept(toHit(q, bvh_.order()[i], h));
      return !any;
    });
    return any;
  }

  /**
   * Every hit, in no particular order: `fn(const RayHit &) -> bool`, false stops.
   *
   * A ray through an edge or a vertex that triangles share hits each of them. So the number of hits
   * doesn't tell the inside from the outside: whether it is odd is wrong for such rays, which those
   * along an axis often are. Use the side of the nearest hit (RayHit::backFacing) instead.
   */
  template <class Fn> Status rayAll(const Ray &ray, Fn &&fn) const
  {
    if (!ray.valid()) {
      return Status::error(StatusCode::InvalidArgument, "rayAll: the ray isn't valid");
    }
    const bvh::RayQuery q(ray);
    double limit = q.tMax;
    rayTraverse(q, limit, [&](int i, const bvh::PrimHit &h) {
      return bool(fn(toHit(q, bvh_.order()[i], h)));
    });
    return Status();
  }

  /** The nearest primitive within `radius` (inclusive) of `p` that `accept(int) -> bool` takes.
   * Solids contain their inside. Distances whose square overflows aren't found. */
  template <class Accept = bvh::AcceptAll>
  NearestHit nearest(const double3 &p, double radius, Accept &&accept = {}) const
  {
    NearestHit best;
    if (!finitePoint(p) || !(radius >= 0.0)) {
      return best;
    }
    double limitSq = (std::min)(radius * radius, (std::numeric_limits<double>::max)());
    bvh_.traverseOrdered(
        [&](const BvhNode<Scalar> &n) { return nodeDistanceSq(n, p); },
        [&] { return limitSq; },
        [&](int first, int count) {
          for (int i = first; i < first + count; i++) {
            if (!bvh_.present(i)) {
              continue;
            }
            const double3 c = prims_.closest(i, p);
            const double d = distanceSq(p, c);
            const int id = bvh_.order()[i];
            if (d <= limitSq && (best.primitive < 0 || d < limitSq || id < best.primitive) &&
                accept(id))
            {
              limitSq = d;
              best.primitive = id;
              best.point = c;
            }
          }
          return true;
        });
    best.distance = best.primitive >= 0 ? std::sqrt(limitSq) : 0.0;
    return best;
  }

  /** Every primitive within `radius` (inclusive) of `p`, in no particular order:
   * `fn(const NearestHit &) -> bool` or `fn(int) -> bool`, false stops. */
  template <class Fn> Status sphereOverlap(const double3 &p, double radius, Fn &&fn) const
  {
    if (!finitePoint(p) || !(radius >= 0.0)) {
      return Status::error(StatusCode::InvalidArgument,
                           "sphereOverlap: the point or the radius isn't valid");
    }
    const double limitSq = radius * radius;
    bvh_.traverse([&](const BvhNode<Scalar> &n) { return nodeDistanceSq(n, p) <= limitSq; },
                  [&](int first, int count) {
                    for (int i = first; i < first + count; i++) {
                      if (!bvh_.present(i)) {
                        continue;
                      }
                      const double3 c = prims_.closest(i, p);
                      const double d = distanceSq(p, c);
                      if (d <= limitSq && !report(fn, bvh_.order()[i], d, c)) {
                        return false;
                      }
                    }
                    return true;
                  });
    return Status();
  }

  /**
   * Candidates for overlapping the intersection of up to 8 half-spaces:
   * `fn(int) -> bool`, false stops. A primitive that overlaps it is always among them;
   * one that doesn't may be, if it isn't entirely outside any single plane. More planes
   * are InvalidArgument.
   */
  template <class Fn> Status region(std::span<const BvhPlane> planes, Fn &&fn) const
  {
    if (planes.size() > kMaxPlanes) {
      return Status::error(StatusCode::InvalidArgument, "region: more than 8 planes");
    }
    for (const BvhPlane &p : planes) {
      if (!finitePoint(p.n) || !bvh::finite(p.w)) {
        return Status::error(StatusCode::InvalidArgument, "region: a plane isn't finite");
      }
    }
    bvh_.traverse(
        [&](const BvhNode<Scalar> &n) {
          for (const BvhPlane &p : planes) {
            if (bvh::boxOutside(n.min, n.max, p)) {
              return false;
            }
          }
          return true;
        },
        [&](int first, int size) {
          for (int i = first; i < first + size; i++) {
            bool out = !bvh_.present(i);
            for (size_t k = 0; k < planes.size() && !out; k++) {
              out = prims_.outside(i, planes[k]);
            }
            if (!out && !fn(bvh_.order()[i])) {
              return false;
            }
          }
          return true;
        });
    return Status();
  }

 private:
  static constexpr size_t kMaxPlanes = 8;

  template <class Fn> static bool report(Fn &fn, int primitive, double distanceSq, const double3 &c)
  {
    if constexpr (std::is_invocable_v<Fn &, const NearestHit &>) {
      return fn(NearestHit{primitive, std::sqrt(distanceSq), c});
    } else {
      return fn(primitive);
    }
  }
  static RayHit toHit(const bvh::RayQuery &q, int primitive, const bvh::PrimHit &h)
  {
    return {primitive, q.toRayT(h.t), h.u, h.v, h.inside, h.backFacing};
  }

  /* `hit(position, PrimHit) -> bool` may lower `limit`. */
  template <class Hit>
  void rayTraverse(const bvh::RayQuery &q, double &limit, Hit &&hit) const
  {
    const auto present = [&](int i, const bvh::PrimHit &h) {
      return !bvh_.present(i) || hit(i, h);
    };
    bvh_.traverseOrdered(
        [&](const BvhNode<Scalar> &n) {
          double enter;
          return q.nodeSlab(n.min, n.max, limit, enter) ? enter : bvh::kSkip;
        },
        [&] { return limit; },
        [&](int first, int count) { return prims_.rayRange(q, first, count, limit, present); });
  }

  static bool finitePoint(const double3 &p)
  {
    return bvh::finite(p[0]) && bvh::finite(p[1]) && bvh::finite(p[2]);
  }
  static double distanceSq(const double3 &a, const double3 &b)
  {
    const double3 d = a - b;
    return d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
  }
  /* closest() gives points in the bounds up to rounding, which for far coordinates can be large
   * compared to the distance: each axis gets that much less. */
  static double nodeDistanceSq(const BvhNode<Scalar> &n, const double3 &p)
  {
    constexpr double kRound = 16.0 * std::numeric_limits<double>::epsilon();
    double sum = 0.0;
    for (int i = 0; i < 3; i++) {
      const double min = double(n.min[i]), max = double(n.max[i]);
      const double lo = min - p[i], hi = p[i] - max;
      double d = lo > hi ? lo : hi;
      if (d > 0.0) {
        const double a = std::fabs(min), b = std::fabs(max);
        d -= ((a > b ? a : b) + std::fabs(p[i])) * kRound;
        sum += d > 0.0 ? d * d : 0.0;
      }
    }
    return sum;
  }

  Tree bvh_;
  Source prims_;
  BvhOptions opts_;
};

} // namespace coredcc
