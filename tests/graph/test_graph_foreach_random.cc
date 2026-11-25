/*
 * Random edits of a foreach: its table, its def, the defs in that, and the blocks they read.
 *
 * The oracle has a GraphInstance per element, of the foreach's def, which gets the element's
 * values as its inputs. It must give the same values or the same first error. Since it is
 * kept as long as the foreach's own state for the element, the foreach must also run exactly
 * as many kernels as all of them together, unless elements share a state.
 */
#include "foreach_fixture.h"
#include "fuzz.h"

#include <cstdlib>
#include <map>
#include <memory>
#include <random>

using namespace fixture;

test_init;

namespace {

struct Totals {
  int edits = 0, pulls = 0, failures = 0, exact = 0, shared = 0;
  uint64_t runs = 0, elements = 0;
};

struct Random {
  std::mt19937_64 gen;
  int below(int n)
  {
    return int(gen() % uint64_t(n));
  }
  bool oneIn(int n)
  {
    return below(n) == 0;
  }
  double value()
  {
    static const double values[] = {-100, -1, 0, 1, 2, 3, 50};
    return oneIn(4) ? values[below(7)] : double(below(4));
  }
};

/* What a pull should give. */
struct Expected {
  bool ok = false;
  Elements table;
  StatusCode code = StatusCode::Ok;
  NodePath origin;
  std::vector<int64_t> elements;
  uint64_t runs = 0;
  bool exact = true;
  bool shared = false;
};

struct Fuzz {
  World w;
  Random &r;
  Totals &totals;
  Graph root, defs[3];
  NodeId node;
  Elements tab;
  litestl::util::Vector<BlockHandle> things;

  struct Oracle {
    std::unique_ptr<GraphInstance> inst;
    /* Where each input's value came from, by interface id. */
    std::map<int64_t, std::pair<int, uint64_t>> sources;
  };
  /* A long-lived instance and its oracle. */
  struct Side {
    GraphInstance inst;
    std::map<int64_t, Oracle> elements;
    /* Elements may lack a state that their oracle has: those that shared one, or that lost
     * theirs, don't run while their row is the same. Until all have one, runs aren't exact. */
    bool stateless = false;
    Side(Runtime *rt, Library *lib, BlockId id) : inst(rt, lib, id) {}
  };
  std::unique_ptr<Side> sides[2];

  Fuzz(Random &random, Totals &t) : r(random), totals(t), root(newGraph(w.lib, "root"))
  {
    for (Graph &g : defs) {
      g = newGraph(w.lib, "def");
      for (const char *name : {"a", "b", "c"}) {
        addSocket(*g.def, SocketSide::Input, name, r.oneIn(4) ? "int" : "float");
      }
      addDefaults(*g.def);
      for (const char *name : {"x", "y"}) {
        addSocket(*g.def, SocketSide::Output, name, "float");
      }
      add(*g.def, "graph.input");
      add(*g.def, "graph.output");
    }
    for (int i = 0; i < 3; i++) {
      things.append(newThing(w.lib, "t", i));
    }
    const int64_t in = addSocket(*root.def, SocketSide::Input, "table", "elements");
    const NodeId input = add(*root.def, "graph.input");
    node = add(*root.def, kGraphForeachType);
    test_assert(root.def->setGroup(node, defs[0].id).ok());
    connect(*root.def, input, key(in), node, kElementsSocket);
    for (auto &s : sides) {
      s = std::make_unique<Side>(&w.rt, &w.lib, root.id);
      s->inst.setIncremental(r.below(4) != 0);
    }
    /* Something to run from the start. */
    for (int i = 0; i < 40; i++) {
      editDef(r.below(3));
    }
    editTable();
  }

  /* ---- edits ---- */

  const GraphNode *anyNode(GraphDef &d)
  {
    return d.nodeCount() ? d.nodeAt(r.below(d.nodeCount())) : nullptr;
  }

  void addNode(int index)
  {
    static const char *types[] = {"t.sum",   "t.sum",  "t.sum",  "math.float", "t.read",
                                  "t.readIf", "t.range", "t.total", "graph.group", "graph.foreach"};
    GraphDef &d = *defs[index].def;
    if (d.nodeCount() > 14) {
      return;
    }
    const NodeId id = add(d, types[r.below(10)]);
    const GraphNode &n = *d.node(id);
    if (strncmp(n.typeName().c_str(), "graph.", 6) == 0) {
      d.setGroup(id, defs[r.below(3)].id).ignore();
      d.setParam(id, kMaxCachedParam, PropValue::ofInt(r.below(4))).ignore();
    }
    d.setParam(id, "thing", toThing(idOf(w.lib, things[r.below(3)]))).ignore();
    d.setParam(id, "column", PropValue::ofString(r.oneIn(2) ? "x" : "i")).ignore();
    if (strcmp(n.typeName().c_str(), "t.range") == 0) {
      d.setLiteral(id, "count", PropValue::ofFloat(r.below(4))).ignore();
    }
    for (int i = 0; i < 4; i++) {
      linkAny(d);
    }
  }

  void linkAny(GraphDef &d)
  {
    const GraphNode *a = anyNode(d), *b = anyNode(d);
    if (r.oneIn(3)) {
      d.forEachNode([&](const GraphNode &n) {
        b = strcmp(n.typeName().c_str(), "graph.output") == 0 ? &n : b;
      });
    }
    if (!a || !b) {
      return;
    }
    const SocketInfos outs = d.sockets(*a, SocketSide::Output), ins = d.sockets(*b, SocketSide::Input);
    if (outs.size() && ins.size()) {
      d.link({a->id(), outs[r.below(int(outs.size()))].key.c_str()},
             {b->id(), ins[r.below(int(ins.size()))].key.c_str()})
          .status()
          .ignore();
    }
  }

  void editDef(int index)
  {
    GraphDef &d = *defs[index].def;
    const GraphNode *n = anyNode(d);
    const SocketSide side = r.oneIn(2) ? SocketSide::Input : SocketSide::Output;
    const auto &iface = d.interfaceSockets(side);
    const int64_t socket = iface.size() ? iface[r.below(int(iface.size()))].id : 0;
    switch (r.below(17)) {
      case 0:
      case 1:
      case 2:
        return addNode(index);
      case 3:
      case 4:
      case 5:
        return linkAny(d);
      case 6:
        if (n && strcmp(n->typeName().c_str(), "graph.input") != 0) {
          d.removeNode(n->id()).ignore();
        }
        return;
      case 7:
        if (n && n->linksIn().size()) {
          d.unlink(n->linksIn()[0]->id).ignore();
        }
        return;
      case 8:
      case 9:
      case 10:
        if (n && d.sockets(*n, SocketSide::Input).size()) {
          const SocketInfos list = d.sockets(*n, SocketSide::Input);
          const std::string k = list[r.below(int(list.size()))].key.c_str();
          d.setLiteral(n->id(), k, r.oneIn(5) ? PropValue() : PropValue::ofFloat(r.value())).ignore();
        }
        return;
      case 11:
        if (n) {
          d.setParam(n->id(), "op", PropValue::ofEnum(r.below(3))).ignore();
          d.setParam(n->id(), "thing", toThing(idOf(w.lib, things[r.below(3)]))).ignore();
          d.setParam(n->id(), kMaxCachedParam, PropValue::ofInt(r.below(4))).ignore();
        }
        return;
      case 12:
        if (n && hasGroup(n->type()->role)) {
          d.setGroup(n->id(), r.oneIn(6) ? BlockId() : defs[r.below(3)].id).ignore();
        }
        return;
      case 13:
        d.moveInterface(socket, 0).ignore();
        return;
      case 14:
        if (r.oneIn(3) && iface.size() > 1) {
          d.removeInterface(socket).ignore();
        }
        else {
          const std::string name(1, char((side == SocketSide::Input ? 'a' : 'x') + r.below(3)));
          d.addInterface(side, name, "float", r.below(int(iface.size()) + 1)).status().ignore();
          addDefaults(d);
        }
        return;
      case 15:
        if (side == SocketSide::Input) {
          d.setInterfaceDefault(socket, PropValue::ofFloat(r.below(5))).ignore();
        }
        else {
          d.setInterfaceName(socket, std::string(1, char('x' + r.below(3)))).ignore();
        }
        return;
      default:
        d.setInterfaceType(socket, r.oneIn(2) ? "int" : "float").ignore();
        return addDefaults(d);
    }
  }

  /* Every interface input has a default. Without one it would have its type's, as unlinked
   * sockets inside have, and a link between the two could come and go without a run. */
  void addDefaults(GraphDef &d)
  {
    for (int i = 0; i < int(d.interfaceSockets(SocketSide::Input).size()); i++) {
      const InterfaceSocket &s = d.interfaceSockets(SocketSide::Input)[i];
      if (s.defaultValue.isNone()) {
        test_assert(d.setInterfaceDefault(s.id, PropValue::ofFloat(0)).ok());
      }
    }
  }

  void editTable()
  {
    Elements t;
    const int rows = r.below(6);
    for (int i = 0; i < rows; i++) {
      t.keys.append(r.oneIn(12) ? 1 : i * 3 % 7 + r.below(2) * 10);
    }
    static const char *names[] = {"a", "b", "c", "unused"};
    for (int c = 0; c < 4; c++) {
      if (r.oneIn(5)) {
        continue;
      }
      Elements::Column col;
      col.name = names[c];
      for (int i = 0; i < rows; i++) {
        const int kind = r.below(40);
        const GraphValue v = kind == 0 ? GraphValue::ofString("no") :
                             kind < 5  ? GraphValue::ofInt(r.below(3)) :
                                         GraphValue::ofFloat(r.value());
        col.values.append(v);
      }
      t.columns.append(std::move(col));
    }
    setTable(std::move(t));
  }

  /* One value, or the order of the rows. */
  void editRow()
  {
    Elements t = tab;
    if (r.oneIn(4) && t.size() > 1) {
      const int i = r.below(t.size() - 1);
      std::swap(t.keys[i], t.keys[i + 1]);
      for (Elements::Column &c : t.columns) {
        std::swap(c.values[i], c.values[i + 1]);
      }
    }
    else if (t.size() > 0 && t.columns.size() > 0) {
      t.columns[r.below(int(t.columns.size()))].values[r.below(t.size())] =
          GraphValue::ofFloat(r.value());
    }
    setTable(std::move(t));
  }

  void setTable(Elements t)
  {
    tab = std::move(t);
    for (auto &s : sides) {
      test_assert(s->inst.setInput("table", tableValue(tab)).ok());
    }
  }

  void editBlock()
  {
    const BlockHandle h = things[r.below(3)];
    const Datablock &b = *w.lib.get(h);
    if (r.oneIn(4)) {
      (b.state() == BlockState::Live ? w.lib.markDeleted(h) : w.lib.restoreFromHistory(h)).ignore();
      return;
    }
    w.thing(h).value = r.value();
    w.notify(b.id());
  }

  void edit()
  {
    totals.edits++;
    switch (r.below(12)) {
      case 0:
      case 1:
      case 2:
        return editDef(0);
      case 3:
        return editDef(1);
      case 4:
        return editDef(2);
      case 5:
        return editTable();
      case 6:
      case 7:
        return editRow();
      case 8:
        return editBlock();
      case 9:
        switch (r.below(8)) {
          case 0:
            return w.rt.ext<ChangeBus>().notifyAll();
          case 1:
            redirect(*defs[2].def, w.lib, idOf(w.lib, things[r.below(3)]));
            return;
          case 2: {
            const Graph &g = defs[r.below(3)];
            const bool live = w.lib.get(g.h)->state() == BlockState::Live;
            (live ? w.lib.markDeleted(g.h) : w.lib.restoreFromHistory(g.h)).ignore();
            return;
          }
          case 3:
            for (auto &s : sides) {
              test_assert(s->inst.retry().ok());
              for (auto &e : s->elements) {
                test_assert(e.second.inst->retry().ok());
              }
            }
            return;
          default:
            w.rt.touch();
            return;
        }
      case 10:
        root.def->setParam(node, kMaxCachedParam, PropValue::ofInt(r.oneIn(2) ? 100 : r.below(4)))
            .ignore();
        return;
      default: {
        const SocketInfos list = root.def->sockets(*root.def->node(node), SocketSide::Input);
        const std::string k = list[r.below(int(list.size()))].key.c_str();
        root.def->setLiteral(node, k, r.oneIn(4) ? PropValue() : PropValue::ofFloat(r.value()))
            .ignore();
        return;
      }
    }
  }

  /* ---- the oracle ---- */

  /* The row's values as the inputs of an instance of the def. */
  Status setInputs(Oracle &o, const GraphDef &def, int row)
  {
    const SocketTypeRegistry &types = w.rt.ext<SocketTypeRegistry>();
    for (const InterfaceSocket &s : def.interfaceSockets(SocketSide::Input)) {
      const SocketType &type = *types.find(s.typeName.c_str());
      const GraphNode &n = *root.def->node(node);
      const Elements::Column *c = tab.column(s.name.c_str());
      const NodeValue *literal = n.literal(s.key().c_str(), type);
      GraphValue v;
      std::pair<int, uint64_t> source;
      if (!c) {
        v = root.def->literalValue(n, s.key().c_str(), type);
        source = literal ? std::pair(1, literal->version) : std::pair(2, s.defaultVersion);
      }
      else if (type.holds(c->values[row])) {
        v = c->values[row];
      }
      else {
        CDCC_TRY(types.convert(*types.builtin(c->values[row].type()), type, c->values[row], v));
      }
      if (o.sources.count(s.id) && o.sources[s.id] != source) {
        test_assert(o.inst->setInput(s.name.c_str(), GraphValue()).ok()); /* a new version */
      }
      o.sources[s.id] = source;
      test_assert(o.inst->setInput(s.name.c_str(), v).ok());
    }
    return Status();
  }

  Expected expected(Side &s)
  {
    Expected e;
    e.exact = !s.stateless;
    auto fail = [&](StatusCode code, NodePath origin) {
      e.code = code;
      e.origin = std::move(origin);
      return e;
    };
    const GraphDef *def = root.def->groupTarget(*root.def->node(node));
    if (!def) {
      return fail(StatusCode::NotFound, at({}, node));
    }
    if (!tab.validate().ok()) {
      return fail(StatusCode::InvalidArgument, at({}, node));
    }
    const auto &outs = def->interfaceSockets(SocketSide::Output);
    e.table.keys = tab.keys;
    for (const InterfaceSocket &o : outs) {
      Elements::Column c;
      c.name = o.name;
      e.table.columns.append(std::move(c));
    }
    const int maxCached =
        int(root.def->paramValue(*root.def->node(node), kMaxCachedParam).asInt());
    bool failed = false;
    for (int row = 0; row < tab.size() && !failed; row++) {
      const int64_t key = tab.keys[row];
      Oracle temp;
      const bool shared = row >= maxCached;
      e.exact &= !shared;
      e.shared |= shared;
      Oracle &oracle = shared ? temp : s.elements[key];
      if (!oracle.inst) {
        oracle.inst = std::make_unique<GraphInstance>(&w.rt, &w.lib, defs[0].id);
      }
      GraphInstance &inst = *oracle.inst;
      totals.elements++;
      totals.shared += shared;
      const uint64_t before = inst.totalRuns();
      if (Status st = setInputs(oracle, *def, row); !st.ok()) {
        failed = true;
        e.elements = {key};
        fail(st.code(), at({}, node));
      }
      for (int o = 0; o < int(outs.size()) && !failed; o++) {
        const SocketPath p = SocketPath::output(outs[o].key().c_str());
        const Status st = inst.evaluate(p);
        if (st.ok()) {
          e.table.columns[o].values.append(*inst.value(p));
          continue;
        }
        failed = true;
        const GraphError &error = *inst.error(p);
        e.elements = {key};
        const auto &inner = error.origin.elements;
        e.elements.insert(e.elements.end(), inner.begin(), inner.end());
        NodePath origin = place(&error);
        origin.groups = {};
        origin.groups.append(node);
        for (NodeId id : error.origin.groups) {
          origin.groups.append(id);
        }
        fail(st.code(), origin);
      }
      e.runs += inst.totalRuns() - before;
    }
    e.ok = !failed;
    if (!failed || int(s.elements.size()) > 2 * maxCached) {
      std::map<int64_t, bool> wanted;
      for (int row = 0; row < tab.size() && row < maxCached; row++) {
        wanted[tab.keys[row]] = true;
      }
      std::erase_if(s.elements, [&](const auto &p) { return !wanted.count(p.first); });
      s.stateless |= failed;
    }
    s.stateless |= e.shared;
    return e;
  }

  bool pull(Side &s)
  {
    totals.pulls++;
    const Expected e = expected(s);
    const SocketPath p = SocketPath::of(node, kElementsSocket);
    const uint64_t before = s.inst.totalRuns();
    const Status st = s.inst.evaluate(p);
    const uint64_t runs = s.inst.totalRuns() - before;
    totals.runs += runs;
    const GraphError *error = s.inst.error(p);
    auto wrong = [&](const char *what) {
      printf("%s: got %s, expected %s, %d runs for %d\n", what, st.toString().c_str(),
             statusCodeName(e.code), int(runs), int(e.runs));
      return false;
    };
    if (st.ok() != e.ok || st.code() != e.code) {
      return wrong("the status");
    }
    if (st.ok() && !(*s.inst.value(p)->payload<Elements>() == e.table)) {
      return wrong("the table");
    }
    if (!st.ok()) {
      totals.failures++;
      const auto &elements = error->origin.elements;
      if (!(place(error) == e.origin) || elements.size() != e.elements.size() ||
          !std::equal(e.elements.begin(), e.elements.end(), elements.begin()))
      {
        return wrong("the error's origin");
      }
    }
    if (e.exact) {
      totals.exact++;
    }
    if (e.exact && runs != e.runs) {
      return wrong("the runs");
    }
    if (e.exact && s.inst.elementCount() < int(s.elements.size())) {
      return wrong("the elements kept");
    }
    const NodeReport report = s.inst.report(at({}, node));
    if (st.ok()) {
      if (report.elements != tab.size() || report.cachedElements > report.elements) {
        return wrong("the report");
      }
      s.stateless &= e.shared || report.cachedElements < report.elements;
    }
    /* Nothing moves: nothing runs, whatever it gave. */
    if (r.oneIn(3)) {
      w.rt.touch();
    }
    const uint64_t again = s.inst.totalRuns();
    s.inst.evaluate(p).ignore();
    return s.inst.totalRuns() == again ? true : wrong("runs after no edit");
  }

  bool run(int steps)
  {
    for (int i = 0; i < steps; i++) {
      for (int n = r.below(5); n >= 0; n--) {
        edit();
      }
      for (auto &s : sides) {
        if ((s == sides[0] || r.oneIn(3)) && !pull(*s)) {
          printf("at step %d\n", i);
          return false;
        }
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
  Totals totals;
  for (int seed = first; seed < first + count; seed++) {
    Random r;
    r.gen.seed(uint64_t(seed));
    Fuzz f(r, totals);
    const bool ok = f.run(150);
    if (!ok) {
      printf("seed %d\n", seed);
    }
    test_assert(ok);
    if (!ok) {
      break;
    }
  }
  printf("seeds %d to %d: %d edits, %d pulls (%d failed, %d with exact runs), %d elements (%d shared), "
         "%d runs\n",
         first, first + count - 1, totals.edits, totals.pulls, totals.failures, totals.exact,
         int(totals.elements), totals.shared, int(totals.runs));
  return test_end();
}
