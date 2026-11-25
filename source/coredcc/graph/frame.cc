/* Frames: making them, keeping them up to date with their defs, and dropping them. */
#include "coredcc/graph/evaluator.h"

#include "coredcc/base/assert.h"
#include "coredcc/graph/provider.h"
#include "coredcc/lib/library.h"

#include <algorithm>
#include <string>

namespace coredcc::graph {

using litestl::util::Vector;

KernelState::KernelState() = default;
KernelState::~KernelState() = default;

Frame::~Frame()
{
  for (KernelState *k : kernels) {
    litestl::alloc::Delete(k);
  }
  for (Group &g : groups) {
    litestl::alloc::Delete(g.child);
  }
}

NodePath Frame::pathOf(NodeId node) const
{
  NodePath p;
  p.node = node;
  for (const Frame *f = this; f->parent; f = f->parent) {
    p.groups.append(f->groupNode);
  }
  std::reverse(p.groups.begin(), p.groups.end());
  return p;
}

int Frame::count() const
{
  int n = 1;
  for (const Group &g : groups) {
    n += g.child ? g.child->count() : 0;
  }
  return n;
}

ErrorPtr Evaluator::makeError(const Frame &f,
                              NodeId node,
                              Status status,
                              std::string_view socket) const
{
  GraphError e;
  e.status = std::move(status);
  e.origin = f.pathOf(node);
  e.socket = std::string(socket).c_str();
  return std::make_shared<const GraphError>(std::move(e));
}

void Evaluator::dropFrame(Frame *f)
{
  f->top->frames -= f->count();
  litestl::alloc::Delete(f);
}

void Evaluator::dropNewFrames()
{
  Vector<Frame *> stack;
  stack.append(root_.get());
  while (stack.size() > 0) {
    Frame *f = stack.pop_back();
    for (Frame::Group &g : f->groups) {
      if (g.child && g.child->call == call_) {
        dropFrame(g.child);
        g.child = nullptr;
      }
      else if (g.child) {
        stack.append(g.child);
      }
    }
    f->forEachElement([&](Element &e) { stack.append(e.frame); });
  }
}

GraphDef *Evaluator::findDef(Status &status) const
{
  const Library *lib = lib_.get();
  if (GraphDef *def = source_.def) {
    if (&def->runtime() != &rt_ || (def->library() && def->library() != lib)) {
      status = Status::error(StatusCode::InvalidArgument,
                             "the instance's graph is of another Runtime or Library");
      return nullptr;
    }
    return def;
  }
  if (lib && &lib->runtime() != &rt_) {
    status = Status::error(StatusCode::InvalidArgument,
                           "the instance's Library is of another Runtime");
    return nullptr;
  }
  GraphDef *def = lib ? graphDefOf(*lib, lib->find(source_.block)) : nullptr;
  if (!def) {
    status = Status::error(StatusCode::NotFound, "the instance's graph isn't there");
  }
  return def;
}

Status Evaluator::prepareRoot()
{
  Status status;
  GraphDef *def = findDef(status);
  CDCC_TRY(status);
  if (!root_ || root_->def != def || root_->defSerial != def->serial()) {
    root_ = std::make_unique<Frame>();
    root_->top = root_.get();
    root_->def = def;
    root_->defSerial = def->serial();
    root_->serial = rt_.nextVersion();
  }
  return Status();
}

void Evaluator::sync(Frame &f)
{
  if (f.pass == pass_) {
    return;
  }
  f.pass = pass_;
  std::shared_ptr<const GraphTopology> topo = f.def->topology();
  if (f.topo == topo) {
    return;
  }
  /* What was kept for a node goes to its new place, or goes. */
  const std::shared_ptr<const GraphTopology> old = std::move(f.topo);
  Vector<KernelState *, 2> kernels = std::move(f.kernels);
  Vector<Frame::Group, 1> groups = std::move(f.groups);
  f.topo = std::move(topo);
  f.inputs = {};
  f.inputs.resize(size_t(f.topo->inputs.size()));
  f.kernels = {};
  for (int i = 0; i < int(f.topo->nodes.size()); i++) {
    f.kernels.append(nullptr);
  }
  f.groups = {};
  f.groups.resize(size_t(f.topo->groups.size()));
  for (int i = 0; i < int(kernels.size()); i++) {
    if (KernelState *k = kernels[i]) {
      const int at = f.topo->find(old->nodes[i].id);
      if (at >= 0 && isOneStep(f.topo->nodes[at].role) &&
          f.topo->nodes[at].type == k->type)
      {
        f.kernels[at] = k;
      }
      else {
        litestl::alloc::Delete(k);
      }
    }
  }
  for (int i = 0; i < int(groups.size()); i++) {
    if (Frame *c = groups[i].child) {
      const int at = f.topo->find(old->nodes[old->groups[i].index].id);
      const int group = at >= 0 ? f.topo->nodes[at].group : -1;
      if (group >= 0) {
        f.groups[group].child = c;
      }
      else {
        dropFrame(c);
      }
    }
  }
}

void Evaluator::pinDef(Frame &f)
{
  Library *lib = f.def->library();
  if (f.pinned == call_ || !lib || f.def->self().isNull()) {
    return;
  }
  f.pinned = call_;
  pins_.append(LibraryPin(*lib, lib->find(f.def->self())));
}

KernelState &Evaluator::kernel(Frame &f, int node)
{
  KernelState *&k = f.kernels[node];
  if (!k) {
    k = litestl::alloc::New<KernelState>("coredcc::KernelState");
    k->type = f.topo->nodes[node].type;
    if (k->type && k->type->role == NodeRole::Foreach) {
      k->outputs.resize(1);
      k->foreach = std::make_unique<Foreach>();
    }
    else if (k->type) {
      k->outputs.resize(size_t(k->type->outputs.size()));
    }
  }
  return *k;
}

Status Evaluator::checkGroup(const Frame &f, const GraphNode &node, const GraphDef *def) const
{
  if (!def) {
    return Status::error(StatusCode::NotFound,
                         node.group().isNull() ? "the group has no graph" :
                                                 "the group's graph isn't there");
  }
  for (const Frame *a = &f; a; a = a->parent) {
    if (a->def == def) {
      return Status::error(StatusCode::FailedPrecondition, "the graph contains itself");
    }
  }
  if (f.depth + 1 > kMaxDepth) {
    return Status::error(
        StatusCode::OutOfRange,
        ("graphs are nested deeper than " + std::to_string(kMaxDepth)).c_str());
  }
  return Status();
}

Frame *Evaluator::newFrame(Frame &parent, const GraphNode &node, GraphDef *def)
{
  Frame *f = litestl::alloc::New<Frame>("coredcc::GraphFrame");
  f->def = def;
  f->defSerial = def->serial();
  f->parent = &parent;
  f->top = parent.top;
  f->groupNode = node.id();
  f->depth = parent.depth + 1;
  f->serial = rt_.nextVersion();
  f->call = call_;
  return f;
}

void Evaluator::setReplaced(Frame &f, int group, Frame &c)
{
  c.replacing.clear();
  c.replaced = nullptr;
  const GraphTopology::Group &g = f.topo->groups[group];
  auto add = [&c](const InteriorKey &key, int depth, const Vertex &by) {
    if (depth < int(key.groups.size())) {
      c.replacing.append(Frame::Replacing{&key, depth, by});
      return;
    }
    const int at = c.topo->find(key.node);
    const int input = at >= 0 ? c.topo->inputIndex(c.topo->nodes[at], key.key.c_str()) : -1;
    if (input >= 0) {
      if (!c.replaced) {
        c.replaced = std::make_unique<litestl::util::Map<int, Vertex>>();
      }
      c.replaced->add_overwrite(c.topo->nodes[at].inBegin + input, by);
    }
  };
  for (const GraphTopology::Interior &i : g.interior) {
    if (i.side == SocketSide::Input && i.index >= 0) {
      add(i.key, 0, Vertex{&f, f.topo->nodes[g.index].inBegin + i.index, false});
    }
  }
  /* After the group's own, so that the outermost counts. */
  for (const Frame::Replacing &r : f.replacing) {
    if (r.key->groups[r.depth] == g.node->id()) {
      add(*r.key, r.depth + 1, r.by);
    }
  }
}

Frame *Evaluator::child(Frame &f, int group, ErrorPtr &error)
{
  Frame::Group &g = f.groups[group];
  if (g.pass == pass_) {
    error = g.error;
    if (!g.error) {
      pinDef(*g.child);
    }
    return g.error ? nullptr : g.child;
  }
  g.error = nullptr;
  const GraphNode &node = *f.topo->groups[group].node;
  GraphDef *def = f.def->groupTarget(node);
  Status status = checkGroup(f, node, def);
  if (!status.ok()) {
    g.pass = pass_;
    g.error = error = makeError(f, node.id(), std::move(status));
    return nullptr;
  }
  if (g.child && (g.child->def != def || g.child->defSerial != def->serial())) {
    dropFrame(g.child);
    g.child = nullptr;
  }
  if (!g.child) {
    if (f.top->frames >= maxFrames) {
      overBudget_ = true;
      return nullptr;
    }
    g.child = newFrame(f, node, def);
    f.top->frames++;
  }
  g.child->groupIndex = f.topo->groups[group].index;
  pinDef(*g.child);
  sync(*g.child);
  setReplaced(f, group, *g.child);
  g.pass = pass_;
  return g.child;
}

} // namespace coredcc::graph
