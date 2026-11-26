/* The headless host drags the transform gizmo: translate, rotate and scale are each one
 * undo step that undo and redo exactly; clicks make none; cancels leave the channels as
 * they were; locked channels are named; children of selected objects move once; the
 * gizmo keeps its size on the screen. */
#include "tool_fixture.h"

#include "coredcc/api/ref_text.h"

#include <cmath>
#include <numbers>

test_init;

using namespace tool_test;

namespace {

/* A pixel on part `p` of the gizmo now, `t` of the way along an axis. */
Pixel onAxis(ToolDoc &d, int axis, double t = 0.7)
{
  const Frame f = d.gizmoFrame();
  return d.pixelOf(f.pivot + f.axes[axis] * (f.unit * t));
}

/* The pixel `px` pixels further along the axis's direction on the screen. */
Pixel alongAxis(ToolDoc &d, int axis, Pixel from, double px)
{
  const Frame f = d.gizmoFrame();
  const Pixel tip = d.pixelOf(f.pivot + f.axes[axis] * f.unit);
  const double dx = tip.x - f.centre.x, dy = tip.y - f.centre.y, len = std::hypot(dx, dy);
  return Pixel{from.x + dx / len * px, from.y + dy / len * px};
}

void drag(ToolDoc &d, Pixel a, Pixel b, int steps = 5, uint32_t mods = 0)
{
  must(d.send(EventType::PointerDown, a.x, a.y, mods).status());
  for (int i = 1; i <= steps; i++) {
    const double f = double(i) / steps;
    must(
        d.send(EventType::PointerMove, a.x + (b.x - a.x) * f, a.y + (b.y - a.y) * f, mods)
            .status());
  }
  must(d.send(EventType::PointerUp, b.x, b.y, mods).status());
}

void testTranslate()
{
  ToolDoc d;
  const BlockHandle a = d.box("a", 1, 2, 0);
  d.select({a});
  d.useTool(kTransformTool);
  const std::string before = d.describe();
  const Frame f = d.gizmoFrame();
  test_assert(f.onScreen && f.pivot[0] == 1.0 && f.pivot[1] == 2.0);

  /* Hovering the arrow highlights it. */
  const Pixel press = onAxis(d, 0);
  must(d.send(EventType::PointerMove, press.x, press.y).status());
  test_assert(d.tm.hoverGizmo() == 1 && d.tm.hoverPart() == part::kX);
  const Pixel to = alongAxis(d, 0, press, 100);
  /* What the drag's math gives for that move. */
  const double t0 = *axisParameter(d.vm, press.x, press.y, f.pivot, f.axes[0]);
  const double t1 = *axisParameter(d.vm, to.x, to.y, f.pivot, f.axes[0]);
  drag(d, press, to);
  test_assert(d.history->size() == 1 && !d.ex->interactiveOpen());
  const double3 p = d.position(a);
  test_assert(std::abs(p[0] - (1.0 + (t1 - t0))) < 1e-12 && p[1] == 2.0 && p[2] == 0.0);
  test_assert(t1 - t0 > 0.1);
  const std::string after = d.describe();
  /* One step: undo puts it back exactly, redo moves it again exactly. */
  must(d.ex->undo(*d.ctx));
  test_assert(d.describe() == before);
  must(d.ex->redo(*d.ctx));
  test_assert(d.describe() == after);
  test_assert(d.tm.status().size() == 0); /* emptied when the gesture ended */

  /* Ctrl snaps to moveStep. */
  must(d.ex->undo(*d.ctx));
  d.evaluate();
  const Pixel press2 = onAxis(d, 0);
  drag(d, press2, alongAxis(d, 0, press2, 150), 3, modifiers::kCtrl);
  const double moved = d.position(a)[0] - 1.0;
  test_assert(moved == std::round(moved) && moved >= 1.0);

  /* In the view plane: the centre. The object ends under the cursor's move. */
  must(d.ex->undo(*d.ctx));
  d.evaluate();
  const Frame g = d.gizmoFrame();
  drag(d, g.centre, Pixel{g.centre.x + 60, g.centre.y + 20});
  const Pixel now = d.pixelOf(d.position(a));
  test_assert(std::abs(now.x - (g.centre.x + 60)) < 1e-6 &&
              std::abs(now.y - (g.centre.y + 20)) < 1e-6);
}

void testClickAndCancel()
{
  ToolDoc d;
  const BlockHandle a = d.box("a");
  d.select({a});
  d.useTool(kTransformTool);
  const std::string before = d.describe();
  /* A click on the gizmo makes no step. */
  const Pixel press = onAxis(d, 1);
  must(d.send(EventType::PointerDown, press.x, press.y).status());
  test_assert(d.tm.modalOpen() && d.ex->interactiveOpen());
  must(d.send(EventType::PointerUp, press.x, press.y).status());
  test_assert(d.history->size() == 0 && !d.ex->interactiveOpen() &&
              d.describe() == before);
  /* Escape, a right press, and an undo during the drag, all cancel exactly. */
  for (int how = 0; how < 3; how++) {
    must(d.send(EventType::PointerDown, press.x, press.y).status());
    const Pixel to = alongAxis(d, 1, press, 80);
    must(d.send(EventType::PointerMove, to.x, to.y).status());
    test_assert(d.describe() != before);
    if (how == 0) {
      InputEvent esc;
      esc.type = EventType::KeyDown;
      esc.key = keys::kEscape;
      must(d.send(esc).status());
    } else if (how == 1) {
      must(d.send(EventType::PointerDown, to.x, to.y, 0, 2).status());
    } else {
      must(d.ex->undo(*d.ctx));
      must(d.send(EventType::PointerMove, to.x + 5, to.y).status());
    }
    test_assert(!d.tm.modalOpen() && !d.ex->interactiveOpen());
    test_assert(d.describe() == before && d.history->size() == 0);
  }
}

void testRotate()
{
  ToolDoc d;
  d.lookFrom(double3(0.5, -1.0, 12.0)); /* from above: the Z ring faces the eye */
  const BlockHandle a = d.box("a");
  const BlockHandle b = d.box("b", 2, 0, 0);
  d.select({a, b});
  d.settingKey("mode", "ROTATE");
  d.useTool(kTransformTool);
  const Frame f = d.gizmoFrame();
  test_assert(f.pivot[0] == 1.0 && f.pivot[1] == 0.0);
  /* A point of the Z ring: its front arc. */
  Vector<double3> pts;
  Vector<int32_t> arcs;
  frontArc(d.vm, f.pivot, f.axes[2], f.unit, pts, arcs);
  test_assert(pts.size() > 16);
  const Pixel start = d.pixelOf(pts[int(pts.size() / 2)]);
  const double r = std::hypot(start.x - f.centre.x, start.y - f.centre.y);
  const double a0 = std::atan2(f.centre.y - start.y, start.x - f.centre.x);
  /* Round the pivot on the screen, counter-clockwise, 720° and then 90° more. */
  must(d.send(EventType::PointerDown, start.x, start.y).status());
  test_assert(d.tm.activePart() == part::kZ);
  const int steps = 90;
  const double total = 2.0 * std::numbers::pi * 2.25;
  for (int i = 1; i <= steps; i++) {
    const double ang = a0 + total * i / steps;
    must(d.send(EventType::PointerMove,
                f.centre.x + r * std::cos(ang),
                f.centre.y - r * std::sin(ang))
             .status());
  }
  test_assert(std::string(d.tm.status().c_str()).find("Rotate 810") == 0);
  must(d.send(EventType::PointerUp,
              f.centre.x + r * std::cos(a0 + total),
              f.centre.y - r * std::sin(a0 + total))
           .status());
  test_assert(d.history->size() == 1);
  /* Seen from above, counter-clockwise is +z: b went from (2, 0) to about (1, 1). */
  const double3 pb = d.position(b);
  test_assert(std::abs(pb[0] - 1.0) < 1e-9 && std::abs(pb[1] - 1.0) < 1e-9);
  must(d.ex->undo(*d.ctx));
  d.evaluate();
  test_assert(d.isAt(b, 2.0) && d.isAt(a, 0.0));

  /* INDIVIDUAL: each turns about itself, so nothing moves. */
  d.settingKey("pivot", "INDIVIDUAL");
  d.evaluate();
  must(d.send(EventType::PointerDown, start.x, start.y).status());
  must(d.send(EventType::PointerMove, f.centre.x - r, f.centre.y).status());
  must(d.send(EventType::PointerUp, f.centre.x - r, f.centre.y).status());
  test_assert(d.history->size() == 1);
  test_assert(d.isAt(b, 2.0) && d.isAt(a, 0.0));
  test_assert(d.describe().size() > 0);
}

void testScale()
{
  ToolDoc d;
  const BlockHandle a = d.box("a", 1, 1, 0);
  d.select({a});
  d.settingKey("mode", "SCALE");
  d.useTool(kTransformTool);
  const Frame f = d.gizmoFrame();
  /* Press on the X axis's handle, and drag to twice its distance from the centre. */
  const Pixel press = onAxis(d, 0, 0.8);
  const double dx = press.x - f.centre.x, dy = press.y - f.centre.y;
  drag(
      d, press, Pixel{f.centre.x + dx * 2.0, f.centre.y + dy * 2.0}, 4, modifiers::kCtrl);
  test_assert(d.history->size() == 1);
  const double *w = d.result(a).state.world;
  test_assert(std::abs(w[0] - 2.0) < 1e-12 && std::abs(w[5] - 1.0) < 1e-12);
  test_assert(d.isAt(a, 1.0, 1.0)); /* about its own place */
}

void testLockedAndChildren()
{
  ToolDoc d;
  const BlockHandle parent = d.box("parent");
  const BlockHandle child = d.box("child", 1, 0, 0);
  d.setParent(child, parent);
  d.select({parent, child});
  d.useTool(kTransformTool);
  const Pixel press = onAxis(d, 0);
  drag(d, press, alongAxis(d, 0, press, 100));
  const double moved = d.position(parent)[0];
  test_assert(moved > 0.1);
  /* The child moves with its parent, once. */
  test_assert(std::abs(d.position(child)[0] - (1.0 + moved)) < 1e-12);
  must(d.ex->undo(*d.ctx));
  d.evaluate();

  /* Locked: the status names them; nothing fails. */
  d.select({child});
  d.ok(kSetLocksCommand,
       {{"channels", PropValue::ofString("location")}, {"on", PropValue::ofBool(true)}});
  d.evaluate();
  const Pixel p2 = onAxis(d, 0);
  must(d.send(EventType::PointerDown, p2.x, p2.y).status());
  const Pixel to = alongAxis(d, 0, p2, 100);
  must(d.send(EventType::PointerMove, to.x, to.y).status());
  test_assert(std::string(d.tm.status().c_str()).find("1 object locked") !=
              std::string::npos);
  must(d.send(EventType::PointerUp, to.x, to.y).status());
  test_assert(d.isAt(child, 1.0));
}

void testPivotAndSize()
{
  ToolDoc d;
  const BlockHandle a = d.box("a", 0, 0, 0);
  const BlockHandle b = d.box("b", 4, 2, 0);
  const BlockHandle hidden = d.box("hidden", 100, 0, 0);
  const BlockId hiddenId = d.idOf(hidden);
  d.ok(kSetFlagsCommand,
       {{"objects",
         PropValue::ofString(
             formatBlockIds(d.rt, std::span<const BlockId>(&hiddenId, 1)))},
        {"flags", PropValue::ofFlags(1)},
        {"on", PropValue::ofBool(true)}});
  d.select({a, b, hidden});
  d.useTool(kTransformTool);
  /* The median of what a drag moves: hidden objects aren't. */
  const Frame f = d.gizmoFrame();
  test_assert(f.pivot[0] == 2.0 && f.pivot[1] == 1.0 && f.pivot[2] == 0.0);
  const CommandDef *def = d.rt.ext<CommandRegistry>().find(kDragDeltaCommand);
  must(d.ex->beginInteractive(*d.ctx, *def, def->newInputs()));
  ApplyDelta *drag = d.ex->interactiveCommand()->as<ApplyDelta>();
  test_assert(drag->count() == 2);
  double3 sum(0, 0, 0);
  for (int i = 0; i < drag->count(); i++) {
    sum = sum + *drag->startPivot(*d.ctx, i);
  }
  test_assert(sum[0] / 2 == f.pivot[0] && sum[1] / 2 == f.pivot[1]);
  must(d.ex->cancel(*d.ctx));

  /* The same size on the screen at three zoom levels. */
  double lengths[3];
  for (int z = 0; z < 3; z++) {
    const double k = 1.0 + z * 2.5;
    d.lookFrom(f.pivot + double3(6.0, -8.0, 5.0) * k, f.pivot);
    const Frame g = d.gizmoFrame();
    const Pixel tip = d.pixelOf(g.pivot + g.axes[0] * g.unit);
    lengths[z] = std::hypot(tip.x - g.centre.x, tip.y - g.centre.y);
  }
  test_assert(std::abs(lengths[1] - lengths[0]) < 1e-6 * lengths[0]);
  test_assert(std::abs(lengths[2] - lengths[0]) < 1e-6 * lengths[0]);
}

void testDraw()
{
  ToolDoc d;
  const BlockHandle a = d.box("a");
  d.select({a});
  d.useTool(kTransformTool);
  DrawList out;
  must(d.tm.draw(*d.ctx, d.view, out));
  DrawListReader rd;
  must(rd.read(out.bytes()));
  /* Three arrows, three planes and the centre; ids of gizmo 1. */
  test_assert(rd.items().size() == 7);
  for (const DrawItem &it : rd.items()) {
    test_assert(it.style.id / ToolManager::kIdStride == 1);
  }
  /* During a drag only the dragged part is drawn, by the modal, where it is now. */
  const Pixel press = onAxis(d, 2);
  must(d.send(EventType::PointerDown, press.x, press.y).status());
  must(d.tm.draw(*d.ctx, d.view, out));
  must(rd.read(out.bytes()));
  test_assert(rd.items().size() == 1 &&
              rd.items()[0].style.id == ToolManager::kIdStride + 2);
  must(d.tm.cancel(*d.ctx));
}

} // namespace

int main()
{
  testTranslate();
  testClickAndCancel();
  testRotate();
  testScale();
  testLockedAndChildren();
  testPivotAndSize();
  testDraw();
  return test_end();
}
