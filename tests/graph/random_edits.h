#pragma once

/* The random edits of test_graph_random.cc. */
#include "random_world.h"

namespace random_graph {

class Editor {
 public:
  Editor(World &w, SplitMix64 &rng, std::vector<Client> &clients)
      : w_(w), rng_(rng), clients_(clients)
  {
  }

  SocketPath randomSocket(const Client &c)
  {
    const Graph &root = w_.defs[size_t(c.def)];
    const GraphDef *def = root.def;
    if (rng_.below(4) == 0) {
      const auto &outs = def->interfaceSockets(SocketSide::Output);
      return outs.size() == 0 || rng_.below(20) == 0 ?
                 SocketPath::output("missing") :
                 SocketPath::output(outs[int(rng_.below(uint32_t(outs.size())))].name.c_str());
    }
    NodePath path;
    for (int tries = 0; tries < 12; tries++) {
      const GraphNode *n = randomNode(*def);
      if (!n) {
        break;
      }
      const GraphDef *inner = def->groupTarget(*n);
      if (inner && path.groups.size() < 3 && rng_.below(3)) {
        path.groups.append(n->id());
        def = inner;
        continue;
      }
      if (n->linksIn().size() == 0 && rng_.below(3)) {
        continue; /* rather one with something behind it */
      }
      path.node = n->id();
      const SocketSide side = rng_.below(4) ? SocketSide::Output : SocketSide::Input;
      const auto &list = def->sockets(*n, side);
      if (list.size() == 0) {
        continue;
      }
      return SocketPath::of(path, list[int(rng_.below(uint32_t(list.size())))].key.c_str(), side);
    }
    return SocketPath::of(NodeId{1}, "missing");
  }

  void edit()
  {
    g_counts.ops++;
    Graph &g = w_.defs[rng_.below(kDefs)];
    switch (rng_.below(34)) {
      case 0:
      case 1:
      case 2:
        return addNode(*g.def);
      case 3:
        return removeNode(*g.def);
      case 4:
      case 5:
      case 6:
      case 7:
      case 8:
      case 9:
        return addLink(*g.def, false);
      case 10:
        return addLink(*g.def, true);
      case 11:
      case 12:
        return unlink(*g.def);
      case 13:
      case 14:
      case 15:
        return setParam(*g.def);
      case 16:
      case 17:
      case 18:
        return setLiteral(*g.def);
      case 19:
      case 20:
        return addInterface(*g.def);
      case 21:
        return editInterface(*g.def);
      case 22:
      case 23:
        return setGroup(*g.def);
      case 24:
        return toggleDef(g);
      case 25:
      case 26:
      case 27:
        return editThing();
      case 28:
        return toggleThing();
      case 29:
        return setInput();
      case 30:
        w_.rt.touch();
        return;
      case 31:
        if (rng_.below(3) == 0) {
          for (BlockId id : w_.things) {
            w_.env.notified[id]++;
          }
          w_.rt.ext<ChangeBus>().notifyAll();
        }
        return;
      case 32:
        return rare(g);
      default:
        return removeAndRevive(*g.def);
    }
  }

 private:
  const GraphNode *randomNode(const GraphDef &def)
  {
    if (def.nodeCount() == 0) {
      return nullptr;
    }
    int at = int(rng_.below(uint32_t(def.nodeCount())));
    const GraphNode *found = nullptr;
    def.forEachNode([&](const GraphNode &n) {
      if (at-- == 0) {
        found = &n;
      }
    });
    return found;
  }
  bool randomKey(const GraphDef &def, const GraphNode &n, SocketSide side, std::string &out)
  {
    const auto &list = def.sockets(n, side);
    if (list.size() == 0) {
      return false;
    }
    out = list[int(rng_.below(uint32_t(list.size())))].key.c_str();
    return true;
  }
  PropValue randomProp(const std::string &type)
  {
    const int n = int(rng_.below(10)) - 2;
    if (type == "bool") {
      return PropValue::ofBool(n % 2 != 0);
    }
    if (type == "int") {
      return PropValue::ofInt(n);
    }
    if (type == "vec3") {
      return PropValue::ofVec3(n, 1, 2);
    }
    if (type == "string") {
      return PropValue::ofString(n % 2 ? "ab" : "abc");
    }
    return PropValue::ofFloat(n + (rng_.below(3) == 0 ? 0.5 : 0));
  }
  std::string randomType()
  {
    const char *types[] = {"float", "float", "float", "int", "bool", "vec3", "string", "r.P"};
    return types[rng_.below(8)];
  }

  void addNode(GraphDef &def)
  {
    if (def.nodeCount() > 14) {
      return removeNode(def);
    }
    const char *types[] = {"r.sum",   "r.sum",   "r.sum",   "r.split",     "r.block",
                           "r.vec",   "r.text",  "r.pay",   "r.count",     "r.makeVec",
                           "r.sum",   "r.late",  "graph.input", "graph.output", "graph.group",
                           "graph.group"};
    const std::string type = types[rng_.below(16)];
    if (type != "r.late" || w_.late) {
      def.addNode(type).status().ignore();
      return;
    }
    NodeState s;
    s.id = NodeId{int64_t(1 + rng_.below(1 << 30))};
    s.typeName = "r.late";
    s.inputs.append(SocketInfo{"a", "a", "float"});
    s.outputs.append(SocketInfo{"value", "value", "float"});
    s.outputs.append(SocketInfo{"z", "z", "r.Q"});
    def.insertNode(s).ignore();
  }
  void removeNode(GraphDef &def)
  {
    if (const GraphNode *n = randomNode(def)) {
      test_assert(def.removeNode(n->id()).ok());
    }
  }
  void removeAndRevive(GraphDef &def)
  {
    const GraphNode *n = randomNode(def);
    if (!n) {
      return;
    }
    const NodeState state = def.stateOf(*n);
    std::vector<LinkState> links;
    for (const GraphLink *l : n->linksIn()) {
      links.push_back(GraphDef::stateOf(*l));
    }
    for (const GraphLink *l : n->linksOut()) {
      if (l->to != n) {
        links.push_back(GraphDef::stateOf(*l));
      }
    }
    test_assert(def.removeNode(state.id).ok());
    const bool ok = def.insertNode(state).ok(); /* a recursive group is refused */
    for (const LinkState &l : links) {
      test_assert(def.insertLink(l).ok() || !ok);
    }
  }
  void addLink(GraphDef &def, bool unchecked)
  {
    const GraphNode *a = randomNode(def), *b = randomNode(def);
    std::string from, to;
    if (!a || !randomKey(def, *a, SocketSide::Output, from) ||
        !randomKey(def, *b, SocketSide::Input, to))
    {
      return;
    }
    if (!unchecked) {
      for (int i = 0; i < 4 && !def.link({a->id(), from}, {b->id(), to}).ok(); i++) {
        a = randomNode(def);
        if (!randomKey(def, *a, SocketSide::Output, from)) {
          return;
        }
      }
      return;
    }
    LinkState l;
    l.id = LinkId{int64_t(1 + rng_.below(1 << 30))};
    l.from = a->id();
    l.fromKey = from.c_str();
    l.to = b->id();
    l.toKey = to.c_str();
    def.insertLink(l).ignore();
  }
  void unlink(GraphDef &def)
  {
    const GraphNode *n = randomNode(def);
    if (n && n->linksIn().size() > 0) {
      test_assert(def.unlink(n->linksIn()[0]->id).ok());
    }
  }
  void setParam(GraphDef &def)
  {
    const GraphNode *n = randomNode(def);
    if (!n || !n->type() || n->type()->params.size() == 0) {
      return;
    }
    const PropertyDef &p = n->type()->params[0].def;
    PropValue v;
    if (p.kind() == PropKind::Pointer) {
      if (rng_.below(4)) {
        StableRef r;
        r.owner = OwnerKey::ofBlock(w_.things[rng_.below(kThings)]);
        v = PropValue::ofPointer(r);
      }
    }
    else if (rng_.below(6)) {
      v = p.kind() == PropKind::Int ? PropValue::ofInt(int(rng_.below(6)) - 2) :
                                      randomProp("string");
    }
    test_assert(def.setParam(n->id(), p.apiName().c_str(), v).ok());
  }
  void setLiteral(GraphDef &def)
  {
    const GraphNode *n = randomNode(def);
    std::string key;
    if (!n || !randomKey(def, *n, SocketSide::Input, key)) {
      return;
    }
    for (const SocketInfo &s : def.sockets(*n, SocketSide::Input)) {
      if (key == s.key.c_str()) {
        def.setLiteral(n->id(), key, rng_.below(6) ? randomProp(s.typeName.c_str()) : PropValue())
            .ignore();
        return;
      }
    }
  }
  void addInterface(GraphDef &def)
  {
    const SocketSide side = rng_.below(2) ? SocketSide::Input : SocketSide::Output;
    const int n = int(def.interfaceSockets(side).size());
    if (n >= 4) {
      return removeInterface(def);
    }
    const std::string name = "s" + std::to_string(w_.names++);
    test_assert(
        def.addInterface(side, name, randomType(), int(rng_.below(uint32_t(n + 1)))).ok());
  }
  const InterfaceSocket *randomInterface(GraphDef &def, SocketSide &side)
  {
    side = rng_.below(2) ? SocketSide::Input : SocketSide::Output;
    const auto &list = def.interfaceSockets(side);
    return list.size() == 0 ? nullptr : &list[int(rng_.below(uint32_t(list.size())))];
  }
  void removeInterface(GraphDef &def)
  {
    SocketSide side;
    const InterfaceSocket *s = randomInterface(def, side);
    if (!s) {
      return;
    }
    removed_.push_back({&def, side, *s});
    test_assert(def.removeInterface(s->id).ok());
  }
  void editInterface(GraphDef &def)
  {
    SocketSide side;
    const InterfaceSocket *s = randomInterface(def, side);
    if (rng_.below(3) == 0 && !removed_.empty()) {
      const Removed r = removed_.back();
      removed_.pop_back();
      r.def->addInterface(r.side, r.socket.name.c_str(), r.socket.typeName.c_str(), 0, r.socket.id)
          .status()
          .ignore();
      return;
    }
    if (!s) {
      return;
    }
    const int64_t id = s->id;
    switch (rng_.below(5)) {
      case 0:
        return removeInterface(def);
      case 1:
        test_assert(def.moveInterface(id, int(rng_.below(uint32_t(
                                              def.interfaceSockets(side).size()))))
                        .ok());
        return;
      case 2:
        test_assert(def.setInterfaceType(id, randomType()).ok());
        return;
      case 3:
        test_assert(def.setInterfaceName(id, "s" + std::to_string(w_.names++)).ok());
        return;
      default:
        def.setInterfaceDefault(id, rng_.below(5) ? randomProp(s->typeName.c_str()) : PropValue())
            .ignore();
        return;
    }
  }
  void setGroup(GraphDef &def)
  {
    NodeId id;
    int seen = 0;
    def.forEachNode([&](const GraphNode &n) {
      if (strcmp(n.typeName().c_str(), "graph.group") == 0 && rng_.below(uint32_t(++seen)) == 0) {
        id = n.id();
      }
    });
    if (id) {
      const BlockId to = rng_.below(8) ? w_.defs[rng_.below(kDefs)].id : BlockId();
      def.setGroup(id, to).ignore(); /* recursion is refused */
    }
  }
  void toggleDef(Graph &g)
  {
    Graph *dead = nullptr;
    for (Graph &d : w_.defs) {
      dead = w_.live(d) ? dead : &d;
    }
    if (dead) {
      test_assert(w_.lib.restoreFromHistory(dead->h).ok());
    }
    else if (rng_.below(3) == 0) {
      test_assert(w_.lib.markDeleted(g.h).ok());
    }
  }
  void editThing()
  {
    const BlockId id = w_.things[rng_.below(kThings)];
    if (const Datablock *b = w_.lib.get(w_.lib.find(id))) {
      static_cast<Thing *>(b->clientData())->value = int(rng_.below(5));
      w_.notify(id);
    }
  }
  void toggleThing()
  {
    const BlockId id = w_.things[rng_.below(kThings)];
    const BlockHandle h = w_.lib.find(id);
    const Datablock *b = w_.lib.get(h);
    if (!b) {
      Thing *t = litestl::alloc::New<Thing>("test::Thing");
      t->value = int(rng_.below(5));
      test_assert(w_.lib.reviveAs(kThing, id, "thing", t).ok());
    }
    else if (b->state() == BlockState::Live) {
      test_assert((rng_.below(2) ? w_.lib.markDeleted(h) : w_.lib.onHostDestroyed(id)).ok());
    }
    else if (b->state() == BlockState::Detached) {
      test_assert(w_.lib.onHostRestored(id).ok());
    }
    else {
      test_assert((rng_.below(2) ? w_.lib.restoreFromHistory(h) : w_.lib.destroyFromHistory(h))
                      .ok());
    }
  }
  void setInput()
  {
    Client &c = clients_[rng_.below(uint32_t(clients_.size()))];
    if (!w_.live(w_.defs[size_t(c.def)])) {
      return;
    }
    const auto &list = w_.defs[size_t(c.def)].def->interfaceSockets(SocketSide::Input);
    if (list.size() == 0) {
      return;
    }
    const InterfaceSocket &s = list[int(rng_.below(uint32_t(list.size())))];
    if (rng_.below(5) == 0) {
      test_assert(c.inst->setInput(s.name.c_str(), GraphValue()).ok());
      c.external.erase(s.id);
      return;
    }
    const std::string type = s.typeName.c_str();
    const GraphValue v = type == "r.P" ?
                             GraphValue::ofPayload(makePayload(P{int(rng_.below(4))})) :
                             *GraphValue::fromProp(randomProp(type));
    test_assert(c.inst->setInput(s.name.c_str(), v).ok());
    auto it = c.external.find(s.id);
    if (it == c.external.end() || type == "r.P" || !(it->second.first == toValue(v))) {
      c.external[s.id] = {toValue(v), ++c.externalVersion};
    }
  }
  void rare(Graph &g)
  {
    switch (rng_.below(9)) {
      case 0:
        return w_.registerLateConversion();
      case 1:
        return w_.registerLateType();
      case 2:
      case 5: { /* a group that points anywhere, with no checks */
        struct Args {
          World &w;
          SplitMix64 &rng;
        } args{w_, rng_};
        RefVisitor v(
            [](void *p, RefCell &cell) {
              Args &a = *static_cast<Args *>(p);
              if (a.rng.below(2)) {
                return;
              }
              if (std::any_of(a.w.things.begin(), a.w.things.end(), [&](const BlockId &id) {
                    return id == cell.target;
                  }))
              { /* a Pointer param */
                test_assert(a.w.lib.writeRef(cell, a.w.things[a.rng.below(kThings)]).ok());
              }
              else if (!cell.isNull()) {
                test_assert(a.w.lib.writeRef(cell, a.w.defs[a.rng.below(kDefs)].id).ok());
              }
            },
            &args);
        return g.def->foreachRef(v);
      }
      case 3: {
        Client &c = clients_[rng_.below(uint32_t(clients_.size()))];
        test_assert(c.inst->clear().ok());
        c.nodes.clear();
        c.frameDefs.clear();
        c.last.clear();
        return;
      }
      case 4: {
        Client &c = clients_[rng_.below(uint32_t(clients_.size()))];
        test_assert(c.inst->retry().ok());
        for (auto &[key, n] : c.nodes) {
          n.hasRun = n.hasRun && !n.run.failed;
        }
        return;
      }
      default:
        return;
    }
  }

  struct Removed {
    GraphDef *def;
    SocketSide side;
    InterfaceSocket socket;
  };
  World &w_;
  SplitMix64 &rng_;
  std::vector<Client> &clients_;
  std::vector<Removed> removed_;
};

} // namespace random_graph
