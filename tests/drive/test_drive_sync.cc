/* The generated graph follows the document: nested groups, retargeted adapters,
 * reparenting, transform types, stale parents; and cycles, which are left out and
 * reported. */
#include "drive_doc.h"

test_init;

using namespace drive_test;

namespace {

void testNested()
{
  DriveDoc d;
  const BlockHandle a = d.object("a"), b = d.object("b", 2);
  const BlockHandle inner = d.graph("inner", false);
  d.follow(inner, a, b, 1);
  const BlockHandle outer = d.graph("outer");
  const NodeId n = d.node(outer, kGraphGroupType);
  must(d.def(outer).setGroup(n, d.idOf(inner)));
  d.evaluate();
  test_assert(d.evaluatedAt(a, 3));
  /* Used twice: a drives its channel twice. */
  const NodeId n2 = d.node(outer, kGraphGroupType);
  must(d.def(outer).setGroup(n2, d.idOf(inner)));
  test_assert(!evaluateScene(*d.ctx).ok());
  test_assert(d.hasDiagnostic("driven twice"));
  must(d.def(outer).removeNode(n2));
  d.evaluate();
  test_assert(d.evaluatedAt(a, 3));
  /* Contains itself: refused by the graph, or cut by the scan. */
  const NodeId self = d.node(inner, kGraphGroupType);
  (void)d.def(inner).setGroup(self, d.idOf(outer));
  (void)evaluateScene(*d.ctx);
  must(d.def(inner).removeNode(self));
  d.evaluate();
  test_assert(d.evaluatedAt(a, 3));
}

void testRetarget()
{
  DriveDoc d;
  const BlockHandle a = d.object("a"), b = d.object("b", 2), c = d.object("c", 5);
  const BlockHandle g = d.graph("g");
  d.follow(g, a, b);
  d.evaluate();
  test_assert(d.evaluatedAt(a, 2));
  NodeId r;
  d.def(g).forEachNode([&](const GraphNode &n) {
    if (strcmp(n.typeName().c_str(), kStateNode) == 0) {
      r = n.id();
    }
  });
  must(d.def(g).setParam(r, "object", pointerTo(d.idOf(c))));
  d.evaluate();
  test_assert(d.evaluatedAt(a, 5) && d.drivers->transformCount() == 1);
  /* By a cell write: only the epoch moves. */
  const GraphNode *node = d.def(g).node(r);
  for (const NodeBlockRef &ref : node->blockRefs()) {
    if (strcmp(ref.name.c_str(), "object") == 0) {
      must(d.lib->writeRef(const_cast<RefCell &>(ref.cell), d.idOf(b)));
    }
  }
  d.evaluate();
  test_assert(d.evaluatedAt(a, 2));
}

void testHierarchy()
{
  DriveDoc d;
  const BlockHandle p = d.object("p", 10), q = d.object("q", 100);
  const BlockHandle b = d.object("b", 1), a = d.object("a");
  d.setParent(b, p);
  const BlockHandle g = d.graph("g");
  d.follow(g, a, b);
  d.evaluate();
  test_assert(d.evaluatedAt(a, 11));
  /* A reparent within the document, by a command (it notifies). */
  d.ok(kSetParentCommand,
       {{"parent", d.ptr(q)},
        {"objects", PropValue::ofString(formatBlockId(d.rt, d.idOf(b)))},
        {"keepWorld", PropValue::ofBool(false)}});
  d.evaluate();
  test_assert(d.evaluatedAt(a, 101));
  must(d.ex->undo(*d.ctx));
  d.evaluate();
  test_assert(d.evaluatedAt(a, 11));

  /* Its transform type changed: another node type. */
  d.ok(kSetTransformTypeCommand,
       {{"type", PropValue::ofString(kFbxTransform)},
        {"objects", PropValue::ofString(formatBlockId(d.rt, d.idOf(b)))}});
  d.evaluate();
  test_assert(d.evaluatedAt(a, 11));
  const TransformState *gb = d.drivers->generatedState(d.idOf(b));
  test_assert(gb && sameState(*gb, d.result(b).state));

  /* A host whose parent changes without a word: the node is stale, and made again. */
  const BlockHandle h = d.hostObject("h", 1);
  const BlockHandle e = d.object("e");
  d.follow(g, e, h);
  d.evaluate();
  test_assert(d.evaluatedAt(e, 1));
  d.hostRaw(h).info.parent = d.idOf(q); /* without a notification */
  const int rebuilds = d.drivers->rebuilds();
  must(d.drivers->update(d.sc().time() + 1)); /* another time, so it evaluates */
  test_assert(d.drivers->rebuilds() > rebuilds);
  test_assert(d.stateAt(e, d.sc().time() + 1)[0] == 101);
}

void testCycles()
{
  /* A reader of a child that drives its parent: left out, reported, the parent fails. */
  DriveDoc d;
  const BlockHandle p = d.object("p", 1), c = d.object("c", 1);
  d.setParent(c, p);
  const BlockHandle g = d.graph("g");
  d.follow(g, p, c);
  test_assert(!evaluateScene(*d.ctx).ok());
  test_assert(d.hasDiagnostic("cycle"));
  test_assert(d.statusOf(p) == EvalStatus::ModelFailed);
  test_assert(d.statusOf(c) ==
              EvalStatus::Ok); /* under its parent's fallback, as in P3 */

  /* A cycle of parents: Cycle, as the direct pass says. */
  DriveDoc h;
  const BlockHandle x = h.object("x", 1), y = h.object("y", 2), z = h.object("z");
  h.setParent(x, y);
  h.setParent(y, x);
  const BlockHandle gh = h.graph("g");
  h.follow(gh, z, x);
  h.evaluate();
  test_assert(h.statusOf(x) == EvalStatus::Cycle && h.statusOf(y) == EvalStatus::Cycle);
  const TransformState *gx = h.drivers->generatedState(h.idOf(x));
  test_assert(gx && sameState(*gx, h.result(x).state));
  test_assert(h.evaluatedAt(z, 1));
  h.setParent(y, BlockHandle());
  h.evaluate();
  test_assert(h.evaluatedAt(x, 3) && h.evaluatedAt(z, 3));
}

} // namespace

int main()
{
  testNested();
  testRetarget();
  testHierarchy();
  testCycles();
  return test_end();
}
