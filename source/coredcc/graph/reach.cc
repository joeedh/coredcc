/* Reach tables, and what uses them at edit time: the cycle check of GraphDef::link and the
 * recursion check of setGroup. Nothing here recurses natively: defs may nest deeply. */
#include "coredcc/base/text.h"
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

/** Computes tables for the defs below one def, each once. */
class GraphReachBuilder {
 public:
  explicit GraphReachBuilder(const GraphDef &root)
      : key_(GraphPassKey::of(root.runtime(), root.library()))
  {
  }
  std::shared_ptr<const GraphReach> get(const GraphDef &root)
  {
    if (root.reach_ && root.reachKey_ == key_) {
      return root.reach_;
    }
    push(root);
    while (stack_.size() > 0) {
      Entry &e = stack_.last();
      if (e.group < int(e.topo->groups.size())) {
        const GraphDef *target = e.def->groupTarget(*e.topo->groups[e.group].node);
        e.group++;
        if (!target) {
          continue;
        }
        if (const Slot *s = slots_.lookup_ptr(target)) {
          recursive_ |= !s->done;
        }
        else if (target->reach_ && target->reachKey_ == key_) {
          slots_.add(target, Slot{true, target->reach_});
        }
        else {
          push(*target);
        }
        continue;
      }
      std::shared_ptr<const GraphReach> r = finish(e);
      *slots_.lookup_ptr(e.def) = Slot{true, r};
      stack_.pop_back();
      if (stack_.size() == 0) {
        return r;
      }
    }
    return nullptr;
  }

 private:
  struct Slot {
    bool done = false;
    std::shared_ptr<const GraphReach> reach;
  };
  struct Entry {
    const GraphDef *def = nullptr;
    std::shared_ptr<const GraphTopology> topo;
    int group = 0;
  };

  void push(const GraphDef &def)
  {
    slots_.add(&def, Slot());
    stack_.append(Entry{&def, def.topology(), 0});
  }

  std::shared_ptr<const GraphReach> finish(const Entry &e)
  {
    Vector<const GraphReach *> groups;
    Vector<uint64_t> versions;
    for (const GraphTopology::Group &g : e.topo->groups) {
      const GraphDef *target = e.def->groupTarget(*g.node);
      const Slot *s = target ? slots_.lookup_ptr(target) : nullptr;
      groups.append(s ? s->reach.get() : nullptr);
      versions.append(s && s->reach ? s->reach->version : 0);
    }
    const std::shared_ptr<const GraphReach> &old = e.def->reach_;
    if (old && old->topology == e.topo->serial) {
      bool same = true; /* the topology says how many groups there are */
      for (int i = 0; i < int(versions.size()); i++) {
        same &= old->groups[i] == versions[i];
      }
      if (same) {
        e.def->reachKey_ = key_;
        return old;
      }
    }
    std::shared_ptr<GraphReach> r = std::make_shared<GraphReach>();
    r->topology = e.topo->serial;
    r->groups = std::move(versions);
    r->version = e.def->runtime().nextVersion();
    compute(*e.topo, *e.def, groups, *r);
    if (!recursive_) { /* with a recursion, it depends on where the walk started */
      e.def->reach_ = r;
      e.def->reachKey_ = key_;
    }
    return r;
  }

  /* Walks up from each interface output. `seen` holds, per input socket, the last output
   * whose walk visited it. */
  static void compute(const GraphTopology &t,
                      const GraphDef &def,
                      const Vector<const GraphReach *> &groups,
                      GraphReach &out)
  {
    const int nOutputs = int(def.interfaceSockets(SocketSide::Output).size());
    out.inputs.resize(size_t(nOutputs));
    if (t.outputNode < 0) {
      return;
    }
    Vector<int> seen;
    for (int i = 0; i < int(t.inputs.size()); i++) {
      seen.append(-1);
    }
    Vector<int> stack;
    for (int o = 0; o < nOutputs; o++) {
      Vector<int> &reached = out.inputs[o];
      auto visit = [&](int input) {
        if (seen[input] != o) {
          seen[input] = o;
          stack.append(input);
        }
      };
      visit(t.nodes[t.outputNode].inBegin + o);
      while (stack.size() > 0) {
        const GraphTopology::Input &in = t.inputs[stack.pop_back()];
        if (in.fromNode < 0) {
          continue;
        }
        const GraphTopology::Node &n = t.nodes[in.fromNode];
        if (n.role == NodeRole::Input) {
          if (!reached.contains(in.fromOutput)) {
            reached.append(in.fromOutput);
          }
        }
        else if (n.role == NodeRole::Group) {
          const GraphReach *g = groups[n.group];
          if (g && t.groups[n.group].targetInterface) {
            for (int i : g->inputs[in.fromOutput]) {
              visit(n.inBegin + i);
            }
          }
        }
        else {
          for (int i = 0; i < n.inCount; i++) {
            visit(n.inBegin + i);
          }
        }
      }
      std::sort(reached.begin(), reached.end());
    }
  }

  GraphPassKey key_;
  Map<const GraphDef *, Slot> slots_;
  Vector<Entry> stack_;
  bool recursive_ = false;
};

std::shared_ptr<const GraphReach> graphReach(const GraphDef &def)
{
  GraphReachBuilder b(def);
  return b.get(def);
}

/* ---- link ---- */

namespace {

int interfaceIndex(const GraphDef &def, SocketSide side, std::string_view key)
{
  const Vector<InterfaceSocket> &list = def.interfaceSockets(side);
  for (int i = 0; i < int(list.size()); i++) {
    if (key == list[i].key().c_str()) {
      return i;
    }
  }
  return -1;
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
      : def_(def), forward_(forward), goal_(std::move(goal))
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
    if (role == NodeRole::Kernel) {
      return follow(s.node, [](const string &) { return true; });
    }
    const GraphDef *target = def_.groupTarget(*s.node);
    if (!target) {
      return false;
    }
    const SocketSide mine = forward_ ? SocketSide::Input : SocketSide::Output;
    const SocketSide other = forward_ ? SocketSide::Output : SocketSide::Input;
    const int index = interfaceIndex(*target, mine, s.key.c_str());
    const std::shared_ptr<const GraphReach> reach = graphReach(*target);
    return follow(s.node, [&](const string &key) {
      const int o = interfaceIndex(*target, other, key.c_str());
      if (index < 0 || o < 0) {
        return false;
      }
      return reach->inputs[forward_ ? o : index].contains(forward_ ? index : o);
    });
  }

 private:
  void add(Socket s)
  {
    /* All sockets of a kernel node lead to the same places. */
    const NodeType *type = s.node->type();
    const bool kernel = !type || type->role == NodeRole::Kernel;
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
