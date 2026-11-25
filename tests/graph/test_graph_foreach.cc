/* graph.foreach: what its elements get, what runs again, and what errors do. */
#include "foreach_fixture.h"

using namespace fixture;

test_init;

namespace {

void testTables()
{
  Elements t = table({1, 2}, {{"a", {1, 2}}, {"b", {3, 4}}});
  test_assert(t.validate().ok() && t.size() == 2 && t.column("b") && !t.column("c"));
  test_assert(t == table({1, 2}, {{"a", {1, 2}}, {"b", {3, 4}}}) && t.validate().ok());
  test_assert(!(t == table({1, 3}, {{"a", {1, 2}}, {"b", {3, 4}}})));
  test_assert(!(t == table({1, 2}, {{"a", {1, 2}}, {"c", {3, 4}}})));
  test_assert(!(t == table({1, 2}, {{"a", {1, 2}}, {"b", {3, 5}}})));
  test_assert(!(t == table({1, 2}, {{"a", {1, 2}}})));
  test_assert(!(t == table({1, 2, 3}, {{"a", {1, 2}}, {"b", {3, 4}}})));
  test_assert(hasCode(table({1, 1}, {}).validate(), StatusCode::InvalidArgument));
  test_assert(hasCode(table({1, 2}, {{"a", {1}}}).validate(), StatusCode::InvalidArgument));
  test_assert(hasCode(table({1}, {{"a", {1, 2}}}).validate(), StatusCode::InvalidArgument));
  test_assert(hasCode(table({1}, {{"a", {1}}, {"a", {1}}}).validate(), StatusCode::InvalidArgument));
  Elements big = t;
  big.keys.append(3);
  test_assert(PayloadTraits<Elements>::bytes(big) > PayloadTraits<Elements>::bytes(t));
  big = t;
  big.columns[0].values.append(GraphValue::ofFloat(1));
  test_assert(PayloadTraits<Elements>::bytes(big) > PayloadTraits<Elements>::bytes(t));
  /* Equal tables are equal payloads. */
  test_assert(tableValue(t) == tableValue(table({1, 2}, {{"a", {1, 2}}, {"b", {3, 4}}})));
}

/* Sockets, and what each input of the def gets. */
void testInputs()
{
  World w;
  Loop l(w);
  const GraphDef &def = *l.root.def;
  const GraphNode &n = *def.node(l.node);
  test_assert(def.sockets(n, SocketSide::Input).size() == 4);
  test_assert(strcmp(def.sockets(n, SocketSide::Input)[0].key.c_str(), "elements") == 0);
  test_assert(def.sockets(n, SocketSide::Output).size() == 1);

  /* No table, and an empty one. */
  test_assert(l.is({}, {{"sum", {}}, {"c2", {}}}, 0));
  l.set(table({}, {}));
  test_assert(l.is({}, {{"sum", {}}, {"c2", {}}}, 0));
  /* Columns, the node's own inputs (here the interface's default 0), columns of no input. */
  l.set(table({7, 8}, {{"a", {1, 2}}, {"other", {5, 6}}}));
  test_assert(l.is({7, 8}, {{"sum", {1, 2}}, {"c2", {0, 0}}}, 4));
  l.set(table({7, 8}, {{"a", {1, 2}}, {"b", {10, 20}}}));
  test_assert(l.is({7, 8}, {{"sum", {11, 22}}, {"c2", {0, 0}}}, 2));
  /* A column wins over a literal; without it the literal is there. */
  test_assert(l.root.def->setLiteral(l.node, key(l.b), PropValue::ofFloat(100)).ok());
  test_assert(l.is({7, 8}, {{"sum", {11, 22}}, {"c2", {0, 0}}}, 0));
  l.set(table({7, 8}, {{"a", {1, 2}}}));
  test_assert(l.is({7, 8}, {{"sum", {101, 102}}, {"c2", {0, 0}}}, 2));
  l.set(table({7, 8}, {{"a", {1, 2}}, {"b", {100, 100}}})); /* another source, same value */
  test_assert(l.is({7, 8}, {{"sum", {101, 102}}, {"c2", {0, 0}}}, 2));
  /* A link, and the root's input. */
  const NodeId in = l.in;
  const int64_t k = addSocket(*l.root.def, SocketSide::Input, "k", "float");
  connect(*l.root.def, in, key(k), l.node, key(l.c));
  test_assert(l.inst.setInput("k", GraphValue::ofFloat(3)).ok());
  test_assert(l.is({7, 8}, {{"sum", {101, 102}}, {"c2", {9, 9}}}, 2));

  /* Conversions: int to float; a string isn't one. */
  Elements t = table({1, 2}, {{"a", {1, 2}}});
  t.columns[0].values[1] = GraphValue::ofInt(5);
  l.set(t);
  test_assert(l.is({1, 2}, {{"sum", {101, 105}}, {"c2", {9, 9}}}, 4));
  t.columns[0].values[1] = GraphValue::ofFloat(5); /* the value it had */
  l.set(t);
  test_assert(l.is({1, 2}, {{"sum", {101, 105}}, {"c2", {9, 9}}}, 0));
  t.columns[0].values[1] = GraphValue::ofString("5");
  l.set(t);
  test_assert(l.fails(StatusCode::TypeMismatch, {2}, "element 2: column 'a': "));
  t.columns[0].values[1] = GraphValue();
  l.set(t);
  test_assert(l.fails(StatusCode::TypeMismatch, {2}, "element 2: column 'a': "));
  t.columns[0].values[1] = GraphValue::ofPayload(PayloadRef());
  l.set(t);
  test_assert(l.fails(StatusCode::TypeMismatch, {2}, "element 2: column 'a': "));
  t.columns[0].values[1] = tableValue(Elements()); /* a payload without a conversion */
  l.set(t);
  test_assert(l.fails(StatusCode::TypeMismatch, {2}, "element 2: column 'a': "));

  /* Bad tables run nothing. */
  for (const Elements &bad : {table({1, 2, 1}, {{"a", {9, 9, 9}}}), table({1}, {{"a", {1, 2}}})}) {
    const uint64_t runs = l.inst.totalRuns();
    l.set(bad);
    test_assert(l.fails(StatusCode::InvalidArgument, {}, ""));
    test_assert(strcmp(l.error()->socket.c_str(), "elements") == 0 && l.inst.totalRuns() == runs);
    l.root.def->setLabel(l.node, "x").ignore();
    w.rt.touch();
    test_assert(l.fails(StatusCode::InvalidArgument, {}, "") && l.inst.totalRuns() == runs);
    test_assert(l.root.def->setLiteral(l.node, key(l.b), PropValue::ofFloat(1)).ok());
    test_assert(l.fails(StatusCode::InvalidArgument, {}, "") && l.inst.totalRuns() == runs);
    test_assert(l.root.def->setLiteral(l.node, key(l.b), PropValue::ofFloat(100)).ok());
  }

  /* Interface edits of the def: the sockets and columns follow. */
  l.set(table({1}, {{"a", {1}}, {"b", {2}}}));
  test_assert(l.is({1}, {{"sum", {3}}, {"c2", {9}}}, 1)); /* c2 is kept through the errors */
  test_assert(l.inner.def->setInterfaceName(l.b, "B").ok());
  test_assert(l.is({1}, {{"sum", {101}}, {"c2", {9}}}, 1));
  test_assert(l.inner.def->moveInterface(l.b, 0).ok());
  test_assert(l.is({1}, {{"sum", {101}}, {"c2", {9}}}, 0));
  test_assert(l.inner.def->setInterfaceName(l.sum, "total").ok());
  test_assert(l.is({1}, {{"total", {101}}, {"c2", {9}}}, 0));
  test_assert(l.inner.def->removeInterface(l.c2).ok());
  test_assert(l.is({1}, {{"total", {101}}}, 0));
  const int64_t more = addSocket(*l.inner.def, SocketSide::Output, "more", "vec3");
  const NodeId out = l.innerOut;
  test_assert(l.evaluate().ok() && *l.column("more") == GraphValue::ofVec3(0, 0, 0));
  connect(*l.inner.def, l.add, "value", out, key(more));
  test_assert(l.evaluate().ok() && *l.column("more") == GraphValue::ofVec3(101, 101, 101));
  test_assert(l.inner.def->removeNode(out).ok()); /* no output node: defaults */
  test_assert(l.is({1}, {{"total", {0}}, {"more", {}}}, 0));
}

/* What runs, edit by edit. Elements run l.add (a + b) and l.square (c * c). */
void testRuns()
{
  World w;
  Loop l(w);
  l.set(table({1, 2, 3}, {{"a", {1, 2, 3}}}));
  test_assert(l.is({1, 2, 3}, {{"sum", {1, 2, 3}}, {"c2", {0, 0, 0}}}, 6) && l.ran() == 1);
  test_assert(l.is({1, 2, 3}, {{"sum", {1, 2, 3}}, {"c2", {0, 0, 0}}}, 0) && l.ran() == 0);
  /* Edits elsewhere don't even visit the elements. */
  const NodeId other = add(*l.root.def, "math.float");
  w.rt.touch();
  newThing(w.lib, "unused", 1);
  test_assert(l.is({1, 2, 3}, {{"sum", {1, 2, 3}}, {"c2", {0, 0, 0}}}, 0) && l.ran() == 0);
  test_assert(l.root.def->setLiteral(other, "a", PropValue::ofFloat(1)).ok());
  test_assert(l.inner.def->setLabel(l.add, "label").ok());
  test_assert(l.is({1, 2, 3}, {{"sum", {1, 2, 3}}, {"c2", {0, 0, 0}}}, 0) && l.ran() == 0);
  /* An equal table, one value, a new key, another order, a key that goes and comes back. */
  l.set(table({1, 2, 3}, {{"a", {1, 2, 3}}}));
  test_assert(l.is({1, 2, 3}, {{"sum", {1, 2, 3}}, {"c2", {0, 0, 0}}}, 0) && l.ran() == 0);
  l.set(table({1, 2, 3}, {{"a", {1, 5, 3}}}));
  test_assert(l.is({1, 2, 3}, {{"sum", {1, 5, 3}}, {"c2", {0, 0, 0}}}, 1) && l.ran() == 1);
  l.set(table({1, 2, 9, 3}, {{"a", {1, 5, 7, 3}}}));
  test_assert(l.is({1, 2, 9, 3}, {{"sum", {1, 5, 7, 3}}, {"c2", {0, 0, 0, 0}}}, 2));
  l.set(table({3, 9, 2, 1}, {{"a", {3, 7, 5, 1}}}));
  test_assert(l.is({3, 9, 2, 1}, {{"sum", {3, 7, 5, 1}}, {"c2", {0, 0, 0, 0}}}, 0));
  l.set(table({3, 2, 1}, {{"a", {3, 5, 1}}}));
  test_assert(l.is({3, 2, 1}, {{"sum", {3, 5, 1}}, {"c2", {0, 0, 0}}}, 0));
  l.set(table({3, 9, 2}, {{"a", {3, 7, 5}}}));
  test_assert(l.is({3, 9, 2}, {{"sum", {3, 7, 5}}, {"c2", {0, 0, 0}}}, 2)); /* 9 was evicted */
  l.set(table({1, 2, 3}, {{"a", {1, 2, 3}}}));
  test_assert(l.is({1, 2, 3}, {{"sum", {1, 2, 3}}, {"c2", {0, 0, 0}}}, 3));

  /* What all elements read: the node's inputs, the def, its interface's defaults. */
  test_assert(l.root.def->setLiteral(l.node, key(l.c), PropValue::ofFloat(2)).ok());
  test_assert(l.is({1, 2, 3}, {{"sum", {1, 2, 3}}, {"c2", {4, 4, 4}}}, 3));
  test_assert(l.inner.def->setInterfaceDefault(l.b, PropValue::ofFloat(10)).ok());
  test_assert(l.is({1, 2, 3}, {{"sum", {11, 12, 13}}, {"c2", {4, 4, 4}}}, 3));
  test_assert(l.inner.def->setLiteral(l.square, "b", PropValue::ofFloat(1)).ok()); /* linked */
  test_assert(l.is({1, 2, 3}, {{"sum", {11, 12, 13}}, {"c2", {4, 4, 4}}}, 0) && l.ran() == 1);
  test_assert(l.inner.def->setParam(l.square, "op", PropValue::ofEnum(0)).ok());
  test_assert(l.is({1, 2, 3}, {{"sum", {11, 12, 13}}, {"c2", {4, 4, 4}}}, 3)); /* 2 + 2 */
  const NodeId unused = add(*l.inner.def, "math.float");
  test_assert(l.inner.def->setLiteral(unused, "a", PropValue::ofFloat(1)).ok());
  test_assert(l.is({1, 2, 3}, {{"sum", {11, 12, 13}}, {"c2", {4, 4, 4}}}, 0));
  /* An output table that is equal moves nothing. */
  const NodeId total = add(*l.root.def, "t.total");
  connect(*l.root.def, l.node, kElementsSocket, total, "elements");
  const SocketPath totalPath = SocketPath::of(total, "value");
  test_assert(l.inst.evaluate(totalPath).ok());
  const uint64_t before = l.inst.totalRuns();
  test_assert(l.inner.def->setLiteral(unused, "a", PropValue::ofFloat(2)).ok());
  test_assert(l.inst.evaluate(totalPath).ok() && l.inst.totalRuns() == before);
  /* An element that changes after that still finds its state. */
  l.set(table({1, 2, 3}, {{"a", {1, 2, 4}}}));
  test_assert(l.is({1, 2, 3}, {{"sum", {11, 12, 14}}, {"c2", {4, 4, 4}}}, 1));
  /* Both at once: the skip for unchanged rows mustn't hide the edit. */
  l.set(table({1, 2, 3}, {{"a", {1, 2, 5}}}));
  test_assert(l.inner.def->setParam(l.square, "op", PropValue::ofEnum(1)).ok()); /* 2 - 2 */
  test_assert(l.is({1, 2, 3}, {{"sum", {11, 12, 15}}, {"c2", {0, 0, 0}}}, 4));

  /* A group inside: its def, deleting it, and restoring it. */
  const Graph g = newGraph(w.lib, "deep");
  const int64_t gi = addSocket(*g.def, SocketSide::Input, "i", "float");
  const int64_t go = addSocket(*g.def, SocketSide::Output, "o", "float");
  const NodeId gin = add(*g.def, "graph.input"), gout = add(*g.def, "graph.output");
  const NodeId gm = add(*g.def, "math.float");
  connect(*g.def, gin, key(gi), gm, "a");
  connect(*g.def, gm, "value", gout, key(go));
  const NodeId group = add(*l.inner.def, "graph.group");
  test_assert(l.inner.def->setGroup(group, g.id).ok());
  connect(*l.inner.def, l.add, "value", group, key(gi));
  connect(*l.inner.def, group, key(go), l.innerOut, key(l.sum));
  test_assert(l.is({1, 2, 3}, {{"sum", {11, 12, 15}}, {"c2", {0, 0, 0}}}, 3));
  test_assert(l.inst.frameCount() == 0); /* elements, and what is in them, don't count */
  test_assert(g.def->setLiteral(gm, "b", PropValue::ofFloat(1)).ok());
  test_assert(l.is({1, 2, 3}, {{"sum", {12, 13, 16}}, {"c2", {0, 0, 0}}}, 3));
  test_assert(w.lib.markDeleted(g.h).ok());
  test_assert(l.fails(StatusCode::NotFound, {1}, "element 1: "));
  test_assert(l.error()->origin == at({l.node}, group));
  test_assert(w.lib.restoreFromHistory(g.h).ok());
  test_assert(l.is({1, 2, 3}, {{"sum", {12, 13, 16}}, {"c2", {0, 0, 0}}}, 0));
  /* Other data under the group's id: the content is as new as it was, but another. */
  test_assert(w.lib.markDeleted(g.h).ok() && w.lib.destroyFromHistory(g.h).ok());
  GraphDef *again = litestl::alloc::New<GraphDef>("coredcc::GraphDef", &w.rt, &w.lib, g.id);
  test_assert(again->addInterface(SocketSide::Output, "o", "float", -1, go).ok());
  test_assert(w.lib.reviveAs(TypeId::of(kGraphDefType), g.id, "deep", again).ok());
  test_assert(l.is({1, 2, 3}, {{"sum", {0, 0, 0}}, {"c2", {0, 0, 0}}}, 0));
}

/* Blocks that the elements read. */
void testBlocks()
{
  World w;
  Loop l(w);
  const BlockHandle h = newThing(w.lib, "t", 5), h2 = newThing(w.lib, "u", 6);
  const NodeId read = add(*l.inner.def, "t.read");
  test_assert(l.inner.def->setParam(read, "thing", toThing(idOf(w.lib, h))).ok());
  connect(*l.inner.def, read, "value", l.add, "b");
  l.set(table({1, 2}, {{"a", {1, 2}}, {"c", {3, 4}}}));
  test_assert(l.is({1, 2}, {{"sum", {6, 7}}, {"c2", {9, 16}}}, 6));
  w.thing(h).value = 7;
  test_assert(l.is({1, 2}, {{"sum", {6, 7}}, {"c2", {9, 16}}}, 0));
  w.notify(idOf(w.lib, h));
  test_assert(l.is({1, 2}, {{"sum", {8, 9}}, {"c2", {9, 16}}}, 4) && l.ran() == 1);
  w.notify(idOf(w.lib, h2));
  test_assert(l.is({1, 2}, {{"sum", {8, 9}}, {"c2", {9, 16}}}, 0) && l.ran() == 0);
  test_assert(w.lib.markDeleted(h).ok() && l.evaluate().ok() && l.ran() == 1);
  test_assert(w.lib.restoreFromHistory(h).ok());
  test_assert(l.is({1, 2}, {{"sum", {8, 9}}, {"c2", {9, 16}}}, 4));
  /* After a run of one element only, the others' blocks are still known. */
  l.set(table({1, 2}, {{"a", {1, 3}}, {"c", {3, 4}}}));
  test_assert(l.is({1, 2}, {{"sum", {8, 10}}, {"c2", {9, 16}}}, 1));
  w.thing(h).value = 8;
  w.notify(idOf(w.lib, h));
  test_assert(l.is({1, 2}, {{"sum", {9, 11}}, {"c2", {9, 16}}}, 4));
  /* A Library may write the cell; the def doesn't know. */
  redirect(*l.inner.def, w.lib, idOf(w.lib, h2));
  test_assert(l.is({1, 2}, {{"sum", {7, 9}}, {"c2", {9, 16}}}, 4));
  l.ran();
  w.notify(idOf(w.lib, h));
  test_assert(l.evaluate().ok() && l.ran() == 0); /* not read any more */
  w.thing(h2).value = 1;
  w.notify(idOf(w.lib, h2));
  test_assert(l.is({1, 2}, {{"sum", {2, 4}}, {"c2", {9, 16}}}, 4));
  l.ran();
  newThing(w.lib, "more", 1); /* the references are compared, and the same */
  test_assert(l.is({1, 2}, {{"sum", {2, 4}}, {"c2", {9, 16}}}, 0) && l.ran() == 0);
  newThing(w.lib, "more", 1);
  test_assert(l.is({1, 2}, {{"sum", {2, 4}}, {"c2", {9, 16}}}, 0) && l.ran() == 0);
  /* Queries count too: an interface comes and goes with no notification. */
  const NodeId query = add(*l.inner.def, "t.query");
  test_assert(l.inner.def->setParam(query, "thing", toThing(idOf(w.lib, h2))).ok());
  connect(*l.inner.def, query, "value", l.add, "b");
  test_assert(l.is({1, 2}, {{"sum", {0, 2}}, {"c2", {9, 16}}}, 4)); /* -1 */
  test_assert(w.addScaled(h2, 50));
  test_assert(l.is({1, 2}, {{"sum", {51, 53}}, {"c2", {9, 16}}}, 4));
  /* A block that moves while the elements run is looked at again. */
  w.notifyDuring = idOf(w.lib, h2);
  l.set(table({1, 2}, {{"a", {1, 3}}, {"c", {3, 5}}}));
  connect(*l.inner.def, read, "value", l.add, "b");
  w.notify(idOf(w.lib, h2));
  l.ran();
  test_assert(l.evaluate().ok() && l.ran() == 1 && w.notifyDuring.isNull());
  test_assert(l.evaluate().ok() && l.ran() == 1);
  test_assert(l.evaluate().ok() && l.ran() == 0);
}

/* References to groups that a Library writes, and pins. */
Library *g_lib = nullptr;
BlockId g_block;

Status pinsKernel(NodeExecContext &ctx)
{
  const int pins = g_lib->get(g_lib->find(g_block))->pins(PinKind::Host);
  return ctx.setOutput(0, GraphValue::ofFloat(pins + ctx.input(0).asFloat()));
}

void testGroupRefs()
{
  World w;
  Loop l(w);
  Graph g[2];
  int64_t o[2];
  for (int i = 0; i < 2; i++) {
    g[i] = newGraph(w.lib, "g");
    o[i] = addSocket(*g[i].def, SocketSide::Output, "o", "float");
    const NodeId out = add(*g[i].def, "graph.output"), v = add(*g[i].def, "value.float");
    test_assert(g[i].def->setParam(v, "value", PropValue::ofFloat(i + 1)).ok());
    connect(*g[i].def, v, "value", out, key(o[i]));
  }
  /* Three groups, of 0, 1 and 0. The third's becomes 1, which is in the list of defs already. */
  NodeId n[3];
  for (int i = 0; i < 3; i++) {
    n[i] = add(*l.inner.def, "graph.group");
    test_assert(l.inner.def->setGroup(n[i], g[i & 1].id).ok());
  }
  connect(*l.inner.def, n[2], key(o[0]), l.innerOut, key(l.sum));
  l.set(table({5}, {}));
  test_assert(l.is({5}, {{"sum", {1}}, {"c2", {0}}}, 2));
  w.lib.writeRef(const_cast<RefCell &>(l.inner.def->node(n[2])->group()), g[1].id).ignore();
  test_assert(l.is({5}, {{"sum", {0}}, {"c2", {0}}}, 0)); /* it has no such socket */

  NodeTypeDef t("t.pins");
  t.input(PropertyDef::floatProp("a")).output("value", "float");
  addType<pinsKernel>(w.rt, t, "t.pins");
  const NodeId p = add(*l.inner.def, "t.pins");
  connect(*l.inner.def, l.innerIn, key(l.a), p, "a");
  connect(*l.inner.def, p, "value", l.innerOut, key(l.sum));
  g_lib = &w.lib;
  g_block = l.inner.id;
  l.set(table({1, 2, 3}, {{"a", {0, 10, 20}}}));
  test_assert(l.is({1, 2, 3}, {{"sum", {1, 11, 21}}, {"c2", {0, 0, 0}}}, 6)); /* one pin */
  test_assert(w.lib.get(l.inner.h)->pins(PinKind::Host) == 0);
}

/* Elements with and without a state of their own. */
void testMaxCached()
{
  World w;
  Loop l(w);
  const Elements five = table({1, 2, 3, 4, 5}, {{"a", {1, 2, 3, 4, 5}}});
  l.set(five);
  auto setMax = [&](int64_t n) {
    return l.root.def->setParam(l.node, kMaxCachedParam, PropValue::ofInt(n)).ok();
  };
  const Columns all = {{"sum", {1, 2, 3, 4, 5}}, {"c2", {0, 0, 0, 0, 0}}};
  test_assert(setMax(2));
  /* Two with 2 runs each; the other three run l.add each, and share l.square's. */
  test_assert(l.is({1, 2, 3, 4, 5}, all, 2 * 2 + 3 + 1));
  w.rt.touch();
  test_assert(l.is({1, 2, 3, 4, 5}, all, 0) && l.ran() == 0);
  l.set(table({1, 2, 3, 4, 5}, {{"a", {1, 7, 3, 4, 5}}}));
  test_assert(l.is({1, 2, 3, 4, 5}, {{"sum", {1, 7, 3, 4, 5}}, {"c2", {0, 0, 0, 0, 0}}}, 1));
  l.set(table({1, 2, 3, 4, 5}, {{"a", {1, 7, 3, 4, 8}}}));
  test_assert(l.is({1, 2, 3, 4, 5}, {{"sum", {1, 7, 3, 4, 8}}, {"c2", {0, 0, 0, 0, 0}}}, 1));
  NodeReport report = l.inst.report(at({}, l.node));
  test_assert(report.elements == 5 && report.cachedElements == 2);
  /* 5 comes first: 2 loses its state, and 5 gets one when it runs. No row differs. */
  l.set(table({1, 5, 2, 3, 4}, {{"a", {1, 8, 7, 3, 4}}}));
  test_assert(l.is({1, 5, 2, 3, 4}, {{"sum", {1, 8, 7, 3, 4}}, {"c2", {0, 0, 0, 0, 0}}}, 0));
  report = l.inst.report(at({}, l.node));
  test_assert(report.elements == 5 && report.cachedElements == 1 && l.inst.elementCount() == 1);
  l.set(table({1, 5, 2, 3, 4}, {{"a", {1, 9, 7, 3, 4}}}));
  test_assert(l.is({1, 5, 2, 3, 4}, {{"sum", {1, 9, 7, 3, 4}}, {"c2", {0, 0, 0, 0, 0}}}, 2));
  test_assert(l.inst.elementCount() == 2);
  l.set(table({1, 2, 5, 3, 4}, {{"a", {1, 7, 9, 3, 4}}}));
  test_assert(l.is({1, 2, 5, 3, 4}, {{"sum", {1, 7, 9, 3, 4}}, {"c2", {0, 0, 0, 0, 0}}}, 0));
  /* A row gone, a new one, and one that changed. */
  l.set(table({1, 6, 5, 4}, {{"a", {1, 7, 9, 5}}}));
  test_assert(l.is({1, 6, 5, 4}, {{"sum", {1, 7, 9, 5}}, {"c2", {0, 0, 0, 0}}}, 2 + 1));
  l.set(table({1, 2, 3, 4, 5}, {{"a", {1, 7, 3, 4, 8}}}));
  test_assert(l.is({1, 2, 3, 4, 5}, {{"sum", {1, 7, 3, 4, 8}}, {"c2", {0, 0, 0, 0, 0}}}, 2 + 3));
  /* None, and all. */
  test_assert(setMax(0) && l.evaluate().ok());
  l.set(five);
  test_assert(l.is({1, 2, 3, 4, 5}, all, 2));
  test_assert(setMax(16) && l.is({1, 2, 3, 4, 5}, all, 10));
  test_assert(l.root.def->setParam(l.node, kMaxCachedParam, PropValue()).ok());
  test_assert(l.is({1, 2, 3, 4, 5}, all, 0));
  /* A block read only by elements without a state. */
  test_assert(setMax(1));
  const BlockHandle h = newThing(w.lib, "t", 5);
  const NodeId read = add(*l.inner.def, "t.readIf");
  test_assert(l.inner.def->setParam(read, "thing", toThing(idOf(w.lib, h))).ok());
  connect(*l.inner.def, l.innerIn, key(l.a), read, "a");
  connect(*l.inner.def, read, "value", l.add, "b");
  l.set(table({1, 2}, {{"a", {-1, 2}}})); /* the first doesn't read it */
  test_assert(l.is({1, 2}, {{"sum", {-1, 7}}, {"c2", {0, 0}}}, 4));
  w.thing(h).value = 6;
  w.notify(idOf(w.lib, h));
  test_assert(l.is({1, 2}, {{"sum", {-1, 8}}, {"c2", {0, 0}}}, 2));
}

} // namespace

int main()
{
  testTables();
  testInputs();
  testRuns();
  testBlocks();
  testGroupRefs();
  testMaxCached();
  return test_end();
}
