/* GraphInstance: what it says without evaluating, and dropping what it keeps. */
#include "foreach_fixture.h"

#include "coredcc/graph/interior.h"
#include "coredcc/graph/system.h"

using namespace fixture;

test_init;

namespace {

using State = NodeReport::State;

bool isFloat(const GraphValue &v, double x)
{
  const bool ok = v == GraphValue::ofFloat(x);
  if (!ok) {
    printf("%s %g\n",
           graphValueTypeName(v.type()),
           v.type() == GraphValue::Type::Float ? v.asFloat() : 0.0);
  }
  return ok;
}

SocketPath
socket(NodePath node, std::string_view key, SocketSide side = SocketSide::Output)
{
  return SocketPath::of(std::move(node), key, side);
}

/* a -> group(x -> y = x + 1 -> z = y + 2) -> b, and a standalone c. */
struct Nest {
  World w;
  Graph root, inner;
  int64_t x = 0, z = 0, rootIn = 0, rootOut = 0;
  NodeId a, group, b, c, in, out, rootInput, rootOutput;
  NodeId y, zNode;
  GraphInstance inst;
  Nest()
      : root(newGraph(w.lib, "root")), inner(newGraph(w.lib, "inner")),
        inst(&w.rt, &w.lib, root.id)
  {
    GraphDef &d = *inner.def;
    x = addSocket(d, SocketSide::Input, "x", "float");
    z = addSocket(d, SocketSide::Output, "z", "float");
    in = add(d, "graph.input");
    out = add(d, "graph.output");
    y = add(d, "t.sum");
    zNode = add(d, "t.sum");
    test_assert(d.setLiteral(y, "b", PropValue::ofFloat(1)).ok());
    test_assert(d.setLiteral(zNode, "b", PropValue::ofFloat(2)).ok());
    connect(d, in, key(x), y, "a");
    connect(d, y, "value", zNode, "a");
    connect(d, zNode, "value", out, key(z));
    GraphDef &r = *root.def;
    rootIn = addSocket(r, SocketSide::Input, "in", "float");
    rootOut = addSocket(r, SocketSide::Output, "out", "float");
    rootInput = add(r, "graph.input");
    rootOutput = add(r, "graph.output");
    a = add(r, "t.sum");
    b = add(r, "t.sum");
    c = add(r, "t.sum");
    group = add(r, kGraphGroupType);
    test_assert(r.setGroup(group, inner.id).ok());
    connect(r, rootInput, key(rootIn), a, "a");
    connect(r, a, "value", group, key(x));
    connect(r, group, key(z), b, "a");
    connect(r, b, "value", rootOutput, key(rootOut));
  }
  Status evaluate()
  {
    return inst.evaluate(SocketPath::output("out"));
  }
};

void testPeek()
{
  Nest n;
  GraphInstance &i = n.inst;
  const SocketPath a = SocketPath::of(n.a, "value");
  test_assert(i.peek(a).isNone() && i.peek(SocketPath::output("out")).isNone());
  test_assert(i.setInput("in", GraphValue::ofFloat(10)).ok() && n.evaluate().ok());
  const uint64_t runs = i.totalRuns(), visits = i.totalVisits();
  /* All that was on the way, at every kind of socket. */
  test_assert(isFloat(i.peek(a), 10) && !i.value(a));
  test_assert(isFloat(i.peek(socket(at({}, n.a), "a", SocketSide::Input)), 10));
  test_assert(isFloat(i.peek(socket(at({}, n.group), key(n.x), SocketSide::Input)), 10));
  test_assert(isFloat(i.peek(socket(at({}, n.group), key(n.z))), 13));
  test_assert(isFloat(i.peek(socket(at({}, n.b), "a", SocketSide::Input)), 13));
  test_assert(isFloat(i.peek(socket(at({n.group}, n.in), key(n.x))), 10));
  test_assert(isFloat(i.peek(socket(at({n.group}, n.y), "value")), 11));
  test_assert(isFloat(i.peek(socket(at({n.group}, n.y), "b", SocketSide::Input)), 1));
  test_assert(isFloat(i.peek(socket(at({n.group}, NodeId()), key(n.z))), 13));
  const std::string interior = "@" + std::to_string(n.y.value) + ".value";
  test_assert(i.peek(socket(at({}, n.group), interior)).isNone()); /* no link uses it */
  test_assert(isFloat(i.peek(socket(at({}, n.rootInput), key(n.rootIn))), 10));
  test_assert(isFloat(i.peek(SocketPath::output("out")), 13));
  test_assert(isFloat(i.peek(SocketPath::output(key(n.rootOut))), 13));
  test_assert(i.totalRuns() == runs && i.totalVisits() == visits);
  /* What isn't there, or wasn't computed. */
  test_assert(i.peek(SocketPath::of(n.c, "value")).isNone());
  test_assert(i.peek(SocketPath::of(n.c, "a", SocketSide::Input)).isNone());
  test_assert(i.peek(SocketPath::of(n.a, "no")).isNone());
  test_assert(i.peek(SocketPath::of(NodeId{77}, "value")).isNone());
  test_assert(i.peek(socket(at({n.a}, n.y), "value")).isNone());
  test_assert(i.peek(socket(at({NodeId{77}}, n.y), "value")).isNone());
  test_assert(i.peek(SocketPath::output("no")).isNone());

  /* A default, and the interface's. */
  test_assert(i.setInput("in", GraphValue()).ok());
  test_assert(isFloat(i.peek(socket(at({}, n.rootInput), key(n.rootIn))), 0));
  test_assert(n.root.def->setInterfaceDefault(n.rootIn, PropValue::ofFloat(4)).ok());
  test_assert(isFloat(i.peek(socket(at({}, n.rootInput), key(n.rootIn))), 4));
  test_assert(isFloat(i.peek(a), 10)); /* not computed since */
  test_assert(n.evaluate().ok() && isFloat(i.peek(a), 4));

  /* After an edit: outputs as they were, and no inputs that it concerns. */
  test_assert(n.inner.def->setLiteral(n.y, "b", PropValue::ofFloat(3)).ok());
  test_assert(i.evaluate(a).ok()); /* it looks at no more than `a` */
  test_assert(isFloat(i.peek(socket(at({n.group}, n.y), "value")), 5));
  test_assert(i.peek(socket(at({n.group}, n.y), "b", SocketSide::Input)).isNone());
  test_assert(i.peek(socket(at({}, n.group), key(n.z))).isNone());
  test_assert(n.evaluate().ok() && isFloat(i.peek(socket(at({}, n.group), key(n.z))), 9));

  /* An error: nothing below it has a value. */
  test_assert(i.setInput("in", GraphValue::ofFloat(-100)).ok() && !n.evaluate().ok());
  test_assert(i.peek(a).isNone() && i.peek(SocketPath::output("out")).isNone());
  test_assert(i.peek(socket(at({n.group}, n.y), "value")).isNone());
  test_assert(i.peek(socket(at({n.group}, n.y), "a", SocketSide::Input)).isNone());
  test_assert(isFloat(i.peek(socket(at({}, n.a), "a", SocketSide::Input)), -100));
  test_assert(i.clear().ok() &&
              i.peek(socket(at({}, n.a), "a", SocketSide::Input)).isNone());

  /* Interior sockets that links use. */
  connect(*n.root.def, n.group, interior, n.c, "a");
  test_assert(i.setInput("in", GraphValue::ofFloat(1)).ok());
  test_assert(i.evaluate(SocketPath::of(n.c, "value")).ok());
  test_assert(isFloat(i.peek(socket(at({}, n.group), interior)), 4));
  /* The group's def goes: its outputs do. */
  test_assert(n.root.def->setGroup(n.group, BlockId()).ok() && !n.evaluate().ok());
  test_assert(i.peek(socket(at({}, n.group), key(n.z))).isNone());
  test_assert(i.peek(socket(at({}, n.group), interior)).isNone());
}

/* Frames that go when a call is over its budget may be what other frames' inputs read. */
void testPeekOverBudget()
{
  Nest n;
  test_assert(n.inst.setInput("in", GraphValue::ofFloat(1)).ok() && n.evaluate().ok());
  test_assert(isFloat(n.inst.peek(socket(at({}, n.b), "a", SocketSide::Input)), 4));
  test_assert(n.inst.clear().ok());
  n.inst.setMaxFrames(1);
  const NodeId again = add(*n.root.def, kGraphGroupType);
  test_assert(n.root.def->setGroup(again, n.inner.id).ok());
  const SocketPath both[] = {SocketPath::of(n.b, "value"),
                             SocketPath::of(again, key(n.z))};
  litestl::util::Vector<Status> results;
  test_assert(hasCode(n.inst.evaluate(both, results), StatusCode::OutOfRange));
  test_assert(n.inst.peek(socket(at({}, n.b), "a", SocketSide::Input)).isNone());
  n.inst.setMaxFrames(100);
  test_assert(n.inst.evaluate(both, results).ok());
  test_assert(isFloat(n.inst.peek(socket(at({}, n.b), "a", SocketSide::Input)), 4));

  /* In an element, that has a group: the second one's is too much. */
  World w;
  Loop l(w);
  const Graph leaf = newGraph(w.lib, "leaf");
  const int64_t out = addSocket(*leaf.def, SocketSide::Output, "o", "float");
  const NodeId output = add(*leaf.def, "graph.output"), one = add(*leaf.def, "t.sum");
  connect(*leaf.def, one, "value", output, key(out));
  const NodeId group = add(*l.inner.def, kGraphGroupType);
  const NodeId second = add(*l.inner.def, kGraphGroupType);
  test_assert(l.inner.def->setGroup(group, leaf.id).ok());
  test_assert(l.inner.def->setGroup(second, leaf.id).ok());
  connect(*l.inner.def, group, key(out), l.add, "b");
  connect(*l.inner.def, second, key(out), l.square, "a");
  l.set(table({1}, {{"a", {1}}, {"c", {3}}}));
  l.inst.setMaxFrames(1);
  test_assert(hasCode(l.evaluate(), StatusCode::OutOfRange));
  SocketPath b = socket(at({l.node}, l.add), "b", SocketSide::Input);
  b.node.elements.append(1);
  test_assert(l.inst.peek(b).isNone());
  l.inst.setMaxFrames(100);
  test_assert(l.evaluate().ok() && isFloat(l.inst.peek(b), 0));
}

void testElements()
{
  World w;
  Loop l(w);
  test_assert(l.inst.setInput("table", tableValue(table({5, 6}, {{"a", {1, 2}}}))).ok());
  test_assert(l.root.def->setLiteral(l.node, key(l.b), PropValue::ofFloat(10)).ok());
  test_assert(l.evaluate().ok());
  auto inside = [&](int64_t element, NodeId node) {
    NodePath p = at({l.node}, node);
    p.elements.append(element);
    return p;
  };
  auto peek = [&](int64_t element,
                  NodeId node,
                  std::string_view key,
                  SocketSide side = SocketSide::Output) {
    return l.inst.peek(socket(inside(element, node), key, side));
  };
  test_assert(isFloat(peek(5, l.add, "value"), 11) &&
              isFloat(peek(6, l.add, "value"), 12));
  test_assert(isFloat(peek(6, l.add, "a", SocketSide::Input), 2));
  test_assert(isFloat(peek(6, l.innerIn, key(l.a)), 2));  /* a column */
  test_assert(isFloat(peek(6, l.innerIn, key(l.b)), 10)); /* the node's own */
  test_assert(isFloat(peek(6, NodeId(), key(l.sum)), 12));
  test_assert(peek(7, l.add, "value").isNone());
  test_assert(l.inst.peek(socket(at({l.node}, l.add), "value")).isNone()); /* which? */
  test_assert(inside(5, l.add).toString() ==
              std::to_string(l.node.value) + "/" + std::to_string(l.add.value) + "[5]");
  test_assert(!(inside(5, l.add) == inside(6, l.add)) &&
              inside(5, l.add) == inside(5, l.add));

  /* Reports. */
  NodeReport r = l.inst.report(inside(5, l.add));
  test_assert(r.state == State::Ok && r.runs == 1 && r.current &&
              r.totalSeconds >= r.seconds);
  test_assert(l.inst.report(inside(7, l.add)).state == State::NotEvaluated);
  const NodeReport whole = l.inst.report(at({}, l.node));
  test_assert(whole.elements == 2 && whole.current && whole.memory.elements == 2);
  test_assert(whole.seconds > 0 && whole.totalSeconds == whole.seconds);
  test_assert(whole.memory.kernels == 1 + 2 * 2 && whole.memory.frames == 2);
  test_assert(whole.memory.payloadBytes > 0 && whole.memory.bytes > 2 * r.memory.bytes);

  /* An element that fails, and one that isn't reached. The origin leads to it. */
  l.set(table({5, 6, 7}, {{"a", {1, -100, 3}}}));
  test_assert(!l.evaluate().ok() && l.error());
  test_assert(l.error()->origin == inside(6, l.add));
  r = l.inst.report(l.error()->origin);
  test_assert(r.state == State::Failed &&
              r.error.status.code() == StatusCode::OutOfRange);
  test_assert(r.error.origin.elements.size() == 0); /* as seen in the element */
  test_assert(peek(6, l.add, "value").isNone() && isFloat(peek(5, l.add, "value"), 11));
  test_assert(l.inst.report(inside(5, l.add)).current);
  test_assert(!l.inst.report(inside(7, l.add)).current);
  test_assert(
      hasCode(l.inst.evaluate(socket(inside(5, l.add), "value")), StatusCode::NotFound));
  test_assert(hasCode(l.inst.drop(inside(5, l.add)), StatusCode::Unsupported));

  /* One that wasn't reached since something else moved, though the foreach is current. */
  l.set(table({5, 6, 7}, {{"a", {1, 2, 3}}}));
  test_assert(l.evaluate().ok() && l.inst.report(inside(7, l.add)).current);
  test_assert(l.inner.def->setLiteral(l.add, "b", PropValue::ofFloat(0)).ok());
  l.set(table({5, 6, 7}, {{"a", {1, -100, 3}}}));
  test_assert(!l.evaluate().ok() && l.inst.report(at({}, l.node)).current);
  test_assert(l.inst.report(inside(5, l.add)).current);
  test_assert(!l.inst.report(inside(7, l.add)).current);

  /* Evaluated again after an edit that concerns all of them: not what it was. */
  l.set(table({5, 6}, {{"a", {1, 2}}}));
  test_assert(l.evaluate().ok() && l.inst.report(inside(5, l.add)).current);
  test_assert(l.inner.def->setLiteral(l.add, "b", PropValue::ofFloat(1)).ok());
  test_assert(!l.inst.report(inside(5, l.add)).current);
  /* An element that shares a state has none to look at. */
  test_assert(l.root.def->setParam(l.node, kMaxCachedParam, PropValue::ofInt(1)).ok());
  test_assert(l.evaluate().ok());
  test_assert(isFloat(peek(5, l.add, "value"), 11) && peek(6, l.add, "value").isNone());
}

/* A foreach in a foreach: the keys in order. */
void testNestedElements()
{
  World w;
  Loop l(w);
  const Graph leaf = newGraph(w.lib, "leaf");
  const int64_t x = addSocket(*leaf.def, SocketSide::Input, "index", "float");
  const int64_t y = addSocket(*leaf.def, SocketSide::Output, "y", "float");
  const NodeId in = add(*leaf.def, "graph.input"), out = add(*leaf.def, "graph.output");
  const NodeId sum = add(*leaf.def, "t.sum");
  connect(*leaf.def, in, key(x), sum, "a");
  connect(*leaf.def, sum, "value", out, key(y));
  GraphDef &d = *l.inner.def;
  const NodeId table2 = add(d, "t.range"), each = add(d, kGraphForeachType);
  const NodeId total = add(d, "t.total");
  test_assert(d.setGroup(each, leaf.id).ok());
  test_assert(d.setParam(total, "column", PropValue::ofString("y")).ok());
  test_assert(leaf.def->setInterfaceName(x, "i").ok()); /* t.range's column */
  connect(d, l.innerIn, key(l.a), table2, "count");
  connect(d, table2, "elements", each, kElementsSocket);
  connect(d, each, kElementsSocket, total, "elements");
  connect(d, total, "value", l.add, "b");
  l.set(table({4, 5}, {{"a", {2, 3}}}));
  test_assert(l.evaluate().ok());
  auto peek = [&](std::initializer_list<int64_t> elements) {
    NodePath p = at({l.node, each}, sum);
    for (int64_t e : elements) {
      p.elements.append(e);
    }
    return l.inst.peek(socket(p, "a", SocketSide::Input));
  };
  test_assert(isFloat(peek({4, 1}), 1) && isFloat(peek({5, 2}), 2) &&
              isFloat(peek({5, 0}), 0));
  test_assert(peek({4, 2}).isNone() && peek({2, 5}).isNone() && peek({5}).isNone());
  NodePath p = at({l.node, each}, sum);
  p.elements.append(5);
  p.elements.append(1);
  test_assert(l.inst.report(p).state == State::Ok && l.inst.report(p).current);
  test_assert(l.root.def->setParam(l.node, kMaxCachedParam, PropValue::ofInt(1)).ok());
  test_assert(!l.inst.report(p).current);
}

/* Nothing of a group that can't be evaluated, though its frame may still be there. */
void testPeekLost()
{
  Nest n;
  GraphInstance &i = n.inst;
  const std::string interior = "@" + std::to_string(n.y.value) + ".value";
  connect(*n.root.def, n.group, interior, n.c, "a");
  const SocketPath c = SocketPath::of(n.c, "value");
  const SocketPath inside = socket(at({}, n.group), interior);
  const SocketPath whole = socket(at({}, n.group), key(n.z));
  test_assert(i.evaluate(c).ok() && n.evaluate().ok());
  test_assert(isFloat(i.peek(inside), 1) && isFloat(i.peek(whole), 3));
  test_assert(n.w.lib.markDeleted(n.inner.h).ok());
  test_assert(!n.evaluate().ok() &&
              i.evaluate(c).ok()); /* a link from nowhere has the default */
  test_assert(i.peek(inside).isNone() && i.peek(whole).isNone());
  test_assert(n.w.lib.restoreFromHistory(n.inner.h).ok());
  test_assert(i.evaluate(c).ok() && isFloat(i.peek(inside), 1));
}

/* A group that contains itself, as only an undo record could have it: its frame is still
 * that of the graph it had, where nodes of the other may be found by id. */
void testPeekSelf(bool nested)
{
  Nest n;
  GraphInstance &i = n.inst;
  GraphDef &r = *n.root.def;
  const NodeState y = n.inner.def->stateOf(*n.inner.def->node(n.y));
  test_assert(r.insertNode(y).ok());
  Graph mid = newGraph(n.w.lib, "mid");
  const NodeId leaf = add(*mid.def, kGraphGroupType);
  test_assert(mid.def->setGroup(leaf, n.inner.id).ok());
  NodeId group = n.group;
  std::string interior = "@" + std::to_string(n.y.value) + ".value";
  if (nested) {
    group = add(r, kGraphGroupType);
    test_assert(r.setGroup(group, mid.id).ok());
    interior = "@" + std::to_string(leaf.value) + "/" + interior.substr(1);
  }
  connect(r, group, interior, n.c, "a");
  const SocketPath c = SocketPath::of(n.c, "value"),
                   inside = socket(at({}, group), interior);
  test_assert(i.evaluate(c).ok() && isFloat(i.peek(inside), 1));
  GraphDef &changed = nested ? *mid.def : r;
  const NodeId place = nested ? leaf : group;
  test_assert(changed.restoreRef(place, "", n.root.id).ok());
  test_assert(!i.evaluate(c).ok() && i.peek(inside).isNone());
  test_assert(changed.restoreRef(place, "", n.inner.id).ok());
  test_assert(i.evaluate(c).ok() && isFloat(i.peek(inside), 1));
}

void testReports()
{
  Nest n;
  GraphInstance &i = n.inst;
  auto report = [&](NodeId node) { return i.report(at({}, node)); };
  test_assert(report(n.group).state == State::NotEvaluated && !report(n.group).current);
  test_assert(i.setInput("in", GraphValue::ofFloat(10)).ok() && n.evaluate().ok());
  NodeReport g = report(n.group);
  test_assert(g.state == State::Ok && g.runs == 2 && g.current);
  test_assert(g.memory.frames == 1 && g.memory.kernels == 2 && g.memory.inputs == 5);
  const NodeReport y = i.report(at({n.group}, n.y)), z = i.report(at({n.group}, n.zNode));
  test_assert(g.seconds == y.seconds + z.seconds && y.totalSeconds == y.seconds);
  test_assert(y.memory.kernels == 1 && y.memory.frames == 0 && y.memory.bytes > 0);
  test_assert(report(n.c).state == State::NotEvaluated && report(n.c).memory.bytes == 0);

  /* Time adds up. */
  test_assert(i.setInput("in", GraphValue::ofFloat(11)).ok());
  test_assert(!report(n.a).current && !report(n.group).current);
  test_assert(n.evaluate().ok() && report(n.a).current && report(n.a).runs == 2);
  const NodeReport y2 = i.report(at({n.group}, n.y));
  test_assert(y2.totalSeconds == y.totalSeconds + y2.seconds);
  g = report(n.group);
  test_assert(g.runs == 4 && g.totalSeconds >= y2.totalSeconds);

  /* An edit: what doesn't depend on it is evaluated, and current then. */
  test_assert(n.root.def->setLiteral(n.b, "b", PropValue::ofFloat(1)).ok());
  test_assert(!report(n.a).current); /* it might */
  test_assert(i.evaluate(SocketPath::of(n.a, "value")).ok());
  test_assert(report(n.a).current && report(n.group).current && !report(n.b).current);
  test_assert(n.evaluate().ok() && report(n.b).current);

  /* All of it is looked at again: what isn't asked for wasn't. */
  n.w.rt.touch();
  test_assert(i.evaluate(SocketPath::of(n.a, "value")).ok());
  test_assert(report(n.a).current && !report(n.b).current && !report(n.group).current);
  test_assert(n.evaluate().ok() && report(n.group).current);

  /* The worst of what is in a group. */
  test_assert(i.setInput("in", GraphValue::ofFloat(-100)).ok() && !n.evaluate().ok());
  g = report(n.group);
  test_assert(g.state == State::InputFailed && g.error.origin == at({}, n.a));
  test_assert(i.setInput("in", GraphValue::ofFloat(0)).ok());
  test_assert(n.inner.def->setLiteral(n.y, "b", PropValue::ofFloat(-100)).ok());
  test_assert(!n.evaluate().ok()); /* y is -100, so z fails */
  g = report(n.group);
  test_assert(g.state == State::Failed && g.error.origin == at({n.group}, n.zNode));
  test_assert(i.report(at({n.group}, n.y)).state == State::Ok);
  test_assert(report(n.b).state == State::InputFailed);
  /* Whichever comes first: y fails, and z can't run. */
  test_assert(n.inner.def->setLiteral(n.y, "b", PropValue::ofFloat(0)).ok());
  test_assert(n.root.def->setLiteral(n.a, "b", PropValue::ofFloat(-150)).ok());
  test_assert(!n.evaluate().ok());
  g = report(n.group);
  test_assert(g.state == State::Failed && g.error.origin == at({n.group}, n.y));
  test_assert(i.report(at({n.group}, n.zNode)).state == State::InputFailed);
  test_assert(n.root.def->setLiteral(n.a, "b", PropValue::ofFloat(0)).ok());
  /* A group inside that can't be evaluated. */
  const NodeId lost = add(*n.inner.def, kGraphGroupType);
  const int64_t extra = addSocket(*n.inner.def, SocketSide::Output, "extra", "float");
  const Graph gone = newGraph(n.w.lib, "gone");
  const int64_t o = addSocket(*gone.def, SocketSide::Output, "o", "float");
  test_assert(n.inner.def->setGroup(lost, gone.id).ok());
  connect(*n.inner.def, lost, key(o), n.out, key(extra));
  test_assert(n.inner.def->setLiteral(n.y, "b", PropValue::ofFloat(1)).ok());
  test_assert(i.evaluate(socket(at({}, n.group), key(extra))).ok() && n.evaluate().ok());
  test_assert(report(n.group).state == State::Ok);
  test_assert(n.w.lib.markDeleted(gone.h).ok());
  test_assert(!i.evaluate(socket(at({}, n.group), key(extra))).ok());
  g = report(n.group);
  test_assert(g.state == State::Failed && g.error.origin == at({n.group}, lost));
  /* A group in it counts with what is in it. */
  test_assert(n.w.lib.restoreFromHistory(gone.h).ok());
  const NodeId fails = add(*gone.def, "t.sum"), goneOut = add(*gone.def, "graph.output");
  test_assert(gone.def->setLiteral(fails, "a", PropValue::ofFloat(-100)).ok());
  connect(*gone.def, fails, "value", goneOut, key(o));
  test_assert(!i.evaluate(socket(at({}, n.group), key(extra))).ok());
  g = report(n.group);
  test_assert(g.state == State::Failed && g.error.origin == at({n.group, lost}, fails));
  test_assert(g.memory.frames == 2 && g.memory.kernels == 3);
  /* The group's own error. */
  test_assert(n.root.def->setGroup(n.group, BlockId()).ok() && !n.evaluate().ok());
  g = report(n.group);
  test_assert(g.state == State::Failed && g.error.origin == at({}, n.group));
}

void testDrop()
{
  Nest n;
  GraphInstance &i = n.inst;
  auto drop = [&](NodePath p) { return i.drop(p); };
  test_assert(hasCode(drop(at({}, n.a)), StatusCode::NotFound));
  test_assert(i.setInput("in", GraphValue::ofFloat(10)).ok() && n.evaluate().ok());
  const uint64_t stamp = i.changedAt(SocketPath::output("out"));
  auto runs = [&, last = i.totalRuns()]() mutable {
    const int r = int(i.totalRuns() - last);
    last = i.totalRuns();
    return r;
  };
  /* A node: it runs when it is needed, and what is below it, as it might give another
   * value. */
  test_assert(drop(at({}, n.a)).ok() &&
              i.report(at({}, n.a)).state == State::NotEvaluated);
  test_assert(i.peek(SocketPath::of(n.a, "value")).isNone());
  test_assert(
      isFloat(*i.value(SocketPath::output("out")), 13)); /* a result isn't the node's */
  test_assert(n.evaluate().ok() && runs() == 2); /* a, and y, which gives what it did */
  test_assert(i.changedAt(SocketPath::output("out")) == stamp);
  test_assert(n.evaluate().ok() && runs() == 0);
  /* One that nothing needs. */
  test_assert(i.evaluate(SocketPath::of(n.c, "value")).ok() && runs() == 1);
  test_assert(drop(at({}, n.c)).ok() && n.evaluate().ok() && runs() == 0);
  test_assert(i.evaluate(SocketPath::of(n.c, "value")).ok() && runs() == 1);
  /* Inside a group; twice; a node without a kernel. */
  test_assert(drop(at({n.group}, n.zNode)).ok() && drop(at({n.group}, n.zNode)).ok());
  test_assert(drop(at({}, n.rootInput)).ok());
  test_assert(n.evaluate().ok() && runs() == 2 &&
              isFloat(*i.value(SocketPath::output("out")), 13));

  /* A group: its frames go, also those that only a removed node's link led to. */
  const GraphMemory before = i.memory();
  test_assert(i.frameCount() == 1 && drop(at({}, n.group)).ok() && i.frameCount() == 0);
  test_assert(i.memory().frames == 1 && i.memory().kernels == before.kernels - 2);
  test_assert(i.report(at({}, n.group)).memory.frames == 0);
  test_assert(hasCode(drop(at({n.group}, n.y)), StatusCode::NotFound));
  test_assert(n.evaluate().ok() && runs() == 3 && i.frameCount() == 1);
  test_assert(isFloat(*i.value(SocketPath::output("out")), 13));
  /* Prepared sockets in it find their way. */
  Result<SocketHandle> h = i.prepare(socket(at({n.group}, n.y), "value"));
  litestl::util::Vector<int> moved;
  test_assert(h.ok() && i.evaluate({&*h, 1}, moved).ok() && isFloat(*i.value(*h), 11));
  test_assert(drop(at({}, n.group)).ok() && i.evaluate({&*h, 1}, moved).ok());
  test_assert(moved.size() == 0 && isFloat(*i.value(*h), 11) && runs() == 1);
  test_assert(n.evaluate().ok());

  /* An output of a group that no kernel of it feeds. */
  const int64_t plain = addSocket(*n.inner.def, SocketSide::Output, "plain", "float");
  connect(*n.inner.def, n.in, key(n.x), n.out, key(plain));
  connect(*n.root.def, n.group, key(plain), n.c, "a");
  const SocketPath c = SocketPath::of(n.c, "value");
  test_assert(i.evaluate(c).ok() && isFloat(*i.value(c), 10));
  test_assert(drop(at({}, n.group)).ok() &&
              i.setInput("in", GraphValue::ofFloat(11)).ok());
  test_assert(i.evaluate(c).ok() && isFloat(*i.value(c), 11));
  test_assert(i.setInput("in", GraphValue::ofFloat(10)).ok() && n.evaluate().ok());

  /* A kernel's blocks aren't followed for it any more, and are when it runs again. */
  const BlockHandle t = newThing(n.w.lib, "t", 3);
  const NodeId read = add(*n.root.def, "t.read");
  test_assert(n.root.def->setParam(read, "thing", toThing(idOf(n.w.lib, t))).ok());
  const SocketPath p = SocketPath::of(read, "value");
  test_assert(i.evaluate(p).ok() && isFloat(*i.value(p), 3));
  test_assert(drop(at({}, read)).ok() && n.evaluate().ok());
  const uint64_t edit = n.w.rt.lastEdit();
  n.w.rt.ext<GraphSystem>().follow(idOf(n.w.lib, t),
                                   1); /* so that a notification is an edit */
  n.w.thing(t).value = 4;
  n.w.notify(idOf(n.w.lib, t));
  test_assert(n.w.rt.lastEdit() > edit && i.isCurrent(SocketPath::output("out")));
  test_assert(i.evaluate(p).ok() && isFloat(*i.value(p), 4));
  n.w.thing(t).value = 5;
  n.w.notify(idOf(n.w.lib, t));
  test_assert(i.evaluate(p).ok() && isFloat(*i.value(p), 5));

  /* A foreach: its elements and its table. */
  World w;
  Loop l(w);
  l.set(table({1, 2}, {{"a", {1, 2}}}));
  test_assert(l.is({1, 2}, {{"sum", {1, 2}}, {"c2", {0, 0}}}, 4) &&
              l.inst.elementCount() == 2);
  const size_t payloads = l.inst.report(at({}, l.node)).memory.payloadBytes;
  test_assert(payloads > 0 && l.inst.drop(at({}, l.node)).ok() &&
              l.inst.elementCount() == 0);
  const NodeReport dropped = l.inst.report(at({}, l.node));
  test_assert(dropped.memory.payloadBytes == 0 && dropped.elements == 0);
  test_assert(l.is({1, 2}, {{"sum", {1, 2}}, {"c2", {0, 0}}}, 4));
  l.set(table({1, 2}, {{"a", {1, 3}}}));
  test_assert(l.is({1, 2}, {{"sum", {1, 3}}, {"c2", {0, 0}}}, 1));
  /* Nothing is left of it. */
  test_assert(l.inst.clear().ok() && l.inst.memory().bytes == 0);
}

/* What the system follows goes with the last that holds it. */
void testFollowing()
{
  World w;
  GraphSystem &system = w.rt.ext<GraphSystem>();
  litestl::util::Vector<BlockId> ids;
  for (int i = 0; i < 300; i++) {
    ids.append(BlockId{ClientId::of(kCoredccClient), LocalId{uint64_t(i) + 1, 7}});
  }
  BlockVersionPtr held = system.follow(ids[0]), part = system.follow(ids[1], 2);
  const uint64_t version = held->version;
  system.follow(ids[1], 4);
  for (const BlockId &id : ids) {
    system.follow(id);
  }
  test_assert(system.blockCount() < 130);
  test_assert(system.follow(ids[0]) == held && held->version == version);
  test_assert(system.follow(ids[1], 2) == part);
  w.notify(ids[0]);
  test_assert(held->version > version);
  /* Many that are held stay. */
  litestl::util::Vector<BlockVersionPtr> all;
  for (const BlockId &id : ids) {
    all.append(system.follow(id));
  }
  test_assert(system.blockCount() == 300);
  for (int i = 0; i < 300; i++) {
    test_assert(system.follow(ids[i]) == all[i]);
  }
  w.rt.ext<ChangeBus>().notifyAll();
  test_assert(system.blockCount() == 0 && !(system.follow(ids[0]) == held));

  /* Through an instance: thousands of nodes whose blocks come and go. */
  Loop l(w);
  const NodeId read = add(*l.inner.def, "t.read");
  connect(*l.inner.def, read, "value", l.add, "b");
  l.set(table({1}, {{"a", {1}}}));
  for (int i = 0; i < 400; i++) {
    const BlockHandle h = newThing(w.lib, "t", i);
    test_assert(l.inner.def->setParam(read, "thing", toThing(idOf(w.lib, h))).ok());
    test_assert(l.evaluate().ok());
    w.thing(h).value = i + 1000;
    w.notify(idOf(w.lib, h));
    test_assert(l.evaluate().ok() && isFloat(*l.column("sum"), i + 1001));
  }
  test_assert(system.blockCount() < 130);

  /* Many that are in use at once. */
  GraphDef def(&w.rt, &w.lib);
  GraphInstance inst(&w.rt, &w.lib, &def);
  litestl::util::Vector<SocketPath> paths;
  litestl::util::Vector<BlockHandle> things;
  for (int i = 0; i < 300; i++) {
    const NodeId node = add(def, "t.read");
    things.append(newThing(w.lib, "t", i));
    test_assert(def.setParam(node, "thing", toThing(idOf(w.lib, things[i]))).ok());
    paths.append(SocketPath::of(node, "value"));
  }
  litestl::util::Vector<Status> results;
  test_assert(inst.evaluate({paths.data(), size_t(paths.size())}, results).ok());
  for (int i = 0; i < 300; i++) {
    w.thing(things[i]).value = -i;
    w.notify(idOf(w.lib, things[i]));
  }
  test_assert(inst.evaluate({paths.data(), size_t(paths.size())}, results).ok());
  for (int i = 0; i < 300; i++) {
    test_assert(isFloat(*inst.value(paths[i]), -i));
  }
}

} // namespace

int main()
{
  testPeek();
  testPeekOverBudget();
  testElements();
  testNestedElements();
  testPeekLost();
  testPeekSelf(false);
  testPeekSelf(true);
  testReports();
  testDrop();
  testFollowing();
  return test_end();
}
