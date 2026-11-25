/* GraphInstance: what is evaluated, what runs, and what errors do. */
#include "graph_fixture.h"

#include "coredcc/graph/instance.h"

using namespace fixture;

test_init;

namespace {

struct Mesh {
  int verts = 0;
};
struct Cloud {
  int points = 0;
};

} // namespace

template <> struct coredcc::PayloadTraits<Mesh> {
  static constexpr const char *name = "test.Mesh";
};
template <> struct coredcc::PayloadTraits<Cloud> {
  static constexpr const char *name = "test.Cloud";
  static bool equal(const Cloud &a, const Cloud &b)
  {
    return a.points == b.points;
  }
};

namespace {

/* a + b, but fails when a is negative. */
Status sum(NodeExecContext &ctx)
{
  const double a = ctx.input("a")->asFloat();
  if (a < 0) {
    CDCC_TRY(ctx.setOutput("value", GraphValue::ofFloat(123)));
    return Status::error(StatusCode::OutOfRange,
                         ("a is " + std::to_string(int(a))).c_str());
  }
  return ctx.setOutput("value", GraphValue::ofFloat(a + ctx.input("b")->asFloat()));
}

/* The number of inputs that are linked, and the two halves of the sign of a. */
Status split(NodeExecContext &ctx)
{
  const double a = ctx.input(0).asFloat();
  CDCC_TRY(ctx.setOutput("sign", GraphValue::ofFloat(a < 0 ? -1 : 1)));
  return ctx.setOutput("linked", GraphValue::ofInt(ctx.isLinked(0) ? 1 : 0));
}

/* What is wrong with its outputs or its Runtime goes by the mode. */
Status bad(NodeExecContext &ctx)
{
  switch (ctx.param("mode")->asInt()) {
    case 1:
      return ctx.setOutput("x", GraphValue::ofFloat(1)); /* not y */
    case 2:
      ctx.setOutput("x", GraphValue::ofInt(1)).ignore();
      break;
    case 3:
      ctx.runtime().setStickyError(Status::error(StatusCode::Cancelled, "thrown"));
      break;
    case 4:
      ctx.runtime().setStickyError(Status::error(StatusCode::Cancelled, "thrown"));
      return Status::error(StatusCode::Unsupported, "returned");
    default:
      break;
  }
  ctx.setOutput("x", GraphValue::ofFloat(double(ctx.param("mode")->asInt()))).ignore();
  return ctx.setOutput("y", GraphValue::ofFloat(2));
}

Status makeMesh(NodeExecContext &ctx)
{
  const int n = int(ctx.input(0).asInt());
  CDCC_TRY(ctx.setOutput("mesh", GraphValue::ofPayload(makePayload(Mesh{n}))));
  return ctx.setOutput("cloud", GraphValue::ofPayload(makePayload(Cloud{n / 10})));
}

Status countMesh(NodeExecContext &ctx)
{
  const Mesh *m = ctx.input(0).payload<Mesh>();
  return ctx.setOutput(0, GraphValue::ofInt(m ? m->verts : -1));
}

Status countCloud(NodeExecContext &ctx)
{
  const Cloud *c = ctx.input(0).payload<Cloud>();
  return ctx.setOutput(0, GraphValue::ofInt(c ? c->points : -1));
}

Status readUser(NodeExecContext &ctx)
{
  const int *p = static_cast<const int *>(ctx.userData());
  return ctx.setOutput(0, GraphValue::ofInt(p ? *p : -1));
}

void addTypes(Runtime &rt)
{
  SocketTypeRegistry &sockets = rt.ext<SocketTypeRegistry>();
  test_assert(sockets.addPayloadType<Mesh>().ok() && sockets.addPayloadType<Cloud>().ok());
  NodeTypeDef s("t.sum");
  s.input(PropertyDef::floatProp("a"))
      .input(PropertyDef::floatProp("b").defaultValue(PropValue::ofFloat(10)))
      .output("value", "float");
  addType<sum>(rt, s, "t.sum");
  NodeTypeDef p("t.split");
  p.input(PropertyDef::floatProp("a")).output("sign", "float").output("linked", "int");
  addType<split>(rt, p, "t.split");
  NodeTypeDef b("t.bad");
  b.param(PropertyDef::intProp("mode")).output("x", "float").output("y", "float");
  addType<bad>(rt, b, "t.bad");
  NodeTypeDef m("t.mesh");
  m.input(PropertyDef::intProp("n")).output("mesh", "test.Mesh").output("cloud", "test.Cloud");
  addType<makeMesh>(rt, m, "t.mesh");
  NodeTypeDef c("t.countMesh");
  c.payloadInput("mesh", "test.Mesh").output("n", "int");
  addType<countMesh>(rt, c, "t.countMesh");
  NodeTypeDef d("t.countCloud");
  d.payloadInput("cloud", "test.Cloud").output("n", "int");
  addType<countCloud>(rt, d, "t.countCloud");
  NodeTypeDef u("t.user");
  u.output("n", "int");
  addType<readUser>(rt, u, "t.user");
}

struct World {
  Runtime rt;
  Library lib;
  World() : lib(&rt)
  {
    addThings(rt);
    test_assert(registerGraphBlocks(rt).ok());
    addTypes(rt);
  }
  ~World()
  {
    test_assert(rt.validateAll().ok() && lib.validate().ok() && !rt.hasStickyError());
  }
};

SocketPath out(NodeId n, const char *socket = "value")
{
  return SocketPath::of(n, socket);
}
NodePath at(NodeId n, std::initializer_list<NodeId> groups = {})
{
  NodePath p;
  p.node = n;
  for (NodeId g : groups) {
    p.groups.append(g);
  }
  return p;
}
double num(GraphInstance &g, const SocketPath &p)
{
  const Status s = g.evaluate(p);
  if (!s.ok()) {
    printf("%s\n", s.toString().c_str());
    return -999999;
  }
  const GraphValue *v = g.value(p);
  return v->type() == GraphValue::Type::Int ? double(v->asInt()) : v->asFloat();
}
PropValue fl(double v)
{
  return PropValue::ofFloat(v);
}

/* Kernel runs since the last call. */
struct Runs {
  GraphInstance &g;
  uint64_t last;
  explicit Runs(GraphInstance &i) : g(i), last(i.totalRuns()) {}
  int take()
  {
    const uint64_t now = g.totalRuns();
    const int n = int(now - last);
    last = now;
    return n;
  }
};

void testIncremental()
{
  World w;
  GraphDef def(&w.rt);
  const NodeId a = add(def, "value.float"), b = add(def, "value.float");
  const NodeId s = add(def, "t.sum"), p = add(def, "t.split"), q = add(def, "t.sum");
  const NodeId other = add(def, "t.sum");
  connect(def, a, "value", s, "a");
  connect(def, b, "value", s, "b");
  connect(def, s, "value", p, "a");
  connect(def, p, "sign", q, "a");
  connect(def, b, "value", other, "a");
  GraphInstance g(&w.rt, nullptr, &def);
  Runs runs(g);
  test_assert(!g.value(out(q)) && !g.isCurrent(out(q)) && g.changedAt(out(q)) == 0);
  test_assert(num(g, out(q)) == 11 && runs.take() == 5); /* not `other`: it wasn't asked for */
  test_assert(g.isCurrent(out(q)) && !g.isCurrent(out(s)) && !g.value(out(s)));
  test_assert(num(g, out(s)) == 0 && num(g, out(q)) == 11 && runs.take() == 0);

  /* An edit runs what it reaches, and stops where a value is the same again. */
  test_assert(def.setParam(a, "value", fl(3)).ok());
  test_assert(!g.isCurrent(out(q)));
  const uint64_t qAt = g.changedAt(out(q)), sAt = g.changedAt(out(s));
  test_assert(num(g, out(q)) == 11 && runs.take() == 3); /* a, s, p */
  test_assert(g.changedAt(out(q)) == qAt);
  test_assert(num(g, out(s)) == 3 && g.changedAt(out(s)) > sAt && runs.take() == 0);
  test_assert(def.setParam(a, "value", fl(3)).ok()); /* the same again */
  test_assert(num(g, out(q)) == 11 && runs.take() == 1);

  /* Two sources of one node change: one run. A source that nobody asks for doesn't run. */
  test_assert(def.setParam(a, "value", fl(1)).ok() && def.setParam(b, "value", fl(2)).ok());
  test_assert(num(g, out(s)) == 3 && runs.take() == 3);
  test_assert(num(g, out(q)) == 11 && runs.take() == 0); /* s is 3 again */
  test_assert(num(g, out(other)) == 12 && runs.take() == 1);

  /* Labels, positions, and nodes off to the side run nothing; nor does an empty touch. */
  test_assert(def.setLabel(s, "x").ok() && def.setPosition(s, 1, 1).ok());
  test_assert(g.isCurrent(out(q)));
  const NodeId extra = add(def, "t.sum");
  test_assert(!g.isCurrent(out(q)));
  w.rt.touch();
  test_assert(num(g, out(q)) == 11 && runs.take() == 0);
  test_assert(def.removeNode(extra).ok() && num(g, out(q)) == 11 && runs.take() == 0);

  /* Literals: only of the input that reads it, and only if it isn't linked. */
  const LinkId l = def.node(s)->linkInto("b")->id;
  test_assert(def.unlink(l).ok() && num(g, out(s)) == 11 && runs.take() == 1); /* default 10 */
  test_assert(def.setLiteral(s, "b", fl(5)).ok() && num(g, out(s)) == 6 && runs.take() == 1);
  test_assert(def.setLiteral(s, "a", fl(5)).ok() && num(g, out(s)) == 6 && runs.take() == 0);
  test_assert(def.setLiteral(s, "b", PropValue()).ok() && num(g, out(s)) == 11);
  test_assert(runs.take() == 1 && def.setLiteral(s, "b", fl(1)).ok());
  /* Replaced by a link whose value is the same: the source is another, so it runs. */
  test_assert(num(g, out(s)) == 2 && runs.take() == 1);
  connect(def, a, "value", s, "b");
  test_assert(num(g, out(s)) == 2 && runs.take() == 1);
  test_assert(num(g, SocketPath::of(p, "linked")) == 1 && runs.take() == 1); /* s moved */
  test_assert(def.unlink(def.node(p)->linkInto("a")->id).ok());
  test_assert(num(g, SocketPath::of(p, "linked")) == 0 && runs.take() == 1);

  /* A node that comes back under its id, and one replaced by another type. */
  NodeState state = def.stateOf(*def.node(other));
  const LinkState link = GraphDef::stateOf(*def.node(other)->linksIn()[0]);
  test_assert(def.removeNode(other).ok());
  test_assert(hasCode(g.evaluate(out(other)), StatusCode::NotFound));
  test_assert(def.insertNode(state).ok() && def.insertLink(link).ok());
  test_assert(num(g, out(other)) == 12);
  test_assert(def.removeNode(other).ok());
  state.typeName = "math.float";
  test_assert(def.insertNode(state).ok() && num(g, out(other)) == 0);
  test_assert(def.setLiteral(other, "a", fl(4)).ok() && num(g, out(other)) == 4);

  /* Many sockets in one call, unknown ones among them. */
  const SocketPath paths[] = {
      out(s), out(q, "nothing"), out(NodeId{7}), SocketPath::of(q, "a", SocketSide::Input),
      SocketPath::output("nothing"), SocketPath::of(at(q, {s}), "value")};
  litestl::util::Vector<Status> results;
  test_assert(g.evaluate(paths, results).ok() && results.size() == 6);
  test_assert(results[0].ok() && results[3].ok());
  for (int i : {1, 2, 4, 5}) {
    test_assert(hasCode(results[i], StatusCode::NotFound) && !g.value(paths[i]));
  }
  test_assert(*g.value(paths[3]) == GraphValue::ofFloat(1)); /* sign, as q's input */

  /* A fresh instance agrees; a cleared one starts again. */
  GraphInstance fresh(&w.rt, nullptr, &def);
  test_assert(num(fresh, out(q)) == num(g, out(q)));
  runs.take();
  test_assert(g.clear().ok() && !g.value(out(q)) && num(g, out(q)) == 11 && runs.take() > 0);
}

void testParts()
{
  World w;
  GraphDef def(&w.rt);
  const NodeId n = add(def, "value.int"), m = add(def, "t.mesh");
  const NodeId c = add(def, "t.countMesh"), d = add(def, "t.countCloud");
  connect(def, n, "value", m, "n");
  connect(def, m, "mesh", c, "mesh");
  connect(def, m, "cloud", d, "cloud");
  GraphInstance g(&w.rt, nullptr, &def);
  Runs runs(g);
  test_assert(def.setParam(n, "value", PropValue::ofInt(12)).ok());
  test_assert(num(g, out(c, "n")) == 12 && num(g, out(d, "n")) == 1 && runs.take() == 4);
  /* A payload type with `equal` cuts off; one without compares objects. Per output. */
  test_assert(def.setParam(n, "value", PropValue::ofInt(13)).ok());
  test_assert(num(g, out(c, "n")) == 13 && num(g, out(d, "n")) == 1 && runs.take() == 3);
  /* An equal payload keeps the old object. */
  test_assert(g.evaluate(out(m, "cloud")).ok());
  const PayloadRef cloud = g.value(out(m, "cloud"))->asPayload();
  test_assert(def.setParam(n, "value", PropValue::ofInt(14)).ok());
  test_assert(g.evaluate(out(m, "cloud")).ok());
  test_assert(g.value(out(m, "cloud"))->asPayload().same(cloud));

  const NodeId u = add(def, "t.user");
  int user = 4;
  g.setUserData(&user);
  test_assert(num(g, out(u, "n")) == 4);
  user = 5; /* not a dependency */
  w.rt.touch();
  test_assert(num(g, out(u, "n")) == 4);
  test_assert(g.report(at(u)).runs == 1 && g.report(at(u)).state == NodeReport::State::Ok);
  test_assert(g.report(at(u)).seconds >= 0);
  test_assert(g.report(at(NodeId{3})).state == NodeReport::State::NotEvaluated);
  test_assert(g.report(at(u, {u})).runs == 0);
}

void testErrors()
{
  World w;
  GraphDef def(&w.rt);
  const NodeId a = add(def, "value.float"), f = add(def, "t.sum"), g1 = add(def, "t.sum");
  const NodeId g2 = add(def, "t.sum"), ok = add(def, "t.sum");
  connect(def, a, "value", f, "a");
  connect(def, f, "value", g1, "a");
  connect(def, g1, "value", g2, "b");
  connect(def, a, "value", g2, "a");
  connect(def, a, "value", ok, "b");
  GraphInstance g(&w.rt, nullptr, &def);
  Runs runs(g);
  test_assert(def.setParam(a, "value", fl(1)).ok());
  test_assert(num(g, out(g2)) == 22 && num(g, out(f)) == 11 && num(g, out(ok)) == 1);
  runs.take();

  /* f fails: so do those below it, with f as the origin. `ok` doesn't care. */
  const uint64_t fAt = g.changedAt(out(f)), g1At = g.changedAt(out(g2));
  test_assert(def.setParam(a, "value", fl(-5)).ok());
  test_assert(hasCode(g.evaluate(out(f)), StatusCode::OutOfRange) && runs.take() == 2);
  test_assert(!g.value(out(f)) && *g.lastGood(out(f)) == GraphValue::ofFloat(11));
  test_assert(g.changedAt(out(f)) > fAt);
  test_assert(hasCode(g.evaluate(out(g2)), StatusCode::OutOfRange) && runs.take() == 0);
  test_assert(g.error(out(g2))->origin == at(f) && g.error(out(g2))->socket.size() == 0);
  test_assert(strcmp(g.error(out(g2))->status.message(), "a is -5") == 0);
  test_assert(*g.lastGood(out(g2)) == GraphValue::ofFloat(22) && g.changedAt(out(g2)) > g1At);
  test_assert(g.report(at(f)).state == NodeReport::State::Failed);
  test_assert(g.report(at(g1)).state == NodeReport::State::InputFailed);
  test_assert(g.report(at(g1)).error.origin == at(f) && g.report(at(g1)).runs == 1);
  test_assert(num(g, out(ok)) == -5 && runs.take() == 1);

  /* It isn't run again until something changed, or on retry. */
  w.rt.touch();
  test_assert(!g.evaluate(out(g2)).ok() && runs.take() == 0);
  const uint64_t stays = g.changedAt(out(g2));
  test_assert(g.retry().ok() && !g.isCurrent(out(g2)));
  test_assert(!g.evaluate(out(g2)).ok() && runs.take() == 1);
  test_assert(g.changedAt(out(g2)) == stays); /* the same error again */
  test_assert(def.setParam(a, "value", fl(-6)).ok());
  test_assert(!g.evaluate(out(g2)).ok() && runs.take() == 2);
  test_assert(g.changedAt(out(g2)) > stays); /* another message */
  test_assert(!g.isCurrent(out(f)) && strcmp(g.error(out(f))->status.message(), "a is -5") == 0);

  /* Back to the old value: f and those below are out of error, so they move. */
  const uint64_t before = g.changedAt(out(g2));
  test_assert(def.setParam(a, "value", fl(1)).ok());
  test_assert(num(g, out(g2)) == 22 && g.changedAt(out(g2)) > before && !g.error(out(g2)));
  test_assert(num(g, out(f)) == 11 && g.report(at(g1)).state == NodeReport::State::Ok);

  /* The first failing input, in input order, whichever was evaluated first. */
  const NodeId both = add(def, "t.sum"), f2 = add(def, "t.sum");
  test_assert(def.setLiteral(f2, "a", fl(-9)).ok() && def.setParam(a, "value", fl(-5)).ok());
  connect(def, f2, "value", both, "a");
  connect(def, f, "value", both, "b");
  test_assert(!g.evaluate(out(f)).ok() && !g.evaluate(out(both)).ok());
  test_assert(g.error(out(both))->origin == at(f2));
  connect(def, f, "value", both, "a");
  connect(def, f2, "value", both, "b");
  test_assert(!g.evaluate(out(both)).ok() && g.error(out(both))->origin == at(f));
  /* Both inputs were visited, though the first had failed. */
  test_assert(g.report(at(f2)).state == NodeReport::State::Failed);

  /* Kernels that break the rules. */
  const NodeId b = add(def, "t.bad");
  test_assert(num(g, out(b, "x")) == 0 && num(g, out(b, "y")) == 2);
  const struct {
    int mode;
    StatusCode code;
    const char *message;
  } cases[] = {{1, StatusCode::Internal, "the kernel didn't set output 'y'"},
               {2, StatusCode::Internal, "output 'x' is a float, but was set to a Int value"},
               {3, StatusCode::Cancelled, "thrown"},
               {4, StatusCode::Unsupported, "returned"}};
  for (const auto &c : cases) {
    test_assert(def.setParam(b, "mode", PropValue::ofInt(c.mode)).ok());
    test_assert(hasCode(g.evaluate(out(b, "x")), c.code));
    test_assert(strcmp(g.error(out(b, "x"))->status.message(), c.message) == 0);
    test_assert(!w.rt.hasStickyError() && g.error(out(b, "x"))->origin == at(b));
    test_assert(*g.lastGood(out(b, "x")) == GraphValue::ofFloat(0)); /* nothing committed */
  }
  test_assert(def.setParam(b, "mode", PropValue::ofInt(7)).ok() && num(g, out(b, "x")) == 7);

  /* A socket of an unknown type: of a group whose graph is gone, only the snapshot is left. */
  NodeState lost;
  lost.id = NodeId{77};
  lost.typeName = "graph.group";
  lost.inputs.append(SocketInfo{"#9", "x", "no.such"});
  test_assert(def.insertNode(lost).ok());
  const SocketPath lostIn = SocketPath::of(lost.id, "#9", SocketSide::Input);
  for (int linked = 0; linked < 2; linked++) {
    test_assert(hasCode(g.evaluate(lostIn), StatusCode::NotFound));
    test_assert(g.error(lostIn)->origin == at(lost.id) &&
                strcmp(g.error(lostIn)->socket.c_str(), "#9") == 0);
    LinkState l;
    l.id = LinkId{78};
    l.from = b;
    l.fromKey = "x";
    l.to = lost.id;
    l.toKey = "#9";
    test_assert(linked || def.insertLink(l).ok());
  }
  test_assert(def.removeNode(lost.id).ok());

  /* A pending sticky error isn't the graph's. */
  w.rt.setStickyError(Status::error(StatusCode::Internal, "someone else's"));
  litestl::util::Vector<Status> results;
  results.append(Status());
  const SocketPath path = out(b, "x");
  test_assert(hasCode(g.evaluate({&path, 1}, results), StatusCode::FailedPrecondition));
  test_assert(results.size() == 0 && hasCode(w.rt.takeStickyError(), StatusCode::Internal));

  /* A type that isn't registered (yet). */
  NodeState s;
  s.id = NodeId{55};
  s.typeName = "late.sum";
  s.inputs.append(SocketInfo{"a", "a", "float"});
  s.outputs.append(SocketInfo{"value", "value", "float"});
  test_assert(def.insertNode(s).ok());
  connect(def, b, "x", s.id, "a");
  connect(def, s.id, "value", g1, "b");
  test_assert(def.setParam(a, "value", fl(1)).ok());
  test_assert(hasCode(g.evaluate(out(g1)), StatusCode::NotFound));
  test_assert(g.error(out(g1))->origin == at(s.id));
  NodeTypeDef late("late.sum");
  late.input(PropertyDef::floatProp("a")).input(PropertyDef::floatProp("b")).output("value", "float");
  addType<sum>(w.rt, late, "late.sum");
  test_assert(num(g, out(g1)) == 18); /* 11 + 7 */
}

void testGroups()
{
  World w;
  const Graph inner = newGraph(w.lib, "inner"), outer = newGraph(w.lib, "outer");
  GraphDef &in = *inner.def, &o = *outer.def;
  const int64_t ia = addSocket(in, SocketSide::Input, "a", "float");
  const int64_t ib = addSocket(in, SocketSide::Input, "b", "float");
  const int64_t sumOut = addSocket(in, SocketSide::Output, "sum", "float");
  const int64_t pass = addSocket(in, SocketSide::Output, "pass", "float");
  const NodeId input = add(in, "graph.input"), output = add(in, "graph.output");
  const NodeId s = add(in, "t.sum");
  connect(in, input, key(ia), s, "a");
  connect(in, input, key(ib), s, "b");
  connect(in, s, "value", output, key(sumOut));
  connect(in, input, key(ib), output, key(pass));

  /* A root's interface inputs: a default, then a value set on the instance. */
  GraphInstance alone(&w.rt, &w.lib, inner.id);
  Runs runs(alone);
  test_assert(num(alone, SocketPath::output("sum")) == 0 && runs.take() == 1);
  test_assert(in.setInterfaceDefault(ib, fl(4)).ok());
  test_assert(num(alone, SocketPath::output("sum")) == 4 && runs.take() == 1);
  test_assert(num(alone, SocketPath::output(key(pass))) == 4 && runs.take() == 0);
  test_assert(alone.setInput("b", GraphValue::ofFloat(6)).ok());
  test_assert(!alone.isCurrent(SocketPath::output("sum")));
  test_assert(num(alone, SocketPath::output("sum")) == 6 && runs.take() == 1);
  test_assert(alone.setInput("b", GraphValue::ofFloat(6)).ok());
  test_assert(alone.isCurrent(SocketPath::output("sum")));
  test_assert(in.setInterfaceDefault(ib, fl(5)).ok()); /* hidden by the set value */
  test_assert(num(alone, SocketPath::output("sum")) == 6 && runs.take() == 0);
  test_assert(hasCode(alone.setInput("b", GraphValue::ofInt(1)), StatusCode::TypeMismatch));
  const Status unknown = alone.setInput("bb", GraphValue::ofInt(1));
  test_assert(hasCode(unknown, StatusCode::NotFound));
  test_assert(alone.setInput("b", GraphValue()).ok());
  test_assert(num(alone, SocketPath::output("sum")) == 5 && runs.take() == 1);
  test_assert(num(alone, SocketPath::of(input, key(ib))) == 5);
  test_assert(num(alone, SocketPath::of(s, "b", SocketSide::Input)) == 5 && runs.take() == 0);
  test_assert(alone.setInput("b", GraphValue::ofFloat(6)).ok());
  test_assert(in.setInterfaceType(ib, "int").ok()); /* now it doesn't hold */
  test_assert(num(alone, SocketPath::output("pass")) == 0);
  test_assert(in.setInterfaceType(ib, "float").ok());
  test_assert(num(alone, SocketPath::output("pass")) == 6);
  test_assert(in.setInterfaceDefault(ib, fl(5)).ok()); /* it went with the type */

  /* Two groups of it, with state of their own. */
  const NodeId g1 = add(o, "graph.group"), g2 = add(o, "graph.group"), v = add(o, "value.float");
  test_assert(o.setGroup(g1, inner.id).ok() && o.setGroup(g2, inner.id).ok());
  connect(o, v, "value", g1, key(ia));
  connect(o, g1, key(sumOut), g2, key(ia));
  connect(o, g1, key(pass), g2, key(ib));
  GraphInstance g(&w.rt, &w.lib, outer.id);
  Runs gr(g);
  const SocketPath last = SocketPath::of(g2, key(sumOut));
  test_assert(num(g, last) == 10 && gr.take() == 3 && g.frameCount() == 2); /* 5 + 5 */
  test_assert(g.report(at(s, {g1})).runs == 1 && g.report(at(s, {g2})).runs == 1);
  test_assert(num(g, SocketPath::of(at(s, {g1}), "value")) == 5);
  test_assert(num(g, SocketPath::of(at(s, {g2}), "a", SocketSide::Input)) == 5);
  test_assert(num(g, SocketPath::of(at(input, {g2}), key(ia))) == 5 && gr.take() == 0);
  test_assert(hasCode(g.evaluate(SocketPath::of(at(s, {v}), "value")), StatusCode::NotFound));
  test_assert(o.setParam(v, "value", fl(1)).ok() && num(g, last) == 11 && gr.take() == 3);

  /* A literal on a group hides the interface's default, and only its readers run. */
  test_assert(o.setLiteral(g1, key(ib), fl(7)).ok());
  test_assert(num(g, last) == 15 && gr.take() == 2); /* 8 + 7 */
  test_assert(in.setInterfaceDefault(ib, fl(9)).ok() && num(g, last) == 15 && gr.take() == 0);
  test_assert(o.setLiteral(g1, key(ia), fl(3)).ok() && num(g, last) == 15 && gr.take() == 0);
  test_assert(o.setLiteral(g1, key(ib), PropValue()).ok() && num(g, last) == 19);
  gr.take();

  /* An edit inside shows in every group of it, through any number of instances. */
  test_assert(in.setLiteral(s, "b", fl(100)).ok()); /* linked: nothing */
  test_assert(num(g, last) == 19 && gr.take() == 0);
  const LinkId link = in.node(s)->linkInto("b")->id;
  const LinkState state = GraphDef::stateOf(*in.link(link));
  test_assert(in.unlink(link).ok() && num(g, last) == 201 && gr.take() == 2);
  test_assert(num(alone, SocketPath::output("sum")) == 100);
  test_assert(in.insertLink(state).ok() && num(g, last) == 19 && gr.take() == 2);

  /* Interface changes: what an input resolves to changes with no edit of its own def. */
  test_assert(o.setLiteral(g1, key(ib), fl(9)).ok() && num(g, last) == 19);
  gr.take();
  test_assert(in.removeInterface(pass).ok()); /* g2.b is now unlinked: the default, also 9 */
  test_assert(num(g, last) == 19 && gr.take() == 1);
  test_assert(o.setLiteral(g1, key(ib), PropValue()).ok());
  test_assert(in.setInterfaceDefault(ib, fl(2)).ok() && num(g, last) == 5);
  test_assert(in.addInterface(SocketSide::Output, "pass", "float", 0, pass).ok());
  test_assert(num(g, last) == 5);
  gr.take();
  test_assert(in.moveInterface(pass, 1).ok() && num(g, last) == 5 && gr.take() == 0);
  test_assert(in.setInterfaceName(pass, "Pass").ok() && num(g, last) == 5 && gr.take() == 0);

  /* A literal that isn't of its socket's type any more counts as unset. */
  test_assert(o.unlink(o.node(g2)->linkInto(key(ib))->id).ok() && num(g, last) == 5);
  const SocketPath g2b = SocketPath::of(g2, key(ib), SocketSide::Input);
  test_assert(o.setLiteral(g2, key(ib), fl(2.5)).ok() && num(g, last) == 5.5);
  test_assert(in.setInterfaceType(ib, "int").ok() && num(g, last) == 1); /* 1 + 0 + 0 */
  test_assert(g.evaluate(g2b).ok() && *g.value(g2b) == GraphValue::ofInt(0));
  test_assert(in.setInterfaceType(ib, "float").ok());
  test_assert(in.setInterfaceDefault(ib, fl(2)).ok() && num(g, last) == 5.5);
  /* Two literals of one insert have one version; which one is read still counts. */
  test_assert(o.unlink(o.node(g1)->linkInto(key(ia))->id).ok());
  test_assert(o.setLiteral(g1, key(ia), fl(1)).ok() && o.setLiteral(g1, key(ib), fl(30)).ok());
  const NodeState both = o.stateOf(*o.node(g1));
  const LinkState fromG1 = GraphDef::stateOf(*o.node(g1)->linksOut()[0]);
  test_assert(o.removeNode(g1).ok() && o.insertNode(both).ok() && o.insertLink(fromG1).ok());
  test_assert(num(g, last) == 33.5);
  const LinkState sb = GraphDef::stateOf(*in.node(s)->linkInto("b"));
  test_assert(in.unlink(sb.id).ok());
  connect(in, input, key(ia), s, "b"); /* for g1: the literal 1 instead of the 30 */
  test_assert(num(g, last) == 4);      /* 1 + 1, and that doubled */
  test_assert(in.unlink(in.node(s)->linkInto("b")->id).ok() && in.insertLink(sb).ok());
  test_assert(o.setLiteral(g2, key(ib), PropValue()).ok());
  test_assert(o.setLiteral(g1, key(ib), PropValue()).ok());
  connect(o, v, "value", g1, key(ia));
  test_assert(num(g, last) == 5);
  gr.take();

  /* Deleting the def, and bringing it back. */
  test_assert(w.lib.markDeleted(inner.h).ok());
  test_assert(hasCode(g.evaluate(last), StatusCode::NotFound));
  test_assert(g.error(last)->origin == at(g2) && *g.lastGood(last) == GraphValue::ofFloat(5));
  test_assert(g.report(at(g2)).state == NodeReport::State::Failed);
  test_assert(hasCode(alone.evaluate(SocketPath::output("sum")), StatusCode::NotFound));
  test_assert(w.lib.restoreFromHistory(inner.h).ok() && num(g, last) == 5 && gr.take() == 0);
  test_assert(g.report(at(g2)).state == NodeReport::State::NotEvaluated);
  test_assert(num(alone, SocketPath::output("sum")) == 6); /* 0 + 6, set above */

  /* Another def, and none. */
  const Graph copy = newGraph(w.lib, "copy");
  const int64_t ia2 = *copy.def->addInterface(SocketSide::Input, "a", "float", -1, ia);
  test_assert(copy.def->addInterface(SocketSide::Output, "sum", "float", -1, sumOut).ok());
  const NodeId cin = add(*copy.def, "graph.input"), cout = add(*copy.def, "graph.output");
  connect(*copy.def, cin, key(ia2), cout, key(sumOut));
  test_assert(o.setGroup(g1, copy.id).ok() && num(g, last) == 3); /* 1 + the default 2 */
  test_assert(o.setGroup(g1, BlockId()).ok());
  test_assert(hasCode(g.evaluate(last), StatusCode::NotFound) && g.error(last)->origin == at(g1));
  test_assert(o.setGroup(g1, inner.id).ok() && num(g, last) == 5 && g.frameCount() == 2);
  test_assert(o.removeNode(g1).ok() && num(g, last) == 2 && g.frameCount() == 1);

  /* retry reaches the nodes in groups. */
  test_assert(o.setLiteral(g2, key(ia), fl(-1)).ok());
  test_assert(hasCode(g.evaluate(last), StatusCode::OutOfRange));
  test_assert(g.error(last)->origin == at(s, {g2}));
  gr.take();
  test_assert(g.retry().ok() && !g.evaluate(last).ok() && gr.take() == 1);
  test_assert(!g.evaluate(last).ok() && gr.take() == 0);
  test_assert(o.setLiteral(g2, key(ia), PropValue()).ok() && num(g, last) == 2);

  /* Outputs of a def without an output node, and unlinked ones. */
  test_assert(in.unlink(in.node(output)->linkInto(key(sumOut))->id).ok() && num(g, last) == 0);
  test_assert(in.setLiteral(output, key(sumOut), fl(8)).ok() && num(g, last) == 8);
  test_assert(in.removeNode(output).ok() && num(g, last) == 0);
  test_assert(num(alone, SocketPath::output("sum")) == 0);
}

Status meshToFloat(const GraphValue &in, GraphValue &out)
{
  const Mesh *m = in.payload<Mesh>();
  if (!m) {
    return Status::error(StatusCode::FailedPrecondition, "no mesh");
  }
  out = GraphValue::ofFloat(m->verts);
  return Status();
}

void testConversions()
{
  World w;
  const Graph inner = newGraph(w.lib, "inner"), outer = newGraph(w.lib, "outer");
  GraphDef &in = *inner.def, &o = *outer.def;
  const int64_t a = addSocket(in, SocketSide::Input, "a", "float");
  const int64_t b = addSocket(in, SocketSide::Output, "b", "float");
  const NodeId input = add(in, "graph.input"), output = add(in, "graph.output");
  connect(in, input, key(a), output, key(b));
  const NodeId v = add(o, "value.float"), group = add(o, "graph.group"), s = add(o, "t.sum");
  test_assert(o.setGroup(group, inner.id).ok());
  connect(o, v, "value", group, key(a));
  connect(o, group, key(b), s, "a");
  test_assert(o.setParam(v, "value", fl(2.75)).ok());
  GraphInstance g(&w.rt, &w.lib, outer.id);
  Runs runs(g);
  test_assert(num(g, out(s)) == 12.75);
  runs.take();
  /* The types crossed are part of what an input reads: float -> int -> float. */
  test_assert(in.setInterfaceType(a, "int").ok() && num(g, out(s)) == 12 && runs.take() == 1);
  test_assert(num(g, SocketPath::of(group, key(a), SocketSide::Input)) == 2);
  test_assert(in.setInterfaceType(b, "int").ok() && num(g, out(s)) == 12);
  runs.take();
  test_assert(in.setInterfaceType(a, "float").ok() && num(g, out(s)) == 12);
  test_assert(in.setInterfaceType(b, "bool").ok() && num(g, out(s)) == 11);
  test_assert(in.setInterfaceType(b, "float").ok() && num(g, out(s)) == 12.75);
  /* A type without a conversion: the input that reads it fails. */
  test_assert(in.setInterfaceType(a, "test.Mesh").ok());
  test_assert(hasCode(g.evaluate(out(s)), StatusCode::TypeMismatch));
  test_assert(g.error(out(s))->origin == at(group));
  test_assert(strcmp(g.error(out(s))->socket.c_str(), key(a).c_str()) == 0);
  /* One registered late: mesh -> float, reached from a null payload. */
  test_assert(o.unlink(o.node(group)->linkInto(key(a))->id).ok());
  test_assert(hasCode(g.evaluate(out(s)), StatusCode::TypeMismatch));
  test_assert(g.error(out(s))->origin == at(output, {group}));
  test_assert(w.rt.ext<SocketTypeRegistry>().addConversion("test.Mesh", "float", meshToFloat).ok());
  test_assert(hasCode(g.evaluate(out(s)), StatusCode::FailedPrecondition));
  test_assert(strcmp(g.error(out(s))->status.message(), "no mesh") == 0);
  const NodeId m = add(o, "t.mesh");
  test_assert(o.setLiteral(m, "n", PropValue::ofInt(40)).ok());
  connect(o, m, "mesh", group, key(a));
  test_assert(num(g, out(s)) == 50);
  /* A converted value follows its source, and only then. */
  runs.take();
  test_assert(o.setLiteral(m, "n", PropValue::ofInt(41)).ok());
  test_assert(num(g, out(s)) == 51 && runs.take() == 2);
  w.rt.touch();
  test_assert(num(g, out(s)) == 51 && runs.take() == 0);
}

} // namespace

int main()
{
  testIncremental();
  testParts();
  testErrors();
  testGroups();
  testConversions();
  return test_end();
}
