/* The drag of a transform gizmo's part: the delta from the press, per object when the
 * pivot or the orientation is the objects' own, and the status text. */
#include "coredcc/command/executor.h"
#include "coredcc/scene/commands.h"
#include "coredcc/tool/elements.h"
#include "coredcc/tool/gizmos/transform_gizmo.h"
#include "coredcc/tool/manager.h"

#include "litestl/binding/binding.h"

#include <cmath>
#include <cstdio>
#include <numbers>
#include <string>

namespace coredcc::gizmo {

namespace lbt = litestl::binding::types;
using litestl::util::Vector;

namespace {

const char *const kAxisName[3] = {"X", "Y", "Z"};
const char *const kPlaneName[3] = {"YZ", "XZ", "XY"};

std::string number(double v)
{
  char buf[64];
  snprintf(buf, sizeof buf, "%.4g", std::abs(v) < 5e-13 ? 0.0 : v);
  return buf;
}

bool isAxis(int32_t p)
{
  return p >= part::kX && p <= part::kZ;
}

bool isPlane(int32_t p)
{
  return p >= part::kPlaneX && p <= part::kPlaneZ;
}

} // namespace

void TransformModal::begin(const ViewMath &view)
{
  const Frame &f = frame;
  pressOk = true;
  if (f.mode == Mode::Translate) {
    if (isAxis(part)) {
      Result<double> t = axisParameter(view, press.x, press.y, f.pivot, f.axes[part]);
      pressOk = t.ok();
      pressT = t.ok() ? *t : 0.0;
    } else {
      const double3 n = isPlane(part) ? f.axes[part - part::kPlaneX] : f.eye;
      Result<double3> q = planePoint(view, press.x, press.y, f.pivot, n);
      pressOk = q.ok();
      pressPoint = q.ok() ? *q : f.pivot;
    }
  } else if (f.mode == Mode::Rotate) {
    angle = AngleTracker(f.centre, press);
    handedness = screenHandedness(view, f.pivot);
  }
  last = TransformDelta();
  last.pivot = pivot;
  if (f.mode == Mode::Scale) {
    last.kind = DeltaKind::Scale;
    last.basis = f.basis;
    last.vector = double3(1, 1, 1);
  }
}

bool TransformModal::deltaAt(const ViewMath &view,
                             Pixel p,
                             bool snapping,
                             TransformDelta &out)
{
  const Frame &f = frame;
  out = TransformDelta();
  out.pivot = pivot;
  if (!pressOk) {
    return false;
  }
  if (f.mode == Mode::Translate) {
    out.kind = DeltaKind::Translate;
    if (isAxis(part)) {
      Result<double> t = axisParameter(view, p.x, p.y, f.pivot, f.axes[part]);
      if (!t.ok()) {
        return false;
      }
      double d = *t - pressT;
      d = snapping ? snap(d, settings.moveStep) : d;
      out.vector = f.axes[part] * d;
      amount = d;
      return true;
    }
    const double3 n = isPlane(part) ? f.axes[part - part::kPlaneX] : f.eye;
    Result<double3> q = planePoint(view, p.x, p.y, f.pivot, n);
    if (!q.ok()) {
      return false;
    }
    double3 v = *q - pressPoint;
    if (snapping) {
      /* Each of the gizmo's axes on its own. */
      double3 snapped(0, 0, 0);
      for (int i = 0; i < 3; i++) {
        snapped = snapped + f.axes[i] * snap(v.dot(f.axes[i]), settings.moveStep);
      }
      v = snapped - n * (snapped.dot(n) / n.dot(n)); /* stay in the plane */
    }
    out.vector = v;
    amount = v.length();
    return true;
  }
  if (f.mode == Mode::Rotate) {
    const double turned = angle.update(p) * handedness;
    double3 axis = f.eye;
    double a = turned;
    if (isAxis(part)) {
      axis = f.axes[part];
      /* Seen from where the axis points, a positive turn is counter-clockwise. */
      a = axis.dot(f.eye) >= 0.0 ? turned : -turned;
    }
    a = snapping ? snap(a, settings.angleStep) : a;
    out.kind = DeltaKind::Rotate;
    out.axis = axis;
    out.angle = a;
    amount = a;
    return true;
  }
  /* Scale: along the axis's direction on the screen, or overall. */
  Pixel dir{0.0, 0.0};
  if (isAxis(part)) {
    Pixel tip;
    if (!toPixel(view, f.pivot + f.axes[part] * f.unit, tip)) {
      return false;
    }
    const double len = std::hypot(tip.x - f.centre.x, tip.y - f.centre.y);
    if (!(len > 1.0)) {
      return false;
    }
    dir = Pixel{(tip.x - f.centre.x) / len, (tip.y - f.centre.y) / len};
  }
  double k = scaleFactor(f.centre, press, p, dir);
  k = snapping ? snap(k, settings.scaleStep) : k;
  out.kind = DeltaKind::Scale;
  out.basis = f.basis;
  out.vector = double3(1, 1, 1);
  if (isAxis(part)) {
    out.vector[part] = k;
  } else if (isPlane(part)) {
    int a = (part - part::kPlaneX + 1) % 3, b = (part - part::kPlaneX + 2) % 3;
    out.vector[a] = k;
    out.vector[b] = k;
  } else {
    out.vector = double3(k, k, k);
  }
  amount = k;
  return true;
}

Status TransformModal::perObject(Context &ctx,
                                 ApplyDelta &cmd,
                                 const TransformDelta &d,
                                 Vector<TransformDelta> &out) const
{
  out.clear();
  const bool individual =
      settings.pivot == PivotMode::Individual && d.kind != DeltaKind::Translate;
  const bool local = settings.orientation == Orientation::Local;
  const quatd toGizmo = frame.basis.conjugated();
  for (int i = 0; i < cmd.count(); i++) {
    TransformDelta t = d;
    if (individual) {
      Result<double3> pv = cmd.startPivot(ctx, i);
      CDCC_TRY(pv.status());
      t.pivot = *pv;
    }
    if (local) {
      Result<TransformState> st = cmd.startState(ctx, i);
      CDCC_TRY(st.status());
      const quatd own = st->rotation;
      if (d.kind == DeltaKind::Translate) {
        t.vector = own.rotate(toGizmo.rotate(d.vector));
      } else if (d.kind == DeltaKind::Rotate) {
        t.axis = own.rotate(toGizmo.rotate(d.axis));
      } else {
        t.basis = own;
      }
    }
    out.append(t);
  }
  return Status();
}

ModalResult TransformModal::onEvent(Context &ctx,
                                    ToolManager &tm,
                                    const ViewMath &view,
                                    const InputEvent &e)
{
  if ((e.type == EventType::KeyDown && e.key == keys::kEscape) ||
      (e.type == EventType::PointerDown && e.button == 2))
  {
    return ModalResult::Cancel;
  }
  if (e.type == EventType::Wheel) {
    return ModalResult::PassThrough;
  }
  const bool release = e.type == EventType::PointerUp && e.button == 0;
  if (e.type != EventType::PointerMove && !release) {
    return ModalResult::Continue;
  }
  CommandExecutor *ex = ctx.executor();
  TransformDelta d;
  if (deltaAt(view, Pixel{e.x, e.y}, e.has(modifiers::kCtrl), d)) {
    last = d;
    int locked = 0, driven = 0;
    const bool each = settings.pivot == PivotMode::Individual ||
                      settings.orientation == Orientation::Local;
    Status s = ex->updateInteractive(ctx, [&](Context &c, Command &cmd) {
      if (ElementsDelta *el = cmd.as<ElementsDelta>()) {
        return el->update(c, d); /* elements have no pivots or bases of their own */
      }
      ApplyDelta *a = cmd.as<ApplyDelta>();
      if (!a) {
        return Status::error(StatusCode::Internal, "the open command isn't a drag delta");
      }
      Status u;
      if (each) {
        Vector<TransformDelta> deltas;
        CDCC_TRY(perObject(c, *a, d, deltas));
        u = a->update(
            c, std::span<const TransformDelta>(deltas.data(), size_t(deltas.size())));
      } else {
        u = a->update(c, d);
      }
      locked = a->locked();
      driven = a->driven();
      return u;
    });
    std::string text;
    const char *what = isAxis(part)    ? kAxisName[part]
                       : isPlane(part) ? kPlaneName[part - part::kPlaneX]
                                       : "the view";
    if (frame.mode == Mode::Translate) {
      text = "Move " + number(amount) + (isAxis(part) ? " along " : " in ") + what;
    } else if (frame.mode == Mode::Rotate) {
      text = "Rotate " + number(amount * 180.0 / std::numbers::pi) + "\xc2\xb0 about " +
             what;
    } else {
      text = "Scale " + number(amount) + (isAxis(part) ? " along " : " in ") + what;
    }
    if (locked > 0) {
      text += ", " + std::to_string(locked) + (locked == 1 ? " object" : " objects") +
              " locked";
    }
    if (driven > 0) {
      text += ", " + std::to_string(driven) + (driven == 1 ? " object" : " objects") +
              " driven";
    }
    if (!s.ok()) {
      text += std::string(": ") + s.message();
    }
    tm.setStatus(text);
  }
  if (!release) {
    return ModalResult::Continue;
  }
  Command *cmd = ex->interactiveCommand();
  const ApplyDelta *a = cmd ? cmd->as<ApplyDelta>() : nullptr;
  const ElementsDelta *el = cmd ? cmd->as<ElementsDelta>() : nullptr;
  /* A click that moved nothing makes no step. */
  const bool changed = (a && a->changed()) || (el && el->changed());
  return changed ? ModalResult::Finish : ModalResult::Cancel;
}

void TransformModal::drawOverlay(const ViewMath &view, DrawList &out) const
{
  Placement moved = placement;
  if (frame.mode == Mode::Translate) {
    moved.pivot = moved.pivot + last.vector;
  }
  const Frame f = makeFrame(view, moved, settings);
  f.draw(view, part, true, id, out);
}

void TransformModal::end(Context &, bool)
{
}

lbt::Struct<TransformModal> *TransformModal::defineBindings()
{
  static lbt::Struct<TransformModal> *st = nullptr;
  if (!st) {
    litestl::alloc::PermanentGuard guard;
    st = new lbt::Struct<TransformModal>("coredcc::gizmo::TransformModal",
                                         sizeof(TransformModal));
    BIND_STRUCT_METHOD(st, onEvent, MARGS("ctx", "tm", "view", "e"));
    BIND_STRUCT_METHOD(st, drawOverlay, MARGS("view", "out"));
    BIND_STRUCT_METHOD(st, end, MARGS("ctx", "committed"));
  }
  return st;
}

} // namespace coredcc::gizmo
