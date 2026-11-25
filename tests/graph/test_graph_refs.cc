#include "def_fixture.h"

#include <chrono>

using namespace fixture;

test_init;

namespace {

void testRefs()
{
  World w;
  const Graph g = newGraph(w.lib, "G");
  w.lib.setFlag(g.h, BlockFlags::Root, true);
  const BlockHandle t1 = newThing(w.lib, "t1", 1);
  const BlockHandle t2 = newThing(w.lib, "t2", 2);
  const BlockId id1 = idOf(w.lib, t1), id2 = idOf(w.lib, t2);
  const NodeId a = add(*g.def, "test.two");
  const uint64_t epoch = w.lib.epoch();
  const uint64_t version = g.def->node(a)->paramVersion();
  test_assert(g.def->setParam(a, "thing", pointerTo(id1, kThingStruct)).ok());
  test_assert(w.lib.epoch() > epoch && g.def->node(a)->paramVersion() > version);
  test_assert(g.def->setParam(a, "hint", pointerTo(id2, kThingStruct)).ok());
  test_assert(g.def->node(a)->blockRefs().size() == 2);
  test_assert(g.def->node(a)->blockRefs()[1].cell.kind == RefKind::Weak);
  test_assert(w.lib.usersOf(t1).size() == 1 && w.lib.usersOf(t1)[0] == g.h);

  /* A graph can't point at itself, at part of a block, or at another struct. */
  const uint64_t before = w.lib.epoch();
  test_assert(hasCode(g.def->setParam(a, "thing", pointerTo(g.id, StructId())),
                      StatusCode::InvalidArgument));
  StableRef part;
  part.owner = OwnerKey::ofBlock(id1);
  part.key.append(KeySegment::member("x"));
  test_assert(hasCode(g.def->setParam(a, "thing", PropValue::ofPointer(part)),
                      StatusCode::InvalidArgument));
  StableRef tool;
  tool.owner = OwnerKey::ofRuntime(7);
  test_assert(hasCode(g.def->setParam(a, "thing", PropValue::ofPointer(tool)),
                      StatusCode::InvalidArgument));
  test_assert(hasCode(g.def->setParam(a, "thing", pointerTo(id1, StructId::of("Other"))),
                      StatusCode::TypeMismatch));
  test_assert(w.lib.epoch() == before);
  test_assert(g.def->node(a)->blockRefs()[0].cell.target == id1);
  GraphDef alone(&w.rt);
  const NodeId n = add(alone, "test.two");
  test_assert(hasCode(alone.setParam(n, "thing", pointerTo(id1, kThingStruct)),
                      StatusCode::FailedPrecondition));

  /* GC: the strong target stays, the weak one goes; the audit agrees. */
  Result<int> swept = w.lib.collect();
  test_assert(swept.ok() && *swept == 1 && w.lib.get(t1) && !w.lib.get(t2));
  /* Removing and reviving the node, with a collect in each state. */
  const NodeState state = g.def->stateOf(*g.def->node(a));
  test_assert(w.lib.usersOf(t1).size() == 1); /* caches the relations */
  test_assert(g.def->removeNode(a).ok() && w.lib.usersOf(t1).size() == 0);
  test_assert(g.def->insertNode(state).ok() && w.lib.usersOf(t1).size() == 1);
  test_assert(g.def->node(a)->blockRefs()[1].cell.kind == RefKind::Weak);
  test_assert(g.def->node(a)->blockRefs()[1].cell.target == id2);
  swept = w.lib.collect();
  test_assert(swept.ok() && *swept == 0);
  test_assert(g.def->setParam(a, "thing", PropValue::nullPointer()).ok());
  test_assert(g.def->node(a)->blockRefs()[0].cell.isNull());
  swept = w.lib.collect();
  test_assert(swept.ok() && *swept == 1 && !w.lib.get(t1));

  NodeState needsLib = state;
  needsLib.id = NodeId{42};
  test_assert(hasCode(alone.insertNode(needsLib), StatusCode::FailedPrecondition));
}

void testGroups()
{
  World w;
  const Graph outer = newGraph(w.lib, "outer");
  const Graph mid = newGraph(w.lib, "mid");
  const Graph inner = newGraph(w.lib, "inner");
  const BlockHandle thing = newThing(w.lib, "t", 0);
  for (const Graph &g : {outer, mid, inner}) {
    w.lib.setFlag(g.h, BlockFlags::Root, g.h == outer.h);
  }

  /* inner: out1 depends on in1, out2 on in2, out3 on both, out4 on none. */
  int64_t in[2], out[4];
  in[0] = addSocket(*inner.def, SocketSide::Input, "i0", "float");
  in[1] = addSocket(*inner.def, SocketSide::Input, "i1", "float");
  for (int i = 0; i < 4; i++) {
    out[i] = addSocket(
        *inner.def, SocketSide::Output, ("o" + std::to_string(i)).c_str(), "float");
  }
  const NodeId input = add(*inner.def, "graph.input");
  const NodeId output = add(*inner.def, "graph.output");
  const NodeId sum = add(*inner.def, "math.float");
  connect(*inner.def, input, key(in[0]), output, key(out[0]));
  const LinkId straight = connect(*inner.def, input, key(in[1]), output, key(out[1]));
  connect(*inner.def, input, key(in[0]), sum, "a");
  connect(*inner.def, input, key(in[1]), sum, "b");
  connect(*inner.def, sum, "value", output, key(out[2]));
  auto reach = graphReach(*inner.def);
  test_assert(reach->inputs.size() == 4 && reach->inputs[0].size() == 1);
  test_assert(reach->inputs[0][0] == 0 && reach->inputs[1][0] == 1);
  test_assert(reach->inputs[2].size() == 2 && reach->inputs[3].size() == 0);
  test_assert(graphReach(*inner.def) == reach); /* cached */
  /* Two outputs that share everything below them. */
  const LinkId shared = connect(*inner.def, sum, "value", output, key(out[3]));
  test_assert(graphReach(*inner.def)->inputs[3].size() == 2);
  test_assert(inner.def->unlink(shared).ok());
  reach = graphReach(*inner.def);

  /* mid: a group of inner, its sockets passed through crosswise. */
  const int64_t mi = addSocket(*mid.def, SocketSide::Input, "mi", "float");
  const int64_t mo = addSocket(*mid.def, SocketSide::Output, "mo", "float");
  const NodeId group = add(*mid.def, "graph.group");
  test_assert(hasCode(mid.def->setGroup(input, inner.id), StatusCode::NotFound));
  test_assert(hasCode(inner.def->setGroup(sum, mid.id), StatusCode::TypeMismatch));
  test_assert(
      hasCode(mid.def->setGroup(group, idOf(w.lib, thing)), StatusCode::TypeMismatch));
  test_assert(hasCode(mid.def->setGroup(group, BlockId{inner.id.client, LocalId{1, 1}}),
                      StatusCode::NotFound));
  test_assert(mid.def->groupTarget(*mid.def->node(group)) == nullptr);
  test_assert(w.lib.usersOf(inner.h).size() == 0);
  test_assert(mid.def->setGroup(group, inner.id).ok());
  test_assert(w.lib.usersOf(inner.h).size() == 1);
  test_assert(graphDefOf(w.lib, thing) == nullptr && graphDefOf(w.lib, mid.h) == mid.def);
  test_assert(mid.def->groupTarget(*mid.def->node(group)) == inner.def);
  test_assert(mid.def->sockets(*mid.def->node(group), SocketSide::Input).size() == 2);
  test_assert(mid.def->sockets(*mid.def->node(group), SocketSide::Output).size() == 4);

  /* Cycles go by what each output depends on: o0 -> i1 is fine, o0 -> i0 isn't. */
  GraphDef &md = *mid.def;
  test_assert(hasCode(md.link({group, key(out[0])}, {group, key(in[0])}),
                      StatusCode::InvalidArgument));
  const LinkId cross = connect(md, group, key(out[0]), group, key(in[1]));
  test_assert(hasCode(md.link({group, key(out[1])}, {group, key(in[0])}),
                      StatusCode::InvalidArgument));
  test_assert(hasCode(md.link({group, key(out[2])}, {group, key(in[1])}),
                      StatusCode::InvalidArgument));
  test_assert(md.link(cross));
  connect(md, group, key(out[3]), group, key(in[0])); /* depends on nothing */
  test_assert(md.validate().ok() && md.linkCount() == 2);
  const NodeId mIn = add(md, "graph.input"), mOut = add(md, "graph.output");
  connect(md, mIn, key(mi), group, key(in[0]));
  connect(md, group, key(out[1]), mOut, key(mo));
  /* mo <- o1 <- i1 <- o0 <- i0 <- mi */
  auto midReach = graphReach(md);
  test_assert(midReach->inputs[0].size() == 1 && midReach->inputs[0][0] == 0);
  /* And through a kernel outside the group. */
  const NodeId k = add(md, "math.float");
  connect(md, group, key(out[1]), k, "a");
  test_assert(
      hasCode(md.link({k, "value"}, {group, key(in[1])}), StatusCode::InvalidArgument));
  test_assert(
      hasCode(md.link({k, "value"}, {group, key(in[0])}), StatusCode::InvalidArgument));

  /* An edit inside shows in the tables above. */
  midReach = graphReach(md);
  test_assert(inner.def->unlink(straight).ok());
  auto again = graphReach(md);
  test_assert(again != midReach && again->inputs[0].size() == 0);
  test_assert(graphReach(*inner.def) != reach);
  connect(md, k, "value", group, key(in[1])); /* o1 no longer depends on i1 */
  /* ... and may make a cycle out of it, which the edit doesn't see. */
  connect(*inner.def, input, key(in[1]), output, key(out[1]));
  test_assert(graphReach(md)->inputs.size() == 1 && md.validate().ok());

  /* Recursion. */
  const NodeId og = add(*outer.def, "graph.group");
  test_assert(outer.def->setGroup(og, mid.id).ok());
  const NodeId ig = add(*inner.def, "graph.group");
  test_assert(hasCode(inner.def->setGroup(ig, inner.id), StatusCode::InvalidArgument));
  test_assert(hasCode(inner.def->setGroup(ig, mid.id), StatusCode::InvalidArgument));
  test_assert(hasCode(inner.def->setGroup(ig, outer.id), StatusCode::InvalidArgument));
  test_assert(inner.def->node(ig)->group().isNull());
  NodeState state = inner.def->stateOf(*inner.def->node(ig));
  state.id = NodeId{4242};
  state.group = outer.id;
  test_assert(hasCode(inner.def->insertNode(state), StatusCode::InvalidArgument));
  test_assert(inner.def->setGroup(ig, BlockId()).ok());
  /* A group comes back with its def. */
  state = outer.def->stateOf(*outer.def->node(og));
  test_assert(outer.def->removeNode(og).ok() && w.lib.usersOf(mid.h).size() == 0);
  test_assert(outer.def->insertNode(state).ok() && w.lib.usersOf(mid.h).size() == 1);
  test_assert(outer.def->groupTarget(*outer.def->node(og)) == mid.def);

  /* A deleted def: the group keeps its sockets and links, and its ref. */
  const uint64_t serial = md.topology()->serial;
  test_assert(md.topology()->serial == serial);
  test_assert(w.lib.markDeleted(inner.h).ok());
  test_assert(md.groupTarget(*md.node(group)) == nullptr && md.diagnostics().size() == 0);
  test_assert(md.sockets(*md.node(group), SocketSide::Output).size() == 4);
  test_assert(md.topology()->serial != serial &&
              md.topology()->groups[0].targetInterface == 0);
  test_assert(graphReach(md)->inputs[0].size() == 0);
  test_assert(w.lib.restoreFromHistory(inner.h).ok());
  test_assert(md.topology()->groups[0].targetInterface == inner.def->interfaceVersion());

  /* The interface changes: so does the group. */
  const uint64_t now = md.topology()->serial;
  test_assert(inner.def->setInterfaceDefault(in[0], PropValue::ofFloat(1)).ok());
  test_assert(md.topology()->serial == now); /* a default isn't structure */
  test_assert(inner.def->removeInterface(out[1]).ok());
  test_assert(md.topology()->serial != now);
  test_assert(md.sockets(*md.node(group), SocketSide::Output).size() == 3);
  test_assert(md.diagnostics().size() == 2); /* o1 fed mOut and k */

  /* Group refs keep blocks alive, and GC agrees about them. */
  Result<int> swept = w.lib.collect();
  test_assert(swept.ok() && *swept == 1 && w.lib.get(inner.h) && w.lib.get(mid.h));
  test_assert(outer.def->removeNode(og).ok());
  swept = w.lib.collect();
  test_assert(swept.ok() && *swept == 2 && !w.lib.get(inner.h) && !w.lib.get(mid.h));

  /* Pass-through outputs of a def without a graph.output node depend on nothing. */
  addSocket(*outer.def, SocketSide::Output, "o", "float");
  test_assert(graphReach(*outer.def)->inputs.size() == 1);
}

/* A chain of defs that each hold two of the next: 2^n instances, n tables. */
void testDoubling()
{
  World w;
  const int n = 40;
  litestl::util::Vector<Graph> g;
  litestl::util::Vector<int64_t> in, out;
  const auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < n; i++) {
    g.append(newGraph(w.lib, "g"));
    in.append(addSocket(*g[i].def, SocketSide::Input, "i", "float"));
    out.append(addSocket(*g[i].def, SocketSide::Output, "o", "float"));
  }
  for (int i = n - 1; i >= 0; i--) {
    GraphDef &d = *g[i].def;
    const NodeId a = add(d, "graph.input"), b = add(d, "graph.output"),
                 m = add(d, "math.float");
    connect(d, m, "value", b, key(out[i]));
    for (const char *socket : {"a", "b"}) {
      if (i + 1 == n) {
        connect(d, a, key(in[i]), m, socket);
        continue;
      }
      const NodeId q = add(d, "graph.group");
      test_assert(d.setGroup(q, g[i + 1].id).ok());
      connect(d, q, key(out[i + 1]), m, socket);
      connect(d, a, key(in[i]), q, key(in[i + 1]));
    }
  }
  test_assert(graphReach(*g[0].def)->inputs[0].size() == 1);
  const std::chrono::duration<double> t = std::chrono::steady_clock::now() - start;
  test_assert(t.count() < 5);

  /* Recursion made by writing the RefCell: the tables still terminate. */
  struct Args {
    Library *lib;
    BlockId to;
  } args{&w.lib, g[0].id};
  RefVisitor v(
      [](void *p, RefCell &cell) {
        const Args &r = *static_cast<Args *>(p);
        test_assert(r.lib->writeRef(cell, r.to).ok());
      },
      &args);
  const NodeId extra = add(*g[n - 1].def, "graph.group");
  (void)extra;
  g[n - 1].def->foreachRef(v);
  test_assert(g[n - 1].def->groupTarget(*g[n - 1].def->node(extra)) == g[0].def);
  test_assert(graphReach(*g[0].def)->inputs.size() == 1);
  test_assert(graphReach(*g[n - 1].def)->inputs.size() == 1);
}

} // namespace

int main()
{
  testRefs();
  testGroups();
  testDoubling();
  return test_end();
}
