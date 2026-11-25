/* GraphInstance: what an edit makes it look at, prepared sockets, and memory. */
#include "foreach_fixture.h"

using namespace fixture;

test_init;

namespace {

SocketPath out(NodeId n, const char *socket = "value")
{
  return SocketPath::of(n, socket);
}

/* What an evaluation cost. */
struct Cost {
  uint64_t runs = 0, visits = 0, full = 0;
};

template <class Pull> Cost costOf(GraphInstance &i, const Pull &pull)
{
  const Cost c{i.totalRuns(), i.totalVisits(), i.totalFullPasses()};
  pull();
  return Cost{
      i.totalRuns() - c.runs, i.totalVisits() - c.visits, i.totalFullPasses() - c.full};
}

/* A row of 100 sums with a read at its start, and a group with a chain of 10 at its end.
 */
struct Scene {
  World w;
  Graph g, in;
  BlockHandle thing, elsewhere;
  NodeId read, group;
  std::vector<NodeId> row, inside;
  int64_t gi = 0, go = 0;
  std::unique_ptr<GraphInstance> inst;

  Scene()
  {
    thing = newThing(w.lib, "thing", 1);
    elsewhere = newThing(w.lib, "elsewhere", 1);
    g = newGraph(w.lib, "g");
    in = newGraph(w.lib, "in");
    gi = addSocket(*in.def, SocketSide::Input, "i", "float");
    go = addSocket(*in.def, SocketSide::Output, "o", "float");
    chain(*in.def, inside, 10);
    connect(*in.def, add(*in.def, "graph.input"), key(gi), inside[0], "a");
    connect(*in.def, inside[9], "value", add(*in.def, "graph.output"), key(go));
    GraphDef &d = *g.def;
    read = add(d, "t.read");
    test_assert(d.setParam(read, "thing", toThing(idOf(w.lib, thing))).ok());
    chain(d, row, 100);
    connect(d, read, "value", row[0], "a");
    group = add(d, "graph.group");
    test_assert(d.setGroup(group, in.id).ok());
    connect(d, row[99], "value", group, key(gi));
    inst = std::make_unique<GraphInstance>(&w.rt, &w.lib, g.id);
    test_assert(pull().ok());
  }
  void chain(GraphDef &d, std::vector<NodeId> &nodes, int n)
  {
    for (int i = 0; i < n; i++) {
      nodes.push_back(add(d, "t.sum"));
      if (i > 0) {
        connect(d, nodes[size_t(i - 1)], "value", nodes[size_t(i)], "a");
      }
    }
  }
  SocketPath end() const
  {
    return SocketPath::of(group, key(go));
  }
  Status pull()
  {
    return inst->evaluate(end());
  }
  /* Evaluates after an edit, which moved the value by `by`. */
  bool costs(uint64_t runs, uint64_t mostVisits, double by, uint64_t full = 0)
  {
    const double before = inst->lastGood(end())->asFloat();
    test_assert(!inst->isCurrent(end()));
    const Cost c = costOf(*inst, [&]() { test_assert(pull().ok()); });
    const bool ok = c.runs == runs && c.visits <= mostVisits && c.full == full &&
                    inst->value(end())->asFloat() == before + by;
    if (!ok) {
      printf("%d runs, %d visits, %d full passes, moved by %g\n",
             int(c.runs),
             int(c.visits),
             int(c.full),
             inst->value(end())->asFloat() - before);
    }
    return ok && inst->isCurrent(end());
  }
  bool free()
  {
    const bool current = inst->isCurrent(end());
    const Cost c = costOf(*inst, [&]() { test_assert(pull().ok()); });
    return current && c.runs == 0 && c.visits == 0 && c.full == 0;
  }
  Status literal(GraphDef &d, NodeId n, const char *socket, double v)
  {
    return d.setLiteral(n, socket, PropValue::ofFloat(v));
  }
};

/* What an instance doesn't depend on costs nothing, and leaves it current. */
void testElsewhere()
{
  Scene s;
  World &w = s.w;
  test_assert(s.free());
  const Graph other = newGraph(w.lib, "other");
  test_assert(s.pull().ok()); /* a new block may be one that was missing */
  const NodeId n = add(*other.def, "t.sum");
  test_assert(s.free());
  test_assert(s.literal(*other.def, n, "a", 3).ok() && s.free());
  addSocket(*other.def, SocketSide::Input, "i", "float");
  test_assert(s.free());
  /* What it said of one edit isn't its answer for the next. */
  test_assert(s.literal(*other.def, n, "a", 4).ok() && s.inst->isCurrent(s.end()));
  test_assert(s.literal(*s.g.def, s.row[99], "b", 2).ok() && s.costs(11, 40, 2));
  test_assert(s.literal(*other.def, n, "a", 5).ok() && s.inst->isCurrent(s.end()));
  test_assert(s.literal(*s.g.def, s.row[99], "b", 0).ok() && s.costs(11, 40, -2));
  test_assert(other.def->setGroup(add(*other.def, "graph.group"), s.g.id).ok());
  test_assert(s.pull().ok());
  test_assert(other.def->removeNode(n).ok() && s.free());
  /* A block that another instance uses. */
  const NodeId reads = add(*other.def, "t.read");
  test_assert(
      other.def->setParam(reads, "thing", toThing(idOf(w.lib, s.elsewhere))).ok());
  GraphInstance second(&w.rt, &w.lib, other.id);
  test_assert(s.pull().ok() && second.evaluate(out(reads)).ok());
  w.notify(idOf(w.lib, s.elsewhere));
  test_assert(!second.isCurrent(out(reads)) && s.free());
  w.thing(s.thing).value = 2;
  w.notify(idOf(w.lib, s.thing));
  test_assert(s.costs(111, 400, 1));
  /* A node of its def that nothing leads to yet. */
  const NodeId loose = add(*s.g.def, "t.sum");
  test_assert(s.costs(0, 400, 0));
  test_assert(s.literal(*s.g.def, loose, "a", 3).ok() && !s.inst->isCurrent(s.end()));
  const Cost c = costOf(*s.inst, [&]() { test_assert(s.pull().ok()); });
  test_assert(c.runs == 0 && c.visits == 0 && s.inst->isCurrent(s.end()));
  /* The plain touch says nothing. */
  w.rt.touch();
  test_assert(s.costs(0, 400, 0, 1));
  /* Nor does one before an edit that does. */
  w.rt.touch();
  test_assert(s.literal(*s.g.def, s.row[99], "b", 1).ok());
  test_assert(s.costs(11, 400, 1, 1));
}

/* What an edit can't have moved isn't looked at. */
void testDirtyCone()
{
  Scene s;
  GraphDef &d = *s.g.def;
  test_assert(s.literal(d, s.row[99], "b", 5).ok());
  test_assert(s.costs(11, 40, 5));
  test_assert(s.literal(d, s.row[50], "b", 5).ok());
  test_assert(s.costs(60, 200, 5));
  test_assert(s.literal(*s.in.def, s.inside[9], "b", 2).ok());
  test_assert(s.costs(1, 8, 2));
  /* Several at once, one behind another, and one that is undone before it is looked at.
   */
  test_assert(s.literal(d, s.row[98], "b", 5).ok() &&
              s.literal(d, s.row[99], "b", 6).ok() &&
              s.literal(*s.in.def, s.inside[9], "b", 4).ok());
  test_assert(s.costs(12, 60, 5 + 1 + 2));
  test_assert(s.literal(d, s.row[0], "b", 9).ok() && s.literal(d, s.row[0], "b", 0).ok());
  test_assert(s.costs(s.inst->report(NodePath{{}, s.row[0]}).runs ? 1 : 0, 400, 0));
  /* Nothing past a value that is the same. */
  test_assert(s.literal(d, s.row[97], "b", 1).ok() &&
              s.literal(d, s.row[98], "b", 4).ok());
  test_assert(s.costs(2, 60, 0));
  /* Links of a def: its frames are looked at, others aren't. */
  const LinkId old = d.node(s.row[99])->linkInto("a")->id;
  test_assert(d.unlink(old).ok());
  const double upTo = s.inst->value(s.end())->asFloat() - 6 - 4;
  test_assert(s.costs(11, 400, -upTo));
  connect(d, s.row[98], "value", s.row[99], "a");
  test_assert(s.costs(11, 400, upTo));
  const LinkId in = s.in.def->node(s.inside[9])->linkInto("a")->id;
  test_assert(s.in.def->unlink(in).ok() && s.pull().ok());
  connect(*s.in.def, s.inside[8], "value", s.inside[9], "a");
  const double all = s.inst->value(s.end())->asFloat();
  test_assert(s.costs(1, 80, upTo + 6 + 4 - all));
  /* Of an interface: everything is. */
  addSocket(*s.in.def, SocketSide::Input, "j", "float");
  test_assert(s.costs(0, 500, 0, 1));
  /* Off, it always is. */
  s.inst->setIncremental(false);
  test_assert(s.literal(d, s.row[99], "b", 16).ok());
  test_assert(s.costs(11, 500, 10, 1));
  s.inst->setIncremental(true);
  test_assert(s.literal(d, s.row[99], "b", 6).ok());
  test_assert(s.costs(11, 40, -10));
}

/* An input of the instance: what reads that one. */
void testOwnInputs()
{
  World w;
  const Graph g = newGraph(w.lib, "g");
  GraphDef &d = *g.def;
  const int64_t a = addSocket(d, SocketSide::Input, "a", "float");
  const int64_t b = addSocket(d, SocketSide::Input, "b", "float");
  const NodeId input = add(d, "graph.input"), sa = add(d, "t.sum"), sb = add(d, "t.sum");
  const NodeId second = add(d, "graph.input"), sc = add(d, "t.sum");
  connect(d, input, key(a), sa, "a");
  connect(d, input, key(b), sb, "a");
  connect(d, second, key(b), sc, "a");
  GraphInstance i(&w.rt, &w.lib, g.id);
  std::vector<SocketPath> all = {out(sa), out(sb), out(sc)};
  litestl::util::Vector<Status> results;
  auto pull = [&]() { test_assert(i.evaluate(all, results).ok()); };
  pull();
  test_assert(i.setInput("b", GraphValue::ofFloat(4)).ok());
  Cost c = costOf(i, pull);
  test_assert(c.runs == 2 && c.full == 0 && c.visits <= 4);
  test_assert(i.value(out(sb))->asFloat() == 4 && i.value(out(sc))->asFloat() == 4);
  test_assert(i.setInput("a", GraphValue::ofFloat(1)).ok() &&
              i.setInput("b", GraphValue::ofFloat(2)).ok());
  c = costOf(i, pull);
  test_assert(c.runs == 3 && c.full == 0 && i.value(out(sa))->asFloat() == 1);
  test_assert(i.setInput("b", GraphValue()).ok());
  c = costOf(i, pull);
  test_assert(c.runs == 2 && c.full == 0 && i.value(out(sc))->asFloat() == 0);
  /* After the interface's order changed, too. */
  test_assert(d.moveInterface(b, 0).ok());
  test_assert(i.setInput("a", GraphValue::ofFloat(7)).ok());
  pull();
  test_assert(i.value(out(sa))->asFloat() == 7 && i.value(out(sb))->asFloat() == 0);
  test_assert(i.setInput("b", GraphValue::ofFloat(8)).ok());
  c = costOf(i, pull);
  test_assert(c.runs == 2 && c.full == 0 && i.value(out(sb))->asFloat() == 8);
  /* retry is for everything. */
  test_assert(i.retry().ok());
  c = costOf(i, pull);
  test_assert(c.runs == 0 && c.full == 1);
}

/* Of a foreach: an edit inside its def or of a block its elements use. */
void testForeachs()
{
  World w;
  const BlockHandle thing = newThing(w.lib, "thing", 1);
  const Graph in = newGraph(w.lib, "in"), g = newGraph(w.lib, "g"),
              deep = newGraph(w.lib, "deep");
  const int64_t dout = addSocket(*deep.def, SocketSide::Output, "o", "float");
  const NodeId read = add(*deep.def, "t.read");
  test_assert(deep.def->setParam(read, "thing", toThing(idOf(w.lib, thing))).ok());
  connect(*deep.def, read, "value", add(*deep.def, "graph.output"), key(dout));
  const int64_t o = addSocket(*in.def, SocketSide::Output, "o", "float");
  const NodeId group = add(*in.def, "graph.group"), sum = add(*in.def, "t.sum");
  test_assert(in.def->setGroup(group, deep.id).ok());
  connect(*in.def, group, key(dout), sum, "a");
  connect(*in.def, sum, "value", add(*in.def, "graph.output"), key(o));
  GraphDef &d = *g.def;
  const NodeId range = add(d, "t.range"), each = add(d, "graph.foreach"),
               apart = add(d, "t.sum");
  test_assert(d.setLiteral(range, "count", PropValue::ofFloat(5)).ok());
  test_assert(d.setGroup(each, in.id).ok());
  connect(d, range, "elements", each, kElementsSocket);
  GraphInstance i(&w.rt, &w.lib, g.id);
  std::vector<SocketPath> all = {SocketPath::of(each, kElementsSocket), out(apart)};
  litestl::util::Vector<Status> results;
  auto pull = [&]() {
    test_assert(i.evaluate(all, results).ok() && results[0].ok() && results[1].ok());
  };
  auto value = [&]() {
    const Elements *t = i.value(all[0])->payload<Elements>();
    return t && t->size() == 5 ? t->columns[0].values[4].asFloat() : -1;
  };
  pull();
  test_assert(value() == 1 && i.totalRuns() == 1 + 5 * 2 + 1);
  Cost c = costOf(i, pull);
  test_assert(c.visits == 0);
  w.thing(thing).value = 2;
  w.notify(idOf(w.lib, thing));
  c = costOf(i, pull);
  test_assert(value() == 2 && c.runs == 5 * 2 && c.full == 0);
  test_assert(in.def->setLiteral(sum, "b", PropValue::ofFloat(1)).ok());
  c = costOf(i, pull);
  test_assert(value() == 3 && c.runs == 5 && c.full == 0);
  /* A link in the def of a def. */
  test_assert(deep.def->unlink(deep.def->node(read)->linksOut()[0]->id).ok());
  c = costOf(i, pull);
  test_assert(value() == 1 && c.full == 0);
  /* The block isn't used any more. */
  w.notify(idOf(w.lib, thing));
  c = costOf(i, pull);
  test_assert(c.runs == 0 && c.visits == 0);
  /* An edit outside doesn't run them. */
  test_assert(d.setLiteral(apart, "a", PropValue::ofFloat(1)).ok());
  c = costOf(i, pull);
  test_assert(c.runs == 1 && c.full == 0);
}

/* Interior sockets may come and go with an edit of another def. */
void testInterior()
{
  Scene s;
  const std::string k = "@" + std::to_string(s.inside[5].value) + ".b";
  /* By now it has found out that there are none. */
  test_assert(s.literal(*s.g.def, s.row[99], "b", 1).ok() && s.costs(11, 40, 1));
  connect(*s.g.def, s.read, "value", s.group, k);
  test_assert(s.costs(5, 500, 1, 1));
  test_assert(s.in.def->unlink(s.in.def->node(s.inside[3])->linkInto("a")->id).ok());
  test_assert(s.costs(7, 500, -2, 1));
  test_assert(s.literal(*s.in.def, s.inside[9], "b", 4).ok());
  test_assert(s.costs(1, 10, 4));
  test_assert(s.g.def->unlink(s.g.def->node(s.group)->linkInto(k)->id).ok());
  test_assert(s.costs(5, 500, -1, 1));
  /* And when it hasn't looked since the last was linked. */
  connect(*s.g.def, s.read, "value", s.group, k);
  test_assert(s.costs(5, 500, 1, 1));
  test_assert(s.g.def->unlink(s.g.def->node(s.group)->linkInto(k)->id).ok());
  test_assert(s.costs(5, 500, -1, 1));
}

/* An output node of a group comes, and is linked. */
void testOutputNode()
{
  Scene s;
  GraphDef &in = *s.in.def;
  const double value = s.inst->value(s.end())->asFloat();
  const NodeId old = in.node(s.inside[9])->linksOut()[0]->to->id();
  test_assert(in.removeNode(old).ok() && s.costs(0, 500, -value, 1));
  test_assert(s.literal(in, s.inside[9], "b", 0).ok() && s.pull().ok());
  const NodeId output = add(in, "graph.output");
  test_assert(s.costs(0, 500, 0, 1));
  connect(in, s.inside[9], "value", output, key(s.go));
  test_assert(s.costs(1, 500, value));
}

/* The journal keeps 4096 edits. */
void testJournal()
{
  for (int edits : {4095, 4096, 4097}) {
    Scene s;
    const Graph other = newGraph(s.w.lib, "other");
    const NodeId n = add(*other.def, "t.sum");
    test_assert(s.pull().ok());
    test_assert(s.literal(*s.g.def, s.row[99], "b", 1).ok());
    for (int i = 1; i < edits; i++) {
      test_assert(s.literal(*other.def, n, "a", i).ok());
    }
    test_assert(s.costs(11, 500, 1, edits > 4096 ? 1 : 0));
    /* Behind by all of it, without a concern. */
    for (int i = 0; i < edits; i++) {
      test_assert(s.literal(*other.def, n, "a", -i).ok());
    }
    test_assert(s.inst->isCurrent(s.end()) == (edits <= 4096));
    test_assert(s.pull().ok() && s.inst->isCurrent(s.end()));
  }
}

/* A vertex that nothing asked for in a while doesn't miss what happened. */
void testLeftBehind()
{
  Scene s;
  const SocketPath middle = out(s.row[50]);
  test_assert(s.inst->evaluate(middle).ok());
  const double before = s.inst->value(middle)->asFloat();
  test_assert(s.literal(*s.g.def, s.row[40], "b", 5).ok());
  test_assert(s.inst->evaluate(out(s.row[10])).ok());
  test_assert(s.inst->evaluate(out(s.row[10])).ok());
  test_assert(!s.inst->isCurrent(middle) && s.inst->value(middle)->asFloat() == before);
  test_assert(s.literal(*s.g.def, s.row[99], "b", 1).ok());
  test_assert(s.costs(70, 300, 6));
  test_assert(!s.inst->isCurrent(middle) && s.inst->evaluate(middle).ok());
  test_assert(s.inst->value(middle)->asFloat() == before + 5);
  /* Nor does one that lost what it reads, and has it back. */
  test_assert(s.g.def->unlink(s.g.def->node(s.row[60])->linkInto("a")->id).ok() &&
              s.pull().ok());
  test_assert(s.literal(*s.g.def, s.row[20], "b", 3).ok());
  Cost c = costOf(*s.inst, [&]() { test_assert(s.pull().ok()); });
  test_assert(c.runs == 0 && c.visits <= 1);
  connect(*s.g.def, s.row[59], "value", s.row[60], "a");
  test_assert(s.pull().ok() && s.inst->value(s.end())->asFloat() == 1 + 5 + 1 + 3);
}

/* ---- prepared sockets ---- */

void testHandles()
{
  Scene s;
  GraphDef &d = *s.g.def;
  GraphInstance i(&s.w.rt, &s.w.lib, s.g.id);
  const SocketHandle end = *i.prepare(s.end()), again = *i.prepare(s.end());
  const SocketHandle middle = *i.prepare(out(s.row[50]));
  const SocketHandle input =
      *i.prepare(SocketPath::of(s.row[99], "b", SocketSide::Input));
  const SocketHandle none = *i.prepare(SocketPath::of(NodeId{77777}, "value"));
  test_assert(!(end == again) && end.generation > 0);
  /* Before they are evaluated. */
  test_assert(hasCode(i.status(end), StatusCode::FailedPrecondition) && !i.value(end) &&
              !i.lastGood(end) && !i.error(end) && i.changedAt(end) == 0 &&
              !i.isCurrent(end));
  const std::vector<SocketHandle> all = {end, middle, input, none, again};
  litestl::util::Vector<int> moved;
  auto pull = [&](std::vector<int> want) {
    const Status st = i.evaluate(all, moved);
    const bool ok = st.ok() && std::vector<int>(moved.begin(), moved.end()) == want;
    if (!ok) {
      printf("%s:", st.toString().c_str());
      for (int m : moved) {
        printf(" %d", m);
      }
      printf("\n");
    }
    return ok;
  };
  test_assert(pull({0, 1, 2, 3, 4}));
  test_assert(i.status(end).ok() && i.value(end)->asFloat() == 1 && i.isCurrent(end));
  test_assert(i.value(input)->asFloat() == 0 && i.value(again)->asFloat() == 1);
  test_assert(hasCode(i.status(none), StatusCode::NotFound) && !i.value(none));
  test_assert(i.changedAt(end) > 0);
  test_assert(pull({}));
  /* They aren't results of paths, and those aren't theirs. */
  test_assert(!i.value(s.end()) && i.changedAt(s.end()) == 0);
  test_assert(i.evaluate(s.end()).ok() && i.forget(s.end()).ok() && i.value(end) &&
              pull({}));

  test_assert(s.literal(d, s.row[99], "b", 2).ok() && !i.isCurrent(end) &&
              i.isCurrent(none) == false);
  test_assert(pull({0, 2, 4}) && i.value(end)->asFloat() == 3);
  /* What moved while only some were asked for is listed when it is asked for. */
  test_assert(s.literal(d, s.row[10], "b", 2).ok());
  test_assert(i.evaluate({&middle, 1}, moved).ok() && moved.size() == 1 && moved[0] == 0);
  test_assert(pull({0, 4}));
  /* Each is told of what it saw itself: `again` is where it was. */
  test_assert(s.literal(d, s.row[99], "b", 7).ok() && i.evaluate({&end, 1}, moved).ok());
  test_assert(s.literal(d, s.row[99], "b", 2).ok());
  test_assert(pull({0}));

  /* An error is a move, and so is another; the same one isn't. */
  test_assert(s.literal(d, s.row[60], "a", 0).ok());
  test_assert(d.unlink(d.node(s.row[60])->linkInto("a")->id).ok());
  test_assert(s.literal(d, s.row[60], "a", -100).ok());
  test_assert(pull({0, 4}) && hasCode(i.status(end), StatusCode::OutOfRange) &&
              !i.value(end));
  test_assert(i.error(end)->origin.node == s.row[60] && i.lastGood(end)->asFloat() == 5);
  test_assert(s.literal(d, s.row[60], "b", 1).ok() && pull({}));
  test_assert(s.literal(d, s.row[60], "a", -101).ok() && pull({0, 4}));
  test_assert(s.literal(d, s.row[60], "a", 1).ok() && pull({0, 4}) && i.status(end).ok());

  /* One that goes, and comes back. */
  const NodeState state = d.stateOf(*d.node(s.row[50]));
  litestl::util::Vector<LinkState> links;
  links.append(GraphDef::stateOf(*d.node(s.row[50])->linkInto("a")));
  links.append(GraphDef::stateOf(*d.node(s.row[50])->linksOut()[0]));
  test_assert(d.removeNode(s.row[50]).ok());
  test_assert(pull({1})); /* row[60] doesn't read it any more */
  test_assert(hasCode(i.status(middle), StatusCode::NotFound) && !i.value(middle));
  test_assert(!i.lastGood(middle) && i.error(middle));
  test_assert(pull({}));
  test_assert(d.insertNode(state).ok() && pull({1}) && i.value(middle)->asFloat() == 0);
  test_assert(d.insertLink(links[0]).ok() && pull({1}) &&
              i.value(middle)->asFloat() == 3);

  /* Released ones, and those of another. */
  test_assert(i.release(again).ok() &&
              hasCode(i.release(again), StatusCode::InvalidArgument));
  test_assert(hasCode(i.status(again), StatusCode::FailedPrecondition) &&
              !i.value(again) && !i.error(again) && i.changedAt(again) == 0 &&
              !i.isCurrent(again));
  moved.append(5);
  test_assert(hasCode(i.evaluate(all, moved), StatusCode::InvalidArgument) &&
              moved.size() == 0);
  const SocketHandle next = *i.prepare(out(s.row[3]));
  test_assert(next.index == again.index && next.generation != again.generation);
  test_assert(hasCode(i.evaluate({&again, 1}, moved), StatusCode::InvalidArgument));
  test_assert(i.evaluate({&next, 1}, moved).ok() && moved.size() == 1);
  const SocketHandle null, far{1000, 1};
  test_assert(hasCode(i.evaluate({&null, 1}, moved), StatusCode::InvalidArgument));
  test_assert(hasCode(i.evaluate({&far, 1}, moved), StatusCode::InvalidArgument));

  /* clear drops what they have, not them. */
  test_assert(i.clear().ok() && !i.value(end) && i.changedAt(end) == 0);
  const std::vector<SocketHandle> two = {end, next};
  test_assert(i.evaluate(two, moved).ok() && moved.size() == 2 && i.status(end).ok());
  /* A call that fails says nothing of them. */
  test_assert(s.w.lib.markDeleted(s.g.h).ok());
  test_assert(hasCode(i.evaluate(two, moved), StatusCode::NotFound) && moved.size() == 0);
  test_assert(i.value(end) && !i.isCurrent(end));
  test_assert(s.w.lib.restoreFromHistory(s.g.h).ok());
  test_assert(i.evaluate(two, moved).ok() && moved.size() == 0 && i.isCurrent(end));
}

/* An interface output by its name follows the name. */
void testHandlesOfOutputs()
{
  World w;
  const Graph g = newGraph(w.lib, "g");
  GraphDef &d = *g.def;
  const int64_t x = addSocket(d, SocketSide::Output, "x", "float");
  const int64_t y = addSocket(d, SocketSide::Output, "y", "float");
  const NodeId output = add(d, "graph.output"), s = add(d, "t.sum");
  connect(d, s, "value", output, key(x));
  test_assert(d.setLiteral(s, "a", PropValue::ofFloat(4)).ok());
  GraphInstance i(&w.rt, &w.lib, g.id);
  const std::vector<SocketHandle> all = {*i.prepare(SocketPath::output("x")),
                                         *i.prepare(SocketPath::output(key(x)))};
  litestl::util::Vector<int> moved;
  test_assert(i.evaluate(all, moved).ok() && moved.size() == 2 &&
              i.value(all[0])->asFloat() == 4);
  test_assert(d.setInterfaceName(x, "z").ok());
  test_assert(i.evaluate(all, moved).ok() && moved.size() == 1 && moved[0] == 0);
  test_assert(hasCode(i.status(all[0]), StatusCode::NotFound) &&
              i.value(all[1])->asFloat() == 4);
  test_assert(d.setInterfaceName(y, "x").ok());
  test_assert(i.evaluate(all, moved).ok() && moved.size() == 1 &&
              i.value(all[0])->asFloat() == 0);
  /* An output node more: it is the first that counts. */
  test_assert(d.removeNode(output).ok() && i.evaluate(all, moved).ok() &&
              moved.size() == 1);
  test_assert(moved[0] == 1);
  const NodeId again = add(d, "graph.output");
  test_assert(i.evaluate(all, moved).ok() && i.value(all[1])->asFloat() == 0);
  connect(d, s, "value", again, key(x));
  test_assert(i.evaluate(all, moved).ok() && moved.size() == 1 &&
              i.value(all[1])->asFloat() == 4);
}

/* Groups inside groups, over the budget: what was listed as moved is listed again. */
void testHandlesOverBudget()
{
  Scene s;
  const NodeId second = add(*s.g.def, "graph.group");
  test_assert(s.g.def->setGroup(second, s.in.id).ok());
  GraphInstance i(&s.w.rt, &s.w.lib, s.g.id);
  const SocketPath far = SocketPath::of(second, key(s.go));
  const std::vector<SocketHandle> all = {*i.prepare(s.end()), *i.prepare(far)};
  litestl::util::Vector<int> moved;
  i.setMaxFrames(1);
  test_assert(hasCode(i.evaluate(all, moved), StatusCode::OutOfRange) &&
              moved.size() == 0);
  test_assert(i.evaluate({&all[0], 1}, moved).ok() && moved.size() == 1);
  /* Remembered, for a batch that has them all, in order. */
  Status failed;
  Cost c = costOf(i, [&]() { failed = i.evaluate(all, moved); });
  test_assert(hasCode(failed, StatusCode::OutOfRange));
  c = costOf(i, [&]() { failed = i.evaluate(all, moved); });
  test_assert(hasCode(failed, StatusCode::OutOfRange) && c.visits == 0 &&
              moved.size() == 0);
  test_assert(i.evaluate({&all[0], 1}, moved).ok() && moved.size() <= 1);
  i.setMaxFrames(2);
  test_assert(i.evaluate(all, moved).ok() && i.status(all[1]).ok() &&
              i.frameCount() == 2);

  /* A failure after an edit that it had said was no concern: that isn't so any more. */
  const Graph other = newGraph(s.w.lib, "other");
  const NodeId n = add(*other.def, "t.sum");
  const NodeId user = add(*s.g.def, "t.sum");
  connect(*s.g.def, second, key(s.go), user, "a");
  GraphInstance j(&s.w.rt, &s.w.lib, s.g.id);
  j.setMaxFrames(1);
  test_assert(j.evaluate(s.end()).ok());
  test_assert(s.literal(*other.def, n, "a", 1).ok() && j.isCurrent(s.end()));
  test_assert(hasCode(j.evaluate(out(user)), StatusCode::OutOfRange));
  test_assert(!j.isCurrent(s.end()));
  j.setMaxFrames(2);
  test_assert(j.evaluate(out(user)).ok() && j.value(out(user))->asFloat() == 0);
  test_assert(j.evaluate(s.end()).ok() && j.value(s.end())->asFloat() == 1);
}

/* ---- over the budget ---- */

/* The same call again doesn't try again, until something could be otherwise. */
void testRemembered()
{
  World w;
  std::vector<Graph> g;
  std::vector<int64_t> outs;
  std::vector<NodeId> groups;
  for (int k = 0; k < 14; k++) {
    g.push_back(newGraph(w.lib, "g"));
    outs.push_back(addSocket(*g.back().def, SocketSide::Output, "o", "float"));
    const NodeId s = add(*g.back().def, "t.sum");
    connect(
        *g.back().def, s, "value", add(*g.back().def, "graph.output"), key(outs.back()));
    for (int c = 0; k > 0 && c < 2; c++) {
      groups.push_back(add(*g.back().def, "graph.group"));
      test_assert(g.back().def->setGroup(groups.back(), g[size_t(k - 1)].id).ok());
      connect(*g.back().def, groups.back(), key(outs[size_t(k - 1)]), s, c ? "b" : "a");
    }
  }
  GraphInstance i(&w.rt, &w.lib, g[13].id);
  i.setMaxFrames(1000);
  const SocketPath p = SocketPath::output("o");
  litestl::util::Vector<Status> results;
  const std::vector<SocketPath> one = {p};
  Status st;
  auto cost = [&](std::span<const SocketPath> paths) {
    return costOf(i, [&]() { st = i.evaluate(paths, results); }).visits;
  };
  const uint64_t first = cost(one);
  test_assert(first > 500 && hasCode(st, StatusCode::OutOfRange));
  /* It names the group that was one too many. */
  const std::string message = st.message();
  const std::string prefix = "the graph needs more than 1000 groups, at node ";
  test_assert(message.rfind(prefix, 0) == 0);
  test_assert(message.find(std::to_string(groups[24].value) + "/") == prefix.size() ||
              message.find(std::to_string(groups[25].value) + "/") == prefix.size());
  test_assert(cost(one) == 0 && st.message() == message && results.size() == 0);
  const std::vector<SocketPath> more = {p, SocketPath::of(groups[25], key(outs[12]))};
  test_assert(cost(more) == 0 && hasCode(st, StatusCode::OutOfRange));
  /* Not for other sockets: these are fewer. */
  const std::vector<SocketPath> fewer = {SocketPath::of(groups[0], "nothing")};
  test_assert(i.evaluate(fewer, results).ok() && results.size() == 1);
  test_assert(cost(one) == 0 && hasCode(st, StatusCode::OutOfRange));
  /* Any edit may help, another budget does, and clear starts over. */
  w.rt.touch();
  test_assert(cost(one) > 500 && cost(one) == 0);
  test_assert(i.clear().ok() && cost(one) > 500 && cost(one) == 0);
  i.setMaxFrames(1001);
  test_assert(cost(one) > 500 && hasCode(st, StatusCode::OutOfRange));
  i.setMaxFrames(1 << 14);
  test_assert(cost(one) > 500 && st.ok() && i.frameCount() == (1 << 14) - 2);
}

/* ---- memory ---- */

void testMemory()
{
  Scene s;
  GraphMemory m = s.inst->memory();
  test_assert(m.frames == 2 && m.kernels == 111 && m.elements == 0 &&
              m.payloadBytes == 0);
  test_assert(m.inputs == 2 * 110 + 1 + 1);
  /* Around a kB each. */
  test_assert(m.bytes > 111 * 300 && m.bytes < 111 * 1500);
  const size_t before = m.bytes;
  for (NodeId n : s.row) {
    test_assert(s.inst->evaluate(out(n)).ok());
  }
  m = s.inst->memory();
  test_assert(m.bytes > before + 100 * 100 && m.kernels == 111);
  test_assert(s.inst->clear().ok());
  m = s.inst->memory();
  test_assert(m.frames == 0 && m.kernels == 0 && m.inputs == 0 && m.bytes < 1000);

  World w;
  Loop l(w);
  l.set(table({1, 2, 3}, {{"a", {1, 2, 3}}}));
  test_assert(l.evaluate().ok());
  m = l.inst.memory();
  test_assert(m.frames == 1 + 3 && m.elements == 3 && m.kernels == 1 + 3 * 2);
  test_assert(m.payloadBytes > 0);
  const size_t three = m.bytes;
  l.set(table({1, 2, 3, 4, 5, 6}, {{"a", {1, 2, 3, 4, 5, 6}}}));
  test_assert(l.evaluate().ok());
  m = l.inst.memory();
  test_assert(m.elements == 6 && m.bytes > three + 3 * 2 * 300);
  l.set(table({}, {}));
  test_assert(l.evaluate().ok());
  test_assert(l.inst.memory().elements == 0 && l.inst.memory().bytes < three);
}

/* Elements past the budget of kernels share one state. */
void testMaxCachedKernels()
{
  World w;
  Loop l(w);
  const Columns c = {{"a", {1, 2, 3, 4, 5}}};
  auto kept = [&]() {
    const NodeReport r = l.inst.report(at({}, l.node));
    test_assert(r.elements == 5 && r.cachedElements == l.inst.elementCount());
    return r.cachedElements;
  };
  l.inst.setMaxCachedKernels(6);
  l.set(table({1, 2, 3, 4, 5}, c));
  test_assert(
      l.is({1, 2, 3, 4, 5}, {{"sum", {1, 2, 3, 4, 5}}, {"c2", {}}}, 3 * 2 + 2 + 1) &&
      kept() == 3);
  /* More room, less room. */
  l.inst.setMaxCachedKernels(100);
  l.set(table({1, 2, 3, 4, 5}, {{"a", {1, 2, 3, 4, 6}}}));
  test_assert(l.is({1, 2, 3, 4, 5}, {{"sum", {1, 2, 3, 4, 6}}, {"c2", {}}}, 2) &&
              kept() == 4);
  l.set(table({1, 2, 3, 4, 5}, {{"a", {1, 2, 3, 7, 6}}}));
  test_assert(l.is({1, 2, 3, 4, 5}, {{"sum", {1, 2, 3, 7, 6}}, {"c2", {}}}, 2) &&
              kept() == 5);
  /* A failed run makes room for another, and what it made goes with the next that
   * doesn't. */
  l.set(table({1, 2, 3, 4, 5, 9}, {{"a", {1, 2, 3, 7, 6, -100}}}));
  test_assert(!l.evaluate().ok() && l.inst.elementCount() == 6);
  l.set(table({1, 2, 3, 4, 5}, {{"a", {1, 2, 3, 7, 5}}}));
  test_assert(l.is({1, 2, 3, 4, 5}, {{"sum", {1, 2, 3, 7, 5}}, {"c2", {}}}, 1) &&
              kept() == 5);
  l.set(table({1, 2, 3, 4, 5}, {{"a", {1, 2, 3, 7, 6}}}));
  test_assert(l.is({1, 2, 3, 4, 5}, {{"sum", {1, 2, 3, 7, 6}}, {"c2", {}}}, 1) &&
              kept() == 5);
  l.inst.setMaxCachedKernels(0);
  l.set(table({1, 6, 3, 4, 5}, {{"a", {1, 2, 3, 7, 6}}}));
  test_assert(l.is({1, 6, 3, 4, 5}, {{"sum", {1, 2, 3, 7, 6}}, {"c2", {}}}, 1) &&
              kept() == 4);
  /* What has a state keeps it. */
  l.set(table({1, 6, 3, 4, 5}, {{"a", {1, 2, 3, 8, 6}}}));
  test_assert(l.is({1, 6, 3, 4, 5}, {{"sum", {1, 2, 3, 8, 6}}, {"c2", {}}}, 1) &&
              kept() == 4);

  /* States that go make room. */
  World w2;
  Loop m(w2);
  m.inst.setMaxCachedKernels(6);
  m.set(table({1, 2, 3}, {{"a", {1, 2, 3}}}));
  test_assert(m.evaluate().ok() && m.inst.elementCount() == 3);
  m.set(table({}, {{"a", {}}}));
  test_assert(m.evaluate().ok() && m.inst.elementCount() == 0);
  m.set(table({4, 5, 6}, {{"a", {1, 2, 3}}}));
  test_assert(m.evaluate().ok() && m.inst.elementCount() == 3);
  m.set(table({7, 8, 9, 10}, {{"a", {1, 2, 3, -100}}}));
  test_assert(!m.evaluate().ok() && m.inst.elementCount() == 0);
}

} // namespace

int main()
{
  testElsewhere();
  testDirtyCone();
  testOwnInputs();
  testForeachs();
  testInterior();
  testOutputNode();
  testJournal();
  testLeftBehind();
  testHandles();
  testHandlesOfOutputs();
  testHandlesOverBudget();
  testRemembered();
  testMemory();
  testMaxCachedKernels();
  return test_end();
}
