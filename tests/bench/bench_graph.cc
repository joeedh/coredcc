/* The graph benchmarks: one JSON object per line on stdout, with the time of a repetition
 * (minimum and median, milliseconds) and how many kernels it ran.
 *
 *   bench_graph [--reps N] [--scale D]     sizes are divided by D, for a quick look */
#include "coredcc/api/change_bus.h"
#include "coredcc/graph/def.h"
#include "coredcc/graph/foreach.h"
#include "coredcc/graph/instance.h"
#include "coredcc/graph/interior.h"
#include "coredcc/graph/provider.h"
#include "coredcc/lib/library.h"

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

struct World {
  Runtime rt;
  Library lib;
  World() : lib(&rt)
  {
    check(registerGraphBlocks(rt).ok(), "registering");
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

void benchForeach(int maxCached)
{
  World w;
  const int n = sized(10000), inside = 10;
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
    return std::string("foreach, ") + (maxCached ? "cached: " : "shared: ") + what;
  };
  measure(name("full").c_str(), n,
          [&](int) {
            inst = std::make_unique<GraphInstance>(&w.rt, &w.lib, root.id);
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
  benchGroups();
  benchForeach(1 << 20);
  benchForeach(0);
  return 0;
}
