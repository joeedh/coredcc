/*
 * Random edits of graphs with interior links, against a naive evaluator, which looks up
 * the source of every input in the expanded graph. GraphDef::link must refuse a link
 * exactly if it makes a cycle there.
 */
#include "fuzz.h"
#include "graph_fixture.h"

#include "coredcc/graph/instance.h"
#include "coredcc/graph/interior.h"

#include <algorithm>
#include <map>
#include <optional>
#include <random>
#include <vector>

using namespace fixture;

test_init;

namespace {

/* a + b, but fails when a is -100 or less. */
Status sum(NodeExecContext &ctx)
{
  const double a = ctx.input("a")->asFloat();
  if (a <= -100) {
    return Status::error(StatusCode::OutOfRange, "too small");
  }
  return ctx.setOutput("value", GraphValue::ofFloat(a + ctx.input("b")->asFloat()));
}

struct World {
  Runtime rt;
  Library lib;
  World() : lib(&rt)
  {
    test_assert(registerGraphBlocks(rt).ok());
    NodeTypeDef s("t.sum");
    s.input(PropertyDef::floatProp("a"))
        .input(PropertyDef::floatProp("b"))
        .output("value", "float");
    addType<sum>(rt, s, "t.sum");
  }
};

/* ---- the naive evaluator ---- */

using Path = std::vector<NodeId>;

struct Answer {
  GraphValue value;
  std::optional<StatusCode> error;
  Path origin;
  bool operator==(const Answer &b) const
  {
    return error == b.error && (error ? origin == b.origin : value == b.value);
  }
};

/* Vertices are input sockets and kernel nodes, as text. */
struct Naive {
  const GraphDef &root;
  std::map<std::string, Answer> memo;

  explicit Naive(const GraphDef &r) : root(r)
  {
  }

  const GraphDef *defAt(const Path &path) const
  {
    const GraphDef *d = &root;
    for (NodeId id : path) {
      const GraphNode *n = d ? d->node(id) : nullptr;
      d = n && n->type() && n->type()->role == NodeRole::Group ? d->groupTarget(*n)
                                                               : nullptr;
    }
    return d;
  }
  static std::string text(const Path &path, NodeId node, const std::string &key)
  {
    std::string s;
    for (NodeId id : path) {
      s += std::to_string(id.value) + "/";
    }
    return s + std::to_string(node.value) + "." + key;
  }
  static const GraphLink *working(const GraphDef &d, NodeId node, const std::string &key)
  {
    const GraphNode *n = d.node(node);
    const GraphLink *l = n ? n->linkInto(key) : nullptr;
    return l && d.findSocket(*l->from, SocketSide::Output, l->fromKey.c_str()) &&
                   d.findSocket(*n, SocketSide::Input, key)
               ? l
               : nullptr;
  }

  /* A place a value comes from: an input socket, a kernel's output, or a fixed answer. */
  struct Source {
    Path path;
    NodeId node;
    std::string key;
    bool input = false, kernel = false;
    Answer fixed;
  };

  Source output(const Path &path, NodeId node, const std::string &key) const
  {
    const GraphDef &d = *defAt(path);
    const GraphNode &n = *d.node(node);
    Source s;
    switch (n.type()->role) {
    case NodeRole::Kernel:
      s = Source{path, node, "", false, true, {}};
      return s;
    case NodeRole::Input: {
      if (!path.empty()) {
        Path up = path;
        up.pop_back();
        return Source{up, path.back(), key, true, false, {}};
      }
      for (const InterfaceSocket &i : d.interfaceSockets(SocketSide::Input)) {
        if (key == i.key().c_str()) {
          s.fixed.value = i.defaultValue.isNone()
                              ? typeOf(i.typeName.c_str()).defaultValue
                              : *GraphValue::fromProp(i.defaultValue);
        }
      }
      return s;
    }
    default:
      break;
    }
    const GraphDef *target = d.groupTarget(n);
    Path in = path;
    in.push_back(node);
    if (!target) {
      s.fixed.error = StatusCode::NotFound;
      s.fixed.origin = in;
      return s;
    }
    if (const std::optional<InteriorKey> k = InteriorKey::parse(key)) {
      for (NodeId id : k->groups) {
        in.push_back(id);
      }
      return output(in, k->node, k->key.c_str());
    }
    target->forEachNode([&](const GraphNode &o) {
      if (o.type()->role == NodeRole::Output) {
        s = Source{in, o.id(), key, true, false, {}};
      }
    });
    if (!s.input) {
      for (const InterfaceSocket &i : target->interfaceSockets(SocketSide::Output)) {
        if (key == i.key().c_str()) {
          s.fixed.value = typeOf(i.typeName.c_str()).defaultValue;
        }
      }
    }
    return s;
  }

  const SocketType &typeOf(const char *name) const
  {
    return *root.runtime().ext<SocketTypeRegistry>().find(name);
  }

  /* What an input gets, through links only. None when it isn't linked. */
  std::optional<Source> input(const Path &path, NodeId node, const std::string &key) const
  {
    /* Replaced by a link of a group around it? The outermost first. */
    for (size_t i = 0; i < path.size() && key[0] != '@'; i++) {
      const Path outer(path.begin(), path.begin() + long(i));
      std::string k = "@";
      for (size_t j = i + 1; j < path.size(); j++) {
        k += std::to_string(path[j].value) + "/";
      }
      k += std::to_string(node.value) + "." + key;
      if (const GraphLink *l = working(*defAt(outer), path[i], k)) {
        return output(outer, l->from->id(), l->fromKey.c_str());
      }
    }
    if (const GraphLink *l = working(*defAt(path), node, key)) {
      return output(path, l->from->id(), l->fromKey.c_str());
    }
    return std::nullopt;
  }

  std::vector<Source> sourcesOf(const Source &v) const
  {
    std::vector<Source> out;
    if (v.input) {
      if (std::optional<Source> s = input(v.path, v.node, v.key)) {
        out.push_back(*s);
      }
    } else if (v.kernel) {
      for (const char *k : {"a", "b"}) {
        out.push_back(Source{v.path, v.node, k, true, false, {}});
      }
    }
    return out;
  }
  static std::string name(const Source &s)
  {
    return text(s.path, s.node, s.key) + (s.kernel ? "!" : "");
  }
  /* Whether v depends on itself. */
  bool onCycle(const Source &v) const
  {
    std::map<std::string, bool> seen;
    std::vector<Source> stack = sourcesOf(v);
    while (!stack.empty()) {
      const Source s = stack.back();
      stack.pop_back();
      if ((!s.input && !s.kernel) || seen.count(name(s))) {
        continue;
      }
      if (name(s) == name(v)) {
        return true;
      }
      seen[name(s)] = true;
      for (const Source &next : sourcesOf(s)) {
        stack.push_back(next);
      }
    }
    return false;
  }

  Answer eval(const Source &v)
  {
    if (!v.input && !v.kernel) {
      return v.fixed;
    }
    const std::string id = name(v);
    if (const auto it = memo.find(id); it != memo.end()) {
      return it->second;
    }
    Answer a;
    if (onCycle(v)) {
      a.error = StatusCode::FailedPrecondition;
    } else if (v.kernel) {
      const Answer x = eval(Source{v.path, v.node, "a", true, false, {}});
      const Answer y = eval(Source{v.path, v.node, "b", true, false, {}});
      if (x.error || y.error) {
        a = x.error ? x : y;
      } else if (x.value.asFloat() <= -100) {
        a.error = StatusCode::OutOfRange;
        a.origin = v.path;
        a.origin.push_back(v.node);
      } else {
        a.value = GraphValue::ofFloat(x.value.asFloat() + y.value.asFloat());
      }
    } else {
      const GraphDef &d = *defAt(v.path);
      const GraphNode &n = *d.node(v.node);
      const SocketType &type =
          typeOf(d.findSocket(n, SocketSide::Input, v.key)->typeName.c_str());
      const std::optional<Source> s = input(v.path, v.node, v.key);
      if (s) {
        a = eval(*s);
      } else {
        a.value = d.literalValue(n, v.key, type);
      }
      if (!a.error && !type.holds(a.value)) {
        GraphValue out;
        test_assert(
            root.runtime()
                .ext<SocketTypeRegistry>()
                .convert(*root.runtime().ext<SocketTypeRegistry>().typeOf(a.value),
                         type,
                         a.value,
                         out)
                .ok());
        a.value = out;
      }
    }
    memo[id] = a;
    return a;
  }

  /* Calls fn(path, node) for every node of the expanded graph. */
  template <class Fn> void forEach(const GraphDef &d, Path &path, Fn &&fn) const
  {
    d.forEachNode([&](const GraphNode &n) {
      fn(path, n);
      if (const GraphDef *t =
              n.type()->role == NodeRole::Group ? d.groupTarget(n) : nullptr)
      {
        path.push_back(n.id());
        forEach(*t, path, fn);
        path.pop_back();
      }
    });
  }
  bool hasCycle() const
  {
    bool found = false;
    Path path;
    forEach(root, path, [&](const Path &p, const GraphNode &n) {
      for (const SocketInfo &s : defAt(p)->sockets(n, SocketSide::Input)) {
        found = found || onCycle(Source{p, n.id(), s.key.c_str(), true, false, {}});
      }
      for (const GraphLink *l : n.linksIn()) {
        found = found || (l->toKey.c_str()[0] == '@' &&
                          working(*defAt(p), n.id(), l->toKey.c_str()) &&
                          onCycle(Source{p, n.id(), l->toKey.c_str(), true, false, {}}));
      }
    });
    return found;
  }
};

/* ---- edits ---- */

struct Totals {
  int edits = 0, links = 0, interior = 0, refused = 0, pulls = 0, errors = 0, cycles = 0;
  int batches = 0, batchesRefused = 0, fullPasses = 0;
};

struct Fuzz {
  static constexpr int kDefs = 4;
  World w;
  Totals &totals;
  std::mt19937_64 gen;
  Graph defs[kDefs];
  std::unique_ptr<GraphInstance> inst;
  int64_t forcedLinks = 0; /* for their ids */

  Fuzz(uint64_t seed, Totals &t) : totals(t)
  {
    gen.seed(seed);
    for (Graph &g : defs) {
      g = makeGraph(w.lib, "d");
      for (int i = 0; i < 2; i++) {
        addSocket(*g.def, SocketSide::Input, i ? "i" : "j", below(4) ? "float" : "int");
        addSocket(*g.def, SocketSide::Output, i ? "x" : "y", below(4) ? "float" : "int");
      }
      add(*g.def, "graph.input");
      add(*g.def, "graph.output");
      add(*g.def, "t.sum");
    }
    inst = std::make_unique<GraphInstance>(&w.rt, &w.lib, defs[0].id);
    inst->setIncremental(seed % 4 != 0);
    for (int i = 0; i < 60; i++) {
      edit();
    }
  }
  int below(int n)
  {
    return int(gen() % uint64_t(n));
  }

  /* A key of one of the node's sockets, or an interior one. */
  std::string socketOf(const GraphDef &d, const GraphNode &n, SocketSide side)
  {
    const GraphDef *t = n.type()->role == NodeRole::Group ? d.groupTarget(n) : nullptr;
    if (t && below(3)) {
      std::string path = "@";
      const GraphDef *in = t;
      while (true) {
        const GraphNode &m = *in->nodeAt(below(in->nodeCount()));
        const GraphDef *next =
            m.type()->role == NodeRole::Group ? in->groupTarget(m) : nullptr;
        path += std::to_string(m.id().value);
        if (next && below(2)) {
          path += "/";
          in = next;
          continue;
        }
        const SocketInfos list = in->sockets(m, side);
        return list.size() ? path + "." + list[below(int(list.size()))].key.c_str() : "";
      }
    }
    const SocketInfos list = d.sockets(n, side);
    return list.size() ? list[below(int(list.size()))].key.c_str() : "";
  }

  void link(int index)
  {
    GraphDef &d = *defs[index].def;
    const GraphNode &a = *d.nodeAt(below(d.nodeCount())),
                    &b = *d.nodeAt(below(d.nodeCount()));
    const std::string from = socketOf(d, a, SocketSide::Output),
                      to = socketOf(d, b, SocketSide::Input);
    if (from.empty() || to.empty()) {
      return;
    }
    const bool before = Naive(d).hasCycle();
    const GraphLink *old = b.linkInto(to);
    const LinkState was = old ? GraphDef::stateOf(*old) : LinkState();
    const Result<LinkId> l = d.link({a.id(), from}, {b.id(), to});
    totals.links++;
    totals.interior += from[0] == '@' || to[0] == '@';
    if (before) {
      return; /* nothing can be said */
    }
    if (l.ok()) {
      test_assert(!Naive(d).hasCycle());
      return;
    }
    totals.refused++;
    test_assert(hasCode(l, StatusCode::InvalidArgument));
    LinkState forced;
    forced.id = LinkId{1 + forcedLinks++};
    forced.from = a.id();
    forced.fromKey = from.c_str();
    forced.to = b.id();
    forced.toKey = to.c_str();
    if (old) {
      test_assert(d.unlink(was.id).ok());
    }
    test_assert(d.insertLink(forced).ok() && Naive(d).hasCycle());
    if (below(8)) { /* else it stays, for the evaluator to find */
      test_assert(d.unlink(forced.id).ok());
      if (old) {
        test_assert(d.insertLink(was).ok());
      }
    }
  }

  static std::vector<LinkState> linksOf(const GraphDef &d)
  {
    std::vector<LinkState> out;
    d.forEachLink([&](const GraphLink &l) { out.push_back(GraphDef::stateOf(l)); });
    std::sort(out.begin(), out.end(), [](const LinkState &a, const LinkState &b) {
      return a.id < b.id;
    });
    return out;
  }
  static bool sameLinks(const std::vector<LinkState> &a, const std::vector<LinkState> &b)
  {
    return std::equal(a.begin(),
                      a.end(),
                      b.begin(),
                      b.end(),
                      [](const LinkState &x, const LinkState &y) {
                        return x.id == y.id && x.from == y.from && x.to == y.to &&
                               strcmp(x.fromKey.c_str(), y.fromKey.c_str()) == 0 &&
                               strcmp(x.toKey.c_str(), y.toKey.c_str()) == 0;
                      });
  }

  /* Several at once: all, or none if one of them is on a cycle. */
  void linkMany(int index)
  {
    GraphDef &d = *defs[index].def;
    struct Spec {
      NodeId a, b;
      std::string from, to;
    };
    std::vector<Spec> specs;
    bool twice = false;
    for (int i = 0, n = 1 + below(5); i < n; i++) {
      const GraphNode &a = *d.nodeAt(below(d.nodeCount())),
                      &b = *d.nodeAt(below(d.nodeCount()));
      Spec s{a.id(),
             b.id(),
             socketOf(d, a, SocketSide::Output),
             socketOf(d, b, SocketSide::Input)};
      if (s.from.empty() || s.to.empty()) {
        continue;
      }
      bool taken = false;
      for (const Spec &o : specs) {
        taken |= o.b == s.b && o.to == s.to;
      }
      if (taken && below(4)) {
        continue;
      }
      twice |= taken;
      specs.push_back(s);
    }
    std::vector<LinkSpec> list;
    for (const Spec &s : specs) {
      list.push_back(LinkSpec{{s.a, s.from}, {s.b, s.to}});
    }
    const bool before = Naive(d).hasCycle();
    const std::vector<LinkState> was = linksOf(d);
    litestl::util::Vector<LinkId> ids;
    const Status status = d.linkAll(list, ids);
    totals.batches++;
    if (status.ok()) {
      test_assert(!twice && ids.size() == list.size() &&
                  (before || !Naive(d).hasCycle()));
      for (size_t i = 0; i < specs.size(); i++) {
        const GraphLink *l = d.link(ids[int(i)]);
        test_assert(l && l->from->id() == specs[i].a &&
                    l->fromKey.c_str() == specs[i].from &&
                    d.node(specs[i].b)->linkInto(specs[i].to) == l);
      }
      return;
    }
    totals.batchesRefused++;
    test_assert(hasCode(status, StatusCode::InvalidArgument) && ids.size() == 0);
    test_assert(sameLinks(linksOf(d), was));
    if (before || twice) {
      return;
    }
    std::vector<LinkState> olds;
    std::vector<LinkId> forced;
    for (const Spec &s : specs) {
      if (const GraphLink *old = d.node(s.b)->linkInto(s.to)) {
        olds.push_back(GraphDef::stateOf(*old));
        test_assert(d.unlink(old->id).ok());
      }
      LinkState l;
      l.id = LinkId{1 + forcedLinks++};
      l.from = s.a;
      l.fromKey = s.from.c_str();
      l.to = s.b;
      l.toKey = s.to.c_str();
      test_assert(d.insertLink(l).ok());
      forced.push_back(l.id);
    }
    test_assert(Naive(d).hasCycle());
    for (LinkId id : forced) {
      test_assert(d.unlink(id).ok());
    }
    for (const LinkState &l : olds) {
      test_assert(d.insertLink(l).ok());
    }
  }

  void edit()
  {
    totals.edits++;
    const int index = below(3) ? 0 : below(kDefs);
    GraphDef &d = *defs[index].def;
    const GraphNode &n = *d.nodeAt(below(d.nodeCount()));
    const SocketInfos ins = d.sockets(n, SocketSide::Input);
    switch (below(14)) {
    case 0:
      if (d.nodeCount() < 9) {
        add(d, "t.sum");
      }
      return;
    case 1:
    case 2:
      if (d.nodeCount() < 9 && index + 1 < kDefs) {
        const NodeId id = add(d, "graph.group");
        test_assert(d.setGroup(id, defs[index + 1 + below(kDefs - index - 1)].id).ok());
      }
      return;
    case 3:
      if (below(3) == 0 && d.nodeCount() > 2) {
        test_assert(d.removeNode(n.id()).ok());
      }
      return;
    case 4:
      if (n.linksIn().size()) {
        test_assert(d.unlink(n.linksIn()[below(int(n.linksIn().size()))]->id).ok());
      }
      return;
    case 5:
    case 6:
      if (ins.size()) {
        const std::string k = ins[below(int(ins.size()))].key.c_str();
        d.setLiteral(n.id(), k, PropValue::ofFloat(below(25) ? below(5) : -100)).ignore();
      }
      return;
    case 7: {
      const SocketSide side = below(2) ? SocketSide::Input : SocketSide::Output;
      const auto &list = d.interfaceSockets(side);
      if (list.size() < 3 && below(2)) {
        d.addInterface(side, std::to_string(gen()), "float").status().ignore();
      } else if (list.size()) {
        const int64_t id = list[below(int(list.size()))].id;
        (below(2) ? d.setInterfaceType(id, below(2) ? "int" : "float")
                  : d.moveInterface(id, 0))
            .ignore();
        if (below(6) == 0) {
          test_assert(d.removeInterface(id).ok());
        }
      }
      return;
    }
    case 8:
      if (n.type()->role == NodeRole::Group && index + 1 < kDefs) {
        const int target = index + 1 + below(kDefs - index - 1);
        test_assert(d.setGroup(n.id(), below(5) ? defs[target].id : BlockId()).ok());
      }
      return;
    case 9:
      if (below(4) == 0) {
        const Graph &g = defs[1 + below(kDefs - 1)];
        const bool live = w.lib.get(g.h)->state() == BlockState::Live;
        (live ? w.lib.markDeleted(g.h) : w.lib.restoreFromHistory(g.h)).ignore();
      }
      return;
    case 10:
      return linkMany(index);
    default:
      return link(index);
    }
  }

  bool check()
  {
    Naive naive(*defs[0].def);
    Path path;
    bool good = true;
    auto compare = [&](const SocketPath &p, const Answer &e) {
      totals.pulls++;
      const Status st = inst->evaluate(p);
      const GraphError *error = inst->error(p);
      bool ok = e.error ? st.code() == *e.error : st.ok() && *inst->value(p) == e.value;
      totals.errors += e.error.has_value();
      if (ok && e.error == StatusCode::FailedPrecondition) {
        totals.cycles++;
      } else if (ok && e.error) {
        ok = error->origin.node == e.origin.back() &&
             error->origin.groups.size() == e.origin.size() - 1 &&
             std::equal(
                 e.origin.begin(), e.origin.end() - 1, error->origin.groups.begin());
      }
      if (!ok && good) {
        printf("%s.%s: %s, expected %s %g\n",
               p.node.toString().c_str(),
               p.key.c_str(),
               st.toString().c_str(),
               e.error ? statusCodeName(*e.error) : "Ok",
               !e.error && e.value.type() == GraphValue::Type::Float ? e.value.asFloat()
                                                                     : 0);
      }
      good &= ok;
    };
    naive.forEach(*defs[0].def, path, [&](const Path &p, const GraphNode &n) {
      if (!good || below(2)) {
        return;
      }
      NodePath np;
      for (NodeId id : p) {
        np.groups.append(id);
      }
      np.node = n.id();
      const GraphDef &d = *naive.defAt(p);
      for (const SocketInfo &s : d.sockets(n, SocketSide::Output)) {
        compare(SocketPath::of(np, s.key.c_str()),
                naive.eval(naive.output(p, n.id(), s.key.c_str())));
      }
      for (const SocketInfo &s : d.sockets(n, SocketSide::Input)) {
        compare(SocketPath::of(np, s.key.c_str(), SocketSide::Input),
                naive.eval(Naive::Source{p, n.id(), s.key.c_str(), true, false, {}}));
      }
      for (const GraphLink *l : n.linksOut()) { /* as sockets of the group node */
        const std::string k = l->fromKey.c_str();
        if (k[0] == '@' && d.findSocket(n, SocketSide::Output, k)) {
          compare(SocketPath::of(np, k), naive.eval(naive.output(p, n.id(), k)));
        }
      }
    });
    /* What was pulled before the last edits stays right: nothing runs twice. */
    if (below(3) == 0) {
      w.rt.touch();
    }
    return good;
  }

  bool run(int steps)
  {
    for (int i = 0; i < steps; i++) {
      for (int n = below(4); n >= 0; n--) {
        edit();
      }
      if (!check()) {
        printf("at step %d\n", i);
        return false;
      }
      for (const Graph &g : defs) {
        test_assert(g.def->validate().ok());
      }
    }
    return true;
  }
};

} // namespace

int main(int argc, char **argv)
{
  const int scale = coredcc_test::testScale();
  const int first = argc > 1 ? atoi(argv[1]) : 1;
  const int count = argc > 2 ? atoi(argv[2]) : 20 * scale;
  Totals t;
  for (int seed = first; seed < first + count && !retval; seed++) {
    Fuzz f(uint64_t(seed), t);
    const bool ok = f.run(40);
    t.fullPasses += int(f.inst->totalFullPasses());
    if (!ok || retval) {
      printf("seed %d\n", seed);
    }
    test_assert(ok);
  }
  printf("seeds %d to %d: %d edits, %d links (%d interior, %d refused), %d batches (%d "
         "refused), "
         "%d pulls, %d errors, %d of cycles, %d full passes\n",
         first,
         first + count - 1,
         t.edits,
         t.links,
         t.interior,
         t.refused,
         t.batches,
         t.batchesRefused,
         t.pulls,
         t.errors,
         t.cycles,
         t.fullPasses);
  return test_end();
}
