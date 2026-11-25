/* Random edits over several graphs that use each other, with long-lived instances checked
 * against a from-scratch evaluation (naive_eval.h) after each batch: the values, the errors
 * with their origins, and exactly which kernels ran. See the plan's "Tests". */
#include "random_edits.h"

#include "coredcc/graph/topology.h"

using namespace random_graph;

test_init;

namespace {

bool check(bool ok, const std::string &what)
{
  if (!ok) {
    printf("seed %llu, batch %d: %s\n", (unsigned long long)g_seed, g_batch, what.c_str());
  }
  return ok;
}

std::string text(const GroupIds &p, int64_t node)
{
  std::string s;
  for (int64_t id : p) {
    s += std::to_string(id) + "/";
  }
  return s + std::to_string(node);
}
std::string text(const naive::Error &e)
{
  return std::string(statusCodeName(e.code)) + " at " + text(e.groups, e.node) + "." + e.socket;
}
std::string text(const Value &v)
{
  std::string s = v.type + "(" + v.text;
  for (double d : v.numbers) {
    s += " " + std::to_string(d);
  }
  return s + ")";
}

naive::Error toError(const GraphError &e)
{
  naive::Error out;
  out.code = e.status.code();
  for (NodeId id : e.origin.groups) {
    out.groups.push_back(id.value);
  }
  out.node = e.origin.node.value;
  out.socket = e.socket.c_str();
  return out;
}

std::string pathKey(const SocketPath &p)
{
  return p.node.toString() + (p.side == SocketSide::Input ? "<" : ">") + p.key.c_str();
}

/* Forgets what the instance drops when it enters the frames of `e`. */
void enterFrames(Client &c, const naive::Evaluation &e)
{
  for (const naive::Frame &f : e.frames()) {
    auto known = c.frameDefs.find(f.path);
    if (known != c.frameDefs.end() && known->second != f.def->serial()) {
      c.dropBelow(f.path);
    }
    c.frameDefs[f.path] = f.def->serial();
    for (auto it = c.nodes.begin(); it != c.nodes.end();) {
      const GroupIds &p = it->first.first;
      const GraphNode *n = f.def->node(NodeId{it->first.second});
      const bool gone =
          p == f.path && (!n || it->second.type != n->typeName().c_str() ||
                          (it->second.type == "r.late" && n->type() &&
                           it->second.outputs.empty() && it->second.visited));
      it = gone ? c.nodes.erase(it) : std::next(it);
    }
    std::vector<GroupIds> lost;
    for (const auto &[p, serial] : c.frameDefs) {
      if (p.size() == f.path.size() + 1 && std::equal(f.path.begin(), f.path.end(), p.begin())) {
        const GraphNode *g = f.def->node(NodeId{p.back()});
        if (!g || strcmp(g->typeName().c_str(), "graph.group") != 0) {
          lost.push_back(p);
        }
      }
    }
    for (const GroupIds &p : lost) {
      c.dropBelow(p);
    }
  }
}

/* Compares the kernels that ran with what had to and what was allowed to. */
void checkRuns(Client &c, const naive::Evaluation &e, uint64_t before)
{
  uint64_t total = 0;
  for (const naive::KernelVisit &k : e.kernels()) {
    const std::pair<GroupIds, int64_t> key{k.path, k.node};
    Client::Node &n = c.nodes[key];
    n.type = k.type;
    NodePath path;
    for (int64_t id : k.path) {
      path.groups.append(NodeId{id});
    }
    path.node = NodeId{k.node};
    const NodeReport report = c.inst->report(path);
    const std::string where = text(k.path, k.node) + " (" + k.type + ")";
    test_assert(check(report.runs >= n.runs, where + " lost its state"));
    const uint64_t ran = report.runs - n.runs;
    n.runs = report.runs;
    total += ran;
    g_counts.visits++;
    g_counts.cycles += k.cycle;
    g_counts.errors += k.failed;

    const bool blocked = k.cycle || k.unregistered || k.inputFailed;
    if (blocked) {
      test_assert(check(ran == 0, where + " ran, though it couldn't"));
      test_assert(check(report.state == (k.inputFailed ? NodeReport::State::InputFailed :
                                                         NodeReport::State::Failed),
                        where + " has the wrong state"));
    }
    else {
      Client::Run now;
      now.type = k.type;
      now.params = k.params;
      now.paramVersion = k.def->node(NodeId{k.node})->paramVersion();
      now.block = k.block;
      now.failed = k.failed;
      for (const naive::Outcome &in : k.inputs) {
        now.inputs.push_back(in.value);
        std::string s = in.source.text;
        if (in.source.kernel >= 0) {
          const naive::KernelVisit &from = e.kernels()[size_t(in.source.kernel)];
          s += " v" + std::to_string(
                          c.nodes[{from.path, from.node}].versions[size_t(in.source.output)]);
        }
        now.sources.push_back(s + " as " + in.source.chain);
      }
      const bool must = n.hasRun && (now.inputs != n.run.inputs || now.params != n.run.params ||
                                     !now.block.seesSame(n.run.block));
      const bool may = !n.hasRun || must || now.paramVersion != n.run.paramVersion ||
                       now.sources != n.run.sources || !(now.block == n.run.block);
      g_counts.musts += must;
      g_counts.mays += may;
      g_counts.runs += ran;
      test_assert(check(ran <= 1, where + " ran twice"));
      test_assert(check(!must || ran == 1, where + " had to run"));
      test_assert(check(may || ran == 0, where + " ran for nothing"));
      test_assert(check(!may || ran == 1, where + " didn't run, though what it read moved"));
      test_assert(check(report.state ==
                            (k.failed ? NodeReport::State::Failed : NodeReport::State::Ok),
                        where + " has the wrong state"));
      if (ran) {
        n.hasRun = true;
        n.run = now;
      }
    }

    /* The versions of its outputs, by the plan's rule for changedAt. */
    const bool flip = !n.visited || n.failed != k.failed || (k.failed && !(n.error == k.error));
    const NodeType *type = k.def->node(NodeId{k.node})->type();
    n.versions.resize(type ? size_t(type->outputs.size()) : 0, 0);
    for (size_t i = 0; i < n.versions.size(); i++) {
      const bool newValue = !k.failed && (n.outputs.empty() || n.outputs[i] != k.outputs[i]);
      const bool newObject = !k.failed && ran && k.outputs[i].type.substr(0, 2) == "r.";
      if (flip || newValue || newObject) {
        n.versions[i] = ++c.versions; /* unique: a node may go and come back */
      }
    }
    if (!k.failed) {
      n.outputs = k.outputs;
    }
    n.visited = true;
    n.failed = k.failed;
    n.error = k.error;
  }
  test_assert(check(total == c.inst->totalRuns() - before, "kernels ran that weren't pulled"));
}

/* Pulls sockets of one instance, and checks everything about the answer. */
void pull(World &w, Client &c, const std::vector<SocketPath> &paths)
{
  g_counts.pulls++;
  litestl::util::Vector<Status> results;
  const uint64_t before = c.inst->totalRuns();
  const Status status = c.inst->evaluate({paths.data(), paths.size()}, results);
  const Graph &g = w.defs[size_t(c.def)];
  if (!w.live(g)) {
    test_assert(check(status.code() == StatusCode::NotFound, "a deleted root was evaluated"));
    return;
  }
  test_assert(check(status.ok(), "evaluate failed: " + std::string(status.toString().c_str())));

  std::vector<naive::Evaluation::Request> requests;
  for (const SocketPath &p : paths) {
    requests.push_back({p, false, {}});
  }
  naive::Evaluation e(w.env, g.def, c.external);
  e.run(requests);
  enterFrames(c, e);
  /* What is gone is forgotten, whatever was asked for. */
  for (auto it = c.last.begin(); it != c.last.end();) {
    const bool there = naive::Evaluation(w.env, g.def, c.external).finds(it->second.path);
    const bool known = c.inst->value(it->second.path) || c.inst->error(it->second.path);
    test_assert(check(there || !known, it->first + " is gone, but has a result"));
    test_assert(check(!there || known, it->first + " is there, but has no result"));
    it = there ? std::next(it) : c.last.erase(it);
  }
  checkRuns(c, e, before);
  g_counts.groups += e.frames().size() - 1;

  for (size_t i = 0; i < paths.size(); i++) {
    const SocketPath &p = paths[i];
    const naive::Evaluation::Request &r = requests[i];
    const std::string where = pathKey(p);
    if (!r.found) {
      test_assert(check(results[int(i)].code() == StatusCode::NotFound,
                        where + " was found: " + results[int(i)].toString().c_str()));
      continue;
    }
    const GraphError *error = c.inst->error(p);
    const GraphValue *value = c.inst->value(p);
    test_assert(check(c.inst->isCurrent(p), where + " isn't current"));
    if (r.outcome.failed) {
      const bool same = error && !value && toError(*error) == r.outcome.error &&
                        results[int(i)].code() == r.outcome.error.code;
      test_assert(check(same, where + " should fail with " + text(r.outcome.error) + ", not " +
                                  (error ? text(toError(*error)) : "succeed")));
      g_counts.recursions += error && strcmp(error->status.message(),
                                             "the graph contains itself") == 0;
    }
    else {
      const bool same = !error && value && sameValue(*value, r.outcome.value);
      test_assert(check(same, where + " should be " + text(r.outcome.value) + ", not " +
                                  (value ? text(toValue(*value)) :
                                           error ? text(toError(*error)) : "nothing")));
    }
    /* changedAt and lastGood. */
    Client::Last &l = c.last[where];
    l.path = p;
    const uint64_t at = c.inst->changedAt(p);
    const std::string message = error ? error->status.message() : "";
    const bool payload = !r.outcome.failed && r.outcome.value.type.substr(0, 2) == "r.";
    const bool changed =
        l.changedAt == 0 || l.failed != r.outcome.failed ||
        (r.outcome.failed ? !(l.error == r.outcome.error) || l.message != message :
                            !l.hasValue || !(l.good == r.outcome.value));
    test_assert(check(!changed || at > l.changedAt, where + ": changedAt stood still"));
    test_assert(check(changed || payload || at == l.changedAt,
                      where + ": changedAt moved for nothing"));
    if (!r.outcome.failed) {
      l.good = r.outcome.value;
      l.hasValue = true;
    }
    const GraphValue *good = c.inst->lastGood(p);
    test_assert(check(l.hasValue ? good && sameValue(*good, l.good) : !good,
                      where + ": the wrong lastGood"));
    l.failed = r.outcome.failed;
    l.error = r.outcome.error;
    l.message = message;
    l.changedAt = at;
  }

  /* At once again: nothing runs, whichever way it is asked. */
  const uint64_t runs = c.inst->totalRuns();
  for (size_t i = 0; i < paths.size(); i++) {
    const Status again = c.inst->evaluate(paths[i]);
    test_assert(check(again.code() == results[int(i)].code(), "another answer in one pass"));
  }
  test_assert(check(c.inst->totalRuns() == runs, "an unchanged pull ran kernels"));
}

std::string storedText(const NodeValues &values)
{
  std::set<std::string> out;
  for (const NodeValue &v : values) {
    out.insert(std::string(v.name.c_str()) + "=" + v.value.toString().c_str() +
               (v.off ? " off" : ""));
  }
  std::string text;
  for (const std::string &s : out) {
    text += " " + s;
  }
  return text;
}

/* What undo must bring back: ids, node order, links, what is stored, and the values in
 * effect. */
std::string describe(World &w)
{
  std::string out;
  for (const Graph &g : w.defs) {
    std::map<int64_t, std::string> parts;
    const GraphDef &def = *g.def;
    for (int i = 0; i < def.nodeCount(); i++) {
      out += std::to_string(def.nodeAt(i)->id().value) + " ";
    }
    def.forEachNode([&](const GraphNode &n) {
      const NodeState s = def.stateOf(n);
      std::string t = s.typeName.c_str() + (" g" + std::to_string(s.group.local.lo));
      t += storedText(n.params()) + ";" + storedText(n.literals());
      NodeValues extras;
      for (const BlockEntry &e : n.extras()) {
        extras.append(NodeValue{e.name, e.value});
      }
      t += ";" + storedText(extras);
      const NodeType *type = n.type();
      for (int i = 0; type && i < int(type->params.size()); i++) {
        const char *name = type->params[i].def.apiName().c_str();
        t += std::string(" ") + def.paramValue(n, name).toString().c_str() + "/" +
             std::to_string(n.blockTarget(name).local.lo);
      }
      /* Those last seen of a group that isn't there, which undo doesn't bring back. */
      const bool lost = type && type->role == NodeRole::Group && !def.groupTarget(n);
      for (const SocketInfo &in : def.sockets(n, SocketSide::Input)) {
        if (lost) {
          break;
        }
        const SocketType *st = w.rt.ext<SocketTypeRegistry>().find(in.typeName.c_str());
        if (st && st->hasLiteral()) {
          const Value v = toValue(def.literalValue(n, in.key.c_str(), *st));
          t += " " + std::string(in.key.c_str()) + text(v);
          if (type && type->role != NodeRole::Kernel) {
            t += def.usesLiteral(n, in.key.c_str()) ? "!" : "";
          }
        }
      }
      parts[s.id.value] = t;
    });
    def.forEachLink([&](const GraphLink &l) {
      parts[l.id.value] = std::to_string(l.from->id().value) + "." + l.fromKey.c_str() + ">" +
                          std::to_string(l.to->id().value) + "." + l.toKey.c_str();
    });
    out += w.live(g) ? "def\n" : "dead\n";
    for (const auto &[id, part] : parts) {
      out += std::to_string(id) + ": " + part + "\n";
    }
    for (SocketSide side : {SocketSide::Input, SocketSide::Output}) {
      for (const InterfaceSocket &s : def.interfaceSockets(side)) {
        out += std::string(s.name.c_str()) + " " + s.typeName.c_str() + " " +
               s.defaultValue.toString().c_str() + ";";
      }
      out += "|\n";
    }
  }
  return out;
}

void runWorld(uint64_t seed, int batches)
{
  g_seed = seed;
  const bool undoable = seed % 2 == 0;
  World w(seed, undoable);
  SplitMix64 rng(seed);
  std::vector<Client> clients;
  for (int def : {0, 1, 0}) {
    Client c;
    c.def = def;
    c.inst = std::make_unique<GraphInstance>(&w.rt, &w.lib, w.defs[size_t(def)].id);
    clients.push_back(std::move(c));
  }
  Editor editor(w, rng, clients);
  /* Something to start from. */
  for (int i = 0; i < 200; i++) {
    editor.edit();
  }
  editor.reviveDefs(); /* the clear would destroy a deleted one */
  w.history.clear();
  w.history.setMemoryBudget(uint64_t(1) << 40);
  const std::string first = describe(w);
  for (g_batch = 0; g_batch < batches && !retval; g_batch++) {
    const int edits = int(rng.below(6));
    for (int i = 0; i < edits; i++) {
      editor.edit();
    }
    for (Client &c : clients) {
      if (rng.below(3) == 0) {
        continue; /* it falls behind */
      }
      std::vector<SocketPath> paths;
      const int n = 1 + int(rng.below(4));
      for (int i = 0; i < n; i++) {
        paths.push_back(editor.randomSocket(c));
      }
      pull(w, c, paths);
      if (retval) {
        return;
      }
    }
    if (g_batch % 64 == 0) {
      for (const Graph &g : w.defs) {
        test_assert(check(g.def->validate().ok(), "a def is invalid"));
      }
      test_assert(w.rt.validateAll().ok() && w.lib.validate().ok());
    }
  }
  if (!undoable || retval) {
    return;
  }
  /* All the way back and forth. A pull in between, or instances that fall behind. */
  const std::string last = describe(w);
  int steps = 0;
  auto same = [&](const std::string &want, const char *what) {
    const std::string now = describe(w);
    if (!check(now == want, what)) {
      const size_t at = size_t(
          std::mismatch(want.begin(), want.end(), now.begin(), now.end()).first - want.begin());
      const size_t from = at < 100 ? 0 : at - 100;
      printf("wanted: %s\ngot: %s\n", want.substr(from, 300).c_str(),
             now.substr(from, 300).c_str());
    }
    test_assert(now == want);
    for (Client &c : clients) {
      pull(w, c, {editor.randomSocket(c), editor.randomSocket(c)});
    }
  };
  while (w.history.canUndo() && !retval) {
    editor.step(true);
    steps++;
  }
  same(first, "undoing everything made another def");
  for (; steps > 0 && !retval; steps--) {
    editor.step(false);
  }
  same(last, "redoing everything made another def");
}

} // namespace

int main(int argc, char **argv)
{
  const int scale = coredcc_test::testScale();
  const uint64_t first = argc > 1 ? uint64_t(atoll(argv[1])) : 1;
  const int worlds = argc > 2 ? atoi(argv[2]) : 20 * scale;
  const int batches = 250;
  for (int i = 0; i < worlds && !retval; i++) {
    runWorld(first + uint64_t(i), batches);
  }
  printf("seeds %llu to %llu: %llu edits, %llu pulls, %llu kernel visits, %llu runs (%llu had "
         "to, %llu might), %llu failures, %llu on cycles, %llu recursions, %llu groups, %llu undos, %llu redos\n",
         (unsigned long long)first, (unsigned long long)(first + uint64_t(worlds) - 1),
         (unsigned long long)g_counts.ops, (unsigned long long)g_counts.pulls,
         (unsigned long long)g_counts.visits, (unsigned long long)g_counts.runs,
         (unsigned long long)g_counts.musts, (unsigned long long)g_counts.mays,
         (unsigned long long)g_counts.errors, (unsigned long long)g_counts.cycles,
         (unsigned long long)g_counts.recursions, (unsigned long long)g_counts.groups,
         (unsigned long long)g_counts.undos, (unsigned long long)g_counts.redos);
  return test_end();
}
