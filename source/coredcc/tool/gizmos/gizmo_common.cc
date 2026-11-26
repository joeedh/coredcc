#include "coredcc/tool/gizmos/gizmo_common.h"

#include "coredcc/scene/capabilities.h"
#include "coredcc/scene/evaluated.h"
#include "coredcc/scene/scene.h"
#include "coredcc/tool/elements.h"

#include "litestl/math/transform.h"
#include "litestl/util/map.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace coredcc::gizmo {

using litestl::util::Vector;

namespace {

bool usable(const ObjectEval &ev)
{
  const uint32_t off =
      uint32_t(ObjectFlags::Hidden) | uint32_t(ObjectFlags::Unselectable);
  return ev.status == EvalStatus::Ok && (ev.flags & off) == 0;
}

double3 positionOf(const ObjectEval &ev)
{
  const double *d = ev.state.world;
  return double3(d[12], d[13], d[14]);
}

} // namespace

Placement computePlacement(Context &ctx, PivotMode pivot, Orientation orientation)
{
  Placement out;
  Library *lib = ctx.library();
  Scene *scene = lib ? Scene::of(*lib, ctx.scene()) : nullptr;
  if (!scene) {
    return out;
  }
  const Vector<BlockHandle> &evaluated = scene->evaluatedObjects();
  const std::span<const ObjectEval> results = scene->results();
  /* The selected objects that are usable, by handle. */
  litestl::util::Map<uint64_t, int32_t> chosen;
  Vector<BlockHandle> order;
  Vector<ObjectEval> evals;
  for (BlockHandle h : scene->selected(*lib)) {
    ObjectEval ev;
    if (scene->evaluated(h, ev) && ev.member && usable(ev) &&
        !chosen.lookup_ptr(h.computeHash()))
    {
      chosen.add(h.computeHash(), int32_t(order.size()));
      order.append(h);
      evals.append(ev);
    }
  }
  /* Not below another of them, as ApplyDelta does. A cycle has parent -1. */
  Vector<int32_t> roots;
  for (int i = 0; i < int(order.size()); i++) {
    bool below = false;
    int steps = 0;
    for (int32_t p = evals[i].parent;
         p >= 0 && p < int32_t(results.size()) && steps < int(results.size());
         p = results[p].parent, steps++)
    {
      if (chosen.lookup_ptr(evaluated[p].computeHash())) {
        below = true;
        break;
      }
    }
    if (!below) {
      roots.append(i);
    }
  }
  if (roots.size() == 0) {
    return out;
  }
  out.visible = true;
  out.count = int32_t(roots.size());
  const BlockHandle active = scene->active(*lib);
  int32_t activeAt = -1;
  double3 sum(0.0, 0.0, 0.0);
  for (int32_t i : roots) {
    sum = sum + positionOf(evals[i]);
    if (order[i] == active) {
      activeAt = i;
    }
  }
  out.pivot = sum * (1.0 / double(roots.size()));
  if (pivot == PivotMode::Active && activeAt >= 0) {
    out.pivot = positionOf(evals[activeAt]);
  }
  if (orientation == Orientation::Local) {
    out.basis = evals[activeAt >= 0 ? activeAt : roots[0]].state.rotation;
  }
  return out;
}

Placement computeElementPlacement(Context &ctx, int32_t domain, Orientation orientation)
{
  Placement out;
  Library *lib = ctx.library();
  Scene *scene = lib ? Scene::of(*lib, ctx.scene()) : nullptr;
  if (!scene) {
    return out;
  }
  litestl::util::Map<uint64_t, bool>
      seen; /* a data block shared by objects counts once */
  double3 sum(0.0, 0.0, 0.0);
  int64_t n = 0;
  const BlockHandle active = scene->active(*lib);
  bool haveBasis = false;
  ElementSet set;
  for (BlockHandle h : scene->selected(*lib)) {
    ObjectEval ev;
    if (!scene->evaluated(h, ev) || !ev.member || !usable(ev)) {
      continue;
    }
    const BlockHandle data = detail::elementsBlock(*lib, h);
    if (!data || seen.lookup_ptr(data.computeHash())) {
      continue;
    }
    seen.add(data.computeHash(), true);
    set.clear();
    const TransformableElements te = lib->query<TransformableElements>(data);
    if (!te.gather(domain, set).ok() || ctx.runtime().hasStickyError()) {
      ctx.runtime().takeStickyError().ignore(); /* drawn without it */
      continue;
    }
    for (int i = 0; i < set.size(); i++) {
      const double3 p(
          set.positions[3 * i], set.positions[3 * i + 1], set.positions[3 * i + 2]);
      sum = sum + litestl::math::transformPoint(ev.dataWorld(), p);
      n++;
    }
    if (set.size() > 0 && orientation == Orientation::Local &&
        (!haveBasis || h == active))
    {
      out.basis = ev.state.rotation;
      haveBasis = true;
    }
  }
  if (n == 0) {
    return Placement();
  }
  out.visible = true;
  out.count = int32_t(n);
  out.pivot = sum * (1.0 / double(n));
  return out;
}

quatd basisFor(const Placement &p, Orientation o, const ViewMath &view)
{
  if (o != Orientation::View) {
    return p.basis;
  }
  /* The view's axes in the world: right, up, and toward the eye. */
  mat4d inv;
  if (!litestl::math::tryInverse(view.view().view, inv)) {
    return quatd();
  }
  const double *d = inv;
  double3 x(d[0], d[1], d[2]), y(d[4], d[5], d[6]);
  if (x.normalize() == 0.0 || y.normalize() == 0.0) {
    return quatd();
  }
  y = y - x * x.dot(y);
  y.normalize();
  const double3 z = x.cross(y);
  mat4d m;
  m.identity();
  double *md = m;
  for (int i = 0; i < 3; i++) {
    md[i] = x[i];
    md[4 + i] = y[i];
    md[8 + i] = z[i];
  }
  return quatd::fromMatrix(m);
}

double unitAt(const ViewMath &view, const double3 &pivot, double size)
{
  return view.pixelSizeAt(pivot) * size;
}

double axisFade(const double3 &axis, const double3 &towardEye)
{
  const double idot = 1.0 - std::abs(axis.dot(towardEye));
  return std::clamp(
      (idot - look::kAxisHide) / (look::kAxisShow - look::kAxisHide), 0.0, 1.0);
}

double planeFade(const double3 &normal, const double3 &towardEye)
{
  const double idot = std::abs(normal.dot(towardEye));
  return std::clamp(
      (idot - look::kPlaneHide) / (look::kPlaneShow - look::kPlaneHide), 0.0, 1.0);
}

uint32_t withAlpha(uint32_t rgb, double alpha)
{
  const uint32_t a = uint32_t(std::lround(std::clamp(alpha, 0.0, 1.0) * 255.0));
  return (rgb & 0x00ffffffu) | (a << 24);
}

double segmentDistance(Pixel p, Pixel a, Pixel b)
{
  const double dx = b.x - a.x, dy = b.y - a.y;
  const double len2 = dx * dx + dy * dy;
  double t = len2 > 0.0 ? ((p.x - a.x) * dx + (p.y - a.y) * dy) / len2 : 0.0;
  t = std::clamp(t, 0.0, 1.0);
  return std::hypot(p.x - (a.x + dx * t), p.y - (a.y + dy * t));
}

double polylineDistance(Pixel p, const Vector<Pixel> &line, bool closed)
{
  double best = std::numeric_limits<double>::infinity();
  const int n = int(line.size());
  for (int i = 0; i + 1 < n; i++) {
    best = std::min(best, segmentDistance(p, line[i], line[i + 1]));
  }
  if (closed && n > 2) {
    best = std::min(best, segmentDistance(p, line[n - 1], line[0]));
  }
  if (n == 1) {
    best = std::hypot(p.x - line[0].x, p.y - line[0].y);
  }
  return best;
}

double polygonDistance(Pixel p, const Vector<Pixel> &poly)
{
  const int n = int(poly.size());
  if (n < 3) {
    return polylineDistance(p, poly, false);
  }
  int pos = 0, neg = 0;
  for (int i = 0; i < n; i++) {
    const Pixel a = poly[i], b = poly[(i + 1) % n];
    const double c = (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
    pos += c > 0.0 ? 1 : 0;
    neg += c < 0.0 ? 1 : 0;
  }
  return pos == 0 || neg == 0 ? 0.0 : polylineDistance(p, poly, true);
}

void frontArc(const ViewMath &view,
              const double3 &centre,
              const double3 &normal,
              double radius,
              Vector<double3> &points,
              Vector<int32_t> &arcs)
{
  constexpr int kSegments = 64;
  points.clear();
  arcs.clear();
  double3 n = normal;
  if (n.normalize() == 0.0) {
    return;
  }
  double3 u = std::abs(n[0]) < 0.9 ? double3(1, 0, 0) : double3(0, 1, 0);
  u = u - n * u.dot(n);
  u.normalize();
  const double3 v = n.cross(u);
  const double3 eye = towardEye(view, centre);
  /* Start where the circle goes behind, so that the front is one run. */
  double start = std::atan2(-eye.dot(u), eye.dot(v)) - std::numbers::pi * 0.5;
  bool inRun = false;
  for (int i = 0; i <= kSegments; i++) {
    const double a = start + 2.0 * std::numbers::pi * double(i) / kSegments;
    const double3 dir = u * std::cos(a) + v * std::sin(a);
    if (dir.dot(eye) >= -1e-9) {
      if (!inRun) {
        arcs.append(int32_t(points.size()));
        inRun = true;
      }
      points.append(centre + dir * radius);
    } else {
      inRun = false;
    }
  }
}

} // namespace coredcc::gizmo
