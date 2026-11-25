/* What grows with the size of a graph: sixteen times the size may take no more than 64
 * times as long, where something quadratic takes 256. And what an edit looks at, which
 * doesn't grow. */
#include "edit_fixture.h"
#include "foreach_fixture.h"

#include <algorithm>
#include <chrono>
#include <functional>
#include <vector>

using namespace fixture;

test_init;

namespace {

double secondsOf(const std::function<void()> &fn)
{
  const auto start = std::chrono::steady_clock::now();
  fn();
  const std::chrono::duration<double> t = std::chrono::steady_clock::now() - start;
  return t.count();
}

/* `run(n)` gives the seconds that matter. The best of a few, and a few more if it looks
 * bad: the machine may be busy. */
bool grows(const char *name, int n, const std::function<double(int)> &run)
{
  double small = 1e9, large = 1e9;
  for (int tries = 0; tries < 8; tries++) {
    small = std::min(small, run(n));
    large = std::min(large, run(16 * n));
    if (tries >= 1 && large < 40 * small + 0.002) {
      break;
    }
  }
  const bool ok = large < 64 * small + 0.002;
  if (!ok) {
    printf(
        "%s: %g ms for %d, %g ms for %d\n", name, small * 1000, n, large * 1000, 16 * n);
  }
  return ok;
}

/* ---- edits ---- */

struct Fan {
  World w;
  Graph g = newGraph(w.lib, "g");
  NodeId root = add(*g.def, "t.sum");
  std::vector<NodeId> nodes;
  std::vector<LinkSpec> specs;
  litestl::util::Vector<LinkId> ids;

  explicit Fan(int n)
  {
    for (int i = 0; i < n; i++) {
      nodes.push_back(add(*g.def, "t.sum"));
      specs.push_back({{root, "value"}, {nodes.back(), "a"}});
    }
  }
};

void testLinks()
{
  test_assert(grows("links out of a node", 500, [](int n) {
    Fan f(n);
    return secondsOf([&]() {
      for (const LinkSpec &s : f.specs) {
        connect(*f.g.def, s.from.node, s.from.key, s.to.node, s.to.key);
      }
    });
  }));
  test_assert(grows("removing nodes a node leads to", 500, [](int n) {
    Fan f(n);
    test_assert(f.g.def->linkAll(f.specs, f.ids).ok());
    return secondsOf([&]() {
      for (NodeId id : f.nodes) {
        test_assert(f.g.def->removeNode(id).ok());
      }
    });
  }));
  test_assert(grows("unlinking, the last first", 500, [](int n) {
    Fan f(n);
    test_assert(f.g.def->linkAll(f.specs, f.ids).ok());
    return secondsOf([&]() {
      for (int i = n - 1; i >= 0; i--) {
        test_assert(f.g.def->unlink(f.ids[i]).ok());
      }
    });
  }));
  test_assert(grows("linkAll of a chain", 500, [](int n) {
    Fan f(n);
    std::vector<LinkSpec> chain;
    for (size_t i = 1; i < f.nodes.size(); i++) {
      chain.push_back({{f.nodes[i - 1], "value"}, {f.nodes[i], "b"}});
    }
    return secondsOf([&]() { test_assert(f.g.def->linkAll(chain, f.ids).ok()); });
  }));
  test_assert(grows("each link's diagnostic", 250, [](int n) {
    Fan f(n);
    test_assert(f.g.def->linkAll(f.specs, f.ids).ok());
    return secondsOf([&]() {
      for (LinkId id : f.ids) {
        test_assert(!f.g.def->diagnostic(id));
      }
    });
  }));
  test_assert(grows("a link in a chain that is as long as it gets", 500, [](int n) {
    Fan f(n);
    std::vector<LinkSpec> chain;
    for (size_t i = 1; i < f.nodes.size(); i++) {
      chain.push_back({{f.nodes[i - 1], "value"}, {f.nodes[i], "b"}});
    }
    test_assert(f.g.def->linkAll(chain, f.ids).ok());
    return secondsOf([&]() {
      test_assert(hasCode(f.g.def->link({f.nodes.back(), "value"}, {f.nodes[0], "b"}),
                          StatusCode::InvalidArgument));
    });
  }));
}

/* A group of a def with `n` inputs, or sums inside. */
struct Wide {
  World w;
  Graph in = newGraph(w.lib, "in"), g = newGraph(w.lib, "g");
  NodeId group = add(*g.def, "graph.group"), from = add(*g.def, "t.sum");
  std::vector<int64_t> sockets;
  std::vector<std::string> keys;
  std::vector<NodeId> to;
  std::vector<LinkSpec> specs;
  litestl::util::Vector<LinkId> ids;

  Wide()
  {
    test_assert(g.def->setGroup(group, in.id).ok());
  }
  void interface(int n)
  {
    for (int i = 0; i < n; i++) {
      sockets.push_back(addSocket(
          *in.def, SocketSide::Input, ("s" + std::to_string(i)).c_str(), "float"));
      keys.push_back(key(sockets.back()));
    }
    for (const std::string &k : keys) {
      specs.push_back({{from, "value"}, {group, k}});
    }
  }
  /* Links into sums inside, and out of them. */
  void interior(int n)
  {
    for (int i = 0; i < n; i++) {
      const std::string id = "@" + std::to_string(add(*in.def, "t.sum").value);
      keys.push_back(id + ".a");
      keys.push_back(id + ".value");
      to.push_back(add(*g.def, "t.sum"));
    }
    for (size_t i = 0; i < to.size(); i++) {
      specs.push_back({{from, "value"}, {group, keys[2 * i]}});
      specs.push_back({{group, keys[2 * i + 1]}, {to[i], "a"}});
    }
  }
  double linkAll()
  {
    return secondsOf([&]() { test_assert(g.def->linkAll(specs, ids).ok()); });
  }
  double evaluate(GraphInstance &i)
  {
    std::vector<SocketPath> paths;
    for (NodeId n : to) {
      paths.push_back(SocketPath::of(n, "value"));
    }
    for (const std::string &k : keys) {
      if (to.empty()) {
        paths.push_back(SocketPath::of(group, k, SocketSide::Input));
      }
    }
    litestl::util::Vector<Status> results;
    return secondsOf([&]() {
      test_assert(i.evaluate(paths, results).ok());
      for (const Status &s : results) {
        test_assert(s.ok());
      }
    });
  }
};

void testWide()
{
  for (const bool inside : {false, true}) {
    auto wide = [inside](int n) {
      auto out = std::make_unique<Wide>();
      inside ? out->interior(n) : out->interface(n);
      return out;
    };
    const std::string name = inside ? "interior sockets: " : "an interface: ";
    test_assert(grows(
        (name + "linkAll").c_str(), 250, [&](int n) { return wide(n)->linkAll(); }));
    test_assert(grows((name + "evaluate").c_str(), 250, [&](int n) {
      const auto f = wide(n);
      f->linkAll();
      GraphInstance i(&f->w.rt, &f->w.lib, f->g.id);
      return f->evaluate(i);
    }));
    test_assert(grows((name + "evaluate again").c_str(), 250, [&](int n) {
      const auto f = wide(n);
      f->linkAll();
      GraphInstance i(&f->w.rt, &f->w.lib, f->g.id);
      f->evaluate(i);
      f->w.rt.touch();
      return f->evaluate(i);
    }));
    test_assert(grows((name + "diagnostics").c_str(), 250, [&](int n) {
      const auto f = wide(n);
      f->linkAll();
      return secondsOf([&]() { test_assert(f->g.def->diagnostics().size() == 0); });
    }));
    test_assert(grows((name + "unlink").c_str(), 250, [&](int n) {
      const auto f = wide(n);
      f->linkAll();
      return secondsOf([&]() {
        for (LinkId id : f->ids) {
          test_assert(f->g.def->unlink(id).ok());
        }
      });
    }));
    /* With all the others there. */
    test_assert(grows((name + "a link").c_str(), 250, [&](int n) {
      const auto f = wide(n);
      f->linkAll();
      return secondsOf(
          [&]() { connect(*f->g.def, f->from, "value", f->group, f->keys[0]); });
    }));
  }
}

/* What an undo step holds on to: a node with many references. */
void testPins()
{
  test_assert(grows("undo of a node with many references", 500, [](int n) {
    Doc d;
    const Graph g = newGraph(d.lib, "g");
    NodeState s;
    s.id = NodeId{1};
    s.typeName = "t.unknown";
    for (int i = 0; i < n; i++) {
      NodeState::Ref r;
      r.name = std::to_string(i).c_str();
      r.target = idOf(d.lib, newThing(d.lib, "thing", 0));
      s.refs.append(r);
    }
    test_assert(g.def->insertNode(s).ok());
    const double t = secondsOf([&]() {
      test_assert(runGraphEdit(d.ctx, g.id, "remove", [](GraphEdit &e) {
                    return e.removeNode(NodeId{1});
                  }).ok());
    });
    return t;
  }));
}

/* ---- evaluation ---- */

/* A tree of sums, or one with a wide root. */
struct Tree {
  World w;
  Graph g = newGraph(w.lib, "g");
  std::vector<NodeId> nodes;
  std::unique_ptr<GraphInstance> inst;
  std::vector<SocketHandle> handles;
  litestl::util::Vector<int> moved;

  Tree(int n, int width)
  {
    std::vector<LinkSpec> specs;
    for (int i = 0; i < n; i++) {
      nodes.push_back(add(*g.def, "t.sum"));
      if (i > 0) {
        specs.push_back({{nodes[size_t((i - 1) / width)], "value"}, {nodes.back(), "a"}});
      }
    }
    litestl::util::Vector<LinkId> ids;
    test_assert(g.def->linkAll(specs, ids).ok());
    inst = std::make_unique<GraphInstance>(&w.rt, &w.lib, g.id);
    for (NodeId id : nodes) {
      handles.push_back(*inst->prepare(SocketPath::of(id, "value")));
    }
  }
  void evaluate()
  {
    test_assert(inst->evaluate(handles, moved).ok());
  }
};

void testEvaluation()
{
  for (const int width : {1, 8, 1 << 20}) {
    const std::string name = "a tree of width " + std::to_string(width) + ": ";
    test_assert(grows((name + "first").c_str(), 250, [&](int n) {
      Tree t(n, width);
      return secondsOf([&]() { t.evaluate(); });
    }));
    test_assert(grows((name + "everything again").c_str(), 250, [&](int n) {
      Tree t(n, width);
      t.evaluate();
      test_assert(t.g.def->setLiteral(t.nodes[0], "b", PropValue::ofFloat(1)).ok());
      return secondsOf([&]() { t.evaluate(); });
    }));
    /* What an edit looks at is what is behind it, however much else there is. */
    uint64_t visits[2];
    for (int k = 0; k < 2; k++) {
      Tree t(k ? 8000 : 2000, width);
      t.evaluate();
      test_assert(t.moved.size() == t.nodes.size());
      visits[k] = t.inst->totalVisits();
      test_assert(t.g.def->setLiteral(t.nodes.back(), "b", PropValue::ofFloat(1)).ok());
      t.evaluate();
      test_assert(t.moved.size() == 1 && t.moved[0] == int(t.nodes.size()) - 1);
      visits[k] = t.inst->totalVisits() - visits[k];
    }
    test_assert(visits[0] == visits[1] && visits[0] > 0 && visits[0] < 10);
  }
}

/* Over its budget, a foreach costs what its rows do. */
void testForeach()
{
  test_assert(grows("a foreach over its budget", 500, [](int n) {
    World w;
    Loop l(w);
    test_assert(l.root.def->setParam(l.node, kMaxCachedParam, PropValue::ofInt(10)).ok());
    Elements t;
    Elements::Column c;
    c.name = "a";
    for (int i = 0; i < n; i++) {
      t.keys.append(i);
      c.values.append(GraphValue::ofFloat(i));
    }
    t.columns.append(std::move(c));
    l.set(t);
    test_assert(l.evaluate().ok());
    const uint64_t runs = l.inst.totalRuns();
    t.columns[0].values[0] = GraphValue::ofFloat(-1);
    l.set(t);
    const double time = secondsOf([&]() { test_assert(l.evaluate().ok()); });
    test_assert(l.inst.totalRuns() == runs + 1);
    return time;
  }));
}

} // namespace

int main()
{
  testLinks();
  testWide();
  testPins();
  testEvaluation();
  testForeach();
  return test_end();
}
