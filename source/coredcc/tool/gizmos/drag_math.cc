#include "coredcc/tool/gizmos/drag_math.h"

#include <cmath>
#include <numbers>

namespace coredcc::gizmo {

namespace {

constexpr double kPi = std::numbers::pi;

Status degenerate(const char *what)
{
  return Status::error(StatusCode::FailedPrecondition, what);
}

} // namespace

bool toPixel(const ViewMath &view, const double3 &p, Pixel &out)
{
  const ViewPoint vp = view.project(p);
  out.x = vp.x;
  out.y = vp.y;
  return !vp.behind && std::isfinite(vp.x) && std::isfinite(vp.y);
}

double3 towardEye(const ViewMath &view, const double3 &p)
{
  /* The ray through p's pixel runs from the eye through p; its direction is unit. */
  Pixel px;
  if (toPixel(view, p, px)) {
    const double3 d = view.rayDirection(px.x, px.y);
    if (d.length() > 0.5) {
      return d * -1.0;
    }
  }
  /* Behind the eye: the near plane's normal points into the view. */
  double3 n(view.nearPlane()[0], view.nearPlane()[1], view.nearPlane()[2]);
  const double len = n.length();
  return len > 0.0 ? n * (-1.0 / len) : double3(0.0, 0.0, 1.0);
}

Result<double> axisParameter(
    const ViewMath &view, double x, double y, const double3 &origin, const double3 &dir)
{
  const double3 ro = view.rayOrigin(x, y);
  const double3 rd = view.rayDirection(x, y);
  const double dd = dir.dot(dir);
  if (!(dd > 0.0)) {
    return degenerate("axisParameter: an axis without a direction");
  }
  /* Closest points of two lines: origin + t·dir and ro + s·rd, rd unit. */
  const double3 w = origin - ro;
  const double b = dir.dot(rd);
  const double denom = dd - b * b;
  if (!(denom > dd * 1e-4)) { /* sin² of the angle below 1e-4: about 0.57° */
    return degenerate("axisParameter: the axis is parallel to the view ray");
  }
  const double d = dir.dot(w);
  const double e = rd.dot(w);
  return (b * e - d) / denom;
}

Result<double3> planePoint(const ViewMath &view,
                           double x,
                           double y,
                           const double3 &origin,
                           const double3 &normal)
{
  const double3 ro = view.rayOrigin(x, y);
  const double3 rd = view.rayDirection(x, y);
  const double nl = normal.length();
  if (!(nl > 0.0)) {
    return degenerate("planePoint: a plane without a normal");
  }
  const double denom = normal.dot(rd) / nl;
  if (!(std::abs(denom) > 1e-6)) {
    return degenerate("planePoint: the ray is parallel to the plane");
  }
  const double t = normal.dot(origin - ro) / nl / denom;
  if (!(t >= 0.0) || !std::isfinite(t)) {
    return degenerate("planePoint: the plane is behind the ray");
  }
  return ro + rd * t;
}

double screenHandedness(const ViewMath &view, const double3 &p)
{
  /* Turn a vector in the screen's plane about the direction toward the eye, and see
   * which way it goes on the screen. */
  const double3 n = towardEye(view, p);
  double3 u = std::abs(n[0]) < 0.9 ? double3(1, 0, 0) : double3(0, 1, 0);
  u = u - n * u.dot(n);
  const double s = view.pixelSizeAt(p) * 50.0;
  const double len = u.length();
  if (!(len > 0.0) || !(s > 0.0)) {
    return 1.0;
  }
  u = u * (s / len);
  const double3 v = n.cross(u); /* u turned by +90° about n */
  Pixel c, a, b;
  if (!toPixel(view, p, c) || !toPixel(view, p + u, a) || !toPixel(view, p + v, b)) {
    return 1.0;
  }
  /* With y up: flip the y differences. */
  const double cross = (a.x - c.x) * (c.y - b.y) - (c.y - a.y) * (b.x - c.x);
  return cross >= 0.0 ? 1.0 : -1.0;
}

AngleTracker::AngleTracker(Pixel centre, Pixel start) : centre_(centre)
{
  last_ = std::atan2(centre.y - start.y, start.x - centre.x);
}

double AngleTracker::update(Pixel p)
{
  const double dx = p.x - centre_.x, dy = centre_.y - p.y;
  if (dx * dx + dy * dy < 1e-12) {
    return total_;
  }
  const double a = std::atan2(dy, dx);
  double d = a - last_;
  while (d > kPi) {
    d -= 2.0 * kPi;
  }
  while (d < -kPi) {
    d += 2.0 * kPi;
  }
  total_ += d;
  last_ = a;
  return total_;
}

double scaleFactor(Pixel centre, Pixel start, Pixel now, Pixel axis)
{
  const double sx = start.x - centre.x, sy = start.y - centre.y;
  const double nx = now.x - centre.x, ny = now.y - centre.y;
  if (axis.x == 0.0 && axis.y == 0.0) {
    const double s = std::hypot(sx, sy);
    return s < 1.0 ? 1.0 : std::hypot(nx, ny) / s;
  }
  const double s = sx * axis.x + sy * axis.y;
  return std::abs(s) < 1.0 ? 1.0 : (nx * axis.x + ny * axis.y) / s;
}

double snap(double v, double step)
{
  return step > 0.0 ? std::round(v / step) * step : v;
}

} // namespace coredcc::gizmo
