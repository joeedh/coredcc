/*
 * A stand-in for the scene evaluator (P5c): it builds a standalone graph with a transform
 * node per object, and links parents, drivers and users' graphs. It uses only what the
 * plan says P5c may ("What P5c (the scene evaluator) uses").
 */
#include "edit_fixture.h"

#include "coredcc/graph/instance.h"
#include "coredcc/graph/interior.h"

#include <map>
#include <string>
#include <vector>

using namespace fixture;

test_init;

namespace {

/* A one dimensional transform, carried as a payload. */
struct Place {
  double x = 0;
  double scale = 1;
};

/* What objects (things) implement. */
struct Placed : IfaceRef {
  double localX() const
  {
    return CDCC_CALL(localX);
  }
  CDCC_INTERFACE(Placed, "test::Placed")
  {
    BIND_STRUCT_METHOD(st, localX, MARGS());
  }
};
struct ThingPlaced {
  double value = 0; /* Thing's */
  double localX() const
  {
    return value;
  }
  static lbt::Struct<ThingPlaced> *defineBindings()
  {
    auto *st = new lbt::Struct<ThingPlaced>("test::ThingPlaced", sizeof(ThingPlaced));
    BIND_STRUCT_METHOD(st, localX, MARGS());
    return st;
  }
};

} // namespace

template <> struct coredcc::PayloadTraits<Place> {
  static constexpr const char *name = "test.Place";
  static bool equal(const Place &a, const Place &b)
  {
    return a.x == b.x && a.scale == b.scale;
  }
};

namespace {

struct EvalStats {
  int transforms = 0;
};

Place placeOf(const GraphValue &v)
{
  const Place *p = v.payload<Place>();
  return p ? *p : Place();
}

/* parent, offset, object -> place. Fails if the object isn't there, or is at 666. */
Status transformKernel(NodeExecContext &ctx)
{
  static_cast<EvalStats *>(ctx.userData())->transforms++;
  const BlockUse use = ctx.paramBlock("object");
  const Placed object = ctx.queryBlock<Placed>(use.handle);
  if (!use.data || !object || !ctx.library()) {
    return Status::error(StatusCode::NotFound, "the object isn't there");
  }
  const double local = object.localX() + ctx.input("offset")->asFloat();
  if (local == 666) {
    return Status::error(StatusCode::OutOfRange, "singular");
  }
  const Place parent = placeOf(*ctx.input("parent"));
  return ctx.setOutput(
      0, GraphValue::ofPayload(makePayload(Place{parent.x + local, parent.scale})));
}

/* A driver's value: time times the object's x. */
Status driverKernel(NodeExecContext &ctx)
{
  const Thing *t = static_cast<const Thing *>(ctx.paramBlock("object").data);
  return ctx.setOutput(
      0, GraphValue::ofFloat(t ? t->value * ctx.input("time")->asFloat() : 0));
}

/* In users' graphs: the place of an object, as the scene gives it; and the offset of one.
 */
Status getPlaceKernel(NodeExecContext &ctx)
{
  return ctx.setOutput(0, GraphValue::ofFloat(placeOf(ctx.input(0)).x));
}
Status setOffsetKernel(NodeExecContext &ctx)
{
  return ctx.setOutput(0, ctx.input(0));
}

constexpr const char *kTransform = "scene.transform";
constexpr const char *kSetOffset = "scene.setOffset";
constexpr const char *kGetPlace = "scene.getPlace";

void addSceneTypes(Runtime &rt)
{
  test_assert(registerInterface<Placed>(rt).ok());
  test_assert((registerImpl<Placed, ThingPlaced>(rt, "test.Thing").ok()));
  test_assert(rt.ext<SocketTypeRegistry>().addPayloadType<Place>().ok());
  const PropertyDef object = PropertyDef::pointerProp("object", kThingStruct);
  NodeTypeDef t(kTransform);
  t.payloadInput("parent", "test.Place")
      .input(PropertyDef::floatProp("offset"))
      .param(object, true)
      .output("place", "test.Place");
  addType<transformKernel>(rt, t, kTransform);
  NodeTypeDef d("scene.driver");
  d.input(PropertyDef::floatProp("time")).param(object, true).output("value", "float");
  addType<driverKernel>(rt, d, "scene.driver");
  NodeTypeDef g(kGetPlace);
  g.payloadInput("place", "test.Place").param(object).output("x", "float");
  addType<getPlaceKernel>(rt, g, kGetPlace);
  NodeTypeDef s(kSetOffset);
  s.flags(NodeTypeFlags::OutputAdapter)
      .input(PropertyDef::floatProp("offset"))
      .param(object)
      .output("offset", "float");
  NodeTypeDef copy = s.copy();
  addType<setOffsetKernel>(rt, copy, kSetOffset);
}

/* ---- the evaluator ---- */

class SceneEvaluator {
public:
  SceneEvaluator(Runtime &rt, Library &lib)
      : lib_(lib), def(&rt, &lib), inst(&rt, &lib, &def)
  {
    time_ = addSocket(def, SocketSide::Input, "time", "float");
    input_ = add(def, "graph.input");
    inst.setUserData(&stats);
  }

  /* Edits of the scene, each a few edits of the graph. */

  void addObject(BlockHandle h)
  {
    const NodeId n = add(def, kTransform);
    test_assert(def.setParam(n, "object", pointerTo(idOf(lib_, h), kThingStruct)).ok());
    nodes_[idOf(lib_, h)] = n;
  }
  void removeObject(BlockId id)
  {
    test_assert(def.removeNode(nodes_[id]).ok());
    nodes_.erase(id);
    applied.erase(id);
  }
  Status setParent(BlockId child, BlockId parent)
  {
    if (parent.isNull()) {
      const GraphLink *l = def.node(nodes_[child])->linkInto("parent");
      return l ? def.unlink(l->id) : Status();
    }
    return def.link({nodes_[parent], "place"}, {nodes_[child], "parent"}).status();
  }
  void addDriver(BlockId target, BlockId source)
  {
    const NodeId n = add(def, "scene.driver");
    test_assert(def.setParam(n, "object", pointerTo(source, kThingStruct)).ok());
    connect(def, input_, key(time_), n, "time");
    connect(def, n, "value", nodes_[target], "offset");
  }
  /* A user's graph: its adapters are linked to the transforms of their objects. */
  Status addGraph(BlockId graph, NodeId &group)
  {
    group = add(def, "graph.group");
    CDCC_TRY(def.setGroup(group, graph));
    const GraphDef &user = *def.groupTarget(*def.node(group));
    Status status;
    auto connectAdapter = [&](const GraphNode &n) {
      const bool writes = strcmp(n.typeName().c_str(), kSetOffset) == 0;
      const auto object = nodes_.find(n.blockTarget("object"));
      if (object == nodes_.end() || !status.ok()) {
        return;
      }
      const std::string socket =
          "@" + std::to_string(n.id().value) + (writes ? ".offset" : ".place");
      const SocketRef mine{object->second, writes ? "offset" : "place"},
          theirs{group, socket};
      status = (writes ? def.link(theirs, mine) : def.link(mine, theirs)).status();
    };
    user.forEachNode(NodeTypeFlags::OutputAdapter, connectAdapter);
    user.forEachNode([&](const GraphNode &n) {
      if (strcmp(n.typeName().c_str(), kGetPlace) == 0) {
        connectAdapter(n);
      }
    });
    return status;
  }

  /* One batched evaluation, and what it gives to the objects that moved. */
  int apply()
  {
    std::vector<SocketPath> sockets;
    std::vector<BlockId> ids;
    for (const auto &[id, node] : nodes_) {
      ids.push_back(id);
      sockets.push_back(SocketPath::of(node, "place"));
    }
    litestl::util::Vector<Status> results;
    test_assert(inst.evaluate(sockets, results).ok() && results.size() == sockets.size());
    int moved = 0;
    errors = 0;
    for (size_t i = 0; i < ids.size(); i++) {
      errors += !results[int(i)].ok();
      const uint64_t version = inst.changedAt(sockets[i]);
      Applied &a = applied[ids[i]];
      if (a.version == version) {
        continue;
      }
      a.version = version;
      moved++;
      const GraphValue *v = inst.value(sockets[i]);
      a.stale = !v;
      v = v ? v : inst.lastGood(sockets[i]);
      a.x = v ? placeOf(*v).x : 0;
    }
    return moved;
  }
  SocketPath placePath(BlockId id)
  {
    return SocketPath::of(nodes_[id], "place");
  }

private:
  Library &lib_;
  std::map<BlockId, NodeId> nodes_;
  int64_t time_ = 0;
  NodeId input_;

public:
  struct Applied {
    double x = 0;
    bool stale = false;
    uint64_t version = 0;
  };
  GraphDef def;
  GraphInstance inst;
  EvalStats stats;
  std::map<BlockId, Applied> applied;
  int errors = 0;
};

struct Stage {
  Runtime rt;
  Library lib;
  std::vector<BlockId> ids;
  std::unique_ptr<SceneEvaluator> scene;
  Stage() : lib(&rt)
  {
    addThings(rt);
    test_assert(registerGraphBlocks(rt).ok());
    addSceneTypes(rt);
    lib.setQuarantine(true);
    scene = std::make_unique<SceneEvaluator>(rt, lib);
  }
  ~Stage()
  {
    scene = nullptr;
    test_assert(rt.validateAll().ok() && lib.validate().ok());
  }
  BlockId object(double x)
  {
    const BlockHandle h = newThing(lib, "o", x);
    lib.setFlag(h, BlockFlags::Root, true); /* the scene would keep it */
    ids.push_back(idOf(lib, h));
    scene->addObject(h);
    return ids.back();
  }
  void set(BlockId id, double x)
  {
    static_cast<Thing *>(lib.get(lib.find(id))->clientData())->value = x;
    rt.ext<ChangeBus>().notify(OwnerKey::ofBlock(id));
  }
  double x(BlockId id)
  {
    return scene->applied[id].x;
  }
  int transforms()
  {
    const int n = scene->stats.transforms;
    scene->stats.transforms = 0;
    return n;
  }
};

void testScene()
{
  Stage s;
  SceneEvaluator &e = *s.scene;
  const BlockId a = s.object(1), b = s.object(10), c = s.object(100), d = s.object(5);
  test_assert(e.setParent(b, a).ok() && e.setParent(c, b).ok());
  test_assert(hasCode(e.setParent(a, c), StatusCode::InvalidArgument)); /* a cycle */
  test_assert(e.apply() == 4 && s.transforms() == 4);
  test_assert(s.x(a) == 1 && s.x(b) == 11 && s.x(c) == 111 && s.x(d) == 5);
  test_assert(e.apply() == 0 && s.transforms() == 0);
  s.set(b, 20);
  test_assert(e.apply() == 2 && s.transforms() == 2 && s.x(c) == 121);
  s.set(b, 20); /* notified, unchanged: it runs, nothing after it does */
  test_assert(e.apply() == 0 && s.transforms() == 1);
  test_assert(e.setParent(c, d).ok());
  test_assert(e.apply() == 1 && s.transforms() == 1 && s.x(c) == 105);
  test_assert(e.setParent(c, BlockId()).ok() && e.apply() == 1 && s.x(c) == 100);
  test_assert(e.setParent(c, b).ok() && e.apply() == 1 && s.x(c) == 121);

  /* A driver, with time. */
  e.addDriver(d, a);
  test_assert(e.apply() == 0 && e.inst.setInput("time", GraphValue::ofFloat(3)).ok());
  test_assert(e.apply() == 1 && s.x(d) == 8); /* 5 + 1 * 3 */
  test_assert(e.inst.setInput("time", GraphValue::ofFloat(4)).ok());
  s.transforms();
  test_assert(e.apply() == 1 && s.transforms() == 1 && s.x(d) == 9);

  /* An error: what depends on it keeps its last place, the others move on. */
  s.set(a, 666);
  test_assert(e.apply() == 4 && e.errors == 3); /* a, b and c fail; d is driven by a */
  test_assert(s.x(c) == 121 && e.applied[c].stale && s.x(d) == 5 + 666 * 4 &&
              !e.applied[d].stale);
  test_assert(e.inst.error(e.placePath(c))->origin ==
              e.inst.error(e.placePath(a))->origin);
  s.set(a, 2);
  test_assert(e.apply() == 4 && e.errors == 0 && s.x(c) == 122 && !e.applied[c].stale);

  /* An object is deleted, and comes back; then it is removed from the scene. */
  test_assert(s.lib.markDeleted(s.lib.find(d)).ok() && e.apply() == 1 && e.errors == 1);
  test_assert(s.lib.restoreFromHistory(s.lib.find(d)).ok() && e.apply() == 1 &&
              s.x(d) == 13);
  test_assert(e.def.node(NodeId{0}) == nullptr);
  e.removeObject(d);
  s.transforms();
  test_assert(e.apply() == 0 && s.transforms() == 0);
  /* The graph's references keep nothing alive. */
  s.lib.setFlag(s.lib.find(d), BlockFlags::Root, false);
  s.lib.setFlag(s.lib.find(c), BlockFlags::Root, false);
  test_assert(s.lib.collect().ok());
  test_assert(s.lib.find(d).isNull() && s.lib.find(c).isNull());
  test_assert(e.apply() == 1 && e.errors == 1 && e.applied[c].stale);
}

void testUserGraphs()
{
  Stage s;
  SceneEvaluator &e = *s.scene;
  const BlockId a = s.object(1), b = s.object(10), c = s.object(100);
  test_assert(e.setParent(b, a).ok());
  /* A user's graph: c's offset is twice b's place. */
  const Graph user = makeGraph(s.lib, "user");
  s.lib.setFlag(user.h, BlockFlags::Root, true);
  const NodeId get = add(*user.def, kGetPlace), set = add(*user.def, kSetOffset);
  const NodeId twice = add(*user.def, "math.float");
  test_assert(user.def->setParam(get, "object", pointerTo(b, kThingStruct)).ok());
  test_assert(user.def->setParam(set, "object", pointerTo(c, kThingStruct)).ok());
  connect(*user.def, get, "x", twice, "a");
  connect(*user.def, get, "x", twice, "b");
  connect(*user.def, twice, "value", set, "offset");
  NodeId group;
  test_assert(e.addGraph(user.id, group).ok());
  test_assert(e.apply() == 3 && s.x(c) == 122);
  s.set(a, 2);
  s.transforms();
  test_assert(e.apply() == 3 && s.transforms() == 3 && s.x(c) == 124);
  /* The user edits the graph. */
  test_assert(user.def->setLiteral(twice, "a", PropValue::ofFloat(1)).ok() &&
              e.apply() == 0);
  test_assert(user.def->unlink(user.def->node(twice)->linkInto("a")->id).ok());
  test_assert(e.apply() == 1 && s.x(c) == 113 && s.x(a) == 2);
  /* The same graph for other objects: a second group, with its own values. */
  const Graph other = makeGraph(s.lib, "other");
  s.lib.setFlag(other.h, BlockFlags::Root, true);
  const NodeId inner = add(*other.def, "graph.group");
  test_assert(other.def->setGroup(inner, user.id).ok());
  test_assert(e.apply() == 0);

  /* One that would make a cycle: c moves a, and c is moved by b, a's child. */
  const Graph bad = makeGraph(s.lib, "bad");
  s.lib.setFlag(bad.h, BlockFlags::Root, true);
  const NodeId get2 = add(*bad.def, kGetPlace), set2 = add(*bad.def, kSetOffset);
  test_assert(bad.def->setParam(get2, "object", pointerTo(c, kThingStruct)).ok());
  test_assert(bad.def->setParam(set2, "object", pointerTo(a, kThingStruct)).ok());
  connect(*bad.def, get2, "x", set2, "offset");
  NodeId group2;
  test_assert(hasCode(e.addGraph(bad.id, group2), StatusCode::InvalidArgument));
  test_assert(e.def.removeNode(group2).ok());
  test_assert(e.apply() >= 0 && e.errors == 0);
  /* The user makes it later, inside: the evaluator reports it, for those on it and after
   * it. */
  const NodeId free = add(*bad.def, "math.float");
  test_assert(bad.def->unlink(bad.def->node(set2)->linkInto("offset")->id).ok());
  connect(*bad.def, free, "value", set2, "offset");
  test_assert(e.addGraph(bad.id, group2).ok() && e.apply() == 0);
  connect(*bad.def, get2, "x", free, "a");
  test_assert(e.apply() == 3 && e.errors == 3);
  test_assert(hasCode(e.inst.evaluate(e.placePath(a)), StatusCode::FailedPrecondition));
  test_assert(bad.def->removeNode(free).ok());
  test_assert(e.apply() == 3 && e.errors == 0 && s.x(c) == 113);

  /* Adapters are found by their flag. */
  int found = 0;
  user.def->forEachNode(NodeTypeFlags::OutputAdapter, [&](const GraphNode &n) {
    found++;
    test_assert(n.id() == set);
  });
  test_assert(found == 1);
  found = 0;
  user.def->forEachNode(NodeTypeFlags::None, [&](const GraphNode &) { found++; });
  test_assert(found == 3);
  /* A new def under the graph's id: its adapters aren't there. */
  test_assert(e.def.diagnostics().size() == 0);
  test_assert(s.lib.markDeleted(user.h).ok() && e.def.diagnostics().size() == 2);
  test_assert(e.apply() == 1 && s.x(c) == 100);
  test_assert(s.lib.restoreFromHistory(user.h).ok() && e.apply() == 1 && s.x(c) == 113);
}

void testChain()
{
  Stage s;
  SceneEvaluator &e = *s.scene;
  const int n = 10000;
  for (int i = 0; i < n; i++) {
    s.object(1);
    if (i > 0) {
      test_assert(e.setParent(s.ids[size_t(i)], s.ids[size_t(i - 1)]).ok());
    }
  }
  test_assert(e.apply() == n && s.transforms() == n && s.x(s.ids.back()) == n);
  s.set(s.ids.back(), 2);
  test_assert(e.apply() == 1 && s.transforms() == 1 && s.x(s.ids.back()) == n + 1);
  s.set(s.ids.front(), 2);
  test_assert(e.apply() == n && s.transforms() == n);
  /* Cut in the middle, and joined again. */
  test_assert(e.setParent(s.ids[n / 2], BlockId()).ok() && e.apply() == n / 2 &&
              s.transforms() == n / 2);
  test_assert(e.setParent(s.ids[n / 2], s.ids[n / 2 - 1]).ok() && e.apply() == n / 2);
  test_assert(
      hasCode(e.setParent(s.ids.front(), s.ids.back()), StatusCode::InvalidArgument));
}

} // namespace

int main()
{
  testScene();
  testUserGraphs();
  testChain();
  return test_end();
}
