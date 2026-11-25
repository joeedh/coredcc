/* Reach tables, and what uses them at edit time: the cycle check of GraphDef::link and the
 * recursion check of setGroup. Nothing here recurses natively: defs may nest deeply. */
#include "coredcc/base/text.h"
#include "coredcc/graph/interior.h"
#include "coredcc/graph/provider.h"
#include "coredcc/graph/topology.h"
#include "coredcc/lib/library.h"

#include <algorithm>
#include <cstring>
#include <string>

namespace coredcc {

using litestl::util::Map;
using litestl::util::string;
using litestl::util::Vector;

/* ---- ReachKeys ---- */

static int indexOfKey(const Vector<InteriorKey> &list, const InteriorKey &key)
{
  for (int i = 0; i < int(list.size()); i++) {
    if (list[i] == key) {
      return i;
    }
  }
  return -1;
}

void ReachKeys::add(SocketSide side, const InteriorKey &key)
{
  Vector<InteriorKey> &list = side == SocketSide::Input ? ins : outs;
  if (indexOfKey(list, key) < 0) {
    list.append(key);
  }
}

int ReachKeys::find(SocketSide side, const InteriorKey &key) const
{
  return indexOfKey(side == SocketSide::Input ? ins : outs, key);
}

std::string ReachKeys::toString() const
{
  std::string s;
  for (const InteriorKey &k : outs) {
    s += k.toString() + ">";
  }
  for (const InteriorKey &k : ins) {
    s += k.toString() + "<";
  }
  return s;
}

/* ---- tables ---- */

/** Computes tables for the defs below one def, each once per set of interior keys. */
class GraphReachBuilder {
 public:
  explicit GraphReachBuilder(const GraphDef &root)
      : key_(GraphPassKey::of(root.runtime(), root.library()))
  {
  }
  std::shared_ptr<const GraphReach> get(const GraphDef &root, const ReachKeys &keys)
  {
    const std::string name = keys.toString();
    if (std::shared_ptr<const GraphReach> r = cached(root, name)) {
      return r;
    }
    push(root, keys, name);
    while (stack_.size() > 0) {
      Entry &e = stack_.last();
      if (e.group < int(e.topo->groups.size())) {
        const GraphTopology::Group &g = e.topo->groups[e.group++];
        const GraphDef *target = e.def->groupTarget(*g.node);
        ReachKeys inner;
        if (target) {
          inner = keysInside(e, g);
        }
        const std::string text = inner.toString();
        e.groups.append(GroupEntry{target, inner, text});
        if (!target) {
          continue;
        }
        const SlotKey slot{target, text};
        if (const Slot *s = slots_.lookup_ptr(slot)) {
          recursive_ |= !s->done;
        }
        else if (std::shared_ptr<const GraphReach> r = cached(*target, text)) {
          slots_.add(slot, Slot{true, r});
        }
        else {
          push(*target, inner, text);
        }
        continue;
      }
      std::shared_ptr<const GraphReach> r = finish(e);
      *slots_.lookup_ptr(SlotKey{e.def, e.name}) = Slot{true, r};
      stack_.pop_back();
      if (stack_.size() == 0) {
        return r;
      }
    }
    return nullptr;
  }

 private:
  /* Cached tables of one def. Those of keys that aren't used any more go with the others. */
  static constexpr int kMaxTables = 256;

  struct Slot {
    bool done = false;
    std::shared_ptr<const GraphReach> reach;
  };
  struct SlotKey {
    const GraphDef *def = nullptr;
    std::string name;
    uint64_t computeHash() const
    {
      return detail::fnv1a64(name.c_str(), uint64_t(uintptr_t(def)) * 0x9E3779B97F4A7C15ull);
    }
    bool operator==(const SlotKey &) const = default;
  };
  struct GroupEntry {
    const GraphDef *target = nullptr;
    ReachKeys keys;
    std::string name;
  };
  struct Entry {
    const GraphDef *def = nullptr;
    ReachKeys keys;
    std::string name;
    std::shared_ptr<const GraphTopology> topo;
    int group = 0;
    Vector<GroupEntry> groups;
  };

  std::shared_ptr<const GraphReach> cached(const GraphDef &def, const std::string &name) const
  {
    const GraphDef::CachedReach *c = def.reach_.lookup_ptr(name.c_str());
    return c && c->key == key_ ? c->reach : nullptr;
  }

  void push(const GraphDef &def, const ReachKeys &keys, const std::string &name)
  {
    slots_.add(SlotKey{&def, name}, Slot());
    Entry e;
    e.def = &def;
    e.keys = keys;
    e.name = name;
    e.topo = def.topology();
    stack_.append(std::move(e));
  }

  /* The keys that matter inside a group: those its node uses, and the rest of the outer ones
   * that lead into it. */
  static ReachKeys keysInside(const Entry &e, const GraphTopology::Group &g)
  {
    ReachKeys out;
    for (const GraphTopology::Interior &i : g.interior) {
      if (i.index >= 0) {
        out.add(i.side, i.key);
      }
    }
    for (const SocketSide side : {SocketSide::Output, SocketSide::Input}) {
      for (const InteriorKey &k : side == SocketSide::Output ? e.keys.outs : e.keys.ins) {
        if (k.groups.size() > 0 && k.groups[0] == g.node->id()) {
          out.add(side, k.inner());
        }
      }
    }
    return out;
  }

  std::shared_ptr<const GraphReach> finish(const Entry &e)
  {
    Vector<const GraphReach *> groups;
    Vector<uint64_t> versions;
    for (const GroupEntry &g : e.groups) {
      const Slot *s = g.target ? slots_.lookup_ptr(SlotKey{g.target, g.name}) : nullptr;
      groups.append(s ? s->reach.get() : nullptr);
      versions.append(s && s->reach ? s->reach->version : 0);
    }
    GraphDef::CachedReach *old = e.def->reach_.lookup_ptr(e.name.c_str());
    if (old && old->reach->topology == e.topo->serial) {
      bool same = true; /* the topology says how many groups there are, and their keys */
      for (int i = 0; i < int(versions.size()); i++) {
        same &= old->reach->groups[i] == versions[i];
      }
      if (same) {
        old->key = key_;
        return old->reach;
      }
    }
    std::shared_ptr<GraphReach> r = std::make_shared<GraphReach>();
    r->topology = e.topo->serial;
    r->groups = std::move(versions);
    r->version = e.def->runtime().nextVersion();
    compute(e, groups, *r);
    if (!recursive_) { /* with a recursion, it depends on where the walk started */
      if (e.def->reach_.size() >= kMaxTables) {
        e.def->reach_ = {};
      }
      e.def->reach_.add_overwrite(e.name.c_str(), GraphDef::CachedReach{r, key_});
    }
    return r;
  }

  /* Walks up from each interface output and each interior output. `seen` holds, per input
   * socket, the last of them whose walk visited it. */
  static void compute(const Entry &e, const Vector<const GraphReach *> &groups, GraphReach &out)
  {
    const GraphTopology &t = *e.topo;
    const int nIns = int(e.def->interfaceSockets(SocketSide::Input).size());
    const int nOuts = int(e.def->interfaceSockets(SocketSide::Output).size());
    Map<int, int> replaced; /* interior inputs, by input socket */
    for (int i = 0; i < int(e.keys.ins.size()); i++) {
      const InteriorKey &k = e.keys.ins[i];
      const int at = k.groups.size() == 0 ? t.find(k.node) : -1;
      const int input = at >= 0 ? t.inputIndex(t.nodes[at], k.key.c_str()) : -1;
      if (input >= 0) {
        replaced.add(t.nodes[at].inBegin + input, i);
      }
    }

    Vector<int> seen;
    for (int i = 0; i < int(t.inputs.size()); i++) {
      seen.append(-1);
    }
    Vector<int> stack;
    int row = 0;
    auto reach = [&](int in) {
      if (!out.inputs[row].contains(in)) {
        out.inputs[row].append(in);
      }
    };
    auto visit = [&](int input) {
      if (seen[input] != row) {
        seen[input] = row;
        stack.append(input);
      }
    };
    /* What a row of a group's table depends on. */
    auto throughGroup = [&](const GraphTopology::Node &n, int groupRow) {
      const GraphReach *g = groups[n.group];
      if (!g || !t.groups[n.group].targetInterface || groupRow < 0) {
        return;
      }
      const GroupEntry &ge = e.groups[n.group];
      const int groupIns = int(ge.target->interfaceSockets(SocketSide::Input).size());
      for (int i : g->inputs[groupRow]) {
        if (i < groupIns) {
          visit(n.inBegin + i);
          continue;
        }
        InteriorKey k = ge.keys.ins[i - groupIns];
        InteriorKey outer = k;
        outer.groups = {};
        outer.groups.append(n.id);
        for (NodeId id : k.groups) {
          outer.groups.append(id);
        }
        if (const int from = e.keys.find(SocketSide::Input, outer); from >= 0) {
          reach(nIns + from); /* the outermost counts */
        }
        else {
          visit(n.inBegin + t.inputIndex(n, k.toString()));
        }
      }
    };
    auto interiorRow = [&](const GraphTopology::Node &n, const InteriorKey &k) {
      const GroupEntry &ge = e.groups[n.group];
      const int i = ge.keys.find(SocketSide::Output, k);
      return ge.target && i >= 0 ?
                 int(ge.target->interfaceSockets(SocketSide::Output).size()) + i :
                 -1;
    };
    auto fromOutput = [&](const GraphTopology::Node &n, int output) {
      if (n.role == NodeRole::Input) {
        reach(output);
      }
      else if (n.role == NodeRole::Group && output < n.interfaceOuts) {
        throughGroup(n, output);
      }
      else if (n.role == NodeRole::Group) {
        for (const GraphTopology::Interior &i : t.groups[n.group].interior) {
          if (i.side == SocketSide::Output && i.index == output) {
            throughGroup(n, interiorRow(n, i.key));
          }
        }
      }
      else if (isOneStep(n.role)) {
        for (int i = 0; i < n.inCount; i++) {
          visit(n.inBegin + i);
        }
      }
    };

    out.inputs.resize(size_t(nOuts) + size_t(e.keys.outs.size()));
    for (row = 0; row < int(out.inputs.size()); row++) {
      if (row < nOuts) {
        if (t.outputNode >= 0) {
          visit(t.nodes[t.outputNode].inBegin + row);
        }
      }
      else {
        const InteriorKey &k = e.keys.outs[row - nOuts];
        const bool here = k.groups.size() == 0;
        const int at = t.find(here ? k.node : k.groups[0]);
        if (at >= 0 && here) {
          const int o = t.outputIndex(t.nodes[at], k.key.c_str());
          if (o >= 0) {
            fromOutput(t.nodes[at], o);
          }
        }
        else if (at >= 0 && t.nodes[at].role == NodeRole::Group) {
          throughGroup(t.nodes[at], interiorRow(t.nodes[at], k.inner()));
        }
      }
      while (stack.size() > 0) {
        const int input = stack.pop_back();
        const GraphTopology::Input &in = t.inputs[input];
        if (const int *k = replaced.lookup_ptr(input)) {
          reach(nIns + *k);
        }
        else if (in.fromNode >= 0) {
          fromOutput(t.nodes[in.fromNode], in.fromOutput);
        }
      }
      std::sort(out.inputs[row].begin(), out.inputs[row].end());
    }
  }

  GraphPassKey key_;
  Map<SlotKey, Slot> slots_;
  Vector<Entry> stack_;
  bool recursive_ = false;
};

std::shared_ptr<const GraphReach> graphReach(const GraphDef &def, const ReachKeys &keys)
{
  GraphReachBuilder b(def);
  return b.get(def, keys);
}

/* ---- link ---- */

namespace {

/* A socket of a group node in the table of its def: a row for an output, a column for an
 * input. -1 if it isn't there. */
int tableIndex(const GraphDef &target, const ReachKeys &keys, SocketSide side, const string &key)
{
  const Vector<InterfaceSocket> &list = target.interfaceSockets(side);
  for (int i = 0; i < int(list.size()); i++) {
    if (strcmp(list[i].key().c_str(), key.c_str()) == 0) {
      return i;
    }
  }
  const std::optional<InteriorKey> k = InteriorKey::parse(key.c_str());
  const int i = k ? keys.find(side, *k) : -1;
  return i >= 0 ? int(list.size()) + i : -1;
}

/* One direction of the cycle check: a walk over sockets, one step at a time. Forward, the
 * sockets are inputs and the walk follows what depends on them; backward, they are outputs
 * and it follows what they depend on. */
class CycleWalk {
 public:
  struct Socket {
    const GraphNode *node;
    string key;
  };

  CycleWalk(const GraphDef &def, bool forward, Socket start, Socket goal)
      : def_(def), forward_(forward), start_(start), goal_(std::move(goal))
  {
    add(std::move(start));
  }
  bool done() const
  {
    return next_ >= int(queue_.size());
  }
  /** Follows one socket; true if it found the goal. */
  bool step()
  {
    const Socket s = queue_[next_++];
    const NodeType *type = s.node->type();
    const NodeRole role = type ? type->role : NodeRole::Kernel;
    if (role == NodeRole::Input || role == NodeRole::Output) {
      return false;
    }
    if (isOneStep(role)) {
      return follow(s.node, [](const string &) { return true; });
    }
    const GraphDef *target = def_.groupTarget(*s.node);
    if (!target) {
      return false;
    }
    const SocketSide mine = forward_ ? SocketSide::Input : SocketSide::Output;
    const SocketSide other = forward_ ? SocketSide::Output : SocketSide::Input;
    const ReachKeys keys = interiorKeys(*s.node);
    const int index = tableIndex(*target, keys, mine, s.key);
    const std::shared_ptr<const GraphReach> reach = graphReach(*target, keys);
    return follow(s.node, [&](const string &key) {
      const int o = tableIndex(*target, keys, other, key);
      if (index < 0 || o < 0) {
        return false;
      }
      return reach->inputs[forward_ ? o : index].contains(forward_ ? index : o);
    });
  }

 private:
  /* Of a group node's links, the new one included. */
  ReachKeys interiorKeys(const GraphNode &n) const
  {
    ReachKeys keys;
    auto use = [&keys](SocketSide side, const string &key) {
      if (const std::optional<InteriorKey> k = InteriorKey::parse(key.c_str())) {
        keys.add(side, *k);
      }
    };
    for (const GraphLink *l : n.linksIn()) {
      if (isInteriorKey(l->toKey.c_str()) &&
          def_.findSocket(*l->from, SocketSide::Output, l->fromKey.c_str()))
      {
        use(SocketSide::Input, l->toKey); /* else it replaces nothing */
      }
    }
    for (const GraphLink *l : n.linksOut()) {
      use(SocketSide::Output, l->fromKey);
    }
    if (start_.node == &n) {
      use(forward_ ? SocketSide::Input : SocketSide::Output, start_.key);
    }
    if (goal_.node == &n) {
      use(forward_ ? SocketSide::Output : SocketSide::Input, goal_.key);
    }
    return keys;
  }
  void add(Socket s)
  {
    /* All sockets of such a node lead to the same places. */
    const NodeType *type = s.node->type();
    const bool kernel = !type || isOneStep(type->role);
    const uint64_t at = uint64_t(uintptr_t(s.node));
    if (seen_.add(Seen{at, kernel ? string() : s.key}, true)) {
      queue_.append(std::move(s));
    }
  }
  template <class Fn> bool follow(const GraphNode *node, Fn &&leadsTo)
  {
    if (node == goal_.node && leadsTo(goal_.key)) {
      return true;
    }
    for (const GraphLink *l : forward_ ? node->linksOut() : node->linksIn()) {
      if (leadsTo(forward_ ? l->fromKey : l->toKey)) {
        add(forward_ ? Socket{l->to, l->toKey} : Socket{l->from, l->fromKey});
      }
    }
    return false;
  }

  struct Seen {
    uint64_t node = 0;
    string key;
    uint64_t computeHash() const
    {
      return detail::fnv1a64(key.c_str(), node * 0x9E3779B97F4A7C15ull);
    }
    bool operator==(const Seen &b) const
    {
      return node == b.node && strcmp(key.c_str(), b.key.c_str()) == 0;
    }
  };
  const GraphDef &def_;
  bool forward_;
  Socket start_;
  Socket goal_;
  Vector<Socket> queue_;
  int next_ = 0;
  Map<Seen, bool> seen_;
};

} // namespace

bool GraphDef::wouldCycle(const GraphNode &from,
                          std::string_view fromKey,
                          const GraphNode &to,
                          std::string_view toKey) const
{
  /* Both directions at once, so adding to either end of a long chain is cheap. */
  const CycleWalk::Socket out{&from, lsString(fromKey)};
  const CycleWalk::Socket in{&to, lsString(toKey)};
  CycleWalk down(*this, true, in, out);
  CycleWalk up(*this, false, out, in);
  while (!down.done() && !up.done()) {
    if (down.step() || up.step()) {
      return true;
    }
  }
  return false;
}

/* ---- setGroup ---- */

Status GraphDef::checkRecursion(BlockId def) const
{
  const Library *lib = lib_.get();
  if (self_.isNull() || !lib) {
    return Status(); /* no group can name this def */
  }
  Vector<const GraphDef *> stack;
  Map<const GraphDef *, bool> seen;
  auto visit = [&](const GraphDef *d) {
    if (d && seen.add(d, true)) {
      stack.append(d);
    }
  };
  visit(graphDefOf(*lib, lib->find(def)));
  while (stack.size() > 0) {
    const GraphDef *d = stack.pop_back();
    if (d == this) {
      return Status::error(StatusCode::InvalidArgument,
                           "a graph can't contain itself, through groups either");
    }
    const std::shared_ptr<const GraphTopology> t = d->topology();
    for (const GraphTopology::Group &g : t->groups) {
      visit(d->groupTarget(*g.node));
    }
  }
  return Status();
}

} // namespace coredcc
