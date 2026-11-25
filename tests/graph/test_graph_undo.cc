/* What undo of graph edits puts back exactly: values that weren't set, node order, extras. And
 * undo when its Library is gone, or isn't the Context's. */
#include "edit_fixture.h"

#include "coredcc/api/library_io.h"
#include "coredcc/graph/instance.h"

using namespace fixture;

test_init;

namespace {

/* Its Optional param, or -1; plus the others. */
Status paramKernel(NodeExecContext &ctx)
{
  const PropValue *maybe = ctx.param("maybe");
  const double m = maybe && !maybe->isNone() ? maybe->asFloat() : -1;
  return ctx.setOutput(
      0, GraphValue::ofFloat(m + 100 * ctx.param("plain")->asFloat() + 10000 * ctx.input(0).asFloat()));
}

PropValue fl(double v)
{
  return PropValue::ofFloat(v);
}

struct UndoDoc : Doc {
  Graph g;
  NodeId n;
  SocketPath out;

  UndoDoc()
  {
    NodeTypeDef t("t.params");
    t.input(PropertyDef::floatProp("a").defaultValue(fl(1)))
        .param(PropertyDef::floatProp("maybe").access(PropAccess::Optional))
        .param(PropertyDef::floatProp("plain").defaultValue(fl(2)))
        .param(PropertyDef::vecProp("vec", 3))
        .output("value", "float");
    addType<paramKernel>(rt, t, "t.params");
    g = newGraph(lib, "g");
    lib.setFlag(g.h, BlockFlags::Root, true);
    n = add(*g.def, "t.params");
    out = SocketPath::of(n, "value");
  }
  const GraphNode &node() const
  {
    return *g.def->node(n);
  }
  std::string path(const char *rest, NodeId at = {})
  {
    return "graphs[\"g\"].nodes[#" + std::to_string((at ? at : n).value) + "]" + rest;
  }
  bool gives(double want)
  {
    GraphInstance inst(&rt, &lib, g.id);
    const Status s = inst.evaluate(out);
    if (!s.ok() || inst.value(out)->asFloat() != want) {
      printf("%s: %g, not %g\n", s.toString().c_str(),
             s.ok() ? inst.value(out)->asFloat() : 0.0, want);
    }
    return s.ok() && inst.value(out)->asFloat() == want;
  }
  litestl::util::Vector<uint8_t> save()
  {
    Result<litestl::util::Vector<uint8_t>> b = saveLibrary(lib);
    test_assert(b.ok());
    return *b;
  }
};

bool same(const litestl::util::Vector<uint8_t> &a, const litestl::util::Vector<uint8_t> &b)
{
  return a.size() == b.size() && memcmp(a.data(), b.data(), size_t(a.size())) == 0;
}

/* ---- values ---- */

void testParams()
{
  UndoDoc d;
  test_assert(d.gives(10199));
  const auto before = d.save();
  /* Unset: undo unsets it again. */
  test_assert(d.set(d.path(".params.maybe"), fl(5)).ok() && d.gives(10205));
  test_assert(d.undo().ok() && d.gives(10199) && !d.node().param("maybe"));
  test_assert(same(before, d.save()));
  test_assert(d.redo().ok() && d.gives(10205) && d.node().param("maybe"));
  /* Set: undo sets what it was. */
  test_assert(d.set(d.path(".params.maybe"), fl(7)).ok() && d.undo().ok() && d.gives(10205));
  test_assert(d.undo().ok());

  /* So that the type's default may change later, plain params and literals aren't set either. */
  test_assert(d.set(d.path(".params.plain"), fl(3)).ok() && d.undo().ok());
  test_assert(!d.node().param("plain") && d.node().params().size() == 0);
  test_assert(d.set(d.path(".params.vec[1]"), fl(3)).ok());
  test_assert(d.is(d.path(".params.vec"), PropValue::ofVec3(0, 3, 0)));
  test_assert(d.undo().ok() && !d.node().param("vec"));
  test_assert(d.redo().ok() && d.is(d.path(".params.vec"), PropValue::ofVec3(0, 3, 0)));
  test_assert(d.undo().ok());
  test_assert(d.set(d.path(".inputs.a"), fl(3)).ok() && d.gives(30199) && d.undo().ok());
  test_assert(d.node().literals().size() == 0 && d.gives(10199) && same(before, d.save()));
  test_assert(d.redo().ok() && d.gives(30199) && d.undo().ok());

  /* Others stay as they are. */
  test_assert(d.g.def->setParam(d.n, "plain", fl(3)).ok());
  test_assert(d.set(d.path(".params.maybe"), fl(5)).ok() && d.undo().ok());
  test_assert(d.node().params().size() == 1 && d.g.def->setParam(d.n, "plain", PropValue()).ok());

  /* A gesture is one step. */
  for (int i = 0; i < 3; i++) {
    test_assert(setValueUndo(d.ctx, d.path(".params.maybe"), fl(i), 9).ok());
  }
  test_assert(d.undo().ok() && !d.node().param("maybe"));
  test_assert(d.redo().ok() && d.gives(10202) && d.undo().ok());

  /* A value that its def doesn't accept reads as the default, and comes back. */
  NodeValues odd;
  odd.append(NodeValue{"plain", PropValue::ofString("odd")});
  test_assert(d.g.def->restoreValues(d.n, true, "plain", odd).ok());
  test_assert(d.is(d.path(".params.plain"), fl(2)));
  test_assert(d.set(d.path(".params.plain"), fl(4)).ok() && d.gives(10399) && d.undo().ok());
  test_assert(d.node().param("plain")->value == PropValue::ofString("odd"));

  /* Watchers hear of it, and it waits for an evaluation like a set does. */
  int fired = 0;
  auto c = d.rt.ext<ChangeBus>().watch(d.ctx, d.path(".params.maybe"),
                                       [&](const PropValue &, const Status &) { fired++; });
  test_assert(c.ok());
  test_assert(d.set(d.path(".params.maybe"), fl(5)).ok());
  d.rt.ext<ChangeBus>().flush();
  test_assert(fired == 1 && d.undo().ok());
  d.rt.ext<ChangeBus>().flush();
  test_assert(fired == 2 && d.redo().ok() && d.undo().ok());
}

/* The sockets of a group node, and interface defaults. */
void testSockets()
{
  UndoDoc d;
  const Graph inner = newGraph(d.lib, "inner");
  const int64_t a = addSocket(*inner.def, SocketSide::Input, "a", "float");
  const NodeId group = add(*d.g.def, "graph.group");
  test_assert(d.g.def->setGroup(group, inner.id).ok());
  const GraphNode &n = *d.g.def->node(group);
  const std::string socket = d.path(".sockets[\"a\"]", group);

  test_assert(d.set(socket + ".isSet", PropValue::ofBool(true)).ok() && n.literals().size() == 1);
  test_assert(d.set(socket + ".value", fl(3)).ok());
  test_assert(d.undo().ok() && d.is(socket + ".value", fl(0)));
  test_assert(d.undo().ok() && n.literals().size() == 0); /* it had no value */
  test_assert(d.redo().ok() && d.redo().ok() && d.is(socket + ".value", fl(3)));
  /* One out of use keeps its value, and so does one of another type. */
  test_assert(d.set(socket + ".isSet", PropValue::ofBool(false)).ok() && n.literals()[0].off);
  test_assert(d.undo().ok() && !n.literals()[0].off && d.redo().ok() && n.literals()[0].off);
  test_assert(d.undo().ok());
  test_assert(inner.def->setInterfaceType(a, "int").ok());
  test_assert(d.set(socket + ".isSet", PropValue::ofBool(true)).ok() && n.literals().size() == 2);
  test_assert(d.set(socket + ".value", PropValue::ofInt(8)).ok());
  test_assert(d.undo().ok() && d.undo().ok() && n.literals().size() == 1);
  test_assert(n.literals()[0].value == fl(3));

  /* Redo stores what was stored, not what the input has by now. */
  test_assert(inner.def->setInterfaceType(a, "float").ok());
  test_assert(d.g.def->setLiteral(group, key(a), PropValue()).ok());
  test_assert(inner.def->setInterfaceDefault(a, fl(1)).ok());
  test_assert(d.set(socket + ".isSet", PropValue::ofBool(true)).ok() && d.undo().ok());
  test_assert(inner.def->setInterfaceDefault(a, fl(2)).ok() && d.is(socket + ".value", fl(2)));
  test_assert(d.redo().ok() && d.is(socket + ".value", fl(1)) && d.undo().ok());
  test_assert(inner.def->setInterfaceDefault(a, PropValue()).ok());

  /* A default that was never set follows its type. */
  test_assert(inner.def->setInterfaceType(a, "float").ok());
  const std::string def = "graphs[\"inner\"].inputs[#" + std::to_string(a) + "].default";
  test_assert(d.is(def, fl(0)) && d.set(def, fl(6)).ok() && d.is(def, fl(6)));
  test_assert(d.undo().ok() && inner.def->interfaceSocket(a)->defaultValue.isNone());
  test_assert(d.redo().ok() && d.is(def, fl(6)) && d.undo().ok());
}

/* ---- node order ---- */

void testOrder()
{
  UndoDoc d;
  GraphDef &def = *d.g.def;
  litestl::util::Vector<NodeId> ids;
  ids.append(d.n);
  for (int i = 1; i < 40; i++) {
    ids.append(add(def, "t.params"));
  }
  for (int i = 1; i < 40; i++) {
    connect(def, ids[i / 2], "value", ids[i], "a");
  }
  auto ordered = [&]() {
    bool same = def.nodeCount() == int(ids.size()) && def.validate().ok();
    for (int i = 0; same && i < int(ids.size()); i++) {
      same = def.nodeAt(i)->id() == ids[i];
    }
    return same && !def.nodeAt(-1) && !def.nodeAt(def.nodeCount());
  };
  auto remove = [&](int i) {
    return d.run(kGraphNodeRemoveCommand,
                 {{"graph", pointerTo(d.g)}, {"node", PropValue::ofInt(ids[i].value)}})
        .ok();
  };
  const auto before = d.save();
  for (int i : {20, 0, 39}) {
    test_assert(remove(i) && def.nodeCount() == 39 && !ordered());
    test_assert(d.undo().ok() && ordered() && same(before, d.save()));
    test_assert(d.redo().ok() && d.undo().ok() && ordered());
  }
  /* All of them, in an order that isn't theirs. */
  for (int i = 0; i < 40; i++) {
    test_assert(remove(i * 7 % 40));
  }
  test_assert(def.nodeCount() == 0 && !def.nodeAt(0) && def.validate().ok());
  for (int i = 0; i < 40; i++) {
    test_assert(d.undo().ok());
  }
  test_assert(ordered() && same(before, d.save()));
  /* In one step, with new ones after it. */
  test_assert(runGraphEdit(d.ctx, d.g.id, "remove", [&](GraphEdit &e) {
                for (int i : {3, 4, 5, 2, 1, 0, 39}) {
                  CDCC_TRY(e.removeNode(ids[i]));
                }
                return e.addNode("t.params").status();
              }).ok());
  const NodeId last = def.nodeAt(def.nodeCount() - 1)->id();
  test_assert(d.undo().ok() && ordered() && same(before, d.save()));
  test_assert(d.redo().ok() && def.nodeAt(def.nodeCount() - 1)->id() == last && d.undo().ok());

  /* By hand: after a node, first, and last when there is no such node. */
  NodeState s = def.stateOf(*def.nodeAt(5));
  test_assert(!s.after);
  s.id = NodeId{1};
  s.after = ids[2];
  test_assert(def.insertNode(s).ok() && def.nodeAt(3)->id() == s.id);
  test_assert(def.nodeAt(3)->previous()->id() == ids[2] && !def.nodeAt(0)->previous());
  s.id = NodeId{2};
  s.after = NodeId();
  test_assert(def.insertNode(s).ok() && def.nodeAt(0)->id() == s.id);
  s.id = NodeId{3};
  s.after = NodeId{999};
  test_assert(def.insertNode(s).ok() && def.nodeAt(def.nodeCount() - 1)->id() == s.id);
  test_assert(def.validate().ok());
}

/* Undo of other edits gives the same bytes too, though it stores things in another order. */
void testBytes()
{
  UndoDoc d;
  GraphDef &def = *d.g.def;
  const NodeId a = add(def, "t.params"), b = add(def, "t.params");
  const int64_t in = addSocket(def, SocketSide::Output, "x", "float");
  const int64_t in2 = addSocket(def, SocketSide::Output, "y", "float");
  const NodeId out = add(def, "graph.output");
  const LinkId l = connect(def, a, "value", out, key(in));
  connect(def, b, "value", out, key(in2));
  test_assert(def.setParam(a, "maybe", fl(1)).ok() && def.setParam(a, "plain", fl(2)).ok());
  test_assert(def.setExtra(a, "ui.x", fl(1)).ok() && def.setExtra(a, "ui.y", fl(1)).ok());
  const auto before = d.save();
  test_assert(runGraphEdit(d.ctx, d.g.id, "all", [&](GraphEdit &e) {
                CDCC_TRY(e.setParam(a, "maybe", PropValue()));
                CDCC_TRY(e.setExtra(a, "ui.x", PropValue()));
                return e.unlink(l);
              }).ok());
  test_assert(!same(before, d.save()) && d.undo().ok() && same(before, d.save()));
}

/* Literals of two types under one key. */
void testLiteralBytes()
{
  UndoDoc d;
  const Graph inner = newGraph(d.lib, "inner");
  const int64_t a = addSocket(*inner.def, SocketSide::Input, "a", "float");
  const NodeId group = add(*d.g.def, "graph.group");
  test_assert(d.g.def->setGroup(group, inner.id).ok());
  NodeValues values;
  values.append(NodeValue{key(a).c_str(), fl(1)});
  values.append(NodeValue{key(a).c_str(), PropValue::ofInt(2)});
  test_assert(d.g.def->restoreValues(group, false, key(a), values).ok());
  const auto before = d.save();
  std::swap(values[0], values[1]);
  test_assert(d.g.def->restoreValues(group, false, key(a), values).ok());
  test_assert(same(before, d.save()));
}

/* ---- extras ---- */

void testExtras()
{
  UndoDoc d;
  GraphDef &def = *d.g.def;
  GraphInstance inst(&d.rt, &d.lib, d.g.id);
  test_assert(inst.evaluate(d.out).ok());
  const uint64_t content = def.contentVersion();
  auto is = [&](const char *name, const PropValue &v) {
    for (const BlockEntry &e : d.node().extras()) {
      if (strcmp(e.name.c_str(), name) == 0) {
        return e.value == v;
      }
    }
    return v.isNone();
  };
  test_assert(def.setExtra(d.n, "ui.color", PropValue::ofVec3(1, 0, 0)).ok());
  test_assert(def.setExtra(d.n, "ui.width", fl(80)).ok() && d.node().extras().size() == 2);
  test_assert(def.setExtra(d.n, "ui.width", fl(90)).ok() && d.node().extras().size() == 2);
  test_assert(is("ui.width", fl(90)) && is("ui.color", PropValue::ofVec3(1, 0, 0)));
  test_assert(def.setExtra(d.n, "ui.color", PropValue()).ok() && d.node().extras().size() == 1);
  test_assert(def.setExtra(d.n, "ui.color", PropValue()).ok());
  test_assert(hasCode(def.setExtra(d.n, "", fl(1)), StatusCode::InvalidArgument));
  test_assert(hasCode(def.setExtra(NodeId{5}, "a", fl(1)), StatusCode::NotFound));
  test_assert(hasCode(def.setExtra(d.n, "a", pointerTo(d.g)), StatusCode::Unsupported));
  test_assert(hasCode(def.setExtra(d.n, "a", PropValue::nullPointer()), StatusCode::Unsupported));
  /* Nothing evaluates because of them. */
  test_assert(def.contentVersion() == content && inst.isCurrent(d.out));

  /* Undo, with the node too. */
  auto edit = [&](const char *name, const PropValue &v) {
    return runGraphEdit(d.ctx, d.g.id, "extra",
                        [&](GraphEdit &e) { return e.setExtra(d.n, name, v); });
  };
  test_assert(edit("ui.width", fl(3)).ok() && edit("ui.new", fl(4)).ok());
  test_assert(edit("ui.width", PropValue()).ok() && is("ui.width", PropValue()));
  test_assert(hasCode(edit("", fl(1)), StatusCode::InvalidArgument));
  test_assert(d.undo().ok() && is("ui.width", fl(3)) && is("ui.new", fl(4)));
  test_assert(d.undo().ok() && is("ui.new", PropValue()) && d.undo().ok() && is("ui.width", fl(90)));
  test_assert(d.redo().ok() && d.redo().ok() && is("ui.width", fl(3)) && is("ui.new", fl(4)));
  test_assert(d.run(kGraphNodeRemoveCommand,
                    {{"graph", pointerTo(d.g)}, {"node", PropValue::ofInt(d.n.value)}}).ok());
  test_assert(d.undo().ok() && is("ui.width", fl(3)) && is("ui.new", fl(4)));
  test_assert(def.stateOf(d.node()).extras.size() == 2);

  /* A standalone def has them too, and no Library to ask. */
  GraphDef alone(&d.rt);
  const NodeId m = add(alone, "t.params");
  test_assert(alone.setExtra(m, "a", fl(1)).ok() && alone.node(m)->extras().size() == 1);
}

/* ---- Libraries ---- */

struct Two {
  DocRuntime rt;
  std::unique_ptr<Library> one;
  Library two;
  LinearHistory history;
  CommandExecutor executor;
  Context in1, in2;

  Two()
      : one(std::make_unique<Library>(&rt)), two(&rt), executor(&rt, &history),
        in1(&rt, one.get(), rt.ext<ApiRegistry>().runtimeOwner("doc")),
        in2(&rt, &two, rt.ext<ApiRegistry>().runtimeOwner("doc"))
  {
    in1.setExecutor(&executor);
    in2.setExecutor(&executor);
  }
  ~Two()
  {
    history.clear();
    test_assert(!rt.hasStickyError() && rt.validateAll().ok());
  }
  /* Creates a graph, or edits one. */
  void act(bool create)
  {
    if (create) {
      const CommandDef *def = rt.ext<CommandRegistry>().find(kGraphCreateCommand);
      test_assert(executor.run(in1, *def, def->newInputs()).ok());
      return;
    }
    const Graph g = newGraph(*one, "g");
    /* The same graph in the other document. */
    GraphDef *twin = litestl::alloc::New<GraphDef>("coredcc::GraphDef", &rt, &two, g.id);
    test_assert(two.reviveAs(TypeId::of(kGraphDefType), g.id, "g", twin).ok());
    test_assert(runGraphEdit(in1, g.id, "add", [](GraphEdit &e) {
                  return e.addNode("math.float").status();
                }).ok());
  }
  static std::string text(const Library &lib)
  {
    std::string out;
    lib.forEach([&](BlockHandle h, const Datablock &b) {
      if (b.state() == BlockState::InHistory) { /* a failed redo lets go of it */
        return;
      }
      const GraphDef *def = graphDefOf(lib, h);
      out += std::string(b.name().c_str()) + std::to_string(int(b.state())) + " " +
             std::to_string(def ? def->nodeCount() : -1) + ";";
    });
    return out;
  }
};

void testLibraries()
{
  for (const bool create : {false, true}) {
    for (const bool redo : {false, true}) {
      Two d;
      d.act(create);
      if (redo) {
        test_assert(d.executor.undo(d.in1).ok());
      }
      const std::string one = Two::text(*d.one), two = Two::text(d.two);
      const uint64_t epoch = d.two.epoch();
      Status s = redo ? d.executor.redo(d.in2) : d.executor.undo(d.in2);
      test_assert(hasCode(s, StatusCode::FailedPrecondition));
      test_assert(strstr(s.message(), "in another Library"));
      test_assert(Two::text(*d.one) == one && Two::text(d.two) == two);
      test_assert(d.two.epoch() == epoch);
    }
    for (int when = 0; when < 3; when++) {
      Two d;
      d.act(create);
      if (when == 1) {
        test_assert(d.executor.undo(d.in1).ok());
      }
      d.one = nullptr;
      Context gone(&d.rt, nullptr, d.in1.root());
      gone.setExecutor(&d.executor);
      if (when < 2) {
        Status s = when ? d.executor.redo(gone) : d.executor.undo(gone);
        test_assert(hasCode(s, StatusCode::FailedPrecondition));
        test_assert(strstr(s.message(), "Library is gone"));
      }
      test_assert(d.history.validate().ok());
    }
  }
}

/* An undone creation holds the graph, and says so. */
void testMemory()
{
  Doc d;
  test_assert(d.run(kGraphCreateCommand, {}).ok());
  const uint64_t done = d.history.memoryUsed();
  test_assert(d.undo().ok() && d.history.memoryUsed() > done);
  test_assert(d.redo().ok() && d.history.memoryUsed() == done);
}

} // namespace

int main()
{
  testParams();
  testSockets();
  testOrder();
  testBytes();
  testLiteralBytes();
  testExtras();
  testLibraries();
  testMemory();
  return test_end();
}
