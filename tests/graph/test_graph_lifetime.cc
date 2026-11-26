/* Aliasing payloads, and graphs, instances and blocks that outlive or are outlived by
 * each other. */
#include "graph_fixture.h"

#include "coredcc/graph/instance.h"

#include <functional>
#include <memory>

using namespace fixture;

test_init;

namespace {

/* A view of a Thing. Two views of equal values are equal, unless they alias. */
struct View {
  const Thing *thing = nullptr;
};

} // namespace

template <> struct coredcc::PayloadTraits<View> {
  static constexpr const char *name = "test.View";
  static bool equal(const View &a, const View &b)
  {
    return a.thing->value == b.thing->value;
  }
  static size_t bytes(const View &v)
  {
    return size_t(v.thing->value);
  }
};

namespace {

std::function<void()> g_inKernel;

PayloadRef g_view;
Status sharedViewKernel(NodeExecContext &ctx)
{
  return ctx.setOutput(0, GraphValue::ofPayload(g_view));
}

/* A view of its param's block. */
Status viewKernel(NodeExecContext &ctx)
{
  const BlockUse use = ctx.paramBlock("thing");
  if (!use.data) {
    return Status::error(StatusCode::NotFound, "no thing");
  }
  Result<PayloadRef> p =
      ctx.alias(View{static_cast<const Thing *>(use.data)}, use.handle);
  return p.ok() ? ctx.setOutput(0, GraphValue::ofPayload(*p)) : p.status();
}

Status readKernel(NodeExecContext &ctx)
{
  if (g_inKernel) {
    g_inKernel();
  }
  const View *v = ctx.input(0).payload<View>();
  return ctx.setOutput(0, GraphValue::ofFloat(v ? v->thing->value : -1));
}

/* A block type whose provider calls back, and whose data may own an instance. */
struct Owner {
  std::unique_ptr<GraphInstance> inst;
};
std::function<void()> g_onForeach;
std::function<void()> g_onDestroy;
int g_ownersDestroyed = 0;

struct OwnerProvider {
  void *data(const Datablock &b) const
  {
    return b.clientData();
  }
  void forEachRef(const Datablock &, RefVisitor &) const
  {
    if (g_onForeach) {
      g_onForeach();
    }
  }
  void destroy(const Datablock &b)
  {
    g_ownersDestroyed++;
    if (g_onDestroy) {
      g_onDestroy();
    }
    litestl::alloc::Delete(static_cast<Owner *>(b.clientData()));
  }
  static lbt::Struct<OwnerProvider> *defineBindings()
  {
    auto *st =
        new lbt::Struct<OwnerProvider>("test::OwnerProvider", sizeof(OwnerProvider));
    BIND_STRUCT_METHOD(st, data, MARGS("block"));
    BIND_STRUCT_METHOD(st, forEachRef, MARGS("block", "visitor"));
    BIND_STRUCT_METHOD(st, destroy, MARGS("block"));
    return st;
  }
};
const TypeId kOwner = TypeId::of("test.Owner");
const TypeId kTransient = TypeId::of("test.Transient");

struct World {
  Runtime rt;
  std::unique_ptr<Library> lib;

  World() : lib(std::make_unique<Library>(&rt))
  {
    addThings(rt);
    test_assert(registerGraphBlocks(rt).ok());
    test_assert(
        (registerImpl<BlockProvider, OwnerProvider>(rt, "test.Owner#provider").ok()));
    for (const char *name : {"test.Owner", "test.Transient"}) {
      Result<Owned<BlockProvider>> p = own<BlockProvider>(
          rt, "test.Owner#provider", litestl::alloc::New<OwnerProvider>("provider"));
      BlockTypeInfo info;
      info.name = name;
      info.client = ClientId::of(kCoredccClient);
      info.dataStruct = StructId::of(name);
      if (strcmp(name, "test.Transient") == 0) {
        info.storage = BlockStorage::Transient;
      }
      test_assert(rt.ext<BlockTypeRegistry>().add(info, std::move(p).value()).ok());
    }
    test_assert(rt.ext<SocketTypeRegistry>().addPayloadType<View>().ok());
    NodeTypeDef v("t.view");
    v.param(PropertyDef::pointerProp("thing", kThingStruct)).output("view", "test.View");
    addType<viewKernel>(rt, v, "t.view");
    NodeTypeDef s("t.shared");
    s.output("view", "test.View");
    addType<sharedViewKernel>(rt, s, "t.shared");
    NodeTypeDef r("t.read");
    r.payloadInput("view", "test.View").output("value", "float");
    addType<readKernel>(rt, r, "t.read");
    lib->setQuarantine(true);
  }
  ~World()
  {
    test_assert(rt.validateAll().ok() && !rt.hasStickyError());
    test_assert(!lib || lib->validate().ok());
    g_inKernel = nullptr;
    g_onForeach = nullptr;
    g_onDestroy = nullptr;
  }
  Thing *thing(BlockHandle h)
  {
    return static_cast<Thing *>(lib->get(h)->clientData());
  }
  int pins(BlockHandle h)
  {
    return lib->get(h)->pins(PinKind::Host);
  }
  BlockHandle newOwner()
  {
    Result<BlockHandle> h =
        lib->create(kOwner, "owner", litestl::alloc::New<Owner>("test::Owner"));
    test_assert(h.ok());
    return *h;
  }
};

PropValue pointerTo(BlockId id)
{
  StableRef r;
  r.owner = OwnerKey::ofBlock(id);
  return PropValue::ofPointer(r);
}

SocketPath out(NodeId n, const char *socket = "value")
{
  return SocketPath::of(n, socket);
}

void testAlias()
{
  World w;
  Library &lib = *w.lib;
  const BlockHandle h = newThing(lib, "a", 3), h2 = newThing(lib, "b", 3);
  const BlockId id = idOf(lib, h);
  const View view{w.thing(h)};
  {
    Result<PayloadRef> p = makeAlias(view, lib, h);
    test_assert(p.ok() && p->isAlias() && w.pins(h) == 1);
    PayloadRef copy = *p;
    test_assert(copy.get<View>()->thing == view.thing && copy.same(*p) && w.pins(h) == 1);
    /* Nobody may write the user's data through it. */
    test_assert(!copy.mutate<View>() && copy.same(*p));
    *p = PayloadRef();
    test_assert(w.pins(h) == 1 && !copy.mutate<View>());
    test_assert(copy.bytes() == 3);
    /* Equal by identity only, even though View has an `equal`. */
    Result<PayloadRef> twin = makeAlias(view, lib, h);
    test_assert(!(copy == *twin) && copy == PayloadRef(copy) && w.pins(h) == 2);
    const PayloadRef plain = makePayload(view);
    test_assert(!plain.isAlias() && !(copy == plain) && !(plain == copy));
    test_assert(plain == makePayload(view));
    /* Two blocks. */
    const BlockHandle both[] = {h, h2};
    Result<PayloadRef> two = makeAlias(view, lib, both);
    test_assert(two.ok() && w.pins(h) == 3 && w.pins(h2) == 1);
    test_assert(lib.onHostDestroyed(idOf(lib, h2)).ok());
    test_assert(!two->get<View>() && copy.get<View>());
    test_assert(lib.onHostRestored(idOf(lib, h2)).ok() && two->get<View>());
  }
  test_assert(w.pins(h) == 0 && w.pins(h2) == 0);

  /* Errors leave no pins. */
  const BlockHandle bad[] = {h, BlockHandle()};
  test_assert(hasCode(makeAlias(view, lib, bad), StatusCode::NotFound) && w.pins(h) == 0);
  test_assert(hasCode(makeAlias(view, lib, std::span<const BlockHandle>()),
                      StatusCode::InvalidArgument));
  test_assert(lib.markDeleted(h2).ok());
  test_assert(hasCode(makeAlias(view, lib, h2), StatusCode::FailedPrecondition));
  Result<BlockHandle> t =
      lib.create(kTransient, "t", litestl::alloc::New<Owner>("test::Owner"));
  test_assert(hasCode(makeAlias(view, lib, *t), StatusCode::Unsupported));

  /* Deleted, dropped from the history while aliased, and its id taken by another. */
  Result<PayloadRef> p = makeAlias(view, lib, h);
  test_assert(lib.markDeleted(h).ok() && p->get<View>()->thing->value == 3);
  test_assert(lib.destroyFromHistory(h).ok() && !lib.find(id));
  test_assert(lib.get(h)->state() == BlockState::Doomed);
  Thing *fresh = litestl::alloc::New<Thing>("test::Thing");
  fresh->value = 8;
  Result<BlockHandle> again = lib.reviveAs(kThing, id, "again", fresh);
  test_assert(again.ok() && !(*again == h) && p->get<View>()->thing->value == 3);
  test_assert(w.pins(*again) == 0 && lib.validate().ok());
  *p = PayloadRef();
  test_assert(!lib.get(h) && lib.get(*again));
  test_assert(lib.collect().ok());

  /* Detached: the host freed the data. */
  Result<BlockHandle> d =
      lib.create(kThing, "d", litestl::alloc::New<Thing>("test::Thing"));
  Result<PayloadRef> q = makeAlias(View{w.thing(*d)}, lib, *d);
  test_assert(q.ok() && lib.onHostDestroyed(idOf(lib, *d)).ok());
  test_assert(!q->get<View>() && !q->mutate<View>() && q->bytes() == sizeof(View));
  test_assert(!(*q == makePayload(View{})) && *q == PayloadRef(*q) && q->type());
  test_assert(lib.onHostRestored(idOf(lib, *d)).ok() && q->get<View>());

  /* After the Library. */
  w.lib = nullptr;
  test_assert(!q->get<View>() && q->bytes() == sizeof(View));
  *q = PayloadRef();
}

void testAliasInGraph()
{
  World w;
  Library &lib = *w.lib;
  const BlockHandle h = newThing(lib, "a", 3);
  const BlockId id = idOf(lib, h);
  GraphDef def(&w.rt, &lib);
  const NodeId v = add(def, "t.view"), r = add(def, "t.read");
  connect(def, v, "view", r, "view");
  test_assert(def.setParam(v, "thing", pointerTo(id)).ok());
  GraphInstance inst(&w.rt, &lib, &def);
  auto is = [&](double value, int runs) {
    const uint64_t before = inst.totalRuns();
    const Status s = inst.evaluate(out(r));
    return s.ok() && *inst.value(out(r)) == GraphValue::ofFloat(value) &&
           int(inst.totalRuns() - before) == runs;
  };
  test_assert(is(3, 2) && is(3, 0) && w.pins(h) == 1);
  /* Edited in place: the new view reads the same memory as the old one, and still the
   * reader runs. */
  w.thing(h)->value = 4;
  w.rt.ext<ChangeBus>().notify(OwnerKey::ofBlock(id));
  test_assert(is(4, 2) && w.pins(h) == 1);
  /* Pinned: an unused block survives a collection, and a delete. */
  test_assert(lib.collect().ok() && lib.get(h));
  test_assert(inst.evaluate(out(v, "view")).ok());
  const PayloadRef kept = inst.value(out(v, "view"))->asPayload();
  test_assert(lib.markDeleted(h).ok() && lib.destroyFromHistory(h).ok());
  test_assert(kept.get<View>()->thing->value == 4);
  test_assert(hasCode(inst.evaluate(out(r)), StatusCode::NotFound));
  /* lastGood still has it, until clear. */
  test_assert(inst.lastGood(out(v, "view"))->payload<View>()->thing->value == 4);
  test_assert(inst.clear().ok() && kept.get<View>() && lib.get(h));
}

void testInstances()
{
  /* An instance and a standalone def after their Library. */
  {
    World w;
    const Graph g = makeGraph(*w.lib, "g");
    const int64_t o = addSocket(*g.def, SocketSide::Output, "o", "float");
    const NodeId n = add(*g.def, "graph.output");
    test_assert(g.def->setLiteral(n, key(o), PropValue::ofFloat(3)).ok());
    GraphInstance inst(&w.rt, w.lib.get(), g.id);
    test_assert(inst.evaluate(SocketPath::output("o")).ok());
    GraphDef alone(&w.rt, w.lib.get());
    const NodeId v = add(alone, "t.view"), r = add(alone, "t.read");
    const NodeId group = add(alone, "graph.group");
    test_assert(alone.setGroup(group, g.id).ok());
    connect(alone, v, "view", r, "view");
    const BlockHandle h = newThing(*w.lib, "a", 3);
    test_assert(alone.setParam(v, "thing", pointerTo(idOf(*w.lib, h))).ok());
    GraphInstance other(&w.rt, w.lib.get(), &alone);
    test_assert(other.evaluate(out(r)).ok() &&
                other.evaluate(out(group, key(o).c_str())).ok());
    test_assert(alone.library() == w.lib.get());
    w.lib = nullptr;
    test_assert(!alone.library());
    test_assert(hasCode(inst.evaluate(SocketPath::output("o")), StatusCode::NotFound));
    test_assert(*inst.lastGood(SocketPath::output("o")) == GraphValue::ofFloat(3));
    test_assert(hasCode(other.evaluate(out(r)), StatusCode::NotFound));
    test_assert(
        hasCode(other.evaluate(out(group, key(o).c_str())), StatusCode::NotFound));
    test_assert(alone.setLabel(r, "x").ok() && alone.removeNode(v).ok());
    test_assert(alone.validate().ok());
  }
  /* The host destroys and restores a block between two pulls. */
  {
    World w;
    const BlockHandle h = newThing(*w.lib, "a", 3);
    GraphDef def(&w.rt, w.lib.get());
    const NodeId v = add(def, "t.view");
    test_assert(def.setParam(v, "thing", pointerTo(idOf(*w.lib, h))).ok());
    GraphInstance inst(&w.rt, w.lib.get(), &def);
    test_assert(inst.evaluate(out(v, "view")).ok() && inst.totalRuns() == 1);
    test_assert(w.lib->onHostDestroyed(idOf(*w.lib, h)).ok());
    test_assert(w.lib->onHostRestored(idOf(*w.lib, h)).ok());
    test_assert(inst.evaluate(out(v, "view")).ok() && inst.totalRuns() == 2);
    w.rt.touch();
    test_assert(inst.evaluate(out(v, "view")).ok() && inst.totalRuns() == 2);
  }
  /* Nothing is evaluated while the Library collects. */
  {
    World w;
    const BlockHandle h = w.newOwner();
    w.lib->setFlag(h, BlockFlags::Root, true);
    GraphDef def(&w.rt, w.lib.get());
    const NodeId r = add(def, "t.read");
    GraphInstance inst(&w.rt, w.lib.get(), &def);
    int calls = 0;
    g_onForeach = [&]() {
      calls++;
      test_assert(hasCode(inst.evaluate(out(r)), StatusCode::Busy));
    };
    test_assert(w.lib->collect().ok() && calls > 0);
    g_onForeach = nullptr;
    test_assert(inst.evaluate(out(r)).ok());
  }
}

/* A kernel that destroys the graphs being evaluated. */
void testDestroyedMidWalk()
{
  World w;
  Library &lib = *w.lib;
  const Graph inner = makeGraph(lib, "inner"), outer = makeGraph(lib, "outer");
  const int64_t o = addSocket(*inner.def, SocketSide::Output, "o", "float");
  const NodeId r = add(*inner.def, "t.read"), iout = add(*inner.def, "graph.output");
  connect(*inner.def, r, "value", iout, key(o));
  const NodeId g1 = add(*outer.def, "graph.group"), g2 = add(*outer.def, "graph.group");
  test_assert(outer.def->setGroup(g1, inner.id).ok() &&
              outer.def->setGroup(g2, inner.id).ok());
  GraphInstance inst(&w.rt, &lib, outer.id);
  int calls = 0;
  g_inKernel = [&]() {
    if (calls++ == 0) {
      test_assert(w.pins(outer.h) == 1 && w.pins(inner.h) >= 1);
      for (const Graph &g : {outer, inner}) {
        test_assert(lib.markDeleted(g.h).ok() && lib.destroyFromHistory(g.h).ok());
        test_assert(lib.get(g.h) && lib.get(g.h)->state() == BlockState::Doomed);
      }
    }
  };
  const SocketPath paths[] = {out(g1, key(o).c_str()), out(g2, key(o).c_str())};
  litestl::util::Vector<Status> results;
  test_assert(inst.evaluate(paths, results).ok() && results[0].ok());
  test_assert(calls == 1 && hasCode(results[1], StatusCode::NotFound));
  /* Freed with the call. */
  test_assert(!lib.get(outer.h) && !lib.get(inner.h) && !lib.find(outer.id));
  test_assert(hasCode(inst.evaluate(paths, results), StatusCode::NotFound));
  test_assert(inst.value(paths[0]) && inst.report(NodePath()).runs == 0);

  /* A memoized group is pinned by a later call too, which walks on inside it. */
  calls = 1;
  const Graph in2 = makeGraph(lib, "in2"), out2 = makeGraph(lib, "out2");
  const int64_t o1 = addSocket(*in2.def, SocketSide::Output, "o1", "float");
  const int64_t o2 = addSocket(*in2.def, SocketSide::Output, "o2", "float");
  const NodeId a = add(*in2.def, "t.read"), b = add(*in2.def, "t.read");
  const NodeId io = add(*in2.def, "graph.output"), g = add(*out2.def, "graph.group");
  connect(*in2.def, a, "value", io, key(o1));
  connect(*in2.def, b, "value", io, key(o2));
  test_assert(out2.def->setGroup(g, in2.id).ok());
  GraphInstance j(&w.rt, &lib, out2.id);
  test_assert(j.evaluate(out(g, key(o1).c_str())).ok() && w.pins(in2.h) == 0);
  g_inKernel = [&]() {
    test_assert(w.pins(in2.h) == 1 && w.pins(out2.h) == 1 && ++calls);
  };
  const int before = calls;
  test_assert(j.evaluate(out(g, key(o2).c_str())).ok() && calls == before + 1);
  test_assert(w.pins(in2.h) == 0 && w.pins(out2.h) == 0);
  /* Once per group and call, however often it is read. */
  test_assert(j.clear().ok());
  const SocketPath both[] = {out(g, key(o1).c_str()), out(g, key(o2).c_str())};
  test_assert(j.evaluate(both, results).ok() && calls == before + 3);

  /* Also when it goes straight to a vertex it knows, for a prepared socket. */
  const BlockHandle thing = newThing(lib, "thing", 3);
  const NodeId v = add(*in2.def, "t.view");
  test_assert(in2.def->setParam(v, "thing", pointerTo(idOf(lib, thing))).ok());
  connect(*in2.def, v, "view", a, "view");
  const SocketHandle handle = *j.prepare(both[0]);
  litestl::util::Vector<int> moved;
  test_assert(j.evaluate({&handle, 1}, moved).ok() && calls == before + 4);
  static_cast<Thing *>(lib.data(thing))->value = 4;
  w.rt.ext<ChangeBus>().notify(OwnerKey::ofBlock(idOf(lib, thing)));
  const uint64_t full = j.totalFullPasses();
  test_assert(j.evaluate({&handle, 1}, moved).ok() && calls == before + 5);
  test_assert(j.totalFullPasses() == full && moved.size() == 1 && w.pins(in2.h) == 0);
}

/* A block owns an instance whose payload aliases that very block. */
void testOwnPin()
{
  World w;
  Library &lib = *w.lib;
  const BlockHandle h = newThing(lib, "a", 3), o = w.newOwner();
  GraphDef def(&w.rt, &lib);
  const NodeId v = add(def, "t.view"), r = add(def, "t.read");
  connect(def, v, "view", r, "view");
  test_assert(def.setParam(v, "thing", pointerTo(idOf(lib, h))).ok());
  GraphInstance *inst = new GraphInstance(&w.rt, &lib, &def);
  static_cast<Owner *>(lib.get(o)->clientData())->inst.reset(inst);
  PayloadRef self;
  g_inKernel = [&]() {
    Result<PayloadRef> p = makeAlias(View{w.thing(h)}, lib, o);
    test_assert(p.ok());
    self = *p;
  };
  test_assert(inst->evaluate(out(r)).ok());
  g_inKernel = nullptr;
  /* Through an input, the instance keeps it. */
  addSocket(def, SocketSide::Input, "in", "test.View");
  const NodeId in = add(def, "graph.input");
  connect(def, in, def.interfaceSockets(SocketSide::Input)[0].key().c_str(), r, "view");
  test_assert(inst->setInput("in", GraphValue::ofPayload(self)).ok());
  test_assert(inst->evaluate(SocketPath::of(r, "view", SocketSide::Input)).ok());
  test_assert(inst->setInput("in", GraphValue()).ok());
  self = PayloadRef();
  test_assert(w.pins(o) >= 1);
  g_ownersDestroyed = 0;
  test_assert(lib.collect().ok() && g_ownersDestroyed == 0);
  test_assert(lib.markDeleted(o).ok() && lib.destroyFromHistory(o).ok());
  test_assert(lib.get(o) && lib.get(o)->state() == BlockState::Doomed);
  test_assert(inst->clear().ok()); /* frees the block, and the instance with it */
  test_assert(!lib.get(o) && g_ownersDestroyed == 1);
}

/* What a removed node referred to is collected, and the relations cache is right. */
void testCollect()
{
  World w;
  Library &lib = *w.lib;
  lib.setAuditRelations(true);
  const Graph outer = makeGraph(lib, "outer"), inner = makeGraph(lib, "inner");
  const BlockHandle h = newThing(lib, "a", 3);
  lib.setFlag(outer.h, BlockFlags::Root, true);
  const NodeId g = add(*outer.def, "graph.group"), v = add(*outer.def, "t.view");
  test_assert(outer.def->setGroup(g, inner.id).ok());
  test_assert(outer.def->setParam(v, "thing", pointerTo(idOf(lib, h))).ok());
  GraphInstance inst(&w.rt, &lib, outer.id);
  test_assert(inst.evaluate(out(v, "view")).ok());
  test_assert(lib.usersOf(inner.h).size() == 1 && *lib.collect() == 0);
  const NodeState state = outer.def->stateOf(*outer.def->node(g));
  test_assert(outer.def->removeNode(g).ok() && lib.usersOf(inner.h).size() == 0);
  test_assert(outer.def->insertNode(state).ok() && lib.usersOf(inner.h).size() == 1);
  test_assert(*lib.collect() == 0 && outer.def->removeNode(g).ok() &&
              *lib.collect() == 1);
  test_assert(!lib.get(inner.h));
  /* The payload's pin goes only when the node has run without the block. */
  test_assert(outer.def->removeNode(v).ok() && *lib.collect() == 0);
  test_assert(inst.clear().ok() && *lib.collect() == 1 && !lib.get(h));
}

/* The same, where the pin goes because the graph was replaced: in `evaluate` itself. */
void testOwnPinReplaced()
{
  for (const bool gone : {false, true}) {
    World w;
    Library &lib = *w.lib;
    const BlockHandle h = newThing(lib, "a", 3), o = w.newOwner();
    const Graph g = makeGraph(lib, "g");
    const NodeId s = add(*g.def, "t.shared"), r = add(*g.def, "t.read");
    connect(*g.def, s, "view", r, "view");
    GraphInstance *inst = new GraphInstance(&w.rt, &lib, g.id);
    static_cast<Owner *>(lib.get(o)->clientData())->inst.reset(inst);
    g_view = *makeAlias(View{w.thing(h)}, lib, o);
    test_assert(inst->evaluate(out(r)).ok());
    g_view = PayloadRef();
    test_assert(w.pins(o) == 1 && lib.markDeleted(o).ok() &&
                lib.destroyFromHistory(o).ok());
    test_assert(lib.markDeleted(g.h).ok() && lib.destroyFromHistory(g.h).ok());
    GraphDef *fresh =
        litestl::alloc::New<GraphDef>("coredcc::GraphDef", &w.rt, &lib, g.id);
    const NodeId other = add(*fresh, "t.read");
    test_assert(lib.reviveAs(TypeId::of(kGraphDefType), g.id, "g", fresh).ok());
    bool inside = false;
    int calls = 0;
    g_inKernel = [&]() {
      calls++;
      test_assert(lib.get(o)); /* not yet */
    };
    g_ownersDestroyed = 0;
    g_onDestroy = [&]() { inside = !w.rt.busyIfEvaluating("test").ok(); };
    const Status st = inst->evaluate(gone ? out(r) : out(other)); /* frees the instance */
    test_assert(gone ? hasCode(st, StatusCode::NotFound) : st.ok() && calls == 1);
    test_assert(!lib.get(o) && g_ownersDestroyed == 1 && !inside);
  }
}

/* Destroying an instance lets go of the last pins of blocks: they are destroyed when all
 * of the instance is. */
void testInstancePins()
{
  for (int which = 0; which < 3; which++) {
    World w;
    Library &lib = *w.lib;
    const BlockHandle h = newThing(lib, "a", 3);
    const BlockHandle owners[2] = {w.newOwner(), w.newOwner()};
    GraphDef def(&w.rt, &lib);
    const int64_t in = addSocket(def, SocketSide::Input, "in", "test.View");
    const NodeId s = add(def, "t.shared"), r = add(def, "t.read"),
                 io = add(def, "graph.input");
    connect(def, s, "view", r, "view");
    auto inst = std::make_unique<GraphInstance>(&w.rt, &lib, &def);
    /* In two of: a kernel's outputs, the results, an input. */
    g_view = *makeAlias(View{w.thing(h)}, lib, owners[0]);
    if (which != 2) {
      test_assert(inst->evaluate(out(r)).ok());
    }
    if (which != 1) {
      g_view = *makeAlias(View{w.thing(h)}, lib, owners[which == 0 ? 1 : 0]);
      test_assert(inst->setInput("in", GraphValue::ofPayload(g_view)).ok());
    }
    if (which != 0) {
      test_assert(inst->setInput("in", GraphValue()).ok());
      g_view = *makeAlias(View{w.thing(h)}, lib, owners[1]);
      test_assert(inst->setInput("in", GraphValue::ofPayload(g_view)).ok());
      test_assert(inst->evaluate(out(io, key(in).c_str())).ok());
      if (which == 2) {
        g_view = *makeAlias(View{w.thing(h)}, lib, owners[0]);
        test_assert(inst->evaluate(out(s, "view")).ok());
      }
    }
    g_view = PayloadRef();
    for (BlockHandle o : owners) {
      test_assert(w.pins(o) >= 1 && lib.markDeleted(o).ok() &&
                  lib.destroyFromHistory(o).ok());
      test_assert(lib.get(o));
    }
    g_ownersDestroyed = 0;
    g_onDestroy = [&]() {
      for (BlockHandle o : owners) {
        test_assert(!lib.get(o) || w.pins(o) == 0);
      }
    };
    inst = nullptr;
    test_assert(g_ownersDestroyed == 2);
  }
}

} // namespace

int main()
{
  testAlias();
  testAliasInGraph();
  testInstances();
  testDestroyedMidWalk();
  testOwnPin();
  testOwnPinReplaced();
  testInstancePins();
  testCollect();
  return test_end();
}
