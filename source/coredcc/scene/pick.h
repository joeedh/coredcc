#pragma once

/** \file
 * Picking, as a Spatial sees it: the query, and the sink for its shapes.
 * scene/pick_scene.h asks the objects of a scene.
 */

#include "coredcc/base/status.h"
#include "coredcc/bvh/ray.h"
#include "coredcc/draw/view.h"
#include "coredcc/lib/library.h"
#include "coredcc/scene/outline.h"

#include "litestl/util/vector.h"

#include <cstdint>
#include <limits>
#include <span>

namespace litestl::binding::types {
template <typename T> struct Struct;
}

namespace coredcc {

/**
 * Half-spaces in an object's space, for finding what is in a part of the image. A shape
 * that is outside any of them isn't there. `GeometryBvh::region(query.planes(), ...)`
 * finds candidates.
 */
class RegionQuery {
public:
  RegionQuery() = default;
  RegionQuery(const ViewRegion &region, const mat4d &world);

  std::span<const BvhPlane> planes() const
  {
    return planes_;
  }

  /* For scripts. */
  int32_t planeCount() const
  {
    return ViewRegion::kPlanes;
  }
  double3 planeNormal(int32_t i) const;
  /** The plane's `w`: the half-space is `normal.p + w >= 0`. */
  double planeOffset(int32_t i) const;
  /** The normals and offsets of all planes, four numbers each, as AnyBvh::region takes
   * them. */
  litestl::util::Vector<double> planeValues() const;
  static litestl::binding::types::Struct<RegionQuery> *defineBindings();

private:
  BvhPlane planes_[ViewRegion::kPlanes];
};

/** How hits are found and ranked. Sizes are in logical pixels. */
struct PickOptions {
  /** How far from the pixel something may be. */
  double radius = 0.0;
  /** A bit for each ElementDomain that is wanted. Bounds have none, and are always taken.
   */
  uint32_t domains = ~0u;
  /** The largest pixel radius that shapes have (PickSink::point, segment). Those that
   * have a larger one may be missed. */
  double shapeRadius = 0.0;
  /** The most hits, or 0 for all. 1 is faster: what can't be the first hit isn't looked
   * at. */
  int32_t maxHits = 0;
  /** Not only the best hit of each object, but that of each of its domains. */
  bool everyDomain = false;
  /** Thin shapes behind a surface come after it. Otherwise they come first all the same.
   */
  bool occlusion = true;
  /** Solids with a radius on the screen up to this are thin. */
  double thinRadius = 4.0;
  /**
   * How far behind the frontmost surface under the pixel a thin shape may be and not
   * count as behind it: this many times the reach (radius and shapeRadius, at least 1) in
   * world units at the surface. So it is the slope, against the image plane, up to which
   * a surface doesn't hide the points and edges on itself.
   */
  double slope = 2.0;

  /** Whether the numbers are finite and not negative. */
  bool valid() const;
  static litestl::binding::types::Struct<PickOptions> *defineBindings();
};

/** A pixel in a view, with options: what the PickQuery of every object has in common. */
class PickCursor {
public:
  PickCursor() = default;
  /** Device pixels. The options must be valid. */
  PickCursor(const ViewMath &view, double x, double y, const PickOptions &options);

  const ViewMath &view() const
  {
    return view_;
  }
  const PickOptions &options() const
  {
    return options_;
  }
  const ViewRegion &region() const
  {
    return region_;
  }
  /** Through the pixel, from the near to the far plane. */
  const Ray &ray() const
  {
    return ray_;
  }
  /** The reach, in world units at a world point. */
  double reachAt(const double3 &p) const;

private:
  friend class PickQuery;
  ViewMath view_;
  PickOptions options_;
  double x_ = 0.0, y_ = 0.0;
  ViewRegion region_;
  Ray ray_;
  /* The near plane, normalized, and how much of the distance from it a unit of the ray
   * is. */
  double4 front_, back_;
  double along_ = 1.0;
};

/**
 * What a Spatial is asked for, in its object's space. To find candidates:
 *
 * - The half-spaces `planes()`. Shapes outside any of them are too far from the pixel,
 * not visible, or too far behind what has been found already. It is always possible to
 * use them.
 * - The ray through the pixel, if there is one. Whatever it hits first covers the pixel,
 * but it misses what is only near it.
 */
class PickQuery {
public:
  PickQuery() = default;
  /** For an object with the matrix `world`. `cursor` must outlive it. What is more than
   * `limitDepth` behind the near plane, in world units, isn't wanted. Surfaces that are
   * near the pixel but don't cover it are wanted only if `nearSurfaces`. */
  PickQuery(const PickCursor &cursor,
            const mat4d &world,
            double limitDepth = (std::numeric_limits<double>::infinity)(),
            bool nearSurfaces = true);

  const RegionQuery &region() const
  {
    return region_;
  }
  std::span<const BvhPlane> planes() const
  {
    return region_.planes();
  }
  /** False when the object's matrix has no inverse (a scale of 0). */
  bool hasRay() const
  {
    return hasRay_;
  }
  /** From the near plane to the far plane, or to limitDepth(). Its t is the distance in
   * the world. */
  const Ray &ray() const
  {
    return ray_;
  }
  /** PickOptions::domains. Shapes of other domains needn't be given, except to occlude.
   */
  uint32_t domains() const
  {
    return options_.domains;
  }
  /** Whether elements of a domain are wanted. */
  bool wants(int32_t domain) const
  {
    return domain < 0 || (domain < 32 ? (options_.domains >> domain & 1) != 0
                                      : options_.domains == ~0u);
  }
  /** Whether surfaces are wanted, of whatever domain, since they hide thin shapes behind
   * them. */
  bool occlusion() const
  {
    return options_.occlusion;
  }
  /** Device pixels from the pixel within which shapes are wanted, not counting their own
   * radius. */
  double radiusDevicePixels() const
  {
    return radius_;
  }
  /** The same for surfaces and wide solids: less, once something has been found that
   * comes before those that only are near. */
  double surfaceRadiusDevicePixels() const
  {
    return surfaceRadius_;
  }
  /** World units behind the near plane; infinite for no limit. */
  double limitDepth() const
  {
    return limitDepth_;
  }
  /**
   * An object-space length that is at least a logical pixel at `p`, in whichever
   * direction it is seen: for a radius that covers so many pixels. 0 if `p` is behind the
   * eye, and infinite if the object's matrix flattens it.
   */
  double pixelSizeAt(const double3 &p) const;

  /* For scripts. */
  int32_t planeCount() const
  {
    return region_.planeCount();
  }
  double3 planeNormal(int32_t i) const
  {
    return region_.planeNormal(i);
  }
  double planeOffset(int32_t i) const
  {
    return region_.planeOffset(i);
  }
  litestl::util::Vector<double> planeValues() const
  {
    return region_.planeValues();
  }
  double3 rayOrigin() const
  {
    return ray_.origin;
  }
  double3 rayDirection() const
  {
    return ray_.dir;
  }
  double rayLimit() const
  {
    return ray_.tMax;
  }
  static litestl::binding::types::Struct<PickQuery> *defineBindings();

private:
  friend class PickSink;
  RegionQuery region_;
  Ray ray_;
  bool hasRay_ = false;
  detail::ScreenMap map_;
  mat4d world_;
  /* World to object, transposed, for normals, up to a positive factor. */
  double3 normalColumns_[3];
  double3 worldDir_;
  PickOptions options_;
  const PickCursor *cursor_ = nullptr;
  double radius_ = 0.0;
  double surfaceRadius_ = 0.0;
  double limitDepth_ = (std::numeric_limits<double>::infinity)();
  double dpiScale_ = 1.0;
};

struct PickHit {
  BlockHandle object;
  /** An ElementDomain, and the element as the object reported it. -1 for its bounds. */
  int32_t domain = -1;
  int32_t element = -1;
  /** The instance, as the object reported it, or -1. */
  int32_t instance = -1;
  /** Logical pixels from the cursor to what is seen of the shape, beyond its own pixel
   * radius. 0 if it covers the cursor. */
  double distance = 0.0;
  /** Of `point` (ViewMath::viewDistance). */
  double depth = 0.0;
  /** In the world: where the ray through the cursor hits the shape, or the shape's point
   * that is nearest to the cursor on the screen. */
  double3 point;
  /** The unit normal at `point`, in the world; 0 for points and segments, and where a
   * solid isn't hit by the ray. That of a triangle abc is (b - a) x (c - a) in the
   * object's space, also when the object is mirrored. */
  double3 normal;
  /** On a triangle abc, `point` is at a + u (b - a) + v (c - a). On a segment or capsule,
   * `u` is how far along its axis it is, from 0 to 1. */
  double u = 0.0, v = 0.0;
  /** The normal points away from the eye. */
  bool backFacing = false;
  /** A point, a segment, or a solid that is thin on the screen (PickOptions::thinRadius).
   */
  bool thin = false;

  /** Whether the shape covers the cursor. */
  bool covers() const
  {
    return distance == 0.0;
  }
};

namespace detail {

/** A hit, with what its rank also depends on. */
struct PickCandidate {
  PickHit hit;
  /** The object's, in the scene's order. */
  int32_t place = 0;
  /** 0 for points, 1 for segments and thin solids, 2 for the rest. */
  int32_t dimension = 2;
};

/** The frontmost surface that covers the cursor, and the depth from which on things are
 * behind it. */
struct Occluder {
  double depth = (std::numeric_limits<double>::infinity)();
  double behind = (std::numeric_limits<double>::infinity)();

  void include(const Occluder &b)
  {
    if (b.depth < depth) {
      *this = b;
    }
  }
};

/**
 * Orders the hits, and leaves one of each object, or of each object's domain, and at most
 * maxHits:
 *
 * 1. Thin shapes that aren't behind the occluder: by distance, then the dimension, then
 * the depth.
 * 2. What covers the cursor, by depth.
 * 3. The rest, by distance, then the depth.
 *
 * Then by place; equals stay in their order.
 */
void rankPicks(const PickOptions &options,
               const Occluder &occluder,
               litestl::util::Vector<PickCandidate> &hits);

} // namespace detail

/**
 * Takes the shapes an object has near a pixel, and keeps those that may become hits.
 * Shapes that aren't near may be given too, and in any order.
 *
 * - Coordinates and radii are in the object's space. Shapes with values that aren't
 * finite, or a negative radius, are ignored.
 * - Points, segments and solids that are narrow on the screen are thin: they can be
 * picked in front of a surface without being hit exactly. Surfaces hide what is behind
 * them.
 * - Pixel radii are logical. A point or segment with one covers the cursor within it.
 */
class PickSink {
public:
  /** `query` must outlive it. */
  explicit PickSink(const PickQuery &query) : query_(query)
  {
  }
  PickSink(const PickSink &) = delete;
  PickSink &operator=(const PickSink &) = delete;

  /** What the shapes after this belong to: an ElementDomain, and an element. */
  void element(int32_t domain, int32_t element)
  {
    domain_ = domain;
    element_ = element;
  }
  /** Which instance the shapes after this belong to; -1 for none. Shapes are in the
   * object's space all the same. */
  void instance(int32_t instance)
  {
    instance_ = instance;
  }
  void point(const double3 &p, double pixels = 0.0);
  void segment(const double3 &a, const double3 &b, double pixels = 0.0);
  void triangle(const double3 &a, const double3 &b, const double3 &c);
  /** A solid. */
  void sphere(const double3 &center, double radius);
  /** A solid: the points within `radius` of a segment. */
  void capsule(const double3 &a, const double3 &b, double radius);
  /** The surface of a box. */
  void box(const Box3d &box);

  /* Many at once, for scripts: the numbers of one shape after those of another (a point
   * has 3, a segment 6, a triangle 9, a capsule 7: its ends and its radius), and the
   * element of each. InvalidArgument, and nothing taken, if the sizes don't match. */
  Status points(int32_t domain,
                const litestl::util::Vector<int32_t> &elements,
                const litestl::util::Vector<double> &values,
                double pixels);
  Status segments(int32_t domain,
                  const litestl::util::Vector<int32_t> &elements,
                  const litestl::util::Vector<double> &values,
                  double pixels);
  Status triangles(int32_t domain,
                   const litestl::util::Vector<int32_t> &elements,
                   const litestl::util::Vector<double> &values);
  Status capsules(int32_t domain,
                  const litestl::util::Vector<int32_t> &elements,
                  const litestl::util::Vector<double> &values);

  bool any() const
  {
    return kept_.size() > 0;
  }
  /** The best, without `object`, if there is any: as if the object were alone. */
  PickHit best() const;
  /** Every hit that may come first, in no particular order. */
  const litestl::util::Vector<detail::PickCandidate> &kept() const
  {
    return kept_;
  }
  const detail::Occluder &occluder() const
  {
    return occluder_;
  }

  static litestl::binding::types::Struct<PickSink> *defineBindings();

private:
  struct Found {
    detail::ScreenPoint at;
    /** Device pixels. */
    double pixels = 0.0;
    int32_t dimension = 2;
    bool thin = false;
    double3 normal;
    double u = 0.0, v = 0.0;
  };
  void corners(const double3 *p, int count, double pixels);
  void take(const Found &f);
  void keep(const detail::PickCandidate &c);

  const PickQuery &query_;
  int32_t domain_ = -1;
  int32_t element_ = -1;
  int32_t instance_ = -1;
  litestl::util::Vector<detail::PickCandidate> kept_;
  detail::Occluder occluder_;
};

} // namespace coredcc
