/* The walk: which vertices a request needs, in which order, and what each one gives. */
#include "coredcc/graph/evaluator.h"

#include "coredcc/base/assert.h"
#include "coredcc/graph/socket_type.h"
#include "coredcc/lib/library.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <string>

namespace coredcc::graph {

using litestl::util::string;
using litestl::util::Vector;

namespace {

Status unknownType()
{
  return Status::error(StatusCode::NotFound, "a socket's type isn't registered");
}

bool sameError(const ErrorPtr &a, const ErrorPtr &b)
{
  return a == b || (a && b && *a == *b);
}

struct NoCollectScope {
  explicit NoCollectScope(Library *l) : lib(l)
  {
    if (lib) {
      lib->beginNoCollect();
    }
  }
  ~NoCollectScope()
  {
    if (lib) {
      lib->endNoCollect();
    }
  }
  NoCollectScope(const NoCollectScope &) = delete;
  NoCollectScope &operator=(const NoCollectScope &) = delete;
  Library *lib;
};

} // namespace

Evaluator::Evaluator(Runtime *rt, Library *lib, GraphSource source)
    : rt_(*rt), lib_(lib), source_(source), blocks_(*rt, lib)
{
}

Evaluator::~Evaluator()
{
  CDCC_CHECK(!running_, "a GraphInstance was destroyed while it evaluates");
}

Evaluator::Key Evaluator::currentKey() const
{
  return Key{GraphPassKey::of(rt_, lib_.get()), own_};
}

Status Evaluator::evaluate(std::span<const SocketPath> sockets, Vector<Status> &results)
{
  results.clear();
  CDCC_TRY(rt_.busyIfEvaluating("GraphInstance::evaluate"));
  if (rt_.hasStickyError()) {
    return Status::error(StatusCode::FailedPrecondition,
                         "GraphInstance::evaluate: a sticky error is pending; take it first");
  }
  Library *lib = lib_.get();
  if (lib && lib->sweeping()) {
    return Status::error(StatusCode::Busy, "GraphInstance::evaluate: the library is sweeping");
  }
  CDCC_TRY(prepareRoot());
  const Key key = currentKey();
  if (!(key == passKey_)) {
    passKey_ = key;
    pass_++;
    numbers_ = 0;
  }
  call_++;
  overBudget_ = false;

  NoCollectScope noCollect(lib);
  EvaluationScope scope(rt_);
  running_ = true;
  for (const SocketPath &path : sockets) {
    Request r = find(path);
    if (!r.done && !overBudget_) {
      walk(r.vertex);
    }
    if (overBudget_) {
      break;
    }
    Status status;
    store(path, r, status);
    results.append(std::move(status));
  }
  running_ = false;
  if (overBudget_) {
    results.clear();
    dropNewFrames();
    passKey_ = Key(); /* nothing of this pass stands */
    return Status::error(
        StatusCode::OutOfRange,
        ("the graph needs more than " + std::to_string(maxFrames) + " groups").c_str());
  }
  return Status();
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
  auto output = [&](Frame &f, int o, const SocketType *type, NodeId node, const Frame &at) {
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
  const int s = input ? f->topo->inputIndex(n, path.key.c_str()) :
                        f->topo->outputIndex(n, path.key.c_str());
  if (s < 0) {
    return missing("the node has no such socket");
  }
  if (input) {
    r.vertex = Vertex{f, n.inBegin + s, false};
    return r;
  }
  const SocketType *type = f->topo->outputs[n.outBegin + s].type;
  if (n.role == NodeRole::Kernel) {
    r.vertex = Vertex{f, i, true};
    r.output = s;
    return r;
  }
  if (n.role == NodeRole::Group) {
    ErrorPtr error;
    Frame *c = child(*f, n.group, error);
    return c ? output(*c, s, type, path.node.node, *f) : failed(std::move(error));
  }
  /* An interface input. */
  if (f->parent) {
    r.vertex = Vertex{f->parent, f->parent->topo->nodes[f->groupIndex].inBegin + s, false};
    return r;
  }
  if (!type) {
    return failed(makeError(*f, path.node.node, unknownType()));
  }
  return constant(external(s, *type), *type, chains_);
}

void Evaluator::store(const SocketPath &path, const Request &r, Status &status)
{
  if (!r.status.ok()) {
    status = r.status;
    return; /* no socket, no result */
  }
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
  }
  else if (!r.done) {
    const InputState &s = r.vertex.frame->inputs[r.vertex.index];
    error = s.error;
    origin = s.origin;
    value = s.value;
  }

  if (!results_.lookup_ptr(path)) {
    results_.add(path, Pulled());
  }
  Pulled &out = *results_.lookup_ptr(path);
  const bool first = out.changedAt == 0;
  bool changed = first || !sameError(out.error, error);
  out.key = passKey_;
  out.error = error;
  if (error) {
    status = error->status;
  }
  else if (!out.hasValue || !(out.origin == origin)) {
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

const Evaluator::Pulled *Evaluator::result(const SocketPath &socket) const
{
  return results_.lookup_ptr(socket);
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
  stack_.append(Item{v, 0});
  component_.append(v);
}

bool Evaluator::walk(Vertex root)
{
  if (walkState(root).pass == pass_) {
    return true;
  }
  enter(root);
  while (stack_.size() > 0) {
    Item &item = stack_.last();
    const Vertex v = item.vertex;
    std::optional<Vertex> next;
    if (v.kernel) {
      const GraphTopology::Node &n = v.frame->topo->nodes[v.index];
      if (n.type && item.next < n.inCount) {
        next = Vertex{v.frame, n.inBegin + item.next++, false};
      }
    }
    else if (item.next == 0) {
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
      }
      else if (w.onStack) {
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
    if (first == int(component_.size()) - 1) {
      if (v.kernel) {
        computeKernel(*v.frame, v.index);
      }
      else {
        computeInput(*v.frame, v.index);
      }
    }
    else {
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
      list.append(Member{v.frame->pathOf(v.frame->topo->nodes[in.node].node->id()), in.key});
    }
  }
  std::sort(list.begin(), list.end(), [](const Member &a, const Member &b) {
    return a.path < b.path || (a.path == b.path && strcmp(a.key.c_str(), b.key.c_str()) < 0);
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
    }
    else {
      v.frame->inputs[v.index].error = error;
    }
  }
}

/* ---- inputs ---- */

std::optional<Vertex> Evaluator::follow(Frame &f, int input)
{
  const GraphTopology &t = *f.topo;
  const GraphTopology::Input &in = t.inputs[input];
  InputState &s = f.inputs[input];
  s.error = nullptr;
  s.value = nullptr;
  s.source = Vertex();
  if (in.fromNode < 0) {
    return std::nullopt;
  }
  const GraphTopology::Node &from = t.nodes[in.fromNode];
  s.sourceType = t.outputs[from.outBegin + in.fromOutput].type;
  switch (from.role) {
    case NodeRole::Kernel:
      s.source = Vertex{&f, in.fromNode, true};
      s.sourceOutput = in.fromOutput;
      break;
    case NodeRole::Input:
      if (f.parent) {
        const GraphTopology::Node &g = f.parent->topo->nodes[f.groupIndex];
        s.source = Vertex{f.parent, g.inBegin + in.fromOutput, false};
      }
      break;
    case NodeRole::Group:
      if (Frame *c = child(f, from.group, s.error); c && c->topo->outputNode >= 0) {
        const GraphTopology::Node &o = c->topo->nodes[c->topo->outputNode];
        s.source = Vertex{c, o.inBegin + in.fromOutput, false};
      }
      break;
    case NodeRole::Output:
      break; /* it has no outputs */
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
  if (const NodeValue *lit = n.node->literal(in.key.c_str())) {
    Result<GraphValue> v = GraphValue::fromProp(lit->value);
    if (v.ok() && in.type->holds(*v)) {
      c.origin.kind = Origin::Kind::Literal;
      c.origin.a = f.serial;
      c.origin.b = n.node->id().value;
      c.origin.c = coredcc::detail::fnv1a64(in.key.c_str());
      c.origin.version = lit->version;
      c.value = std::move(v).value();
      return c;
    }
  }
  if (n.role == NodeRole::Kernel && n.type) {
    c.origin.kind = Origin::Kind::NodeDefault;
    c.origin.a = n.type->stamp;
    c.origin.c = uint64_t(index);
    c.value = n.type->inputs[index].defaultValue;
    return c;
  }
  if (n.role == NodeRole::Group) {
    const GraphDef *def = f.def->groupTarget(*n.node);
    if (def && interfaceDefault(*def, index, *in.type, c.origin, c.value)) {
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
    s.error = makeError(f, f.topo->nodes[in.node].node->id(), std::move(status), in.key.c_str());
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
  const SocketType *type = in.fromNode < 0 ? in.type : s.sourceType;
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
  }
  else if (s.source.frame) {
    const InputState &from = s.source.frame->inputs[s.source.index];
    if (from.error) {
      s.error = from.error;
      return;
    }
    s.origin = from.origin;
    value = from.value;
  }
  else {
    if (!type) {
      return fail(unknownType());
    }
    if (in.fromNode < 0) {
      constant = unlinked(f, input);
    }
    else if (f.topo->nodes[in.fromNode].role == NodeRole::Input) {
      constant = external(in.fromOutput, *type);
    }
    else {
      constant = typeDefault(*type);
    }
    s.origin = TraceInput{constant.origin, chains_.extend(TypeChains::kEmpty, type)};
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
  }
  else if (s.own && s.own->origin == s.origin) {
    s.value = &s.own->value; /* made from the same before */
  }
  else if (type == in.type) {
    keep(s.origin, std::move(constant.value));
  }
  else {
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
  if (!k.visited || !sameError(k.error, error)) {
    const uint64_t version = rt_.nextVersion();
    for (KernelState::Output &o : k.outputs) {
      o.changedAt = version;
    }
  }
  k.visited = true;
  k.inputFailed = inputFailed;
  k.error = std::move(error);
}

void Evaluator::computeKernel(Frame &f, int node)
{
  const GraphTopology::Node &n = f.topo->nodes[node];
  KernelState &k = kernel(f, node);
  if (!n.type) {
    Status status = rt_.ext<NodeTypeRegistry>().unknown(n.node->typeName().c_str());
    return settle(k, makeError(f, n.node->id(), std::move(status)), false);
  }
  Trace trace;
  trace.paramVersion = n.node->paramVersion();
  bool same = k.ran && k.trace.paramVersion == trace.paramVersion;
  if (n.node->blockRefs().size() > 0 || k.trace.paramBlocks.size() > 0) {
    for (const NodeParamDef &p : n.type->params) {
      trace.paramBlocks.append(n.node->blockTarget(p.def.apiName().c_str()));
    }
    same = same && std::equal(trace.paramBlocks.begin(), trace.paramBlocks.end(),
                              k.trace.paramBlocks.begin(), k.trace.paramBlocks.end());
  }
  for (int i = 0; i < n.inCount; i++) {
    const InputState &s = f.inputs[n.inBegin + i];
    if (s.error) {
      return settle(k, s.error, true);
    }
    same = same && k.trace.inputs[i] == s.origin;
    trace.inputs.append(s.origin);
  }
  if (!same || !blocks_.current(k.trace)) {
    run(f, node, k, std::move(trace));
  }
  settle(k, k.runError, false);
}

void Evaluator::run(Frame &f, int node, KernelState &k, Trace trace)
{
  const GraphTopology::Node &n = f.topo->nodes[node];
  Vector<NodeExecContext::Input> inputs;
  for (int i = 0; i < n.inCount; i++) {
    inputs.append(NodeExecContext::Input{f.inputs[n.inBegin + i].value,
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
  setup.userData = userData;
  NodeExecContext ctx(setup);

  const auto start = std::chrono::steady_clock::now();
  Status status = n.type->kernel.get().evaluate(ctx);
  k.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
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
