#include "def_fixture.h"

using namespace fixture;

test_init;

namespace {

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
  test_assert(def.contentVersion() == w.rt.lastEdit() &&
              def.structureVersion() == structure);
  test_assert(def.setParam(a, "count", PropValue()).ok());
  test_assert(def.paramValue(*n, "count") == PropValue::ofInt(2) &&
              n->params().size() == 0);
  test_assert(def.paramValue(*n, "maybe").isNone());
  test_assert(def.setParam(a, "maybe", PropValue::ofFloat(1)).ok());
  test_assert(def.paramValue(*n, "maybe") == PropValue::ofFloat(1));
  test_assert(def.paramValue(*n, "no").isNone() && def.paramValue(*n, "thing").isNone());
  const Status noParam = def.setParam(a, "cuont", PropValue::ofInt(1));
  test_assert(hasCode(noParam, StatusCode::NotFound) && noParam.suggestions().size() > 0);
  const uint64_t failed = w.rt.lastEdit();
  test_assert(hasCode(def.setParam(a, "count", PropValue::ofString("x")),
                      StatusCode::TypeMismatch));
  test_assert(
      hasCode(def.setParam(NodeId{5}, "count", PropValue()), StatusCode::NotFound));
  test_assert(w.rt.lastEdit() == failed);

  /* Literals: coerced to the input's def. */
  const SocketType &floats = *w.rt.ext<SocketTypeRegistry>().find("float");
  auto literal = [&](const char *name) { return n->literal(name, floats); };
  test_assert(def.setLiteral(a, "a", PropValue::ofInt(30)).ok());
  test_assert(literal("a")->value == PropValue::ofFloat(10));
  const uint64_t version = literal("a")->version;
  const uint64_t paramVersion = n->paramVersion();
  test_assert(version == def.contentVersion());
  test_assert(def.setLiteral(a, "b", PropValue::ofFloat(2)).ok());
  test_assert(literal("a")->version == version && literal("b")->version > version);
  test_assert(n->paramVersion() == paramVersion);
  test_assert(def.setLiteral(a, "a", PropValue::ofFloat(5)).ok());
  test_assert(literal("a")->value == PropValue::ofFloat(5) &&
              literal("a")->version > version);
  test_assert(def.setLiteral(a, "a", PropValue()).ok() && !literal("a") && literal("b"));
  test_assert(
      hasCode(def.setLiteral(a, "mesh", PropValue::ofFloat(1)), StatusCode::Unsupported));
  test_assert(
      hasCode(def.setLiteral(a, "x", PropValue::ofFloat(1)), StatusCode::NotFound));
  test_assert(
      hasCode(def.setLiteral(a, "a", PropValue::ofString("")), StatusCode::TypeMismatch));

  /* Removing and bringing back, with its id and content. */
  const LinkId l = connect(def, b, "value", a, "a");
  const NodeState state = def.stateOf(*n);
  const LinkState link = GraphDef::stateOf(*def.link(l));
  test_assert(def.removeNode(a).ok() && def.linkCount() == 0 && !def.node(a));
  test_assert(hasCode(def.removeNode(a), StatusCode::NotFound));
  test_assert(def.node(b)->linksOut().size() == 0 && def.validate().ok());
  test_assert(def.insertNode(state).ok() && def.insertLink(link).ok() &&
              def.validate().ok());
  n = def.node(a);
  test_assert(isText(n->label(), "First") && n->x() == 3);
  test_assert(def.paramValue(*n, "maybe") == PropValue::ofFloat(1));
  test_assert(literal("b")->version == n->paramVersion() && n->paramVersion() > version);
  test_assert(def.link(l) && n->linkInto("a")->id == l);
  test_assert(hasCode(def.insertNode(state), StatusCode::AlreadyExists));
  test_assert(hasCode(def.insertLink(link), StatusCode::AlreadyExists));
  LinkState taken = link;
  taken.id = LinkId{77};
  test_assert(hasCode(def.insertLink(taken), StatusCode::AlreadyExists)); /* the input */
  taken.toKey = "b";
  taken.id = LinkId{a.value};
  test_assert(
      hasCode(def.insertLink(taken), StatusCode::AlreadyExists)); /* a node's id */
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
  const int64_t out =
      addSocket(def, SocketSide::Output, "a", "float"); /* names are per side */
  test_assert(def.interfaceSockets(SocketSide::Input).size() == 3);
  test_assert(def.interfaceSocket(a) &&
              def.interfaceSocket(SocketSide::Output, "a")->id == out);
  test_assert(!def.interfaceSocket(SocketSide::Output, "b") && !def.interfaceSocket(999));
  test_assert(hasCode(def.addInterface(SocketSide::Input, "a", "int"),
                      StatusCode::AlreadyExists));
  test_assert(hasCode(def.addInterface(SocketSide::Input, "", "int"),
                      StatusCode::InvalidArgument));
  test_assert(
      hasCode(def.addInterface(SocketSide::Input, "x", "flaot"), StatusCode::NotFound));
  test_assert(hasCode(def.addInterface(SocketSide::Input, "x", "int", 4),
                      StatusCode::OutOfRange));
  test_assert(hasCode(def.addInterface(SocketSide::Input, "x", "int", -2),
                      StatusCode::OutOfRange));
  test_assert(hasCode(def.addInterface(SocketSide::Input, "x", "int", 0, a),
                      StatusCode::AlreadyExists));
  test_assert(
      hasCode(def.addInterface(SocketSide::Input, "x", "int", 0, int64_t(1) << 52),
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
  test_assert(names(in, SocketSide::Input).empty() &&
              names(o, SocketSide::Output).empty());
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
  test_assert(hasCode(def.setInterfaceDefault(a, PropValue::ofString("")),
                      StatusCode::TypeMismatch));
  test_assert(hasCode(def.setInterfaceDefault(out, PropValue::ofFloat(1)),
                      StatusCode::InvalidArgument));
  test_assert(hasCode(def.setInterfaceDefault(c, PropValue::ofFloat(1)),
                      StatusCode::Unsupported));
  test_assert(hasCode(def.setInterfaceDefault(99, PropValue()), StatusCode::NotFound));

  /* A removed socket's links stay, and are diagnosed; bringing it back revives them. */
  const NodeId n = add(def, "math.float");
  const LinkId l = connect(def, in, key(a), n, "a");
  connect(def, n, "value", o, key(out));
  test_assert(def.setLiteral(o, key(out), PropValue::ofFloat(1)).ok());
  test_assert(def.diagnostics().size() == 0);
  test_assert(def.removeInterface(a).ok() &&
              hasCode(def.removeInterface(a), StatusCode::NotFound));
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
  const Graph g = makeGraph(w.lib, "G");
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
  test_assert(!def.setParam(a, "count", PropValue::ofString("")).ok() &&
              count.take() == 0);
  test_assert(def.removeNode(b).ok() && count.take() == 2);

  /* A listener sees the data already changed. */
  int seen = 0;
  ScopedConnection look = w.rt.ext<ChangeBus>().addListener([&](const OwnerKey *) {
    const GraphNode *n = def.node(a);
    seen += def.paramValue(*n, "count") == PropValue::ofInt(5) &&
            n->literal("b", *w.rt.ext<SocketTypeRegistry>().find("float")) &&
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
    test_assert(hasCode(def.setExtra(a, "x", PropValue::ofInt(1)), StatusCode::Busy));
    test_assert(hasCode(def.setParam(a, "count", PropValue::ofInt(1)), StatusCode::Busy));
    test_assert(hasCode(def.setLiteral(a, "a", PropValue::ofInt(1)), StatusCode::Busy));
    test_assert(hasCode(def.setGroup(a, g.id), StatusCode::Busy));
    test_assert(hasCode(def.link({a, "x"}, {a, "a"}), StatusCode::Busy));
    test_assert(hasCode(def.insertLink(LinkState()), StatusCode::Busy));
    test_assert(hasCode(def.unlink(l), StatusCode::Busy));
    test_assert(
        hasCode(def.addInterface(SocketSide::Input, "x", "int"), StatusCode::Busy));
    test_assert(hasCode(def.removeInterface(s), StatusCode::Busy));
    test_assert(hasCode(def.moveInterface(s, 0), StatusCode::Busy));
    test_assert(hasCode(def.setInterfaceType(s, "int"), StatusCode::Busy));
    test_assert(hasCode(def.setInterfaceName(s, "x"), StatusCode::Busy));
    test_assert(hasCode(def.setInterfaceDefault(s, PropValue()), StatusCode::Busy));
    test_assert(w.rt.lastEdit() == edit);
  }
}

/* Nodes of a type that isn't registered yet. */
/* Names that look like keys. */
void testInterfaceNames()
{
  World w;
  GraphDef def(&w.rt, &w.lib);
  const int64_t a = addSocket(def, SocketSide::Output, "a#1", "float");
  for (const char *name : {"", "#", "#12", "@", "@12.a"}) {
    for (SocketSide side : {SocketSide::Input, SocketSide::Output}) {
      test_assert(
          hasCode(def.addInterface(side, name, "float"), StatusCode::InvalidArgument));
    }
    test_assert(hasCode(def.setInterfaceName(a, name), StatusCode::InvalidArgument));
  }
  test_assert(def.setInterfaceName(a, "b@").ok());
}

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
  test_assert(
      hasCode(def.setParam(s.id, "count", PropValue::ofInt(1)), StatusCode::NotFound));
  test_assert(
      def.setLiteral(s.id, "a", PropValue::ofInt(4)).ok()); /* from the snapshot */
  const NodeId f = add(def, "math.float");
  connect(def, f, "value", s.id, "a");
  test_assert(
      hasCode(def.link({s.id, "x"}, {f, "a"}), StatusCode::NotFound)); /* no such type */
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
  test_assert(n->blockRefs().size() == 1 && n->blockRefs()[0].cell.isNull());
  test_assert(def.setParam(s.id, "thing", pointerTo(idOf(w.lib, t), kThingStruct)).ok());
  test_assert(n->blockRefs().size() == 1 &&
              n->blockRefs()[0].cell.target == idOf(w.lib, t));
  test_assert(def.validate().ok());
}

} // namespace

int main()
{
  testNodes();
  testLinks();
  testInterface();
  testHome();
  testInterfaceNames();
  testLateTypes();
  return test_end();
}
