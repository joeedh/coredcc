/* GraphInstance: cycles, recursion, depth, size, and calls made while a graph evaluates.
 */
#include "graph_fixture.h"

#include "coredcc/graph/instance.h"

#include <chrono>
#include <functional>
#include <vector>

using namespace fixture;

test_init;

namespace {

std::function<void()> g_inKernel;

Status sum(NodeExecContext &ctx)
{
  if (g_inKernel) {
    g_inKernel();
  }
  return ctx.setOutput(
      0, GraphValue::ofFloat(ctx.input(0).asFloat() + ctx.input(1).asFloat()));
}

Status two(NodeExecContext &ctx)
{
  CDCC_TRY(ctx.setOutput(0, GraphValue::ofFloat(1)));
  return ctx.setOutput(1, GraphValue::ofFloat(2));
}

struct World {
  Runtime rt;
  Library lib;
  World() : lib(&rt)
  {
    addThings(rt);
    test_assert(registerGraphBlocks(rt).ok());
    NodeTypeDef s("t.sum");
    s.input(PropertyDef::floatProp("a"))
        .input(PropertyDef::floatProp("b"))
        .output("value", "float");
    addType<sum>(rt, s, "t.sum");
    NodeTypeDef t("t.two");
    t.input(PropertyDef::floatProp("a")).output("x", "float").output("y", "float");
    addType<two>(rt, t, "t.two");
  }
};

SocketPath out(NodeId n, const char *socket = "value")
{
  return SocketPath::of(n, socket);
}
SocketPath out(NodeId n, int64_t socket)
{
  return SocketPath::of(n, key(socket));
}
NodePath at(NodeId n, std::initializer_list<NodeId> groups = {})
{
  NodePath p;
  p.node = n;
  for (NodeId g : groups) {
    p.groups.append(g);
  }
  return p;
}

/* A link with no checks, as a file or an undo makes it. */
void force(
    GraphDef &def, NodeId from, std::string_view out, NodeId to, std::string_view in)
{
  static int64_t id = 1000;
  LinkState s;
  s.id = LinkId{id++};
  s.from = from;
  s.fromKey = std::string(out).c_str();
  s.to = to;
  s.toKey = std::string(in).c_str();
  test_assert(def.insertLink(s).ok());
}

NodeId insert(GraphDef &def, const char *type, int64_t id)
{
  NodeState s;
  s.id = NodeId{id};
  s.typeName = type;
  test_assert(def.insertNode(s).ok());
  return s.id;
}

/* The error is the same whichever socket is pulled first, in a new instance. */
void everyEntry(World &w,
                const Graph &g,
                const std::vector<SocketPath> &sockets,
                const GraphError &want)
{
  for (size_t i = 0; i < sockets.size(); i++) {
    GraphInstance inst(&w.rt, &w.lib, g.id);
    for (size_t j = 0; j < sockets.size(); j++) {
      const SocketPath &p = sockets[(i + j) % sockets.size()];
      test_assert(!inst.evaluate(p).ok() && !inst.value(p));
      const bool same = *inst.error(p) == want;
      if (!same) {
        printf("%s at %s.%s\n",
               inst.error(p)->status.toString().c_str(),
               inst.error(p)->origin.toString().c_str(),
               inst.error(p)->socket.c_str());
      }
      test_assert(same);
    }
  }
}

void testCycles()
{
  World w;
  /* Kernels: 1 -> 2 -> 3 -> 1, with 4 feeding in and 5 hanging off. */
  const Graph g = newGraph(w.lib, "g");
  GraphDef &d = *g.def;
  const NodeId n3 = insert(d, "t.sum", 3), n1 = insert(d, "t.sum", 1),
               n2 = insert(d, "t.sum", 2);
  const NodeId n4 = insert(d, "t.sum", 4), n5 = insert(d, "t.sum", 5);
  connect(d, n1, "value", n2, "a");
  connect(d, n2, "value", n3, "b");
  force(d, n3, "value", n1, "b");
  connect(d, n4, "value", n1, "a");
  connect(d, n3, "value", n5, "a");
  GraphInstance i(&w.rt, &w.lib, g.id);
  test_assert(hasCode(i.evaluate(out(n5)), StatusCode::FailedPrecondition));
  const GraphError want = *i.error(out(n5));
  test_assert(want.origin == at(n1) && strcmp(want.socket.c_str(), "b") == 0);
  test_assert(strcmp(want.status.message(), "a cycle through 1.b, 2.a, 3.b") == 0);
  test_assert(i.report(at(n2)).state == NodeReport::State::Failed &&
              i.report(at(n2)).runs == 0);
  test_assert(i.report(at(n5)).state == NodeReport::State::InputFailed);
  test_assert(i.evaluate(out(n4)).ok());
  everyEntry(w,
             g,
             {out(n1),
              out(n2),
              out(n3),
              out(n5),
              SocketPath::of(n1, "b", SocketSide::Input),
              SocketPath::of(n3, "b", SocketSide::Input)},
             want);
  /* The input beside the cycle is fine. */
  test_assert(i.evaluate(SocketPath::of(n1, "a", SocketSide::Input)).ok());
  /* Broken open, it evaluates; nothing is left of the cycle. */
  test_assert(d.unlink(d.node(n1)->linkInto("b")->id).ok());
  test_assert(i.evaluate(out(n5)).ok() &&
              i.report(at(n2)).state == NodeReport::State::Ok);

  /* Two loops that share a node are one component, and 8 members are named. */
  force(d, n3, "value", n1, "b");
  force(d, n2, "value", n4, "a");
  test_assert(!i.evaluate(out(n4)).ok());
  test_assert(strcmp(i.error(out(n4))->status.message(),
                     "a cycle through 1.a, 1.b, 2.a, 3.b, 4.a") == 0);
  NodeId last = n4;
  for (int k = 10; k < 20; k++) {
    const NodeId n = insert(d, "t.sum", k);
    force(d, last, "value", n, "a");
    last = n;
  }
  test_assert(d.unlink(d.node(n1)->linkInto("a")->id).ok());
  force(d, last, "value", n1, "a");
  test_assert(!i.evaluate(out(n1)).ok());
  test_assert(
      strcmp(i.error(out(n1))->status.message(),
             "a cycle through 1.a, 1.b, 2.a, 3.b, 4.a, 10.a, 11.a, 12.a and 7 more") ==
      0);
}

/* Two outputs of one node have one version; which one is read still counts. */
void testSameVersion()
{
  World w;
  GraphDef d(&w.rt);
  const NodeId both = add(d, "t.two"), user = add(d, "t.sum");
  connect(d, both, "x", user, "a");
  GraphInstance i(&w.rt, nullptr, &d);
  test_assert(i.evaluate(out(user)).ok() &&
              *i.value(out(user)) == GraphValue::ofFloat(1));
  connect(d, both, "y", user, "a");
  test_assert(i.evaluate(out(user)).ok() &&
              *i.value(out(user)) == GraphValue::ofFloat(2));
  test_assert(i.totalRuns() == 3);
}

/* Cycles made by editing a def that another uses. */
void testInnerCycles()
{
  World w;
  const Graph inner = newGraph(w.lib, "inner"), outer = newGraph(w.lib, "outer");
  GraphDef &in = *inner.def, &o = *outer.def;
  int64_t i[2], u[2];
  for (int k = 0; k < 2; k++) {
    i[k] = *in.addInterface(SocketSide::Input, k ? "i1" : "i0", "float", -1, 100 + k);
    u[k] = *in.addInterface(SocketSide::Output, k ? "o1" : "o0", "float", -1, 200 + k);
  }
  const NodeId input = insert(in, "graph.input", 1),
               output = insert(in, "graph.output", 2);
  connect(in, input, key(i[0]), output, key(u[0]));
  const NodeId g = insert(o, "graph.group", 5), s = insert(o, "t.sum", 6);
  test_assert(o.setGroup(g, inner.id).ok());
  connect(o, g, key(u[0]), g, key(i[1])); /* o0 depends on i0 only */
  connect(o, g, key(u[1]), s, "a");
  connect(o, s, "value", g, key(i[0])); /* o1 depends on nothing */
  GraphInstance inst(&w.rt, &w.lib, outer.id);
  test_assert(inst.evaluate(out(s)).ok());

  /* o1 <- i1 makes the outer one a cycle, through a kernel: s -> i0 -> o0 -> i1 -> o1 ->
   * s. */
  connect(in, input, key(i[1]), output, key(u[1]));
  test_assert(hasCode(inst.evaluate(out(s)), StatusCode::FailedPrecondition));
  GraphError want = *inst.error(out(s));
  test_assert(want.origin == at(g) && strcmp(want.socket.c_str(), "#100") == 0);
  test_assert(strcmp(want.status.message(),
                     "a cycle through 5.#100, 5.#101, 6.a, 5/2.#200, 5/2.#201") == 0);
  std::vector<SocketPath> all = {
      out(s),
      out(g, u[0]),
      out(g, u[1]),
      SocketPath::of(s, "a", SocketSide::Input),
      SocketPath::of(g, key(i[0]), SocketSide::Input),
      SocketPath::of(g, key(i[1]), SocketSide::Input),
      SocketPath::of(at(input, {g}), key(i[0])),
      SocketPath::of(at(input, {g}), key(i[1])),
      SocketPath::of(at(output, {g}), key(u[0]), SocketSide::Input)};
  everyEntry(w, outer, all, want);

  /* Without any kernel in it: o1 -> i0 directly. */
  test_assert(o.unlink(o.node(g)->linkInto(key(i[0]))->id).ok());
  force(o, g, key(u[1]), g, key(i[0]));
  test_assert(!inst.evaluate(out(s)).ok());
  want = *inst.error(out(s));
  test_assert(strcmp(want.status.message(),
                     "a cycle through 5.#100, 5.#101, 5/2.#200, 5/2.#201") == 0);
  everyEntry(w, outer, all, want);
  test_assert(inst.report(at(s)).state == NodeReport::State::InputFailed);
  test_assert(o.unlink(o.node(g)->linkInto(key(i[0]))->id).ok() &&
              inst.evaluate(out(s)).ok());

  /* A def whose output node feeds itself through an inner group. */
  const Graph top = newGraph(w.lib, "top");
  const NodeId tg = insert(*top.def, "graph.group", 7);
  test_assert(top.def->setGroup(tg, inner.id).ok());
  force(*top.def, tg, key(u[0]), tg, key(i[0]));
  GraphInstance t(&w.rt, &w.lib, top.id);
  test_assert(!t.evaluate(out(tg, u[0])).ok());
  everyEntry(w,
             top,
             {out(tg, u[0]), SocketPath::of(tg, key(i[0]), SocketSide::Input)},
             *t.error(out(tg, u[0])));
  test_assert(t.evaluate(out(tg, u[1])).ok());
}

struct Redirect {
  Library *lib;
  BlockId to;
};

/* Points every group of `def` at `to`, with no checks. */
void redirect(Library &lib, GraphDef &def, BlockId to)
{
  Redirect r{&lib, to};
  RefVisitor v(
      [](void *p, RefCell &cell) {
        const Redirect &d = *static_cast<Redirect *>(p);
        test_assert(d.lib->writeRef(cell, d.to).ok());
      },
      &r);
  def.foreachRef(v);
}

void testRecursion()
{
  World w;
  const Graph a = newGraph(w.lib, "a"), b = newGraph(w.lib, "b");
  const int64_t ao = addSocket(*a.def, SocketSide::Output, "o", "float");
  const int64_t bo = addSocket(*b.def, SocketSide::Output, "o", "float");
  const NodeId aOut = add(*a.def, "graph.output"), bOut = add(*b.def, "graph.output");
  const NodeId ag = add(*a.def, "graph.group"), bg = add(*b.def, "graph.group");
  test_assert(a.def->setGroup(ag, b.id).ok());
  connect(*a.def, ag, key(bo), aOut, key(ao));
  GraphInstance i(&w.rt, &w.lib, a.id);
  test_assert(i.evaluate(SocketPath::output("o")).ok());
  redirect(w.lib, *b.def, a.id);
  force(*b.def, bg, key(ao), bOut, key(bo));
  test_assert(
      hasCode(i.evaluate(SocketPath::output("o")), StatusCode::FailedPrecondition));
  test_assert(i.error(SocketPath::output("o"))->origin == at(bg, {ag}));
  test_assert(strcmp(i.error(SocketPath::output("o"))->status.message(),
                     "the graph contains itself") == 0);
  test_assert(i.frameCount() == 1);
  /* Directly. */
  redirect(w.lib, *b.def, b.id);
  test_assert(b.def->unlink(b.def->node(bOut)->linkInto(key(bo))->id).ok());
  force(*b.def, bg, key(bo), bOut, key(bo));
  GraphInstance j(&w.rt, &w.lib, b.id);
  test_assert(
      hasCode(j.evaluate(SocketPath::output("o")), StatusCode::FailedPrecondition));
  test_assert(j.error(SocketPath::output("o"))->origin == at(bg) && j.frameCount() == 0);
  redirect(w.lib, *b.def, BlockId());
  test_assert(hasCode(j.evaluate(SocketPath::output("o")), StatusCode::NotFound));
}

/* A chain of n groups, each holding `width` of the next; the last is empty. */
std::vector<Graph> nest(World &w, int n, int width, std::vector<int64_t> &outs)
{
  std::vector<Graph> g;
  for (int i = 0; i <= n; i++) {
    g.push_back(newGraph(w.lib, "n"));
    outs.push_back(addSocket(*g[i].def, SocketSide::Output, "o", "float"));
  }
  for (int i = n; i >= 0; i--) {
    GraphDef &d = *g[i].def;
    const NodeId o = add(d, "graph.output"), s = add(d, "t.sum");
    connect(d, s, "value", o, key(outs[i]));
    if (i == n) {
      test_assert(d.setLiteral(s, "a", PropValue::ofFloat(1)).ok());
    }
    for (int k = 0; i < n && k < width; k++) {
      const NodeId q = add(d, "graph.group");
      test_assert(d.setGroup(q, g[i + 1].id).ok());
      connect(d, q, key(outs[i + 1]), s, k ? "b" : "a");
    }
  }
  return g;
}

void testDepth()
{
  for (int depth : {63, 64, 65}) {
    World w;
    std::vector<int64_t> outs;
    const std::vector<Graph> g = nest(w, depth, 1, outs);
    GraphInstance i(&w.rt, &w.lib, g[0].id);
    const SocketPath p = SocketPath::output("o");
    const Status s = i.evaluate(p);
    test_assert(depth <= 64 ? s.ok() : hasCode(s, StatusCode::OutOfRange));
    test_assert(i.frameCount() == (depth <= 64 ? depth : 64));
    if (depth <= 64) {
      test_assert(*i.value(p) == GraphValue::ofFloat(1));
    } else {
      test_assert(i.error(p)->origin.groups.size() == 64);
      GraphInstance inner(&w.rt, &w.lib, g[1].id);
      test_assert(inner.evaluate(p).ok());
    }
  }
}

void testBudget()
{
  World w;
  std::vector<int64_t> outs;
  const std::vector<Graph> g = nest(w, 40, 2, outs);
  const SocketPath p = SocketPath::output("o");
  GraphInstance i(&w.rt, &w.lib, g[0].id);
  /* 2^40 groups if the budget didn't stop it. Half a second here, more than a second on CI's
   * machines: the limits are for hours, not for a slow machine. */
#ifdef NDEBUG
  const double limit = 5;
#else
  i.setMaxFrames(20000);
  const double limit = 10;
#endif
  const auto start = std::chrono::steady_clock::now();
  litestl::util::Vector<Status> results;
  test_assert(hasCode(i.evaluate({&p, 1}, results), StatusCode::OutOfRange));
  const std::chrono::duration<double> t = std::chrono::steady_clock::now() - start;
  if (t.count() >= limit) {
    printf("the doubling graph took %g s\n", t.count());
  }
  test_assert(t.count() < limit && results.size() == 0);
  test_assert(i.frameCount() == 0 && !i.value(p) && !w.rt.evaluating());
  test_assert(hasCode(i.evaluate({&p, 1}, results), StatusCode::OutOfRange)); /* again */

  /* Deep enough to fit, with and without one to spare. */
  GraphInstance j(&w.rt, &w.lib, g[34].id); /* 6 levels: 2 + 4 + ... + 64 = 126 */
  j.setMaxFrames(125);
  test_assert(hasCode(j.evaluate(p), StatusCode::OutOfRange) && j.frameCount() == 0);
  j.setMaxFrames(126);
  test_assert(j.evaluate(p).ok() && j.frameCount() == 126);
  test_assert(*j.value(p) == GraphValue::ofFloat(64) && j.totalRuns() >= 127);

  /* What fitted before a failed call is kept. */
  GraphInstance k(&w.rt, &w.lib, g[36].id); /* 4 levels: 30 */
  k.setMaxFrames(40);
  test_assert(k.evaluate(p).ok() && k.frameCount() == 30);
  const uint64_t runs = k.totalRuns();
  const NodeId extra = add(*g[36].def, "graph.group");
  test_assert(g[36].def->setGroup(extra, g[37].id).ok()); /* 15 more */
  const SocketPath e = SocketPath::of(extra, key(outs[37]));
  test_assert(hasCode(k.evaluate(e), StatusCode::OutOfRange) && k.frameCount() == 30);
  test_assert(k.evaluate(p).ok());
  test_assert(g[36].def->removeNode(extra).ok() && k.evaluate(p).ok());
  test_assert(k.totalRuns() >= runs && k.totalRuns() - runs <= 15);
  k.setMaxFrames(45);
  const uint64_t after = k.totalRuns();
  test_assert(k.evaluate(p).ok() && k.totalRuns() == after);
}

/* 100,000 nodes in a row, and in a circle: no native recursion. */
void testLong()
{
  World w;
  const Graph g = newGraph(w.lib, "g");
  GraphDef &d = *g.def;
  const int n = 100000;
  const NodeId first = add(d, "t.sum");
  NodeId last = first;
  test_assert(d.setLiteral(first, "a", PropValue::ofFloat(1)).ok());
  for (int i = 1; i < n; i++) {
    const NodeId next = add(d, "t.sum");
    connect(d, last, "value", next, "a");
    last = next;
  }
  GraphInstance i(&w.rt, &w.lib, g.id);
  test_assert(i.evaluate(out(last)).ok() &&
              *i.value(out(last)) == GraphValue::ofFloat(1));
  test_assert(i.totalRuns() == uint64_t(n));
  test_assert(d.setLiteral(first, "a", PropValue::ofFloat(2)).ok());
  test_assert(i.evaluate(out(last)).ok() &&
              *i.value(out(last)) == GraphValue::ofFloat(2));
  w.rt.touch();
  test_assert(i.evaluate(out(last)).ok() && i.totalRuns() == 2 * uint64_t(n));
  /* Calls under one key share their pass: this would be 10^9 steps otherwise. */
  const auto start = std::chrono::steady_clock::now();
  for (int k = 0; k < 10000; k++) {
    test_assert(i.evaluate(out(last)).ok());
  }
  const std::chrono::duration<double> t = std::chrono::steady_clock::now() - start;
  test_assert(t.count() < 5);
  test_assert(
      hasCode(d.link({last, "value"}, {first, "b"}), StatusCode::InvalidArgument));
  force(d, last, "value", first, "a");
  test_assert(hasCode(i.evaluate(out(last)), StatusCode::FailedPrecondition));
  GraphInstance j(&w.rt, &w.lib, g.id);
  test_assert(!j.evaluate(out(first)).ok() &&
              *i.error(out(last)) == *j.error(out(first)));
  test_assert(i.clear().ok());
}

void testReentrancy()
{
  World w;
  const Graph g = newGraph(w.lib, "g");
  w.lib.setFlag(g.h, BlockFlags::Root, true);
  GraphDef &d = *g.def;
  const int64_t in = addSocket(d, SocketSide::Input, "in", "float");
  (void)in;
  const NodeId s = add(d, "t.sum"), s2 = add(d, "t.sum");
  connect(d, s, "value", s2, "a");
  GraphInstance i(&w.rt, &w.lib, g.id), other(&w.rt, &w.lib, g.id);
  int calls = 0;
  g_inKernel = [&]() {
    calls++;
    test_assert(w.rt.evaluating());
    const uint64_t edit = w.rt.lastEdit();
    for (GraphInstance *x : {&i, &other}) {
      litestl::util::Vector<Status> results;
      const SocketPath p = out(s);
      test_assert(hasCode(x->evaluate({&p, 1}, results), StatusCode::Busy));
      test_assert(hasCode(x->setInput("in", GraphValue::ofFloat(1)), StatusCode::Busy));
      test_assert(hasCode(x->forget(p), StatusCode::Busy));
      test_assert(hasCode(x->retry(), StatusCode::Busy) &&
                  hasCode(x->clear(), StatusCode::Busy));
    }
    test_assert(hasCode(w.lib.collect(), StatusCode::Busy));
    test_assert(hasCode(d.addNode("t.sum"), StatusCode::Busy));
    test_assert(
        hasCode(w.rt.ext<SocketTypeRegistry>().addConversion("int", "vec2", nullptr),
                StatusCode::Busy));
    test_assert(
        hasCode(w.rt.ext<NodeTypeRegistry>().add(NodeTypeDef("t.x")), StatusCode::Busy));
    test_assert(w.rt.lastEdit() == edit);
    /* Reading is fine. */
    test_assert(!i.isCurrent(out(s2)) && i.report(at(s2)).runs == 0);
  };
  test_assert(i.evaluate(out(s2)).ok() && calls == 2);
  g_inKernel = nullptr;
  test_assert(!w.rt.evaluating() && w.lib.collect().ok());
  test_assert(i.setInput("in", GraphValue::ofFloat(1)).ok());

  /* A notification from a kernel moves the next call's key, not this one's. */
  const BlockId self = g.id;
  g_inKernel = [&]() { w.rt.touch(); };
  test_assert(d.setLiteral(s, "a", PropValue::ofFloat(1)).ok());
  (void)self;
  test_assert(i.evaluate(out(s2)).ok() && !i.isCurrent(out(s2)));
  g_inKernel = nullptr;
  const uint64_t runs = i.totalRuns();
  test_assert(i.evaluate(out(s2)).ok() && i.isCurrent(out(s2)) && i.totalRuns() == runs);
}

} // namespace

int main()
{
  testCycles();
  testSameVersion();
  testInnerCycles();
  testRecursion();
  testDepth();
  testBudget();
  testLong();
  testReentrancy();
  return test_end();
}
