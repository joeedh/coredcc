/* What an instance says about its nodes without evaluating: reports, values that were
 * computed on the way, and what it takes; and dropping what a node keeps. */
#include "coredcc/graph/evaluator.h"

#include "coredcc/lib/library.h"

#include <cstring>

namespace coredcc::graph {

using litestl::util::Vector;

/* ---- finding ---- */

const Frame *Evaluator::frameAt(const NodePath &path) const
{
  const Frame *f = root_.get();
  int element = 0;
  for (NodeId id : path.groups) {
    const int at = f && f->topo ? f->topo->find(id) : -1;
    if (at < 0) {
      return nullptr;
    }
    const GraphTopology::Node &n = f->topo->nodes[at];
    if (n.role == NodeRole::Foreach) {
      const KernelState *k = f->kernels[at];
      Element *const *e = k && element < int(path.elements.size())
                              ? k->foreach->cached.lookup_ptr(path.elements[element])
                              : nullptr;
      element++;
      f = e ? (*e)->frame : nullptr;
    } else {
      f = n.group >= 0 ? f->groups[n.group].child : nullptr;
    }
  }
  return f && f->topo ? f : nullptr;
}

/* The pass of which what is in a frame holds together, or 0. An element's are the passes
 * of its runs. */
uint64_t Evaluator::standingPass(const Frame &f) const
{
  if (f.top->element) {
    return f.top->pass;
  }
  return passKey_ == Key() ? 0 : pass_;
}

bool Evaluator::stands(const Frame &f, uint64_t pass) const
{
  if (pass == 0 || pass != standingPass(f)) {
    return false;
  }
  const Element *e = f.top->element;
  if (!e) {
    return untouched(currentKey());
  }
  /* The run of the foreach that it was last in must stand too. */
  const Frame &outer = *f.top->parent;
  const int at = outer.topo->find(f.top->groupNode);
  const KernelState *k = at >= 0 ? outer.kernels[at] : nullptr;
  return k && k->foreach && k->ran && e->world == k->foreach->world && k->walk.done &&
         stands(outer, k->walk.pass);
}

/* ---- values ---- */

GraphValue Evaluator::peek(const SocketPath &path) const
{
  const Frame *f = frameAt(path.node);
  if (!f) {
    return GraphValue();
  }
  const std::string_view key = path.key.c_str();
  const bool iface = path.node.node.isNull();
  const int at = iface ? f->topo->outputNode : f->topo->find(path.node.node);
  if (at < 0) {
    return GraphValue();
  }
  const GraphTopology::Node &n = f->topo->nodes[at];
  if (!iface && path.side == SocketSide::Output) {
    const int o = f->topo->outputIndex(n, key);
    return o >= 0 ? peekOutput(*f, at, o) : GraphValue();
  }
  int i = f->topo->inputIndex(n, key);
  if (i < 0 && iface && f == root_.get()) {
    Status status;
    const GraphDef *def = findDef(status);
    const InterfaceSocket *s =
        def ? def->interfaceSocket(SocketSide::Output, key) : nullptr;
    i = s ? f->topo->inputIndex(n, s->key().c_str()) : -1;
  }
  return i >= 0 ? peekInput(*f, n.inBegin + i) : GraphValue();
}

GraphValue Evaluator::peekInput(const Frame &f, int input) const
{
  const InputState &s = f.inputs[input];
  const uint64_t pass = standingPass(f);
  /* Else its value may be of a kernel that is gone. */
  const bool there = pass != 0 && s.walk.pass == pass && s.walk.done && s.value;
  return there ? *s.value : GraphValue();
}

GraphValue Evaluator::peekOutput(const Frame &f, int node, int output) const
{
  const GraphTopology::Node &n = f.topo->nodes[node];
  const char *key = f.topo->outputs[n.outBegin + output].key.c_str();
  if (isOneStep(n.role)) {
    const KernelState *k = f.kernels[node];
    return k && !k->error && output < int(k->outputs.size()) ? k->outputs[output].value
                                                             : GraphValue();
  }
  if (n.role == NodeRole::Group) {
    const Frame::Group &g = f.groups[n.group];
    const Frame *c = g.error ? nullptr : g.child;
    if (!c || !c->topo) {
      return GraphValue();
    }
    if (output < n.interfaceOuts) {
      const int out = c->topo->outputNode;
      const int i = out >= 0 ? c->topo->inputIndex(c->topo->nodes[out], key) : -1;
      return i >= 0 ? peekInput(*c, c->topo->nodes[out].inBegin + i) : GraphValue();
    }
    const InteriorKey &inner = f.topo->groups[n.group].interiorOutput(n, output).key;
    for (NodeId id : inner.groups) {
      const int at = c->topo->find(id);
      const int group = at >= 0 ? c->topo->nodes[at].group : -1;
      c = group >= 0 && !c->groups[group].error ? c->groups[group].child : nullptr;
      if (!c || !c->topo) {
        return GraphValue();
      }
    }
    const int at = c->topo->find(inner.node);
    const int o =
        at >= 0 ? c->topo->outputIndex(c->topo->nodes[at], inner.key.c_str()) : -1;
    return o >= 0 ? peekOutput(*c, at, o) : GraphValue();
  }
  /* A graph.input: what is at the group node, in the table's row, or set on the instance.
   */
  if (f.element) {
    for (const Element::Input &in : f.element->inputs) {
      if (in.column && strcmp(interfaceKey(in.id).c_str(), key) == 0) {
        return in.value;
      }
    }
  }
  if (f.parent) {
    const GraphTopology &t = *f.parent->topo;
    const int at = t.find(f.groupNode);
    const int i = at >= 0 ? t.inputIndex(t.nodes[at], key) : -1;
    return i >= 0 ? peekInput(*f.parent, t.nodes[at].inBegin + i) : GraphValue();
  }
  Status status;
  const GraphDef *def = findDef(status);
  if (def && def == f.def && def->serial() == f.defSerial) {
    const Vector<InterfaceSocket> &list = def->interfaceSockets(SocketSide::Input);
    for (int i = 0; i < int(list.size()); i++) {
      const SocketType *type =
          strcmp(list[i].key().c_str(), key) == 0
              ? rt_.ext<SocketTypeRegistry>().find(list[i].typeName.c_str())
              : nullptr;
      if (type) {
        return external(i, *type).value;
      }
    }
  }
  return GraphValue();
}

/* ---- memory ---- */

/* What a Vector has outside of itself, if it is filled without room to spare. */
template <class T, int N> static size_t heapBytes(const Vector<T, N> &v)
{
  return v.size() > N ? size_t(v.size()) * sizeof(T) : 0;
}

static void addValue(GraphMemory &m, const GraphValue &v)
{
  m.bytes += v.heapBytes();
  if (v.type() == GraphValue::Type::Payload) {
    m.payloadBytes += v.asPayload().bytes();
  }
}

static void addKernel(GraphMemory &m, const KernelState &k)
{
  m.kernels++;
  m.bytes += sizeof(KernelState) + heapBytes(k.outputs) + heapBytes(k.trace.inputs) +
             size_t(k.blockLinks.size()) * sizeof(BlockLink);
  for (const KernelState::Output &o : k.outputs) {
    addValue(m, o.value);
  }
  if (const Trace::Blocks *b = k.trace.more.get()) {
    m.bytes +=
        sizeof(*b) + heapBytes(b->params) + heapBytes(b->blocks) + heapBytes(b->queries);
  }
}

/* Of the frames in `stack`, and all below them. */
static void addFrames(GraphMemory &m, Vector<const Frame *> &stack)
{
  while (stack.size() > 0) {
    const Frame &f = *stack.pop_back();
    m.frames++;
    m.inputs += f.inputs.size();
    m.bytes += sizeof(Frame) + size_t(f.inputs.size()) * sizeof(InputState) +
               size_t(f.links.size()) * sizeof(InputLink) +
               size_t(f.kernels.size()) * sizeof(KernelState *) +
               size_t(f.groups.size()) * sizeof(Frame::Group);
    for (const InputState &s : f.inputs) {
      if (s.own) {
        m.bytes += sizeof(InputState::Own);
        addValue(m, s.own->value);
      }
    }
    for (const KernelState *k : f.kernels) {
      if (k) {
        addKernel(m, *k);
      }
    }
    for (const Frame::Group &g : f.groups) {
      if (g.child) {
        stack.append(g.child);
      }
    }
    f.forEachElement([&](const Element &e) {
      m.elements += e.counts != nullptr;
      m.bytes += sizeof(Element) + heapBytes(e.inputs) + heapBytes(e.outputs);
      stack.append(e.frame);
    });
  }
}

GraphMemory Evaluator::memory() const
{
  GraphMemory m;
  Vector<const Frame *> stack;
  if (root_) {
    stack.append(root_.get());
  }
  addFrames(m, stack);
  auto pulled = [&](const SocketPath &path, const Pulled &p) {
    m.bytes += sizeof(path) + sizeof(p);
    addValue(m, p.value);
  };
  for (auto &pair : results_) {
    pulled(pair.key, pair.value);
  }
  for (const Prepared &p : prepared_) {
    pulled(p.path, p.pulled);
  }
  m.bytes += chains_.bytes();
  return m;
}

/* ---- reports ---- */

/* Which of two states a group of both is in. */
static bool worse(NodeReport::State a, NodeReport::State b)
{
  using State = NodeReport::State;
  auto rank = [](State s) {
    return s == State::Failed ? 3 : s == State::InputFailed ? 2 : s == State::Ok ? 1 : 0;
  };
  return rank(a) > rank(b);
}

static NodeReport::State stateOf(const KernelState &k)
{
  return !k.visited      ? NodeReport::State::NotEvaluated
         : !k.error      ? NodeReport::State::Ok
         : k.inputFailed ? NodeReport::State::InputFailed
                         : NodeReport::State::Failed;
}

NodeReport Evaluator::report(const NodePath &path) const
{
  NodeReport r;
  const Frame *f = frameAt(path);
  const int at = f ? f->topo->find(path.node) : -1;
  if (at < 0) {
    return r;
  }
  auto add = [&r](const KernelState &k) {
    r.runs += k.runs;
    r.seconds += k.seconds;
    r.totalSeconds += k.totalSeconds;
    const NodeReport::State state = stateOf(k);
    if (worse(state, r.state)) {
      r.state = state;
      r.error = k.error ? *k.error : GraphError();
    }
  };
  Vector<const Frame *> inside;
  if (const KernelState *k = f->kernels[at]) {
    add(*k);
    r.current = k->visited && k->walk.done && stands(*f, k->walk.pass);
    addKernel(r.memory, *k);
    if (k->foreach) {
      r.elements = k->foreach->rows;
      r.cachedElements = k->foreach->cachedRows;
      k->foreach->forEachElement([&](const Element &e) {
        r.memory.elements += e.counts != nullptr;
        r.memory.bytes += sizeof(Element) + heapBytes(e.inputs) + heapBytes(e.outputs);
        inside.append(e.frame);
      });
    }
  } else if (const int group = f->topo->nodes[at].group; group >= 0) {
    const Frame::Group &g = f->groups[group];
    r.current = stands(*f, g.pass);
    if (g.child) {
      inside.append(g.child);
    }
    Vector<const Frame *> stack = inside;
    while (stack.size() > 0) {
      const Frame &c = *stack.pop_back();
      for (const KernelState *inner : c.kernels) {
        if (inner) {
          add(*inner);
        }
      }
      for (const Frame::Group &inner : c.groups) {
        if (inner.error && worse(NodeReport::State::Failed, r.state)) {
          r.state = NodeReport::State::Failed;
          r.error = *inner.error;
        }
        if (inner.child) {
          stack.append(inner.child);
        }
      }
    }
    if (g.error) {
      r.state = NodeReport::State::Failed;
      r.error = *g.error;
    }
  }
  addFrames(r.memory, inside);
  return r;
}

/* ---- dropping ---- */

void Evaluator::dropKernel(Frame &f, int node)
{
  KernelState *k = f.kernels[node];
  if (!k) {
    return;
  }
  markStale(Vertex{&f, node, true});
  /* Its inputs stay, and its outputs are there, as None: inputs may point to them. */
  for (KernelState::Output &o : k->outputs) {
    o.value = GraphValue();
  }
  k->ran = false;
  k->visited = false;
  k->inputFailed = false;
  k->runError = nullptr;
  k->error = nullptr;
  k->trace = Trace();
  if (f.deps) {
    f.deps->setBlocks(*k, rt_.ext<ClientRegistry>());
  }
  if (Foreach *s = k->foreach.get()) {
    s->clear();
    s->deps.clear();
    s->rows = s->cachedRows = 0;
  }
}

Status Evaluator::drop(const NodePath &path)
{
  CDCC_TRY(rt_.busyIfEvaluating("GraphInstance::drop"));
  if (path.elements.size() > 0) {
    return Status::error(StatusCode::Unsupported,
                         "GraphInstance::drop: elements go with their foreach node");
  }
  NoCollectScope noCollect(lib_.get()); /* as in clear */
  Frame *f = const_cast<Frame *>(frameAt(path));
  const int at = f ? f->topo->find(path.node) : -1;
  if (at < 0) {
    return Status::error(StatusCode::NotFound,
                         "GraphInstance::drop: nothing is kept of such a node");
  }
  dropKernel(*f, at);
  const int group = f->topo->nodes[at].group;
  if (group < 0 || !f->groups[group].child) {
    return Status();
  }
  /* First what depends on it, all over the instance, then the frames. */
  Vector<Frame *> stack;
  stack.append(f->groups[group].child);
  while (stack.size() > 0) {
    Frame *c = stack.pop_back();
    for (int i = 0; i < c->inputs.size(); i++) {
      markStale(Vertex{c, i, false});
    }
    for (int i = 0; i < c->kernels.size(); i++) {
      markStale(Vertex{c, i, true});
    }
    for (const Frame::Group &g : c->groups) {
      if (g.child) {
        stack.append(g.child);
      }
    }
  }
  dropFrame(f->groups[group].child);
  f->groups[group] = Frame::Group();
  layout_++;
  return Status();
}

} // namespace coredcc::graph
