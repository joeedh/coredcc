/* graph.foreach: failing elements, nesting, limits, and what the def and the datapath know. */
#include "edit_fixture.h"
#include "foreach_fixture.h"

using namespace fixture;

test_init;

namespace {

void testErrors()
{
  World w;
  Loop l(w);
  l.set(table({1, 2, 3}, {{"a", {1, 2, 3}}}));
  test_assert(l.is({1, 2, 3}, {{"sum", {1, 2, 3}}, {"c2", {0, 0, 0}}}, 6));
  const NodePath self = at({}, l.node);
  test_assert(l.inst.report(self).state == NodeReport::State::Ok);

  /* The first that fails stops the run: 3 isn't visited. */
  l.set(table({1, 2, 3}, {{"a", {5, -100, 6}}}));
  const uint64_t runs = l.inst.totalRuns();
  test_assert(l.fails(StatusCode::OutOfRange, {2}, "element 2: a is -100"));
  test_assert(l.inst.totalRuns() - runs == 2 && l.error()->origin == at({l.node}, l.add));
  test_assert(l.inst.report(self).state == NodeReport::State::Failed && l.ran() == 2);
  test_assert(l.inst.report(self).seconds > 0);
  test_assert(l.inst.lastGood(l.path)->payload<Elements>()->columns[0].values[1].asFloat() == 2);
  /* Nothing moved: it isn't run again, until retry, which runs what failed. */
  test_assert(l.fails(StatusCode::OutOfRange, {2}, "element 2: ") && l.ran() == 0);
  w.rt.touch();
  test_assert(l.fails(StatusCode::OutOfRange, {2}, "element 2: ") && l.ran() == 0);
  test_assert(l.inst.retry().ok() && l.fails(StatusCode::OutOfRange, {2}, "") && l.ran() == 1);
  test_assert(l.inst.totalRuns() - runs == 3);
  /* Another row changes: the failing one is still failing. */
  l.set(table({1, 2, 3}, {{"a", {6, -100, 6}}}));
  test_assert(l.fails(StatusCode::OutOfRange, {2}, "") && l.inst.totalRuns() - runs == 4);
  Elements text = table({1, 2, 3}, {{"a", {6, 2, 6}}});
  text.columns[0].values[1] = GraphValue::ofString("x");
  l.set(text);
  test_assert(l.fails(StatusCode::TypeMismatch, {2}, ""));
  text.columns[0].values[0] = GraphValue::ofFloat(5);
  l.set(text);
  test_assert(l.fails(StatusCode::TypeMismatch, {2}, ""));
  /* The others' states stay. */
  l.set(table({1, 2, 3}, {{"a", {5, 2, 6}}}));
  test_assert(l.is({1, 2, 3}, {{"sum", {5, 2, 6}}, {"c2", {0, 0, 0}}}, 2));
  /* An edit while it fails reaches those after the failing one too. */
  l.set(table({1, 2, 3}, {{"a", {5, -100, 6}}}));
  test_assert(!l.evaluate().ok());
  test_assert(l.root.def->setLiteral(l.node, key(l.c), PropValue::ofFloat(3)).ok());
  test_assert(!l.evaluate().ok());
  l.set(table({1, 2, 3}, {{"a", {5, 2, 6}}}));
  test_assert(l.is({1, 2, 3}, {{"sum", {5, 2, 6}}, {"c2", {9, 9, 9}}}, 1 + 2));

  /* Keys that go while a run fails are evicted once one succeeds. */
  l.set(table({1, 2}, {{"a", {5, -100}}}));
  test_assert(!l.evaluate().ok() && l.inst.elementCount() == 3);
  l.set(table({3, 1}, {{"a", {6, 5}}}));
  test_assert(l.is({3, 1}, {{"sum", {6, 5}}, {"c2", {9, 9}}}, 0) && l.inst.elementCount() == 2);
  /* Without limit, though: as many again as may be cached. */
  test_assert(l.root.def->setParam(l.node, kMaxCachedParam, PropValue::ofInt(2)).ok());
  test_assert(l.is({3, 1}, {{"sum", {6, 5}}, {"c2", {9, 9}}}, 0));
  l.set(table({4, 5}, {{"a", {1, -100}}}));
  test_assert(!l.evaluate().ok() && l.inst.elementCount() == 4);
  l.set(table({6, 5}, {{"a", {1, -100}}}));
  test_assert(!l.evaluate().ok() && l.inst.elementCount() == 2);
  l.set(table({7, 8}, {{"a", {1, 2}}}));
  test_assert(l.evaluate().ok() && l.inst.elementCount() == 2);
  l.set(table({9, 10}, {{"a", {1, -100}}}));
  test_assert(!l.evaluate().ok() && l.inst.elementCount() == 4);
  l.set(table({11, 10, 7}, {{"a", {1, -100, 3}}})); /* 7 is below the two that may be cached */
  test_assert(!l.evaluate().ok() && l.inst.elementCount() == 2);

  /* A failing input, and an error deeper down, are the node's. */
  test_assert(l.root.def->setParam(l.node, kMaxCachedParam, PropValue()).ok());
  l.set(table({1, 2}, {{"a", {1, 2}}}));
  const NodeId bad = add(*l.root.def, "t.sum");
  test_assert(l.root.def->setLiteral(bad, "a", PropValue::ofFloat(-100)).ok());
  const LinkId link = connect(*l.root.def, bad, "value", l.node, key(l.b));
  const uint64_t before = l.inst.totalRuns();
  test_assert(l.fails(StatusCode::OutOfRange, {}, "a is -100"));
  test_assert(l.inst.report(self).state == NodeReport::State::InputFailed);
  test_assert(l.inst.totalRuns() - before == 1 && l.error()->origin == at({}, bad));
  test_assert(l.root.def->unlink(link).ok());
  test_assert(l.is({1, 2}, {{"sum", {1, 2}}, {"c2", {9, 9}}}, 4));

  /* A cycle inside (written past the checks), and a missing link's type. */
  LinkState loop;
  loop.id = LinkId{77};
  loop.from = loop.to = l.square;
  loop.fromKey = "value";
  loop.toKey = "b";
  test_assert(l.inner.def->unlink(l.inner.def->node(l.square)->linkInto("b")->id).ok());
  test_assert(l.inner.def->insertLink(loop).ok());
  test_assert(l.fails(StatusCode::FailedPrecondition, {1}, "element 1: a cycle through "));
  test_assert(l.inner.def->unlink(loop.id).ok() && l.evaluate().ok());
  /* A cycle outside, with a foreach above one of its nodes. */
  GraphDef &r = *l.root.def;
  const NodeId total = add(r, "t.total"), x = add(r, "t.sum"), y = add(r, "t.sum");
  connect(r, l.node, kElementsSocket, total, "elements");
  connect(r, total, "value", x, "a");
  connect(r, y, "value", x, "b");
  loop.from = x;
  loop.to = y;
  loop.toKey = "a";
  test_assert(r.insertLink(loop).ok());
  l.set(table({1, 2}, {{"a", {1, 8}}}));
  test_assert(hasCode(l.inst.evaluate(SocketPath::of(y, "value")), StatusCode::FailedPrecondition));
  test_assert(l.inst.report(at({}, x)).state == NodeReport::State::Failed);
  test_assert(l.inst.report(at({}, y)).state == NodeReport::State::Failed);
  test_assert(l.inst.report(at({}, total)).state == NodeReport::State::Ok);
}

/* Elements without a state of their own fail as the others do. */
void testSharedErrors()
{
  World w;
  Loop l(w);
  test_assert(l.root.def->setParam(l.node, kMaxCachedParam, PropValue::ofInt(1)).ok());
  l.set(table({1, 2, 3, 4}, {{"a", {1, 2, -100, 4}}}));
  test_assert(l.fails(StatusCode::OutOfRange, {3}, "element 3: a is -100") && l.ran() == 1);
  test_assert(l.error()->origin == at({l.node}, l.add));
  w.rt.touch();
  test_assert(!l.evaluate().ok() && l.ran() == 0);
  l.set(table({1, 2, 3, 4}, {{"a", {1, -100, -100, 4}}}));
  const uint64_t runs = l.inst.totalRuns();
  test_assert(l.fails(StatusCode::OutOfRange, {2}, "element 2: a is -100"));
  test_assert(l.inst.totalRuns() == runs); /* the same as what failed last */
  test_assert(l.inst.retry().ok() && l.fails(StatusCode::OutOfRange, {2}, ""));
  test_assert(l.inst.totalRuns() == runs + 1);
  l.set(table({1, 2, 3, 4}, {{"a", {1, 2, 3, 4}}}));
  test_assert(l.is({1, 2, 3, 4}, {{"sum", {1, 2, 3, 4}}, {"c2", {0, 0, 0, 0}}}, 3));
}

/* Foreach in foreach: each outer element has a table of `a` rows, 0 to a - 1, and sums
 * i + b over them. */
struct Nested {
  Loop l;
  NodeId range, inner, total;
  explicit Nested(World &w) : l(w)
  {
    const Graph leaf = newGraph(w.lib, "leaf");
    const int64_t i = addSocket(*leaf.def, SocketSide::Input, "i", "float");
    const int64_t b = addSocket(*leaf.def, SocketSide::Input, "b", "float");
    const int64_t o = addSocket(*leaf.def, SocketSide::Output, "o", "float");
    const NodeId in = add(*leaf.def, "graph.input"), out = add(*leaf.def, "graph.output");
    leafSum = add(*leaf.def, "t.sum");
    connect(*leaf.def, in, key(b), leafSum, "a");
    connect(*leaf.def, in, key(i), leafSum, "b");
    connect(*leaf.def, leafSum, "value", out, key(o));
    GraphDef &d = *l.inner.def;
    range = add(d, "t.range");
    inner = add(d, kGraphForeachType);
    total = add(d, "t.total");
    test_assert(d.setGroup(inner, leaf.id).ok());
    test_assert(d.setParam(total, "column", PropValue::ofString("o")).ok());
    connect(d, l.innerIn, key(l.a), range, "count");
    connect(d, range, "elements", inner, kElementsSocket);
    connect(d, l.innerIn, key(l.b), inner, key(b));
    connect(d, inner, kElementsSocket, total, "elements");
    connect(d, total, "value", l.innerOut, key(l.sum));
    leafDef = leaf.def;
    leafB = b;
  }
  GraphDef *leafDef = nullptr;
  NodeId leafSum;
  int64_t leafB = 0;
};

void testNested()
{
  World w;
  Nested n(w);
  Loop &l = n.l;
  l.set(table({10, 20}, {{"a", {3, 2}}, {"b", {1, 5}}}));
  /* 0+1+2 + 3*1 and 0+1 + 2*5. Runs: range and total and square, and 5 leaves. */
  test_assert(l.is({10, 20}, {{"sum", {6, 11}}, {"c2", {0, 0}}}, 6 + 5));
  test_assert(l.inst.elementCount() == 2 + 5);
  w.rt.touch();
  test_assert(l.is({10, 20}, {{"sum", {6, 11}}, {"c2", {0, 0}}}, 0) && l.ran() == 0);
  l.set(table({10, 20}, {{"a", {3, 2}}, {"b", {1, 6}}}));
  test_assert(l.is({10, 20}, {{"sum", {6, 13}}, {"c2", {0, 0}}}, 2 + 1));
  l.set(table({10, 20}, {{"a", {4, 2}}, {"b", {1, 6}}}));
  test_assert(l.is({10, 20}, {{"sum", {10, 13}}, {"c2", {0, 0}}}, 1 + 1 + 1));
  /* An edit two levels down, of a def and of a block. */
  const NodeId read = add(*n.leafDef, "t.read");
  const BlockHandle h = newThing(w.lib, "t", 100);
  test_assert(n.leafDef->setParam(read, "thing", toThing(idOf(w.lib, h))).ok());
  test_assert(n.leafDef->unlink(n.leafDef->node(n.leafSum)->linkInto("a")->id).ok());
  connect(*n.leafDef, read, "value", n.leafSum, "a");
  test_assert(l.is({10, 20}, {{"sum", {406, 201}}, {"c2", {0, 0}}}, 2 * 6 + 2));
  w.thing(h).value = 200;
  w.notify(idOf(w.lib, h));
  test_assert(l.is({10, 20}, {{"sum", {806, 401}}, {"c2", {0, 0}}}, 2 * 6 + 2));
  w.rt.touch();
  test_assert(l.is({10, 20}, {{"sum", {806, 401}}, {"c2", {0, 0}}}, 0) && l.ran() == 0);
  /* Again with one element whose inner foreach wasn't visited in the last run. */
  l.set(table({10, 20}, {{"a", {4, 1}}, {"b", {1, 6}}}));
  test_assert(l.is({10, 20}, {{"sum", {806, 200}}, {"c2", {0, 0}}}, 2));
  w.thing(h).value = 300;
  w.notify(idOf(w.lib, h));
  test_assert(l.is({10, 20}, {{"sum", {1206, 300}}, {"c2", {0, 0}}}, 2 * 5 + 2));
  /* Both keys of a failing element. */
  w.thing(h).value = -100;
  w.notify(idOf(w.lib, h));
  test_assert(l.fails(StatusCode::OutOfRange, {10, 0}, "element 10: element 0: a is -100"));
  test_assert(l.error()->origin == at({l.node, n.inner}, n.leafSum));
  /* The same with shared states, at each level. */
  w.thing(h).value = 7;
  w.notify(idOf(w.lib, h));
  for (int mode = 0; mode < 4; mode++) {
    const PropValue none = PropValue::ofInt(0), some = PropValue::ofInt(16);
    test_assert(l.root.def->setParam(l.node, kMaxCachedParam, mode & 1 ? none : some).ok());
    test_assert(l.inner.def->setParam(n.inner, kMaxCachedParam, mode & 2 ? none : some).ok());
    l.set(table({10, 20, 30}, {{"a", {4, 1, 0}}, {"b", {1, 6, 2}}}));
    test_assert(l.evaluate().ok());
    const Elements::Column &c = *l.result()->column("sum");
    test_assert(c.values[0].asFloat() == 34 && c.values[1].asFloat() == 7 && c.values[2].asFloat() == 0);
  }
}

void testGroups()
{
  World w;
  Loop l(w);
  GraphDef &r = *l.root.def;
  l.set(table({1}, {{"a", {4}}}));
  /* Recursion through a foreach is refused, and found when it is written. */
  const NodeId back = add(*l.inner.def, kGraphForeachType);
  test_assert(hasCode(l.inner.def->setGroup(back, l.root.id), StatusCode::InvalidArgument));
  test_assert(hasCode(l.inner.def->setGroup(back, l.inner.id), StatusCode::InvalidArgument));
  test_assert(l.inner.def->sockets(*l.inner.def->node(back), SocketSide::Input).size() == 1);
  test_assert(l.evaluate().ok() && l.inner.def->removeNode(back).ok());
  NodeState state;
  state.id = NodeId{50};
  state.typeName = kGraphForeachType;
  state.group = l.root.id;
  test_assert(hasCode(l.inner.def->insertNode(state), StatusCode::InvalidArgument));
  test_assert(l.inner.def->restoreNode(state).ok());
  const int64_t t = addSocket(*l.inner.def, SocketSide::Output, "t", "elements");
  LinkState link;
  link.id = LinkId{51};
  link.from = state.id;
  link.fromKey = kElementsSocket;
  link.to = l.innerOut;
  link.toKey = interfaceKey(t);
  test_assert(l.inner.def->insertLink(link).ok());
  test_assert(l.fails(StatusCode::FailedPrecondition, {1}, "element 1: the graph contains itself"));
  test_assert(l.error()->origin == at({l.node}, state.id));
  test_assert(l.inner.def->removeNode(state.id).ok() && l.inner.def->removeInterface(t).ok());
  test_assert(l.is({1}, {{"sum", {4}}, {"c2", {0}}}, 0));

  /* Links: its output depends on all its inputs. */
  const NodeId total = add(r, "t.total");
  connect(r, l.node, kElementsSocket, total, "elements");
  test_assert(hasCode(r.link({total, "value"}, {l.node, key(l.c)}), StatusCode::InvalidArgument));
  test_assert(hasCode(r.link({l.node, kElementsSocket}, {l.node, kElementsSocket}),
                      StatusCode::InvalidArgument));
  test_assert(hasCode(r.setLiteral(l.node, kElementsSocket, PropValue::ofFloat(1)),
                      StatusCode::Unsupported));

  /* A reference keeps the def alive; without one, there is no graph. */
  w.lib.setFlag(l.root.h, BlockFlags::Root, true);
  test_assert(w.lib.usersOf(l.inner.h).size() == 1);
  test_assert(w.lib.collect().ok() && w.lib.get(l.inner.h));
  test_assert(r.setGroup(l.node, BlockId()).ok());
  test_assert(l.fails(StatusCode::NotFound, {}, "the group has no graph"));
  test_assert(l.error()->origin == at({}, l.node) && l.inst.elementCount() == 1);
  test_assert(r.setGroup(l.node, l.inner.id).ok());
  test_assert(l.is({1}, {{"sum", {4}}, {"c2", {0}}}, 0));
  test_assert(w.lib.markDeleted(l.inner.h).ok());
  test_assert(l.fails(StatusCode::NotFound, {}, "the group's graph isn't there"));
  test_assert(r.sockets(*r.node(l.node), SocketSide::Input).size() == 4); /* as last seen */
  test_assert(w.lib.restoreFromHistory(l.inner.h).ok());
  test_assert(l.is({1}, {{"sum", {4}}, {"c2", {0}}}, 0));
  /* Another def: new states. */
  const Graph other = newGraph(w.lib, "other");
  const int64_t oo = addSocket(*other.def, SocketSide::Output, "o", "float");
  test_assert(r.setGroup(l.node, other.id).ok());
  test_assert(l.is({1}, {{"o", {0}}}, 0) && l.inst.elementCount() == 1);
  test_assert(other.def->removeInterface(oo).ok() && l.is({1}, {}, 0));
  test_assert(r.setGroup(l.node, l.inner.id).ok());
  test_assert(l.is({1}, {{"sum", {4}}, {"c2", {0}}}, 2));
  /* A new def under its id. */
  const uint64_t serial = l.inner.def->serial();
  test_assert(w.lib.markDeleted(l.inner.h).ok() && w.lib.destroyFromHistory(l.inner.h).ok());
  GraphDef *def = litestl::alloc::New<GraphDef>("coredcc::GraphDef", &w.rt, &w.lib, l.inner.id);
  test_assert(def->addInterface(SocketSide::Output, "new", "int", -1, l.sum).ok());
  test_assert(w.lib.reviveAs(TypeId::of(kGraphDefType), l.inner.id, "inner", def).ok());
  test_assert(def->serial() != serial && l.evaluate().ok() && l.result()->columns.size() == 1);
  test_assert(*l.column("new") == GraphValue::ofInt(0));
  /* The node is removed, or made another type's: so are the elements. */
  test_assert(l.inst.elementCount() == 1);
  test_assert(r.removeNode(l.node).ok() && l.evaluate().ok() && l.inst.elementCount() == 0);
}

/* A node type, a socket type and a conversion that the elements wait for. */
Status give(NodeExecContext &ctx)
{
  return ctx.setOutput(0, GraphValue::ofFloat(42));
}

void testLateTypes()
{
  World w;
  Loop l(w);
  l.set(table({1}, {{"a", {1}}, {"b", {1}}}));
  NodeState s;
  s.id = NodeId{100};
  s.typeName = "late.give";
  s.outputs.append(SocketInfo{"value", "value", "float"});
  test_assert(l.inner.def->insertNode(s).ok());
  LinkState link;
  link.id = LinkId{101};
  link.from = s.id;
  link.fromKey = "value";
  link.to = l.add;
  link.toKey = "b";
  test_assert(l.inner.def->unlink(l.inner.def->node(l.add)->linkInto("b")->id).ok());
  test_assert(l.inner.def->insertLink(link).ok());
  test_assert(l.fails(StatusCode::NotFound, {1}, "element 1: "));
  test_assert(l.error()->origin == at({l.node}, s.id) && l.ran() == 1);
  w.rt.touch();
  test_assert(!l.evaluate().ok() && l.ran() == 0);
  NodeTypeDef late("late.give");
  late.output("value", "float");
  addType<give>(w.rt, late, "late.give");
  test_assert(l.is({1}, {{"sum", {43}}, {"c2", {0}}}, 3));

  Elements t = table({1}, {{"a", {1}}});
  t.columns[0].values[0] = GraphValue::ofString("12");
  l.set(t);
  test_assert(l.fails(StatusCode::TypeMismatch, {1}, "element 1: column 'a': "));
  test_assert(w.rt.ext<SocketTypeRegistry>()
                  .addConversion("string", "float",
                                 [](const GraphValue &in, GraphValue &out) {
                                   out = GraphValue::ofFloat(atof(in.asString().c_str()));
                                   return Status();
                                 })
                  .ok());
  test_assert(l.is({1}, {{"sum", {54}}, {"c2", {0}}}, 1));
}

void testLimits()
{
  /* Depth: an element is a level, and so is a group in it. */
  {
    World w;
    Loop l(w);
    l.set(table({1}, {{"a", {1}}}));
    /* Under 63 groups the elements are at depth 64. */
    Graph g = l.root;
    int64_t socket = g.def->interfaceSockets(SocketSide::Output)[0].id;
    for (int depth = 1; depth <= 64; depth++) {
      const Graph up = newGraph(w.lib, "up");
      const int64_t o = addSocket(*up.def, SocketSide::Output, "out", "elements");
      const NodeId out = add(*up.def, "graph.output");
      const NodeId group = add(*up.def, "graph.group");
      test_assert(up.def->setGroup(group, g.id).ok());
      connect(*up.def, group, key(socket), out, key(o));
      const NodeId t = add(*up.def, "t.range");
      test_assert(up.def->setLiteral(t, "count", PropValue::ofFloat(1)).ok());
      if (depth == 1) {
        connect(*up.def, t, "elements", group, up.def->sockets(*up.def->node(group), SocketSide::Input)[0].key.c_str());
      }
      GraphInstance inst(&w.rt, &w.lib, up.id);
      const Status s = inst.evaluate(SocketPath::output("out"));
      test_assert(depth < 64 ? s.ok() : hasCode(s, StatusCode::OutOfRange));
      if (depth < 64) {
        test_assert(inst.frameCount() == depth && inst.elementCount() == 1);
      }
      g = up;
      socket = o;
    }
  }
  /* The budget is each element's own; the whole call fails. */
  {
    World w;
    Loop l(w);
    const Graph leaf = newGraph(w.lib, "leaf");
    const int64_t o = addSocket(*leaf.def, SocketSide::Output, "o", "float");
    const NodeId g1 = add(*l.inner.def, "graph.group"), g2 = add(*l.inner.def, "graph.group");
    test_assert(l.inner.def->setGroup(g1, leaf.id).ok() && l.inner.def->setGroup(g2, leaf.id).ok());
    connect(*l.inner.def, g1, key(o), l.add, "a");
    connect(*l.inner.def, g2, key(o), l.add, "b");
    l.inst.setMaxFrames(2);
    l.set(table({1, 2, 3}, {}));
    test_assert(l.is({1, 2, 3}, {{"sum", {0, 0, 0}}, {"c2", {0, 0, 0}}}, 6));
    test_assert(l.inst.elementCount() == 2 && l.inst.frameCount() == 0); /* 3 shares */
    l.set(table({4, 5}, {{"c", {1, 2}}})); /* both share; a run that fails makes room */
    test_assert(l.evaluate().ok() && l.inst.elementCount() == 0);
    l.set(table({1, 2}, {}));
    test_assert(l.evaluate().ok() && l.inst.elementCount() == 2);
    Elements text = table({4, 5}, {{"c", {1, 2}}});
    text.columns[0].values[1] = GraphValue::ofString("x");
    l.set(text);
    test_assert(!l.evaluate().ok() && l.inst.elementCount() == 0);
    l.set(table({1, 2, 3}, {}));
    test_assert(l.evaluate().ok() && l.inst.elementCount() == 2);
    l.inst.setMaxFrames(1);
    l.set(table({1, 2, 3, 4}, {}));
    test_assert(l.evaluate().ok()); /* they have their groups */
    test_assert(l.inst.clear().ok() && l.inst.elementCount() == 0);
    const SocketPath p = l.path;
    litestl::util::Vector<Status> results;
    test_assert(hasCode(l.inst.evaluate({&p, 1}, results), StatusCode::OutOfRange));
    test_assert(results.size() == 0 && !w.rt.evaluating());
    test_assert(hasCode(l.inst.evaluate({&p, 1}, results), StatusCode::OutOfRange));
    l.inst.setMaxFrames(100);
    test_assert(l.is({1, 2, 3, 4}, {{"sum", {0, 0, 0, 0}}, {"c2", {0, 0, 0, 0}}}, 8));
  }
  /* Nested, where the inner call fails. */
  {
    World w;
    Nested n(w);
    const Graph leaf = newGraph(w.lib, "deep");
    const int64_t o = addSocket(*leaf.def, SocketSide::Output, "o", "float");
    const NodeId g = add(*n.leafDef, "graph.group");
    test_assert(n.leafDef->setGroup(g, leaf.id).ok());
    connect(*n.leafDef, g, key(o), n.leafSum, "b");
    n.l.inst.setMaxFrames(0);
    n.l.set(table({1, 2}, {{"a", {2, 3}}, {"b", {1, 1}}}));
    test_assert(hasCode(n.l.evaluate(), StatusCode::OutOfRange));
    n.l.inst.setMaxFrames(100);
    test_assert(n.l.is({1, 2}, {{"sum", {2, 3}}, {"c2", {0, 0}}}, 6 + 5));
  }
}

/* Paths don't lead into elements; reports and requests are about the node. */
void testPaths()
{
  World w;
  Loop l(w);
  l.set(table({1}, {{"a", {1}}}));
  test_assert(l.evaluate().ok());
  const SocketPath inside = SocketPath::of(at({l.node}, l.add), "value");
  test_assert(hasCode(l.inst.evaluate(inside), StatusCode::NotFound));
  test_assert(l.inst.report(at({l.node}, l.add)).state == NodeReport::State::NotEvaluated);
  const SocketPath out = SocketPath::of(l.node, kElementsSocket);
  test_assert(l.inst.evaluate(out).ok() && l.inst.value(out)->payload<Elements>()->size() == 1);
  const SocketPath in = SocketPath::of(l.node, kElementsSocket, SocketSide::Input);
  test_assert(l.inst.evaluate(in).ok() && l.inst.value(in)->payload<Elements>()->size() == 1);
  test_assert(l.inst.report(at({}, l.node)).state == NodeReport::State::Ok);
}

/* The node in datapaths and commands. */
void testDocument()
{
  Doc d;
  addForeachTypes(d.rt);
  const Graph g = newGraph(d.lib, "g"), inner = newGraph(d.lib, "inner");
  const int64_t a = addSocket(*inner.def, SocketSide::Input, "a", "float");
  StableRef ref;
  ref.owner = OwnerKey::ofBlock(g.id);
  ref.type = StructId::of(kGraphDefType);
  test_assert(d.run(kGraphNodeAddCommand, {{"graph", PropValue::ofPointer(ref)},
                                           {"type", PropValue::ofString(kGraphForeachType)}})
                  .ok());
  const NodeId n = g.def->nodeAt(0)->id();
  const std::string path = "graphs[\"g\"].nodes[#" + std::to_string(n.value) + "]";
  test_assert(d.is(path + ".params.maxCached", PropValue::ofInt(16384)));
  test_assert(d.set(path + ".params.maxCached", PropValue::ofInt(-5)).ok());
  test_assert(d.is(path + ".params.maxCached", PropValue::ofInt(0)));
  ref.owner = OwnerKey::ofBlock(inner.id);
  test_assert(d.set(path + ".group", PropValue::ofPointer(ref)).ok());
  test_assert(g.def->groupTarget(*g.def->node(n)) == inner.def);
  const std::string socket = path + ".sockets[#" + std::to_string(a) + "]";
  test_assert(d.set(socket + ".isSet", PropValue::ofBool(true)).ok());
  test_assert(d.set(socket + ".value", PropValue::ofFloat(3)).ok());
  test_assert(d.is(socket + ".value", PropValue::ofFloat(3)));
  test_assert(d.lib.markDeleted(inner.h).ok());
  test_assert(!d.set(socket + ".value", PropValue::ofFloat(4)).ok()); /* it isn't there */
  test_assert(d.lib.restoreFromHistory(inner.h).ok());
  while (d.history.canUndo()) {
    test_assert(d.undo().ok());
  }
  test_assert(g.def->nodeCount() == 0);
  while (d.history.canRedo()) {
    test_assert(d.redo().ok());
  }
  test_assert(d.is(socket + ".value", PropValue::ofFloat(3)));
  test_assert(d.is(path + ".params.maxCached", PropValue::ofInt(0)));
}

} // namespace

int main()
{
  testErrors();
  testSharedErrors();
  testNested();
  testGroups();
  testLateTypes();
  testLimits();
  testPaths();
  testDocument();
  return test_end();
}
