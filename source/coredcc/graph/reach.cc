/* Reach tables. Nothing here recurses natively: defs may nest deeply. */
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

void ReachKeys::add(SocketSide side, const InteriorKey &key)
{
  Vector<InteriorKey> &list = side == SocketSide::Input ? ins : outs;
  if ((side == SocketSide::Input ? inAt_ : outAt_).add(key, int(list.size()))) {
    list.append(key);
  }
}

int ReachKeys::find(SocketSide side, const InteriorKey &key) const
{
  const int *i = (side == SocketSide::Input ? inAt_ : outAt_).lookup_ptr(key);
  return i ? *i : -1;
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
        e.groups.append(GroupEntry{target, inner, text, {}, 0, 0});
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
  /* Cached tables of one def, and what they hold. Those of keys that aren't used any more go
   * with the others. */
  static constexpr int kMaxTables = 256;
  static constexpr size_t kMaxCells = size_t(1) << 20;

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
    /* What keys.ins are outside: an input of the def, or ~i for the outer keys' i. */
    Vector<int> ins;
    /* The first rows and columns that are for keys. */
    int interfaceOuts = 0;
    int interfaceIns = 0;
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

  static void mapKeys(Entry &e)
  {
    const GraphTopology &t = *e.topo;
    for (int i = 0; i < int(e.groups.size()); i++) {
      GroupEntry &g = e.groups[i];
      if (!g.target) {
        continue;
      }
      g.interfaceOuts = int(g.target->interfaceSockets(SocketSide::Output).size());
      g.interfaceIns = int(g.target->interfaceSockets(SocketSide::Input).size());
      const GraphTopology::Node &n = t.nodes[t.groups[i].index];
      for (const InteriorKey &k : g.keys.ins) {
        InteriorKey outer = k;
        outer.groups = {};
        outer.groups.append(n.id);
        for (NodeId id : k.groups) {
          outer.groups.append(id);
        }
        const int from = e.keys.find(SocketSide::Input, outer); /* the outermost counts */
        g.ins.append(from >= 0 ? ~from : n.inBegin + t.inputIndex(n, k.toString()));
      }
    }
  }

  std::shared_ptr<const GraphReach> finish(Entry &e)
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
    mapKeys(e);
    std::shared_ptr<GraphReach> r = std::make_shared<GraphReach>();
    r->topology = e.topo->serial;
    r->groups = std::move(versions);
    r->version = e.def->runtime().nextVersion();
    compute(e, groups, *r);
    if (!recursive_) { /* with a recursion, it depends on where the walk started */
      size_t cells = 0;
      for (const Vector<int> &row : r->inputs) {
        cells += 1 + size_t(row.size());
      }
      if (e.def->reach_.size() >= kMaxTables || e.def->reachCells_ + cells > kMaxCells) {
        e.def->reach_ = {};
        e.def->reachCells_ = 0;
      }
      e.def->reachCells_ += cells;
      e.def->reach_.add_overwrite(e.name.c_str(), GraphDef::CachedReach{r, key_});
    }
    return r;
  }

  /* Walks up from each interface output and each interior output. `seen` holds, per input
   * socket, the last of them whose walk visited it; `reached` the same for the columns. */
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
    Vector<int> reached;
    for (int i = 0; i < nIns + int(e.keys.ins.size()); i++) {
      reached.append(-1);
    }
    auto reach = [&](int in) {
      if (reached[in] != row) {
        reached[in] = row;
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
      for (int i : g->inputs[groupRow]) {
        const int at = i < ge.interfaceIns ? n.inBegin + i : ge.ins[i - ge.interfaceIns];
        if (at >= 0) {
          visit(at);
        }
        else {
          reach(nIns + ~at);
        }
      }
    };
    auto interiorRow = [&](const GraphTopology::Node &n, const InteriorKey &k) {
      const GroupEntry &ge = e.groups[n.group];
      const int i = ge.keys.find(SocketSide::Output, k);
      return ge.target && i >= 0 ? ge.interfaceOuts + i : -1;
    };
    auto fromOutput = [&](const GraphTopology::Node &n, int output) {
      if (n.role == NodeRole::Input) {
        reach(output);
      }
      else if (n.role == NodeRole::Group && output < n.interfaceOuts) {
        throughGroup(n, output);
      }
      else if (n.role == NodeRole::Group) {
        throughGroup(n, interiorRow(n, t.groups[n.group].interiorOutput(n, output).key));
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

} // namespace coredcc
