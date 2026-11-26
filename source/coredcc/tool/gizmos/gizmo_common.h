#pragma once

/** \file
 * What the transform gizmos share: where they are (from the last evaluation), their parts
 * and look (Blender's defaults), and the screen geometry their hit tests and drawing both
 * use, so that what is drawn is what is hit.
 */

#include "coredcc/api/context.h"
#include "coredcc/draw/drawlist.h"
#include "coredcc/draw/view.h"
#include "coredcc/lib/library.h"
#include "coredcc/scene/element_domain.h"
#include "coredcc/tool/gizmos/drag_math.h"

#include "litestl/math/quat.h"
#include "litestl/util/vector.h"

#include <cstdint>

namespace coredcc::gizmo {

using litestl::math::quatd;

/** The transform tool's `mode`. */
enum class Mode : int32_t { Translate = 0, Rotate = 1, Scale = 2 };
/** Its `orientation`. */
enum class Orientation : int32_t { Global = 0, Local = 1, View = 2 };
/** Its `pivot`. */
enum class PivotMode : int32_t { Median = 0, Active = 1, Individual = 2 };

/** Gizmo parts. Planes are named by their normal. */
namespace part {
inline constexpr int32_t kX = 0, kY = 1, kZ = 2;
inline constexpr int32_t kPlaneX = 3, kPlaneY = 4, kPlaneZ = 5;
/** Translate: the view plane; scale: uniform. */
inline constexpr int32_t kCentre = 6;
/** Rotate: about the view direction. */
inline constexpr int32_t kView = 7;
inline constexpr int32_t kCount = 8;
} // namespace part

/** Blender's look: theme colors (RGBA8, red in the lowest byte) and sizes. */
namespace look {
inline constexpr uint32_t kAxis[3] = {0x005233ffu, 0x0000dc8bu, 0x00ff9028u};
inline constexpr uint32_t kWhite = 0x00ffffffu;
/** Alpha of a part, and of the one under the cursor or dragged. */
inline constexpr double kAlpha = 0.6, kAlphaHi = 1.0;
/** Logical pixels: the gizmo's size (Preferences > Viewport > Gizmo Size). */
inline constexpr double kSize = 75.0;
/** An axis fades out as 1 - |cos| of its angle to the view goes from 0.1 to 0.02, and is
 * hidden below; a plane as |cos| of its normal's goes from 0.25 to 0.175. */
inline constexpr double kAxisHide = 0.02, kAxisShow = 0.1;
inline constexpr double kPlaneHide = 0.175, kPlaneShow = 0.25;
/** Line widths, logical pixels. */
inline constexpr float kLineWidth = 2.0f, kRingWidth = 3.0f;
} // namespace look

/** Where the gizmo is: the objects that a drag moves, and their pivot and orientation. */
struct Placement {
  bool visible = false;
  double3 pivot;
  /** GLOBAL's or LOCAL's; VIEW's depends on the view (basisFor). */
  quatd basis;
  int32_t count = 0;
};

/**
 * From the last evaluation of the Context's scene, over the objects that ApplyDelta
 * moves: selected, visible and selectable, and not below another of them. The pivot is
 * their mean position (Blender's median point), or the active one's for ACTIVE when it is
 * among them. LOCAL is the active object's rotation, or the first one's.
 */
Placement computePlacement(Context &ctx, PivotMode pivot, Orientation orientation);

/** The same for the elements of `domain` that core.element.dragDelta moves: their mean
 * position in the world. The basis is as for objects. */
Placement
computeElementPlacement(Context &ctx, ElementDomain domain, Orientation orientation);

/** The rotation whose columns are the gizmo's axes in the world. */
quatd basisFor(const Placement &p, Orientation o, const ViewMath &view);

/** The world length of the gizmo's unit at its pivot: `size` logical pixels. */
double unitAt(const ViewMath &view, const double3 &pivot, double size);

/** 0 (hidden) to 1: how much of an axis or a plane's normal is shown. */
double axisFade(const double3 &axis, const double3 &towardEye);
double planeFade(const double3 &normal, const double3 &towardEye);

/** A color with an alpha in 0 to 1. */
uint32_t withAlpha(uint32_t rgb, double alpha);

/** Logical pixels from a pixel to a segment, or a polyline, on the screen. */
double segmentDistance(Pixel p, Pixel a, Pixel b);
double polylineDistance(Pixel p, const litestl::util::Vector<Pixel> &line, bool closed);
/** 0 inside a convex polygon; the distance to its edges outside. */
double polygonDistance(Pixel p, const litestl::util::Vector<Pixel> &poly);

/** The front half of a circle in the world (where it faces the eye), as points in order,
 * in arcs: a run of points that are adjacent. `arcs` gets the index each arc starts at.
 */
void frontArc(const ViewMath &view,
              const double3 &centre,
              const double3 &normal,
              double radius,
              litestl::util::Vector<double3> &points,
              litestl::util::Vector<int32_t> &arcs);

} // namespace coredcc::gizmo
