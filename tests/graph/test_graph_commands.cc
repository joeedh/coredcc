/* The graph commands and runGraphEdit: what they do, undo, redo and keep alive. */
#include "edit_fixture.h"

#include "coredcc/graph/instance.h"
#include "coredcc/lib/io/library_io.h"

#include <map>

using namespace fixture;

test_init;

namespace {

Status addKernel(NodeExecContext &ctx)
{
  const BlockUse use = ctx.paramBlock("thing");
  const Thing *t = static_cast<const Thing *>(use.data);
  return ctx.setOutput(0, GraphValue::ofFloat(ctx.input(0).asFloat() + ctx.input(1).asFloat() +
                                              (t ? t->value : 0)));
}

void addTypes(Runtime &rt)
{
  NodeTypeDef d("t.add");
  d.input(PropertyDef::floatProp("a"))
      .input(PropertyDef::floatProp("b"))
      .param(PropertyDef::pointerProp("thing", kThingStruct))
      .output("value", "float");
  addType<addKernel>(rt, d, "t.add");
}

PropValue fl(double v)
{
  return PropValue::ofFloat(v);
}
PropValue in(int64_t v)
{
  return PropValue::ofInt(v);
}
PropValue str(const char *s)
{
  return PropValue::ofString(s);
}

/* What an edit must put back, as text: nodes and links by id, sockets in order. */
std::string describe(const GraphDef &def)
{
  std::map<int64_t, std::string> parts;
  auto values = [](const NodeValues &list) {
    std::string s;
    for (const NodeValue &v : list) {
      s += std::string(v.name.c_str()) + (v.off ? "~" : "=") + v.value.toString().c_str() + ",";
    }
    return s;
  };
  def.forEachNode([&](const GraphNode &n) {
    const NodeState s = def.stateOf(n);
    std::string t = std::string(s.typeName.c_str()) + " '" + s.label.c_str() + "' " +
                    std::to_string(s.pos[0]) + "," + std::to_string(s.pos[1]) + " " +
                    values(s.params) + " " + values(s.literals) + " ";
    for (const NodeState::Ref &r : s.refs) {
      t += std::string(r.name.c_str()) + (r.weak ? "~" : "=") + std::to_string(r.target.local.lo) + ",";
    }
    parts[s.id.value] = t + " " + std::to_string(s.group.local.lo);
  });
  def.forEachLink([&](const GraphLink &l) {
    const LinkState s = GraphDef::stateOf(l);
    parts[s.id.value] = std::to_string(s.from.value) + "." + s.fromKey.c_str() + ">" +
                        std::to_string(s.to.value) + "." + s.toKey.c_str();
  });
  std::string out;
  for (const auto &[id, text] : parts) {
    out += std::to_string(id) + ": " + text + "\n";
  }
  for (SocketSide side : {SocketSide::Input, SocketSide::Output}) {
    out += "|";
    for (const InterfaceSocket &s : def.interfaceSockets(side)) {
      out += std::to_string(s.id) + " " + s.name.c_str() + " " + s.typeName.c_str() + " " +
             s.defaultValue.toString().c_str() + ";";
    }
  }
  return out;
}

/* Undoes and redoes the last step: both ends are as they were, and each stamps anew. */
void roundTrip(Doc &d, const GraphDef &def, const std::string &before)
{
  const std::string after = describe(def);
  test_assert(after != before);
  const uint64_t v0 = def.contentVersion();
  test_assert(d.undo().ok() && describe(def) == before);
  const uint64_t v1 = def.contentVersion();
  test_assert(d.redo().ok() && describe(def) == after);
  test_assert(def.contentVersion() > v1 && v1 >= v0);
  test_assert(d.undo().ok() && describe(def) == before);
  test_assert(d.redo().ok() && describe(def) == after);
  test_assert(def.validate().ok());
}

void testCommands()
{
  Doc d;
  addTypes(d.rt);
  test_assert(registerGraphCommands(d.rt).ok()); /* again */
  Result<PropertyBag> made = d.run(kGraphCreateCommand, {{"name", str("g")}});
  test_assert(made.ok());
  const StableRef *ref = made->get("graph")->tryPointer();
  test_assert(ref && ref->owner.kind == OwnerKey::Kind::Block);
  const Graph g{d.lib.find(ref->owner.block), ref->owner.block,
                graphDefOf(d.lib, d.lib.find(ref->owner.block))};
  test_assert(g.def && d.is("graphs[\"g\"].nodes.length", in(0)));
  const PropValue gp = pointerTo(g);
  GraphDef &def = *g.def;
  test_assert(d.undo().ok() && d.lib.get(g.h)->state() == BlockState::InHistory);
  test_assert(hasCode(d.run(kGraphNodeAddCommand, {{"graph", gp}, {"type", str("t.add")}}),
                      StatusCode::FailedPrecondition));
  test_assert(d.redo().ok() && graphDefOf(d.lib, g.h) == &def);
  GraphInstance inst(&d.rt, &d.lib, g.id);

  std::string before = describe(def);
  const double at[2] = {1, 2};
  const NodeId a{d.runFor("node", kGraphNodeAddCommand,
                          {{"graph", gp},
                           {"type", str("t.add")},
                           {"position", PropValue::ofComponents(PropValue::Type::Vec2, at)},
                           {"label", str("A")}})};
  test_assert(def.node(a) && def.node(a)->x() == 1 && def.node(a)->y() == 2);
  test_assert(strcmp(def.node(a)->label().c_str(), "A") == 0);
  roundTrip(d, def, before);
  const NodeId b{d.runFor("node", kGraphNodeAddCommand, {{"graph", gp}, {"type", str("t.add")}})};
  const NodeId v{
      d.runFor("node", kGraphNodeAddCommand, {{"graph", gp}, {"type", str("value.float")}})};

  auto link = [&](NodeId from, NodeId to, const char *key) {
    return LinkId{d.runFor("link", kGraphLinkCommand,
                           {{"graph", gp},
                            {"from", in(from.value)},
                            {"fromSocket", str("value")},
                            {"to", in(to.value)},
                            {"toSocket", str(key)}})};
  };
  before = describe(def);
  const LinkId va = link(v, a, "a");
  test_assert(def.link(va));
  roundTrip(d, def, before);
  link(v, a, "b");
  link(a, b, "a");
  link(a, b, "b");
  const SocketPath out = SocketPath::of(b, "value");
  auto value = [&]() {
    test_assert(inst.evaluate(out).ok());
    return inst.value(out)->asFloat();
  };
  test_assert(d.set("graphs[\"g\"].nodes[#" + std::to_string(v.value) + "].params.value", fl(3)).ok());
  test_assert(value() == 12);

  /* Into an input that has a link: undo brings the old one back, with its id. */
  before = describe(def);
  const LinkId replaced = link(b, v, "nothing");
  test_assert(!def.link(replaced) && describe(def) == before);
  const LinkId ab = def.node(b)->linkInto("a")->id;
  link(v, b, "a");
  test_assert(!def.link(ab) && value() == 9);
  roundTrip(d, def, before);
  test_assert(d.undo().ok() && def.node(b)->linkInto("a")->id == ab && value() == 12);

  before = describe(def);
  test_assert(d.run(kGraphUnlinkCommand, {{"graph", gp}, {"link", in(ab.value)}}).ok());
  test_assert(value() == 6);
  roundTrip(d, def, before);
  test_assert(d.undo().ok() && value() == 12);

  /* A node in the middle, with links on both sides; undo is seen by the instance. */
  before = describe(def);
  test_assert(d.set("graphs[\"g\"].nodes[\"A\"].inputs.a", fl(5)).ok());
  before = describe(def);
  test_assert(d.run(kGraphNodeRemoveCommand, {{"graph", gp}, {"node", in(a.value)}}).ok());
  test_assert(!def.node(a) && def.linkCount() == 0 && value() == 0);
  roundTrip(d, def, before);
  test_assert(d.undo().ok() && value() == 12 && def.linkCount() == 4);

  /* A param through its datapath, after the node was removed and is back. */
  test_assert(d.undo().ok() && d.undo().ok());

  /* Failures change nothing, and aren't in the history. */
  before = describe(def);
  const uint64_t version = def.contentVersion();
  const int steps = d.history.size();
  test_assert(hasCode(d.run(kGraphNodeAddCommand, {{"graph", gp}, {"type", str("t.no")}}), StatusCode::NotFound));
  test_assert(hasCode(d.run(kGraphNodeRemoveCommand, {{"graph", gp}, {"node", in(4)}}), StatusCode::NotFound));
  test_assert(hasCode(d.run(kGraphUnlinkCommand, {{"graph", gp}, {"link", in(4)}}), StatusCode::NotFound));
  test_assert(hasCode(d.run(kGraphNodeAddCommand, {{"graph", PropValue::nullPointer()}, {"type", str("t.add")}}),
                      StatusCode::FailedPrecondition));
  test_assert(describe(def) == before && def.contentVersion() == version);
  test_assert(d.history.size() == steps);
}

void testInterface()
{
  Doc d;
  addTypes(d.rt);
  const Graph g = newGraph(d.lib, "g"), o = newGraph(d.lib, "o");
  GraphDef &def = *g.def;
  const PropValue gp = pointerTo(g);
  auto add = [&](int side, const char *name, int index) {
    return d.runFor("socket", kGraphInterfaceAddCommand,
                    {{"graph", gp},
                     {"side", PropValue::ofEnum(side)},
                     {"name", str(name)},
                     {"type", str("float")},
                     {"index", in(index)}});
  };
  std::string before = describe(def);
  const int64_t x = add(0, "x", -1);
  test_assert(x && def.interfaceSocket(SocketSide::Input, "x"));
  roundTrip(d, def, before);
  before = describe(def);
  const int64_t y = add(0, "y", 0);
  test_assert(def.interfaceSockets(SocketSide::Input)[0].id == y);
  roundTrip(d, def, before);
  const int64_t r = add(1, "r", -1);
  test_assert(def.interfaceSocket(SocketSide::Output, "r")->id == r);

  /* A use of it: links and literals to a removed socket come back to life. */
  const NodeId input = fixture::add(def, "graph.input"), output = fixture::add(def, "graph.output");
  connect(def, input, key(x), output, key(r));
  const NodeId group = fixture::add(*o.def, "graph.group");
  test_assert(o.def->setGroup(group, g.id).ok());
  test_assert(o.def->setLiteral(group, key(x), fl(6)).ok());
  GraphInstance inst(&d.rt, &d.lib, o.id);
  const SocketPath out = SocketPath::of(group, key(r));
  auto is = [&](double v) {
    return inst.evaluate(out).ok() && *inst.value(out) == GraphValue::ofFloat(v);
  };
  test_assert(is(6));

  test_assert(d.set("graphs[\"g\"].inputs[\"x\"].default", fl(1)).ok());
  before = describe(def);
  test_assert(d.set("graphs[\"g\"].inputs[\"x\"].default", fl(2)).ok());
  roundTrip(d, def, before);
  before = describe(def);
  test_assert(d.run(kGraphInterfaceRemoveCommand, {{"graph", gp}, {"socket", in(x)}}).ok());
  test_assert(is(0));
  roundTrip(d, def, before);
  test_assert(d.undo().ok() && is(6));

  before = describe(def);
  test_assert(d.run(kGraphInterfaceMoveCommand, {{"graph", gp}, {"socket", in(x)}, {"index", in(0)}}).ok());
  test_assert(def.interfaceSockets(SocketSide::Input)[0].id == x);
  roundTrip(d, def, before);
  test_assert(hasCode(d.run(kGraphInterfaceMoveCommand, {{"graph", gp}, {"socket", in(x)}, {"index", in(2)}}),
                      StatusCode::OutOfRange));

  before = describe(def);
  auto setType = [&](const char *type) {
    return d.run(kGraphInterfaceSetTypeCommand, {{"graph", gp}, {"socket", in(x)}, {"type", str(type)}});
  };
  test_assert(setType("bool").ok() && def.interfaceSocket(x)->defaultValue.isNone() && is(0));
  roundTrip(d, def, before);
  test_assert(d.undo().ok() && def.interfaceSocket(x)->defaultValue == fl(2) && is(6));
  test_assert(hasCode(setType("no"), StatusCode::NotFound));
  test_assert(hasCode(d.run(kGraphInterfaceRemoveCommand, {{"graph", gp}, {"socket", in(3)}}), StatusCode::NotFound));
  /* Without a literal in the way, the default is restored for instances too. */
  test_assert(o.def->setLiteral(group, key(x), PropValue()).ok() && is(2));
  test_assert(setType("bool").ok() && is(0) && d.undo().ok() && is(2));
}

Status edits(GraphEdit &e, NodeId removed, BlockId thing, BlockId other)
{
  Result<NodeId> a = e.addNode("t.add");
  if (!a.ok()) {
    return a.status();
  }
  CDCC_TRY(e.setLabel(*a, "made"));
  CDCC_TRY(e.setPosition(*a, 3, 4));
  CDCC_TRY(e.setLiteral(*a, "a", fl(7)));
  CDCC_TRY(e.setLiteral(*a, "a", fl(8)));
  CDCC_TRY(e.setParam(*a, "thing", pointerTo(thing)));
  Result<NodeId> v = e.addNode("value.float");
  if (!v.ok()) {
    return v.status();
  }
  CDCC_TRY(e.setParam(*v, "value", fl(2)));
  CDCC_TRY(e.setParam(*v, "value", PropValue()));
  CDCC_TRY(e.setParam(*v, "value", fl(4)));
  CDCC_TRY(e.link({*v, "value"}, {*a, "b"}).status());
  CDCC_TRY(e.removeNode(removed));
  Result<int64_t> s = e.addInterface(SocketSide::Input, "in", "float");
  if (!s.ok()) {
    return s.status();
  }
  CDCC_TRY(e.setInterfaceName(*s, "renamed"));
  CDCC_TRY(e.setInterfaceDefault(*s, in(1))); /* coerced */
  CDCC_TRY(e.setInterfaceType(*s, "int"));
  CDCC_TRY(e.addInterface(SocketSide::Input, "second", "float").status());
  CDCC_TRY(e.moveInterface(*s, 1));
  Result<NodeId> g = e.addNode("graph.group");
  if (!g.ok()) {
    return g.status();
  }
  CDCC_TRY(e.setGroup(*g, other));
  NodeState copy = e.def().stateOf(*e.def().node(*a));
  copy.id = NodeId{77};
  CDCC_TRY(e.insertNode(copy));
  LinkState l;
  l.id = LinkId{78};
  l.from = *v;
  l.fromKey = "value";
  l.to = copy.id;
  l.toKey = "b";
  CDCC_TRY(e.insertLink(l));
  CDCC_TRY(e.unlink(l.id));
  CDCC_TRY(e.useLiteral(copy.id, "a", false));
  CDCC_TRY(e.useLiteral(copy.id, "b", true));
  return e.removeInterface(e.def().interfaceSockets(SocketSide::Input)[0].id);
}

struct RunInside {
  static inline BlockId graph;
  static inline bool fail = false;
  Status exec(Context &ctx)
  {
    CDCC_TRY(runGraphEdit(ctx, graph, "inner", [](GraphEdit &e) {
      return e.addNode("t.add").status();
    }));
    return fail ? Status::error(StatusCode::Cancelled, "outer") : Status();
  }
  Status canRun(Context &) const
  {
    return Status();
  }
  void undo(Context &) {}
  Status redo(Context &)
  {
    return Status(); /* the inner one's */
  }
  uint64_t memSize() const
  {
    return sizeof(*this);
  }
  void discard() {}
  static lbt::Struct<RunInside> *defineBindings()
  {
    auto *st = new lbt::Struct<RunInside>("test::RunInside", sizeof(RunInside));
    BIND_STRUCT_METHOD(st, canRun, MARGS("ctx"));
    BIND_STRUCT_METHOD(st, exec, MARGS("ctx"));
    BIND_STRUCT_METHOD(st, undo, MARGS("ctx"));
    BIND_STRUCT_METHOD(st, redo, MARGS("ctx"));
    BIND_STRUCT_METHOD(st, memSize, MARGS());
    BIND_STRUCT_METHOD(st, discard, MARGS());
    return st;
  }
};

void testRunGraphEdit()
{
  Doc d;
  addTypes(d.rt);
  const Graph g = newGraph(d.lib, "g"), o = newGraph(d.lib, "o");
  for (BlockHandle h : {g.h, o.h}) {
    d.lib.setFlag(h, BlockFlags::Root, true);
  }
  const BlockHandle t1 = newThing(d.lib, "t1", 1), t2 = newThing(d.lib, "t2", 2);
  const BlockId id1 = idOf(d.lib, t1), id2 = idOf(d.lib, t2);
  GraphDef &def = *g.def;
  const NodeId old = add(def, "t.add"), self = add(def, "t.add");
  test_assert(def.setParam(old, "thing", pointerTo(id2)).ok());
  connect(def, old, "value", self, "a");

  const std::string before = describe(def);
  test_assert(runGraphEdit(d.ctx, g.id, "many", [&](GraphEdit &e) {
                return edits(e, old, id1, o.id);
              }).ok());
  test_assert(d.history.size() == 1 && def.node(NodeId{77}));
  roundTrip(d, def, before);

  /* What its refs point to is pinned: the removed node's t2, and t1 and o after the undo. */
  test_assert(d.lib.collect().ok() && d.lib.get(t2) && d.undo().ok());
  test_assert(d.lib.collect().ok() && d.lib.get(t1) && d.lib.get(t2));
  test_assert(d.redo().ok() && d.undo().ok() && describe(def) == before);
  test_assert(d.lib.validate().ok());

  /* A failure anywhere takes back everything, and leaves no step. */
  const int steps = d.history.size();
  for (bool afterEdits : {true, false}) {
    const Status s = runGraphEdit(d.ctx, g.id, "fails", [&](GraphEdit &e) {
      if (afterEdits) {
        CDCC_TRY(edits(e, old, id1, o.id));
      }
      return Status::error(StatusCode::Cancelled, "on purpose");
    });
    test_assert(hasCode(s, StatusCode::Cancelled) && describe(def) == before);
  }
  test_assert(d.history.size() == steps && d.redo().ok() && d.undo().ok());
  /* One edit that fails records nothing, and the rest goes on. */
  test_assert(runGraphEdit(d.ctx, g.id, "some", [&](GraphEdit &e) {
                const NodeId no{5};
                test_assert(hasCode(e.addNode("t.no"), StatusCode::NotFound));
                test_assert(hasCode(e.insertNode(NodeState()), StatusCode::InvalidArgument));
                test_assert(hasCode(e.removeNode(no), StatusCode::NotFound));
                test_assert(hasCode(e.setLabel(no, "x"), StatusCode::NotFound));
                test_assert(hasCode(e.setPosition(no, 1, 1), StatusCode::NotFound));
                test_assert(hasCode(e.setParam(no, "thing", PropValue()), StatusCode::NotFound));
                test_assert(hasCode(e.setParam(old, "no", PropValue()), StatusCode::NotFound));
                test_assert(hasCode(e.setLiteral(no, "a", fl(1)), StatusCode::NotFound));
                test_assert(hasCode(e.useLiteral(old, "no", true), StatusCode::NotFound));
                test_assert(hasCode(e.setGroup(no, o.id), StatusCode::NotFound));
                test_assert(hasCode(e.link({self, "value"}, {old, "a"}), StatusCode::InvalidArgument));
                test_assert(hasCode(e.unlink(LinkId{5}), StatusCode::NotFound));
                test_assert(hasCode(e.insertLink(LinkState()), StatusCode::NotFound));
                test_assert(hasCode(e.setInterfaceType(5, "int"), StatusCode::NotFound));
                test_assert(hasCode(e.setInterfaceName(5, "x"), StatusCode::NotFound));
                test_assert(hasCode(e.setInterfaceDefault(5, fl(1)), StatusCode::NotFound));
                test_assert(hasCode(e.moveInterface(5, 0), StatusCode::NotFound));
                test_assert(hasCode(e.removeInterface(5), StatusCode::NotFound));
                test_assert(hasCode(e.addInterface(SocketSide::Input, "x", "no"), StatusCode::NotFound));
                test_assert(describe(e.def()) == before);
                CDCC_TRY(e.setLiteral(old, "b", fl(3)));
                CDCC_TRY(e.setPosition(old, 8, 9));
                return e.setLabel(old, "kept");
              }).ok());
  roundTrip(d, def, before);
  test_assert(d.undo().ok());

  /* Taken back in the middle. */
  test_assert(runGraphEdit(d.ctx, g.id, "middle", [&](GraphEdit &e) {
                CDCC_TRY(edits(e, old, id1, o.id));
                CDCC_TRY(e.revert());
                test_assert(describe(e.def()) == before);
                return e.setLiteral(old, "b", fl(3));
              }).ok());
  roundTrip(d, def, before);
  test_assert(d.undo().ok());
  /* What `fn` captured is released at once. */
  auto shared = std::make_shared<int>(1);
  test_assert(runGraphEdit(d.ctx, g.id, "x", [shared, old](GraphEdit &e) {
                return e.setLiteral(old, "b", fl(4));
              }).ok());
  test_assert(shared.use_count() == 1 && d.undo().ok());

  /* Errors of the call itself. */
  auto nothing = [](GraphEdit &) { return Status(); };
  test_assert(hasCode(runGraphEdit(d.ctx, id1, "x", nothing), StatusCode::FailedPrecondition));
  test_assert(hasCode(runGraphEdit(d.ctx, BlockId(), "x", nothing), StatusCode::FailedPrecondition));
  Context bare(&d.rt, &d.lib, d.rt.ext<ApiRegistry>().runtimeOwner("doc"));
  test_assert(hasCode(runGraphEdit(bare, g.id, "x", nothing), StatusCode::FailedPrecondition));
  Result<PropertyBag> direct = d.run("graph.edit", {{"name", str("x")}});
  test_assert(hasCode(direct, StatusCode::FailedPrecondition));
  test_assert(strstr(direct.status().message(), "runGraphEdit"));

  /* Inside another command: part of its step, and taken back when it fails. */
  test_assert(addNativeCommand<RunInside>(d.rt, CommandDef("test.inside")).ok());
  RunInside::graph = g.id;
  test_assert(d.run("test.inside", {}).ok() && def.nodeCount() == 3);
  roundTrip(d, def, before);
  test_assert(d.undo().ok() && describe(def) == before);
  RunInside::fail = true;
  test_assert(hasCode(d.run("test.inside", {}), StatusCode::Cancelled) && describe(def) == before);
}

/* Undo and redo when the graph isn't there, or isn't what it was. */
void testGone()
{
  Doc d;
  addTypes(d.rt);
  const Graph g = newGraph(d.lib, "g");
  const PropValue gp = pointerTo(g);
  auto addOne = [&]() {
    return d.run(kGraphNodeAddCommand, {{"graph", gp}, {"type", str("t.add")}}).ok();
  };
  test_assert(addOne() && d.lib.markDeleted(g.h).ok());
  test_assert(hasCode(d.undo(), StatusCode::FailedPrecondition)); /* and the step is dropped */
  test_assert(d.lib.restoreFromHistory(g.h).ok() && g.def->nodeCount() == 1);
  test_assert(addOne() && d.undo().ok() && d.lib.markDeleted(g.h).ok());
  test_assert(hasCode(d.redo(), StatusCode::FailedPrecondition));
  test_assert(d.lib.restoreFromHistory(g.h).ok() && g.def->nodeCount() == 1);

  /* A redo that fails half way takes back its first half. */
  NodeId a, b;
  test_assert(runGraphEdit(d.ctx, g.id, "two", [&](GraphEdit &e) {
                a = e.addNode("t.add").value();
                b = e.addNode("t.add").value();
                return Status();
              }).ok());
  test_assert(d.undo().ok() && g.def->nodeCount() == 1);
  NodeState taken;
  taken.id = b;
  taken.typeName = "t.add";
  test_assert(g.def->insertNode(taken).ok()); /* not through a command */
  test_assert(hasCode(d.redo(), StatusCode::AlreadyExists));
  test_assert(!g.def->node(a) && g.def->nodeCount() == 2 && g.def->removeNode(b).ok());

  /* Another def under the block's id: found by it. */
  test_assert(addOne() && d.undo().ok());
  test_assert(d.lib.markDeleted(g.h).ok() && d.lib.destroyFromHistory(g.h).ok());
  test_assert(!d.lib.find(g.id));
  GraphDef *fresh = litestl::alloc::New<GraphDef>("GraphDef", &d.rt, &d.lib, g.id);
  test_assert(d.lib.reviveAs(TypeId::of(kGraphDefType), g.id, "g", fresh).ok());
  test_assert(d.redo().ok() && fresh->nodeCount() == 1);
  test_assert(d.undo().ok() && fresh->nodeCount() == 0);
}

/* Until graphs are serialized, a save of one fails instead of losing it. */
void testSave()
{
  Doc d;
  const Graph g = newGraph(d.lib, "g");
  test_assert(saveLibrary(d.lib).ok()); /* nothing keeps it */
  d.lib.setFlag(g.h, BlockFlags::Root, true);
  Result<litestl::util::Vector<uint8_t>> saved = saveLibrary(d.lib);
  test_assert(hasCode(saved, StatusCode::FailedPrecondition));
  test_assert(strstr(saved.status().message(), kGraphDefType));
}

/* A group that a direct write made recursive can be removed, and come back. */
void testRecursive()
{
  Doc d;
  addTypes(d.rt);
  const Graph g = newGraph(d.lib, "g"), o = newGraph(d.lib, "o");
  const NodeId n = add(*g.def, "graph.group");
  test_assert(g.def->setGroup(n, o.id).ok());
  struct Redirect {
    Library *lib;
    BlockId to;
  } redirect{&d.lib, g.id};
  RefVisitor v(
      [](void *p, RefCell &cell) {
        const Redirect &r = *static_cast<Redirect *>(p);
        test_assert(r.lib->writeRef(cell, r.to).ok());
      },
      &redirect);
  g.def->foreachRef(v);
  const PropValue gp = pointerTo(g);
  test_assert(d.run(kGraphNodeRemoveCommand, {{"graph", gp}, {"node", in(n.value)}}).ok());
  test_assert(d.undo().ok() && g.def->node(n)->group().target == g.id);
  NodeState s = g.def->stateOf(*g.def->node(n));
  s.id = NodeId{9};
  test_assert(hasCode(g.def->insertNode(s), StatusCode::InvalidArgument));
}

/* Each kind of restore stamps what it puts back, as of now. */
void testStamps()
{
  Doc d;
  addTypes(d.rt);
  const Graph g = newGraph(d.lib, "g"), o = newGraph(d.lib, "o");
  const BlockHandle t = newThing(d.lib, "t", 1);
  GraphDef &def = *g.def;
  const NodeId n = add(def, "t.add"), v = add(def, "value.float"), p = add(def, "graph.group");
  const int64_t x = addSocket(def, SocketSide::Input, "x", "float");
  auto edit = [&](auto fn) {
    test_assert(runGraphEdit(d.ctx, g.id, "x", fn).ok());
    const uint64_t structure = def.structureVersion(), content = def.contentVersion();
    test_assert(d.undo().ok() && def.contentVersion() > content);
    return def.structureVersion() > structure;
  };
  auto now = [&]() { return def.contentVersion(); };

  test_assert(def.setLiteral(n, "a", fl(1)).ok());
  test_assert(!edit([&](GraphEdit &e) { return e.setLiteral(n, "a", fl(2)); }));
  test_assert(def.node(n)->literals()[0].version == now());
  test_assert(def.node(n)->paramVersion() < now());
  test_assert(def.useLiteral(n, "a", false).ok() && def.node(n)->literals()[0].version == now());
  test_assert(def.useLiteral(n, "a", true).ok() && def.node(n)->literals()[0].version == now());
  test_assert(def.setParam(v, "value", fl(1)).ok());
  test_assert(!edit([&](GraphEdit &e) { return e.setParam(v, "value", fl(2)); }));
  test_assert(def.node(v)->params()[0].version == now() && def.node(v)->paramVersion() == now());
  const PropValue thing = pointerTo(idOf(d.lib, t));
  test_assert(!edit([&](GraphEdit &e) { return e.setParam(n, "thing", thing); }));
  test_assert(def.node(n)->paramVersion() == now());
  test_assert(edit([&](GraphEdit &e) { return e.setGroup(p, o.id); }));
  test_assert(def.node(p)->paramVersion() == now() && def.structureVersion() == now());

  auto defaultVersion = [&]() { return def.interfaceSocket(x)->defaultVersion; };
  test_assert(!edit([&](GraphEdit &e) { return e.setInterfaceDefault(x, fl(2)); }));
  test_assert(defaultVersion() == now());
  const uint64_t before = defaultVersion();
  test_assert(edit([&](GraphEdit &e) { return e.removeInterface(x); }));
  test_assert(defaultVersion() > before && def.interfaceVersion() == now());
  test_assert(edit([&](GraphEdit &e) { return e.setInterfaceType(x, "bool"); }));
  test_assert(defaultVersion() == now() && def.interfaceVersion() == now());
}

/* A removed node's target lives as long as the step does. */
void testCollect()
{
  Doc d;
  addTypes(d.rt);
  const Graph g = newGraph(d.lib, "g");
  d.lib.setFlag(g.h, BlockFlags::Root, true);
  const BlockHandle t = newThing(d.lib, "t", 5), u = newThing(d.lib, "u", 6);
  const NodeId n = add(*g.def, "t.add");
  test_assert(g.def->setParam(n, "thing", pointerTo(idOf(d.lib, t))).ok());
  const PropValue gp = pointerTo(g);
  test_assert(d.run(kGraphNodeRemoveCommand, {{"graph", gp}, {"node", in(n.value)}}).ok());
  test_assert(d.lib.collect().ok() && d.lib.get(t) && !d.lib.get(u));
  test_assert(d.lib.get(t)->pins(PinKind::History) == 1 && d.lib.get(t)->pins(PinKind::Host) == 0);
  test_assert(d.undo().ok() && d.lib.collect().ok());
  test_assert(g.def->node(n)->blockTarget("thing") == idOf(d.lib, t));
  GraphInstance inst(&d.rt, &d.lib, g.id);
  test_assert(inst.evaluate(SocketPath::of(n, "value")).ok());
  test_assert(inst.value(SocketPath::of(n, "value"))->asFloat() == 5);
  test_assert(d.redo().ok() && d.lib.collect().ok() && d.lib.get(t));
  d.history.clear(); /* the discard unpins */
  test_assert(d.lib.collect().ok() && !d.lib.get(t));

  /* A group param the edit replaced, and the graph itself while only the history has it. */
  const Graph a = newGraph(d.lib, "a"), b = newGraph(d.lib, "b");
  const NodeId group = add(*g.def, "graph.group");
  test_assert(g.def->setGroup(group, a.id).ok());
  test_assert(runGraphEdit(d.ctx, g.id, "x", [&](GraphEdit &e) {
                return e.setGroup(group, b.id);
              }).ok());
  test_assert(d.lib.collect().ok() && d.lib.get(a.h) && d.undo().ok());
  test_assert(d.lib.collect().ok() && d.lib.get(b.h) && g.def->node(group)->group().target == a.id);
  /* A removed group node's. */
  const Graph c = newGraph(d.lib, "c");
  const NodeId other = add(*g.def, "graph.group");
  test_assert(g.def->setGroup(other, c.id).ok());
  test_assert(d.run(kGraphNodeRemoveCommand, {{"graph", gp}, {"node", in(other.value)}}).ok());
  test_assert(d.lib.collect().ok() && d.lib.get(c.h) && d.undo().ok());
  d.lib.setFlag(g.h, BlockFlags::Root, false);
  test_assert(d.lib.collect().ok() && d.lib.get(g.h) && d.redo().ok());
  d.history.clear();
  test_assert(d.lib.collect().ok() && !d.lib.get(g.h) && !d.lib.get(a.h));
}

} // namespace

int main()
{
  testCommands();
  testInterface();
  testRunGraphEdit();
  testGone();
  testSave();
  testRecursive();
  testStamps();
  testCollect();
  return test_end();
}
