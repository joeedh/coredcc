/* What an instance keeps: results of sockets that are gone, inputs of removed interface sockets,
 * and instances made with the wrong Library. And kernels that use a block's name. */
#include "graph_fixture.h"

#include "coredcc/graph/instance.h"

using namespace fixture;

test_init;

namespace {

int g_alive = 0;
struct Counted {
  Counted()
  {
    g_alive++;
  }
  Counted(const Counted &)
  {
    g_alive++;
  }
  Counted &operator=(const Counted &) = default;
  ~Counted()
  {
    g_alive--;
  }
};

} // namespace

template <> struct coredcc::PayloadTraits<Counted> {
  static constexpr const char *name = "test.Counted";
};

namespace {

Status makeKernel(NodeExecContext &ctx)
{
  return ctx.setOutput(0, GraphValue::ofPayload(makePayload(Counted())));
}
Status nameKernel(NodeExecContext &ctx)
{
  const BlockUse use = ctx.paramBlock("thing");
  const Datablock *b = ctx.library() ? ctx.library()->get(use.handle) : nullptr;
  return ctx.setOutput(0, GraphValue::ofString(b ? b->name().c_str() : ""));
}

struct World {
  Runtime rt;
  Library lib;

  World() : lib(&rt)
  {
    addThings(rt);
    test_assert(registerGraphBlocks(rt).ok());
    test_assert(rt.ext<SocketTypeRegistry>().addPayloadType<Counted>().ok());
    NodeTypeDef m("t.make");
    m.output("value", "test.Counted").input(PropertyDef::floatProp("a"));
    addType<makeKernel>(rt, m, "t.make");
    NodeTypeDef n("t.name");
    n.param(PropertyDef::pointerProp("thing", kThingStruct)).output("value", "string");
    addType<nameKernel>(rt, n, "t.name");
  }
  ~World()
  {
    test_assert(rt.validateAll().ok() && lib.validate().ok() && !rt.hasStickyError());
  }
};

SocketPath out(NodeId n, const char *socket = "value")
{
  return SocketPath::of(n, socket);
}

bool known(const GraphInstance &inst, const SocketPath &p)
{
  return inst.value(p) || inst.lastGood(p) || inst.error(p) || inst.changedAt(p) != 0;
}

/* ---- sockets that are gone ---- */

void testRemovedNodes()
{
  World w;
  const Graph g = newGraph(w.lib, "g");
  GraphInstance inst(&w.rt, &w.lib, g.id);
  const NodeId stays = add(*g.def, "t.make");
  test_assert(inst.evaluate(out(stays)).ok() && g_alive == 1);
  /* Asked for again: NotFound, and nothing is left. */
  NodeId n = add(*g.def, "t.make");
  test_assert(inst.evaluate(out(n)).ok() && g_alive == 2);
  test_assert(g.def->removeNode(n).ok() && known(inst, out(n))); /* the last evaluation's */
  test_assert(hasCode(inst.evaluate(out(n)), StatusCode::NotFound) && !known(inst, out(n)));
  test_assert(g_alive == 1 && !inst.isCurrent(out(n)));
  /* Not asked for: dropped by the next evaluation. */
  for (int i = 0; i < 100; i++) {
    n = add(*g.def, "t.make");
    test_assert(inst.evaluate(out(n)).ok());
    test_assert(inst.evaluate(SocketPath::of(n, "a", SocketSide::Input)).ok());
    test_assert(g.def->removeNode(n).ok() && inst.evaluate(out(stays)).ok());
    test_assert(g_alive == 1 && !known(inst, out(n)));
    test_assert(!known(inst, SocketPath::of(n, "a", SocketSide::Input)));
  }
  test_assert(known(inst, out(stays)));
  /* Back before anyone looked: nothing was lost. */
  n = add(*g.def, "t.make");
  test_assert(inst.evaluate(out(n)).ok());
  const uint64_t at = inst.changedAt(out(n));
  const NodeState s = g.def->stateOf(*g.def->node(n));
  test_assert(g.def->removeNode(n).ok() && g.def->insertNode(s).ok());
  test_assert(inst.evaluate(out(stays)).ok() && inst.changedAt(out(n)) == at);
  test_assert(inst.lastGood(out(n)) && inst.evaluate(out(n)).ok());
  /* A socket that the node doesn't have now, and an output of the interface. */
  const int64_t a = addSocket(*g.def, SocketSide::Output, "a", "float");
  const int64_t b = addSocket(*g.def, SocketSide::Output, "b", "float");
  const NodeId o = add(*g.def, "graph.output");
  const SocketPath in = SocketPath::of(o, key(a), SocketSide::Input);
  const SocketPath byName = SocketPath::output("a"), byKey = SocketPath::output(key(a));
  const SocketPath other = SocketPath::output("b");
  for (const SocketPath &p : {in, byName, byKey, other}) {
    test_assert(inst.evaluate(p).ok() && known(inst, p));
  }
  test_assert(g.def->setInterfaceName(a, "renamed").ok() && inst.evaluate(other).ok());
  test_assert(!known(inst, byName) && known(inst, byKey) && known(inst, in));
  test_assert(g.def->removeInterface(a).ok() && inst.evaluate(other).ok());
  test_assert(!known(inst, byKey) && !known(inst, in) && known(inst, other));
  (void)b;
}

/* In groups, seen through the defs: their frames may not be visited. */
void testRemovedInGroups()
{
  World w;
  const Graph inner = newGraph(w.lib, "inner"), outer = newGraph(w.lib, "outer");
  const Graph other = newGraph(w.lib, "other");
  const NodeId group = add(*outer.def, "graph.group"), top = add(*outer.def, "t.make");
  const NodeId n = add(*inner.def, "t.make"), m = add(*inner.def, "t.make");
  test_assert(outer.def->setGroup(group, inner.id).ok());
  auto at = [&](NodeId node) {
    NodePath p;
    p.groups.append(group);
    p.node = node;
    return SocketPath::of(p, "value");
  };
  GraphInstance inst(&w.rt, &w.lib, outer.id);
  test_assert(inst.evaluate(at(n)).ok() && inst.evaluate(at(m)).ok());
  test_assert(inner.def->removeNode(n).ok() && inst.evaluate(out(top)).ok());
  test_assert(!known(inst, at(n)) && known(inst, at(m)));
  /* A lost group may come back, and an error is an answer. */
  test_assert(w.lib.markDeleted(inner.h).ok() && inst.evaluate(out(top)).ok());
  test_assert(inst.lastGood(at(m)) && !inst.evaluate(at(m)).ok() && inst.error(at(m)));
  test_assert(inst.evaluate(out(top)).ok() && inst.error(at(m)) && inst.lastGood(at(m)));
  test_assert(w.lib.restoreFromHistory(inner.h).ok() && inst.evaluate(at(m)).ok());
  /* Another group doesn't have it. Also when the cell is written by the Library. */
  test_assert(outer.def->setGroup(group, other.id).ok() && inst.evaluate(out(top)).ok());
  test_assert(!known(inst, at(m)));
  test_assert(outer.def->setGroup(group, inner.id).ok() && inst.evaluate(at(m)).ok());
  RefCell &cell = const_cast<RefCell &>(outer.def->node(group)->group());
  test_assert(w.lib.writeRef(cell, other.id).ok() && inst.evaluate(out(top)).ok());
  test_assert(!known(inst, at(m)));
  /* A graph that contains itself. */
  test_assert(w.lib.writeRef(cell, outer.id).ok());
  test_assert(hasCode(inst.evaluate(at(m)), StatusCode::FailedPrecondition));
  test_assert(inst.evaluate(out(top)).ok() && inst.error(at(m)));
  /* No group there any more. */
  test_assert(w.lib.writeRef(cell, inner.id).ok() && inst.evaluate(at(m)).ok());
  test_assert(outer.def->removeNode(group).ok() && inst.evaluate(out(top)).ok());
  test_assert(!known(inst, at(m)));
}

/* A socket that is there, but only while something is linked to it. */
void testInterior()
{
  World w;
  const Graph inner = newGraph(w.lib, "inner"), outer = newGraph(w.lib, "outer");
  const NodeId n = add(*inner.def, "t.make"), group = add(*outer.def, "graph.group");
  test_assert(outer.def->setGroup(group, inner.id).ok());
  const std::string interior = "@" + std::to_string(n.value) + ".a";
  const NodeId f = add(*outer.def, "math.float");
  const LinkId l = connect(*outer.def, f, "value", group, interior);
  GraphInstance inst(&w.rt, &w.lib, outer.id);
  const SocketPath p = SocketPath::of(group, interior, SocketSide::Input);
  test_assert(inst.evaluate(p).ok() && known(inst, p));
  test_assert(outer.def->unlink(l).ok());
  test_assert(hasCode(inst.evaluate(p), StatusCode::NotFound) && !known(inst, p));
}

void testForget()
{
  World w;
  const Graph g = newGraph(w.lib, "g");
  GraphInstance inst(&w.rt, &w.lib, g.id);
  const NodeId n = add(*g.def, "t.make"), m = add(*g.def, "t.make");
  test_assert(inst.evaluate(out(n)).ok() && inst.evaluate(out(m)).ok());
  test_assert(inst.forget(out(n)).ok() && !known(inst, out(n)) && known(inst, out(m)));
  test_assert(inst.forget(out(n)).ok() && inst.forget(SocketPath::output("none")).ok());
  test_assert(inst.evaluate(out(n)).ok() && inst.totalRuns() == 2 && known(inst, out(n)));
}

/* ---- inputs ---- */

void testInputs()
{
  World w;
  const Graph g = newGraph(w.lib, "g");
  const int64_t a = addSocket(*g.def, SocketSide::Input, "a", "test.Counted");
  const int64_t b = addSocket(*g.def, SocketSide::Input, "b", "float");
  const NodeId in = add(*g.def, "graph.input");
  const int64_t o = addSocket(*g.def, SocketSide::Output, "o", "float");
  const NodeId to = add(*g.def, "graph.output");
  connect(*g.def, in, key(b), to, key(o));
  GraphInstance inst(&w.rt, &w.lib, g.id);
  auto set = [&]() {
    return inst.setInput("a", GraphValue::ofPayload(makePayload(Counted()))).ok();
  };
  auto remove = [&](InterfaceSocket &was) {
    was = *g.def->interfaceSocket(a);
    return g.def->removeInterface(a).ok();
  };
  InterfaceSocket was;
  test_assert(set() && g_alive == 1 && inst.setInput(key(b), GraphValue::ofFloat(2)).ok());
  /* Undo may bring the input back, so its value stays. */
  test_assert(remove(was) && inst.evaluate(SocketPath::output("o")).ok() && g_alive == 1);
  test_assert(g.def->restoreInterface(SocketSide::Input, was, 0).ok());
  test_assert(inst.evaluate(out(in, key(a).c_str())).ok());
  test_assert(inst.value(out(in, key(a).c_str()))->payload<Counted>());
  test_assert(inst.forget(out(in, key(a).c_str())).ok());
  /* None by its key drops it, and only it. */
  test_assert(remove(was) && hasCode(inst.setInput("a", GraphValue()), StatusCode::NotFound));
  test_assert(g_alive == 1 && inst.setInput(key(a), GraphValue()).ok() && g_alive == 0);
  test_assert(hasCode(inst.setInput(key(a), GraphValue()), StatusCode::NotFound));
  test_assert(hasCode(inst.setInput(key(a), GraphValue::ofFloat(1)), StatusCode::NotFound));
  /* So does clear, which keeps the others. */
  test_assert(g.def->restoreInterface(SocketSide::Input, was, 0).ok() && set());
  test_assert(inst.clear().ok() && g_alive == 1 && remove(was));
  test_assert(inst.clear().ok() && g_alive == 0);
  test_assert(inst.evaluate(SocketPath::output("o")).ok());
  test_assert(*inst.value(SocketPath::output("o")) == GraphValue::ofFloat(2));
  /* Without its graph, an instance can't tell. */
  test_assert(g.def->restoreInterface(SocketSide::Input, was, 0).ok() && set());
  test_assert(w.lib.markDeleted(g.h).ok() && inst.clear().ok() && g_alive == 1);
  test_assert(hasCode(inst.setInput(key(b), GraphValue::ofFloat(2)), StatusCode::NotFound));
  test_assert(w.lib.restoreFromHistory(g.h).ok() && inst.setInput(key(a), GraphValue()).ok());
  test_assert(g_alive == 0);
}

/* ---- Libraries ---- */

void testLibraries()
{
  World w;
  Library other(&w.rt);
  const Graph inner = newGraph(w.lib, "inner");
  const int64_t o = addSocket(*inner.def, SocketSide::Output, "o", "float");
  GraphDef def(&w.rt, &w.lib);
  const NodeId group = add(def, "graph.group");
  test_assert(def.setGroup(group, inner.id).ok());
  const SocketPath p = out(group, key(o).c_str());
  GraphInstance right(&w.rt, &w.lib, &def), none(&w.rt, nullptr, &def), wrong(&w.rt, &other, &def);
  test_assert(right.evaluate(p).ok() && none.evaluate(p).ok());
  test_assert(hasCode(wrong.evaluate(p), StatusCode::InvalidArgument) && !wrong.value(p));
  test_assert(hasCode(wrong.setInput("x", GraphValue()), StatusCode::InvalidArgument));
  test_assert(wrong.clear().ok() && wrong.retry().ok() && !wrong.isCurrent(p));
  /* Null is the def's, so a change of it is seen. */
  test_assert(w.lib.markDeleted(inner.h).ok());
  test_assert(hasCode(right.evaluate(p), StatusCode::NotFound));
  test_assert(!none.isCurrent(p) && hasCode(none.evaluate(p), StatusCode::NotFound));
  test_assert(w.lib.restoreFromHistory(inner.h).ok() && none.evaluate(p).ok());

  /* A def without one, with and without a Library. */
  GraphDef alone(&w.rt);
  const NodeId m = add(alone, "t.make");
  GraphInstance a(&w.rt, nullptr, &alone), b(&w.rt, &w.lib, &alone);
  test_assert(a.evaluate(out(m)).ok() && b.evaluate(out(m)).ok());

  /* Another Runtime's. */
  World v;
  GraphInstance c(&w.rt, &v.lib, inner.id), d(&w.rt, nullptr, inner.id);
  test_assert(hasCode(c.evaluate(p), StatusCode::InvalidArgument));
  test_assert(hasCode(d.evaluate(p), StatusCode::NotFound));
  GraphDef far(&v.rt), farToo(&v.rt, &v.lib);
  const NodeId n = add(far, "t.make"), n2 = add(farToo, "t.make");
  GraphInstance e(&w.rt, nullptr, &far), f(&w.rt, &v.lib, &farToo), g(&w.rt, nullptr, &farToo);
  test_assert(hasCode(e.evaluate(out(n)), StatusCode::InvalidArgument));
  test_assert(hasCode(f.evaluate(out(n2)), StatusCode::InvalidArgument));
  test_assert(hasCode(g.evaluate(out(n2)), StatusCode::InvalidArgument));
}

/* ---- names ---- */

void testNames()
{
  World w;
  const BlockHandle t = newThing(w.lib, "before", 1), u = newThing(w.lib, "unused", 1);
  GraphDef def(&w.rt, &w.lib);
  const NodeId n = add(def, "t.name");
  StableRef r;
  r.owner = OwnerKey::ofBlock(idOf(w.lib, t));
  test_assert(def.setParam(n, "thing", PropValue::ofPointer(r)).ok());
  GraphInstance inst(&w.rt, &w.lib, &def);
  auto is = [&](const char *name, uint64_t runs) {
    const uint64_t before = inst.totalRuns();
    return inst.evaluate(out(n)).ok() && *inst.value(out(n)) == GraphValue::ofString(name) &&
           inst.totalRuns() - before == runs;
  };
  test_assert(is("before", 1) && is("before", 0) && w.lib.get(t)->renames() == 0);
  test_assert(w.lib.rename(t, "after").ok() && !inst.isCurrent(out(n)) && is("after", 1));
  test_assert(w.lib.get(t)->renames() == 1 && is("after", 0));
  test_assert(w.lib.rename(u, "other").ok() && is("after", 0));
}

} // namespace

int main()
{
  testRemovedNodes();
  test_assert(g_alive == 0);
  testRemovedInGroups();
  testInterior();
  testForget();
  testInputs();
  testLibraries();
  testNames();
  test_assert(g_alive == 0);
  return test_end();
}
