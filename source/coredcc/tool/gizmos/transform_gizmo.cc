/* The transform gizmo: its geometry on the screen, hit tests, drawing, and the start of a
 * drag. The drag itself is in transform_modal.cc. */
#include "coredcc/tool/gizmos/transform_gizmo.h"

#include "coredcc/command/executor.h"
#include "coredcc/scene/commands.h"
#include "coredcc/scene/scene.h"
#include "coredcc/tool/elements.h"
#include "coredcc/tool/settings.h"

#include "litestl/binding/binding.h"

#include <cmath>
#include <limits>

namespace coredcc::gizmo {

namespace lbt = litestl::binding::types;
using litestl::util::Vector;

namespace {

constexpr double kInf = std::numeric_limits<double>::infinity();
/* In units of the gizmo's size. */
constexpr double kAxisStart = 0.25;
constexpr double kPlaneMid = 0.35, kPlaneHalf = 0.1;
constexpr double kCentreRadius = 0.2;
constexpr double kViewRing = 1.2;
constexpr double kScaleHead = 0.95, kScaleCube = 0.1;

double
settingFloat(ToolSettingsStore &st, const ToolDef &def, const char *name, double dflt)
{
  const PropValue *v = st.get(def, name);
  const std::optional<double> f = v ? v->tryFloat() : std::nullopt;
  return f ? *f : dflt;
}

int64_t settingEnum(ToolSettingsStore &st, const ToolDef &def, const char *name)
{
  const PropValue *v = st.get(def, name);
  const std::optional<int64_t> e = v ? v->tryEnum() : std::nullopt;
  return e ? *e : 0;
}

/* A matrix from a shape's space: x and y scaled by `across`, z along `dir` by `along`. */
mat4d along(const double3 &origin, const double3 &dir, double across, double alongLen)
{
  double3 z = dir;
  z.normalize();
  double3 x = std::abs(z[0]) < 0.9 ? double3(1, 0, 0) : double3(0, 1, 0);
  x = x - z * x.dot(z);
  x.normalize();
  const double3 y = z.cross(x);
  mat4d m;
  m.identity();
  double *d = m;
  for (int i = 0; i < 3; i++) {
    d[i] = x[i] * across;
    d[4 + i] = y[i] * across;
    d[8 + i] = z[i] * alongLen;
    d[12 + i] = origin[i];
  }
  return m;
}

/* The two axes other than i. */
void others(int i, int &a, int &b)
{
  a = (i + 1) % 3;
  b = (i + 2) % 3;
}

void planeCorners(const Frame &f, int i, double3 out[4])
{
  int a, b;
  others(i, a, b);
  const double3 mid = f.pivot + (f.axes[a] + f.axes[b]) * (kPlaneMid * f.unit);
  const double3 da = f.axes[a] * (kPlaneHalf * f.unit),
                db = f.axes[b] * (kPlaneHalf * f.unit);
  out[0] = mid - da - db;
  out[1] = mid + da - db;
  out[2] = mid + da + db;
  out[3] = mid - da + db;
}

DrawStyle styleOf(uint32_t color, float width, uint32_t id)
{
  DrawStyle s;
  s.color = color;
  s.width = width;
  s.depthMode = DepthMode::Always;
  s.id = id;
  return s;
}

} // namespace

/* ---- settings ---- */

TransformSettings TransformSettings::read(Runtime &rt, const ToolDef &def)
{
  ToolSettingsStore &st = rt.ext<ToolSettingsStore>();
  TransformSettings s;
  s.mode = Mode(settingEnum(st, def, "mode"));
  s.orientation = Orientation(settingEnum(st, def, "orientation"));
  s.pivot = PivotMode(settingEnum(st, def, "pivot"));
  s.moveStep = settingFloat(st, def, "moveStep", s.moveStep);
  s.angleStep = settingFloat(st, def, "angleStep", s.angleStep);
  s.scaleStep = settingFloat(st, def, "scaleStep", s.scaleStep);
  s.size = settingFloat(st, def, "size", s.size);
  s.elements = settingEnum(st, def, "target") == 1;
  s.domain = int32_t(settingEnum(st, def, "domain"));
  const PropValue *sym = st.get(def, "symmetry");
  s.symmetry = sym && sym->tryBool().value_or(false);
  return s;
}

/* ---- the frame ---- */

Frame makeFrame(const ViewMath &view,
                const Placement &placement,
                const TransformSettings &s)
{
  Frame f;
  f.mode = s.mode;
  f.pivot = placement.pivot;
  f.basis = basisFor(placement, s.orientation, view);
  f.size = s.size;
  f.dpi = view.view().dpiScale > 0.0 ? view.view().dpiScale : 1.0;
  f.unit = unitAt(view, f.pivot, s.size);
  f.eye = towardEye(view, f.pivot);
  f.onScreen = placement.visible && f.unit > 0.0 && toPixel(view, f.pivot, f.centre);
  for (int i = 0; i < 3; i++) {
    double3 e(0, 0, 0);
    e[i] = 1.0;
    f.axes[i] = f.basis.rotate(e);
    f.axes[i].normalize();
  }
  for (int i = 0; i < 3; i++) {
    f.axisShown[i] = axisFade(f.axes[i], f.eye);
    f.planeShown[i] = planeFade(f.axes[i], f.eye);
  }
  return f;
}

bool Frame::hasPart(int32_t p) const
{
  if (p < 0 || p >= part::kCount) {
    return false;
  }
  if (mode == Mode::Rotate) {
    return p <= part::kZ || p == part::kView;
  }
  return p != part::kView;
}

double Frame::distance(const ViewMath &view, int32_t p, Pixel px) const
{
  if (!onScreen || !hasPart(p)) {
    return kInf;
  }
  if (p <= part::kZ && mode != Mode::Rotate) {
    if (axisShown[p] <= 0.0) {
      return kInf;
    }
    Pixel a, b;
    const double end = mode == Mode::Scale ? kScaleHead + kScaleCube * 0.5 : 1.0;
    if (!toPixel(view, pivot + axes[p] * (kAxisStart * unit), a) ||
        !toPixel(view, pivot + axes[p] * (end * unit), b))
    {
      return kInf;
    }
    /* The cone or the cube is about 4 logical pixels wide. */
    return std::max(0.0, segmentDistance(px, a, b) / dpi - 4.0);
  }
  if (p >= part::kPlaneX && p <= part::kPlaneZ) {
    const int i = p - part::kPlaneX;
    if (planeShown[i] <= 0.0) {
      return kInf;
    }
    double3 c[4];
    planeCorners(*this, i, c);
    Vector<Pixel> poly;
    for (const double3 &q : c) {
      Pixel pp;
      if (!toPixel(view, q, pp)) {
        return kInf;
      }
      poly.append(pp);
    }
    return polygonDistance(px, poly) / dpi;
  }
  const double d = std::hypot(px.x - centre.x, px.y - centre.y) / dpi;
  if (p == part::kCentre) {
    return std::max(0.0, d - kCentreRadius * size);
  }
  if (p == part::kView) {
    return std::max(0.0, std::abs(d - kViewRing * size) - look::kRingWidth);
  }
  /* A rotation ring: its front half. */
  Vector<double3> pts;
  Vector<int32_t> arcs;
  frontArc(view, pivot, axes[p], unit, pts, arcs);
  double best = kInf;
  for (int a = 0; a < int(arcs.size()); a++) {
    const int from = arcs[a],
              to = a + 1 < int(arcs.size()) ? arcs[a + 1] : int(pts.size());
    Vector<Pixel> line;
    for (int k = from; k < to; k++) {
      Pixel pp;
      if (toPixel(view, pts[k], pp)) {
        line.append(pp);
      }
    }
    best = std::min(best, polylineDistance(px, line, false) / dpi);
  }
  return std::max(0.0, best - look::kRingWidth);
}

void Frame::draw(
    const ViewMath &view, int32_t p, bool hi, uint32_t id, DrawList &out) const
{
  if (!onScreen || !hasPart(p)) {
    return;
  }
  const double alpha = hi ? look::kAlphaHi : look::kAlpha;
  if (p <= part::kZ && mode != Mode::Rotate) {
    if (axisShown[p] <= 0.0) {
      return;
    }
    const uint32_t c = withAlpha(look::kAxis[p], alpha * axisShown[p]);
    const double3 start = pivot + axes[p] * (kAxisStart * unit);
    if (mode == Mode::Translate) {
      out.shape(styleOf(c, look::kLineWidth, id),
                ShapeKind::Arrow,
                along(start, axes[p], unit, (1.0 - kAxisStart) * unit))
          .ignore();
    } else {
      const double3 seg[2] = {start,
                              pivot + axes[p] * ((kScaleHead - kScaleCube * 0.5) * unit)};
      out.lines(styleOf(c, look::kLineWidth, id), std::span<const double3>(seg, 2))
          .ignore();
      out.shape(styleOf(c, look::kLineWidth, id),
                ShapeKind::Cube,
                along(pivot + axes[p] * (kScaleHead * unit),
                      axes[p],
                      kScaleCube * unit,
                      kScaleCube * unit))
          .ignore();
    }
    return;
  }
  if (p >= part::kPlaneX && p <= part::kPlaneZ) {
    const int i = p - part::kPlaneX;
    if (planeShown[i] <= 0.0) {
      return;
    }
    double3 c[4];
    planeCorners(*this, i, c);
    const double3 tris[6] = {c[0], c[1], c[2], c[0], c[2], c[3]};
    out.triangles(styleOf(withAlpha(look::kAxis[i], alpha * planeShown[i]), 1.0f, id),
                  std::span<const double3>(tris, 6))
        .ignore();
    return;
  }
  if (p == part::kCentre || p == part::kView) {
    const double r = (p == part::kCentre ? kCentreRadius : kViewRing) * size;
    out.circle2d(styleOf(withAlpha(look::kWhite, alpha), look::kLineWidth, id),
                 centre.x,
                 centre.y,
                 r,
                 false)
        .ignore();
    return;
  }
  Vector<double3> pts;
  Vector<int32_t> arcs;
  frontArc(view, pivot, axes[p], unit, pts, arcs);
  Vector<double3> segs;
  for (int a = 0; a < int(arcs.size()); a++) {
    const int from = arcs[a],
              to = a + 1 < int(arcs.size()) ? arcs[a + 1] : int(pts.size());
    for (int k = from; k + 1 < to; k++) {
      segs.append(pts[k]);
      segs.append(pts[k + 1]);
    }
  }
  out.lines(styleOf(withAlpha(look::kAxis[p], alpha), look::kRingWidth, id),
            std::span<const double3>(segs.data(), size_t(segs.size())))
      .ignore();
}

namespace {

/* The pivot as the begun command found the objects or the elements. A command since the
 * last evaluation may have moved them, so `placement` may be stale. */
double3 pivotOfDrag(Context &ctx, const TransformSettings &s, const Placement &placement)
{
  CommandExecutor *ex = ctx.executor();
  Command *cmd = ex ? ex->interactiveCommand() : nullptr;
  if (const ElementsDelta *el = cmd ? cmd->as<ElementsDelta>() : nullptr) {
    double3 c;
    return el->centre(c) ? c : placement.pivot;
  }
  const ApplyDelta *a = cmd ? cmd->as<ApplyDelta>() : nullptr;
  Library *lib = ctx.library();
  Scene *scene = lib ? Scene::of(*lib, ctx.scene()) : nullptr;
  if (!a || !scene) {
    return placement.pivot;
  }
  const BlockHandle active = scene->active(*lib);
  double3 sum(0.0, 0.0, 0.0);
  int n = 0;
  for (int i = 0; i < a->objectCount(); i++) {
    Result<TransformState> st = a->startState(ctx, i);
    if (!st.ok()) {
      continue;
    }
    const double *w = st->world;
    const double3 at(w[12], w[13], w[14]);
    if (s.pivot == PivotMode::Active && a->object(i) == active) {
      return at;
    }
    sum = sum + at;
    n++;
  }
  return n > 0 ? sum * (1.0 / double(n)) : placement.pivot;
}

} // namespace

/* ---- the gizmo ---- */

void TransformGizmo::update(Context &ctx)
{
  if (!tool) {
    placement = Placement();
    return;
  }
  settings = TransformSettings::read(ctx.runtime(), *tool);
  placement = settings.elements
                  ? computeElementPlacement(ctx, settings.domain, settings.orientation)
                  : computePlacement(ctx, settings.pivot, settings.orientation);
}

GizmoHit TransformGizmo::hitTest(const ViewMath &view, double x, double y) const
{
  GizmoHit hit;
  const Frame f = makeFrame(view, placement, settings);
  if (!f.onScreen) {
    return hit;
  }
  /* Ties go to the first: the centre, then the axes, the planes and the view ring. */
  static constexpr int32_t kOrder[part::kCount] = {part::kCentre,
                                                   part::kX,
                                                   part::kY,
                                                   part::kZ,
                                                   part::kPlaneX,
                                                   part::kPlaneY,
                                                   part::kPlaneZ,
                                                   part::kView};
  double best = kInf;
  for (int32_t p : kOrder) {
    const double d = f.distance(view, p, Pixel{x, y});
    if (d < best) {
      best = d;
      hit.part = p;
    }
  }
  if (hit.part >= 0) {
    hit.distance = best;
    hit.viewDistance = view.viewDistance(f.pivot);
  }
  return hit;
}

void TransformGizmo::draw(const ViewMath &view,
                          int32_t hover,
                          int32_t active,
                          uint32_t idBase,
                          DrawList &out) const
{
  if (active >= 0) {
    return; /* the drag's modal draws what moves */
  }
  const Frame f = makeFrame(view, placement, settings);
  for (int32_t p = 0; p < part::kCount; p++) {
    f.draw(view, p, p == hover, idBase + uint32_t(p), out);
  }
}

Status TransformGizmo::beginDrag(
    Context &ctx, ToolManager &tm, const ViewMath &view, const InputEvent &e, int32_t p)
{
  CommandExecutor *ex = ctx.executor();
  const CommandDef *def = ctx.runtime().ext<CommandRegistry>().find(
      settings.elements ? kDragElementDeltaCommand : kDragObjectDeltaCommand);
  if (!ex || !def) {
    return Status::error(
        StatusCode::FailedPrecondition,
        "the transform gizmo needs an executor and the scene's commands");
  }
  PropertyBag inputs = def->newInputs();
  if (settings.elements) {
    CDCC_TRY(inputs.set("domain", PropValue::ofEnum(settings.domain)));
    CDCC_TRY(inputs.set("symmetry", PropValue::ofBool(settings.symmetry)));
  }
  CDCC_TRY(ex->beginInteractive(ctx, *def, std::move(inputs)));
  TransformModal *m =
      litestl::alloc::New<TransformModal>("coredcc::gizmo::TransformModal");
  m->settings = settings;
  m->placement = placement;
  m->frame = makeFrame(view, placement, settings);
  m->pivot = pivotOfDrag(ctx, settings, placement);
  m->part = p;
  m->id = uint32_t(tm.hoverGizmo()) * ToolManager::kIdStride + uint32_t(p);
  m->press = Pixel{e.x, e.y};
  m->begin(view);
  Result<Owned<Modal>> modal = own<Modal>(ctx.runtime(), kTransformModalImpl, m);
  if (!modal.ok()) {
    return modal.status(); /* the manager cancels the command it sees open */
  }
  return tm.pushModal(ctx, std::move(modal).value(), true);
}

lbt::Struct<TransformGizmo> *TransformGizmo::defineBindings()
{
  static lbt::Struct<TransformGizmo> *st = nullptr;
  if (!st) {
    litestl::alloc::PermanentGuard guard;
    st = new lbt::Struct<TransformGizmo>("coredcc::gizmo::TransformGizmo",
                                         sizeof(TransformGizmo));
    BIND_STRUCT_METHOD(st, update, MARGS("ctx"));
    BIND_STRUCT_METHOD(st, hitTest, MARGS("view", "x", "y"));
    BIND_STRUCT_METHOD(st, draw, MARGS("view", "hover", "active", "idBase", "out"));
    BIND_STRUCT_METHOD(st, beginDrag, MARGS("ctx", "tm", "view", "e", "part"));
  }
  return st;
}

} // namespace coredcc::gizmo
