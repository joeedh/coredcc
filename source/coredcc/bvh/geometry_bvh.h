#pragma once

#include "coredcc/bvh/bvh.h"
#include "coredcc/bvh/kernels.h"
#include "coredcc/bvh/sources.h"

#include <cstdint>
#include <span>
#include <utility>

namespace coredcc {

/** What changed in a geometry since its BVH was built. */
enum class GeometryChange : uint32_t {
  None = 0,
  /** Primitives moved. */
  Positions = 1 << 0,
  /** Primitives were added, removed or reordered. */
  Topology = 1 << 1,
};
constexpr GeometryChange operator|(GeometryChange a, GeometryChange b)
{
  return GeometryChange(uint32_t(a) | uint32_t(b));
}
constexpr bool hasChange(GeometryChange set, GeometryChange c)
{
  return (uint32_t(set) & uint32_t(c)) != 0;
}

struct NearestHit {
  /** The source's primitive, or -1 when nothing is within the radius. */
  int primitive = -1;
  double distance = 0.0;
  double3 point;

  explicit operator bool() const
  {
    return primitive >= 0;
  }
};

/** A half-space: the points with `n.p + w >= 0`. */
struct BvhPlane {
  double3 n;
  double w = 0.0;
};

/**
 * A Bvh with a leaf-ordered copy of its source's primitives, and the queries. Everything
 * is in the source's space and in double. Primitives are named by their index in the
 * source. Queries are const and may run on several threads.
 *
 * Ties go to the lowest primitive, so answers don't depend on the tree.
 */
template <class Source> class GeometryBvh {
 public:
  using Scalar = typename Source::Scalar;
  using Tree = Bvh<Scalar>;

  /** Replaces the geometry. On an error the old one is kept. */
  Status build(const Source &src, BvhOptions opts = {})
  {
    CDCC_TRY(bvh_.build(src, opts));
    prims_ = src.permuted(bvh_.order());
    opts_ = opts;
    return Status();
  }

  /**
   * Takes the changed geometry: a refit when only positions changed, else a build (also
   * when the refit is refused). On an error the old one is kept.
   */
  Status onInvalidate(GeometryChange changes, const Source &src)
  {
    if (!hasChange(changes, GeometryChange::Topology) && bvh_.refit(src).ok()) {
      prims_ = src.permuted(bvh_.order());
      return Status();
    }
    return build(src, opts_);
  }

  const Tree &bvh() const
  {
    return bvh_;
  }

  /** The first hit along the ray. */
  RayHit rayClosest(const Ray &ray) const
  {
    RayHit best;
    if (!ray.valid()) {
      return best;
    }
    const bvh::RayQuery q(ray);
    double limit = q.tMax;
    rayTraverse(q, limit, [&](int i, double t, double u, double v, bool inside) {
      const int id = bvh_.order()[i];
      if (best.primitive < 0 || t < limit || (t == limit && id < best.primitive)) {
        best = {id, t, u, v, inside};
        limit = t;
      }
      return true;
    });
    best.t = q.toRayT(best.t);
    return best;
  }

  /** Whether the ray hits anything. */
  bool rayAny(const Ray &ray) const
  {
    if (!ray.valid()) {
      return false;
    }
    const bvh::RayQuery q(ray);
    double limit = q.tMax;
    bool any = false;
    rayTraverse(q, limit, [&](int, double, double, double, bool) {
      any = true;
      return false;
    });
    return any;
  }

  /** Every hit, in no particular order: `fn(const RayHit &) -> bool`, false stops. */
  template <class Fn> void rayAll(const Ray &ray, Fn &&fn) const
  {
    if (!ray.valid()) {
      return;
    }
    const bvh::RayQuery q(ray);
    double limit = q.tMax;
    rayTraverse(q, limit, [&](int i, double t, double u, double v, bool inside) {
      return bool(fn(RayHit{bvh_.order()[i], q.toRayT(t), u, v, inside}));
    });
  }

  /** The nearest primitive within `radius` (inclusive) of `p`. Solids contain their
   * inside. Distances whose square overflows aren't found. */
  NearestHit nearest(const double3 &p, double radius) const
  {
    NearestHit best;
    if (!finitePoint(p) || !(radius >= 0.0)) {
      return best;
    }
    double limitSq = (std::min)(radius * radius, (std::numeric_limits<double>::max)());
    bvh_.traverseOrdered(
        [&](const BvhNode<Scalar> &n) { return nodeDistanceSq(n, p); },
        [&] { return widened(limitSq); },
        [&](int first, int count) {
          for (int i = first; i < first + count; i++) {
            const double3 c = prims_.closest(i, p);
            const double d = distanceSq(p, c);
            const int id = bvh_.order()[i];
            if (d <= limitSq && (best.primitive < 0 || d < limitSq || id < best.primitive)) {
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

  /** Every primitive within `radius` (inclusive) of `p`: `fn(int) -> bool`, false stops. */
  template <class Fn> void sphereOverlap(const double3 &p, double radius, Fn &&fn) const
  {
    if (!finitePoint(p) || !(radius >= 0.0)) {
      return;
    }
    const double limitSq = radius * radius;
    bvh_.traverse([&](const BvhNode<Scalar> &n) { return nodeDistanceSq(n, p) <= widened(limitSq); },
                  [&](int first, int count) {
                    for (int i = first; i < first + count; i++) {
                      if (distanceSq(p, prims_.closest(i, p)) <= limitSq &&
                          !fn(bvh_.order()[i]))
                      {
                        return false;
                      }
                    }
                    return true;
                  });
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
    double p[kMaxPlanes][4];
    for (size_t k = 0; k < planes.size(); k++) {
      p[k][0] = planes[k].n[0];
      p[k][1] = planes[k].n[1];
      p[k][2] = planes[k].n[2];
      p[k][3] = planes[k].w;
    }
    const int count = int(planes.size());
    bvh_.traverse(
        [&](const BvhNode<Scalar> &n) {
          for (int k = 0; k < count; k++) {
            if (bvh::boxOutside(n.min, n.max, p[k])) {
              return false;
            }
          }
          return true;
        },
        [&](int first, int size) {
          for (int i = first; i < first + size; i++) {
            bool out = false;
            for (int k = 0; k < count && !out; k++) {
              out = prims_.outside(i, p[k]);
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

  /* `hit(position, t, u, v, inside) -> bool` may lower `limit`. */
  template <class Hit>
  void rayTraverse(const bvh::RayQuery &q, double &limit, Hit &&hit) const
  {
    bvh_.traverseOrdered(
        [&](const BvhNode<Scalar> &n) {
          double enter;
          return q.slab(n.min, n.max, limit, enter) ? enter : bvh::kSkip;
        },
        [&] { return limit; },
        [&](int first, int count) { return prims_.rayRange(q, first, count, limit, hit); });
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
  /* A primitive's distance can round to less than its box's, so boxes get some slack. */
  static double widened(double limitSq)
  {
    return limitSq + limitSq * 1e-12;
  }
  static double nodeDistanceSq(const BvhNode<Scalar> &n, const double3 &p)
  {
    double sum = 0.0;
    for (int i = 0; i < 3; i++) {
      const double lo = double(n.min[i]) - p[i], hi = p[i] - double(n.max[i]);
      const double d = lo > hi ? lo : hi;
      sum += d > 0.0 ? d * d : 0.0;
    }
    return sum;
  }

  Tree bvh_;
  Source prims_;
  BvhOptions opts_;
};

} // namespace coredcc
