#include "coredcc/draw/view.h"

#include "litestl/math/transform.h"

#include <cmath>
#include <limits>
#include <string>

namespace coredcc {

using litestl::math::planeToLocal;
using litestl::math::transformHomogeneous;
using litestl::math::transformPoint;
using litestl::math::transformVector;
using litestl::math::tryInverse;

namespace {

double element(const mat4d &m, int row, int col)
{
  const double *d = m;
  return d[col * 4 + row];
}

double4 rowOf(const mat4d &m, int row)
{
  return double4(element(m, row, 0), element(m, row, 1), element(m, row, 2), element(m, row, 3));
}

double dot4(const double4 &a, const double4 &b)
{
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
}

double length3(const double4 &v)
{
  return std::hypot(v[0], v[1], v[2]);
}

bool allFinite(const mat4d &m)
{
  const double *d = m;
  for (int i = 0; i < 16; i++) {
    if (!bvh::finite(d[i])) {
      return false;
    }
  }
  return true;
}

/* The sign of a 3x3 determinant's value; columns are scaled first. */
double linearSign(const mat4d &m)
{
  double c[3][3];
  for (int i = 0; i < 3; i++) {
    double big = 0.0;
    for (int r = 0; r < 3; r++) {
      big = std::fmax(big, std::fabs(element(m, r, i)));
    }
    for (int r = 0; r < 3; r++) {
      c[i][r] = big > 0.0 ? element(m, r, i) / big : 0.0;
    }
  }
  return c[0][0] * (c[1][1] * c[2][2] - c[1][2] * c[2][1]) -
         c[1][0] * (c[0][1] * c[2][2] - c[0][2] * c[2][1]) +
         c[2][0] * (c[0][1] * c[1][2] - c[0][2] * c[1][1]);
}

double sign(double v)
{
  return v > 0.0 ? 1.0 : v < 0.0 ? -1.0 : 0.0;
}

Status bad(const char *what)
{
  return Status::error(StatusCode::InvalidArgument, (std::string("View: ") + what).c_str());
}

double nearDepthOf(const View &v)
{
  return v.reversedZ ? 1.0 : v.depth == DepthRange::ZeroToOne ? 0.0 : -1.0;
}
double farDepthOf(const View &v)
{
  return !v.reversedZ ? 1.0 : v.depth == DepthRange::ZeroToOne ? 0.0 : -1.0;
}

/* What a projection has that a float one can't have more exactly. */
constexpr double kAtInfinity = 1e-6;

/* The largest element of the rows of a column. */
double columnScale(const mat4d &m, int col, int rows)
{
  double big = 0.0;
  for (int r = 0; r < rows; r++) {
    big = std::fmax(big, std::fabs(element(m, r, col)));
  }
  return big;
}

/* Makes 0 what is meant to be 0: matrices often come from a general inverse. */
void clean(View &v)
{
  constexpr double kRound = 1e-6;
  double *m = v.view, *p = v.proj;
  bool affine = std::fabs(m[15] - 1.0) <= kRound;
  for (int c = 0; c < 3; c++) {
    affine = affine && std::fabs(m[c * 4 + 3]) <= kRound * columnScale(v.view, c, 3);
  }
  if (affine) {
    for (int c = 0; c < 4; c++) {
      for (int r = 0; r < 3; r++) {
        m[c * 4 + r] /= m[15];
      }
      m[c * 4 + 3] = c < 3 ? 0.0 : m[15];
    }
    m[15] = 1.0;
  }
  const auto round = [p](int row, int col, double scale) {
    if (std::fabs(p[col * 4 + row]) <= kRound * scale) {
      p[col * 4 + row] = 0.0;
    }
  };
  for (int c = 0; c < 2; c++) {
    round(2, c, columnScale(v.proj, c, 2));
    round(3, c, columnScale(v.proj, c, 2));
  }
  if (v.ortho) {
    round(3, 2, std::fabs(element(v.proj, 2, 2)));
  }
  else {
    round(3, 3, std::fabs(element(v.proj, 2, 3)));
  }
}

/* A perspective projection's depth at distance d before the eye is `a + b / d`. This is how far `a`
 * is from the far plane's depth. */
double farGap(const View &v)
{
  const double a = element(v.proj, 2, 2) / element(v.proj, 3, 2);
  return farDepthOf(v) - a;
}

bool farAtInfinity(const View &v)
{
  return !v.ortho && std::fabs(farGap(v)) <= kAtInfinity;
}

Status checkPerspectiveDepth(const View &v)
{
  const double k = std::fabs(element(v.proj, 3, 2));
  const double a = element(v.proj, 2, 2) * sign(element(v.proj, 3, 2)) / k;
  const double b = element(v.proj, 2, 3) / k;
  if ((b > 0.0) != v.reversedZ) {
    return bad(v.reversedZ ? "reversedZ, but the projection's depth increases with distance" :
                             "the projection's depth decreases with distance: reversedZ is missing");
  }
  const double zNear = b / (nearDepthOf(v) - a);
  if (!(zNear > 0.0) || !bvh::finite(zNear)) {
    return bad("the near plane isn't in front of the eye: the depth range is wrong");
  }
  /* A float projection with a distant far plane can't be told from an infinite one. */
  const double gap = farGap(v);
  const bool atInfinity = std::fabs(gap) <= kAtInfinity;
  if (v.infiniteFar && !atInfinity) {
    return bad("infiniteFar, but the projection has a far plane");
  }
  if (!atInfinity && !(b / gap > zNear)) {
    return bad("the far plane isn't past the near plane: the depth range is wrong");
  }
  return Status();
}

/* Cleans the view, and says what is wrong with it. */
Status checkView(View &v)
{
  for (double x : v.viewport) {
    if (!bvh::finite(x)) {
      return bad("the viewport isn't finite");
    }
  }
  if (!(v.viewport[2] > 0.0) || !(v.viewport[3] > 0.0)) {
    return bad("the viewport is empty");
  }
  if (!(v.dpiScale > 0.0) || !bvh::finite(v.dpiScale)) {
    return bad("dpiScale isn't a positive number");
  }
  if ((v.depth != DepthRange::ZeroToOne && v.depth != DepthRange::MinusOneToOne) ||
      (v.handedness != Handedness::Right && v.handedness != Handedness::Left) ||
      (v.up != UpAxis::Y && v.up != UpAxis::Z))
  {
    return bad("an enum has no such value");
  }
  mat4d inverse;
  if (!allFinite(v.view) || !allFinite(v.proj)) {
    return bad("a matrix isn't finite");
  }
  clean(v);
  const double4 last = rowOf(v.view, 3);
  if (last[0] != 0.0 || last[1] != 0.0 || last[2] != 0.0 || last[3] != 1.0) {
    return bad("the view matrix isn't affine");
  }
  if (!tryInverse(v.view, inverse) || !tryInverse(v.proj, inverse)) {
    return bad("a matrix isn't invertible");
  }

  const double4 z = rowOf(v.proj, 2), w = rowOf(v.proj, 3);
  if (z[0] != 0.0 || z[1] != 0.0 || w[0] != 0.0 || w[1] != 0.0) {
    return bad("the projection's depth depends on x or y (an oblique near plane)");
  }
  if (v.ortho ? !(w[2] == 0.0 && w[3] > 0.0) : !(w[2] != 0.0 && w[3] == 0.0)) {
    return bad(v.ortho ? "ortho, but the projection isn't orthographic" :
                       "the projection isn't a perspective one: ortho is missing");
  }
  if (v.ortho && v.infiniteFar) {
    return bad("an orthographic view has no infinite far plane");
  }
  if (!v.ortho) {
    CDCC_TRY(checkPerspectiveDepth(v));
  }

  /* Normalized device coordinates (x right, y up, depth away from the eye) are left-handed.
   * The projection is block triangular, so its determinant's sign is exact. */
  const double xy = element(v.proj, 0, 0) * element(v.proj, 1, 1) -
                    element(v.proj, 0, 1) * element(v.proj, 1, 0);
  const double orientation = sign(linearSign(v.view)) * sign(xy) * sign(z[2] * w[3] - z[3] * w[2]) *
                             (v.reversedZ ? -1.0 : 1.0);
  if (orientation != (v.handedness == Handedness::Right ? -1.0 : 1.0)) {
    return bad(v.ortho ? "the matrices contradict handedness (or reversedZ, or the image is mirrored)" :
                       "the matrices contradict handedness (or the image is mirrored)");
  }
  return Status();
}

} // namespace

Status View::validate() const
{
  View copy = *this;
  return checkView(copy);
}

double View::viewportAt(int32_t i) const
{
  return i >= 0 && i < 4 ? viewport[i] : 0.0;
}

void View::putViewport(int32_t i, double value)
{
  if (i >= 0 && i < 4) {
    viewport[i] = value;
  }
}

void View::setConventions(int32_t depthRange, int32_t hand, int32_t upAxis)
{
  depth = DepthRange(depthRange);
  handedness = Handedness(hand);
  up = UpAxis(upAxis);
}

double ViewRegion::planeAt(int32_t plane, int32_t i) const
{
  if (plane < 0 || plane >= kPlanes || i < 0 || i > 3) {
    return 0.0;
  }
  return i < 3 ? planes[plane].n[i] : planes[plane].w;
}

Status ViewMath::set(const View &given)
{
  ViewMath m;
  m.view_ = given;
  CDCC_TRY(checkView(m.view_));
  const View &v = m.view_;
  if (!tryInverse(v.view, m.invView_) || !tryInverse(v.proj, m.invProj_)) {
    return bad("a matrix isn't invertible");
  }
  for (int r = 0; r < 4; r++) {
    m.clip_[r] = rowOf(v.proj, r);
  }
  const double4 &cx = m.clip_[0], &cy = m.clip_[1], &cz = m.clip_[2], &cw = m.clip_[3];
  m.nearView_ = v.reversedZ ? cw - cz : v.depth == DepthRange::ZeroToOne ? cz : cw + cz;
  m.farView_ = farAtInfinity(v) ? double4(0.0, 0.0, 0.0, 1.0) :
               !v.reversedZ     ? cw - cz :
               v.depth == DepthRange::ZeroToOne ? cz :
                                                  cw + cz;
  m.nearPlane_ = planeToLocal(v.view, m.nearView_);
  m.farPlane_ = planeToLocal(v.view, m.farView_);
  m.nearScale_ = 1.0 / length3(m.nearPlane_);
  m.nearDepth_ = nearDepthOf(v);
  m.midDepth_ = 0.5 * (nearDepthOf(v) + farDepthOf(v));

  const double halfW = 0.5 * v.viewport[2], halfH = 0.5 * v.viewport[3];
  m.pixelRows_[0] = planeToLocal(v.view, cx * halfW + cw * (v.viewport[0] + halfW));
  m.pixelRows_[1] = planeToLocal(v.view, cy * -halfH + cw * (v.viewport[1] + halfH));
  m.pixelRows_[2] = planeToLocal(v.view, cw);
  bool finite = bvh::finite(m.nearScale_) && m.nearScale_ > 0.0;
  for (int r = 0; r < 3; r++) {
    for (int c = 0; c < 4; c++) {
      finite = finite && bvh::finite(m.pixelRows_[r][c]);
    }
  }
  if (!finite) {
    return bad("the matrices' product overflows");
  }
  m.valid_ = true;
  *this = m;
  return Status();
}

ViewPoint ViewMath::project(const double3 &p) const
{
  ViewPoint r;
  const double4 v = transformHomogeneous(view_.view, double4(p[0], p[1], p[2], 1.0));
  const double w = dot4(clip_[3], v), nearSide = dot4(nearView_, v);
  r.distance = nearSide * nearScale_;
  r.behind = !(w > 0.0);
  r.visible = !r.behind && nearSide >= 0.0 && dot4(farView_, v) >= 0.0;
  if (!r.behind) {
    const double *vp = view_.viewport;
    r.x = vp[0] + (0.5 + 0.5 * (dot4(clip_[0], v) / w)) * vp[2];
    r.y = vp[1] + (0.5 - 0.5 * (dot4(clip_[1], v) / w)) * vp[3];
    r.depth = dot4(clip_[2], v) / w;
  }
  return r;
}

/* In view space; w is 0 for a point that isn't in front of the eye, or is at infinity. */
double4 ViewMath::unprojectToView(double x, double y, double depth) const
{
  const double *vp = view_.viewport;
  const double4 ndc((x - vp[0]) / vp[2] * 2.0 - 1.0, 1.0 - (y - vp[1]) / vp[3] * 2.0, depth, 1.0);
  const double4 h = transformHomogeneous(invProj_, ndc);
  double terms = 0.0;
  for (int c = 0; c < 4; c++) {
    terms += std::fabs(element(invProj_, 3, c) * ndc[c]);
  }
  /* h's w is 1 over the clip w. */
  if (!(h[3] > terms * (16.0 * std::numeric_limits<double>::epsilon()))) {
    return double4(0.0, 0.0, 0.0, 0.0);
  }
  return double4(h[0] / h[3], h[1] / h[3], h[2] / h[3], 1.0);
}

Result<double3> ViewMath::unproject(double x, double y, double depth) const
{
  const double4 v = unprojectToView(x, y, depth);
  const double3 p = transformPoint(invView_, double3(v[0], v[1], v[2]));
  if (!valid_ || v[3] == 0.0 || !bvh::finite(p[0]) || !bvh::finite(p[1]) || !bvh::finite(p[2])) {
    return Status::error(StatusCode::FailedPrecondition,
                         "unproject: no point of the view has this depth");
  }
  return p;
}

Ray ViewMath::rayFromPixel(double x, double y) const
{
  const double4 a = unprojectToView(x, y, nearDepth_), b = unprojectToView(x, y, midDepth_);
  Ray ray;
  ray.origin = transformPoint(invView_, double3(a[0], a[1], a[2]));
  ray.dir = transformVector(invView_, double3(b[0] - a[0], b[1] - a[1], b[2] - a[2]));
  const double len = std::hypot(ray.dir[0], ray.dir[1], ray.dir[2]);
  ray.dir = len > 0.0 ? ray.dir * (1.0 / len) : double3();
  return ray;
}

ViewRegion ViewMath::regionFromPixel(double x, double y, double radius) const
{
  const double r = radius * view_.dpiScale;
  return regionAround(x, y, r, r);
}

ViewRegion ViewMath::regionFromRect(double x0, double y0, double x1, double y1) const
{
  return regionAround(0.5 * (x0 + x1),
                      0.5 * (y0 + y1),
                      0.5 * std::fabs(x1 - x0),
                      0.5 * std::fabs(y1 - y0));
}

ViewRegion ViewMath::regionAround(double x, double y, double halfW, double halfH) const
{
  const double *vp = view_.viewport;
  const double rx = std::fmax(halfW, 0.5) * (1.0 + 1e-9);
  const double ry = std::fmax(halfH, 0.5) * (1.0 + 1e-9);
  const double cx = (x - vp[0]) / vp[2] * 2.0 - 1.0, cy = 1.0 - (y - vp[1]) / vp[3] * 2.0;
  const double hx = 2.0 * rx / vp[2], hy = 2.0 * ry / vp[3];
  const double4 &X = clip_[0], &Y = clip_[1], &W = clip_[3];
  const double4 planes[ViewRegion::kPlanes] = {X - W * (cx - hx),
                                               W * (cx + hx) - X,
                                               Y - W * (cy - hy),
                                               W * (cy + hy) - Y,
                                               nearView_,
                                               farView_};
  ViewRegion out;
  if (!valid_) {
    out.planes[0].w = -1.0; /* nothing is inside */
    return out;
  }
  for (int i = 0; i < ViewRegion::kPlanes; i++) {
    const double4 world = planeToLocal(view_.view, planes[i]);
    double terms = 0.0;
    for (int c = 0; c < 4; c++) {
      terms += std::fabs(element(view_.view, c, 3) * planes[i][c]);
    }
    out.planes[i].n = double3(world[0], world[1], world[2]);
    out.planes[i].w = world[3] + terms * 1e-12;
  }
  return out;
}

double ViewMath::pixelSizeAt(const double3 &p) const
{
  const double4 v = transformHomogeneous(view_.view, double4(p[0], p[1], p[2], 1.0));
  const double w = dot4(clip_[3], v);
  if (!(w > 0.0)) {
    return 0.0;
  }
  /* Pixels per unit of W, along the directions that keep W. */
  const double3 gy(pixelRows_[1][0], pixelRows_[1][1], pixelRows_[1][2]);
  const double3 gw(pixelRows_[2][0], pixelRows_[2][1], pixelRows_[2][2]);
  const double ww = gw.dot(gw);
  const double3 along = ww > 0.0 ? gy - gw * (gy.dot(gw) / ww) : gy;
  return w / along.length() * view_.dpiScale;
}

double ViewMath::viewDistance(const double3 &p) const
{
  const double4 v = transformHomogeneous(view_.view, double4(p[0], p[1], p[2], 1.0));
  return dot4(nearView_, v) * nearScale_;
}

Status ViewMath::unprojectTo(double x, double y, double depth, double3 &out) const
{
  Result<double3> r = unproject(x, y, depth);
  if (r.ok()) {
    out = *r;
  }
  return r.ok() ? Status() : r.status();
}

double3 ViewMath::rayOrigin(double x, double y) const
{
  return rayFromPixel(x, y).origin;
}

double3 ViewMath::rayDirection(double x, double y) const
{
  return rayFromPixel(x, y).dir;
}

double ViewMath::pixelRowAt(int32_t row, int32_t i) const
{
  return row >= 0 && row < 3 && i >= 0 && i < 4 ? pixelRows_[row][i] : 0.0;
}

double ViewMath::nearPlaneAt(int32_t i) const
{
  return i >= 0 && i < 4 ? nearPlane_[i] : 0.0;
}

double ViewMath::farPlaneAt(int32_t i) const
{
  return i >= 0 && i < 4 ? farPlane_[i] : 0.0;
}

} // namespace coredcc
