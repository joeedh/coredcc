/* What edits are refused for: cycles of links (GraphDef::link, linkAll) and defs that
 * contain themselves (setGroup). Nothing here recurses natively: defs may nest deeply. */
#include "coredcc/base/text.h"
#include "coredcc/graph/interior.h"
#include "coredcc/graph/provider.h"
#include "coredcc/graph/topology.h"
#include "coredcc/lib/library.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>

namespace coredcc {

using litestl::util::Map;
using litestl::util::string;
using litestl::util::Vector;

/* ---- link ---- */

namespace {

/* One direction of the cycle check: a walk over sockets, one step at a time. Forward, the
 * sockets are inputs and the walk follows what depends on them; backward, they are
 * outputs and it follows what they depend on. */
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
    if (!isOneStep(role)) {
      return throughGroup(s);
    }
    if (s.node == goal_.node) {
      return true;
    }
    for (const GraphLink *l : forward_ ? s.node->linksOut() : s.node->linksIn()) {
      follow(*l);
    }
    return false;
  }

private:
  /* A group node. The sockets of its near side, where the walk comes in, and of its far
   * side go by their index in its def's table. */
  struct Group {
    Map<detail::SocketKey, int> near;
    /* What each near socket leads to. */
    Vector<Vector<int, 1>> leads;
    /* The links at each far socket. */
    Vector<Vector<const GraphLink *, 1>> links;
    int goal = -1;
  };

  bool throughGroup(const Socket &s)
  {
    if (!groups_) {
      groups_ = std::make_unique<Map<const GraphNode *, std::shared_ptr<Group>>>();
    }
    if (!groups_->lookup_ptr(s.node)) {
      groups_->add(s.node, makeGroup(*s.node));
    }
    const Group &g = **groups_->lookup_ptr(s.node);
    const int *at = g.near.lookup_ptr({s.key});
    if (!at) {
      return false;
    }
    for (int far : g.leads[*at]) {
      if (far == g.goal) {
        return true;
      }
      for (const GraphLink *l : g.links[far]) {
        follow(*l);
      }
    }
    return false;
  }

  std::shared_ptr<Group> makeGroup(const GraphNode &n) const
  {
    std::shared_ptr<Group> g = std::make_shared<Group>();
    const GraphDef *target = def_.groupTarget(n);
    if (!target) {
      return g;
    }
    const ReachKeys keys = interiorKeys(n);
    auto index = [&](SocketSide side, Map<detail::SocketKey, int> &out) {
      int i = 0;
      for (const InterfaceSocket &s : target->interfaceSockets(side)) {
        out.add({s.key()}, i++);
      }
      for (const InteriorKey &k : side == SocketSide::Input ? keys.ins : keys.outs) {
        out.add({k.toString().c_str()}, i++);
      }
      return size_t(i);
    };
    Map<detail::SocketKey, int> far;
    g->leads.resize(index(forward_ ? SocketSide::Input : SocketSide::Output, g->near));
    g->links.resize(index(forward_ ? SocketSide::Output : SocketSide::Input, far));
    const std::shared_ptr<const GraphReach> reach = graphReach(*target, keys);
    for (int o = 0; o < int(reach->inputs.size()); o++) {
      for (int i : reach->inputs[o]) {
        g->leads[forward_ ? i : o].append(forward_ ? o : i);
      }
    }
    for (const GraphLink *l : forward_ ? n.linksOut() : n.linksIn()) {
      if (const int *at = far.lookup_ptr({forward_ ? l->fromKey : l->toKey})) {
        g->links[*at].append(l);
      }
    }
    if (const int *at = &n == goal_.node ? far.lookup_ptr({goal_.key}) : nullptr) {
      g->goal = *at;
    }
    return g;
  }

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
  void follow(const GraphLink &l)
  {
    add(forward_ ? Socket{l.to, l.toKey} : Socket{l.from, l.fromKey});
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
  /* Made when there is one. */
  std::unique_ptr<Map<const GraphNode *, std::shared_ptr<Group>>> groups_;
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

/* ---- linkAll ---- */

namespace {

/* The sockets of a def and what they depend on, as arrays: its inputs, then its nodes (a
 * node stands for a kernel's outputs). */
class Dependencies {
public:
  explicit Dependencies(const GraphDef &def) : topo_(def.topology())
  {
    const GraphTopology &t = *topo_;
    for (const GraphTopology::Input &in : t.inputs) {
      begin_.append(int(edges_.size()));
      if (in.fromNode >= 0) {
        addSource(def, t.nodes[in.fromNode], in.fromNode, in.fromOutput);
      }
    }
    for (const GraphTopology::Node &n : t.nodes) {
      begin_.append(int(edges_.size()));
      for (int i = 0; isOneStep(n.role) && i < n.inCount; i++) {
        edges_.append(n.inBegin + i);
      }
    }
    begin_.append(int(edges_.size()));
  }

  /** The vertex of an input; -1 if it isn't there. */
  int inputOf(NodeId node, const char *key) const
  {
    const int at = topo_->find(node);
    const int i = at >= 0 ? topo_->inputIndex(topo_->nodes[at], key) : -1;
    return i >= 0 ? topo_->nodes[at].inBegin + i : -1;
  }

  /** Whether each vertex is on a cycle, by Tarjan's algorithm. */
  Vector<bool> cyclic() const
  {
    const int n = int(begin_.size()) - 1;
    Vector<int> number, low, next, stack, walk;
    Vector<bool> onStack, out;
    for (int i = 0; i < n; i++) {
      number.append(-1);
      low.append(0);
      next.append(begin_[i]);
      onStack.append(false);
      out.append(false);
    }
    int count = 0;
    auto enter = [&](int v) {
      number[v] = low[v] = count++;
      onStack[v] = true;
      stack.append(v);
      walk.append(v);
    };
    for (int root = 0; root < n; root++) {
      if (number[root] >= 0) {
        continue;
      }
      enter(root);
      while (walk.size() > 0) {
        const int v = walk.last();
        if (next[v] < begin_[v + 1]) {
          const int to = edges_[next[v]++];
          if (number[to] < 0) {
            enter(to);
          } else if (onStack[to]) {
            low[v] = std::min(low[v], number[to]);
            out[v] = out[v] || to == v;
          }
          continue;
        }
        walk.pop_back();
        if (walk.size() > 0) {
          low[walk.last()] = std::min(low[walk.last()], low[v]);
        }
        if (low[v] != number[v]) {
          continue;
        }
        const bool alone = stack.last() == v;
        int member = -1;
        while (member != v) {
          member = stack.pop_back();
          onStack[member] = false;
          out[member] = out[member] || !alone;
        }
      }
    }
    return out;
  }

private:
  struct Group {
    std::shared_ptr<const GraphReach> reach;
    /* The rows of the node's interior outputs, and its inputs for the columns of keys. */
    Vector<int> rows;
    Vector<int> columns;
    int interfaceIns = 0;
  };

  void addSource(const GraphDef &def, const GraphTopology::Node &n, int node, int output)
  {
    if (isOneStep(n.role)) {
      edges_.append(int(topo_->inputs.size()) + node);
      return;
    }
    const GraphDef *target =
        n.role == NodeRole::Group ? def.groupTarget(*n.node) : nullptr;
    if (!target) {
      return;
    }
    if (!groups_.lookup_ptr(n.group)) {
      groups_.add(n.group, makeGroup(*target, n));
    }
    const Group &g = **groups_.lookup_ptr(n.group);
    const int row = output < n.interfaceOuts ? output : g.rows[output - n.interfaceOuts];
    for (int i : g.reach->inputs[row]) {
      edges_.append(i < g.interfaceIns ? n.inBegin + i : g.columns[i - g.interfaceIns]);
    }
  }

  std::shared_ptr<Group> makeGroup(const GraphDef &target,
                                   const GraphTopology::Node &n) const
  {
    std::shared_ptr<Group> g = std::make_shared<Group>();
    ReachKeys keys;
    const int outs = int(target.interfaceSockets(SocketSide::Output).size());
    g->interfaceIns = int(target.interfaceSockets(SocketSide::Input).size());
    for (const GraphTopology::Interior &i : topo_->groups[n.group].interior) {
      if (i.index < 0) {
        continue;
      }
      if (i.side == SocketSide::Output) {
        g->rows.append(outs + int(keys.outs.size()));
      } else {
        g->columns.append(n.inBegin + i.index);
      }
      keys.add(i.side, i.key);
    }
    g->reach = graphReach(target, keys);
    return g;
  }

  std::shared_ptr<const GraphTopology> topo_;
  Map<int, std::shared_ptr<Group>> groups_;
  /* The edges of vertex v: edges_[begin_[v]] to edges_[begin_[v + 1] - 1]. */
  Vector<int> begin_;
  Vector<int> edges_;
};

} // namespace

int GraphDef::findCycle(const Vector<GraphLink *> &links) const
{
  const Dependencies deps(*this);
  const Vector<bool> cyclic = deps.cyclic();
  for (int i = 0; i < int(links.size()); i++) {
    /* All that an input depends on is its link. */
    const int at = deps.inputOf(links[i]->to->id(), links[i]->toKey.c_str());
    if (at >= 0 && cyclic[at]) {
      return i;
    }
  }
  return -1;
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
