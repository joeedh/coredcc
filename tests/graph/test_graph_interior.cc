/* Interior sockets of group nodes: what they read and replace, cycles, and what breaks
 * them. */
#include "edit_fixture.h"
#include "foreach_fixture.h"

#include "coredcc/graph/interior.h"
#include "coredcc/graph/topology.h"

using namespace fixture;

test_init;

namespace {

std::string interior(std::initializer_list<NodeId> path, std::string_view key)
{
  std::string s = "@";
  for (NodeId id : path) {
    s += (s.size() > 1 ? "/" : "") + std::to_string(id.value);
  }
  return s + "." + std::string(key);
}

void testKeys()
{
  const std::optional<InteriorKey> a = InteriorKey::parse("@7.value");
  test_assert(a && a->node == NodeId{7} && a->groups.size() == 0);
  test_assert(strcmp(a->key.c_str(), "value") == 0 && a->toString() == "@7.value");
  const std::optional<InteriorKey> b = InteriorKey::parse("@3/40/7.#12");
  test_assert(b && b->node == NodeId{7} && b->groups.size() == 2 &&
              b->groups[1] == NodeId{40});
  test_assert(strcmp(b->key.c_str(), "#12") == 0 && b->toString() == "@3/40/7.#12");
  test_assert(b->inner().toString() == "@40/7.#12" && !(*a == *b) && *b == *b);
  test_assert(!(*a == *InteriorKey::parse("@7.b")) &&
              !(*a == *InteriorKey::parse("@8.value")));
  test_assert(!(b->inner() == *InteriorKey::parse("@41/7.#12")));
  for (const char *bad : {"",
                          "value",
                          "7.value",
                          "17.value",
                          "@",
                          "@7",
                          "@7.",
                          "@.value",
                          "@/7.value",
                          "@7/.value",
                          "@07.value",
                          "@-7.value",
                          "@+7.value",
                          "@0.value",
                          "@7 .value",
                          "@7x.value",
                          "@7.@8.value",
                          "@7//8.value",
                          "@99999999999999999999.value"})
  {
    test_assert(!InteriorKey::parse(bad));
  }
  test_assert(isInteriorKey("@") && !isInteriorKey("") && !isInteriorKey("a@"));
}

/*
 * An inner def: out = last = (first = a + 1) + 10, with the input a. A root def with two
 * groups of it, and a node `last` that adds what it gets.
 */
struct Doc2 {
  World w;
  Graph root, inner;
  NodeId in, out, first, last;
  int64_t a = 0, o = 0;
  NodeId g1, g2, sink, constant;
  GraphInstance inst;

  Doc2()
      : root(makeGraph(w.lib, "root")), inner(makeGraph(w.lib, "inner")),
        inst(&w.rt, &w.lib, root.id)
  {
    GraphDef &d = *inner.def;
    a = addSocket(d, SocketSide::Input, "a", "float");
    o = addSocket(d, SocketSide::Output, "o", "float");
    in = add(d, "graph.input");
    out = add(d, "graph.output");
    first = add(d, "t.sum");
    last = add(d, "t.sum");
    connect(d, in, key(a), first, "a");
    connect(d, first, "value", last, "a");
    connect(d, last, "value", out, key(o));
    test_assert(d.setLiteral(first, "b", PropValue::ofFloat(1)).ok());
    test_assert(d.setLiteral(last, "b", PropValue::ofFloat(10)).ok());
    GraphDef &r = *root.def;
    g1 = add(r, "graph.group");
    g2 = add(r, "graph.group");
    sink = add(r, "t.sum");
    constant = add(r, "value.float");
    test_assert(r.setGroup(g1, inner.id).ok() && r.setGroup(g2, inner.id).ok());
    test_assert(r.setParam(constant, "value", PropValue::ofFloat(100)).ok());
  }
  /* A node's value, and how many kernels ran for it. */
  bool is(const SocketPath &p, double value, int runs)
  {
    const uint64_t before = inst.totalRuns();
    const Status s = inst.evaluate(p);
    const int ran = int(inst.totalRuns() - before);
    const bool ok = s.ok() && *inst.value(p) == GraphValue::ofFloat(value) && ran == runs;
    if (!ok) {
      printf("%s, %g, %d runs\n",
             s.toString().c_str(),
             s.ok() ? inst.value(p)->asFloat() : 0,
             ran);
    }
    return ok;
  }
  bool isSink(double value, int runs)
  {
    return is(SocketPath::of(sink, "value"), value, runs);
  }
  bool isGroup(NodeId g, double value, int runs)
  {
    return is(SocketPath::of(g, key(o)), value, runs);
  }
};

void testOutputs()
{
  Doc2 d;
  GraphDef &r = *d.root.def;
  test_assert(r.setLiteral(d.g1, key(d.a), PropValue::ofFloat(5)).ok());
  const std::string firstKey = interior({d.first}, "value");
  const LinkId l = connect(r, d.g1, firstKey, d.sink, "a");
  test_assert(d.isSink(6, 2));                             /* `last` doesn't run */
  test_assert(d.is(SocketPath::of(d.g1, firstKey), 6, 0)); /* a socket like others */
  test_assert(hasCode(d.inst.evaluate(SocketPath::of(d.g2, firstKey)),
                      StatusCode::NotFound)); /* unused */
  test_assert(d.isGroup(d.g1, 16, 1) && r.diagnostics().size() == 0);
  connect(r, d.g2, firstKey, d.sink, "b");
  test_assert(d.isSink(7, 2)); /* 6 + 1 */
  /* An edit inside that nobody sees, and one that is seen twice. */
  test_assert(d.inner.def->setLiteral(d.last, "b", PropValue::ofFloat(20)).ok() &&
              d.isSink(7, 0));
  test_assert(d.inner.def->setLiteral(d.first, "b", PropValue::ofFloat(2)).ok() &&
              d.isSink(9, 3));

  /* Other outputs: an interface input, and a group's in the group. */
  connect(r, d.g1, interior({d.in}, key(d.a)), d.sink, "b");
  test_assert(d.isSink(12, 1));
  const Graph deep = makeGraph(d.w.lib, "deep");
  const int64_t x = addSocket(*deep.def, SocketSide::Output, "x", "float");
  const NodeId dout = add(*deep.def, "graph.output"), dk = add(*deep.def, "value.float");
  test_assert(deep.def->setParam(dk, "value", PropValue::ofFloat(3)).ok());
  connect(*deep.def, dk, "value", dout, key(x));
  const NodeId q = add(*d.inner.def, "graph.group");
  test_assert(d.inner.def->setGroup(q, deep.id).ok());
  connect(r, d.g2, interior({q}, key(x)), d.sink, "b");
  test_assert(d.isSink(10, 2));
  connect(r, d.g2, interior({q, dk}, "value"), d.sink, "b");
  test_assert(d.isSink(10, 0)); /* the same source */
  /* Types convert as elsewhere. */
  const NodeId e = add(*deep.def, "value.int");
  test_assert(deep.def->setParam(e, "value", PropValue::ofInt(4)).ok());
  connect(r, d.g2, interior({q, e}, "value"), d.sink, "b");
  test_assert(d.isSink(11, 2));
  /* The type of a deep one. */
  const int64_t serial = int64_t(r.topology()->serial);
  connect(r, d.g2, interior({q}, key(x)), d.sink, "b");
  test_assert(d.isSink(10, 1) && deep.def->setInterfaceType(x, "mat4").ok());
  test_assert(hasCode(d.inst.evaluate(SocketPath::of(d.sink, "value")),
                      StatusCode::TypeMismatch));
  test_assert(deep.def->setInterfaceType(x, "float").ok() && d.isSink(10, 0));
  test_assert(int64_t(r.topology()->serial) != serial);
  test_assert(deep.def->setInterfaceType(x, "int").ok() &&
              d.isSink(10, 1)); /* converts */
  const NodeId v = add(*deep.def, "value.mat4");
  test_assert(hasCode(r.link({d.g2, interior({q, v}, "value")}, {d.sink, "b"}),
                      StatusCode::TypeMismatch));

  /* What isn't there. */
  for (const std::string &bad : {interior({d.first}, "valeu"),
                                 interior({NodeId{123}}, "value"),
                                 interior({q}, "value"),
                                 interior({d.first, dk}, "value"),
                                 interior({q, d.first}, "value"),
                                 interior({d.first}, "a"),
                                 std::string("@x"),
                                 std::string("@")})
  {
    test_assert(hasCode(r.link({d.g2, bad}, {d.sink, "b"}), StatusCode::NotFound));
    test_assert(!r.findSocket(*r.node(d.g2), SocketSide::Output, bad));
  }
  test_assert(
      hasCode(r.link({d.sink, firstKey}, {d.g2, key(d.a)}), StatusCode::NotFound));
  test_assert(r.findSocket(*r.node(d.g2), SocketSide::Output, firstKey).has_value());
  test_assert(r.findSocket(*r.node(d.g2), SocketSide::Output, key(d.o)).has_value());
  test_assert(r.sockets(*r.node(d.g1), SocketSide::Output).size() == 1);
  test_assert(hasCode(r.setLiteral(d.g1, interior({d.last}, "b"), PropValue::ofFloat(1)),
                      StatusCode::NotFound));
  const NodeId loop = add(r, kGraphForeachType);
  test_assert(r.setGroup(loop, d.inner.id).ok());
  test_assert(hasCode(r.link({loop, firstKey}, {d.sink, "b"}), StatusCode::NotFound));
  test_assert(r.unlink(l).ok() && r.validate().ok());
}

void testInputs()
{
  Doc2 d;
  GraphDef &r = *d.root.def;
  test_assert(d.isGroup(d.g1, 11, 2) && d.isGroup(d.g2, 11, 2));
  /* A literal and a link, for one group node only. */
  const LinkId b = connect(r, d.constant, "value", d.g1, interior({d.last}, "b"));
  test_assert(d.isGroup(d.g1, 101, 2) && d.isGroup(d.g2, 11, 0));
  const LinkId a = connect(r, d.constant, "value", d.g1, interior({d.last}, "a"));
  test_assert(d.isGroup(d.g1, 200, 1) && d.isGroup(d.g2, 11, 0));
  const SocketPath inside = SocketPath::of(at({d.g1}, d.last), "a", SocketSide::Input);
  test_assert(d.is(inside, 100, 0) &&
              d.is(SocketPath::of(at({d.g2}, d.last), "a", SocketSide::Input), 1, 0));
  /* The value, and what is replaced, change. */
  test_assert(r.setParam(d.constant, "value", PropValue::ofFloat(50)).ok() &&
              d.isGroup(d.g1, 100, 2));
  test_assert(d.inner.def->setLiteral(d.last, "b", PropValue::ofFloat(20)).ok());
  test_assert(d.isGroup(d.g1, 100, 0) && d.isGroup(d.g2, 21, 1));
  test_assert(d.inner.def->setLiteral(d.first, "b", PropValue::ofFloat(2)).ok());
  test_assert(d.isGroup(d.g1, 100, 0) && d.isGroup(d.g2, 22, 2));
  test_assert(r.unlink(a).ok() && d.isGroup(d.g1, 52, 2)); /* first is needed again */
  test_assert(r.unlink(b).ok() && d.isGroup(d.g1, 22, 1));
  /* Twice: the second link replaces the first. */
  const NodeId other = add(r, "value.int");
  test_assert(r.setParam(other, "value", PropValue::ofInt(7)).ok());
  connect(r, d.constant, "value", d.g1, interior({d.last}, "b"));
  const LinkId second = connect(r, other, "value", d.g1, interior({d.last}, "b"));
  test_assert(r.linkCount() == 1 && d.isGroup(d.g1, 9, 2));
  /* Through other interior sockets: g2's `first` feeds g1's `last`. */
  connect(r, d.g2, interior({d.first}, "value"), d.g1, interior({d.last}, "b"));
  test_assert(!r.link(second) && d.isGroup(d.g1, 4, 1));
  /* The input of an interface output, and a group's in the group. */
  const LinkId whole = connect(r, d.constant, "value", d.g1, interior({d.out}, key(d.o)));
  test_assert(d.isGroup(d.g1, 50, 0));
  test_assert(r.unlink(whole).ok() && d.isGroup(d.g1, 4, 0));

  /* A link whose other end is gone replaces nothing. */
  const int64_t ri = addSocket(r, SocketSide::Input, "ri", "float");
  const NodeId rin = add(r, "graph.input");
  connect(r, rin, key(ri), d.g2, interior({d.last}, "b"));
  test_assert(r.setInterfaceDefault(ri, PropValue::ofFloat(1000)).ok() &&
              d.isGroup(d.g2, 1002, 1));
  test_assert(r.removeInterface(ri).ok() && d.isGroup(d.g2, 22, 1));
  test_assert(r.diagnostics().size() == 1);
  test_assert(r.addInterface(SocketSide::Input, "ri", "float", -1, ri).ok());
  test_assert(d.isGroup(d.g2, 2, 1) && r.diagnostics().size() == 0);

  for (const std::string &bad : {interior({d.last}, "value"),
                                 interior({d.last}, "c"),
                                 interior({NodeId{5}}, "a"),
                                 std::string("@7")})
  {
    test_assert(
        hasCode(r.link({d.constant, "value"}, {d.g1, bad}), StatusCode::NotFound));
  }
  test_assert(hasCode(r.link({d.constant, "value"}, {d.sink, interior({d.last}, "a")}),
                      StatusCode::NotFound));
  const NodeId m = add(r, "value.mat4");
  test_assert(hasCode(r.link({m, "value"}, {d.g1, interior({d.last}, "a")}),
                      StatusCode::TypeMismatch));
  /* isLinked is true for a replaced input that has a literal. */
  test_assert(r.validate().ok());
}

/* Is told whether its input is linked. */
Status linkedKernel(NodeExecContext &ctx)
{
  return ctx.setOutput(
      0, GraphValue::ofFloat(ctx.isLinked(0) ? ctx.input(0).asFloat() : -1));
}

void testLinked()
{
  Doc2 d;
  NodeTypeDef t("t.linked");
  t.input(PropertyDef::floatProp("a")).output("value", "float");
  addType<linkedKernel>(d.w.rt, t, "t.linked");
  const NodeId n = add(*d.inner.def, "t.linked");
  const std::string out = interior({n}, "value");
  connect(*d.root.def, d.g1, out, d.sink, "a");
  test_assert(d.isSink(-1, 2));
  connect(*d.root.def, d.constant, "value", d.g1, interior({n}, "a"));
  test_assert(d.isSink(100, 3));
}

void testDeep()
{
  Doc2 d;
  GraphDef &r = *d.root.def;
  /* mid has a group of inner, and replaces last.b in it with 1000. */
  const Graph mid = makeGraph(d.w.lib, "mid");
  const int64_t mo = addSocket(*mid.def, SocketSide::Output, "mo", "float");
  const NodeId mout = add(*mid.def, "graph.output"), q = add(*mid.def, "graph.group");
  const NodeId k = add(*mid.def, "value.float");
  test_assert(mid.def->setParam(k, "value", PropValue::ofFloat(1000)).ok());
  test_assert(mid.def->setGroup(q, d.inner.id).ok());
  connect(*mid.def, q, key(d.o), mout, key(mo));
  connect(*mid.def, k, "value", q, interior({d.last}, "b"));
  test_assert(r.setGroup(d.g1, mid.id).ok());
  const SocketPath p = SocketPath::of(d.g1, key(mo));
  test_assert(d.is(p, 1001, 3));
  /* From the root: the outermost counts. */
  const LinkId outer = connect(r, d.constant, "value", d.g1, interior({q, d.last}, "b"));
  test_assert(d.is(p, 101, 2));
  connect(r, d.g1, interior({q, d.first}, "value"), d.sink, "a");
  test_assert(d.isSink(1, 1));
  test_assert(
      mid.def->unlink(mid.def->node(q)->linkInto(interior({d.last}, "b"))->id).ok());
  test_assert(d.is(p, 101, 0));
  connect(*mid.def, k, "value", q, interior({d.last}, "b"));
  test_assert(d.is(p, 101, 0) && r.unlink(outer).ok() && d.is(p, 1001, 1));
  /* The group's own input in mid, replaced from the root. */
  const LinkId input = connect(r, d.constant, "value", d.g1, interior({q}, key(d.a)));
  test_assert(d.is(p, 1101, 2) && r.unlink(input).ok() && d.is(p, 1001, 2));
  /* Another group on the way: the key isn't there any more, then is again. */
  connect(r, d.constant, "value", d.g1, interior({q, d.last}, "b"));
  test_assert(d.is(p, 101, 1));
  test_assert(mid.def->setGroup(q, BlockId()).ok() && r.diagnostics().size() == 2);
  test_assert(hasCode(d.inst.evaluate(p), StatusCode::NotFound));
  test_assert(mid.def->setGroup(q, d.inner.id).ok() && r.diagnostics().size() == 0);
  test_assert(d.is(p, 101, 0));
}

/* Edits of the def that take an interior socket away, and bring it back. */
void testChanges()
{
  Doc2 d;
  GraphDef &r = *d.root.def;
  connect(r, d.g1, interior({d.first}, "value"), d.sink, "a");
  connect(r, d.constant, "value", d.g1, interior({d.last}, "b"));
  test_assert(d.isSink(1, 2) && d.isGroup(d.g1, 101, 2));
  const uint64_t serial = r.topology()->serial;
  test_assert(d.inner.def->setLiteral(d.first, "b", PropValue::ofFloat(3)).ok());
  test_assert(r.topology()->serial == serial); /* it isn't structure */
  add(*d.inner.def, "t.sum");
  test_assert(r.topology()->serial == serial); /* nor is a node that no key names */
  const NodeState state = d.inner.def->stateOf(*d.inner.def->node(d.first));
  const LinkState in = GraphDef::stateOf(*d.inner.def->node(d.first)->linksIn()[0]);
  const LinkState to = GraphDef::stateOf(*d.inner.def->node(d.first)->linksOut()[0]);
  test_assert(d.inner.def->removeNode(d.first).ok());
  test_assert(r.diagnostics().size() == 1 &&
              hasCode(r.diagnostics()[0].status, StatusCode::NotFound));
  test_assert(r.topology()->serial != serial && d.isSink(0, 1) &&
              d.isGroup(d.g1, 100, 1));
  test_assert(d.inner.def->insertNode(state).ok() && d.inner.def->insertLink(in).ok() &&
              d.inner.def->insertLink(to).ok());
  test_assert(r.diagnostics().size() == 0 && d.isSink(3, 2));
  /* Its type, through an interface in the group. */
  const LinkId l = connect(r, d.g1, interior({d.in}, key(d.a)), d.sink, "b");
  test_assert(d.isSink(3, 0 + 1));
  test_assert(d.inner.def->setInterfaceType(d.a, "mat4").ok());
  test_assert(r.diagnostics().size() == 1 && r.diagnostics()[0].link == l);
  test_assert(hasCode(d.inst.evaluate(SocketPath::of(d.sink, "value")),
                      StatusCode::TypeMismatch));
  test_assert(d.inner.def->setInterfaceType(d.a, "int").ok() &&
              r.diagnostics().size() == 0);
  test_assert(d.isSink(3, 2));
  /* The group's def. */
  test_assert(d.w.lib.markDeleted(d.inner.h).ok() && r.diagnostics().size() == 3);
  test_assert(d.isSink(0, 1));
  test_assert(d.w.lib.restoreFromHistory(d.inner.h).ok() && r.diagnostics().size() == 0);
  test_assert(d.isSink(3, 1));
  /* The sockets of a lost group, as last known, when they moved while nothing was pulled.
   */
  const LinkId own = connect(r, d.g1, key(d.o), d.sink, "a");
  test_assert(r.unlink(l).ok() && d.w.lib.markDeleted(d.inner.h).ok());
  test_assert(
      hasCode(d.inst.evaluate(SocketPath::of(d.sink, "value")), StatusCode::NotFound));
  test_assert(d.w.lib.restoreFromHistory(d.inner.h).ok() &&
              d.inner.def->removeInterface(d.o).ok());
  test_assert(r.sockets(*r.node(d.g1), SocketSide::Output).size() == 0);
  test_assert(d.w.lib.markDeleted(d.inner.h).ok() && d.isSink(0, 1));
  test_assert(d.w.lib.restoreFromHistory(d.inner.h).ok());
  test_assert(d.inner.def->addInterface(SocketSide::Output, "o", "float", -1, d.o).ok());
  test_assert(r.unlink(own).ok());
  connect(r, d.g1, interior({d.first}, "value"), d.sink, "a");
  test_assert(d.isSink(3, 1));
  /* A node type that comes later. */
  NodeState late;
  late.id = NodeId{500};
  late.typeName = "late.sum";
  late.outputs.append(SocketInfo{"value", "value", "late.type"});
  test_assert(d.inner.def->insertNode(late).ok());
  test_assert(hasCode(r.link({d.g1, "@500.value"}, {d.sink, "b"}), StatusCode::NotFound));
  NodeTypeDef t("late.sum");
  t.input(PropertyDef::floatProp("a"))
      .input(PropertyDef::floatProp("b"))
      .output("value", "float");
  addType<sumKernel>(d.w.rt, t, "late.sum");
  connect(r, d.g1, "@500.value", d.sink, "b");
  test_assert(d.isSink(3, 2));
}

void testCycles()
{
  Doc2 d;
  GraphDef &r = *d.root.def;
  const std::string firstOut = interior({d.first}, "value"),
                    lastOut = interior({d.last}, "value");
  const std::string firstB = interior({d.first}, "b"), lastA = interior({d.last}, "a"),
                    lastB = interior({d.last}, "b");
  auto refused = [&](NodeId a, const std::string &from, NodeId b, const std::string &to) {
    const Result<LinkId> l = r.link({a, from}, {b, to});
    return hasCode(l, StatusCode::InvalidArgument);
  };
  /* Within one group. */
  test_assert(refused(d.g1, lastOut, d.g1, lastB) &&
              refused(d.g1, lastOut, d.g1, firstB));
  test_assert(refused(d.g1, lastOut, d.g1, key(d.a)) &&
              refused(d.g1, key(d.o), d.g1, firstB));
  test_assert(refused(d.g1, firstOut, d.g1, key(d.a)));
  test_assert(r.linkCount() == 0);
  const LinkId fine = connect(r, d.g1, firstOut, d.g1, lastB); /* 1 + 1 */
  test_assert(d.isGroup(d.g1, 2, 2));
  test_assert(r.unlink(fine).ok());
  /* Replacing last.a cuts the way from first to last. */
  const LinkId cut = connect(r, d.constant, "value", d.g1, lastA);
  connect(r, d.g1, lastOut, d.g1, firstB);
  test_assert(d.is(SocketPath::of(d.g1, lastOut), 110, 2));
  connect(r, d.g1, firstOut, d.sink, "a");
  test_assert(d.isSink(110, 2));
  test_assert(refused(d.sink, "value", d.g1, lastB)); /* sink <- first <- last */
  connect(r, d.sink, "value", d.g2, lastB);
  test_assert(refused(d.g2, key(d.o), d.g1, lastB) &&
              refused(d.g2, lastOut, d.g1, key(d.a)));
  connect(r, d.g2, firstOut, d.g1, lastB); /* it doesn't depend on g2's last */
  test_assert(d.isSink(101, 4));
  /* Without the cut there is a cycle: refused as a link, and found when it is there. */
  test_assert(r.unlink(cut).ok());
  test_assert(hasCode(d.inst.evaluate(SocketPath::of(d.sink, "value")),
                      StatusCode::FailedPrecondition));
  test_assert(r.validate().ok());
  connect(r, d.constant, "value", d.g1, lastA);
  test_assert(d.isSink(101, 2)); /* the nodes that were on it had errors */

  /* A cut whose link is broken is none. */
  Doc2 c;
  GraphDef &cr = *c.root.def;
  const int64_t ri = addSocket(cr, SocketSide::Input, "ri", "float");
  connect(cr, add(cr, "graph.input"), key(ri), c.g1, interior({c.last}, "a"));
  test_assert(
      cr.unlink(connect(cr, c.g1, interior({c.last}, "value"), c.g1, key(c.a))).ok());
  test_assert(cr.removeInterface(ri).ok());
  test_assert(hasCode(cr.link({c.g1, interior({c.last}, "value")}, {c.g1, key(c.a)}),
                      StatusCode::InvalidArgument));

  /* A group's input that is itself, through the group's input node. */
  const std::string itself = interior({d.in}, key(d.a));
  test_assert(refused(d.g2, itself, d.g2, key(d.a)));
  LinkState forced;
  forced.id = LinkId{999};
  forced.from = forced.to = d.g2;
  forced.fromKey = itself.c_str();
  forced.toKey = key(d.a).c_str();
  test_assert(r.insertLink(forced).ok());
  const SocketPath input = SocketPath::of(d.g2, key(d.a), SocketSide::Input);
  test_assert(hasCode(d.inst.evaluate(input), StatusCode::FailedPrecondition));
  test_assert(d.inst.error(input)->origin == at({}, d.g2));
  test_assert(hasCode(d.inst.evaluate(SocketPath::of(d.g2, key(d.o))),
                      StatusCode::FailedPrecondition));
  test_assert(r.unlink(forced.id).ok() && d.inst.evaluate(input).ok());

  /* Deep: the tables of the groups on the way take part. */
  Doc2 e;
  const Graph mid = makeGraph(e.w.lib, "mid");
  const int64_t mi = addSocket(*mid.def, SocketSide::Input, "mi", "float");
  const NodeId min = add(*mid.def, "graph.input"), q = add(*mid.def, "graph.group");
  test_assert(mid.def->setGroup(q, e.inner.id).ok());
  connect(*mid.def, min, key(mi), q, interior({e.last}, "b"));
  test_assert(e.root.def->setGroup(e.g1, mid.id).ok());
  GraphDef &er = *e.root.def;
  const std::string deepLast = interior({q, e.last}, "value"),
                    deepFirst = interior({q, e.first}, "value");
  test_assert(
      hasCode(er.link({e.g1, deepLast}, {e.g1, key(mi)}), StatusCode::InvalidArgument));
  const LinkId ok = connect(er, e.g1, deepFirst, e.g1, key(mi));
  test_assert(er.unlink(ok).ok());
  const LinkId outer = connect(er, e.constant, "value", e.g1, interior({q, e.last}, "b"));
  connect(er, e.g1, deepLast, e.g1, key(mi)); /* replaced from the root: mi isn't read */
  test_assert(e.is(SocketPath::of(e.g1, deepLast), 101, 3));
  test_assert(er.unlink(outer).ok());
  test_assert(hasCode(e.inst.evaluate(SocketPath::of(e.g1, deepLast)),
                      StatusCode::FailedPrecondition));

  /* Tables are kept while nothing in them moved, and made anew when it did. */
  ReachKeys keys;
  keys.add(SocketSide::Output, *InteriorKey::parse(interior({e.first}, "value")));
  keys.add(SocketSide::Output, *InteriorKey::parse(interior({e.last}, "value")));
  keys.add(SocketSide::Input, *InteriorKey::parse(interior({e.last}, "a")));
  keys.add(SocketSide::Input, *InteriorKey::parse(interior({e.last}, "a")));
  test_assert(keys.outs.size() == 2 && keys.ins.size() == 1);
  test_assert(keys.find(SocketSide::Output, keys.outs[1]) == 1 &&
              keys.find(SocketSide::Input, keys.outs[0]) == -1);
  const auto table = graphReach(*e.inner.def, keys);
  test_assert(table->inputs.size() == 3 && table->inputs[0].size() == 1 &&
              table->inputs[0][0] == 1);
  test_assert(table->inputs[1].size() == 1 && table->inputs[1][0] == 0 &&
              table->inputs[2].size() == 1);
  e.w.rt.touch();
  test_assert(graphReach(*e.inner.def, keys) == table);
  test_assert(graphReach(*e.inner.def) != table &&
              graphReach(*e.inner.def)->inputs.size() == 1);
  test_assert(e.inner.def->unlink(e.inner.def->node(e.first)->linkInto("a")->id).ok());
  const auto after = graphReach(*e.inner.def, keys);
  test_assert(after != table && after->inputs[1].size() == 0);
  /* Many sets of keys: the cache doesn't grow with them. */
  for (int i = 0; i < 600; i++) {
    ReachKeys more;
    more.add(SocketSide::Output,
             *InteriorKey::parse("@" + std::to_string(i + 1) + ".value"));
    test_assert(graphReach(*e.inner.def, more)->inputs.size() == 2);
  }
  test_assert(graphReach(*e.inner.def, keys)->inputs.size() == 3);
}

Status sameKernel(NodeExecContext &ctx)
{
  return ctx.setOutput(0, ctx.input(0));
}

/* Keys that differ in little. */
void testAlike()
{
  Doc2 d;
  GraphDef &r = *d.root.def;
  /* A node whose input and output have the same name. */
  NodeTypeDef t("t.same");
  t.input(PropertyDef::floatProp("x")).output("x", "float");
  addType<sameKernel>(d.w.rt, t, "t.same");
  const std::string x = interior({add(*d.inner.def, "t.same")}, "x");
  connect(r, d.constant, "value", d.g1, x);
  connect(r, d.g1, x, d.sink, "a");
  test_assert(d.isSink(100, 3));

  /* The same def twice in a group: only one is meant. */
  const Graph mid = makeGraph(d.w.lib, "mid");
  GraphDef &m = *mid.def;
  const int64_t m1 = addSocket(m, SocketSide::Output, "m1", "float");
  const int64_t m2 = addSocket(m, SocketSide::Output, "m2", "float");
  const NodeId mout = add(m, "graph.output"), q1 = add(m, "graph.group"),
               q2 = add(m, "graph.group");
  test_assert(m.setGroup(q1, d.inner.id).ok() && m.setGroup(q2, d.inner.id).ok());
  connect(m, q1, key(d.o), mout, key(m1));
  connect(m, q2, key(d.o), mout, key(m2));
  const NodeId g = add(r, "graph.group");
  test_assert(r.setGroup(g, mid.id).ok());
  const LinkId l = connect(r, d.constant, "value", g, interior({q1, d.last}, "b"));
  test_assert(d.is(SocketPath::of(g, key(m1)), 101, 2) &&
              d.is(SocketPath::of(g, key(m2)), 11, 2));
  test_assert(r.unlink(l).ok());
  connect(r, g, interior({q1, d.last}, "value"), d.sink, "a");
  test_assert(hasCode(r.link({d.sink, "value"}, {g, interior({q1, d.last}, "b")}),
                      StatusCode::InvalidArgument));
  connect(r, d.constant, "value", g, interior({q1, d.last}, "b"));
  test_assert(hasCode(r.link({d.sink, "value"}, {g, interior({q1, d.last}, "b")}),
                      StatusCode::InvalidArgument));
  connect(r, d.sink, "value", g, interior({q2, d.last}, "b"));
  test_assert(d.is(SocketPath::of(g, key(m2)), 102, 2));

  /* A broken cut in mid is none for the root. */
  const int64_t mi = addSocket(m, SocketSide::Input, "mi", "float");
  connect(m, add(m, "graph.input"), key(mi), q1, interior({d.last}, "a"));
  auto around = [&]() {
    return r.link({g, interior({q1, d.last}, "value")},
                  {g, interior({q1, d.first}, "b")});
  };
  Result<LinkId> cut = around();
  test_assert(cut.ok() && r.unlink(*cut).ok() && m.removeInterface(mi).ok());
  test_assert(hasCode(around(), StatusCode::InvalidArgument));

  /* Tables: whose keys are the same but for the side, and of a def with interior links.
   */
  ReachKeys outs, ins;
  outs.add(SocketSide::Output, *InteriorKey::parse(x));
  ins.add(SocketSide::Input, *InteriorKey::parse(x));
  test_assert(graphReach(*d.inner.def, outs)->inputs.size() == 2);
  test_assert(graphReach(*d.inner.def, ins)->inputs.size() == 1);
  const Graph top = makeGraph(d.w.lib, "top");
  GraphDef &p = *top.def;
  const int64_t p1 = addSocket(p, SocketSide::Input, "p1", "float");
  const int64_t p2 = addSocket(p, SocketSide::Input, "p2", "float");
  const int64_t o1 = addSocket(p, SocketSide::Output, "o1", "float");
  const int64_t o2 = addSocket(p, SocketSide::Output, "o2", "float");
  const NodeId pin = add(p, "graph.input"), pout = add(p, "graph.output"),
               pg = add(p, "graph.group");
  test_assert(p.setGroup(pg, d.inner.id).ok());
  connect(p, pin, key(p1), pg, key(d.a));
  connect(p, pin, key(p2), pg, interior({d.last}, "b"));
  connect(p, pg, interior({d.first}, "value"), pout, key(o1));
  connect(p, pg, interior({d.last}, "value"), pout, key(o2));
  auto reach = graphReach(p);
  test_assert(reach->inputs[0].size() == 1 && reach->inputs[0][0] == 0);
  test_assert(reach->inputs[1].size() == 2 && reach->inputs[1][0] == 0 &&
              reach->inputs[1][1] == 1);
  ReachKeys one;
  one.add(SocketSide::Input, *InteriorKey::parse(interior({q1, d.last}, "b")));
  const int64_t mi2 = addSocket(m, SocketSide::Input, "mi2", "float");
  connect(m, add(m, "graph.input"), key(mi2), q1, key(d.a));
  reach = graphReach(m, one);
  test_assert(reach->inputs[0].size() == 2 && reach->inputs[1].size() == 0);
  /* A recursion is left out where the walk meets it, and tables with it aren't kept. */
  const Graph ra = makeGraph(d.w.lib, "ra"), rb = makeGraph(d.w.lib, "rb");
  NodeId group[2];
  for (const Graph &v : {ra, rb}) {
    const int64_t vi = addSocket(*v.def, SocketSide::Input, "i", "float");
    const int64_t vo = addSocket(*v.def, SocketSide::Output, "o", "float");
    const NodeId n = group[v.def == rb.def] = add(*v.def, "graph.group");
    const NodeId vin = add(*v.def, "graph.input"), vout = add(*v.def, "graph.output");
    if (v.def == rb.def) {
      test_assert(v.def->setGroup(n, ra.id).ok());
      connect(*v.def,
              vin,
              key(vi),
              n,
              v.def->sockets(*v.def->node(n), SocketSide::Input)[0].key.c_str());
      connect(*v.def,
              n,
              v.def->sockets(*v.def->node(n), SocketSide::Output)[0].key.c_str(),
              vout,
              key(vo));
    } else {
      connect(*v.def, vin, key(vi), vout, key(vo));
    }
  }
  d.w.lib.writeRef(const_cast<RefCell &>(ra.def->node(group[0])->group()), rb.id)
      .ignore();
  test_assert(graphReach(*ra.def)->inputs[0].size() == 1 &&
              graphReach(*rb.def)->inputs[0].size() == 1);
  /* An edit inside, which isn't one of top. */
  test_assert(d.inner.def->unlink(d.inner.def->node(d.first)->linkInto("a")->id).ok());
  reach = graphReach(p);
  test_assert(reach->inputs[0].size() == 0 && reach->inputs[1].size() == 1);
}

/* In a foreach's def, and in what commands may do. */
void testPlaces()
{
  Doc2 d;
  GraphDef &r = *d.root.def;
  const int64_t ri = addSocket(r, SocketSide::Input, "ri", "float");
  const int64_t ro = addSocket(r, SocketSide::Output, "ro", "float");
  const NodeId rin = add(r, "graph.input"), rout = add(r, "graph.output");
  connect(r, rin, key(ri), d.g1, interior({d.last}, "b"));
  connect(r, d.g1, key(d.o), rout, key(ro));
  const Graph top = makeGraph(d.w.lib, "top");
  const NodeId each = add(*top.def, kGraphForeachType);
  test_assert(top.def->setGroup(each, d.root.id).ok());
  GraphInstance inst(&d.w.rt, &d.w.lib, top.id);
  const NodeId range = add(*top.def, "t.range"), total = add(*top.def, "t.total");
  test_assert(top.def->setLiteral(range, "count", PropValue::ofFloat(3)).ok());
  test_assert(top.def->setParam(total, "column", PropValue::ofString("ro")).ok());
  connect(*top.def, range, "elements", each, kElementsSocket);
  connect(*top.def, each, kElementsSocket, total, "elements");
  const SocketPath p = SocketPath::of(total, "value");
  test_assert(r.setInterfaceName(ri, "i").ok() && inst.evaluate(p).ok());
  test_assert(inst.value(p)->asFloat() == 6); /* 1 + 0, 1, 2 */
  test_assert(d.inner.def->setLiteral(d.first, "b", PropValue::ofFloat(2)).ok() &&
              inst.evaluate(p).ok());
  test_assert(inst.value(p)->asFloat() == 9);

  Doc doc;
  const Graph g = makeGraph(doc.lib, "g"), in = makeGraph(doc.lib, "in");
  const NodeId m = add(*in.def, "math.float");
  const NodeId group = add(*g.def, "graph.group"), n = add(*g.def, "math.float");
  test_assert(g.def->setGroup(group, in.id).ok());
  const PropValue path = pointerTo(g);
  auto number = [](NodeId id) { return PropValue::ofInt(id.value); };
  const std::string out = interior({m}, "value"), into = interior({m}, "a");
  test_assert(hasCode(doc.run(kAddGraphLinkCommand,
                              {{"graph", path},
                               {"from", number(group)},
                               {"fromSocket", PropValue::ofString(out)},
                               {"to", number(n)},
                               {"toSocket", PropValue::ofString("a")}}),
                      StatusCode::InvalidArgument));
  test_assert(hasCode(runGraphEdit(doc.ctx,
                                   g.id,
                                   "x",
                                   [&](GraphEdit &e) {
                                     return e.link({n, "value"}, {group, into}).status();
                                   }),
                      StatusCode::InvalidArgument));
  test_assert(g.def->linkCount() == 0);
  connect(*g.def, group, out, n, "a"); /* directly */
}

} // namespace

int main()
{
  testKeys();
  testOutputs();
  testInputs();
  testLinked();
  testDeep();
  testChanges();
  testCycles();
  testAlike();
  testPlaces();
  return test_end();
}
