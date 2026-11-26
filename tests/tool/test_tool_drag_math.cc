/* The geometry of gizmo drags (tool/gizmos/drag_math.h) against every View convention, at
 * scene offsets up to 1e9: a cursor moved by a known world amount gives it back; turns
 * accumulate; the sign of a turn is the world's; hidden axes can't be hit. */
#include "coredcc/tool/gizmos/drag_math.h"
#include "coredcc/tool/gizmos/gizmo_common.h"
#include "coredcc/tool/gizmos/transform_gizmo.h"

#include "fuzz.h"
#include "litestl/tests/test_util.h"
#include "view_cases.h"

#include <cmath>
#include <cstdio>
#include <numbers>

test_init;

using namespace coredcc;
using namespace coredcc::gizmo;
using view_cases::Case;
using view_cases::Convention;
using view_cases::Rand;

namespace {

constexpr double kOffsets[] = {0.0, 1e3, 1e6, 1e9};
/* The largest error seen at each offset, relative to the move, for the report. */
double g_worst[4] = {0, 0, 0, 0};
int g_cases = 0;
int g_turns = 0;

double3 unit(Rand &r)
{
  double3 v = r.vec(1.0);
  while (v.normalize() < 0.1) {
    v = r.vec(1.0);
  }
  return v;
}

/* A point's pixel, if it is between the near and the far plane. */
bool seen(const ViewMath &m, const double3 &p, Pixel &out)
{
  return m.project(p).visible && toPixel(m, p, out);
}

/* A move along the screen of 40 to 200 pixels, as a world vector at p in direction d. */
double moveFor(const ViewMath &m, const double3 &p, Rand &r)
{
  return m.pixelSizeAt(p) * r.range(40.0, 200.0) * (r.coin() ? 1.0 : -1.0);
}

void testCase(const Convention &c, Rand &r, int oi)
{
  const double offset = kOffsets[oi];
  const Case k = view_cases::makeCase(c, r, offset);
  ViewMath m;
  if (!k.v.validate().ok() || !m.set(k.v).ok()) {
    test_assert(false);
    return;
  }
  const double maxDistance = (std::min)(k.zFar - k.zNear, 50.0);
  const double3 pivot = view_cases::pointAt(
      k, m, r.range(-0.5, 0.5), r.range(-0.5, 0.5), r.range(1.0, maxDistance));
  const double3 eye = towardEye(m, pivot);
  test_assert(std::abs(eye.length() - 1.0) < 1e-12);
  /* Pixels are as exact as the projection; this allows for the offset's ulp. */
  const double slack = 1e-9 + 4.0 * offset * 2.2e-16;
  Pixel press, now;

  /* Along an axis that isn't hidden. */
  double3 axis = unit(r);
  if (axisFade(axis, eye) > 0.0 && toPixel(m, pivot, press)) {
    const double a = moveFor(m, pivot, r), d = moveFor(m, pivot, r);
    if (seen(m, pivot + axis * a, press) && seen(m, pivot + axis * (a + d), now)) {
      Result<double> t0 = axisParameter(m, press.x, press.y, pivot, axis);
      Result<double> t1 = axisParameter(m, now.x, now.y, pivot, axis);
      test_assert(t0.ok() && t1.ok());
      if (t0.ok() && t1.ok()) {
        const double err = std::abs((*t1 - *t0) - d);
        g_worst[oi] = (std::max)(g_worst[oi], err / std::abs(d));
        test_assert(err <= std::abs(d) * 1e-6 + slack * 1e3);
      }
    }
  }
  /* In a plane that isn't edge-on. */
  double3 n = unit(r);
  if (planeFade(n, eye) > 0.0) {
    double3 u = unit(r);
    u = u - n * u.dot(n);
    if (u.normalize() > 0.1) {
      const double3 v = n.cross(u);
      const double3 a = u * moveFor(m, pivot, r);
      const double3 d = u * moveFor(m, pivot, r) + v * moveFor(m, pivot, r);
      if (seen(m, pivot + a, press) && seen(m, pivot + a + d, now)) {
        Result<double3> q0 = planePoint(m, press.x, press.y, pivot, n);
        Result<double3> q1 = planePoint(m, now.x, now.y, pivot, n);
        test_assert(q0.ok() && q1.ok());
        if (q0.ok() && q1.ok()) {
          const double err = ((*q1 - *q0) - d).length();
          g_worst[oi] = (std::max)(g_worst[oi], err / d.length());
          test_assert(err <= d.length() * 1e-6 + slack * 1e3);
        }
      }
    }
  }
  /* A turn's sign is the world's, for axes that face the eye or away. */
  axis = unit(r);
  if (std::abs(axis.dot(eye)) > 0.3) {
    double3 u = unit(r);
    u = u - axis * u.dot(axis);
    if (u.normalize() > 0.1) {
      const double s = m.pixelSizeAt(pivot) * 100.0;
      const double alpha = r.range(0.05, 0.5) * (r.coin() ? 1.0 : -1.0);
      const quatd q = quatd::fromAxisAngle(axis, alpha);
      Pixel center;
      /* The whole circle must be in view: a projection of one that goes behind the eye
       * has no orientation. */
      bool whole = true;
      for (int i = 0; i < 16 && whole; i++) {
        Pixel ignored;
        const quatd step = quatd::fromAxisAngle(axis, 2.0 * std::numbers::pi * i / 16.0);
        whole = seen(m, pivot + step.rotate(u) * s, ignored);
      }
      if (whole && seen(m, pivot, center) && seen(m, pivot + u * s, press) &&
          seen(m, pivot + q.rotate(u) * s, now))
      {
        g_turns++;
        AngleTracker t(center, press);
        const double turned = t.update(now) * screenHandedness(m, pivot);
        const double a = axis.dot(eye) >= 0.0 ? turned : -turned;
        test_assert((a > 0.0) == (alpha > 0.0));
      }
    }
  }
  g_cases++;
}

void testConventions()
{
  Rand r(7);
  const int reps = 6 * coredcc_test::testScale();
  for (int bits = 0; bits < 64; bits++) {
    Convention c;
    if (!view_cases::conventionOf(bits, c)) {
      continue;
    }
    for (int i = 0; i < reps; i++) {
      for (int oi = 0; oi < 4; oi++) {
        testCase(c, r, oi);
      }
    }
  }
  test_assert(g_turns > g_cases / 4); /* most turns are checked */
  printf("drag math: %d cases, %d turns; worst relative error at 0, 1e3, 1e6, 1e9: %.3g "
         "%.3g %.3g %.3g\n",
         g_cases,
         g_turns,
         g_worst[0],
         g_worst[1],
         g_worst[2],
         g_worst[3]);
}

void testTurnsAccumulate()
{
  const Pixel center{400, 300};
  AngleTracker t(center, Pixel{500, 300});
  for (int i = 1; i <= 200; i++) {
    const double a = 4.0 * std::numbers::pi * i / 200.0;
    t.update(Pixel{center.x + 100 * std::cos(a), center.y - 100 * std::sin(a)});
  }
  test_assert(std::abs(t.total() - 4.0 * std::numbers::pi) < 1e-9); /* 720° */
  /* And back, clockwise, through the start. */
  for (int i = 199; i >= -50; i--) {
    const double a = 4.0 * std::numbers::pi * i / 200.0;
    t.update(Pixel{center.x + 100 * std::cos(a), center.y - 100 * std::sin(a)});
  }
  test_assert(std::abs(t.total() + std::numbers::pi) < 1e-9);
  /* On the center: the last angle stays. */
  test_assert(std::abs(t.update(center) + std::numbers::pi) < 1e-9);
}

void testScaleAndSnap()
{
  const Pixel c{100, 100};
  test_assert(scaleFactor(c, Pixel{150, 100}, Pixel{200, 100}, Pixel{0, 0}) == 2.0);
  test_assert(scaleFactor(c, Pixel{150, 100}, Pixel{50, 100}, Pixel{1, 0}) == -1.0);
  test_assert(scaleFactor(c, Pixel{150, 100}, Pixel{150, 150}, Pixel{1, 0}) == 1.0);
  test_assert(scaleFactor(c, Pixel{100.5, 100}, Pixel{300, 100}, Pixel{0, 0}) == 1.0);
  test_assert(snap(1.26, 0.25) == 1.25 && snap(-0.13, 0.25) == -0.25);
  test_assert(snap(1.26, 0.0) == 1.26);
}

/* An axis pointing at the eye, or a plane seen edge-on, can't be hit, and isn't drawn. */
void testHidden()
{
  Rand r(11);
  for (int bits = 0; bits < 64; bits++) {
    Convention c;
    if (!view_cases::conventionOf(bits, c)) {
      continue;
    }
    const Case k = view_cases::makeCase(c, r, 0.0);
    ViewMath m;
    test_assert(m.set(k.v).ok());
    Placement p;
    p.visible = true;
    p.pivot = view_cases::pointAt(k, m, 0.1, -0.1, 5.0);
    const double3 eye = towardEye(m, p.pivot);
    /* A basis whose x is the direction toward the eye. */
    const double3 x(1, 0, 0);
    double3 axis = x.cross(eye);
    const double angle = std::acos(std::clamp(x.dot(eye), -1.0, 1.0));
    p.basis = axis.normalize() > 1e-9 ? quatd::fromAxisAngle(axis, angle) : quatd();
    TransformSettings s;
    const Frame f = makeFrame(m, p, s);
    test_assert(f.onScreen && f.axisShown[0] == 0.0 && f.planeShown[1] == 0.0 &&
                f.planeShown[2] == 0.0);
    const double inf = std::numeric_limits<double>::infinity();
    test_assert(f.distance(m, part::kX, f.center) == inf);
    test_assert(f.distance(m, part::kPlaneY, f.center) == inf);
    test_assert(f.distance(m, part::kPlaneX, f.center) < inf);
    DrawList out;
    test_assert(out.begin(p.pivot).ok());
    f.draw(m, part::kX, false, 1, out);
    f.draw(m, part::kPlaneZ, false, 1, out);
    test_assert(out.recordCount() == 0);
    f.draw(m, part::kY, false, 1, out);
    test_assert(out.recordCount() == 1 && out.end().ok());
  }
}

} // namespace

int main()
{
  testConventions();
  testTurnsAccumulate();
  testScaleAndSnap();
  testHidden();
  return test_end();
}
