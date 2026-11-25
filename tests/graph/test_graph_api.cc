/* Graphs in datapaths: reading, writing, and undoing writes. */
#include "edit_fixture.h"

#include "coredcc/graph/instance.h"

using namespace fixture;

test_init;

namespace {

Status scaleKernel(NodeExecContext &ctx)
{
  const BlockUse use = ctx.paramBlock("thing");
  const Thing *t = static_cast<const Thing *>(use.data);
  return ctx.setOutput(0, GraphValue::ofFloat(ctx.input(0).asFloat() *
                                              double(ctx.param("count")->asInt()) +
                                              (t ? t->value : 0)));
}

void addTypes(Runtime &rt)
{
  NodeTypeDef d("t.scale");
  d.uiName("Scale")
      .input(PropertyDef::floatProp("a").range(0, 10).defaultValue(PropValue::ofFloat(2)))
      .param(PropertyDef::intProp("count").range(0, 9).defaultValue(PropValue::ofInt(3)))
      .param(PropertyDef::enumProp("mode").item("FAST", 0).item("EXACT", 1))
      .param(PropertyDef::floatProp("maybe").access(PropAccess::Optional))
      .param(PropertyDef::pointerProp("thing", kThingStruct))
      .output("value", "float");
  addType<scaleKernel>(rt, d, "t.scale");
}

std::string path(const char *graph, NodeId n, const char *rest)
{
  return std::string("graphs[\"") + graph + "\"].nodes[#" + std::to_string(n.value) + "]" + rest;
}
PropValue fl(double v)
{
  return PropValue::ofFloat(v);
}
PropValue str(const char *s)
{
  return PropValue::ofString(s);
}

void testNodes()
{
  Doc d;
  addTypes(d.rt);
  const Graph g = newGraph(d.lib, "g");
  GraphDef &def = *g.def;
  const NodeId a = add(def, "t.scale"), b = add(def, "t.scale"), v = add(def, "value.float");
  const LinkId l = connect(def, v, "value", b, "a");

  /* The graph. */
  test_assert(d.is("graphs[\"g\"].name", str("g")));
  test_assert(d.is("graphs[\"g\"].nodes.length", PropValue::ofInt(3)));
  test_assert(d.is("graphs[\"g\"].nodes[1].id", PropValue::ofInt(b.value)));
  test_assert(d.is("graphs[\"g\"].nodes[2].type", str("value.float")));
  test_assert(def.removeNode(a).ok()); /* a hole in the list */
  test_assert(d.is("graphs[\"g\"].nodes[0].id", PropValue::ofInt(b.value)));
  test_assert(d.is("graphs[\"g\"].nodes[1].id", PropValue::ofInt(v.value)));
  test_assert(hasCode(d.get("graphs[\"g\"].nodes[2].id"), StatusCode::OutOfRange));
  test_assert(hasCode(d.get(path("g", a, ".id")), StatusCode::NotFound));
  test_assert(d.set("graphs[\"g\"].name", str("h")).ok() && d.is("graphs[\"h\"].name", str("h")));
  test_assert(d.undo().ok() && d.is("graphs[\"g\"].name", str("g")));

  /* Labels: a key while only one node has it. */
  test_assert(d.set(path("g", b, ".label"), str("B")).ok());
  test_assert(d.is("graphs[\"g\"].nodes[\"B\"].id", PropValue::ofInt(b.value)));
  test_assert(d.set(path("g", v, ".label"), str("B")).ok());
  test_assert(hasCode(d.get("graphs[\"g\"].nodes[\"B\"].id"), StatusCode::NotFound));
  test_assert(d.undo().ok() && d.is("graphs[\"g\"].nodes[\"B\"].id", PropValue::ofInt(b.value)));
  /* The canonical path has the id. */
  Result<ResolvedPath> rp = resolve(d.ctx, "graphs[\"g\"].nodes[\"B\"].params.count");
  test_assert(rp.ok() && rp->canonical.owner == OwnerKey::ofBlock(g.id));
  test_assert(rp->canonical.key.size() == 4 && rp->canonical.key[1] == KeySegment::ofHandle(b.value));
  test_assert(d.set(path("g", b, ".position[1]"), fl(4)).ok() && def.node(b)->y() == 4);
  test_assert(d.undo().ok() && def.node(b)->y() == 0 && d.redo().ok() && def.node(b)->y() == 4);
  test_assert(hasCode(d.set(path("g", b, ".type"), str("x")), StatusCode::FailedPrecondition));

  /* Links. */
  const std::string link = "graphs[\"g\"].links[#" + std::to_string(l.value) + "]";
  test_assert(d.is("graphs[\"g\"].links.length", PropValue::ofInt(1)));
  test_assert(d.is("graphs[\"g\"].links[0].id", PropValue::ofInt(l.value)));
  test_assert(d.is(link + ".from", PropValue::ofInt(v.value)) && d.is(link + ".to", PropValue::ofInt(b.value)));
  test_assert(d.is(link + ".fromSocket", str("value")) && d.is(link + ".toSocket", str("a")));
  test_assert(hasCode(d.get("graphs[\"g\"].links[#1].id"), StatusCode::NotFound));
  test_assert(hasCode(d.get("graphs[\"g\"].links[\"x\"].id"), StatusCode::NotFound));

  /* Params and literals: coerced, stamped, undone. */
  GraphInstance inst(&d.rt, &d.lib, g.id);
  const SocketPath out = SocketPath::of(b, "value");
  auto value = [&]() {
    test_assert(inst.evaluate(out).ok());
    return inst.value(out)->asFloat();
  };
  test_assert(def.unlink(l).ok() && value() == 6);
  test_assert(d.is(path("g", b, ".params.count"), PropValue::ofInt(3)));
  test_assert(d.is(path("g", b, ".inputs.a"), fl(2)) && d.is(path("g", b, ".params.maybe"), fl(0)));
  test_assert(d.set(path("g", b, ".params.count"), PropValue::ofInt(100)).ok() && value() == 18);
  test_assert(d.set(path("g", b, ".inputs.a"), fl(-1)).ok() && value() == 0);
  test_assert(d.is(path("g", b, ".inputs.a"), fl(0)) && d.is(path("g", b, ".params.count"), PropValue::ofInt(9)));
  test_assert(d.set(path("g", b, ".params.mode[EXACT]"), PropValue::ofBool(true)).ok());
  test_assert(def.paramValue(*def.node(b), "mode") == PropValue::ofEnum(1));
  test_assert(d.undo().ok() && d.undo().ok() && value() == 18 && d.undo().ok() && value() == 6);
  test_assert(d.redo().ok() && value() == 18);
  test_assert(hasCode(d.get(path("g", b, ".params.cuont")), StatusCode::NotFound));
  Result<PropertyDef> meta = getMeta(d.ctx, path("g", b, ".inputs.a"));
  test_assert(meta.ok() && meta->numeric()->hardMax == 10);

  /* A node of an unregistered type, until its type is. */
  NodeState s;
  s.id = NodeId{5};
  s.typeName = "t.late";
  test_assert(def.insertNode(s).ok() && d.is(path("g", s.id, ".type"), str("t.late")));
  test_assert(hasCode(d.get(path("g", s.id, ".params.k")), StatusCode::NotFound));
  const uint64_t epoch = d.rt.ext<ChangeBus>().epoch();
  NodeTypeDef late("t.late");
  late.param(PropertyDef::intProp("k")).output("value", "float");
  addType<scaleKernel>(d.rt, late, "t.late");
  test_assert(d.rt.ext<ChangeBus>().epoch() > epoch);
  test_assert(d.is(path("g", s.id, ".params.k"), PropValue::ofInt(0)));
  test_assert(d.set(path("g", s.id, ".params.k"), PropValue::ofInt(2)).ok());

  /* A struct that is in the way of a node type's. */
  test_assert(d.rt.ext<ApiRegistry>().add(ApiStruct("coredcc.GraphInputs:t.taken")).ok());
  NodeTypeDef taken("t.taken");
  taken.output("value", "float");
  Result<Owned<GraphKernel>> k = makeKernel<FnKernel<scaleKernel>>(d.rt, "t.scale#kernel");
  taken.kernel(std::move(k).value());
  test_assert(hasCode(d.rt.ext<NodeTypeRegistry>().add(std::move(taken)), StatusCode::AlreadyExists));
  for (const char *part : {"coredcc.GraphNode:t.taken", "coredcc.GraphParams:t.taken"}) {
    test_assert(!d.rt.ext<ApiRegistry>().find(StructId::of(part)));
  }
}

void testInterface()
{
  Doc d;
  addTypes(d.rt);
  const Graph inner = newGraph(d.lib, "in"), outer = newGraph(d.lib, "out");
  GraphDef &in = *inner.def, &o = *outer.def;
  const int64_t ia = addSocket(in, SocketSide::Input, "a", "float");
  const int64_t ob = addSocket(in, SocketSide::Output, "b", "float");
  const NodeId input = add(in, "graph.input"), output = add(in, "graph.output");
  connect(in, input, key(ia), output, key(ob));
  const std::string a = "graphs[\"in\"].inputs[\"a\"]";
  const std::string b = "graphs[\"in\"].outputs[#" + std::to_string(ob) + "]";
  test_assert(d.is("graphs[\"in\"].inputs.length", PropValue::ofInt(1)));
  test_assert(d.is(a + ".type", str("float")) && d.is(b + ".name", str("b")));
  test_assert(hasCode(d.get("graphs[\"in\"].outputs[\"a\"].name"), StatusCode::NotFound));
  test_assert(hasCode(d.get("graphs[\"in\"].outputs[#" + std::to_string(ia) + "].name"),
                      StatusCode::NotFound));
  test_assert(d.is(a + ".default", fl(0)) && d.set(a + ".default", fl(3)).ok());
  test_assert(in.interfaceSockets(SocketSide::Input)[0].defaultValue == fl(3));
  test_assert(hasCode(d.set(b + ".default", fl(3)), StatusCode::FailedPrecondition));
  test_assert(d.set(a + ".name", str("A")).ok() && d.undo().ok() && d.is(a + ".default", fl(3)));

  /* A group's sockets: read-only until set. */
  const NodeId g = add(o, "graph.group");
  const std::string group = path("out", g, ".group");
  test_assert(d.is(group, PropValue::nullPointer()) && d.is(path("out", g, ".sockets.length"), PropValue::ofInt(0)));
  test_assert(d.set(group, pointerTo(inner)).ok() && o.node(g)->group().target == inner.id);
  const std::string socket = path("out", g, ".sockets[\"a\"]");
  const std::string byId = path("out", g, ".sockets[#") + std::to_string(ia) + "]";
  test_assert(d.is(socket + ".value", fl(3)) && d.is(byId + ".isSet", PropValue::ofBool(false)));
  test_assert(d.is(socket + ".name", str("a")) && d.is(socket + ".type", str("float")));
  test_assert(hasCode(d.set(socket + ".value", fl(5)), StatusCode::FailedPrecondition));
  test_assert(d.set(socket + ".isSet", PropValue::ofBool(true)).ok());
  test_assert(d.is(socket + ".value", fl(3)) && d.set(byId + ".value", fl(5)).ok());
  GraphInstance inst(&d.rt, &d.lib, outer.id);
  const SocketPath out = SocketPath::of(g, key(ob));
  auto is = [&](double v) {
    return inst.evaluate(out).ok() && *inst.value(out) == GraphValue::ofFloat(v);
  };
  test_assert(is(5) && d.set(a + ".default", fl(4)).ok() && is(5));
  /* Turned off, it keeps its value; undo is exact each time. */
  test_assert(d.set(socket + ".isSet", PropValue::ofBool(false)).ok() && is(4));
  test_assert(d.is(socket + ".value", fl(4)) && d.undo().ok() && is(5));
  test_assert(d.undo().ok() && d.undo().ok() && is(3) && d.undo().ok());
  test_assert(d.is(socket + ".isSet", PropValue::ofBool(false)) && is(3));
  test_assert(d.set(a + ".default", fl(7)).ok() && is(7));
  test_assert(d.undo().ok() && d.undo().ok() && d.is(group, PropValue::nullPointer()));
  test_assert(d.redo().ok() && is(3));

  /* Types: each has a literal of its own, and the interface's default goes with it. */
  test_assert(o.setLiteral(g, key(ia), fl(2.5)).ok() && is(2.5));
  test_assert(in.setInterfaceType(ia, "int").ok() && is(0));
  test_assert(d.is(socket + ".type", str("int")) && d.is(socket + ".value", PropValue::ofInt(0)));
  test_assert(d.is(a + ".default", PropValue::ofInt(0)));
  test_assert(o.setLiteral(g, key(ia), PropValue::ofInt(9)).ok() && is(9));
  test_assert(d.is(socket + ".value", PropValue::ofInt(9)) && o.node(g)->literals().size() == 2);
  test_assert(in.setInterfaceType(ia, "float").ok() && is(2.5));
  test_assert(o.setLiteral(g, key(ia), PropValue()).ok() && is(0));
  test_assert(in.setInterfaceType(ia, "int").ok() && is(9) && in.setInterfaceType(ia, "bool").ok());
  test_assert(d.is(socket + ".value", PropValue::ofBool(false)));
  /* Switched on and off, each time with its own version. */
  test_assert(o.useLiteral(g, key(ia), true).ok() && o.setLiteral(g, key(ia), PropValue::ofBool(true)).ok());
  test_assert(is(1) && o.useLiteral(g, key(ia), false).ok() && is(0));
  test_assert(o.useLiteral(g, key(ia), true).ok() && is(1));
  test_assert(hasCode(o.useLiteral(g, "nothing", true), StatusCode::NotFound));
  test_assert(hasCode(o.useLiteral(NodeId{3}, key(ia), true), StatusCode::NotFound));

  /* While the group isn't there, the sockets last seen are read-only. */
  test_assert(d.lib.markDeleted(inner.h).ok() && d.is(socket + ".isSet", PropValue::ofBool(true)));
  for (const char *member : {".isSet", ".value"}) {
    test_assert(hasCode(d.set(socket + member, PropValue::ofBool(false)),
                        StatusCode::FailedPrecondition));
  }
  test_assert(d.lib.restoreFromHistory(inner.h).ok() && is(1));

  /* Output nodes have them too; input nodes have none. */
  test_assert(in.unlink(in.node(output)->linkInto(key(ob))->id).ok());
  const std::string outSocket = path("in", output, ".sockets[\"b\"]");
  test_assert(d.set(outSocket + ".isSet", PropValue::ofBool(true)).ok());
  test_assert(d.set(outSocket + ".value", fl(8)).ok() && is(8));
  test_assert(hasCode(d.get(path("in", input, ".sockets.length")), StatusCode::NotFound));
}

void testPointers()
{
  Doc d;
  addTypes(d.rt);
  const Graph g = newGraph(d.lib, "g"), h = newGraph(d.lib, "h");
  const BlockHandle t1 = newThing(d.lib, "t1", 10), t2 = newThing(d.lib, "t2", 20);
  const BlockId id1 = idOf(d.lib, t1), id2 = idOf(d.lib, t2);
  for (BlockHandle x : {g.h, h.h}) {
    d.lib.setFlag(x, BlockFlags::Root, true);
  }
  GraphDef &def = *g.def;
  const NodeId n = add(def, "t.scale"), group = add(def, "graph.group");
  const std::string thing = path("g", n, ".params.thing");
  const PropValue p1 = pointerTo(id1, kThingStruct), p2 = pointerTo(id2, kThingStruct);
  auto target = [&]() { return def.node(n)->blockTarget("thing"); };

  test_assert(d.is(thing, PropValue::nullPointer()) && d.set(thing, p1).ok());
  test_assert(target() == id1 && d.is(thing, p1) && d.is(thing + ".value", fl(10)));
  test_assert(d.set(thing, p2).ok() && d.undo().ok() && target() == id1);
  test_assert(d.redo().ok() && target() == id2 && d.undo().ok());
  test_assert(hasCode(d.set(thing, pointerTo(h)), StatusCode::TypeMismatch) && target() == id1);
  test_assert(d.set(thing, PropValue::nullPointer()).ok() && target().isNull());
  test_assert(d.undo().ok() && target() == id1);

  /* A link to a block that isn't Live reads as null, and undo brings the link back. */
  test_assert(d.lib.markDeleted(t1).ok() && d.is(thing, PropValue::nullPointer()));
  test_assert(d.set(thing, p2).ok() && target() == id2);
  test_assert(d.undo().ok() && target() == id1 && d.is(thing, PropValue::nullPointer()));
  test_assert(d.redo().ok() && target() == id2 && d.undo().ok());
  test_assert(d.lib.restoreFromHistory(t1).ok() && d.is(thing, p1));

  /* The def's own block: refused before anything is written (a struct type that may be
   * anything, so that only the def can refuse it). */
  NodeTypeDef any("t.any");
  any.param(PropertyDef::pointerProp("graph", StructId::of(kGraphDefType))).output("value", "float");
  addType<scaleKernel>(d.rt, any, "t.any");
  const NodeId m = add(def, "t.any");
  const std::string own = path("g", m, ".params.graph");
  test_assert(d.set(own, pointerTo(h)).ok() && d.is(own, pointerTo(h)));
  const uint64_t edit = d.rt.lastEdit(), version = def.node(m)->paramVersion();
  const int steps = d.history.size();
  test_assert(hasCode(d.set(own, pointerTo(g)), StatusCode::InvalidArgument));
  test_assert(d.rt.lastEdit() == edit && def.node(m)->paramVersion() == version);
  test_assert(def.node(m)->blockTarget("graph") == h.id && d.history.size() == steps);
  /* Only a direct write makes one. It reads as null, can be replaced, and comes back. */
  struct Redirect {
    Library *lib;
    BlockId from, to;
  } redirect{&d.lib, h.id, g.id};
  RefVisitor v(
      [](void *p, RefCell &cell) {
        const Redirect &r = *static_cast<Redirect *>(p);
        if (cell.target == r.from) {
          test_assert(r.lib->writeRef(cell, r.to).ok());
        }
      },
      &redirect);
  def.foreachRef(v);
  test_assert(d.is(own, PropValue::nullPointer()) && def.node(m)->blockTarget("graph") == g.id);
  test_assert(d.set(own, pointerTo(h)).ok() && d.undo().ok());
  test_assert(def.node(m)->blockTarget("graph") == g.id && d.redo().ok());
  test_assert(def.node(m)->blockTarget("graph") == h.id);

  /* Only a whole block can be a target, not something in it. */
  NodeTypeDef part("t.part");
  part.param(PropertyDef::pointerProp("node", StructId::of(kGraphNodeStruct)))
      .output("value", "float");
  addType<scaleKernel>(d.rt, part, "t.part");
  const NodeId q = add(def, "t.part");
  Result<ResolvedPath> inside = resolve(d.ctx, path("h", add(*h.def, "t.scale"), ""));
  test_assert(inside.ok());
  test_assert(hasCode(d.set(path("g", q, ".params.node"),
                            PropValue::ofPointer(inside->canonical)),
                      StatusCode::InvalidArgument));
  test_assert(def.node(q)->blockTarget("node").isNull());

  /* Groups. */
  const std::string to = path("g", group, ".group");
  test_assert(d.set(to, pointerTo(h)).ok() && d.is(to, pointerTo(h)));
  test_assert(hasCode(d.set(to, pointerTo(g)), StatusCode::InvalidArgument));
  test_assert(d.is(to, pointerTo(h)) && hasCode(d.set(to, p1), StatusCode::TypeMismatch));
  test_assert(d.lib.markDeleted(h.h).ok() && d.is(to, PropValue::nullPointer()));
  test_assert(d.set(to, PropValue::nullPointer()).ok() && def.node(group)->group().isNull());
  test_assert(d.undo().ok() && def.node(group)->group().target == h.id);
  test_assert(d.lib.restoreFromHistory(h.h).ok() && d.is(to, pointerTo(h)));
  test_assert(d.set(thing, p2).ok() && d.set(thing, p1).ok());
  test_assert(d.lib.usersOf(h.h).size() == 1 && d.lib.collect().ok());
  test_assert(d.lib.get(t1) && d.lib.get(t2)); /* pinned by the history */
  d.history.clear();
  test_assert(d.lib.collect().ok() && d.lib.get(t1) && !d.lib.get(t2));
}

/* A standalone def that a host shows inside a struct of its own. */
void testEmbedded()
{
  Doc d;
  addTypes(d.rt);
  GraphDef def(&d.rt, &d.lib);
  const NodeId n = add(def, "t.scale");
  const int64_t ia = addSocket(def, SocketSide::Input, "a", "float");
  ApiRegistry &reg = d.rt.ext<ApiRegistry>();
  ApiStruct host("Host");
  PointerAccessor a;
  a.get = [](const PointerRef &self, Context &) {
    PointerRef r;
    r.data = self.data;
    r.type = StructId::of(kGraphDefType);
    return r;
  };
  a.target = StructId::of(kGraphDefType);
  host.addPointer(PropertyDef::pointerProp("graph", a.target), std::move(a));
  test_assert(reg.add(std::move(host)).ok());
  test_assert(reg.addRuntimeOwner("host", StructId::of("Host"), [&def]() { return &def; }).ok());
  Context ctx(&d.rt, &d.lib, reg.runtimeOwner("host"));
  ctx.setExecutor(&d.executor);
  const std::string count = "graph.nodes[#" + std::to_string(n.value) + "].params.count";
  test_assert(setValueUndo(ctx, count, PropValue::ofInt(5)).ok());
  test_assert(def.paramValue(*def.node(n), "count") == PropValue::ofInt(5));
  test_assert(d.executor.undo(ctx).ok());
  test_assert(def.paramValue(*def.node(n), "count") == PropValue::ofInt(3));
  test_assert(setValue(ctx, "graph.inputs[0].default", fl(2)).ok());
  test_assert(def.interfaceSocket(ia)->defaultValue == fl(2));
  test_assert(getValue(ctx, "graph.name")->asString().size() == 0);
  test_assert(hasCode(setValue(ctx, "graph.name", str("x")), StatusCode::FailedPrecondition));
  d.history.clear();
}

} // namespace

int main()
{
  testNodes();
  testInterface();
  testPointers();
  testEmbedded();
  return test_end();
}
