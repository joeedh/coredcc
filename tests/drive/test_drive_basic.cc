/* SceneDrivers on real objects: drivers, time, readers that drive other objects, errors
 * and what depends on them, the commands, undo, and deletes. */
#include "drive_doc.h"

#include "coredcc/drive/commands.h"

test_init;

using namespace drive_test;

namespace {

void testConstant()
{
  DriveDoc d;
  const BlockHandle a = d.object("a", 1);
  const BlockHandle g = d.graph("g");
  d.driveLocation(g, a, 7, 8, 9);
  d.evaluate();
  test_assert(d.evaluatedAt(a, 7, 8, 9));
  test_assert(d.stateAt(a)[0] == 7);
  test_assert(d.drivers->transformCount() == 0); /* nothing reads a state */
  test_assert(d.drivers->diagnostics().size() == 0);

  /* An edit of the graph is seen; the stored channel isn't touched. */
  NodeId v;
  d.def(g).forEachNode([&](const GraphNode &n) {
    if (strcmp(n.typeName().c_str(), "value.vec3") == 0) {
      v = n.id();
    }
  });
  must(d.def(g).setParam(v, "value", PropValue::ofVec3(3, 0, 0)));
  d.evaluate();
  test_assert(d.evaluatedAt(a, 3));
  test_assert(get(d.obj(a).channels, trs::kLocation)[0] == 1);
}

void testTime()
{
  DriveDoc d;
  const BlockHandle a = d.object("a");
  const BlockHandle g = d.graph("g");
  const NodeId t = d.node(g, kTimeNode);
  const NodeId mul = d.node(g, "math.float");
  must(d.def(g).setParam(mul, "op", PropValue::ofEnum(2)));
  must(d.def(g).setLiteral(mul, "b", PropValue::ofFloat(2.0)));
  const NodeId comb = d.node(g, "vec.combine");
  d.link(g, t, "seconds", mul, "a");
  d.link(g, mul, "value", comb, "x");
  const NodeId dr = d.driver(g, "vec3", a, "location");
  d.link(g, comb, "vector", dr, "value");
  for (double frame : {0.0, 12.0, 30.0}) {
    d.sc().frame = frame;
    d.evaluate();
    test_assert(d.evaluatedAt(a, 2 * d.sc().time()));
    test_assert(d.stateAt(a, 1.5)[0] == 3.0);
  }
}

/* a follows b, a child of p. G is {b, p}; its states agree with the direct pass. */
void testFollow()
{
  DriveDoc d;
  const BlockHandle p = d.object("p", 10);
  const BlockHandle b = d.object("b", 1);
  const BlockHandle a = d.object("a");
  d.setParent(b, p);
  const BlockHandle g = d.graph("g");
  d.follow(g, a, b, 1.0);
  d.evaluate();
  test_assert(d.evaluatedAt(a, 12));
  test_assert(d.drivers->transformCount() == 2);
  const NodeId nb = d.drivers->transformNode(d.idOf(b));
  test_assert(!nb.isNull());
  const TransformState *gb = d.drivers->generatedState(d.idOf(b));
  test_assert(gb && sameState(*gb, d.result(b).state));
  const TransformState *gp = d.drivers->generatedState(d.idOf(p));
  test_assert(gp && sameState(*gp, d.result(p).state));

  d.moveTo(p, 20);
  d.evaluate();
  test_assert(d.evaluatedAt(b, 21) && d.evaluatedAt(a, 22));
  const int rebuilds = d.drivers->rebuilds();
  d.evaluate();
  d.moveTo(b, 2);
  d.evaluate();
  test_assert(d.evaluatedAt(a, 23) && d.drivers->rebuilds() == rebuilds);

  /* Reparenting b out of G's hierarchy makes the graph again. */
  d.setParent(b, BlockHandle());
  d.evaluate();
  test_assert(d.evaluatedAt(a, 3) && d.drivers->transformCount() == 1);
  test_assert(d.drivers->rebuilds() > rebuilds);

  /* A chain: c follows a, which follows b. */
  const BlockHandle c = d.object("c");
  d.follow(g, c, a, 1.0);
  d.evaluate();
  test_assert(d.evaluatedAt(a, 3) && d.evaluatedAt(c, 4));
  d.moveTo(b, 5);
  d.evaluate();
  test_assert(d.evaluatedAt(a, 6) && d.evaluatedAt(c, 7));
}

/* What can't work: a diagnostic, and the object fails (ModelFailed), and so does what
 * reads it. Others are evaluated. */
void testErrors()
{
  DriveDoc d;
  const BlockHandle a = d.object("a", 1), b = d.object("b", 2), c = d.object("c", 3);
  const BlockHandle e = d.object("e", 4);
  const BlockHandle g = d.graph("g");
  /* a: a float driving a vec3 channel. */
  const NodeId f = d.node(g, "value.float");
  const NodeId bad = d.driver(g, "float", a, "location");
  d.link(g, f, "value", bad, "value");
  /* b: a channel its type doesn't have. */
  const NodeId v = d.node(g, "value.vec3");
  const NodeId none = d.driver(g, "vec3", b, "nowhere");
  d.link(g, v, "value", none, "value");
  /* c follows a, which fails; e is driven fine. */
  d.follow(g, c, a);
  d.driveLocation(g, e, 9);
  test_assert(!evaluateScene(*d.ctx).ok());
  test_assert(d.statusOf(a) == EvalStatus::ModelFailed);
  test_assert(d.statusOf(b) == EvalStatus::ModelFailed);
  test_assert(d.statusOf(c) == EvalStatus::ModelFailed);
  test_assert(d.evaluatedAt(e, 9));
  test_assert(d.hasDiagnostic("is a vec3, and a graph drives it with a float"));
  test_assert(!computeState(*d.lib, a, 0).ok());

  /* Driven twice. */
  DriveDoc t;
  const BlockHandle x = t.object("x");
  const BlockHandle g1 = t.graph("g1"), g2 = t.graph("g2");
  t.driveLocation(g1, x, 1);
  t.driveLocation(g2, x, 2);
  test_assert(!evaluateScene(*t.ctx).ok());
  test_assert(t.statusOf(x) == EvalStatus::ModelFailed &&
              t.hasDiagnostic("driven twice"));
  must(t.list().remove(*t.lib, t.idOf(g2)).status());
  t.evaluate();
  test_assert(t.evaluatedAt(x, 1));

  /* A reader whose object isn't set, or isn't Transformable. */
  DriveDoc r;
  const BlockHandle y = r.object("y");
  const BlockHandle gr = r.graph("g");
  r.follow(gr, y, r.shapeBlock("s", 1));
  test_assert(!evaluateScene(*r.ctx).ok());
  test_assert(r.statusOf(y) == EvalStatus::ModelFailed &&
              r.hasDiagnostic("Transformable"));
}

/* The commands, with undo and redo; a deleted graph, object and scene. */
void testCommands()
{
  DriveDoc d;
  const BlockHandle a = d.object("a", 1);
  const BlockHandle g = d.graph("g", false);
  d.driveLocation(g, a, 5);
  d.evaluate();
  test_assert(d.evaluatedAt(a, 1));
  d.ok(kAddDriverCommand, {{"graph", d.ptr(g)}});
  d.evaluate();
  test_assert(d.evaluatedAt(a, 5));
  test_assert(d.fails(kAddDriverCommand, {{"graph", d.ptr(g)}}).code() ==
              StatusCode::AlreadyExists);
  test_assert(d.fails(kAddDriverCommand, {{"graph", d.ptr(a)}}).code() ==
              StatusCode::InvalidArgument);
  must(d.ex->undo(*d.ctx));
  d.evaluate();
  test_assert(d.evaluatedAt(a, 1));
  must(d.ex->redo(*d.ctx));
  d.evaluate();
  test_assert(d.evaluatedAt(a, 5));

  d.ok(kRemoveDriverCommand, {{"graph", d.ptr(g)}});
  d.evaluate();
  test_assert(d.evaluatedAt(a, 1));
  test_assert(d.fails(kRemoveDriverCommand, {{"graph", d.ptr(g)}}).code() ==
              StatusCode::NotFound);
  must(d.ex->undo(*d.ctx));
  d.evaluate();
  test_assert(d.evaluatedAt(a, 5));

  /* A deleted graph drives nothing; restored, it does again. */
  must(d.lib->markDeleted(g));
  d.evaluate();
  test_assert(d.evaluatedAt(a, 1));
  must(d.lib->restoreFromHistory(g));
  d.evaluate();
  test_assert(d.evaluatedAt(a, 5));

  /* A deleted list drives nothing; restored, it does again. */
  const BlockHandle list = DriverList::find(*d.lib);
  must(d.lib->markDeleted(list));
  d.evaluate();
  test_assert(d.evaluatedAt(a, 1));
  must(d.lib->restoreFromHistory(list));
  d.evaluate();
  test_assert(d.evaluatedAt(a, 5));

  /* The generated graph keeps nothing alive: a deleted reader's object is collected. */
  const BlockHandle b = d.loose("b", double3(4, 0, 0));
  const BlockHandle c = d.object("c");
  d.follow(g, c, b);
  d.evaluate();
  test_assert(d.evaluatedAt(c, 4));
  must(d.lib->collect().status());
  must(d.lib->validate());
}

} // namespace

int main()
{
  testConstant();
  testTime();
  testFollow();
  testErrors();
  testCommands();
  return test_end();
}
