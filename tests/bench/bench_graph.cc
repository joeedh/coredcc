/* The graph benchmarks: one JSON object per line on stdout, with the time of a repetition
 * (minimum and median, milliseconds) and how many kernels it ran.
 *
 *   bench_graph [--reps N] [--scale D]     sizes are divided by D, for a quick look */
#include "coredcc/api/change_bus.h"
#include "coredcc/base/iface/owned.h"
#include "coredcc/base/iface/registry.h"
#include "coredcc/graph/def.h"
#include "coredcc/graph/foreach.h"
#include "coredcc/graph/instance.h"
#include "coredcc/graph/interior.h"
#include "coredcc/graph/provider.h"
#include "coredcc/lib/library.h"

#include "litestl/binding/binding.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

using namespace coredcc;

namespace {

int g_reps = 20;
int g_scale = 1;
volatile double g_sink = 0;

int sized(int n)
{
  return std::max(2, n / g_scale);
}

void check(bool ok, const char *what)
{
  if (!ok) {
    fprintf(stderr, "bench_graph: %s failed\n", what);
    exit(1);
  }
}

/* Runs `fn` after `before` (not timed) `reps` times. `calls` repeats fn inside a
 * repetition, for what is too fast to time. */
void measure(const char *name,
             int size,
             const std::function<void(int)> &before,
             const std::function<void(int)> &fn,
             const std::function<uint64_t()> &runs,
             int calls = 1)
{
  std::vector<double> times;
  uint64_t ran = 0;
  for (int i = 0; i < g_reps; i++) {
    if (before) {
      before(i);
    }
    const uint64_t first = runs ? runs() : 0;
    const auto start = std::chrono::steady_clock::now();
    for (int c = 0; c < calls; c++) {
      fn(i);
    }
    const std::chrono::duration<double, std::milli> t = std::chrono::steady_clock::now() - start;
    times.push_back(t.count() / calls);
    ran = (runs ? runs() : 0) - first;
  }
  std::sort(times.begin(), times.end());
  printf("{\"bench\": \"%s\", \"size\": %d, \"min_ms\": %.6f, \"median_ms\": %.6f, \"kernels\": %llu}\n",
         name, size, times[0], times[times.size() / 2], (unsigned long long)ran);
  fflush(stdout);
}

/* A block with a number, and a node that moves its parent's matrix by it. */
struct Thing {
  double value = 0;
};

struct ThingProvider {
  void *data(const Datablock &b) const
  {
    return b.clientData();
  }
  void foreachRef(const Datablock &, RefVisitor &) const {}
  void destroy(const Datablock &b)
  {
    litestl::alloc::Delete(static_cast<Thing *>(b.clientData()));
  }
  static litestl::binding::types::Struct<ThingProvider> *defineBindings()
  {
    auto *st = new litestl::binding::types::Struct<ThingProvider>("bench::ThingProvider",
                                                                  sizeof(ThingProvider));
    BIND_STRUCT_METHOD(st, data, MARGS("block"));
    BIND_STRUCT_METHOD(st, foreachRef, MARGS("block", "visitor"));
    BIND_STRUCT_METHOD(st, destroy, MARGS("block"));
    return st;
  }
};

Status transform(NodeExecContext &ctx)
{
  const BlockUse use = ctx.paramBlock("object");
  double m[16];
  std::copy_n(ctx.input(0).components(), 16, m);
  m[12] += use.data ? static_cast<Thing *>(use.data)->value : 0;
  return ctx.setOutput(0, GraphValue::ofComponents(GraphValue::Type::Mat4, m));
}

struct World {
  Runtime rt;
  Library lib;
  World() : lib(&rt)
  {
    check(registerGraphBlocks(rt).ok(), "registering");
    check(registerImpl<BlockProvider, ThingProvider>(rt, "bench.Thing#provider").ok(), "a provider");
    Result<Owned<BlockProvider>> p = own<BlockProvider>(
        rt, "bench.Thing#provider", litestl::alloc::New<ThingProvider>("provider"));
    BlockTypeInfo info;
    info.name = "bench.Thing";
    info.client = ClientId::of(kCoredccClient);
    info.dataStruct = StructId::of("Thing");
    check(rt.ext<BlockTypeRegistry>().add(info, std::move(p).value()).ok(), "a block type");
    NodeTypeDef def("bench.transform");
    def.input(PropertyDef::mat4Prop("parent"))
        .output("world", "mat4")
        .param(PropertyDef::pointerProp("object", StructId::of("Thing")), true);
    Result<Owned<GraphKernel>> k = makeKernel<FnKernel<transform>>(rt, "bench.transform#kernel");
    def.kernel(std::move(k).value());
    check(rt.ext<NodeTypeRegistry>().add(std::move(def)).ok(), "a node type");
  }
};

NodeId add(GraphDef &d, const char *type)
{
  Result<NodeId> n = d.addNode(type);
  check(n.ok(), type);
  return *n;
}

void link(GraphDef &d, NodeId a, const std::string &from, NodeId b, const std::string &to)
{
  check(d.link({a, from}, {b, to}).ok(), "a link");
}

std::string key(int64_t id)
{
  return interfaceKey(id).c_str();
}

int64_t socket(GraphDef &d, SocketSide side, const char *name, const char *type)
{
  Result<int64_t> id = d.addInterface(side, name, type);
  check(id.ok(), name);
  return *id;
}

PropValue translation(double x)
{
  double m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, x, 0, 0, 1};
  return GraphValue::ofComponents(GraphValue::Type::Mat4, m).toProp();
}

struct Def {
  GraphDef *def = nullptr;
  BlockId id;
};

Def newDef(World &w, const char *name)
{
  Result<BlockHandle> h = createGraph(w.lib, name);
  check(h.ok(), name);
  return Def{graphDefOf(w.lib, *h), w.lib.get(*h)->id()};
}

/* ---- a chain of matrix products ---- */

void benchChain()
{
  World w;
  const int n = sized(10000);
  GraphDef def(&w.rt, &w.lib);
  std::vector<NodeId> nodes;
  measure("chain: building", n, nullptr, [&](int) {
    for (NodeId id : nodes) {
      check(def.removeNode(id).ok(), "removing");
    }
    nodes.clear();
    for (int i = 0; i < n; i++) {
      nodes.push_back(add(def, "mat4.multiply"));
      check(def.setLiteral(nodes.back(), "b", translation(1)).ok(), "a literal");
      if (i > 0) {
        link(def, nodes[size_t(i - 1)], "matrix", nodes[size_t(i)], "a");
      }
    }
  }, nullptr);
  const SocketPath end = SocketPath::of(nodes.back(), "matrix");
  std::unique_ptr<GraphInstance> inst;
  auto pull = [&](int) {
    check(inst->evaluate(end).ok(), "evaluating");
    g_sink = inst->value(end)->components()[12];
  };
  auto runs = [&]() { return inst->totalRuns(); };
  measure("chain: full", n, [&](int) { inst = std::make_unique<GraphInstance>(&w.rt, &w.lib, &def); },
          pull, runs);
  measure("chain: unchanged", n, nullptr, pull, runs, 10000);
  measure("chain: one edit at the end (verifies all)", n,
          [&](int i) { check(def.setLiteral(nodes.back(), "b", translation(i)).ok(), "a literal"); },
          pull, runs);
  measure("chain: an edit elsewhere (verifies all)", n, [&](int) { w.rt.touch(); }, pull, runs);
  measure("chain: one edit at the start (runs all)", n,
          [&](int i) { check(def.setLiteral(nodes[0], "b", translation(i)).ok(), "a literal"); }, pull,
          runs);
  measure("chain: cut and joined at the end", n,
          [&](int) {
            check(def.unlink(def.node(nodes.back())->linkInto("a")->id).ok(), "unlinking");
            link(def, nodes[size_t(n - 2)], "matrix", nodes.back(), "a");
          },
          pull, runs);
}

/* ---- a scene: transforms of blocks, all requested ---- */

void benchScene(int n, const char *shape)
{
  World w;
  GraphDef def(&w.rt, &w.lib);
  std::vector<BlockId> blocks;
  std::vector<NodeId> nodes;
  const bool flat = strcmp(shape, "flat") == 0, chain = strcmp(shape, "chain") == 0;
  auto build = [&]() {
    for (int i = 0; i < n; i++) {
      Result<BlockHandle> h =
          w.lib.create(TypeId::of("bench.Thing"), "thing", litestl::alloc::New<Thing>("thing"));
      blocks.push_back(w.lib.get(*h)->id());
      nodes.push_back(add(def, "bench.transform"));
      StableRef r;
      r.owner = OwnerKey::ofBlock(blocks.back());
      r.type = StructId::of("Thing");
      check(def.setParam(nodes.back(), "object", PropValue::ofPointer(r)).ok(), "a param");
      if (i > 0) {
        const int parent = flat ? 0 : chain ? i - 1 : (i - 1) / 8;
        link(def, nodes[size_t(parent)], "world", nodes[size_t(i)], "parent");
      }
    }
  };
  build();
  std::vector<SocketPath> paths;
  for (NodeId id : nodes) {
    paths.push_back(SocketPath::of(id, "world"));
  }
  std::unique_ptr<GraphInstance> inst;
  std::vector<SocketHandle> handles;
  litestl::util::Vector<int> moved;
  litestl::util::Vector<Status> results;
  auto fresh = [&](int) {
    inst = std::make_unique<GraphInstance>(&w.rt, &w.lib, &def);
    handles.clear();
    for (const SocketPath &p : paths) {
      handles.push_back(*inst->prepare(p));
    }
  };
  auto pull = [&](int) { check(inst->evaluate(handles, moved).ok(), "evaluating"); };
  auto pullPaths = [&](int) { check(inst->evaluate(paths, results).ok(), "evaluating"); };
  auto runs = [&]() { return inst->totalRuns(); };
  auto edit = [&](int at) {
    static_cast<Thing *>(w.lib.data(w.lib.find(blocks[size_t(at)])))->value += 1;
    w.rt.ext<ChangeBus>().notify(OwnerKey::ofBlock(blocks[size_t(at)]));
  };
  auto name = [&](const char *what) { return std::string("scene, ") + shape + ": " + what; };
  measure(name("first").c_str(), n, fresh, pull, runs);
  measure(name("unchanged").c_str(), n, nullptr, pull, runs);
  measure(name("a leaf's block").c_str(), n, [&](int) { edit(n - 1); }, pull, runs);
  measure(name("a leaf's block, by paths").c_str(), n, [&](int) { edit(n - 1); }, pullPaths, runs);
  if (!chain) {
    measure(name("100 blocks of 1 to 73 nodes each").c_str(), n,
            [&](int) {
              for (int i = 0; i < 100; i++) {
                edit(n - 1 - i * (n / 100));
              }
            },
            pull, runs);
  }
  measure(name("the root's block (runs all)").c_str(), n, [&](int) { edit(0); }, pull, runs);
  measure(name("the root's block, and paths (runs all)").c_str(), n, [&](int) { edit(0); },
          pullPaths, runs);
  measure(name("touch").c_str(), n, [&](int) { w.rt.touch(); }, pull, runs);
  measure(name("another graph's edit").c_str(), n,
          [&](int) {
            GraphDef other(&w.rt, &w.lib);
            add(other, "math.float");
          },
          pull, runs);
  measure(name("a leaf put under the root").c_str(), n,
          [&](int) { link(def, nodes[0], "world", nodes[size_t(n - 1)], "parent"); }, pull, runs);
  const GraphMemory m = inst->memory();
  printf("{\"bench\": \"%s\", \"size\": %d, \"bytes_per_node\": %.0f}\n",
         name("memory, with handles and paths").c_str(), n, double(m.bytes) / n);
  measure(name("removing all but the root").c_str(), n,
          [&](int) {
            for (NodeId id : nodes) {
              if (def.node(id)) {
                check(def.removeNode(id).ok(), "removing");
              }
            }
            nodes.clear();
            blocks.clear();
            build();
          },
          [&](int) {
            for (size_t i = 1; i < nodes.size(); i++) {
              check(def.removeNode(nodes[i]).ok(), "removing");
            }
          },
          nullptr);
}

/* ---- wide nodes ---- */

void benchInterior()
{
  World w;
  const int n = sized(1600);
  const Def inner = newDef(w, "inner");
  std::vector<NodeId> in;
  for (int i = 0; i < n; i++) {
    in.push_back(add(*inner.def, "math.float"));
  }
  GraphDef def(&w.rt, &w.lib);
  const NodeId group = add(def, "graph.group");
  check(def.setGroup(group, inner.id).ok(), "a group");
  std::vector<NodeId> from, to;
  std::vector<std::string> inputs, outputs;
  std::vector<SocketPath> sockets;
  for (int i = 0; i < n; i++) {
    from.push_back(add(def, "math.float"));
    to.push_back(add(def, "math.float"));
    inputs.push_back("@" + std::to_string(in[size_t(i)].value) + ".a");
    outputs.push_back("@" + std::to_string(in[size_t(i)].value) + ".value");
    sockets.push_back(SocketPath::of(to.back(), "value"));
  }
  std::vector<LinkSpec> specs;
  for (int i = 0; i < n; i++) {
    specs.push_back(LinkSpec{{from[size_t(i)], "value"}, {group, inputs[size_t(i)]}});
    specs.push_back(LinkSpec{{group, outputs[size_t(i)]}, {to[size_t(i)], "a"}});
  }
  litestl::util::Vector<LinkId> ids;
  auto unlink = [&](int) {
    for (LinkId id : ids) {
      check(def.unlink(id).ok(), "unlinking");
    }
    ids.clear();
  };
  measure("interior sockets: linkAll", n, unlink,
          [&](int) { check(def.linkAll(specs, ids).ok(), "linking"); }, nullptr);
  measure("interior sockets: one more link", n, nullptr,
          [&](int) { link(def, from[0], "value", group, inputs[0]); }, nullptr);
  std::unique_ptr<GraphInstance> inst;
  litestl::util::Vector<Status> results;
  auto pull = [&](int) { check(inst->evaluate(sockets, results).ok(), "evaluating"); };
  auto runs = [&]() { return inst->totalRuns(); };
  measure("interior sockets: first", n,
          [&](int) { inst = std::make_unique<GraphInstance>(&w.rt, &w.lib, &def); }, pull, runs);
  measure("interior sockets: touch", n, [&](int) { w.rt.touch(); }, pull, runs);
  measure("interior sockets: one input", n,
          [&](int i) { check(def.setLiteral(from[5], "a", PropValue::ofFloat(i)).ok(), "a literal"); },
          pull, runs);
}

void benchInterface()
{
  World w;
  const int n = sized(2000);
  std::vector<int64_t> ids;
  Def inner;
  measure("interface: sockets", n, [&](int) { ids.clear(), inner = newDef(w, "inner"); },
          [&](int) {
            for (int i = 0; i < n; i++) {
              ids.push_back(socket(*inner.def, SocketSide::Input, ("s" + std::to_string(i)).c_str(),
                                   "float"));
            }
          },
          nullptr);
  std::vector<LinkSpec> specs;
  std::vector<std::string> keys;
  for (int64_t id : ids) {
    keys.push_back(key(id));
  }
  GraphDef def(&w.rt, &w.lib);
  const NodeId group = add(def, "graph.group"), from = add(def, "math.float");
  check(def.setGroup(group, inner.id).ok(), "a group");
  for (const std::string &k : keys) {
    specs.push_back(LinkSpec{{from, "value"}, {group, k}});
  }
  litestl::util::Vector<LinkId> links;
  measure("interface: linkAll", n, nullptr,
          [&](int) { check(def.linkAll(specs, links).ok(), "linking"); }, nullptr);
  measure("interface: diagnostics", n, nullptr,
          [&](int) { check(def.diagnostics().size() == 0, "diagnostics"); }, nullptr);
}

/* ---- many defs ---- */

void benchDefs()
{
  World w;
  const int n = sized(4000);
  GraphDef def(&w.rt, &w.lib);
  std::vector<Def> defs;
  std::vector<SocketPath> sockets;
  for (int i = 0; i < n; i++) {
    defs.push_back(newDef(w, "def"));
    GraphDef &d = *defs.back().def;
    const int64_t out = socket(d, SocketSide::Output, "out", "float");
    const NodeId value = add(d, "math.float");
    link(d, value, "value", add(d, "graph.output"), key(out));
    const NodeId group = add(def, "graph.group");
    check(def.setGroup(group, defs.back().id).ok(), "a group");
    sockets.push_back(SocketPath::of(group, key(out)));
  }
  GraphInstance inst(&w.rt, &w.lib, &def);
  std::vector<SocketHandle> handles;
  for (const SocketPath &p : sockets) {
    handles.push_back(*inst.prepare(p));
  }
  litestl::util::Vector<int> moved;
  auto pull = [&](int) { check(inst.evaluate(handles, moved).ok(), "evaluating"); };
  auto runs = [&]() { return inst.totalRuns(); };
  pull(0);
  measure("defs: a literal in one", n,
          [&](int i) {
            GraphDef &d = *defs[size_t(i)].def;
            check(d.setLiteral(d.nodeAt(0)->id(), "a", PropValue::ofFloat(i + 1)).ok(), "a literal");
          },
          pull, runs);
  measure("defs: a node more in one", n,
          [&](int i) { add(*defs[size_t(i % n)].def, "math.float"); }, pull, runs);
  measure("defs: a node less in one", n,
          [&](int i) {
            GraphDef &d = *defs[size_t(i % n)].def;
            check(d.removeNode(add(d, "math.float")).ok(), "removing");
          },
          pull, runs);
}

/* ---- the size of a foreach's cache ---- */

void benchForeachMemory(int maxCached, int kernels)
{
  World w;
  const int n = sized(16384), inside = 50;
  const Def inner = newDef(w, "inner"), root = newDef(w, "root");
  const int64_t x = socket(*inner.def, SocketSide::Input, "x", "float");
  const int64_t out = socket(*inner.def, SocketSide::Output, "out", "float");
  const NodeId input = add(*inner.def, "graph.input"), output = add(*inner.def, "graph.output");
  NodeId last = input;
  for (int i = 0; i < inside; i++) {
    const NodeId node = add(*inner.def, "math.float");
    link(*inner.def, last, i ? "value" : key(x), node, "a");
    last = node;
  }
  link(*inner.def, last, "value", output, key(out));
  const int64_t table = socket(*root.def, SocketSide::Input, "table", "elements");
  const NodeId rootIn = add(*root.def, "graph.input"), each = add(*root.def, kGraphForeachType);
  check(root.def->setGroup(each, inner.id).ok(), "a group");
  check(root.def->setParam(each, kMaxCachedParam, PropValue::ofInt(maxCached)).ok(), "a param");
  link(*root.def, rootIn, key(table), each, kElementsSocket);
  Elements t;
  Elements::Column c;
  c.name = "x";
  for (int i = 0; i < n; i++) {
    t.keys.append(i);
    c.values.append(GraphValue::ofFloat(i));
  }
  t.columns.append(c);
  GraphInstance inst(&w.rt, &w.lib, root.id);
  inst.setMaxCachedKernels(kernels);
  check(inst.setInput("table", GraphValue::ofPayload(makePayload(t))).ok(), "the table");
  const size_t before = size_t(litestl::alloc::getMemorySize());
  check(inst.evaluate(SocketPath::of(each, kElementsSocket)).ok(), "evaluating");
  const GraphMemory m = inst.memory();
  printf("{\"bench\": \"foreach, memory: %d cached, %d kernels\", \"size\": %d, \"elements\": %d, "
         "\"kernels\": %d, \"mb\": %.1f, \"tracked_mb\": %.1f}\n",
         maxCached, kernels, n, m.elements, m.kernels, double(m.bytes) / 1e6,
         double(size_t(litestl::alloc::getMemorySize()) - before) / 1e6);
}

/* ---- many instances of a group ---- */

void benchGroups()
{
  World w;
  const int groups = sized(1000), inside = 100;
  const Def inner = newDef(w, "inner");
  const int64_t in = socket(*inner.def, SocketSide::Input, "in", "float");
  const int64_t out = socket(*inner.def, SocketSide::Output, "out", "float");
  const NodeId input = add(*inner.def, "graph.input"), output = add(*inner.def, "graph.output");
  NodeId last = input, first;
  for (int i = 0; i < inside; i++) {
    const NodeId n = add(*inner.def, "math.float");
    link(*inner.def, last, i ? "value" : key(in), n, "a");
    check(inner.def->setLiteral(n, "b", PropValue::ofFloat(1)).ok(), "a literal");
    first = i ? first : n;
    last = n;
  }
  link(*inner.def, last, "value", output, key(out));
  GraphDef def(&w.rt, &w.lib);
  std::vector<SocketPath> sockets;
  std::vector<NodeId> nodes;
  for (int i = 0; i < groups; i++) {
    nodes.push_back(add(def, "graph.group"));
    check(def.setGroup(nodes.back(), inner.id).ok(), "a group");
    check(def.setLiteral(nodes.back(), key(in), PropValue::ofFloat(i)).ok(), "a literal");
    sockets.push_back(SocketPath::of(nodes.back(), key(out)));
  }
  std::unique_ptr<GraphInstance> inst;
  litestl::util::Vector<Status> results;
  auto pull = [&](int) {
    check(inst->evaluate(sockets, results).ok() && results[0].ok(), "evaluating");
    g_sink = inst->value(sockets[0])->asFloat();
  };
  auto runs = [&]() { return inst->totalRuns(); };
  const int n = groups * inside;
  measure("groups: full", n, [&](int) { inst = std::make_unique<GraphInstance>(&w.rt, &w.lib, &def); },
          pull, runs);
  measure("groups: unchanged", n, nullptr, pull, runs, 100);
  measure("groups: one group's input", n,
          [&](int i) {
            check(def.setLiteral(nodes[0], key(in), PropValue::ofFloat(-1 - i)).ok(), "a literal");
          },
          pull, runs);
  measure("groups: an edit elsewhere", n, [&](int) { w.rt.touch(); }, pull, runs);
  measure("groups: an edit of the group's end", n,
          [&](int i) { check(inner.def->setLiteral(last, "b", PropValue::ofFloat(2 + i)).ok(), "a literal"); },
          pull, runs);
  measure("groups: an edit of the group's start", n,
          [&](int i) { check(inner.def->setLiteral(first, "b", PropValue::ofFloat(2 + i)).ok(), "a literal"); },
          pull, runs);
  measure("groups: structure of the group", n,
          [&](int) { check(inner.def->removeNode(add(*inner.def, "math.float")).ok(), "removing"); }, pull, runs);
  /* Interior sockets: every group has one input inside replaced. */
  const NodeId driver = add(def, "math.float");
  const std::string interior = "@" + std::to_string(last.value) + ".b";
  for (NodeId g : nodes) {
    link(def, driver, "value", g, interior);
  }
  measure("groups: an interior input of each", n,
          [&](int i) { check(def.setLiteral(driver, "a", PropValue::ofFloat(i)).ok(), "a literal"); }, pull, runs);
}

/* ---- foreach ---- */

void benchForeach(int maxCached, int count = 10000)
{
  World w;
  const int n = sized(count), inside = 10;
  const Def inner = newDef(w, "inner"), root = newDef(w, "root");
  const int64_t x = socket(*inner.def, SocketSide::Input, "x", "float");
  const int64_t y = socket(*inner.def, SocketSide::Input, "y", "float");
  const int64_t out = socket(*inner.def, SocketSide::Output, "out", "float");
  const NodeId input = add(*inner.def, "graph.input"), output = add(*inner.def, "graph.output");
  NodeId last = input;
  for (int i = 0; i < inside; i++) {
    const NodeId node = add(*inner.def, "math.float");
    link(*inner.def, last, i ? "value" : key(x), node, "a");
    if (i == inside / 2) {
      link(*inner.def, input, key(y), node, "b");
    }
    last = node;
  }
  link(*inner.def, last, "value", output, key(out));
  const int64_t table = socket(*root.def, SocketSide::Input, "table", "elements");
  const NodeId rootIn = add(*root.def, "graph.input"), each = add(*root.def, kGraphForeachType);
  check(root.def->setGroup(each, inner.id).ok(), "a group");
  check(root.def->setParam(each, kMaxCachedParam, PropValue::ofInt(maxCached)).ok(), "a param");
  link(*root.def, rootIn, key(table), each, kElementsSocket);

  Elements t;
  Elements::Column c;
  c.name = "x";
  for (int i = 0; i < n; i++) {
    t.keys.append(i * 7 + 3);
    c.values.append(GraphValue::ofFloat(i));
  }
  t.columns.append(c);
  std::unique_ptr<GraphInstance> inst;
  const SocketPath p = SocketPath::of(each, kElementsSocket);
  auto set = [&]() {
    check(inst->setInput("table", GraphValue::ofPayload(makePayload(t))).ok(), "the table");
  };
  auto pull = [&](int) {
    check(inst->evaluate(p).ok(), "evaluating");
    g_sink = inst->value(p)->payload<Elements>()->columns[0].values[0].asFloat();
  };
  auto runs = [&]() { return inst->totalRuns(); };
  auto name = [&](const char *what) {
    return std::string("foreach, ") +
           (maxCached == 0 ? "shared: " : maxCached == 4096 ? "default: " : "cached: ") + what;
  };
  measure(name("full").c_str(), n,
          [&](int) {
            inst = std::make_unique<GraphInstance>(&w.rt, &w.lib, root.id);
            inst->setMaxCachedKernels(maxCached > 4096 ? 1 << 30 : 131072);
            set();
          },
          pull, runs);
  measure(name("unchanged").c_str(), n, nullptr, pull, runs, 1000);
  measure(name("an edit elsewhere").c_str(), n, [&](int) { w.rt.touch(); }, pull, runs);
  measure(name("one element").c_str(), n,
          [&](int i) {
            t.columns[0].values[n / 2] = GraphValue::ofFloat(-1 - i);
            set();
          },
          pull, runs);
  measure(name("a new order").c_str(), n,
          [&](int) {
            std::reverse(t.keys.begin(), t.keys.end());
            std::reverse(t.columns[0].values.begin(), t.columns[0].values.end());
            set();
          },
          pull, runs);
  measure(name("the node's input").c_str(), n,
          [&](int i) { check(root.def->setLiteral(each, key(y), PropValue::ofFloat(i + 1)).ok(), "a literal"); },
          pull, runs);
  measure(name("an edit inside").c_str(), n,
          [&](int i) { check(inner.def->setLiteral(last, "b", PropValue::ofFloat(i + 1)).ok(), "a literal"); },
          pull, runs);
}

} // namespace

int main(int argc, char **argv)
{
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--reps") == 0 && i + 1 < argc) {
      g_reps = std::max(1, atoi(argv[++i]));
    }
    else if (strcmp(argv[i], "--scale") == 0 && i + 1 < argc) {
      g_scale = std::max(1, atoi(argv[++i]));
    }
    else {
      fprintf(stderr, "usage: bench_graph [--reps N] [--scale D]\n");
      return 2;
    }
  }
  benchChain();
  benchScene(sized(10000), "tree");
  benchScene(sized(10000), "flat");
  benchScene(sized(10000), "chain");
  benchScene(sized(100000), "tree");
  benchInterior();
  benchInterface();
  benchDefs();
  benchGroups();
  benchForeach(1 << 20);
  benchForeach(0);
  benchForeach(4096, 100000);
  benchForeachMemory(16384, 1 << 30);
  benchForeachMemory(4096, 131072);
  return 0;
}
