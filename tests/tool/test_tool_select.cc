/* core.tool.select: a click, Shift-click, a box with and without modifiers, a click on
 * nothing; each selection is one undo step. And the transform tool selects when a press
 * misses its gizmo. */
#include "tool_fixture.h"

test_init;

using namespace tool_test;

namespace {

bool selectedAre(ToolDoc &d, std::initializer_list<BlockHandle> want)
{
  const Vector<BlockHandle> &got = d.sc().selected(*d.lib);
  if (int(got.size()) != int(want.size())) {
    return false;
  }
  for (BlockHandle h : want) {
    bool found = false;
    for (BlockHandle g : got) {
      found = found || g == h;
    }
    if (!found) {
      return false;
    }
  }
  return true;
}

void click(ToolDoc &d, Pixel p, uint32_t mods = 0)
{
  must(d.send(EventType::PointerDown, p.x, p.y, mods).status());
  must(d.send(EventType::PointerUp, p.x + 1, p.y, mods)
           .status()); /* within the threshold */
}

void box(ToolDoc &d, Pixel a, Pixel b, uint32_t mods = 0)
{
  must(d.send(EventType::PointerDown, a.x, a.y, mods).status());
  must(d.send(EventType::PointerMove, (a.x + b.x) / 2, (a.y + b.y) / 2, mods).status());
  must(d.send(EventType::PointerMove, b.x, b.y, mods).status());
  must(d.send(EventType::PointerUp, b.x, b.y, mods).status());
}

void testClicks()
{
  ToolDoc d;
  const BlockHandle a = d.box("a", -2, 0, 0);
  const BlockHandle b = d.box("b", 2, 0, 0);
  d.useTool(kSelectTool);
  const Pixel pa = d.pixelOf(double3(-2, 0, 0)), pb = d.pixelOf(double3(2, 0, 0));
  click(d, pa);
  test_assert(selectedAre(d, {a}) && d.history->size() == 1);
  click(d, pb);
  test_assert(selectedAre(d, {b}) && d.history->size() == 2);
  click(d, pa, modifiers::kShift);
  test_assert(selectedAre(d, {a, b}) && d.history->size() == 3);
  click(d, pb, modifiers::kShift); /* toggles off */
  test_assert(selectedAre(d, {a}) && d.history->size() == 4);
  /* A click on nothing selects nothing; with Shift it does nothing. */
  click(d, Pixel{5, 5}, modifiers::kShift);
  test_assert(selectedAre(d, {a}) && d.history->size() == 4);
  click(d, Pixel{5, 5});
  test_assert(selectedAre(d, {}) && d.history->size() == 5);
  /* Each is one undo step. */
  must(d.ex->undo(*d.ctx));
  test_assert(selectedAre(d, {a}));
  must(d.ex->undo(*d.ctx));
  test_assert(selectedAre(d, {a, b}));
}

void testBoxes()
{
  ToolDoc d;
  const BlockHandle a = d.box("a", -2, 0, 0, 0.3);
  const BlockHandle b = d.box("b", 2, 0, 0, 0.3);
  const BlockHandle c = d.box("c", 0, 0, 3, 0.3);
  d.useTool(kSelectTool);
  const Pixel pa = d.pixelOf(double3(-2, 0, 0)), pb = d.pixelOf(double3(2, 0, 0));
  const Pixel pc = d.pixelOf(double3(0, 0, 3));
  /* A box around a and b, not c. */
  const double top = std::min(pa.y, pb.y) - 30, bottom = std::max(pa.y, pb.y) + 30;
  test_assert(pc.y < top);
  box(d, Pixel{std::min(pa.x, pb.x) - 30, top}, Pixel{std::max(pa.x, pb.x) + 30, bottom});
  test_assert(selectedAre(d, {a, b}) && d.history->size() == 1);
  /* The box overlay is drawn during it. */
  must(d.send(EventType::PointerDown, pc.x - 20, pc.y - 20).status());
  must(d.send(EventType::PointerMove, pc.x + 20, pc.y + 20).status());
  DrawList out;
  must(d.tm.draw(*d.ctx, d.view, out));
  DrawListReader rd;
  must(rd.read(out.bytes()));
  test_assert(rd.records().size() == 1 && rd.records()[0].kind == DrawRecordKind::Line2D);
  /* Shift adds. */
  must(d.send(EventType::PointerUp, pc.x + 20, pc.y + 20, modifiers::kShift).status());
  test_assert(selectedAre(d, {a, b, c}) && d.history->size() == 2);
  /* Ctrl removes. */
  box(d, Pixel{pa.x - 20, pa.y - 20}, Pixel{pa.x + 20, pa.y + 20}, modifiers::kCtrl);
  test_assert(selectedAre(d, {b, c}) && d.history->size() == 3);
  /* An empty box selects nothing. */
  box(d, Pixel{5, 5}, Pixel{40, 40});
  test_assert(selectedAre(d, {}) && d.history->size() == 4);
  /* Escape during a box cancels it. */
  must(d.send(EventType::PointerDown, 5, 5).status());
  must(d.send(EventType::PointerMove, 700, 500).status());
  InputEvent esc;
  esc.type = EventType::KeyDown;
  esc.key = keys::kEscape;
  must(d.send(esc).status());
  test_assert(!d.tm.modalOpen() && selectedAre(d, {}) && d.history->size() == 4);
}

void testTransformToolSelects()
{
  ToolDoc d;
  const BlockHandle a = d.box("a", -3, 0, 0);
  const BlockHandle b = d.box("b", 3, 0, 0);
  d.select({a});
  d.useTool(kTransformTool);
  click(d, d.pixelOf(double3(3, 0, 0)));
  test_assert(selectedAre(d, {b}) && d.history->size() == 1);
  /* The gizmo follows the selection. */
  test_assert(d.gizmoFrame().pivot[0] == 3.0);
}

} // namespace

int main()
{
  testClicks();
  testBoxes();
  testTransformToolSelects();
  return test_end();
}
