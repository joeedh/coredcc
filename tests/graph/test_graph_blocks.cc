/* GraphInstance: kernels that read blocks, and what makes them run again. */
#include "graph_fixture.h"

#include "coredcc/graph/instance.h"
#include "coredcc/graph/system.h"

using namespace fixture;

test_init;

namespace {

/* An interface things may have. */
struct Scaled : IfaceRef {
  double scale() const
  {
    return CDCC_CALL(scale);
  }
  CDCC_INTERFACE(Scaled, "test::Scaled")
  {
    BIND_STRUCT_METHOD(st, scale, MARGS());
  }
};

struct ThingScaled {
  double value = 0;
  double scale() const
  {
    return value;
  }
  static lbt::Struct<ThingScaled> *defineBindings()
  {
    auto *st = new lbt::Struct<ThingScaled>("test::ThingScaled", sizeof(ThingScaled));
    BIND_STRUCT_METHOD(st, scale, MARGS());
    return st;
  }
};

BlockId g_use[2];
bool g_notifyOther = false;

/* The value of its `thing` param's block: -1 without one, -2 if it isn't there, -3 if it
 * isn't Live. */
Status readParam(NodeExecContext &ctx)
{
  const BlockUse use = ctx.paramBlock("thing");
  const Thing *t = static_cast<const Thing *>(use.data);
  const double v = t ? t->value : (use.handle ? -3 : -2);
  return ctx.setOutput(0, GraphValue::ofFloat(v));
}

/* The sum of two blocks by id, whichever exist. It reads the second only if the first is
 * positive. */
Status readIds(NodeExecContext &ctx)
{
  const Thing *a = static_cast<const Thing *>(ctx.useBlock(g_use[0]).data);
  double v = a ? a->value : -100;
  if (v > 0) {
    const Thing *b = static_cast<const Thing *>(ctx.useBlock(g_use[1]).data);
    v += b ? b->value : -100;
  }
  if (g_notifyOther) {
    g_notifyOther = false;
    ctx.runtime().ext<ChangeBus>().notify(OwnerKey::ofBlock(g_use[0]));
  }
  return ctx.setOutput(0, GraphValue::ofFloat(v));
}

/* What the interface of a block says, or -1. */
Status readScale(NodeExecContext &ctx)
{
  const Scaled s = ctx.queryBlock<Scaled>(ctx.useBlock(g_use[0]).handle);
  return ctx.setOutput(0, GraphValue::ofFloat(s ? s.scale() : -1));
}

struct World {
  Runtime rt;
  Library lib;
  World() : lib(&rt)
  {
    addThings(rt);
    test_assert(registerGraphBlocks(rt).ok() && registerInterface<Scaled>(rt).ok());
    NodeTypeDef p("t.param");
    p.param(PropertyDef::pointerProp("thing", kThingStruct)).output("value", "float");
    addType<readParam>(rt, p, "t.param");
    NodeTypeDef i("t.ids");
    i.output("value", "float");
    addType<readIds>(rt, i, "t.ids");
    NodeTypeDef s("t.scale");
    s.output("value", "float");
    addType<readScale>(rt, s, "t.scale");
  }
  Thing &thing(BlockHandle h)
  {
    return *static_cast<Thing *>(lib.get(h)->clientData());
  }
  void notify(BlockId id)
  {
    rt.ext<ChangeBus>().notify(OwnerKey::ofBlock(id));
  }
};

PropValue pointerTo(BlockId id)
{
  StableRef r;
  r.owner = OwnerKey::ofBlock(id);
  r.type = kThingStruct;
  return PropValue::ofPointer(r);
}

/* Pulls a node's `value`, and says how many kernels ran. */
struct Puller {
  GraphInstance inst;
  SocketPath path;
  Puller(Runtime &rt, Library *lib, GraphDef *def, NodeId n)
      : inst(&rt, lib, def), path(SocketPath::of(n, "value"))
  {
  }
  bool is(double value, int runs)
  {
    const uint64_t before = inst.totalRuns();
    const Status s = inst.evaluate(path);
    const bool ok = s.ok() && *inst.value(path) == GraphValue::ofFloat(value) &&
                    int(inst.totalRuns() - before) == runs;
    if (!ok) {
      printf("%s, %g, %d runs\n", s.toString().c_str(),
             inst.value(path) ? inst.value(path)->asFloat() : 0,
             int(inst.totalRuns() - before));
    }
    return ok;
  }
};

void testParams()
{
  World w;
  GraphDef def(&w.rt, &w.lib);
  const BlockHandle h1 = newThing(w.lib, "1", 1), h2 = newThing(w.lib, "2", 2);
  const BlockId id1 = idOf(w.lib, h1), id2 = idOf(w.lib, h2);
  const NodeId n = add(def, "t.param");
  Puller p(w.rt, &w.lib, &def, n);
  test_assert(p.is(-2, 1) && w.rt.ext<GraphSystem>().blockCount() == 0); /* null: no dep */
  test_assert(def.setParam(n, "thing", pointerTo(id1)).ok() && p.is(1, 1) && p.is(1, 0));
  test_assert(w.rt.ext<GraphSystem>().blockCount() == 1);

  /* A change is seen when it is notified; one to another block isn't a reason to run. */
  w.thing(h1).value = 5;
  test_assert(p.is(1, 0));
  w.notify(id1);
  test_assert(!p.inst.isCurrent(p.path) && p.is(5, 1));
  const uint64_t edit = w.rt.lastEdit();
  w.notify(id2); /* nobody uses it: not even an edit */
  test_assert(w.rt.lastEdit() == edit && p.inst.isCurrent(p.path) && p.is(5, 0));
  w.rt.ext<ChangeBus>().notify(OwnerKey::ofRuntime(5));
  test_assert(p.is(5, 0));
  w.rt.ext<ChangeBus>().notify(OwnerKey::ofBlock(id1), "value");
  test_assert(p.is(5, 1));
  w.rt.ext<ChangeBus>().notifyAll();
  test_assert(p.is(5, 1));
  w.rt.ext<ChangeBus>().bumpEpoch();
  test_assert(p.is(5, 0));

  /* Two nodes, two blocks: one runs. */
  const NodeId m = add(def, "t.param");
  test_assert(def.setParam(m, "thing", pointerTo(id2)).ok());
  Puller q(w.rt, &w.lib, &def, m);
  test_assert(q.is(2, 1));
  w.thing(h2).value = 3;
  w.notify(id2);
  test_assert(p.is(5, 0) && q.is(3, 1));

  /* States: deleted, restored, detached, restored. */
  test_assert(w.lib.markDeleted(h1).ok() && p.is(-3, 1) && q.is(3, 0));
  test_assert(w.lib.restoreFromHistory(h1).ok() && p.is(5, 1));
  test_assert(w.lib.onHostDestroyed(id1).ok() && p.is(-3, 1));
  test_assert(w.lib.onHostRestored(id1).ok() && p.is(5, 1));
  /* Destroyed, and another under its id: another handle, though nothing was notified. */
  test_assert(w.lib.markDeleted(h1).ok() && w.lib.destroyFromHistory(h1).ok());
  test_assert(p.is(-2, 1));
  Thing *t = litestl::alloc::New<Thing>("test::Thing");
  t->value = 9;
  test_assert(w.lib.reviveAs(kThing, id1, "again", t).ok() && p.is(9, 1));

  /* A Library may write the param's cell, as a replace of one block by another does. */
  struct Redirect {
    Library *lib;
    BlockId to;
  } redirect{&w.lib, id2};
  RefVisitor v(
      [](void *d, RefCell &cell) {
        const Redirect &r = *static_cast<Redirect *>(d);
        test_assert(r.lib->writeRef(cell, r.to).ok());
      },
      &redirect);
  def.foreachRef(v);
  test_assert(p.is(3, 1) && q.is(3, 0));
  redirect.to = id1;
  def.foreachRef(v);
  test_assert(p.is(9, 1) && q.is(9, 1));
  test_assert(def.setParam(m, "thing", pointerTo(id2)).ok() && q.is(3, 1));

  /* An empty Library edit verifies, and runs nothing. */
  newThing(w.lib, "3", 3);
  test_assert(!p.inst.isCurrent(p.path) && p.is(9, 0) && q.is(3, 0));
}

void testIds()
{
  World w;
  GraphDef def(&w.rt, &w.lib);
  const NodeId n = add(def, "t.ids");
  const ClientId client = ClientId::of(kCoredccClient);
  g_use[0] = BlockId{client, LocalId{1, 1}};
  g_use[1] = BlockId{client, LocalId{2, 2}};
  Puller p(w.rt, &w.lib, &def, n);
  /* Absent blocks are dependencies too. */
  test_assert(p.is(-100, 1) && p.is(-100, 0));
  Thing *t = litestl::alloc::New<Thing>("test::Thing");
  t->value = -4;
  Result<BlockHandle> h = w.lib.createWithId(kThing, g_use[0], "0", t);
  test_assert(h.ok() && p.is(-4, 1));
  w.notify(g_use[1]); /* not read in the last run */
  test_assert(p.is(-4, 0));
  t->value = 4;
  w.notify(g_use[0]);
  test_assert(p.is(-96, 1)); /* now it is */
  Thing *u = litestl::alloc::New<Thing>("test::Thing");
  u->value = 10;
  Result<BlockHandle> h2 = w.lib.createWithId(kThing, g_use[1], "1", u);
  test_assert(h2.ok() && p.is(14, 1));
  w.notify(g_use[1]);
  test_assert(p.is(14, 1));

  /* A kernel's own notification: it runs again in the next call. */
  g_notifyOther = true;
  w.notify(g_use[0]);
  test_assert(p.is(14, 1) && !p.inst.isCurrent(p.path) && p.is(14, 1) && p.is(14, 0));

  /* A client that becomes an alias: the block has a new canonical id. */
  const BlockId old{ClientId::of("old"), LocalId{3, 3}};
  ClientRegistry &clients = w.rt.ext<ClientRegistry>();
  const char *aliases[] = {"old"};
  g_use[0] = BlockId{client, LocalId{3, 3}};
  g_use[1] = old;
  w.notify(BlockId{client, LocalId{1, 1}});
  test_assert(p.is(-100, 1));
  Thing *s = litestl::alloc::New<Thing>("test::Thing");
  s->value = 20;
  test_assert(w.lib.createWithId(kThing, g_use[0], "2", s).ok());
  test_assert(p.is(-80, 1)); /* 20 + absent */
  test_assert(clients.registerClient(kCoredccClient, aliases).ok());
  test_assert(p.is(40, 1));
  w.notify(old); /* changes of the alias are changes of the block */
  test_assert(p.is(40, 1));
  const BlockId now = g_use[0];
  g_use[0] = old;
  w.notify(now);
  test_assert(p.is(40, 1));
  w.notify(now); /* also for a kernel that only used the alias */
  test_assert(p.is(40, 1));
}

void testQueries()
{
  World w;
  GraphDef def(&w.rt, &w.lib);
  const NodeId n = add(def, "t.scale");
  const BlockHandle h = newThing(w.lib, "t", 2.5);
  g_use[0] = idOf(w.lib, h);
  Puller p(w.rt, &w.lib, &def, n);
  test_assert(p.is(-1, 1));
  /* An implementation registered late. */
  test_assert((registerImpl<Scaled, ThingScaled>(w.rt, "test.Thing").ok()));
  test_assert(p.is(2.5, 1) && p.is(2.5, 0));
  /* Unrelated registrations verify only. */
  test_assert((registerImpl<Scaled, ThingScaled>(w.rt, "test.Other").ok()));
  test_assert(p.is(2.5, 0));
  /* An override. (From P2 on, setting it moves the epoch; here, this does.) */
  ThingScaled mine{7};
  const Scaled ref = query<Scaled>(w.rt, TypeId::of("test.Other"), &mine);
  test_assert(w.lib.setOverride(h, InterfaceId::of(Scaled::ifaceName), ref).ok());
  w.rt.touch();
  test_assert(p.is(7, 1));
  ThingScaled yours{8};
  test_assert(w.lib
                  .setOverride(h, InterfaceId::of(Scaled::ifaceName),
                               query<Scaled>(w.rt, TypeId::of("test.Other"), &yours))
                  .ok());
  w.rt.touch();
  test_assert(p.is(8, 1)); /* the same table, another self */
  w.lib.clearOverride(h, InterfaceId::of(Scaled::ifaceName));
  w.rt.touch();
  test_assert(p.is(2.5, 1));
}

/* Instances and defs that come and go. */
void testLifetimes()
{
  World w;
  const Graph g = newGraph(w.lib, "g");
  const int64_t o = addSocket(*g.def, SocketSide::Output, "o", "float");
  const NodeId n = add(*g.def, "graph.output");
  test_assert(g.def->setLiteral(n, key(o), PropValue::ofFloat(3)).ok());
  GraphInstance i(&w.rt, &w.lib, g.id);
  const SocketPath p = SocketPath::output("o");
  test_assert(i.evaluate(p).ok() && *i.value(p) == GraphValue::ofFloat(3));
  test_assert(hasCode(i.setInput("x", GraphValue()), StatusCode::NotFound));

  /* The block's data is replaced: another GraphDef under the same id. */
  test_assert(w.lib.markDeleted(g.h).ok());
  litestl::util::Vector<Status> results;
  test_assert(hasCode(i.evaluate({&p, 1}, results), StatusCode::NotFound));
  test_assert(hasCode(i.setInput("x", GraphValue()), StatusCode::NotFound));
  /* Made before the old one is freed, so it can't be at the same address. */
  GraphDef *again = litestl::alloc::New<GraphDef>("coredcc::GraphDef", &w.rt, &w.lib, g.id);
  test_assert(w.lib.destroyFromHistory(g.h).ok());
  test_assert(hasCode(i.evaluate(p), StatusCode::NotFound));
  test_assert(*i.lastGood(p) == GraphValue::ofFloat(3) && !i.isCurrent(p));
  test_assert(w.lib.reviveAs(TypeId::of(kGraphDefType), g.id, "g", again).ok());
  test_assert(hasCode(i.evaluate(p), StatusCode::NotFound)); /* it has no such output */
  test_assert(again->addInterface(SocketSide::Output, "o", "float").ok());
  test_assert(i.evaluate(p).ok() && *i.value(p) == GraphValue::ofFloat(0));

  /* An instance without a Library, of a def that uses blocks: there are none. */
  GraphDef alone(&w.rt);
  const NodeId k = add(alone, "t.ids");
  Puller q(w.rt, nullptr, &alone, k);
  test_assert(q.is(-100, 1));
  w.rt.ext<ChangeBus>().notifyAll();
  test_assert(q.is(-100, 0));
  GraphInstance none(&w.rt, nullptr, g.id);
  test_assert(hasCode(none.evaluate(p), StatusCode::NotFound));
}

} // namespace

int main()
{
  testParams();
  testIds();
  testQueries();
  testLifetimes();
  return test_end();
}
