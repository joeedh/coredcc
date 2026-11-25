#include "coredcc/graph/instance.h"

#include "coredcc/base/text.h"
#include "coredcc/graph/evaluator.h"

#include <algorithm>
#include <cstring>
#include <string>

namespace coredcc {

using litestl::util::Vector;

/* ---- paths ---- */

bool NodePath::operator==(const NodePath &b) const
{
  if (node != b.node || groups.size() != b.groups.size()) {
    return false;
  }
  for (int i = 0; i < int(groups.size()); i++) {
    if (groups[i] != b.groups[i]) {
      return false;
    }
  }
  return true;
}

bool NodePath::operator<(const NodePath &b) const
{
  const int n = std::min(int(groups.size()), int(b.groups.size()));
  for (int i = 0; i < n; i++) {
    if (groups[i] != b.groups[i]) {
      return groups[i] < b.groups[i];
    }
  }
  if (groups.size() != b.groups.size()) {
    return groups.size() < b.groups.size();
  }
  return node < b.node;
}

std::string NodePath::toString() const
{
  std::string s;
  for (NodeId id : groups) {
    s += std::to_string(id.value) + "/";
  }
  return s + std::to_string(node.value);
}

SocketPath SocketPath::output(std::string_view nameOrKey)
{
  SocketPath p;
  p.key = lsString(nameOrKey);
  return p;
}

SocketPath SocketPath::of(NodePath node, std::string_view key, SocketSide side)
{
  SocketPath p;
  p.node = std::move(node);
  p.key = lsString(key);
  p.side = side;
  return p;
}

SocketPath SocketPath::of(NodeId node, std::string_view key, SocketSide side)
{
  NodePath p;
  p.node = node;
  return of(std::move(p), key, side);
}

bool SocketPath::operator==(const SocketPath &b) const
{
  return side == b.side && node == b.node && strcmp(key.c_str(), b.key.c_str()) == 0;
}

uint64_t SocketPath::computeHash() const
{
  uint64_t h = detail::fnv1a64(key.c_str()) + uint64_t(side);
  for (NodeId id : node.groups) {
    h = (h ^ id.computeHash()) * 0xC2B2AE3D27D4EB4Full;
  }
  return (h ^ node.node.computeHash()) * 0xC2B2AE3D27D4EB4Full;
}

bool GraphError::operator==(const GraphError &b) const
{
  return status.code() == b.status.code() &&
         strcmp(status.message(), b.status.message()) == 0 && origin == b.origin &&
         elements.size() == b.elements.size() &&
         std::equal(elements.begin(), elements.end(), b.elements.begin()) &&
         strcmp(socket.c_str(), b.socket.c_str()) == 0;
}

/* ---- GraphInstance ---- */

GraphInstance::GraphInstance(Runtime *rt, Library *lib, GraphSource source)
    : e_(std::make_unique<graph::Evaluator>(rt, lib, source))
{
}

GraphInstance::~GraphInstance() = default;

Status GraphInstance::evaluate(std::span<const SocketPath> sockets, Vector<Status> &results)
{
  return e_->evaluate(sockets, results);
}

Status GraphInstance::evaluate(const SocketPath &socket)
{
  Vector<Status> results;
  CDCC_TRY(e_->evaluate({&socket, 1}, results));
  return results[0];
}

const GraphValue *GraphInstance::value(const SocketPath &socket) const
{
  const graph::Evaluator::Pulled *r = e_->result(socket);
  return r && !r->error ? &r->value : nullptr;
}

const GraphValue *GraphInstance::lastGood(const SocketPath &socket) const
{
  const graph::Evaluator::Pulled *r = e_->result(socket);
  return r && r->hasValue ? &r->value : nullptr;
}

uint64_t GraphInstance::changedAt(const SocketPath &socket) const
{
  const graph::Evaluator::Pulled *r = e_->result(socket);
  return r ? r->changedAt : 0;
}

const GraphError *GraphInstance::error(const SocketPath &socket) const
{
  const graph::Evaluator::Pulled *r = e_->result(socket);
  return r ? r->error.get() : nullptr;
}

bool GraphInstance::isCurrent(const SocketPath &socket) const
{
  const graph::Evaluator::Pulled *r = e_->result(socket);
  return r && r->key == e_->currentKey();
}

Status GraphInstance::setInput(std::string_view nameOrKey, GraphValue v)
{
  return e_->setInput(nameOrKey, std::move(v));
}

NodeReport GraphInstance::report(const NodePath &node) const
{
  return e_->report(node);
}

Status GraphInstance::retry()
{
  return e_->retry();
}

Status GraphInstance::clear()
{
  return e_->clear();
}

Status GraphInstance::forget(const SocketPath &socket)
{
  return e_->forget(socket);
}

void GraphInstance::setUserData(void *data)
{
  e_->userData = data;
}

void GraphInstance::setMaxFrames(int n)
{
  e_->maxFrames = n;
}

int GraphInstance::frameCount() const
{
  return e_->frameCount();
}

int GraphInstance::elementCount() const
{
  return e_->elementCount();
}

uint64_t GraphInstance::totalRuns() const
{
  return e_->totalRuns;
}

/* ---- the Evaluator's other calls ---- */

namespace graph {

Status Evaluator::setInput(std::string_view name, GraphValue v)
{
  CDCC_TRY(rt_.busyIfEvaluating("GraphInstance::setInput"));
  NoCollectScope noCollect(lib_.get()); /* as in clear */
  Status status;
  const GraphDef *def = findDef(status);
  CDCC_TRY(status);
  const InterfaceSocket *s = def->interfaceSocket(SocketSide::Input, name);
  for (const InterfaceSocket &i : def->interfaceSockets(SocketSide::Input)) {
    if (name == i.key().c_str()) {
      s = &i;
    }
  }
  if (!s && v.isNone()) {
    for (auto &pair : inputs_) {
      if (name == interfaceKey(pair.key).c_str()) {
        inputs_.remove(pair.key); /* of a removed input: nothing reads it */
        return Status();
      }
    }
  }
  if (!s) {
    status = Status::error(StatusCode::NotFound,
                           ("the graph has no input '" + std::string(name) + "'").c_str());
    Suggester sg(name);
    for (const InterfaceSocket &i : def->interfaceSockets(SocketSide::Input)) {
      sg.consider(i.name.c_str());
    }
    sg.addTo(status);
    return status;
  }
  External *old = inputs_.lookup_ptr(s->id);
  if (v.isNone()) {
    if (old) {
      inputs_.remove(s->id);
      own_++;
    }
    return Status();
  }
  const SocketTypeRegistry &types = rt_.ext<SocketTypeRegistry>();
  const SocketType *type = types.find(s->typeName.c_str());
  if (!type) {
    return types.unknown(s->typeName.c_str());
  }
  if (!type->holds(v)) {
    return Status::error(StatusCode::TypeMismatch,
                         ("input '" + std::string(name) + "' is a " + type->name.c_str() +
                          ", not a " + graphValueTypeName(v.type()))
                             .c_str());
  }
  if (old && old->value == v) {
    return Status();
  }
  inputs_.add_overwrite(s->id, External{std::move(v), rt_.nextVersion()});
  own_++;
  return Status();
}

NodeReport Evaluator::report(const NodePath &path) const
{
  NodeReport r;
  const Frame *f = root_.get();
  for (int i = 0; f && f->topo && i < int(path.groups.size()); i++) {
    const int at = f->topo->find(path.groups[i]);
    const int group = at >= 0 ? f->topo->nodes[at].group : -1;
    f = group >= 0 ? f->groups[group].child : nullptr;
  }
  const int at = f && f->topo ? f->topo->find(path.node) : -1;
  if (at < 0) {
    return r;
  }
  ErrorPtr error;
  if (const KernelState *k = f->kernels[at]) {
    r.runs = k->runs;
    r.seconds = k->seconds;
    if (k->visited) {
      error = k->error;
      r.state = !error         ? NodeReport::State::Ok :
                k->inputFailed ? NodeReport::State::InputFailed :
                                 NodeReport::State::Failed;
    }
  }
  else if (f->topo->nodes[at].group >= 0) {
    error = f->groups[f->topo->nodes[at].group].error;
    if (error) {
      r.state = NodeReport::State::Failed;
    }
  }
  if (error) {
    r.error = *error;
  }
  return r;
}

Status Evaluator::retry()
{
  CDCC_TRY(rt_.busyIfEvaluating("GraphInstance::retry"));
  Vector<Frame *> stack;
  if (root_) {
    stack.append(root_.get());
  }
  while (stack.size() > 0) {
    Frame *f = stack.pop_back();
    for (KernelState *k : f->kernels) {
      if (k && k->runError) {
        k->ran = false;
      }
    }
    f->forEachElement([&](Element &e) { stack.append(e.frame); });
    for (const Frame::Group &g : f->groups) {
      if (g.child) {
        stack.append(g.child);
      }
    }
  }
  own_++;
  return Status();
}

Status Evaluator::clear()
{
  CDCC_TRY(rt_.busyIfEvaluating("GraphInstance::clear"));
  /* The last pin of a block that owns this instance may go: only after the scope. */
  NoCollectScope noCollect(lib_.get());
  root_ = nullptr;
  results_.clear();
  passKey_ = Key();
  Status status;
  if (const GraphDef *def = findDef(status)) {
    Vector<int64_t> gone;
    for (auto &pair : inputs_) {
      const InterfaceSocket *s = def->interfaceSocket(pair.key);
      if (!s) {
        gone.append(pair.key);
      }
    }
    for (int64_t id : gone) {
      inputs_.remove(id);
    }
  }
  return Status();
}

Status Evaluator::forget(const SocketPath &socket)
{
  CDCC_TRY(rt_.busyIfEvaluating("GraphInstance::forget"));
  NoCollectScope noCollect(lib_.get()); /* as in clear */
  results_.remove(socket);
  return Status();
}

} // namespace graph
} // namespace coredcc
