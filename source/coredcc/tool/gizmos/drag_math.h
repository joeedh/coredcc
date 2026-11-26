#pragma once

/** \file
 * The geometry of dragging a gizmo, in world space and in device pixels. Everything is
 * double; nothing here knows of scenes or commands, so it is tested against every View
 * convention on its own (test_tool_drag_math).
 */

#include "coredcc/base/status.h"
#include "coredcc/draw/view.h"

namespace coredcc::gizmo {

/** A point on the screen, device pixels, y down. */
struct Pixel {
  double x = 0.0, y = 0.0;
};

/** The unit direction from `p` toward the eye (perspective), or against the view
 * direction (ortho): the way a surface at `p` faces the viewer. */
double3 towardEye(const ViewMath &view, const double3 &p);

/** Where `p` is on the screen; false when it is behind the eye. */
bool toPixel(const ViewMath &view, const double3 &p, Pixel &out);

/**
 * The point of the line `origin + t·dir` nearest the ray through a pixel, as `t`.
 * FailedPrecondition when the line is (nearly) parallel to the ray: within about 0.5° of
 * it.
 */
Result<double> axisParameter(
    const ViewMath &view, double x, double y, const double3 &origin, const double3 &dir);

/** Where the ray through a pixel meets the plane through `origin` with `normal`.
 * FailedPrecondition when the ray is (nearly) parallel to it, or meets it behind. */
Result<double3> planePoint(const ViewMath &view,
                           double x,
                           double y,
                           const double3 &origin,
                           const double3 &normal);

/** +1 if a positive rotation about the direction toward the eye looks
 * counter-clockwise on the screen (y up), -1 if the view is mirrored. */
double screenHandedness(const ViewMath &view, const double3 &p);

/**
 * Accumulates the angle of the cursor about a centre on the screen, counter-clockwise
 * with y up, unwrapped across ±π: a cursor that goes round twice gives 4π.
 */
class AngleTracker {
public:
  AngleTracker() = default;
  AngleTracker(Pixel centre, Pixel start);
  /** The angle from the start to `p`. A cursor on the centre keeps the last angle. */
  double update(Pixel p);
  double total() const
  {
    return total_;
  }

private:
  Pixel centre_;
  double last_ = 0.0;
  double total_ = 0.0;
};

/**
 * The factor of a scale along a screen direction `axis` (unit length) from `centre`:
 * the cursor's distance along it over the start's. Without an axis (0, 0), the ratio of
 * the distances. 1 when the start is within a pixel of the centre.
 */
double scaleFactor(Pixel centre, Pixel start, Pixel now, Pixel axis);

/** `v` rounded to a multiple of `step`; `v` for a step that isn't positive. */
double snap(double v, double step);

} // namespace coredcc::gizmo
