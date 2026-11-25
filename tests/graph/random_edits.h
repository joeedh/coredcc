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
      return outs.size() == 0 || rng_.below(20) == 0
                 ? SocketPath::output("missing")
                 : SocketPath::output(
                       outs[int(rng_.below(uint32_t(outs.size())))].name.c_str());
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
      return SocketPath::of(
          path, list[int(rng_.below(uint32_t(list.size())))].key.c_str(), side);
    }
    return SocketPath::of(NodeId{1}, "missing");
  }

  /* Only edits that instances should be able to follow without a full pass. */
  bool quiet = false;

  void quietEdit()
  {
    Graph &g = w_.defs[rng_.below(kDefs)];
    if (!w_.live(g)) {
      return;
    }
    switch (rng_.below(10)) {
    case 0:
      return addNode(*g.def);
    case 1:
    case 2:
      return addLink(*g.def, rng_.below(8) == 0);
    case 3:
      return unlink(*g.def);
    case 4:
      return setParam(*g.def);
    case 5:
    case 6:
      return setLiteral(*g.def);
    case 7:
      return setInput();
    default:
      return editThing();
    }
  }

  void edit()
  {
    g_counts.ops++;
    if (quiet) {
      return quietEdit();
    }
    if (w_.undoable && rng_.below(5) == 0) {
      const bool undo = rng_.below(5) > 1;
      for (int n = 1 + int(rng_.below(3)); n > 0; n--) {
        if (undo ? w_.history.canUndo() : w_.history.canRedo()) {
          step(undo);
        }
      }
      return;
    }
    if (w_.undoable && rng_.below(12) == 0) {
      reviveDefs();
    }
    size_t at = rng_.below(kDefs);
    while (w_.undoable && !w_.live(w_.defs[at])) { /* commands only edit Live blocks */
      at = (at + 1) % kDefs;
    }
    Graph &g = w_.defs[at];
    switch (rng_.below(35)) {
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
    case 33:
      return setExtra(*g.def);
    default:
      return removeAndRevive(*g.def);
    }
  }

  /* Undoes until no def is deleted. */
  void reviveDefs()
  {
    auto dead = [this](const Graph &g) { return !w_.live(g); };
    while (w_.undoable && std::any_of(w_.defs.begin(), w_.defs.end(), dead)) {
      step(true);
    }
  }
  void step(bool undo)
  {
    (undo ? g_counts.undos : g_counts.redos)++;
    const Status s = undo ? w_.executor.undo(*w_.ctx) : w_.executor.redo(*w_.ctx);
    if (!s.ok()) {
      printf("seed %llu, batch %d: %s failed: %s\n",
             (unsigned long long)g_seed,
             g_batch,
             undo ? "undo" : "redo",
             s.toString().c_str());
    }
    test_assert(s.ok());
  }

private:
  /* `fn`, generic, on the def itself or on an edit of it that can be undone. */
  template <class Fn> Status act(GraphDef &def, Fn fn)
  {
    return w_.undoable
               ? runGraphEdit(
                     *w_.ctx, def.self(), "edit", [&fn](GraphEdit &e) { return fn(e); })
               : fn(def);
  }
  std::string graphPath(const GraphDef &def)
  {
    return "graphs[\"" +
           std::string(w_.lib.get(w_.lib.find(def.self()))->name().c_str()) + "\"]";
  }
  std::string path(const GraphDef &def, NodeId n)
  {
    return graphPath(def) + ".nodes[#" + std::to_string(n.value) + "]";
  }
  void setLiteralPath(GraphDef &def,
                      const GraphNode &n,
                      const std::string &key,
                      const PropValue &v)
  {
    const std::string node = path(def, n.id());
    if (n.type() && n.type()->role == NodeRole::Kernel) {
      setValueUndo(*w_.ctx, node + ".inputs." + key, v).ignore(); /* a payload has none */
      return;
    }
    const std::string socket = node + ".sockets[#" + key.substr(1) + "]";
    if (rng_.below(2)) {
      setValueUndo(*w_.ctx, socket + ".isSet", PropValue::ofBool(!v.isNone())).ignore();
    }
    setValueUndo(*w_.ctx, socket + ".value", v).ignore(); /* read-only unless set */
  }

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
  bool
  randomKey(const GraphDef &def, const GraphNode &n, SocketSide side, std::string &out)
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
    const char *types[] = {
        "float", "float", "float", "int", "bool", "vec3", "string", "r.P"};
    return types[rng_.below(8)];
  }

  void addNode(GraphDef &def)
  {
    if (def.nodeCount() > 14) {
      return removeNode(def);
    }
    const char *types[] = {"r.sum",
                           "r.sum",
                           "r.sum",
                           "r.split",
                           "r.block",
                           "r.vec",
                           "r.text",
                           "r.pay",
                           "r.count",
                           "r.makeVec",
                           "r.sum",
                           "r.late",
                           "graph.input",
                           "graph.output",
                           "graph.group",
                           "graph.group"};
    const std::string type = types[rng_.below(16)];
    if (type != "r.late" || w_.late) {
      act(def, [&](auto &e) { return e.addNode(type).status(); }).ignore();
      return;
    }
    NodeState s;
    s.id = NodeId{int64_t(1 + rng_.below(1 << 30))};
    s.typeName = "r.late";
    s.inputs.append(SocketInfo{"a", "a", "float"});
    s.outputs.append(SocketInfo{"value", "value", "float"});
    s.outputs.append(SocketInfo{"z", "z", "r.Q"});
    act(def, [&](auto &e) { return e.insertNode(s); }).ignore();
  }
  void removeNode(GraphDef &def)
  {
    if (const GraphNode *n = randomNode(def)) {
      const NodeId id = n->id();
      test_assert(act(def, [&](auto &e) { return e.removeNode(id); }).ok());
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
    test_assert(act(def, [&](auto &e) {
                  CDCC_TRY(e.removeNode(state.id));
                  const bool ok =
                      e.insertNode(state).ok(); /* a recursive group is refused */
                  for (const LinkState &l : links) {
                    test_assert(e.insertLink(l).ok() || !ok);
                  }
                  return Status();
                }).ok());
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
      auto link = [&](auto &e) {
        return e.link({a->id(), from}, {b->id(), to}).status();
      };
      for (int i = 0; i < 4 && !act(def, link).ok(); i++) {
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
    act(def, [&](auto &e) { return e.insertLink(l); }).ignore();
  }
  void unlink(GraphDef &def)
  {
    const GraphNode *n = randomNode(def);
    if (n && n->linksIn().size() > 0) {
      const LinkId id = n->linksIn()[0]->id;
      test_assert(act(def, [&](auto &e) { return e.unlink(id); }).ok());
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
    } else if (rng_.below(6)) {
      v = p.kind() == PropKind::Int ? PropValue::ofInt(int(rng_.below(6)) - 2)
                                    : randomProp("string");
    }
    const NodeId id = n->id();
    const std::string name = p.apiName().c_str();
    if (w_.undoable && rng_.below(2) && !v.isNone()) {
      setValueUndo(*w_.ctx, path(def, id) + ".params." + name, v)
          .ignore(); /* a dead target */
      return;
    }
    test_assert(act(def, [&](auto &e) { return e.setParam(id, name, v); }).ok());
  }
  void setExtra(GraphDef &def)
  {
    if (const GraphNode *n = randomNode(def)) {
      const NodeId id = n->id();
      const std::string name = rng_.below(2) ? "ui.a" : "ui.b";
      const PropValue v = rng_.below(4) ? randomProp("string") : PropValue();
      test_assert(act(def, [&](auto &e) { return e.setExtra(id, name, v); }).ok());
    }
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
        const NodeId id = n->id();
        const PropValue v = rng_.below(6) ? randomProp(s.typeName.c_str()) : PropValue();
        if (w_.undoable && rng_.below(2)) {
          return setLiteralPath(def, *n, key, v);
        }
        act(def, [&](auto &e) { return e.setLiteral(id, key, v); }).ignore();
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
    const std::string type = randomType();
    const int at = int(rng_.below(uint32_t(n + 1)));
    test_assert(act(def, [&](auto &e) {
                  return e.addInterface(side, name, type, at).status();
                }).ok());
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
    const int64_t id = s->id;
    test_assert(act(def, [&](auto &e) { return e.removeInterface(id); }).ok());
  }
  void editInterface(GraphDef &def)
  {
    SocketSide side;
    const InterfaceSocket *s = randomInterface(def, side);
    if (rng_.below(3) == 0 && !removed_.empty() && !w_.undoable) {
      const Removed r = removed_.back();
      removed_.pop_back();
      r.def
          ->addInterface(
              r.side, r.socket.name.c_str(), r.socket.typeName.c_str(), 0, r.socket.id)
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
    case 1: {
      const int to = int(rng_.below(uint32_t(def.interfaceSockets(side).size())));
      test_assert(act(def, [&](auto &e) { return e.moveInterface(id, to); }).ok());
      return;
    }
    case 2: {
      const std::string type = randomType();
      test_assert(act(def, [&](auto &e) { return e.setInterfaceType(id, type); }).ok());
      return;
    }
    case 3: {
      const std::string name = "s" + std::to_string(w_.names++);
      test_assert(act(def, [&](auto &e) { return e.setInterfaceName(id, name); }).ok());
      return;
    }
    default: {
      const PropValue v = rng_.below(5) ? randomProp(s->typeName.c_str()) : PropValue();
      if (w_.undoable && side == SocketSide::Input && rng_.below(2) && !v.isNone()) {
        setValueUndo(
            *w_.ctx, graphPath(def) + ".inputs[#" + std::to_string(id) + "].default", v)
            .ignore(); /* a payload has none */
        return;
      }
      act(def, [&](auto &e) { return e.setInterfaceDefault(id, v); }).ignore();
      return;
    }
    }
  }
  void setGroup(GraphDef &def)
  {
    NodeId id;
    int seen = 0;
    def.forEachNode([&](const GraphNode &n) {
      if (strcmp(n.typeName().c_str(), "graph.group") == 0 &&
          rng_.below(uint32_t(++seen)) == 0)
      {
        id = n.id();
      }
    });
    if (id) {
      const BlockId to = rng_.below(8) ? w_.defs[rng_.below(kDefs)].id : BlockId();
      if (w_.undoable && rng_.below(2)) {
        setValueUndo(*w_.ctx,
                     path(def, id) + ".group",
                     to.isNull() ? PropValue::nullPointer()
                                 : pointerTo(to, StructId::of(kGraphDefType)))
            .ignore();
        return;
      }
      act(def, [&](auto &e) {
        return e.setGroup(id, to);
      }).ignore(); /* recursion is refused */
    }
  }
  void toggleDef(Graph &g)
  {
    Graph *dead = nullptr;
    for (Graph &d : w_.defs) {
      dead = w_.live(d) ? dead : &d;
    }
    if (w_.undoable) {
      reviveDefs();
      if (!dead && rng_.below(3) == 0) {
        const CommandDef *del = w_.rt.ext<CommandRegistry>().find(kDeleteBlockCommand);
        PropertyBag in = del->newInputs();
        test_assert(in.set("block", pointerTo(g.id, StructId::of(kGraphDefType))).ok());
        test_assert(w_.executor.run(*w_.ctx, *del, std::move(in)).ok());
      }
    } else if (dead) {
      test_assert(w_.lib.restoreFromHistory(dead->h).ok());
    } else if (rng_.below(3) == 0) {
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
    } else if (b->state() == BlockState::Live) {
      test_assert(
          (rng_.below(2) ? w_.lib.markDeleted(h) : w_.lib.onHostDestroyed(id)).ok());
    } else if (b->state() == BlockState::Detached) {
      test_assert(w_.lib.onHostRestored(id).ok());
    } else {
      test_assert(
          (rng_.below(2) ? w_.lib.restoreFromHistory(h) : w_.lib.destroyFromHistory(h))
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
      test_assert(c.batch->setInput(s.name.c_str(), GraphValue()).ok());
      c.external.erase(s.id);
      return;
    }
    const std::string type = s.typeName.c_str();
    const GraphValue v = type == "r.P"
                             ? GraphValue::ofPayload(makePayload(P{int(rng_.below(4))}))
                             : *GraphValue::fromProp(randomProp(type));
    test_assert(c.inst->setInput(s.name.c_str(), v).ok());
    test_assert(c.batch->setInput(s.name.c_str(), v).ok());
    c.given[s.id] = v;
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
    case 5: {            /* a group that points anywhere, with no checks */
      if (w_.undoable) { /* which no command can undo */
        test_assert(w_.lib.collect().ok());
        return;
      }
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
                })) { /* a Pointer param */
              test_assert(a.w.lib.writeRef(cell, a.w.things[a.rng.below(kThings)]).ok());
            } else if (!cell.isNull()) {
              test_assert(a.w.lib.writeRef(cell, a.w.defs[a.rng.below(kDefs)].id).ok());
            }
          },
          &args);
      return g.def->foreachRef(v);
    }
    case 3: {
      Client &c = clients_[rng_.below(uint32_t(clients_.size()))];
      test_assert(c.inst->clear().ok() && c.batch->clear().ok());
      c.nodes.clear();
      c.frameDefs.clear();
      c.last.clear();
      for (Client::Prepared &p : c.prepared) {
        p.evaluated = false;
        p.changedAt = 0;
      }
      /* Inputs that the interface doesn't have go too. */
      const Graph &root = w_.defs[size_t(c.def)];
      for (auto it = c.external.begin(); w_.live(root) && it != c.external.end();) {
        it = root.def->interfaceSocket(it->first) ? std::next(it) : c.external.erase(it);
      }
      return;
    }
    case 4: {
      Client &c = clients_[rng_.below(uint32_t(clients_.size()))];
      test_assert(c.inst->retry().ok() && c.batch->retry().ok());
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
