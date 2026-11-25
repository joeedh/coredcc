/* Kernels: stopping them, hearing of their progress, replacing them, and those for
 * tables. */
#include "foreach_fixture.h"

#include "coredcc/graph/system.h"

#include <functional>

using namespace fixture;

test_init;

namespace {

bool isFloat(const GraphValue *v, double x)
{
  const bool ok = v && *v == GraphValue::ofFloat(x);
  if (!ok) {
    printf("%s %g\n",
           v ? graphValueTypeName(v->type()) : "null",
           v && v->type() == GraphValue::Type::Float ? v->asFloat() : 0.0);
  }
  return ok;
}

/* a + 1, in steps. */
int g_steps = 0;
Status slowKernel(NodeExecContext &ctx)
{
  for (int i = 0; i < 4; i++) {
    if (ctx.cancelled()) {
      return Status::error(StatusCode::Cancelled, "stopped");
    }
    g_steps++;
    ctx.progress(i / 4.0, "step");
  }
  return ctx.setOutput(0, GraphValue::ofFloat(ctx.input(0).asFloat() + 1));
}

/* What the test says. */
std::function<Status(NodeExecContext &)> g_kernel;
Status testKernel(NodeExecContext &ctx)
{
  return g_kernel(ctx);
}

Status one(NodeExecContext &ctx)
{
  return ctx.setOutput(0, GraphValue::ofFloat(1));
}
Status two(NodeExecContext &ctx)
{
  return ctx.setOutput(0, GraphValue::ofFloat(2));
}

NodeTypeDef versioned()
{
  NodeTypeDef d("t.v");
  d.uiName("V")
      .input(PropertyDef::floatProp("a"))
      .input(PropertyDef::floatProp("b"))
      .payloadInput("t", "elements")
      .output("x", "float")
      .param(PropertyDef::enumProp("mode").item("A", 0).item("B", 1))
      .param(PropertyDef::pointerProp("thing", kThingStruct), true);
  return d;
}

void addTypes(Runtime &rt)
{
  NodeTypeDef s("t.slow");
  s.input(PropertyDef::floatProp("a")).output("value", "float");
  addType<slowKernel>(rt, s, "t.slow");
  NodeTypeDef k("t.test");
  k.input(PropertyDef::floatProp("a")).output("value", "float");
  addType<testKernel>(rt, k, "t.test");
  NodeTypeDef v = versioned();
  addType<one>(rt, v, "t.v");
}

struct Stop {
  /* How many times it may be asked before it says yes; -1 for never. */
  int after = -1;
  int asked = 0;
  std::vector<std::pair<std::string, double>> progress;
  GraphMonitor monitor()
  {
    GraphMonitor m;
    m.cancelled = [this]() { return after >= 0 && asked++ >= after; };
    m.progress = [this](const GraphProgress &p) {
      test_assert(p.text == "step");
      progress.push_back({p.node.toString(), p.fraction});
    };
    return m;
  }
};

/* ---- cancelling ---- */

void testCancel()
{
  World w;
  addTypes(w.rt);
  GraphDef def(&w.rt, &w.lib);
  GraphInstance inst(&w.rt, &w.lib, &def);
  litestl::util::Vector<NodeId> n;
  for (int i = 0; i < 6; i++) {
    n.append(add(def, "t.sum"));
    test_assert(def.setLiteral(n[i], "b", PropValue::ofFloat(1)).ok());
    if (i > 0) {
      connect(def, n[i - 1], "value", n[i], "a");
    }
  }
  const SocketPath last = SocketPath::of(n[5], "value");
  Result<SocketHandle> h = inst.prepare(last);
  litestl::util::Vector<int> moved;
  test_assert(inst.evaluate(last).ok() && isFloat(inst.value(last), 6));
  test_assert(inst.evaluate({&*h, 1}, moved).ok() && moved.size() == 1);
  Stop stop;
  test_assert(inst.setMonitor(stop.monitor()).ok());

  /* Between kernels: what ran stays, the rest doesn't run. Asked again, it goes on. */
  test_assert(def.setLiteral(n[0], "a", PropValue::ofFloat(10)).ok());
  stop.after = 2;
  uint64_t runs = inst.totalRuns();
  test_assert(hasCode(inst.evaluate(last), StatusCode::Cancelled));
  test_assert(inst.totalRuns() == runs + 2 &&
              stop.asked == 3); /* not asked again in this call */
  test_assert(!inst.value(last) && isFloat(inst.lastGood(last), 6) &&
              !inst.isCurrent(last));
  test_assert(inst.error(last)->origin.node == n[2] &&
              inst.error(last)->socket.size() == 0);
  test_assert(inst.report(at({}, n[1])).state == NodeReport::State::Ok);
  test_assert(inst.report(at({}, n[2])).state == NodeReport::State::Failed);
  test_assert(!inst.report(at({}, n[2])).current);
  test_assert(inst.report(at({}, n[3])).state == NodeReport::State::InputFailed);
  test_assert(inst.peek(SocketPath::of(n[2], "value")).isNone());
  stop = Stop();
  stop.after = 1;
  test_assert(hasCode(inst.evaluate(last), StatusCode::Cancelled));
  test_assert(inst.totalRuns() == runs + 3);
  stop = Stop();
  test_assert(inst.evaluate({&*h, 1}, moved).ok() && moved.size() == 1);
  test_assert(isFloat(inst.value(*h), 16) && inst.isCurrent(*h) &&
              inst.totalRuns() == runs + 6);
  test_assert(inst.evaluate(last).ok() && isFloat(inst.value(last), 16));
  test_assert(inst.isCurrent(last) && inst.totalRuns() == runs + 6);

  /* What was computed before, and is still right, needn't run. */
  test_assert(def.setLiteral(n[0], "a", PropValue::ofFloat(11)).ok());
  stop.after = 0;
  test_assert(hasCode(inst.evaluate(last), StatusCode::Cancelled));
  test_assert(def.setLiteral(n[0], "a", PropValue::ofFloat(10)).ok());
  stop = Stop();
  runs = inst.totalRuns();
  test_assert(inst.evaluate(last).ok() && isFloat(inst.value(last), 16));
  test_assert(inst.totalRuns() == runs + 1); /* the one that was stopped */
  /* Said once, it holds for the call, for what else it was asked for too. */
  const NodeId apart = add(def, "t.sum");
  const SocketPath both[] = {last, SocketPath::of(apart, "value")};
  litestl::util::Vector<Status> results;
  test_assert(def.setLiteral(n[5], "b", PropValue::ofFloat(2)).ok());
  GraphMonitor once;
  int asked = 0;
  once.cancelled = [&]() { return asked++ == 0; };
  test_assert(inst.setMonitor(std::move(once)).ok() && inst.evaluate(both, results).ok());
  test_assert(hasCode(results[0], StatusCode::Cancelled));
  test_assert(hasCode(results[1], StatusCode::Cancelled) && asked == 1);
  test_assert(def.setLiteral(n[5], "b", PropValue::ofFloat(1)).ok());
  test_assert(inst.evaluate(both, results).ok() && results[0].ok() && results[1].ok());
  test_assert(inst.setMonitor(stop.monitor()).ok());
  /* Nothing to run, so there is nothing to stop. */
  stop.after = 0;
  test_assert(inst.evaluate(last).ok());
  test_assert(inst.setMonitor(GraphMonitor()).ok());
  test_assert(def.setLiteral(n[0], "a", PropValue::ofFloat(0)).ok() &&
              inst.evaluate(last).ok());
}

void testKernelCancels()
{
  World w;
  addTypes(w.rt);
  GraphDef def(&w.rt, &w.lib);
  GraphInstance inst(&w.rt, &w.lib, &def);
  const NodeId a = add(def, "t.slow"), b = add(def, "t.slow"), other = add(def, "t.sum");
  connect(def, a, "value", b, "a");
  const SocketPath p = SocketPath::of(b, "value");
  Stop stop;
  test_assert(inst.setMonitor(stop.monitor()).ok());
  g_steps = 0;
  stop.after = 7; /* 5 for a, 1 before b, and then b's second step */
  test_assert(hasCode(inst.evaluate(p), StatusCode::Cancelled) && g_steps == 5);
  test_assert(strcmp(inst.error(p)->status.message(), "stopped") == 0);
  test_assert(stop.progress.size() == 5 &&
              stop.progress[0].first == std::to_string(a.value));
  test_assert(stop.progress[3].second == 0.75 && stop.progress[4].second == 0);
  test_assert(stop.progress[4].first == std::to_string(b.value));
  stop = Stop();
  test_assert(inst.evaluate(p).ok() && isFloat(inst.value(p), 2) && g_steps == 9);

  /* Without a monitor nothing is cancelled. */
  GraphInstance quiet(&w.rt, &w.lib, &def);
  test_assert(quiet.evaluate(p).ok() && g_steps == 17);

  /* A kernel's own Cancelled isn't kept, and what doesn't need it is. Other errors are.
   */
  const NodeId t = add(def, "t.test");
  connect(def, t, "value", other, "a");
  const SocketPath o = SocketPath::of(other, "value");
  int calls = 0;
  StatusCode code = StatusCode::Cancelled;
  g_kernel = [&](NodeExecContext &) {
    calls++;
    return Status::error(code, "mine");
  };
  for (int i = 1; i < 4; i++) {
    test_assert(hasCode(inst.evaluate(o), StatusCode::Cancelled) && calls == i);
    test_assert(!inst.isCurrent(o) && inst.evaluate(p).ok() && g_steps == 17);
  }
  code = StatusCode::OutOfRange;
  test_assert(hasCode(inst.evaluate(o), StatusCode::OutOfRange) && calls == 4);
  test_assert(hasCode(inst.evaluate(o), StatusCode::OutOfRange) && calls == 4 &&
              inst.isCurrent(o));
  /* A sticky Cancelled is one too. */
  code = StatusCode::Cancelled;
  test_assert(inst.retry().ok());
  g_kernel = [&](NodeExecContext &ctx) {
    calls++;
    ctx.runtime().setStickyError(Status::error(StatusCode::Cancelled, "thrown"));
    return Status();
  };
  test_assert(hasCode(inst.evaluate(o), StatusCode::Cancelled) && calls == 5);
  test_assert(hasCode(inst.evaluate(o), StatusCode::Cancelled) && calls == 6);

  /* What the functions can't do, and what can't be done while a kernel runs. */
  GraphInstance inner(&w.rt, &w.lib, &def);
  g_kernel = [&](NodeExecContext &ctx) {
    test_assert(hasCode(inst.setMonitor(GraphMonitor()), StatusCode::Busy));
    test_assert(hasCode(inner.setMonitor(GraphMonitor()), StatusCode::Busy));
    test_assert(hasCode(inst.drop(at({}, t)), StatusCode::Busy));
    test_assert(hasCode(inner.evaluate(p), StatusCode::Busy) && !ctx.cancelled());
    ctx.progress(1);
    return ctx.setOutput(0, GraphValue::ofFloat(3));
  };
  GraphMonitor m;
  int told = 0;
  m.progress = [&](const GraphProgress &g) {
    told++;
    test_assert(g.fraction == 1 && g.text.empty() && g.node.node == t);
    test_assert(hasCode(def.setLabel(t, "x"), StatusCode::Busy));
  };
  test_assert(inst.setMonitor(std::move(m)).ok());
  test_assert(inst.evaluate(o).ok() && isFloat(inst.value(o), 3) && told == 1);
}

/* In elements: what ran stays there too. */
void testCancelElements(int maxCached)
{
  World w;
  addTypes(w.rt);
  Loop l(w);
  const NodeId slow = add(*l.inner.def, "t.slow");
  connect(*l.inner.def, l.innerIn, key(l.a), slow, "a");
  connect(*l.inner.def, slow, "value", l.add, "a");
  test_assert(
      l.root.def->setParam(l.node, kMaxCachedParam, PropValue::ofInt(maxCached)).ok());
  l.set(table({3, 4, 5}, {{"a", {1, 2, 3}}}));
  Stop stop;
  test_assert(l.inst.setMonitor(stop.monitor()).ok());
  g_steps = 0;
  stop.after =
      8; /* slow, sum and square of 3 are 7; then 4's slow stops at its second step */
  const uint64_t before = l.inst.totalRuns();
  test_assert(l.fails(StatusCode::Cancelled, {4}, "element 4: stopped") && g_steps == 4);
  test_assert(l.inst.totalRuns() - before == 4 && !l.inst.isCurrent(l.path));
  stop = Stop();
  stop.after = 1000;
  test_assert(l.evaluate().ok() && l.column("sum") && isFloat(l.column("sum"), 2));
  if (maxCached > 0) {
    /* 3 x 3 kernels, and at most the one that was stopped again. */
    test_assert(l.inst.totalRuns() - before == 10 && g_steps == 12);
  }
  const std::string path =
      std::to_string(l.node.value) + "/" + std::to_string(slow.value);
  test_assert(stop.progress.size() >= 4 && stop.progress.back().first == path + "[5]");
  /* All of them. */
  const Elements::Column *sum = l.result()->column("sum");
  test_assert(isFloat(&sum->values[1], 3) && isFloat(&sum->values[2], 4));
  const uint64_t runs = l.inst.totalRuns();
  test_assert(l.evaluate().ok() && l.inst.totalRuns() == runs &&
              l.inst.isCurrent(l.path));
}

/* ---- replacing kernels ---- */

template <Status (*Fn)(NodeExecContext &)>
NodeTypeDef withKernel(Runtime &rt, NodeTypeDef def, const char *impl)
{
  Result<Owned<GraphKernel>> k = makeKernel<FnKernel<Fn>>(rt, impl);
  test_assert(k.ok());
  def.kernel(std::move(k).value());
  return def;
}

void testReplace()
{
  World w;
  addTypes(w.rt);
  NodeTypeRegistry &reg = w.rt.ext<NodeTypeRegistry>();
  const NodeType *type = reg.find("t.v");
  Loop l(w);
  const NodeId inside = add(*l.inner.def, "t.v");
  connect(*l.inner.def, inside, "x", l.add, "b");
  l.set(table({1, 2}, {{"a", {10, 20}}}));
  GraphDef def(&w.rt, &w.lib);
  GraphInstance inst(&w.rt, &w.lib, &def);
  const NodeId v = add(def, "t.v"), sum = add(def, "t.sum"), apart = add(def, "t.sum");
  connect(def, v, "x", sum, "a");
  connect(def, apart, "value", sum, "b");
  const SocketPath p = SocketPath::of(sum, "value");
  test_assert(inst.evaluate(p).ok() && isFloat(inst.value(p), 1));
  test_assert(l.is({1, 2}, {{"sum", {11, 21}}, {"c2", {0, 0}}}, 6));

  const uint64_t runs = inst.totalRuns();
  const uint64_t stamp = type->stamp;
  test_assert(reg.replaceKernel(withKernel<two>(w.rt, versioned(), "t.v#2")).ok());
  test_assert(reg.find("t.v") == type && type->stamp == stamp && !inst.isCurrent(p));
  test_assert(inst.evaluate(p).ok() && isFloat(inst.value(p), 2));
  test_assert(inst.totalRuns() == runs + 2); /* v and sum, not apart */
  test_assert(l.is({1, 2}, {{"sum", {12, 22}}, {"c2", {0, 0}}}, 4));
  test_assert(inst.evaluate(p).ok() && inst.totalRuns() == runs + 2);
  /* The texts stay the type's. */
  NodeTypeDef shown = versioned();
  shown.uiName("W");
  test_assert(reg.replaceKernel(withKernel<one>(w.rt, std::move(shown), "t.v#1")).ok());
  test_assert(strcmp(type->uiName.c_str(), "V") == 0);
  test_assert(inst.evaluate(p).ok() && isFloat(inst.value(p), 1));

  /* What must be the same. */
  auto refused = [&](NodeTypeDef d, StatusCode code = StatusCode::FailedPrecondition) {
    return hasCode(reg.checkReplace(d), code) &&
           hasCode(reg.replaceKernel(withKernel<two>(w.rt, std::move(d), "t.v#2")), code);
  };
  test_assert(reg.checkReplace(versioned()).ok());
  const auto mode = []() {
    return PropertyDef::enumProp("mode").item("A", 0).item("B", 1);
  };
  const auto thing = []() { return PropertyDef::pointerProp("thing", kThingStruct); };
  struct Parts {
    PropertyDef a = PropertyDef::floatProp("a"), b = PropertyDef::floatProp("b");
    const char *payload = "t", *output = "x", *outputType = "float";
  };
  auto made = [&](const Parts &x, PropertyDef m, PropertyDef t, bool weak) {
    NodeTypeDef d("t.v");
    d.input(x.a)
        .input(x.b)
        .payloadInput(x.payload, "elements")
        .output(x.output, x.outputType);
    d.param(std::move(m)).param(std::move(t), weak);
    return d;
  };
  test_assert(reg.checkReplace(made(Parts(), mode(), thing(), true)).ok());
  Parts x;
  std::swap(x.a, x.b);
  test_assert(refused(made(x, mode(), thing(), true)));
  x = Parts();
  x.a = PropertyDef::intProp("a");
  test_assert(refused(made(x, mode(), thing(), true)));
  x = Parts();
  x.payload = "u";
  test_assert(refused(made(x, mode(), thing(), true)));
  x = Parts();
  x.output = "y";
  test_assert(refused(made(x, mode(), thing(), true)));
  x = Parts();
  x.outputType = "int";
  test_assert(refused(made(x, mode(), thing(), true)));
  x = Parts();
  test_assert(
      refused(made(x, PropertyDef::enumProp("mode").item("A", 0), thing(), true)));
  test_assert(refused(
      made(x, PropertyDef::enumProp("mode").item("A", 0).item("B", 2), thing(), true)));
  test_assert(refused(
      made(x, PropertyDef::enumProp("mode").item("A", 0).item("C", 1), thing(), true)));
  test_assert(refused(made(x, PropertyDef::intProp("mode"), thing(), true)));
  test_assert(refused(
      made(x, PropertyDef::enumProp("kind").item("A", 0).item("B", 1), thing(), true)));
  test_assert(refused(made(x, mode(), thing(), false)));
  test_assert(refused(made(x, mode().access(PropAccess::Optional), thing(), true)));
  test_assert(refused(
      made(x, mode(), PropertyDef::pointerProp("thing", StructId::of("S")), true)));
  for (int i = 0; i < 4; i++) {
    NodeTypeDef more = versioned();
    i == 0   ? more.version(2)
    : i == 1 ? more.output("z", "float")
    : i == 2 ? more.param(PropertyDef::intProp("n"))
             : more.input(PropertyDef::intProp("n"));
    test_assert(refused(std::move(more)));
  }
  NodeTypeDef flagged = versioned();
  flagged.flags(NodeTypeFlags::OutputAdapter);
  test_assert(refused(std::move(flagged)));
  NodeTypeDef unknown("t.w");
  unknown.output("x", "float");
  test_assert(refused(std::move(unknown), StatusCode::NotFound));
  NodeTypeDef group("graph.group");
  group.output("x", "float");
  test_assert(refused(std::move(group)));
  /* Exactly that of a type that isn't a kernel's. */
  NodeTypeDef each(kGraphForeachType);
  each.payloadInput(kElementsSocket, "elements").output(kElementsSocket, "elements");
  each.param(foreachMaxCachedParam());
  test_assert(refused(std::move(each)));
  /* Fewer. */
  NodeTypeDef fewer("t.v");
  fewer.input(PropertyDef::floatProp("a")).input(PropertyDef::floatProp("b"));
  fewer.output("x", "float").param(mode()).param(thing(), true);
  test_assert(refused(std::move(fewer)));
  NodeTypeDef wide("t.wide");
  wide.output("x", "float").output("y", "float");
  addType<one>(w.rt, wide, "t.wide");
  NodeTypeDef narrow("t.wide");
  narrow.output("x", "float");
  test_assert(refused(std::move(narrow)));
  test_assert(hasCode(reg.replaceKernel(versioned()), StatusCode::InvalidArgument));
  test_assert(inst.evaluate(p).ok() && isFloat(inst.value(p), 1));

  /* Not while one runs. */
  const NodeId t = add(def, "t.test");
  g_kernel = [&](NodeExecContext &ctx) {
    test_assert(hasCode(reg.checkReplace(versioned()), StatusCode::Busy));
    test_assert(hasCode(reg.replaceKernel(withKernel<two>(w.rt, versioned(), "t.v#2")),
                        StatusCode::Busy));
    return ctx.setOutput(0, GraphValue::ofFloat(0));
  };
  test_assert(inst.evaluate(SocketPath::of(t, "value")).ok());
}

/* ---- names ---- */

void testNames()
{
  World w;
  NodeTypeDef d("t.names");
  d.input(PropertyDef::floatProp("a").uiName("Amount"))
      .input(PropertyDef::floatProp("b"))
      .payloadInput("t", "elements", "Table")
      .payloadInput("u", "elements")
      .output("x", "float", "Result")
      .output("y", "float");
  test_assert(d.inputDef("a") && !d.inputDef("t") && !d.inputDef("no") &&
              !d.paramDef("a"));
  d.param(PropertyDef::intProp("n"));
  test_assert(d.paramDef("n") && !d.inputDef("n"));
  d.paramDef("n")->range(0, 5);
  test_assert(d.setSocketUiName(false, "b", "Bias") &&
              d.setSocketUiName(false, "u", "Other"));
  test_assert(d.setSocketUiName(true, "y", "Rest") &&
              !d.setSocketUiName(true, "a", "no"));
  test_assert(!d.setSocketUiName(false, "x", "no"));
  NodeTypeDef copy = d.copy();
  const NodeType *t = addType<one>(w.rt, copy, "t.names");
  const char *ins[] = {"Amount", "Bias", "Table", "Other"}, *outs[] = {"Result", "Rest"};
  for (int i = 0; i < 4; i++) {
    test_assert(strcmp(t->inputInfos[i].name.c_str(), ins[i]) == 0);
    test_assert(strcmp(t->inputInfos[i].key.c_str(), t->inputs[i].name.c_str()) == 0);
  }
  for (int i = 0; i < 2; i++) {
    test_assert(strcmp(t->outputInfos[i].name.c_str(), outs[i]) == 0);
  }
  test_assert(strcmp(t->literal(1)->uiName().c_str(), "Bias") == 0);
  test_assert(coerce(t->params[0].def, PropValue::ofInt(9)).ok() &&
              coerce(t->params[0].def, PropValue::ofInt(9))->asInt() == 5);
  NodeTypeDef plain("t.plain");
  plain.output("edgeWidth", "float").payloadInput("oldTable", "elements");
  const NodeType *p = addType<one>(w.rt, plain, "t.plain");
  test_assert(strcmp(p->outputInfos[0].name.c_str(), "Edge Width") == 0);
  test_assert(strcmp(p->inputInfos[0].name.c_str(), "Old Table") == 0);
  test_assert(strcmp(p->outputInfos[0].key.c_str(), "edgeWidth") == 0);
}

void testPaths()
{
  const NodePath paths[] = {at({}, NodeId{7}), at({NodeId{1}, NodeId{2}}, NodeId{3})};
  for (NodePath p : paths) {
    for (int64_t e : {int64_t(5), int64_t(-6), INT64_MAX, INT64_MIN}) {
      Result<NodePath> r = NodePath::parse(p.toString());
      test_assert(r.ok() && *r == p);
      p.elements.append(e);
    }
    Result<NodePath> r = NodePath::parse(p.toString());
    test_assert(r.ok() && *r == p && r->elements.size() == 4);
  }
  for (const char *bad : {"",
                          "0",
                          "-1",
                          "a",
                          "1/",
                          "/1",
                          "1//2",
                          "1/0",
                          "1[",
                          "1[]",
                          "1[2",
                          "1[2,3]",
                          "1[2, ]",
                          "1[2] ",
                          " 1",
                          "1.5",
                          "99999999999999999999",
                          "1[2][3]",
                          "+1"})
  {
    const bool refused =
        hasCode(NodePath::parse(bad).status(), StatusCode::InvalidArgument);
    test_assert(refused || !printf("%s\n", bad));
  }
}

/* ---- tables ---- */

void testElementNodes()
{
  World w;
  GraphDef def(&w.rt, &w.lib);
  GraphInstance inst(&w.rt, &w.lib, &def);
  auto table = [&](NodeId n) {
    const SocketPath p = SocketPath::of(n, "elements");
    const Status s = inst.evaluate(p);
    return s.ok() ? inst.value(p)->payload<Elements>() : nullptr;
  };
  auto code = [&](NodeId n, const char *socket) {
    return inst.evaluate(SocketPath::of(n, socket)).code();
  };
  auto value = [&](NodeId n, const char *socket) {
    const SocketPath p = SocketPath::of(n, socket);
    const Status s = inst.evaluate(p);
    return s.ok() ? *inst.value(p) : GraphValue();
  };
  const NodeId range = add(def, "elements.range"), count = add(def, "elements.count");
  const Elements *t = table(range);
  test_assert(t && t->size() == 10 && t->validate().ok() && t->keys[9] == 9);
  test_assert(t->column("index")->values[4] == GraphValue::ofInt(4));
  test_assert(isFloat(&t->column("factor")->values[9], 1) &&
              isFloat(&t->column("factor")->values[0], 0));
  connect(def, range, "elements", count, "elements");
  test_assert(value(count, "count") == GraphValue::ofInt(10));
  test_assert(def.setLiteral(range, "count", PropValue::ofInt(1)).ok());
  t = table(range);
  test_assert(t->size() == 1 && isFloat(&t->column("factor")->values[0], 0));
  test_assert(def.setLiteral(range, "count", PropValue::ofInt(0)).ok());
  test_assert(table(range)->size() == 0 && value(count, "count") == GraphValue::ofInt(0));
  const NodeId n = add(def, "value.int");
  connect(def, n, "value", range, "count");
  for (int64_t bad : {int64_t(-1), (int64_t(1) << 24) + 1}) {
    test_assert(def.setParam(n, "value", PropValue::ofInt(bad)).ok());
    test_assert(code(range, "elements") == StatusCode::OutOfRange);
    test_assert(code(count, "count") == StatusCode::OutOfRange);
  }
  test_assert(def.setParam(n, "value", PropValue::ofInt(5)).ok());
  const NodeId empty = add(def, "elements.count");
  test_assert(value(empty, "count") == GraphValue::ofInt(0));

  /* A value, converted. */
  const NodeId get = add(def, "elements.value.float"),
               vec = add(def, "elements.value.vec3");
  connect(def, range, "elements", get, "elements");
  connect(def, range, "elements", vec, "elements");
  test_assert(code(get, "value") == StatusCode::NotFound); /* no column */
  test_assert(
      strstr(inst.error(SocketPath::of(get, "value"))->status.message(), "'factor'"));
  for (NodeId v : {get, vec}) {
    test_assert(def.setParam(v, "column", PropValue::ofString("index")).ok());
  }
  const GraphValue three = GraphValue::ofFloat(3);
  test_assert(def.setLiteral(get, "index", PropValue::ofInt(3)).ok() &&
              value(get, "value") == three);
  for (int64_t bad : {int64_t(-1), int64_t(5)}) {
    test_assert(def.setLiteral(get, "index", PropValue::ofInt(bad)).ok());
    test_assert(code(get, "value") == StatusCode::OutOfRange);
  }
  test_assert(def.setParam(get, "column", PropValue::ofString("factor")).ok());
  test_assert(def.setLiteral(get, "index", PropValue::ofInt(1)).ok());
  const GraphValue quarter = value(get, "value");
  test_assert(isFloat(&quarter, 0.25));
  const bool converts = w.rt.ext<SocketTypeRegistry>().canConvert(
      *w.rt.ext<SocketTypeRegistry>().find("int"),
      *w.rt.ext<SocketTypeRegistry>().find("vec3"));
  test_assert((code(vec, "value") == StatusCode::Ok) == converts);

  /* Renamed, also over another, and to itself. */
  const NodeId rename = add(def, "elements.rename");
  connect(def, range, "elements", rename, "elements");
  test_assert(code(rename, "elements") == StatusCode::NotFound);
  test_assert(def.setParam(rename, "from", PropValue::ofString("factor")).ok());
  test_assert(def.setParam(rename, "to", PropValue::ofString("x")).ok());
  t = table(rename);
  test_assert(t->columns.size() == 2 && t->column("x") && t->column("index") &&
              t->size() == 5);
  test_assert(isFloat(&t->column("x")->values[1], 0.25) && t->validate().ok());
  test_assert(def.setParam(rename, "to", PropValue::ofString("index")).ok());
  t = table(rename);
  test_assert(t->columns.size() == 1 && isFloat(&t->column("index")->values[1], 0.25));
  test_assert(def.setParam(rename, "to", PropValue::ofString("factor")).ok());
  test_assert(*table(rename) == *table(range));
  test_assert(def.setParam(rename, "to", PropValue::ofString("x")).ok());

  /* Combined by key: the first table's rows and columns count. */
  const NodeId combine = add(def, "elements.combine");
  connect(def, rename, "elements", combine, "a");
  connect(def, range, "elements", combine, "b");
  t = table(combine);
  test_assert(t->columns.size() == 3 && t->column("factor") && t->column("x") &&
              t->validate().ok());
  Elements a, b;
  for (int64_t k : {3, 1}) {
    a.keys.append(k);
  }
  for (int64_t k : {1, 2, 3}) {
    b.keys.append(k);
  }
  auto column = [](Elements &to, const char *name, std::initializer_list<double> values) {
    Elements::Column c;
    c.name = name;
    for (double v : values) {
      c.values.append(GraphValue::ofFloat(v));
    }
    to.columns.append(std::move(c));
  };
  column(a, "v", {30, 10});
  column(b, "v", {1, 2, 3});
  column(b, "w", {100, 200, 300});
  const int64_t ia = addSocket(def, SocketSide::Input, "a", "elements");
  const int64_t ib = addSocket(def, SocketSide::Input, "b", "elements");
  const NodeId in = add(def, "graph.input");
  connect(def, in, key(ia), combine, "a");
  connect(def, in, key(ib), combine, "b");
  test_assert(inst.setInput("a", tableValue(a)).ok() &&
              inst.setInput("b", tableValue(b)).ok());
  t = table(combine);
  test_assert(t->size() == 2 && t->keys[0] == 3 && t->columns.size() == 2);
  test_assert(isFloat(&t->column("v")->values[0], 30) &&
              isFloat(&t->column("w")->values[0], 300));
  test_assert(isFloat(&t->column("w")->values[1], 100));
  test_assert(inst.setInput("a", tableValue(b)).ok() &&
              inst.setInput("b", tableValue(a)).ok());
  test_assert(code(combine, "elements") == StatusCode::NotFound);
  test_assert(inst.setInput("b", GraphValue()).ok());
  test_assert(code(combine, "elements") == StatusCode::NotFound);
  test_assert(inst.setInput("a", GraphValue()).ok() && table(combine)->size() == 0);
  /* A table that isn't well formed. */
  a.keys.append(3);
  test_assert(inst.setInput("a", tableValue(a)).ok());
  test_assert(code(combine, "elements") == StatusCode::InvalidArgument);

  /* All of it, with no native node: the squares of 0 to 4 as a sum of two columns. */
  Loop l(w);
  const NodeId r = add(*l.root.def, "elements.range"),
               name = add(*l.root.def, "elements.rename");
  connect(*l.root.def, r, "elements", name, "elements");
  connect(*l.root.def, name, "elements", l.node, kElementsSocket);
  test_assert(l.root.def->setParam(name, "from", PropValue::ofString("index")).ok());
  test_assert(l.root.def->setParam(name, "to", PropValue::ofString("c")).ok());
  test_assert(l.root.def->setLiteral(r, "count", PropValue::ofInt(5)).ok());
  const NodeId last = add(*l.root.def, "elements.value.float");
  connect(*l.root.def, l.node, kElementsSocket, last, "elements");
  test_assert(l.root.def->setParam(last, "column", PropValue::ofString("c2")).ok());
  test_assert(l.root.def->setLiteral(last, "index", PropValue::ofInt(4)).ok());
  test_assert(l.inst.evaluate(SocketPath::of(last, "value")).ok());
  test_assert(isFloat(l.inst.value(SocketPath::of(last, "value")), 16));
}

} // namespace

int main()
{
  testCancel();
  testKernelCancels();
  testCancelElements(100);
  testCancelElements(0);
  testReplace();
  testNames();
  testPaths();
  testElementNodes();
  return test_end();
}
