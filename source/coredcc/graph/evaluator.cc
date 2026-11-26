/* The walk: which vertices a request needs, in which order, and what each one gives. */
#include "coredcc/graph/evaluator.h"

#include "coredcc/base/assert.h"
#include "coredcc/graph/socket_type.h"
#include "coredcc/graph/system.h"
#include "coredcc/lib/library.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <string>

namespace coredcc::graph {

using litestl::util::string;
using litestl::util::Vector;

Status unknownType()
{
  return Status::error(StatusCode::NotFound, "a socket's type isn't registered");
}

namespace {

bool sameError(const ErrorPtr &a, const ErrorPtr &b)
{
  return a == b || (a && b && *a == *b);
}

/* Whether a kernel was stopped, or gave Cancelled itself. (A conversion's is an error
 * like others.) */
bool isCancelled(const GraphError &e)
{
  return e.status.code() == StatusCode::Cancelled && e.socket.size() == 0;
}

} // namespace

/** For the kernel of one node. */
class Evaluator::Monitor final : public ExecMonitor {
public:
  Monitor(Evaluator &e, const Frame &f, NodeId node) : e_(e), f_(f), node_(node)
  {
  }
  bool cancelled() override
  {
    return e_.cancelled();
  }
  void progress(double fraction, std::string_view text) override
  {
    if (!e_.monitor_.progress) {
      return;
    }
    GraphProgress p;
    p.node = f_.pathOf(node_);
    for (const Frame *f = &f_; f; f = f->parent) {
      if (f->element) {
        p.node.elements.append(f->element->key);
      }
    }
    std::reverse(p.node.elements.begin(), p.node.elements.end());
    p.fraction = fraction;
    p.text = text;
    e_.monitor_.progress(p);
  }

private:
  Evaluator &e_;
  const Frame &f_;
  NodeId node_;
};

bool Evaluator::cancelled()
{
  cancelled_ = cancelled_ || (monitor_.cancelled && monitor_.cancelled());
  return cancelled_;
}

Evaluator::Evaluator(Runtime *rt, Library *lib, GraphSource source)
    : rt_(*rt), lib_(libraryRef(!lib && source.def ? source.def->library() : lib)),
      source_(source), blocks_(*rt, lib_)
{
}

Evaluator::~Evaluator()
{
  CDCC_CHECK(!running_, "a GraphInstance was destroyed while it evaluates");
  /* The last pin of a block may go. It is destroyed after everything here is. */
  NoCollectScope noCollect(lib_.get());
  root_ = nullptr;
  results_.clear();
  prepared_.clear();
  inputs_ = {};
}

Evaluator::Key Evaluator::currentKey() const
{
  return Key{GraphPassKey::of(rt_, lib_.get()), own_};
}

Status Evaluator::begin()
{
  CDCC_TRY(rt_.busyIfEvaluating("GraphInstance::evaluate"));
  if (rt_.hasStickyError()) {
    return Status::error(
        StatusCode::FailedPrecondition,
        "GraphInstance::evaluate: a sticky error is pending; take it first");
  }
  if (lib_.get() && lib_.get()->busy()) {
    return Status::error(StatusCode::Busy,
                         "GraphInstance::evaluate: the library is collecting");
  }
  return Status();
}

Status Evaluator::prepareCall()
{
  CDCC_TRY(prepareRoot());
  const Key key = currentKey();
  if (!untouched(key)) {
    if (!absorb(key)) {
      pass_ = ++passes_;
      fullPasses++;
      layout_++;
      pruneResults();
    }
    ownInputs_.clear();
    ownAll_ = false;
    keyMoves_++;
  }
  passKey_ = key;
  seen_ = rt_.ext<GraphSystem>().journalEnd();
  numbers_ = 0;
  call_++;
  overBudget_ = false;
  cancelled_ = false;
  running_ = true;
  pinDef(*root_);
  return Status();
}

Status Evaluator::endCall()
{
  running_ = false;
  pins_.clear();
  if (!overBudget_) {
    /* To be looked at again, with all that depends on them, though nothing moved. */
    for (const Vertex &v : cancelledAt_) {
      markStale(v);
    }
    cancelledAt_.clear();
    return Status();
  }
  cancelledAt_.clear();
  failedKey_ = passKey_;
  failed_ = Status::error(StatusCode::OutOfRange,
                          ("the graph needs more than " + std::to_string(maxFrames) +
                           " groups, at node " + overBudgetAt_)
                              .c_str());
  dropNewFrames();
  dropPass();
  return failed_;
}

void Evaluator::pull(const SocketPath &path, Pulled &p, Status &status)
{
  Request r;
  if (p.layout == layout_) {
    r.vertex = p.vertex;
    r.output = p.output;
    if (walkState(r.vertex).pass != pass_) {
      for (Frame *f = r.vertex.frame; f; f = f->parent) {
        pinDef(*f); /* as the way to it would have */
      }
    }
  } else {
    r = find(path);
    if (overBudget_) {
      return;
    }
    p.layout = r.done ? 0 : layout_;
    p.vertex = r.vertex;
    p.output = r.output;
  }
  if (r.done || walk(r.vertex)) {
    store(p, r, status);
  }
}

Status Evaluator::evaluate(std::span<const SocketPath> sockets, Vector<Status> &results)
{
  results.clear();
  CDCC_TRY(begin());
  NoCollectScope noCollect(
      lib_.get()); /* ends last: what it lets go may own this, or free defs */
  if (currentKey() == failedKey_ && failedPaths_.size() > 0 &&
      sockets.size() >= size_t(failedPaths_.size()) &&
      std::equal(failedPaths_.begin(), failedPaths_.end(), sockets.begin()))
  {
    return failed_;
  }
  CDCC_TRY(prepareCall());
  EvaluationScope scope(rt_);
  for (const SocketPath &path : sockets) {
    Pulled *p = results_.lookup_ptr(path);
    if (!p) {
      results_.add(path, Pulled());
      p = results_.lookup_ptr(path);
    }
    Status status;
    pull(path, *p, status);
    if (overBudget_) {
      if (p->changedAt == 0) {
        results_.remove(path);
      }
      break;
    }
    if (p->missing) {
      results_.remove(path); /* no socket, no result */
    }
    results.append(std::move(status));
  }
  if (overBudget_) {
    failedHandles_.clear();
    failedPaths_.clear();
    for (int i = 0; i <= int(results.size()); i++) {
      failedPaths_.append(sockets[size_t(i)]);
    }
    results.clear();
  }
  return endCall();
}

/* ---- requests ---- */

Evaluator::Request Evaluator::find(const SocketPath &path)
{
  Request r;
  auto missing = [&r](const char *what) {
    r.done = true;
    r.status = Status::error(StatusCode::NotFound, what);
    return r;
  };
  auto failed = [&r](ErrorPtr e) {
    r.done = true;
    r.error = std::move(e);
    return r;
  };
  auto constant = [&r](Constant c, const SocketType &type, TypeChains &chains) {
    r.done = true;
    r.origin = TraceInput{c.origin, chains.extend(TypeChains::kEmpty, &type)};
    r.value = std::move(c.value);
    return r;
  };
  /* Output `o` of a frame's interface. */
  auto output =
      [&](Frame &f, int o, const SocketType *type, NodeId node, const Frame &at) {
        if (f.topo->outputNode >= 0) {
          r.vertex = Vertex{&f, f.topo->nodes[f.topo->outputNode].inBegin + o, false};
          return r;
        }
        if (!type) {
          return failed(makeError(at, node, unknownType()));
        }
        return constant(typeDefault(*type), *type, chains_);
      };

  Frame *f = root_.get();
  sync(*f);
  if (path.node.node.isNull()) {
    const Vector<InterfaceSocket> &list = f->def->interfaceSockets(SocketSide::Output);
    for (int i = 0; i < int(list.size()); i++) {
      if (strcmp(list[i].name.c_str(), path.key.c_str()) == 0 ||
          strcmp(list[i].key().c_str(), path.key.c_str()) == 0)
      {
        const SocketType *type =
            rt_.ext<SocketTypeRegistry>().find(list[i].typeName.c_str());
        return output(*f, i, type, NodeId(), *f);
      }
    }
    return missing("the graph has no such output");
  }
  for (NodeId id : path.node.groups) {
    const int i = f->topo->find(id);
    if (i < 0 || f->topo->nodes[i].role != NodeRole::Group) {
      return missing("no such group in the path");
    }
    ErrorPtr error;
    f = child(*f, f->topo->nodes[i].group, error);
    if (!f) {
      return failed(std::move(error));
    }
  }
  const int i = f->topo->find(path.node.node);
  if (i < 0) {
    return missing("no such node");
  }
  const GraphTopology::Node &n = f->topo->nodes[i];
  const bool input = path.side == SocketSide::Input;
  const int s = input ? f->topo->inputIndex(n, path.key.c_str())
                      : f->topo->outputIndex(n, path.key.c_str());
  if (s < 0) {
    return missing("the node has no such socket");
  }
  if (input) {
    r.vertex = Vertex{f, n.inBegin + s, false};
    return r;
  }
  const SocketType *type = f->topo->outputs[n.outBegin + s].type;
  Feed source = feed(*f, i, s);
  if (source.vertex.frame) {
    r.vertex = source.vertex;
    r.output = source.output;
    return r;
  }
  if (source.error) {
    return failed(std::move(source.error));
  }
  if (!type) {
    return failed(makeError(*f, path.node.node, unknownType()));
  }
  return constant(n.role == NodeRole::Input ? external(s, *type) : typeDefault(*type),
                  *type,
                  chains_);
}

void Evaluator::store(Pulled &out, const Request &r, Status &status)
{
  out.at = keyMoves_;
  if (!r.status.ok()) {
    status = r.status;
    if (!out.missing) {
      out = Pulled();
      out.missing = true;
      out.at = keyMoves_;
      out.changedAt = rt_.nextVersion();
      GraphError e;
      e.status = r.status;
      out.error = std::make_shared<const GraphError>(std::move(e));
    }
    return;
  }
  bool changed = out.changedAt == 0;
  out.missing = false;
  ErrorPtr error = r.error;
  TraceInput origin = r.origin;
  const GraphValue *value = &r.value;
  if (!r.done && r.vertex.kernel) {
    const KernelState &k = *r.vertex.frame->kernels[r.vertex.index];
    error = k.error;
    if (!error) {
      value = &k.outputs[r.output].value;
      origin.origin.kind = Origin::Kind::Kernel;
      origin.origin.version = k.outputs[r.output].changedAt;
      origin.origin.c = uint64_t(r.output);
    }
  } else if (!r.done) {
    const InputState &s = r.vertex.frame->inputs[r.vertex.index];
    error = s.error;
    origin = s.origin;
    value = s.value;
  }

  changed = changed || !sameError(out.error, error);
  out.error = error;
  if (error) {
    status = error->status;
  } else if (!out.hasValue || !(out.origin == origin)) {
    if (!out.hasValue || out.value != *value) {
      out.value = *value;
      changed = true;
    }
    out.hasValue = true;
    out.origin = origin;
  }
  if (changed) {
    out.changedAt = rt_.nextVersion();
  }
}

bool Evaluator::exists(const SocketPath &path) const
{
  const GraphDef *def = root_->def;
  Vector<const GraphDef *> above;
  for (NodeId id : path.node.groups) {
    const GraphNode *n = def->node(id);
    if (!n || !n->type() || n->type()->role != NodeRole::Group) {
      return false;
    }
    above.append(def);
    def = def->groupTarget(*n);
    if (!def || above.contains(def) || above.size() > kMaxGraphDepth) {
      return true;
    }
  }
  if (path.node.node.isNull()) {
    const std::string_view key = path.key.c_str();
    for (const InterfaceSocket &s : def->interfaceSockets(SocketSide::Output)) {
      if (key == s.name.c_str() || key == s.key().c_str()) {
        return true;
      }
    }
    return false;
  }
  const GraphNode *n = def->node(path.node.node);
  return n && def->findSocket(*n, path.side, path.key.c_str());
}

void Evaluator::pruneResults()
{
  const uint64_t structure = rt_.ext<GraphSystem>().structureVersion();
  const Library *lib = lib_.get();
  const uint64_t epoch = lib ? lib->epoch() : 0;
  if (prunedStructure_ == structure && prunedEpoch_ == epoch) {
    return;
  }
  prunedStructure_ = structure;
  prunedEpoch_ = epoch;
  Vector<SocketPath> gone;
  for (auto &pair : results_) {
    if (!exists(pair.key)) {
      gone.append(pair.key);
    }
  }
  for (const SocketPath &path : gone) {
    results_.remove(path);
  }
}

const Evaluator::Pulled *Evaluator::result(const SocketPath &socket) const
{
  return results_.lookup_ptr(socket);
}

bool Evaluator::isCurrent(const Pulled &p) const
{
  return p.at == keyMoves_ && p.at != 0 && !(p.error && isCancelled(*p.error)) &&
         untouched(currentKey());
}

/* ---- the walk ---- */

WalkState &Evaluator::walkState(const Vertex &v)
{
  return v.kernel ? kernel(*v.frame, v.index).walk : v.frame->inputs[v.index].walk;
}

void Evaluator::enter(const Vertex &v)
{
  WalkState &w = walkState(v);
  w.pass = pass_;
  w.number = w.low = numbers_++;
  w.onStack = true;
  w.done = false;
  visits++;
  stack_.append(Item{v, 0});
  component_.append(v);
}

bool Evaluator::walk(Vertex root)
{
  if (walkState(root).pass == pass_) {
    return true;
  }
  const int base = int(stack_.size()); /* not 0 in a run of an element */
  enter(root);
  while (int(stack_.size()) > base) {
    Item &item = stack_.last();
    const Vertex v = item.vertex;
    std::optional<Vertex> next;
    if (v.kernel) {
      const GraphTopology::Node &n = v.frame->topo->nodes[v.index];
      if (n.type && item.next < n.inCount) {
        next = Vertex{v.frame, n.inBegin + item.next++, false};
      }
    } else if (item.next == 0) {
      item.next = 1;
      next = follow(*v.frame, v.index);
      if (overBudget_) {
        stack_.clear();
        component_.clear();
        return false;
      }
    }
    if (next) {
      const WalkState &w = walkState(*next);
      if (w.pass != pass_) {
        enter(*next);
      } else if (w.onStack) {
        WalkState &mine = walkState(v);
        mine.low = std::min(mine.low, w.number);
      }
      continue;
    }

    const WalkState w = walkState(v);
    stack_.pop_back();
    if (stack_.size() > 0) {
      WalkState &above = walkState(stack_.last().vertex);
      above.low = std::min(above.low, w.low);
    }
    if (w.low != w.number) {
      continue; /* in a cycle with a vertex further down the stack */
    }
    int first = int(component_.size()) - 1;
    while (!(component_[first] == v)) {
      first--;
    }
    /* A group's input can lead to itself directly, through an interior output. */
    const bool self = !v.kernel && v.frame->inputs[v.index].source == v;
    if (first == int(component_.size()) - 1 && !self) {
      if (v.kernel) {
        computeKernel(*v.frame, v.index);
        if (overBudget_) {
          return false;
        }
      } else {
        computeInput(*v.frame, v.index);
      }
    } else {
      Vector<Vertex> members;
      for (int i = first; i < int(component_.size()); i++) {
        members.append(component_[i]);
      }
      failCycle(members);
    }
    while (int(component_.size()) > first) {
      WalkState &done = walkState(component_.pop_back());
      done.onStack = false;
      done.done = true;
    }
  }
  return true;
}

void Evaluator::failCycle(const Vector<Vertex> &members)
{
  struct Member {
    NodePath path;
    string key;
  };
  /* Every cycle goes through inputs. Kernels on it are named by theirs. */
  Vector<Member> list;
  for (const Vertex &v : members) {
    if (!v.kernel) {
      const GraphTopology::Input &in = v.frame->topo->inputs[v.index];
      list.append(
          Member{v.frame->pathOf(v.frame->topo->nodes[in.node].node->id()), in.key});
    }
  }
  std::sort(list.begin(), list.end(), [](const Member &a, const Member &b) {
    return a.path < b.path ||
           (a.path == b.path && strcmp(a.key.c_str(), b.key.c_str()) < 0);
  });
  std::string text = "a cycle through ";
  for (int i = 0; i < int(list.size()) && i < 8; i++) {
    text += (i ? ", " : "") + list[i].path.toString() + "." + list[i].key.c_str();
  }
  if (list.size() > 8) {
    text += " and " + std::to_string(list.size() - 8) + " more";
  }
  GraphError e;
  e.status = Status::error(StatusCode::FailedPrecondition, text.c_str());
  e.origin = list[0].path;
  e.socket = list[0].key;
  const ErrorPtr error = std::make_shared<const GraphError>(std::move(e));
  for (const Vertex &v : members) {
    if (v.kernel) {
      settle(kernel(*v.frame, v.index), error, false);
    } else {
      v.frame->inputs[v.index].error = error;
    }
  }
}

/* ---- inputs ---- */

Evaluator::Feed Evaluator::feed(Frame &f, int node, int output)
{
  const GraphTopology::Node &n = f.topo->nodes[node];
  Feed out;
  switch (n.role) {
  case NodeRole::Kernel:
  case NodeRole::Foreach:
    out.vertex = Vertex{&f, node, true};
    out.output = output;
    break;
  case NodeRole::Input:
    if (f.parent && !f.element) {
      const GraphTopology::Node &g = f.parent->topo->nodes[f.groupIndex];
      out.vertex = Vertex{f.parent, g.inBegin + output, false};
    }
    break;
  case NodeRole::Group: {
    Frame *c = child(f, n.group, out.error);
    if (!c) {
      break;
    }
    if (output < n.interfaceOuts) {
      if (c->topo->outputNode >= 0) {
        const GraphTopology::Node &o = c->topo->nodes[c->topo->outputNode];
        out.vertex = Vertex{c, o.inBegin + output, false};
      }
      break;
    }
    const InteriorKey &key = f.topo->groups[n.group].interiorOutput(n, output).key;
    for (NodeId id : key.groups) {
      const int at = c->topo->find(id);
      c = at >= 0 ? child(*c, c->topo->nodes[at].group, out.error) : nullptr;
      if (!c) {
        return out;
      }
    }
    const int at = c->topo->find(key.node);
    const int o =
        at >= 0 ? c->topo->outputIndex(c->topo->nodes[at], key.key.c_str()) : -1;
    return o >= 0 ? feed(*c, at, o) : out; /* a key names no interior one: this ends */
  }
  case NodeRole::Output:
    break; /* it has no outputs */
  }
  return out;
}

std::optional<Vertex> Evaluator::follow(Frame &f, int input)
{
  const GraphTopology &t = *f.topo;
  const GraphTopology::Input &in = t.inputs[input];
  InputState &s = f.inputs[input];
  s.error = nullptr;
  s.value = nullptr;
  s.source = Vertex();
  const Vertex *by = f.replacedBy(input);
  s.replaced = by;
  if (by) {
    s.source = *by;
  } else if (in.fromNode >= 0) {
    Feed source = feed(f, in.fromNode, in.fromOutput);
    s.source = source.vertex;
    s.sourceOutput = source.output;
    s.error = std::move(source.error);
  }
  if (f.deps) {
    Dependents::setSource(f,
                          input,
                          s.source,
                          s.source.kernel ? &kernel(*s.source.frame, s.source.index)
                                          : nullptr);
  }
  return s.source.frame ? std::optional<Vertex>(s.source) : std::nullopt;
}

Evaluator::Constant Evaluator::typeDefault(const SocketType &type) const
{
  Constant c;
  c.origin.kind = Origin::Kind::TypeDefault;
  c.origin.a = type.id.value;
  c.value = type.defaultValue;
  return c;
}

/* The default of an interface input of `def`, if it has a usable one. */
static bool interfaceDefault(const GraphDef &def,
                             int index,
                             const SocketType &type,
                             Origin &origin,
                             GraphValue &value)
{
  const InterfaceSocket &s = def.interfaceSockets(SocketSide::Input)[index];
  Result<GraphValue> v = GraphValue::fromProp(s.defaultValue);
  if (!v.ok() || !type.holds(*v)) {
    return false;
  }
  origin.kind = Origin::Kind::InterfaceDefault;
  origin.a = def.serial();
  origin.b = s.id;
  origin.version = s.defaultVersion;
  value = std::move(v).value();
  return true;
}

Evaluator::Constant Evaluator::external(int interfaceIndex, const SocketType &type) const
{
  const GraphDef &def = *root_->def;
  const int64_t id = def.interfaceSockets(SocketSide::Input)[interfaceIndex].id;
  Constant c;
  if (const External *e = inputs_.lookup_ptr(id); e && type.holds(e->value)) {
    c.origin.kind = Origin::Kind::External;
    c.origin.b = id;
    c.origin.version = e->version;
    c.value = e->value;
    return c;
  }
  if (interfaceDefault(def, interfaceIndex, type, c.origin, c.value)) {
    return c;
  }
  return typeDefault(type);
}

Evaluator::Constant Evaluator::unlinked(const Frame &f, int input) const
{
  const GraphTopology::Input &in = f.topo->inputs[input];
  const GraphTopology::Node &n = f.topo->nodes[in.node];
  const int index = input - n.inBegin;
  Constant c;
  if (const NodeValue *lit = n.node->literal(in.key.c_str(), *in.type)) {
    c.origin.kind = Origin::Kind::Literal;
    c.origin.a = f.serial;
    c.origin.b = n.node->id().value;
    c.origin.c = coredcc::detail::fnv1a64(in.key.c_str());
    c.origin.version = lit->version;
    c.value = GraphValue::fromProp(lit->value).value();
    return c;
  }
  if (n.role == NodeRole::Kernel && n.type) {
    c.origin.kind = Origin::Kind::NodeDefault;
    c.origin.a = n.type->stamp;
    c.origin.c = uint64_t(index);
    c.value = n.type->inputs[index].defaultValue;
    return c;
  }
  if (hasGroup(n.role)) {
    const GraphDef *def = f.def->groupTarget(*n.node);
    const int at = n.role == NodeRole::Foreach ? index - 1 : index;
    if (def && at >= 0 && interfaceDefault(*def, at, *in.type, c.origin, c.value)) {
      return c;
    }
  }
  return typeDefault(*in.type);
}

void Evaluator::computeInput(Frame &f, int input)
{
  const GraphTopology::Input &in = f.topo->inputs[input];
  InputState &s = f.inputs[input];
  if (s.error) {
    return; /* the group it comes from */
  }
  auto fail = [&](Status status) {
    s.error = makeError(
        f, f.topo->nodes[in.node].node->id(), std::move(status), in.key.c_str());
  };
  auto keep = [&](const TraceInput &origin, GraphValue &&v) {
    if (!s.own) {
      s.own = std::make_unique<InputState::Own>();
    }
    s.own->value = std::move(v);
    s.own->origin = origin;
    s.value = &s.own->value;
  };

  /* The value at the other end of the link, and the types it crossed. */
  Constant constant;
  const GraphValue *value = nullptr;
  const SocketType *type = in.type; /* a replacing input has it too */
  if (in.fromNode >= 0 && !s.replaced) {
    type = f.topo->outputs[f.topo->nodes[in.fromNode].outBegin + in.fromOutput].type;
  }
  if (s.source.frame && s.source.kernel) {
    const Frame &from = *s.source.frame;
    const KernelState &k = *from.kernels[s.source.index];
    if (k.error) {
      s.error = k.error;
      return;
    }
    Origin &o = s.origin.origin;
    o = Origin();
    o.kind = Origin::Kind::Kernel;
    o.a = from.serial;
    o.b = from.topo->nodes[s.source.index].node->id().value;
    o.c = uint64_t(s.sourceOutput);
    o.version = k.outputs[s.sourceOutput].changedAt;
    value = &k.outputs[s.sourceOutput].value;
    s.origin.chain = chains_.extend(TypeChains::kEmpty, type);
  } else if (s.source.frame) {
    const InputState &from = s.source.frame->inputs[s.source.index];
    if (from.error) {
      s.error = from.error;
      return;
    }
    s.origin = from.origin;
    value = from.value;
  } else {
    if (!type) {
      return fail(unknownType());
    }
    const bool inner =
        in.fromNode >= 0 && f.topo->nodes[in.fromNode].role == NodeRole::Input;
    if (inner && f.element) {
      value = elementInput(f, in.fromOutput, *type, s.origin);
    } else {
      constant = in.fromNode < 0 ? unlinked(f, input)
                 : inner         ? external(in.fromOutput, *type)
                                 : typeDefault(*type);
      s.origin = TraceInput{constant.origin, chains_.extend(TypeChains::kEmpty, type)};
    }
  }
  if (!type || !in.type) {
    return fail(unknownType());
  }

  /* To this socket's type. */
  if (type != in.type) {
    s.origin.chain = chains_.extend(s.origin.chain, in.type);
  }
  if (type == in.type && value) {
    s.value = value;
  } else if (s.own && s.own->origin == s.origin) {
    s.value = &s.own->value; /* made from the same before */
  } else if (type == in.type) {
    keep(s.origin, std::move(constant.value));
  } else {
    GraphValue out;
    Status status = rt_.ext<SocketTypeRegistry>().convert(
        *type, *in.type, value ? *value : constant.value, out);
    if (!status.ok()) {
      return fail(std::move(status));
    }
    keep(s.origin, std::move(out));
  }
}

/* ---- kernels ---- */

void Evaluator::settle(KernelState &k, ErrorPtr error, bool inputFailed)
{
  /* A Cancelled that comes and goes moves nothing: what it fed runs if the value is
   * another. */
  const bool passing =
      (error && isCancelled(*error)) || (k.error && isCancelled(*k.error));
  if (!k.visited || (!passing && !sameError(k.error, error))) {
    const uint64_t version = rt_.nextVersion();
    for (KernelState::Output &o : k.outputs) {
      o.changedAt = version;
    }
  }
  k.visited = true;
  k.inputFailed = inputFailed;
  k.error = std::move(error);
  if (k.error && !inputFailed && isCancelled(*k.error)) {
    k.ran = false;
    if (k.frame->deps) {
      cancelledAt_.append(Vertex{k.frame, k.index, true});
    }
  }
}

void Evaluator::computeKernel(Frame &f, int node)
{
  const GraphTopology::Node &n = f.topo->nodes[node];
  if (n.role == NodeRole::Foreach) {
    return computeForeach(f, node);
  }
  KernelState &k = kernel(f, node);
  if (!n.type) {
    Status status = rt_.ext<NodeTypeRegistry>().unknown(n.node->typeName().c_str());
    return settle(k, makeError(f, n.node->id(), std::move(status)), false);
  }
  bool same = k.ran && k.trace.paramVersion == n.node->paramVersion() &&
              k.trace.kernel == n.type->kernelStamp;
  for (int i = 0; i < n.inCount; i++) {
    const InputState &s = f.inputs[n.inBegin + i];
    if (s.error) {
      return settle(k, s.error, true);
    }
    same = same && k.trace.inputs[i] == s.origin;
  }
  const bool params = n.node->blockRefs().size() > 0 || k.trace.paramBlocks().size() > 0;
  auto target = [&n](int param) {
    return n.node->blockTarget(n.type->params[param].def.apiName().c_str());
  };
  for (int i = 0; same && params && i < int(n.type->params.size()); i++) {
    same = i < int(k.trace.paramBlocks().size()) &&
           k.trace.paramBlocks()[size_t(i)] == target(i);
  }
  if (!same || !blocks_.current(k.trace)) {
    Trace trace;
    trace.paramVersion = n.node->paramVersion();
    trace.kernel = n.type->kernelStamp;
    for (int i = 0; i < n.inCount; i++) {
      trace.inputs.append(f.inputs[n.inBegin + i].origin);
    }
    for (int i = 0; params && i < int(n.type->params.size()); i++) {
      trace.blocks().params.append(target(i));
    }
    run(f, node, k, std::move(trace));
  }
  if (sink_.set) {
    sink_.set->add(k.trace, *sink_.trace);
  }
  settle(k, k.runError, false);
}

void Evaluator::run(Frame &f, int node, KernelState &k, Trace trace)
{
  const GraphTopology::Node &n = f.topo->nodes[node];
  if (cancelled()) {
    k.runError =
        makeError(f,
                  n.node->id(),
                  Status::error(StatusCode::Cancelled, "the evaluation was cancelled"));
    return; /* what it has from before stays, to be compared with */
  }
  Vector<NodeExecContext::Input> inputs;
  for (int i = 0; i < n.inCount; i++) {
    inputs.append(
        NodeExecContext::Input{f.inputs[n.inBegin + i].value,
                               f.inputs[n.inBegin + i].source.frame ||
                                   f.topo->inputs[n.inBegin + i].fromNode >= 0});
  }
  Vector<PropValue> params;
  Vector<BlockId> blocks;
  for (const NodeParamDef &p : n.type->params) {
    params.append(f.def->paramValue(*n.node, p.def.apiName().c_str()));
    blocks.append(n.node->blockTarget(p.def.apiName().c_str()));
  }
  BlockRecorder recorder(blocks_, trace);
  NodeExecContext::Setup setup;
  setup.rt = &rt_;
  setup.type = n.type;
  setup.inputs = {inputs.data(), size_t(inputs.size())};
  setup.params = {params.data(), size_t(params.size())};
  setup.paramBlocks = {blocks.data(), size_t(blocks.size())};
  setup.library = lib_.get();
  setup.blocks = &recorder;
  Monitor monitor(*this, f, n.node->id());
  setup.monitor = &monitor;
  setup.userData = userData;
  NodeExecContext ctx(setup);

  const auto start = std::chrono::steady_clock::now();
  Status status = n.type->kernel.get().evaluate(ctx);
  k.seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  k.totalSeconds += k.seconds;
  k.runs++;
  totalRuns++;
  if (rt_.hasStickyError()) {
    Status sticky = rt_.takeStickyError();
    if (status.ok()) {
      status = std::move(sticky);
    }
  }
  if (status.ok()) {
    status = ctx.checkOutputs();
  }
  k.ran = true;
  k.trace = std::move(trace);
  if (f.deps) {
    f.deps->setBlocks(k, rt_.ext<ClientRegistry>());
  }
  if (!status.ok()) {
    k.runError = makeError(f, n.node->id(), std::move(status));
    return;
  }
  k.runError = nullptr;
  const uint64_t version = rt_.nextVersion();
  for (int i = 0; i < int(k.outputs.size()); i++) {
    KernelState::Output &o = k.outputs[i];
    if (o.value != ctx.outputs()[i]) {
      o.value = std::move(ctx.outputs()[i]);
      o.changedAt = version;
    }
  }
}

} // namespace coredcc::graph
