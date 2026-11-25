#pragma once

/* A naive graph evaluator, the oracle of test_graph_random.cc. It evaluates from scratch:
 * it expands the groups reached from the requested sockets into one explicit graph, finds
 * the cycles of that, and computes each vertex once. It shares no code with the engine
 * beyond reading a def's nodes, links and interface. */

#include "graph_fixture.h"

#include "coredcc/graph/instance.h"

#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace naive {

using namespace fixture;

/* A value. A payload is its content, so two kernels' runs can be compared. */
struct Value {
  std::string type;
  std::vector<double> numbers;
  std::string text;
  bool operator==(const Value &) const = default;
};

struct Error {
  StatusCode code = StatusCode::Ok;
  std::vector<int64_t> groups;
  int64_t node = 0;
  std::string socket;
  bool operator==(const Error &) const = default;
};

/* Where an input's value is from, spelled out. */
struct Source {
  std::string text;
  /* A kernel's output, when it is one. */
  int kernel = -1;
  int output = 0;
  std::string chain;
};

struct Outcome {
  bool failed = false;
  Error error;
  Value value;
  Source source;
};

/* What a kernel reads besides its inputs. */
struct BlockRead {
  bool asked = false;
  BlockId id;
  BlockHandle handle;
  BlockState state = BlockState::Live;
  uint64_t stateChanges = 0;
  double value = 0;
  int notified = 0;
  bool seesSame(const BlockRead &b) const
  {
    return asked == b.asked && bool(handle) == bool(b.handle) &&
           (state == BlockState::Live) == (b.state == BlockState::Live) && value == b.value;
  }
  bool operator==(const BlockRead &b) const
  {
    return asked == b.asked && id == b.id && handle == b.handle && state == b.state && stateChanges == b.stateChanges && value == b.value &&
           notified == b.notified;
  }
};

/* The world as the oracle sees it. */
struct Environment {
  Runtime *rt = nullptr;
  Library *lib = nullptr;
  /* What a kernel type computes; the Status code of a failure. */
  using Kernel = std::function<StatusCode(const std::vector<Value> &in,
                                          const std::map<std::string, PropValue> &params,
                                          const BlockRead &block,
                                          std::vector<Value> &out)>;
  std::map<std::string, Kernel> kernels;
  /* Conversions by (from, to). */
  std::map<std::pair<std::string, std::string>, std::function<StatusCode(const Value &, Value &)>>
      conversions;
  /* Socket types, with their defaults. */
  std::map<std::string, Value> types;
  std::map<BlockId, int> notified;
};

struct Frame {
  GraphDef *def = nullptr;
  int parent = -1;
  int64_t groupNode = 0;
  std::vector<int64_t> path;
};

/* An input socket, or a kernel node (an empty key). */
struct Vertex {
  int frame = 0;
  int64_t node = 0;
  std::string key;
  auto tie() const
  {
    return std::tie(frame, node, key);
  }
  bool operator<(const Vertex &b) const
  {
    return tie() < b.tie();
  }
  bool operator==(const Vertex &b) const
  {
    return tie() == b.tie();
  }
};

/* A kernel as this evaluation saw it. */
struct KernelVisit {
  std::vector<int64_t> path;
  int64_t node = 0;
  GraphDef *def = nullptr;
  std::string type;
  bool cycle = false;
  bool unregistered = false;
  bool inputFailed = false;
  bool failed = false;
  Error error;
  std::vector<Outcome> inputs;
  std::vector<Value> outputs;
  std::map<std::string, PropValue> params;
  BlockRead block;
};

class Evaluation {
 public:
  Evaluation(const Environment &env,
             GraphDef *root,
             const std::map<int64_t, std::pair<Value, int>> &external)
      : env_(env), external_(external)
  {
    frames_.push_back(Frame{root, -1, 0, {}});
  }

  /* Evaluates sockets together. NotFound gives no outcome: `found` is false. */
  struct Request {
    SocketPath path;
    bool found = false;
    Outcome outcome;
  };
  /** Whether `run` would find the socket. */
  bool finds(const SocketPath &path)
  {
    Request r{path, false, {}};
    locate(r);
    return r.found;
  }
  void run(std::vector<Request> &requests)
  {
    std::vector<Target> targets;
    for (Request &r : requests) {
      targets.push_back(locate(r));
      if (targets.back().hasVertex) {
        expand(targets.back().vertex);
      }
    }
    findCycles();
    for (size_t i = 0; i < requests.size(); i++) {
      if (!targets[i].hasVertex) {
        continue;
      }
      const Vertex &v = targets[i].vertex;
      if (v.key.empty()) {
        const KernelVisit &k = kernels_[kernel(v)];
        requests[i].outcome.failed = k.failed;
        requests[i].outcome.error = k.error;
        if (!k.failed) {
          requests[i].outcome.value = k.outputs[targets[i].output];
        }
      }
      else {
        requests[i].outcome = input(v);
      }
      requests[i].found = true;
    }
    for (const auto &[v, next] : edges_) {
      if (v.key.empty()) {
        kernel(v); /* everything that was reached */
      }
      else {
        input(v);
      }
    }
  }
  const std::vector<KernelVisit> &kernels() const
  {
    return kernels_;
  }
  /* Frames the evaluation entered, the root first. */
  const std::vector<Frame> &frames() const
  {
    return frames_;
  }

 private:
  struct Target {
    bool hasVertex = false;
    Vertex vertex;
    int output = 0;
  };

  /* ---- sockets, worked out here ---- */

  struct Socket {
    std::string key, type;
  };
  static std::vector<Socket> interfaceList(const GraphDef &def, SocketSide side)
  {
    std::vector<Socket> out;
    for (const InterfaceSocket &s : def.interfaceSockets(side)) {
      out.push_back({"#" + std::to_string(s.id), s.typeName.c_str()});
    }
    return out;
  }
  GraphDef *target(const GraphNode &n) const
  {
    const Datablock *b = env_.lib->get(env_.lib->find(n.group().target));
    return b && b->state() == BlockState::Live ? static_cast<GraphDef *>(b->clientData()) :
                                                 nullptr;
  }
  static std::string typeName(const GraphNode &n)
  {
    return n.typeName().c_str();
  }
  bool registered(const GraphNode &n) const
  {
    return env_.rt->ext<NodeTypeRegistry>().find(typeName(n)) != nullptr;
  }
  static bool isKernel(const GraphNode &n)
  {
    const std::string t = typeName(n);
    return t != "graph.input" && t != "graph.output" && t != "graph.group";
  }
  std::vector<Socket> sockets(const GraphDef &def, const GraphNode &n, SocketSide side) const
  {
    const std::string t = typeName(n);
    if (t == "graph.input") {
      return side == SocketSide::Output ? interfaceList(def, SocketSide::Input) :
                                          std::vector<Socket>();
    }
    if (t == "graph.output") {
      return side == SocketSide::Input ? interfaceList(def, SocketSide::Output) :
                                         std::vector<Socket>();
    }
    std::vector<Socket> out;
    if (t == "graph.group") {
      if (const GraphDef *d = target(n)) {
        return interfaceList(*d, side);
      }
    }
    else if (const NodeType *type = env_.rt->ext<NodeTypeRegistry>().find(t)) {
      if (side == SocketSide::Input) {
        for (const NodeInputDef &d : type->inputs) {
          out.push_back({d.name.c_str(), d.type->name.c_str()});
        }
      }
      else {
        for (const NodeOutputDef &d : type->outputs) {
          out.push_back({d.name.c_str(), d.type->name.c_str()});
        }
      }
      return out;
    }
    for (const SocketInfo &s : def.sockets(n, side)) { /* only the snapshot is left */
      out.push_back({s.key.c_str(), s.typeName.c_str()});
    }
    return out;
  }
  static int indexOf(const std::vector<Socket> &list, const std::string &key)
  {
    for (size_t i = 0; i < list.size(); i++) {
      if (list[i].key == key) {
        return int(i);
      }
    }
    return -1;
  }
  static const GraphNode *outputNode(const GraphDef &def)
  {
    const GraphNode *out = nullptr;
    def.forEachNode([&](const GraphNode &n) {
      if (typeName(n) == "graph.output") {
        out = &n;
      }
    });
    return out;
  }

  /* ---- frames ---- */

  struct Child {
    int frame = -1;
    bool failed = false;
    Error error;
  };
  Child child(int frame, const GraphNode &group)
  {
    const auto key = std::make_pair(frame, group.id().value);
    if (auto it = children_.find(key); it != children_.end()) {
      return it->second;
    }
    Child c;
    GraphDef *def = target(group);
    auto fail = [&](StatusCode code) {
      c.failed = true;
      c.error = Error{code, frames_[frame].path, group.id().value, ""};
    };
    if (!def) {
      fail(StatusCode::NotFound);
    }
    else {
      for (int f = frame; f >= 0; f = frames_[f].parent) {
        if (frames_[f].def == def) {
          fail(StatusCode::FailedPrecondition);
        }
      }
      if (!c.failed && frames_[frame].path.size() + 1 > 64) {
        fail(StatusCode::OutOfRange);
      }
    }
    if (!c.failed) {
      Frame n{def, frame, group.id().value, frames_[frame].path};
      n.path.push_back(group.id().value);
      c.frame = int(frames_.size());
      frames_.push_back(n);
    }
    children_[key] = c;
    return c;
  }

  /* ---- what feeds a vertex ---- */

  struct Feed {
    enum Kind { None, Literal, External, TypeDefault, Failed, FromVertex } kind = None;
    Vertex from;
    int output = 0;
    std::string type;
    int interfaceIndex = 0;
    Error error;
  };
  Feed outputFeed(int frame, const GraphNode &n, const std::string &key)
  {
    Feed feed;
    const GraphDef &def = *frames_[frame].def;
    const std::vector<Socket> outs = sockets(def, n, SocketSide::Output);
    const int o = indexOf(outs, key);
    feed.type = outs[o].type;
    feed.output = o;
    if (isKernel(n)) {
      feed.kind = Feed::FromVertex;
      feed.from = Vertex{frame, n.id().value, ""};
    }
    else if (typeName(n) == "graph.input") {
      if (frames_[frame].parent >= 0) {
        feed.kind = Feed::FromVertex;
        feed.from = Vertex{frames_[frame].parent, frames_[frame].groupNode, key};
      }
      else {
        feed.kind = Feed::External;
        feed.interfaceIndex = o;
      }
    }
    else {
      const Child c = child(frame, n);
      if (c.failed) {
        feed.kind = Feed::Failed;
        feed.error = c.error;
      }
      else if (const GraphNode *out = outputNode(*frames_[c.frame].def)) {
        feed.kind = Feed::FromVertex;
        feed.from = Vertex{c.frame, out->id().value, key};
      }
      else {
        feed.kind = Feed::TypeDefault;
      }
    }
    return feed;
  }
  Feed feedOf(const Vertex &v)
  {
    const GraphDef &def = *frames_[v.frame].def;
    const GraphNode &n = *def.node(NodeId{v.node});
    const std::vector<Socket> ins = sockets(def, n, SocketSide::Input);
    const std::string type = ins[indexOf(ins, v.key)].type;
    Feed feed;
    for (const GraphLink *l : n.linksIn()) {
      if (v.key == l->toKey.c_str() &&
          indexOf(sockets(def, *l->from, SocketSide::Output), l->fromKey.c_str()) >= 0)
      {
        return outputFeed(v.frame, *l->from, l->fromKey.c_str());
      }
    }
    feed.kind = Feed::Literal;
    feed.type = type;
    return feed;
  }

  Target locate(Request &r)
  {
    Target t;
    int frame = 0;
    const GraphDef *def = frames_[0].def;
    auto fromFeed = [&](const Feed &feed) {
      r.found = true;
      if (feed.kind == Feed::FromVertex) {
        t.hasVertex = true;
        t.vertex = feed.from;
        t.output = feed.output;
      }
      else if (feed.kind == Feed::Failed) {
        r.outcome.failed = true;
        r.outcome.error = feed.error;
      }
      else {
        r.outcome = constant(feed, 0, r.path.node.node.value);
      }
      return t;
    };
    if (r.path.node.node.isNull()) {
      const std::vector<Socket> outs = interfaceList(*def, SocketSide::Output);
      for (size_t i = 0; i < outs.size(); i++) {
        const InterfaceSocket &s = def->interfaceSockets(SocketSide::Output)[int(i)];
        if (outs[i].key == r.path.key.c_str() || std::string(s.name.c_str()) == r.path.key.c_str()) {
          Feed feed;
          feed.type = outs[i].type;
          feed.kind = Feed::TypeDefault;
          if (const GraphNode *o = outputNode(*def)) {
            feed.kind = Feed::FromVertex;
            feed.from = Vertex{0, o->id().value, outs[i].key};
          }
          return fromFeed(feed);
        }
      }
      return t;
    }
    for (NodeId id : r.path.node.groups) {
      const GraphNode *g = frames_[frame].def->node(id);
      if (!g || typeName(*g) != "graph.group") {
        return t;
      }
      const Child c = child(frame, *g);
      if (c.failed) {
        r.found = true;
        r.outcome.failed = true;
        r.outcome.error = c.error;
        return t;
      }
      frame = c.frame;
    }
    def = frames_[frame].def;
    const GraphNode *n = def->node(r.path.node.node);
    if (!n || indexOf(sockets(*def, *n, r.path.side), r.path.key.c_str()) < 0) {
      return t;
    }
    if (r.path.side == SocketSide::Output) {
      return fromFeed(outputFeed(frame, *n, r.path.key.c_str()));
    }
    r.found = true;
    t.hasVertex = true;
    t.vertex = Vertex{frame, n->id().value, r.path.key.c_str()};
    return t;
  }

  /* ---- the expanded graph and its cycles ---- */

  void expand(const Vertex &start)
  {
    std::vector<Vertex> todo{start};
    while (!todo.empty()) {
      const Vertex v = todo.back();
      todo.pop_back();
      if (edges_.count(v)) {
        continue;
      }
      std::vector<Vertex> next;
      const GraphDef &def = *frames_[v.frame].def;
      const GraphNode &n = *def.node(NodeId{v.node});
      if (!v.key.empty()) {
        const Feed feed = feedOf(v);
        if (feed.kind == Feed::FromVertex) {
          next.push_back(feed.from);
        }
      }
      else if (registered(n)) {
        for (const Socket &s : sockets(def, n, SocketSide::Input)) {
          next.push_back(Vertex{v.frame, v.node, s.key});
        }
      }
      for (const Vertex &u : next) {
        back_[u].push_back(v);
        todo.push_back(u);
      }
      edges_[v] = next;
    }
  }

  /* Kosaraju: finish order of a walk along the edges, then components against them. */
  void findCycles()
  {
    std::vector<Vertex> order;
    std::set<Vertex> seen;
    for (const auto &[start, unused] : edges_) {
      std::vector<std::pair<Vertex, size_t>> stack;
      if (seen.insert(start).second) {
        stack.push_back({start, 0});
      }
      while (!stack.empty()) {
        auto &[v, i] = stack.back();
        const std::vector<Vertex> &next = edges_[v];
        if (i < next.size()) {
          const Vertex u = next[i++];
          if (seen.insert(u).second) {
            stack.push_back({u, 0});
          }
          continue;
        }
        order.push_back(v);
        stack.pop_back();
      }
    }
    std::set<Vertex> taken;
    for (size_t k = order.size(); k-- > 0;) {
      if (!taken.insert(order[k]).second) {
        continue;
      }
      std::vector<Vertex> members{order[k]};
      for (size_t i = 0; i < members.size(); i++) {
        for (const Vertex &u : back_[members[i]]) {
          if (taken.insert(u).second) {
            members.push_back(u);
          }
        }
      }
      if (members.size() < 2) {
        continue;
      }
      Error e;
      bool first = true;
      for (const Vertex &v : members) {
        if (v.key.empty()) {
          continue;
        }
        const Error at{StatusCode::FailedPrecondition, frames_[v.frame].path, v.node, v.key};
        if (first || before(at, e)) {
          e = at;
          first = false;
        }
      }
      for (const Vertex &v : members) {
        cycles_[v] = e;
      }
    }
  }
  static bool before(const Error &a, const Error &b)
  {
    return std::tie(a.groups, a.node, a.socket) < std::tie(b.groups, b.node, b.socket);
  }

  /* ---- values ---- */

  bool fits(const Value &v, const std::string &type) const
  {
    return v.type == type;
  }
  static Value fromProp(const PropValue &p)
  {
    Value v;
    switch (p.type()) {
      case PropValue::Type::Bool:
        v = Value{"bool", {p.asBool() ? 1.0 : 0.0}, ""};
        break;
      case PropValue::Type::Int:
        v = Value{"int", {double(p.asInt())}, ""};
        break;
      case PropValue::Type::Float:
        v = Value{"float", {p.asFloat()}, ""};
        break;
      case PropValue::Type::Vec3:
        v = Value{"vec3", {p.component(0), p.component(1), p.component(2)}, ""};
        break;
      case PropValue::Type::String:
        v = Value{"string", {}, p.asString().c_str()};
        break;
      default:
        break;
    }
    return v;
  }
  Outcome constant(const Feed &feed, int frame, int64_t node)
  {
    Outcome o;
    o.value = env_.types.at(feed.type);
    o.source.text = "type default " + feed.type;
    o.source.chain = feed.type;
    (void)frame;
    (void)node;
    if (feed.kind == Feed::External) {
      const InterfaceSocket &s =
          frames_[0].def->interfaceSockets(SocketSide::Input)[feed.interfaceIndex];
      auto it = external_.find(s.id);
      if (it != external_.end() && it->second.first.type == feed.type) {
        o.value = it->second.first;
        o.source.text =
            "external " + std::to_string(s.id) + " v" + std::to_string(it->second.second);
        return o;
      }
      interfaceDefault(s, feed.type, *frames_[0].def, o);
    }
    return o;
  }
  void interfaceDefault(const InterfaceSocket &s,
                        const std::string &type,
                        const GraphDef &def,
                        Outcome &o) const
  {
    const Value v = fromProp(s.defaultValue);
    if (v.type == type) {
      o.value = v;
      o.source.text = "default of " + std::to_string(def.serial()) + " #" +
                      std::to_string(s.id) + " v" + std::to_string(s.defaultVersion);
    }
  }
  Outcome literal(const Vertex &v, const std::string &type)
  {
    const GraphDef &def = *frames_[v.frame].def;
    const GraphNode &n = *def.node(NodeId{v.node});
    Outcome o;
    o.value = env_.types.at(type);
    o.source.text = "type default " + type;
    o.source.chain = type;
    for (const NodeValue &l : n.literals()) {
      if (v.key == l.name.c_str() && !l.off && fromProp(l.value).type == type) {
        o.value = fromProp(l.value);
        o.source.text = "literal " + pathText(v.frame) + std::to_string(v.node) + "." + v.key +
                        " v" + std::to_string(l.version);
        return o;
      }
    }
    if (isKernel(n) && registered(n)) {
      const NodeType *t = env_.rt->ext<NodeTypeRegistry>().find(typeName(n));
      const int i = indexOf(sockets(def, n, SocketSide::Input), v.key);
      if (t->literal(i)) {
        o.value = fromProp(t->literal(i)->defaultValue());
      }
      o.source.text = "default of " + typeName(n) + "." + v.key;
    }
    else if (typeName(n) == "graph.group") {
      if (const GraphDef *d = target(n)) {
        const int i = indexOf(interfaceList(*d, SocketSide::Input), v.key);
        interfaceDefault(d->interfaceSockets(SocketSide::Input)[i], type, *d, o);
      }
    }
    return o;
  }
  std::string pathText(int frame) const
  {
    std::string s;
    for (int64_t id : frames_[frame].path) {
      s += std::to_string(id) + "/";
    }
    return s;
  }

  Outcome input(const Vertex &v)
  {
    if (auto it = inputs_.find(v); it != inputs_.end()) {
      return it->second;
    }
    Outcome o;
    if (auto it = cycles_.find(v); it != cycles_.end()) {
      o.failed = true;
      o.error = it->second;
      return inputs_[v] = o;
    }
    const GraphDef &def = *frames_[v.frame].def;
    const GraphNode &n = *def.node(NodeId{v.node});
    const std::vector<Socket> ins = sockets(def, n, SocketSide::Input);
    const std::string type = ins[indexOf(ins, v.key)].type;
    const Feed feed = feedOf(v);
    auto fail = [&](StatusCode code) {
      o = Outcome();
      o.failed = true;
      o.error = Error{code, frames_[v.frame].path, v.node, v.key};
    };
    if (feed.kind == Feed::Failed) {
      o.failed = true;
      o.error = feed.error;
    }
    else if (feed.kind == Feed::FromVertex && feed.from.key.empty()) {
      const int k = kernel(feed.from);
      if (kernels_[k].failed) {
        o.failed = true;
        o.error = kernels_[k].error;
      }
      else {
        o.value = kernels_[k].outputs[feed.output];
        o.source.text = "kernel " + pathText(feed.from.frame) + std::to_string(feed.from.node) +
                        " " + std::to_string(feed.output);
        o.source.kernel = k;
        o.source.output = feed.output;
        o.source.chain = feed.type;
      }
    }
    else if (feed.kind == Feed::FromVertex) {
      o = input(feed.from);
    }
    else if (!env_.types.count(feed.type)) {
      fail(StatusCode::NotFound);
    }
    else if (feed.kind == Feed::Literal) {
      o = literal(v, type);
    }
    else {
      o = constant(feed, v.frame, v.node);
    }
    if (!o.failed && (!env_.types.count(feed.type) || !env_.types.count(type))) {
      fail(StatusCode::NotFound);
    }
    if (!o.failed && feed.type != type) {
      auto it = env_.conversions.find({feed.type, type});
      Value out;
      const StatusCode code =
          it == env_.conversions.end() ? StatusCode::TypeMismatch : it->second(o.value, out);
      if (code != StatusCode::Ok) {
        fail(code);
      }
      else {
        o.value = out;
        o.source.chain += ">" + type;
      }
    }
    return inputs_[v] = o;
  }

  int kernel(const Vertex &v)
  {
    if (auto it = kernelIndex_.find(v); it != kernelIndex_.end()) {
      return it->second;
    }
    const GraphDef &def = *frames_[v.frame].def;
    const GraphNode &n = *def.node(NodeId{v.node});
    KernelVisit k;
    k.path = frames_[v.frame].path;
    k.node = v.node;
    k.def = frames_[v.frame].def;
    k.type = typeName(n);
    if (auto it = cycles_.find(v); it != cycles_.end()) {
      k.cycle = k.failed = true;
      k.error = it->second;
    }
    else if (!registered(n)) {
      k.unregistered = k.failed = true;
      k.error = Error{StatusCode::NotFound, k.path, k.node, ""};
    }
    else {
      std::vector<Value> in;
      for (const Socket &s : sockets(def, n, SocketSide::Input)) {
        k.inputs.push_back(input(Vertex{v.frame, v.node, s.key}));
        in.push_back(k.inputs.back().value);
        if (k.inputs.back().failed && !k.failed) {
          k.inputFailed = k.failed = true;
          k.error = k.inputs.back().error;
        }
      }
      const NodeType *t = env_.rt->ext<NodeTypeRegistry>().find(k.type);
      for (const NodeParamDef &p : t->params) {
        const std::string name = p.def.apiName().c_str();
        k.params[name] = p.def.defaultValue();
        for (const NodeValue &set : n.params()) {
          if (name == set.name.c_str()) {
            k.params[name] = set.value;
          }
        }
        for (const NodeBlockRef &r : n.blockRefs()) {
          if (name == r.name.c_str() && !r.cell.isNull()) {
            k.params[name] = PropValue();
            k.block = readBlock(r.cell.target);
          }
        }
      }
      if (!k.failed) {
        const StatusCode code = env_.kernels.at(k.type)(in, k.params, k.block, k.outputs);
        if (code != StatusCode::Ok) {
          k.failed = true;
          k.error = Error{code, k.path, k.node, ""};
        }
      }
    }
    kernels_.push_back(k);
    return kernelIndex_[v] = int(kernels_.size()) - 1;
  }
  BlockRead readBlock(BlockId id) const
  {
    BlockRead r;
    r.asked = true;
    r.id = id;
    r.handle = env_.lib->find(id);
    if (const Datablock *b = env_.lib->get(r.handle)) {
      r.state = b->state();
      r.stateChanges = b->stateChanges();
      r.value = r.state == BlockState::Live ? static_cast<Thing *>(b->clientData())->value : 0;
    }
    auto it = env_.notified.find(id);
    r.notified = it == env_.notified.end() ? 0 : it->second;
    return r;
  }

  const Environment &env_;
  const std::map<int64_t, std::pair<Value, int>> &external_;
  std::vector<Frame> frames_;
  std::map<std::pair<int, int64_t>, Child> children_;
  std::map<Vertex, std::vector<Vertex>> edges_;
  std::map<Vertex, std::vector<Vertex>> back_;
  std::map<Vertex, Error> cycles_;
  std::map<Vertex, Outcome> inputs_;
  std::map<Vertex, int> kernelIndex_;
  std::vector<KernelVisit> kernels_;
};

} // namespace naive
