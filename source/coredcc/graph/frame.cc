/* Frames: making them, keeping them up to date with their defs, and dropping them. */
#include "coredcc/graph/evaluator.h"

#include "coredcc/base/assert.h"
#include "coredcc/graph/provider.h"
#include "coredcc/lib/library.h"

#include <algorithm>
#include <string>

namespace coredcc::graph {

using litestl::util::Vector;

namespace {

/** Groups nested deeper than this fail. The root is at depth 0. */
constexpr int kMaxDepth = 64;

} // namespace

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
  frameCount -= f->count();
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
  }
}

GraphDef *Evaluator::findDef(Status &status) const
{
  if (source_.def) {
    return source_.def;
  }
  const Library *lib = lib_.get();
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
    frameCount = 0;
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
      if (at >= 0 && f.topo->nodes[at].role == NodeRole::Kernel &&
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

KernelState &Evaluator::kernel(Frame &f, int node)
{
  KernelState *&k = f.kernels[node];
  if (!k) {
    k = litestl::alloc::New<KernelState>("coredcc::KernelState");
    k->type = f.topo->nodes[node].type;
    if (k->type) {
      k->outputs.resize(size_t(k->type->outputs.size()));
    }
  }
  return *k;
}

Frame *Evaluator::child(Frame &f, int group, ErrorPtr &error)
{
  Frame::Group &g = f.groups[group];
  if (g.pass == pass_) {
    error = g.error;
    return g.error ? nullptr : g.child;
  }
  g.error = nullptr;
  const GraphNode &node = *f.topo->groups[group].node;
  GraphDef *def = f.def->groupTarget(node);
  Status status;
  if (!def) {
    status = Status::error(StatusCode::NotFound,
                           node.group().isNull() ? "the group has no graph" :
                                                   "the group's graph isn't there");
  }
  for (const Frame *a = &f; def && status.ok() && a; a = a->parent) {
    if (a->def == def) {
      status = Status::error(StatusCode::FailedPrecondition, "the graph contains itself");
    }
  }
  if (status.ok() && f.depth + 1 > kMaxDepth) {
    status = Status::error(
        StatusCode::OutOfRange,
        ("graphs are nested deeper than " + std::to_string(kMaxDepth)).c_str());
  }
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
    if (frameCount >= maxFrames) {
      overBudget_ = true;
      return nullptr;
    }
    Frame *c = litestl::alloc::New<Frame>("coredcc::GraphFrame");
    c->def = def;
    c->defSerial = def->serial();
    c->parent = &f;
    c->groupNode = node.id();
    c->depth = f.depth + 1;
    c->serial = rt_.nextVersion();
    c->call = call_;
    g.child = c;
    frameCount++;
  }
  g.child->groupIndex = f.topo->groups[group].index;
  sync(*g.child);
  g.pass = pass_;
  return g.child;
}

} // namespace coredcc::graph
