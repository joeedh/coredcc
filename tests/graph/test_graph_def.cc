#include "graph_fixture.h"

#include "coredcc/graph/topology.h"

#include <chrono>

using namespace fixture;

test_init;

namespace {

struct Mesh {
  int verts = 0;
};

} // namespace

template <> struct coredcc::PayloadTraits<Mesh> {
  static constexpr const char *name = "test.Mesh";
};

namespace {

Status nothing(NodeExecContext &)
{
  return Status();
}

void addTypes(Runtime &rt)
{
  test_assert(rt.ext<SocketTypeRegistry>().addPayloadType<Mesh>().ok());
  NodeTypeDef two("test.two");
  two.input(PropertyDef::floatProp("a").range(0, 10).defaultValue(PropValue::ofFloat(1)))
      .input(PropertyDef::floatProp("b"))
      .payloadInput("mesh", "test.Mesh")
      .output("x", "float")
      .output("y", "float")
      .output("m", "mat4")
      .param(PropertyDef::intProp("count").range(0, 8).defaultValue(PropValue::ofInt(2)))
      .param(PropertyDef::floatProp("maybe").access(PropAccess::Optional))
      .param(PropertyDef::pointerProp("thing", kThingStruct))
      .param(PropertyDef::pointerProp("hint", kThingStruct), true);
  addType<nothing>(rt, two, "test.two");
}

struct World {
  Runtime rt;
  Library lib;
  World() : lib(&rt)
  {
    addThings(rt);
    test_assert(registerGraphBlocks(rt).ok() && registerGraphBlocks(rt).ok());
    addTypes(rt);
    lib.setQuarantine(true);
  }
  ~World()
  {
    test_assert(rt.validateAll().ok() && lib.validate().ok());
  }
};

bool isText(const litestl::util::string &a, const char *b)
{
  return strcmp(a.c_str(), b) == 0;
}

void testNodes()
{
  World w;
  GraphDef def(&w.rt, &w.lib);
  test_assert(def.self().isNull() && def.nodeCount() == 0);
  const NodeId a = add(def, "test.two");
  const NodeId b = add(def, "math.float");
  test_assert(a != b && a.value > 0 && a.value < int64_t(1) << 52);
  const Status unknown = def.addNode("math.flaot").status();
  test_assert(hasCode(unknown, StatusCode::NotFound) && unknown.suggestions().size() > 0);

  const GraphNode *n = def.node(a);
  test_assert(n && n->id() == a && isText(n->typeName(), "test.two") && n->type());
  test_assert(!def.node(NodeId{12345}) && def.nodeCount() == 2);
  test_assert(def.sockets(*n, SocketSide::Input).size() == 3);
  test_assert(isText(def.sockets(*n, SocketSide::Output)[2].typeName, "mat4"));

  /* Labels and positions aren't content. */
  const uint64_t edit = w.rt.lastEdit();
  const uint64_t content = def.contentVersion();
  test_assert(def.setLabel(a, "First").ok() && def.setPosition(a, 3, 4).ok());
  test_assert(isText(n->label(), "First") && n->x() == 3 && n->y() == 4);
  test_assert(w.rt.lastEdit() == edit && def.contentVersion() == content);
  test_assert(hasCode(def.setLabel(NodeId{5}, "x"), StatusCode::NotFound));
  test_assert(hasCode(def.setPosition(NodeId{5}, 0, 0), StatusCode::NotFound));

  /* Params: coerced, reset by None, stamped. */
  const uint64_t structure = def.structureVersion();
  test_assert(def.paramValue(*n, "count") == PropValue::ofInt(2));
  const uint64_t before = n->paramVersion();
  test_assert(def.setParam(a, "count", PropValue::ofFloat(20)).ok());
  test_assert(def.paramValue(*n, "count") == PropValue::ofInt(8)); /* clamped */
  test_assert(n->paramVersion() > before && n->paramVersion() == def.contentVersion());
  test_assert(def.contentVersion() == w.rt.lastEdit() && def.structureVersion() == structure);
  test_assert(def.setParam(a, "count", PropValue()).ok());
  test_assert(def.paramValue(*n, "count") == PropValue::ofInt(2) && n->params().size() == 0);
  test_assert(def.paramValue(*n, "maybe").isNone());
  test_assert(def.setParam(a, "maybe", PropValue::ofFloat(1)).ok());
  test_assert(def.paramValue(*n, "maybe") == PropValue::ofFloat(1));
  test_assert(def.paramValue(*n, "no").isNone() && def.paramValue(*n, "thing").isNone());
  const Status noParam = def.setParam(a, "cuont", PropValue::ofInt(1));
  test_assert(hasCode(noParam, StatusCode::NotFound) && noParam.suggestions().size() > 0);
  const uint64_t failed = w.rt.lastEdit();
  test_assert(hasCode(def.setParam(a, "count", PropValue::ofString("x")), StatusCode::TypeMismatch));
  test_assert(hasCode(def.setParam(NodeId{5}, "count", PropValue()), StatusCode::NotFound));
  test_assert(w.rt.lastEdit() == failed);

  /* Literals: coerced to the input's def. */
  test_assert(def.setLiteral(a, "a", PropValue::ofInt(30)).ok());
  test_assert(n->literal("a")->value == PropValue::ofFloat(10));
  const uint64_t version = n->literal("a")->version;
  const uint64_t paramVersion = n->paramVersion();
  test_assert(version == def.contentVersion());
  test_assert(def.setLiteral(a, "b", PropValue::ofFloat(2)).ok());
  test_assert(n->literal("a")->version == version && n->literal("b")->version > version);
  test_assert(n->paramVersion() == paramVersion);
  test_assert(def.setLiteral(a, "a", PropValue::ofFloat(5)).ok());
  test_assert(n->literal("a")->value == PropValue::ofFloat(5) && n->literal("a")->version > version);
  test_assert(def.setLiteral(a, "a", PropValue()).ok() && !n->literal("a") && n->literal("b"));
  test_assert(hasCode(def.setLiteral(a, "mesh", PropValue::ofFloat(1)), StatusCode::Unsupported));
  test_assert(hasCode(def.setLiteral(a, "x", PropValue::ofFloat(1)), StatusCode::NotFound));
  test_assert(hasCode(def.setLiteral(a, "a", PropValue::ofString("")), StatusCode::TypeMismatch));

  /* Removing and bringing back, with its id and content. */
  const LinkId l = connect(def, b, "value", a, "a");
  const NodeState state = def.stateOf(*n);
  const LinkState link = GraphDef::stateOf(*def.link(l));
  test_assert(def.removeNode(a).ok() && def.linkCount() == 0 && !def.node(a));
  test_assert(hasCode(def.removeNode(a), StatusCode::NotFound));
  test_assert(def.node(b)->linksOut().size() == 0 && def.validate().ok());
  test_assert(def.insertNode(state).ok() && def.insertLink(link).ok() && def.validate().ok());
  n = def.node(a);
  test_assert(isText(n->label(), "First") && n->x() == 3);
  test_assert(def.paramValue(*n, "maybe") == PropValue::ofFloat(1));
  test_assert(n->literal("b")->version == n->paramVersion() && n->paramVersion() > version);
  test_assert(def.link(l) && n->linkInto("a")->id == l);
  test_assert(hasCode(def.insertNode(state), StatusCode::AlreadyExists));
  test_assert(hasCode(def.insertLink(link), StatusCode::AlreadyExists));
  LinkState taken = link;
  taken.id = LinkId{77};
  test_assert(hasCode(def.insertLink(taken), StatusCode::AlreadyExists)); /* the input */
  taken.toKey = "b";
  taken.id = LinkId{a.value};
  test_assert(hasCode(def.insertLink(taken), StatusCode::AlreadyExists)); /* a node's id */
  taken.id = LinkId{int64_t(1) << 52};
  test_assert(hasCode(def.insertLink(taken), StatusCode::InvalidArgument));
  taken.id = LinkId{78};
  taken.from = NodeId{5};
  test_assert(hasCode(def.insertLink(taken), StatusCode::NotFound));
  NodeState bad = state;
  bad.id = NodeId{0};
  test_assert(hasCode(def.insertNode(bad), StatusCode::InvalidArgument));
  bad.id = NodeId{int64_t(1) << 52};
  test_assert(hasCode(def.insertNode(bad), StatusCode::InvalidArgument));
  bad.id = NodeId{l.value};
  test_assert(hasCode(def.insertNode(bad), StatusCode::AlreadyExists));

  /* Order, and compaction. */
  litestl::util::Vector<NodeId> ids;
  for (int i = 0; i < 100; i++) {
    ids.append(add(def, "value.float"));
  }
  for (int i = 0; i < 100; i++) {
    if (i % 10) {
      test_assert(def.removeNode(ids[i]).ok());
    }
  }
  test_assert(def.validate().ok() && def.nodeCount() == 12);
  int at = 0;
  def.forEachNode([&](const GraphNode &k) {
    if (at >= 2) {
      test_assert(k.id() == ids[(at - 2) * 10]);
    }
    at++;
  });
  test_assert(at == 12);
}

void testLinks()
{
  World w;
  GraphDef def(&w.rt);
  const NodeId a = add(def, "test.two");
  const NodeId b = add(def, "test.two");
  const NodeId c = add(def, "test.two");
  const uint64_t structure = def.structureVersion();
  const LinkId ab = connect(def, a, "x", b, "a");
  test_assert(def.structureVersion() > structure && def.linkCount() == 1);
  /* One link per input: linking again replaces. */
  const LinkId cb = connect(def, c, "y", b, "a");
  test_assert(def.linkCount() == 1 && !def.link(ab) && def.link(cb));
  test_assert(def.node(a)->linksOut().size() == 0 && def.validate().ok());
  /* An output may feed many inputs. */
  connect(def, c, "y", b, "b");
  test_assert(def.node(c)->linksOut().size() == 2);

  test_assert(hasCode(def.link({a, "z"}, {b, "a"}), StatusCode::NotFound));
  test_assert(hasCode(def.link({a, "a"}, {b, "a"}), StatusCode::NotFound));
  test_assert(hasCode(def.link({a, "x"}, {b, "x"}), StatusCode::NotFound));
  test_assert(hasCode(def.link({NodeId{5}, "x"}, {b, "a"}), StatusCode::NotFound));
  test_assert(hasCode(def.link({a, "x"}, {NodeId{5}, "a"}), StatusCode::NotFound));
  test_assert(hasCode(def.link({a, "m"}, {b, "a"}), StatusCode::TypeMismatch));
  test_assert(hasCode(def.link({a, "x"}, {b, "mesh"}), StatusCode::TypeMismatch));
  test_assert(def.linkCount() == 2 && def.link(cb));

  /* Cycles: to itself, directly and around. A failed link leaves the old one. */
  test_assert(hasCode(def.link({a, "x"}, {a, "a"}), StatusCode::InvalidArgument));
  test_assert(hasCode(def.link({b, "x"}, {c, "a"}), StatusCode::InvalidArgument));
  connect(def, b, "y", a, "a");
  test_assert(hasCode(def.link({a, "x"}, {c, "b"}), StatusCode::InvalidArgument));
  test_assert(hasCode(def.link({a, "x"}, {b, "a"}), StatusCode::InvalidArgument));
  test_assert(def.link(cb) && def.linkCount() == 3);
  test_assert(def.unlink(cb).ok() && hasCode(def.unlink(cb), StatusCode::NotFound));
  test_assert(hasCode(def.link({b, "x"}, {c, "a"}), StatusCode::InvalidArgument));
  test_assert(def.removeNode(c).ok() && def.linkCount() == 1 && def.validate().ok());

  /* A diamond isn't a cycle, and long chains are cheap to extend at either end. */
  GraphDef chain(&w.rt);
  const NodeId first = add(chain, "test.two");
  NodeId last = first, head = first;
  for (int i = 0; i < 3000; i++) {
    const NodeId next = add(chain, "test.two");
    connect(chain, last, "x", next, "a");
    connect(chain, last, "y", next, "b");
    last = next;
    const NodeId prev = add(chain, "test.two");
    connect(chain, prev, "x", head, "a");
    head = prev;
  }
  test_assert(hasCode(chain.link({last, "x"}, {head, "a"}), StatusCode::InvalidArgument));
  test_assert(chain.validate().ok());
}

void testInterface()
{
  World w;
  GraphDef def(&w.rt, &w.lib);
  const int64_t a = addSocket(def, SocketSide::Input, "a", "float");
  const int64_t b = addSocket(def, SocketSide::Input, "b", "int");
  const int64_t c = addSocket(def, SocketSide::Input, "c", "test.Mesh");
  const int64_t out = addSocket(def, SocketSide::Output, "a", "float"); /* names are per side */
  test_assert(def.interfaceSockets(SocketSide::Input).size() == 3);
  test_assert(def.interfaceSocket(a) && def.interfaceSocket(SocketSide::Output, "a")->id == out);
  test_assert(!def.interfaceSocket(SocketSide::Output, "b") && !def.interfaceSocket(999));
  test_assert(hasCode(def.addInterface(SocketSide::Input, "a", "int"), StatusCode::AlreadyExists));
  test_assert(hasCode(def.addInterface(SocketSide::Input, "", "int"), StatusCode::InvalidArgument));
  test_assert(hasCode(def.addInterface(SocketSide::Input, "x", "flaot"), StatusCode::NotFound));
  test_assert(hasCode(def.addInterface(SocketSide::Input, "x", "int", 4), StatusCode::OutOfRange));
  test_assert(hasCode(def.addInterface(SocketSide::Input, "x", "int", -2), StatusCode::OutOfRange));
  test_assert(hasCode(def.addInterface(SocketSide::Input, "x", "int", 0, a),
                      StatusCode::AlreadyExists));
  test_assert(hasCode(def.addInterface(SocketSide::Input, "x", "int", 0, int64_t(1) << 52),
                      StatusCode::InvalidArgument));

  const NodeId in = add(def, "graph.input");
  const NodeId o = add(def, "graph.output");
  test_assert(hasCode(def.addNode("graph.output"), StatusCode::AlreadyExists));
  add(def, "graph.input"); /* as many as one likes */
  auto names = [&](NodeId n, SocketSide side) {
    std::string s;
    for (const SocketInfo &i : def.sockets(*def.node(n), side)) {
      s += i.name.c_str();
      s += ":";
      s += i.typeName.c_str();
      s += " ";
    }
    return s;
  };
  test_assert(names(in, SocketSide::Output) == "a:float b:int c:test.Mesh ");
  test_assert(names(in, SocketSide::Input).empty() && names(o, SocketSide::Output).empty());
  test_assert(names(o, SocketSide::Input) == "a:float ");

  /* Moving, inserting, retyping and renaming, all seen by the nodes. */
  uint64_t version = def.interfaceVersion();
  test_assert(def.moveInterface(c, 0).ok() && def.interfaceVersion() > version);
  test_assert(names(in, SocketSide::Output) == "c:test.Mesh a:float b:int ");
  test_assert(def.moveInterface(c, 2).ok());
  test_assert(names(in, SocketSide::Output) == "a:float b:int c:test.Mesh ");
  test_assert(hasCode(def.moveInterface(c, 3), StatusCode::OutOfRange));
  test_assert(hasCode(def.moveInterface(99, 0), StatusCode::NotFound));
  test_assert(def.addInterface(SocketSide::Input, "d", "quat", 1).ok());
  test_assert(names(in, SocketSide::Output) == "a:float d:quat b:int c:test.Mesh ");
  test_assert(def.setInterfaceType(b, "vec3").ok() && def.setInterfaceName(b, "B").ok());
  test_assert(names(in, SocketSide::Output) == "a:float d:quat B:vec3 c:test.Mesh ");
  test_assert(hasCode(def.setInterfaceName(b, "a"), StatusCode::AlreadyExists));
  test_assert(def.setInterfaceName(b, "B").ok());
  test_assert(hasCode(def.setInterfaceType(b, "no"), StatusCode::NotFound));
  test_assert(hasCode(def.setInterfaceType(99, "int"), StatusCode::NotFound));
  test_assert(hasCode(def.setInterfaceName(99, "z"), StatusCode::NotFound));

  /* Defaults. */
  test_assert(def.setInterfaceDefault(a, PropValue::ofInt(3)).ok());
  test_assert(def.interfaceSocket(a)->defaultValue == PropValue::ofFloat(3));
  version = def.interfaceSocket(a)->defaultVersion;
  test_assert(version == def.contentVersion());
  test_assert(def.setInterfaceDefault(a, PropValue()).ok());
  test_assert(def.interfaceSocket(a)->defaultValue.isNone());
  test_assert(def.interfaceSocket(a)->defaultVersion > version);
  test_assert(hasCode(def.setInterfaceDefault(a, PropValue::ofString("")), StatusCode::TypeMismatch));
  test_assert(hasCode(def.setInterfaceDefault(out, PropValue::ofFloat(1)), StatusCode::InvalidArgument));
  test_assert(hasCode(def.setInterfaceDefault(c, PropValue::ofFloat(1)), StatusCode::Unsupported));
  test_assert(hasCode(def.setInterfaceDefault(99, PropValue()), StatusCode::NotFound));

  /* A removed socket's links stay, and are diagnosed; bringing it back revives them. */
  const NodeId n = add(def, "math.float");
  const LinkId l = connect(def, in, key(a), n, "a");
  connect(def, n, "value", o, key(out));
  test_assert(def.setLiteral(o, key(out), PropValue::ofFloat(1)).ok());
  test_assert(def.diagnostics().size() == 0);
  test_assert(def.removeInterface(a).ok() && hasCode(def.removeInterface(a), StatusCode::NotFound));
  test_assert(def.linkCount() == 2 && def.diagnostics().size() == 1);
  test_assert(def.diagnostics()[0].link == l);
  test_assert(hasCode(def.diagnostics()[0].status, StatusCode::NotFound));
  test_assert(hasCode(def.link({in, key(a)}, {n, "b"}), StatusCode::NotFound));
  const auto topo = def.topology();
  test_assert(topo->inputs[topo->nodes[topo->find(n)].inBegin].fromNode == -1);
  test_assert(def.addInterface(SocketSide::Input, "a", "float", 0, a).ok());
  test_assert(def.diagnostics().size() == 0);
  test_assert(def.setInterfaceType(a, "mat4").ok());
  test_assert(hasCode(def.diagnostics()[0].status, StatusCode::TypeMismatch));
  test_assert(def.validate().ok());
}

/* Counts notifications of one owner. */
struct Counter {
  int own = 0, all = 0;
  uint64_t epoch = 0;
  ScopedConnection c;
  ChangeBus &bus;
  Counter(Runtime &rt, BlockId id) : bus(rt.ext<ChangeBus>())
  {
    c = bus.addListener([this, id](const OwnerKey *k) {
      all++;
      own += k && k->block == id;
    });
    epoch = bus.epoch();
  }
  /* Notified since the last call: none 0, the value 1, the structure too 2. */
  int take()
  {
    const bool moved = bus.epoch() != epoch;
    epoch = bus.epoch();
    const int n = own;
    own = all = 0;
    return n == 1 ? (moved ? 2 : 1) : (n == 0 && !moved ? 0 : -1);
  }
};

void testHome()
{
  World w;
  const Graph g = newGraph(w.lib, "G");
  test_assert(g.def && g.def->self() == g.id && g.def->library() == &w.lib);
  Counter count(w.rt, g.id);
  GraphDef &def = *g.def;
  const NodeId a = add(def, "test.two");
  test_assert(count.take() == 2);
  const NodeId b = add(def, "test.two");
  count.take();
  const LinkId l = connect(def, a, "x", b, "a");
  test_assert(count.take() == 2);
  test_assert(def.setParam(a, "count", PropValue::ofInt(3)).ok() && count.take() == 1);
  test_assert(def.setLiteral(a, "a", PropValue::ofInt(3)).ok() && count.take() == 1);
  test_assert(def.setLabel(a, "x").ok() && count.take() == 1);
  test_assert(def.setPosition(a, 1, 1).ok() && count.take() == 1);
  test_assert(def.unlink(l).ok() && count.take() == 2);
  const int64_t s = addSocket(def, SocketSide::Input, "s", "float");
  test_assert(count.take() == 2);
  test_assert(def.setInterfaceDefault(s, PropValue::ofInt(1)).ok() && count.take() == 1);
  test_assert(def.setInterfaceName(s, "t").ok() && count.take() == 2);
  test_assert(def.setInterfaceType(s, "int").ok() && count.take() == 2);
  test_assert(def.moveInterface(s, 0).ok() && count.take() == 2);
  test_assert(def.removeInterface(s).ok() && count.take() == 2);
  test_assert(!def.setParam(a, "count", PropValue::ofString("")).ok() && count.take() == 0);
  test_assert(def.removeNode(b).ok() && count.take() == 2);

  /* A listener sees the data already changed. */
  int seen = 0;
  ScopedConnection look = w.rt.ext<ChangeBus>().addListener([&](const OwnerKey *) {
    const GraphNode *n = def.node(a);
    seen += def.paramValue(*n, "count") == PropValue::ofInt(5) && n->literal("b") &&
            n->paramVersion() == def.contentVersion();
  });
  test_assert(def.setLiteral(a, "b", PropValue::ofInt(1)).ok());
  test_assert(def.setParam(a, "count", PropValue::ofInt(5)).ok() && seen == 1);
  look.disconnect();

  /* A standalone def tells nobody. */
  GraphDef alone(&w.rt);
  count.take();
  const NodeId n = add(alone, "graph.group");
  test_assert(count.all == 0 && count.take() == 0);
  test_assert(hasCode(alone.setGroup(n, g.id), StatusCode::FailedPrecondition));
  test_assert(alone.setGroup(n, BlockId()).ok());

  /* Everything is Busy while a graph evaluates. */
  const NodeState state = def.stateOf(*def.node(a));
  {
    EvaluationScope scope(w.rt);
    const uint64_t edit = w.rt.lastEdit();
    test_assert(hasCode(def.addNode("test.two"), StatusCode::Busy));
    test_assert(hasCode(def.insertNode(state), StatusCode::Busy));
    test_assert(hasCode(def.removeNode(a), StatusCode::Busy));
    test_assert(hasCode(def.setLabel(a, ""), StatusCode::Busy));
    test_assert(hasCode(def.setPosition(a, 0, 0), StatusCode::Busy));
    test_assert(hasCode(def.setParam(a, "count", PropValue::ofInt(1)), StatusCode::Busy));
    test_assert(hasCode(def.setLiteral(a, "a", PropValue::ofInt(1)), StatusCode::Busy));
    test_assert(hasCode(def.setGroup(a, g.id), StatusCode::Busy));
    test_assert(hasCode(def.link({a, "x"}, {a, "a"}), StatusCode::Busy));
    test_assert(hasCode(def.insertLink(LinkState()), StatusCode::Busy));
    test_assert(hasCode(def.unlink(l), StatusCode::Busy));
    test_assert(hasCode(def.addInterface(SocketSide::Input, "x", "int"), StatusCode::Busy));
    test_assert(hasCode(def.removeInterface(s), StatusCode::Busy));
    test_assert(hasCode(def.moveInterface(s, 0), StatusCode::Busy));
    test_assert(hasCode(def.setInterfaceType(s, "int"), StatusCode::Busy));
    test_assert(hasCode(def.setInterfaceName(s, "x"), StatusCode::Busy));
    test_assert(hasCode(def.setInterfaceDefault(s, PropValue()), StatusCode::Busy));
    test_assert(w.rt.lastEdit() == edit);
  }
}

PropValue pointerTo(BlockId id, StructId type)
{
  StableRef r;
  r.owner = OwnerKey::ofBlock(id);
  r.type = type;
  return PropValue::ofPointer(r);
}

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
    out[i] = addSocket(*inner.def, SocketSide::Output, ("o" + std::to_string(i)).c_str(), "float");
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
  test_assert(hasCode(mid.def->setGroup(group, idOf(w.lib, thing)), StatusCode::TypeMismatch));
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
  test_assert(hasCode(md.link({group, key(out[0])}, {group, key(in[0])}), StatusCode::InvalidArgument));
  const LinkId cross = connect(md, group, key(out[0]), group, key(in[1]));
  test_assert(hasCode(md.link({group, key(out[1])}, {group, key(in[0])}), StatusCode::InvalidArgument));
  test_assert(hasCode(md.link({group, key(out[2])}, {group, key(in[1])}), StatusCode::InvalidArgument));
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
  test_assert(hasCode(md.link({k, "value"}, {group, key(in[1])}), StatusCode::InvalidArgument));
  test_assert(hasCode(md.link({k, "value"}, {group, key(in[0])}), StatusCode::InvalidArgument));

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
  test_assert(md.topology()->serial != serial && md.topology()->groups[0].targetInterface == 0);
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
    const NodeId a = add(d, "graph.input"), b = add(d, "graph.output"), m = add(d, "math.float");
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

/* Nodes of a type that isn't registered yet. */
void testLateTypes()
{
  World w;
  GraphDef def(&w.rt, &w.lib);
  NodeState s;
  s.id = NodeId{100};
  s.typeName = "late.node";
  s.inputs.append(SocketInfo{"a", "a", "float"});
  s.outputs.append(SocketInfo{"x", "x", "late.Payload"});
  s.params.append(NodeValue{"count", PropValue::ofInt(3), 0});
  s.params.append(NodeValue{"wrong", PropValue::ofString("no number"), 0});
  s.literals.append(NodeValue{"a", PropValue::ofFloat(2), 0});
  test_assert(def.insertNode(s).ok());
  const GraphNode *n = def.node(s.id);
  test_assert(n && !n->type() && def.sockets(*n, SocketSide::Output).size() == 1);
  test_assert(def.paramValue(*n, "count").isNone());
  test_assert(hasCode(def.setParam(s.id, "count", PropValue::ofInt(1)), StatusCode::NotFound));
  test_assert(def.setLiteral(s.id, "a", PropValue::ofInt(4)).ok()); /* from the snapshot */
  const NodeId f = add(def, "math.float");
  connect(def, f, "value", s.id, "a");
  test_assert(hasCode(def.link({s.id, "x"}, {f, "a"}), StatusCode::NotFound)); /* no such type */
  const auto topo = def.topology();
  test_assert(topo->nodes[topo->find(s.id)].type == nullptr);
  test_assert(topo->outputs[topo->nodes[topo->find(s.id)].outBegin].type == nullptr);
  test_assert(def.topology() == topo);

  NodeTypeDef late("late.node");
  late.input(PropertyDef::floatProp("a"))
      .input(PropertyDef::floatProp("more"))
      .output("y", "float")
      .param(PropertyDef::intProp("count"))
      .param(PropertyDef::intProp("wrong").defaultValue(PropValue::ofInt(7)))
      .param(PropertyDef::pointerProp("thing", kThingStruct));
  addType<nothing>(w.rt, late, "late.node");
  test_assert(def.sockets(*n, SocketSide::Input).size() == 2 && n->type());
  test_assert(def.paramValue(*n, "count") == PropValue::ofInt(3));
  test_assert(def.paramValue(*n, "wrong") == PropValue::ofInt(7)); /* can't be coerced */
  const auto after = def.topology();
  test_assert(after != topo && after->nodes[after->find(s.id)].type);
  const BlockHandle t = newThing(w.lib, "t", 0);
  test_assert(n->blockRefs().size() == 0);
  test_assert(def.setParam(s.id, "thing", pointerTo(idOf(w.lib, t), kThingStruct)).ok());
  test_assert(n->blockRefs().size() == 1 && n->blockRefs()[0].cell.target == idOf(w.lib, t));
  test_assert(def.validate().ok());
}

} // namespace

int main()
{
  testNodes();
  testLinks();
  testInterface();
  testHome();
  testRefs();
  testGroups();
  testDoubling();
  testLateTypes();
  return test_end();
}
