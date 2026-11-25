#include "coredcc/graph/instance.h"

#include "coredcc/base/text.h"
#include "coredcc/graph/evaluator.h"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <string>

namespace coredcc {

using litestl::util::Vector;

/* ---- paths ---- */

bool NodePath::operator==(const NodePath &b) const
{
  return node == b.node && groups.size() == b.groups.size() &&
         elements.size() == b.elements.size() &&
         std::equal(groups.begin(), groups.end(), b.groups.begin()) &&
         std::equal(elements.begin(), elements.end(), b.elements.begin());
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
  s += std::to_string(node.value);
  for (int i = 0; i < int(elements.size()); i++) {
    s += (i ? ", " : "[") + std::to_string(elements[i]);
  }
  return elements.size() > 0 ? s + "]" : s;
}

Result<NodePath> NodePath::parse(std::string_view text)
{
  const char *at = text.data(), *end = at + text.size();
  auto number = [&](int64_t &out) {
    const std::from_chars_result r = std::from_chars(at, end, out);
    const bool ok = r.ec == std::errc();
    at = ok ? r.ptr : at;
    return ok;
  };
  auto skip = [&](std::string_view s) {
    const bool there = std::string_view(at, size_t(end - at)).substr(0, s.size()) == s;
    at += there ? s.size() : 0;
    return there;
  };
  NodePath p;
  bool ok = number(p.node.value) && p.node.value > 0;
  while (ok && skip("/")) {
    p.groups.append(p.node);
    ok = number(p.node.value) && p.node.value > 0;
  }
  if (ok && skip("[")) {
    do {
      p.elements.append(0);
      ok = number(p.elements.last());
    } while (ok && skip(", "));
    ok = ok && skip("]");
  }
  if (!ok || at != end) {
    return Status::error(
        StatusCode::InvalidArgument,
        ("'" + std::string(text) + "' isn't the path of a node, as 12/34 or 12/34[7]")
            .c_str());
  }
  return p;
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
         strcmp(socket.c_str(), b.socket.c_str()) == 0;
}

/* ---- GraphInstance ---- */

GraphInstance::GraphInstance(Runtime *rt, Library *lib, GraphSource source)
    : e_(std::make_unique<graph::Evaluator>(rt, lib, source))
{
}

GraphInstance::~GraphInstance() = default;

Status GraphInstance::evaluate(std::span<const SocketPath> sockets,
                               Vector<Status> &results)
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
  return r && r->hasValue && !r->error ? &r->value : nullptr;
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
  return r && e_->isCurrent(*r);
}

Status GraphInstance::setInput(std::string_view nameOrKey, GraphValue v)
{
  return e_->setInput(nameOrKey, std::move(v));
}

NodeReport GraphInstance::report(const NodePath &node) const
{
  return e_->report(node);
}

GraphValue GraphInstance::peek(const SocketPath &socket) const
{
  return e_->peek(socket);
}

Status GraphInstance::drop(const NodePath &node)
{
  return e_->drop(node);
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

Status GraphInstance::setMonitor(GraphMonitor monitor)
{
  return e_->setMonitor(std::move(monitor));
}

void GraphInstance::setUserData(void *data)
{
  e_->userData = data;
}

void GraphInstance::setMaxFrames(int n)
{
  e_->setMaxFrames(n);
}

void GraphInstance::setMaxCachedKernels(int n)
{
  e_->maxCachedKernels = n;
}

void GraphInstance::setIncremental(bool on)
{
  e_->incremental = on;
}

uint64_t GraphInstance::totalVisits() const
{
  return e_->visits;
}

uint64_t GraphInstance::totalFullPasses() const
{
  return e_->fullPasses;
}

GraphMemory GraphInstance::memory() const
{
  return e_->memory();
}

int GraphInstance::frameCount() const
{
  return e_->frameCount();
}

int GraphInstance::elementCount() const
{
  return e_->elementCounts().elements;
}

/* ---- prepared sockets ---- */

Result<SocketHandle> GraphInstance::prepare(const SocketPath &socket)
{
  return e_->prepare(socket);
}

Status GraphInstance::release(SocketHandle socket)
{
  return e_->release(socket);
}

Status GraphInstance::evaluate(std::span<const SocketHandle> sockets, Vector<int> &moved)
{
  return e_->evaluate(sockets, moved);
}

Status GraphInstance::status(SocketHandle socket) const
{
  const graph::Evaluator::Pulled *r = e_->result(socket);
  if (!r || r->at == 0) {
    return Status::error(StatusCode::FailedPrecondition,
                         r ? "the socket wasn't evaluated yet"
                           : "the handle isn't valid");
  }
  return r->error ? r->error->status : Status();
}

const GraphValue *GraphInstance::value(SocketHandle socket) const
{
  const graph::Evaluator::Pulled *r = e_->result(socket);
  return r && r->hasValue && !r->error ? &r->value : nullptr;
}

const GraphValue *GraphInstance::lastGood(SocketHandle socket) const
{
  const graph::Evaluator::Pulled *r = e_->result(socket);
  return r && r->hasValue ? &r->value : nullptr;
}

uint64_t GraphInstance::changedAt(SocketHandle socket) const
{
  const graph::Evaluator::Pulled *r = e_->result(socket);
  return r ? r->changedAt : 0;
}

const GraphError *GraphInstance::error(SocketHandle socket) const
{
  const graph::Evaluator::Pulled *r = e_->result(socket);
  return r ? r->error.get() : nullptr;
}

bool GraphInstance::isCurrent(SocketHandle socket) const
{
  const graph::Evaluator::Pulled *r = e_->result(socket);
  return r && e_->isCurrent(*r);
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
    status =
        Status::error(StatusCode::NotFound,
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
      ownInputs_.append(s->id);
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
  const int64_t id = s->id;
  inputs_.add_overwrite(id, External{std::move(v), rt_.nextVersion()});
  own_++;
  ownInputs_.append(id);
  return Status();
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
  ownAll_ = true;
  return Status();
}

Status Evaluator::clear()
{
  CDCC_TRY(rt_.busyIfEvaluating("GraphInstance::clear"));
  /* The last pin of a block that owns this instance may go: only after the scope. */
  NoCollectScope noCollect(lib_.get());
  root_ = nullptr;
  results_.clear();
  for (Prepared &p : prepared_) {
    p.pulled = Pulled();
    p.reported = 0;
  }
  chains_ = TypeChains(); /* nothing has any */
  dropPass();
  failedKey_ = Key();
  layout_++;
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

Status Evaluator::setMonitor(GraphMonitor monitor)
{
  CDCC_TRY(rt_.busyIfEvaluating("GraphInstance::setMonitor"));
  monitor_ = std::move(monitor);
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
