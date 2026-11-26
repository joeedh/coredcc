/* Graphs for scripts, through a fake foreign host: node types with script kernels, what a
 * kernel may call, editing, and script instances. */
#include "../graph/edit_fixture.h"
#include "fake_host.h"

#include "coredcc/bind/foreign_command.h"
#include "coredcc/bind/graph_facades.h"
#include "coredcc/bind/manager.h"
#include "coredcc/bind/path_reader.h"
#include "coredcc/bind/run_facades.h"

#include <functional>
#include <memory>
#include <new>
#include <optional>

using namespace fixture;
namespace sc = coredcc::script;
using fakehost::Host;
using fakehost::Obj;
using litestl::util::string;

test_init;

namespace {

struct Mesh {
  int n = 0;
};

} // namespace

template <> struct coredcc::PayloadTraits<Mesh> {
  static constexpr const char *name = "t.Mesh";
};

namespace {

using Kernel = std::function<Status(NodeExecContext &)>;

/* A script object whose `evaluate` is `fn`. It throws when `fn` gives Cancelled. */
Obj kernelObj(Kernel fn)
{
  Obj o;
  o.methods["evaluate"] = [fn](void **args, void *ret, string *err) {
    Status s = fn(*static_cast<NodeExecContext *>(args[0]));
    if (s.code() == StatusCode::Cancelled) {
      *err = s.message();
      return false;
    }
    new (ret) Status(std::move(s));
    return true;
  };
  return o;
}

/* Input a (Float 2), param k (Float 3), output value (float). */
Status registerScale(Doc &d,
                     Host &host,
                     const char *id,
                     Kernel fn,
                     fakehost::lf::Handle *handle = nullptr,
                     const char *slots = "evaluate")
{
  NodeTypeDef *def = sc::newNodeType(id);
  test_assert(sc::nodeTypeUiName(*def, "Scale").ok());
  test_assert(sc::nodeTypeInput(*def, "a", sc::makeFloat(2)).ok());
  test_assert(sc::nodeTypeParam(*def, "k", sc::makeFloat(3)).ok());
  test_assert(sc::nodeTypeOutput(*def, "value", "float").ok());
  const fakehost::lf::Handle h = host.add(kernelObj(std::move(fn)));
  if (handle) {
    *handle = h;
  }
  Status s = sc::registerForeignNodeType(d.ctx, *def, h, slots);
  litestl::alloc::Delete(def);
  return s;
}

Status scale(NodeExecContext &n)
{
  test_assert(!sc::nodeParam(n, "a").status.ok() && !sc::nodeInput(n, "k").status.ok());
  const double a = *sc::nodeInput(n, "a").value.tryFloat();
  const double k = *sc::nodeParam(n, "k").value.tryFloat();
  return sc::nodeOutput(n, "value", sc::makeFloat(a * k));
}

/* graphs["g"] with a node of `type` whose `value` is the output `out`. */
int64_t makeGraph(Doc &d, const char *type)
{
  test_assert(sc::graphCreate(d.ctx, "g").status.ok());
  const int64_t out =
      sc::graphAddInterface(d.ctx, "graphs[\"g\"]", true, "out", "float").value;
  const int64_t in =
      sc::graphAddInterface(d.ctx, "graphs[\"g\"]", false, "in", "float").value;
  const int64_t node = sc::graphAddNode(d.ctx, "graphs[\"g\"]", type, 1, 2).value;
  const int64_t o = sc::graphAddNode(d.ctx, "graphs[\"g\"]", "graph.output", 0, 0).value;
  const int64_t i = sc::graphAddNode(d.ctx, "graphs[\"g\"]", "graph.input", 0, 0).value;
  test_assert(in && out && node && o && i);
  test_assert(sc::graphLink(d.ctx, "graphs[\"g\"]", node, "value", o, key(out).c_str())
                  .status.ok());
  return node;
}

void testRegistration()
{
  Host host;
  {
    Doc d;
    int tables = d.rt.ifaces().tableCount();
    const int types = d.rt.ext<NodeTypeRegistry>().size();
    struct Case {
      const char *id;
      const char *slots;
      StatusCode code;
    };
    const Case cases[] = {
        {"core.x", "evaluate", StatusCode::InvalidArgument},
        {"graph.x", "evaluate", StatusCode::InvalidArgument},
        {"x y", "evaluate", StatusCode::InvalidArgument},
        {"js.a", "", StatusCode::TypeMismatch},
        {"js.a", "evaluate bogus", StatusCode::TypeMismatch},
        {"scene.x", "evaluate", StatusCode::InvalidArgument},
        {"math.x", "evaluate", StatusCode::InvalidArgument},
        {"vec.x", "evaluate", StatusCode::InvalidArgument},
        {"mat4.x", "evaluate", StatusCode::InvalidArgument},
        {"value.x", "evaluate", StatusCode::InvalidArgument},
        {"elements.x", "evaluate", StatusCode::InvalidArgument},
        {"math.float", "evaluate", StatusCode::InvalidArgument},
        {"mathx.float", "", StatusCode::TypeMismatch}, /* only the namespace is reserved */
    };
    for (const Case &c : cases) {
      fakehost::lf::Handle h = 0;
      test_assert(hasCode(registerScale(d, host, c.id, scale, &h, c.slots), c.code));
      test_assert(host.releasesOf(h) == 1 && d.rt.ifaces().tableCount() == tables);
      test_assert(d.rt.ext<NodeTypeRegistry>().size() == types);
    }
    NodeTypeDef *def = sc::newNodeType("js.b");
    test_assert(hasCode(sc::registerForeignNodeType(d.ctx, *def, 0, "evaluate"),
                        StatusCode::InvalidArgument));
    /* What the def says is checked before anything is registered. */
    auto refused = [&](StatusCode code) {
      const fakehost::lf::Handle h = host.add(kernelObj(scale));
      test_assert(hasCode(sc::registerForeignNodeType(d.ctx, *def, h, "evaluate"), code));
      test_assert(host.releasesOf(h) == 1 && d.rt.ifaces().tableCount() == tables);
    };
    refused(StatusCode::InvalidArgument); /* no output */
    test_assert(sc::nodeTypeOutput(*def, "o", "nope").ok());
    refused(StatusCode::NotFound);
    litestl::alloc::Delete(def);
    def = sc::newNodeType("js.b");
    test_assert(sc::nodeTypeOutput(*def, "o", "float").ok());
    test_assert(hasCode(sc::nodeTypeInput(*def, "no name", sc::makeInt(1)),
                        StatusCode::InvalidArgument));
    test_assert(hasCode(sc::nodeTypeParam(*def, "m", PropValue::ofEnum(1)),
                        StatusCode::InvalidArgument));
    test_assert(sc::nodeTypeInput(*def, "x", sc::makeInt(1)).ok());
    test_assert(sc::nodeTypeInput(*def, "x", sc::makeInt(1)).ok());
    refused(StatusCode::InvalidArgument); /* two inputs */
    litestl::alloc::Delete(def);

    /* The corrected retry; a second one is refused, and the def can be used again. */
    fakehost::lf::Handle h = 0, again = 0;
    test_assert(registerScale(d, host, "js.a", scale, &h).ok() &&
                host.releasesOf(h) == 0);
    test_assert(hasCode(registerScale(d, host, "js.a", scale, &again),
                        StatusCode::AlreadyExists));
    test_assert(host.releasesOf(again) == 1);
    def = sc::newNodeType("js.c");
    test_assert(sc::nodeTypeOutput(*def, "o", "float").ok());
    const fakehost::lf::Handle c = host.add(kernelObj(scale));
    test_assert(sc::registerForeignNodeType(d.ctx, *def, c, "evaluate").ok());
    test_assert(strcmp(def->name().c_str(), "js.c") == 0);
    tables += 2;
    refused(StatusCode::AlreadyExists);
    litestl::alloc::Delete(def);
  }
  test_assert(host.live() == 0 && host.allReleasedOnce());
}

void testKernel()
{
  Host host;
  {
    Doc d;
    test_assert(d.rt.ext<SocketTypeRegistry>().addPayloadType<Mesh>().ok());
    test_assert(registerScale(d, host, "js.scale", scale).ok());
    test_assert(strcmp(d.rt.ext<NodeTypeRegistry>().find("js.scale")->uiName.c_str(),
                       "Scale") == 0);
    const int64_t n = makeGraph(d, "js.scale");
    sc::GraphResult g = sc::graphInstance(d.ctx, "graphs[\"g\"]");
    test_assert(g.status.ok());
    sc::ValueResult r = sc::graphEvaluate(g.value, "out");
    test_assert(r.status.ok() && r.value == sc::makeFloat(6));
    const std::string node = "graphs[\"g\"].nodes[#" + std::to_string(n) + "]";
    test_assert(sc::setValue(d.ctx, (node + ".params.k").c_str(), sc::makeFloat(5)).ok());
    test_assert(sc::setValue(d.ctx, (node + ".inputs.a").c_str(), sc::makeInt(4)).ok());
    const int calls = host.calls;
    test_assert(sc::graphEvaluate(g.value, "out").value == sc::makeFloat(20));
    test_assert(host.calls == calls + 1);
    test_assert(sc::graphEvaluate(g.value, "out").value == sc::makeFloat(20));
    test_assert(host.calls == calls + 1 &&
                sc::graphErrorText(g.value, "out").size() == 0);

    /* What a kernel is told of its mistakes. */
    Status in, out, param, wrong, passIn, passOut, payload, make, pass;
    NodeTypeDef *def = sc::newNodeType("js.odd");
    test_assert(sc::nodeTypePayloadInput(*def, "mesh", "t.Mesh").ok());
    test_assert(sc::nodeTypeInput(*def, "text", sc::makeString("x").value).ok());
    test_assert(sc::nodeTypeInput(*def, "i", sc::makeInt(7)).ok());
    test_assert(sc::nodeTypeOutput(*def, "value", "float").ok());
    test_assert(sc::nodeTypeOutput(*def, "mesh", "t.Mesh").ok());
    test_assert(sc::nodeTypeOutput(*def, "copy", "string").ok());
    Kernel odd = [&](NodeExecContext &c) {
      in = sc::nodeInput(c, "txet").status;
      param = sc::nodeParam(c, "p").status;
      out = sc::nodeOutput(c, "vaule", sc::makeInt(1));
      passIn = sc::nodePass(c, "no", "mesh");
      passOut = sc::nodePass(c, "mesh", "no");
      payload = sc::nodeInput(c, "mesh").status;
      make = sc::nodeOutput(c, "mesh", sc::makeInt(1));
      wrong = sc::nodeOutput(c, "value", sc::makeString("x").value);
      pass = sc::nodePass(c, "text", "copy");
      CDCC_TRY(sc::nodePass(c, "mesh", "mesh"));
      /* An Int, converted. */
      return sc::nodeOutput(c, "value", sc::nodeInput(c, "i").value);
    };
    test_assert(
        sc::registerForeignNodeType(d.ctx, *def, host.add(kernelObj(odd)), "evaluate")
            .ok());
    litestl::alloc::Delete(def);
    const Graph other = newGraph(d.lib, "other");
    const NodeId m = add(*other.def, "js.odd");
    GraphInstance inst(&d.rt, &d.lib, other.id);
    test_assert(inst.evaluate(SocketPath::of(m, "value")).ok());
    test_assert(*inst.value(SocketPath::of(m, "value")) == GraphValue::ofFloat(7));
    test_assert(inst.evaluate(SocketPath::of(m, "copy")).ok());
    test_assert(*inst.value(SocketPath::of(m, "copy")) == GraphValue::ofString("x"));
    test_assert(hasCode(in, StatusCode::NotFound) && in.suggestions().size() == 1);
    test_assert(hasCode(out, StatusCode::NotFound) && out.suggestions().size() == 1);
    test_assert(hasCode(param, StatusCode::NotFound) &&
                hasCode(passIn, StatusCode::NotFound));
    test_assert(hasCode(passOut, StatusCode::NotFound) &&
                hasCode(wrong, StatusCode::TypeMismatch));
    test_assert(hasCode(payload, StatusCode::Unsupported) &&
                hasCode(make, StatusCode::Unsupported));
    test_assert(pass.ok());

    /* A failing one, a throwing one, one that forgets its output, and nodePass of another
     * type, which fails the node whatever it returns. */
    StatusCode code = StatusCode::Ok;
    test_assert(registerScale(d, host, "js.fail", [&](NodeExecContext &c) {
                  if (code == StatusCode::Internal) {
                    sc::nodePass(c, "a", "value").ignore();
                    return sc::nodePass(c, "a", "nothing");
                  }
                  return code == StatusCode::Ok ? Status() : Status::error(code, "boom");
                }).ok());
    test_assert(sc::undo(d.ctx).ok() && sc::undo(d.ctx).ok());
    const int64_t f = sc::graphAddNode(d.ctx, "graphs[\"g\"]", "js.fail", 0, 0).value;
    test_assert(f);
    sc::GraphResult bad = sc::graphInstance(d.ctx, "graphs[\"g\"]");
    GraphDef *gd = nullptr;
    d.lib.forEach([&](BlockHandle h, const Datablock &b) {
      if (strcmp(b.name().c_str(), "g") == 0) {
        gd = graphDefOf(d.lib, h);
      }
    });
    const int64_t outId = gd->interfaceSocket(SocketSide::Output, "out")->id;
    NodeId outNode;
    gd->forEachNode([&](const GraphNode &x) {
      outNode = strcmp(x.typeName().c_str(), "graph.output") == 0 ? x.id() : outNode;
    });
    test_assert(sc::graphLink(
                    d.ctx, "graphs[\"g\"]", f, "value", outNode.value, key(outId).c_str())
                    .status.ok());
    r = sc::graphEvaluate(bad.value, "out");
    test_assert(hasCode(r.status, StatusCode::Internal) &&
                strstr(r.status.message(), "set output"));
    code = StatusCode::OutOfRange;
    d.rt.touch();
    test_assert(hasCode(sc::graphEvaluate(bad.value, "out").status,
                        StatusCode::Internal)); /* cached */
    test_assert(
        sc::setValue(d.ctx,
                     ("graphs[\"g\"].nodes[#" + std::to_string(f) + "].inputs.a").c_str(),
                     sc::makeFloat(1))
            .ok());
    r = sc::graphEvaluate(bad.value, "out");
    test_assert(hasCode(r.status, StatusCode::OutOfRange) &&
                strstr(r.status.message(), "boom"));
    const std::string text = sc::graphErrorText(bad.value, "out").c_str();
    test_assert(text.find("boom") != std::string::npos);
    test_assert(text.find("(node " + std::to_string(f) + ")") != std::string::npos);
    code = StatusCode::Cancelled; /* it throws */
    test_assert(sc::undo(d.ctx).ok());
    r = sc::graphEvaluate(bad.value, "out");
    test_assert(hasCode(r.status, StatusCode::Internal) &&
                strstr(r.status.message(), "boom"));
    test_assert(!d.rt.hasStickyError() && !sc::inForeignCall(d.rt));
    code = StatusCode::Internal;
    test_assert(sc::redo(d.ctx).ok());
    r = sc::graphEvaluate(bad.value, "out");
    test_assert(hasCode(r.status, StatusCode::NotFound));
  }
  test_assert(host.live() == 0 && host.allReleasedOnce());
}

/* An interface with slots named as Command's are. */
struct Probe : IfaceRef {
  Status exec(Context &ctx) const
  {
    return CDCC_CALL(exec, ctx);
  }
  CDCC_INTERFACE(Probe, "test::Probe")
  {
    BIND_STRUCT_METHOD(st, exec, MARGS("ctx"));
  }
};

struct NativeKernel {
  static inline std::function<void()> inside;
  static Status run(NodeExecContext &ctx)
  {
    inside();
    return ctx.setOutput(0, GraphValue::ofFloat(1));
  }
};

void testGates()
{
  Host host;
  {
    Doc d;
    std::function<void()> inside = []() {};
    test_assert(registerScale(d, host, "js.scale", [&](NodeExecContext &n) {
                  inside();
                  return scale(n);
                }).ok());
    const int64_t n = makeGraph(d, "js.scale");
    sc::GraphResult g = sc::graphInstance(d.ctx, "graphs[\"g\"]");
    auto evaluate = [&](double want) {
      test_assert(
          sc::setValue(
              d.ctx,
              ("graphs[\"g\"].nodes[#" + std::to_string(n) + "].params.k").c_str(),
              sc::makeFloat(want / 2))
              .ok());
      return sc::graphEvaluate(g.value, "out").value == sc::makeFloat(want);
    };
    const string name = "graphs[\"g\"].name";
    auto all = [&](std::vector<Status> &out) {
      const string gp = "graphs[\"g\"]";
      out = {sc::resolvePath(d.ctx, name).status,
             sc::pathMeta(d.ctx, name).status,
             sc::valueKeys(d.ctx, name, PropValue::ofEnum(0)).status,
             sc::getValue(d.ctx, name).status,
             sc::formatValue(d.ctx, name).status,
             sc::parseValue(d.ctx, name, "x").status,
             sc::inputGet(d.ctx, "x").status,
             sc::outputSet(d.ctx, "x", sc::makeInt(1)),
             sc::outputGet(d.ctx, "x").status,
             sc::setValue(d.ctx, name, sc::makeString("x").value),
             sc::writeValue(d.ctx, name, sc::makeString("x").value),
             sc::invoke(d.ctx, "core.block.purge").status,
             sc::undo(d.ctx),
             sc::redo(d.ctx),
             sc::canUndo(d.ctx).status,
             sc::canRedo(d.ctx).status,
             sc::graphCreate(d.ctx, "x").status,
             sc::graphAddNode(d.ctx, gp, "js.scale", 0, 0).status,
             sc::graphRemoveNode(d.ctx, gp, n),
             sc::graphLink(d.ctx, gp, n, "value", n, "a").status,
             sc::graphUnlink(d.ctx, gp, 1),
             sc::graphAddInterface(d.ctx, gp, true, "x", "float").status,
             sc::graphInstance(d.ctx, gp).status,
             sc::graphSetInput(g.value, "in", sc::makeFloat(1)),
             sc::graphClearInput(g.value, "in"),
             sc::graphEvaluate(g.value, "out").status,
             sc::readerRead(d.ctx, sc::newPathReader()),
             sc::readerTake(d.ctx, sc::newPathReader(), true).status,
             sc::interactiveId(d.ctx).status,
             sc::runAt(d.ctx, "", "core.block.purge", sc::newRunInputs(), true).status,
             registerScale(d, host, "js.inside", scale)};
    };
    std::vector<Status> seen;
    inside = [&]() {
      const int tables = d.rt.ifaces().tableCount();
      all(seen);
      test_assert(d.rt.ifaces().tableCount() == tables);
    };
    const int steps = d.history.size();
    test_assert(evaluate(8));
    test_assert(seen.size() == 31);
    for (size_t i = 0; i < seen.size(); i++) {
      if (!hasCode(seen[i], StatusCode::Busy)) {
        printf("facade %d\n", int(i));
        test_assert(!"Busy in a kernel");
      }
    }
    /* The facades say so themselves where the engine would allow it. */
    test_assert(strstr(seen[0].message(), "kernel") &&
                strstr(seen[12].message(), "kernel"));
    test_assert(d.history.size() == steps + 1 &&
                d.rt.ext<NodeTypeRegistry>().find("js.inside") == nullptr);
    inside = []() {};

    /* Outside, the same calls work. */
    test_assert(sc::resolvePath(d.ctx, name).status.ok() &&
                sc::getValue(d.ctx, name).status.ok());
    test_assert(sc::canUndo(d.ctx).value);

    /* Another interface's frame: not a Command's, though its slot has the name of one. */
    Status get, set, write, create;
    Obj probe;
    probe.methods["exec"] = [&](void **args, void *ret, string *) {
      Context &ctx = *static_cast<Context *>(args[0]);
      get = sc::getValue(ctx, name).status;
      set = sc::setValue(ctx, name, sc::makeString("x").value);
      write = sc::writeValue(ctx, name, sc::makeString("x").value);
      create = sc::graphCreate(ctx, "x").status;
      new (ret) Status();
      return true;
    };
    const fakehost::lf::Handle ph = host.add(probe);
    const char *slots[] = {"exec"};
    test_assert(registerForeign<Probe>(d.rt,
                                       "test.probe",
                                       d.rt.ext<sc::ForeignBindings>().dispatcher(),
                                       nullptr,
                                       slots)
                    .ok());
    Probe p = query<Probe>(
        d.rt, TypeId::of("test.probe"), reinterpret_cast<void *>(uintptr_t(ph)));
    test_assert(p.exec(d.ctx).ok());
    test_assert(get.ok() && hasCode(set, StatusCode::Busy) &&
                hasCode(write, StatusCode::Busy));
    test_assert(hasCode(create, StatusCode::Busy) &&
                d.is(name.c_str(), PropValue::ofString("g")));
    /* Called from a native kernel, or from a script one, it is the innermost frame: it
     * may read. */
    const Graph other = newGraph(d.lib, "other");
    NodeTypeDef native("t.native");
    native.output("value", "float");
    addType<NativeKernel::run>(d.rt, native, "t.native");
    const NodeId m = add(*other.def, "t.native");
    GraphInstance inst(&d.rt, &d.lib, other.id);
    get = Status::error(StatusCode::Internal, "not called");
    NativeKernel::inside = [&]() { test_assert(p.exec(d.ctx).ok()); };
    test_assert(inst.evaluate(SocketPath::of(m, "value")).ok() && get.ok());
    test_assert(hasCode(set, StatusCode::Busy));
    NativeKernel::inside = nullptr;
    get = Status::error(StatusCode::Internal, "not called");
    Status after;
    inside = [&]() {
      test_assert(p.exec(d.ctx).ok());
      after = sc::getValue(d.ctx, name).status;
    };
    test_assert(evaluate(10) && get.ok() && hasCode(after, StatusCode::Busy));
    inside = []() {};
    fakehost::lf::release(ph);
    d.history.clear();
  }
  test_assert(host.live() == 0 && host.allReleasedOnce());
}

/* A script command whose slots run hooks. */
struct Hooks {
  std::function<Status(Context &)> exec;
  std::function<void(Context &)> undo = [](Context &) {};
};
void registerHooks(Doc &d, Host &host, Hooks *hooks)
{
  Obj factory;
  factory.methods["make"] = [&host, hooks](void **, void *ret, string *) {
    Obj o;
    o.methods["canRun"] = [](void **, void *r, string *) {
      new (r) Status();
      return true;
    };
    o.methods["exec"] = [hooks](void **args, void *r, string *) {
      new (r) Status(hooks->exec(*static_cast<Context *>(args[0])));
      return true;
    };
    o.methods["undo"] = [hooks](void **args, void *, string *) {
      hooks->undo(*static_cast<Context *>(args[0]));
      return true;
    };
    o.methods["memSize"] = [](void **, void *r, string *) {
      *static_cast<uint64_t *>(r) = 0;
      return true;
    };
    o.methods["discard"] = [](void **, void *, string *) { return true; };
    *static_cast<uint32_t *>(ret) = host.add(std::move(o));
    return true;
  };
  CommandDef *def = sc::newCommandDef("js.cmd");
  test_assert(sc::registerForeignCommand(
                  d.ctx, *def, host.add(factory), "canRun exec undo memSize discard")
                  .ok());
  litestl::alloc::Delete(def);
}

void testEditing()
{
  Host host;
  Hooks hooks;
  {
    Doc d;
    const int64_t n = makeGraph(d, "math.float");
    GraphDef *def = nullptr;
    d.lib.forEach([&](BlockHandle h, const Datablock &) { def = graphDefOf(d.lib, h); });
    const string gp = "graphs[\"g\"]";
    test_assert(def && def->node(NodeId{n})->x() == 1 && def->node(NodeId{n})->y() == 2);
    test_assert(def->interfaceSocket(SocketSide::Input, "in"));
    test_assert(def->interfaceSocket(SocketSide::Output, "out"));
    test_assert(sc::undo(d.ctx).ok() && def->linkCount() == 0 && sc::redo(d.ctx).ok());
    int64_t link = 0;
    def->forEachLink([&](const GraphLink &l) { link = l.id.value; });
    test_assert(sc::graphUnlink(d.ctx, gp, link).ok() && def->linkCount() == 0);
    test_assert(hasCode(sc::graphUnlink(d.ctx, gp, link), StatusCode::NotFound));
    test_assert(sc::graphRemoveNode(d.ctx, gp, n).ok() && !def->node(NodeId{n}));
    test_assert(sc::undo(d.ctx).ok() && sc::undo(d.ctx).ok() && def->linkCount() == 1);

    /* Paths: the graph's, or a pointer to it; anything else says so. */
    test_assert(
        hasCode(sc::graphRemoveNode(d.ctx, "graphs[\"no\"]", n), StatusCode::NotFound));
    for (const char *no : {"graphs[\"g\"].name", "graphs[\"g\"].nodes[0]", "graphs"}) {
      test_assert(!sc::graphRemoveNode(d.ctx, no, n).ok());
      test_assert(!sc::graphInstance(d.ctx, no).status.ok());
    }
    test_assert(hasCode(sc::graphRemoveNode(d.ctx, "graphs[\"g\"].nodes[0]", n),
                        StatusCode::InvalidArgument));
    sc::ValueResult made = sc::graphCreate(d.ctx, "h");
    test_assert(made.status.ok() && made.value.tryPointer());
    const int64_t group = sc::graphAddNode(d.ctx, gp, "graph.group", 0, 0).value;
    const std::string member =
        "graphs[\"g\"].nodes[#" + std::to_string(group) + "].group";
    test_assert(
        hasCode(sc::graphAddNode(d.ctx, member.c_str(), "math.float", 0, 0).status,
                StatusCode::InvalidArgument)); /* null */
    test_assert(sc::setValue(d.ctx, member.c_str(), made.value).ok());
    test_assert(sc::graphAddNode(d.ctx, member.c_str(), "math.float", 0, 0).status.ok());
    test_assert(d.is("graphs[\"h\"].nodes.length", PropValue::ofInt(1)));
    test_assert(sc::graphInstance(d.ctx, member.c_str()).status.ok());

    /* From a command's exec: sub-commands of it. Not from its other slots. */
    registerHooks(d, host, &hooks);
    Status inUndo;
    hooks.exec = [&](Context &ctx) {
      test_assert(strstr(sc::undo(ctx).message(), "only outside script slots"));
      CDCC_TRY(sc::graphAddNode(ctx, gp, "math.float", 0, 0).status);
      return sc::graphCreate(ctx, "inner").status;
    };
    hooks.undo = [&](Context &ctx) {
      inUndo = sc::graphAddNode(ctx, gp, "math.float", 0, 0).status;
    };
    const int nodes = def->nodeCount(), steps = d.history.size();
    test_assert(sc::invoke(d.ctx, "js.cmd").status.ok());
    test_assert(def->nodeCount() == nodes + 1 && d.history.size() == steps + 1);
    test_assert(sc::undo(d.ctx).ok() && def->nodeCount() == nodes);
    test_assert(hasCode(inUndo, StatusCode::Busy) &&
                !d.get("graphs[\"inner\"].name").ok());

    /* Without an executor, or the commands. */
    Context bare(&d.rt, &d.lib, d.rt.ext<ApiRegistry>().runtimeOwner("doc"));
    test_assert(
        hasCode(sc::graphCreate(bare, "x").status, StatusCode::FailedPrecondition));
    test_assert(
        hasCode(sc::graphRemoveNode(bare, gp, n), StatusCode::FailedPrecondition));
    Runtime empty;
    Library lib(&empty);
    LinearHistory history;
    CommandExecutor ex(&empty, &history);
    Context ctx(&empty, &lib, PointerRef());
    ctx.setExecutor(&ex);
    test_assert(hasCode(sc::graphCreate(ctx, "x").status, StatusCode::NotFound));
  }
  test_assert(host.live() == 0 && host.allReleasedOnce());
}

void testInstances()
{
  Host host;
  sc::ScriptGraph late;
  {
    Doc d;
    std::function<void()> inside = []() {};
    test_assert(registerScale(d, host, "js.scale", [&](NodeExecContext &n) {
                  inside();
                  return scale(n);
                }).ok());
    const int64_t n = makeGraph(d, "js.scale");
    GraphDef *def = nullptr;
    BlockHandle block;
    d.lib.forEach([&](BlockHandle h, const Datablock &) {
      block = h;
      def = graphDefOf(d.lib, h);
    });
    NodeId input;
    def->forEachNode([&](const GraphNode &x) {
      input = strcmp(x.typeName().c_str(), "graph.input") == 0 ? x.id() : input;
    });
    const int64_t in = def->interfaceSocket(SocketSide::Input, "in")->id;
    connect(*def, input, key(in), NodeId{n}, "a");

    test_assert(sc::graphInstanceCount(d.ctx) == 0);
    std::optional<sc::ScriptGraph> g = sc::graphInstance(d.ctx, "graphs[\"g\"]").value;
    auto is = [&](double want) {
      sc::ValueResult r = sc::graphEvaluate(*g, "out");
      return r.status.ok() && r.value == sc::makeFloat(want);
    };
    test_assert(sc::graphInstanceCount(d.ctx) == 1 && is(0));
    test_assert(sc::graphSetInput(*g, "in", sc::makeInt(4)).ok() &&
                is(12)); /* converted */
    test_assert(hasCode(sc::graphSetInput(*g, "in", sc::makeString("x").value),
                        StatusCode::TypeMismatch));
    Status no = sc::graphSetInput(*g, "inn", sc::makeInt(4));
    test_assert(hasCode(no, StatusCode::NotFound) && no.suggestions().size() == 1);
    test_assert(is(12) && sc::graphClearInput(*g, "in").ok() && is(0));
    test_assert(hasCode(sc::graphClearInput(*g, "inn"), StatusCode::NotFound));
    test_assert(hasCode(sc::graphEvaluate(*g, "no").status, StatusCode::NotFound));
    /* A payload output. */
    test_assert(d.rt.ext<SocketTypeRegistry>().addPayloadType<Mesh>().ok());
    addSocket(*def, SocketSide::Output, "mesh", "t.Mesh");
    addSocket(*def, SocketSide::Input, "meshIn", "t.Mesh");
    test_assert(hasCode(sc::graphEvaluate(*g, "mesh").status, StatusCode::Unsupported));
    test_assert(hasCode(sc::graphSetInput(*g, "meshIn", sc::makeInt(1)),
                        StatusCode::Unsupported));

    /* The graph goes away, and comes back. */
    test_assert(d.lib.markDeleted(block).ok());
    test_assert(hasCode(sc::graphEvaluate(*g, "out").status, StatusCode::NotFound));
    test_assert(
        hasCode(sc::graphSetInput(*g, "in", sc::makeInt(1)), StatusCode::NotFound));
    test_assert(d.lib.restoreFromHistory(block).ok() && is(0));

    /* Copies share it. Dropping the last one in its own kernel, or closing it there, lets
     * the evaluation finish. */
    sc::ScriptGraph copy = *g;
    test_assert(sc::graphSetInput(copy, "in", sc::makeFloat(1)).ok() && is(3));
    copy = sc::ScriptGraph();
    test_assert(is(3));
    inside = [&]() { g.reset(); };
    test_assert(sc::graphSetInput(*g, "in", sc::makeFloat(2)).ok() && is(6));
    test_assert(!g && sc::graphInstanceCount(d.ctx) == 0);
    g = sc::graphInstance(d.ctx, "graphs[\"g\"]").value;
    copy = *g;
    inside = [&]() { sc::graphClose(copy); };
    test_assert(is(0));
    test_assert(
        hasCode(sc::graphEvaluate(*g, "out").status, StatusCode::FailedPrecondition));
    test_assert(hasCode(sc::graphSetInput(*g, "in", sc::makeInt(1)),
                        StatusCode::FailedPrecondition));
    test_assert(hasCode(sc::graphClearInput(*g, "in"), StatusCode::FailedPrecondition));
    test_assert(sc::graphErrorText(*g, "out").size() > 0 &&
                sc::graphInstanceCount(d.ctx) == 0);
    sc::graphClose(*g);
    sc::graphClose(sc::ScriptGraph());
    test_assert(hasCode(sc::graphEvaluate(sc::ScriptGraph(), "x").status,
                        StatusCode::FailedPrecondition));
    inside = []() {};
    d.rt.touch();

    /* One that outlives the Runtime is closed by it. */
    late = sc::graphInstance(d.ctx, "graphs[\"g\"]").value;
    test_assert(sc::graphEvaluate(late, "out").status.ok());
    g.reset();
  }
  test_assert(
      hasCode(sc::graphEvaluate(late, "out").status, StatusCode::FailedPrecondition));
  test_assert(hasCode(sc::graphSetInput(late, "in", sc::makeInt(1)),
                      StatusCode::FailedPrecondition));
  late = sc::ScriptGraph();
  test_assert(host.live() == 0 && host.allReleasedOnce());
}

} // namespace

int main()
{
  testRegistration();
  testKernel();
  testGates();
  testEditing();
  testInstances();
  test_assert(sc::bindings());
  return test_end();
}
