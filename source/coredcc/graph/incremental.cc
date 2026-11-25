/* What is kept between evaluate calls: after an edit, the vertices it touches and what
 * depends on them are stale, and the rest isn't looked at. And prepared sockets. */
#include "coredcc/graph/evaluator.h"

#include "coredcc/lib/library.h"

#include <algorithm>

namespace coredcc::graph {

using litestl::util::Vector;

/* ---- edits ---- */

/* Whether the journal says all that happened between two keys. */
static bool
journalled(const Evaluator::Key &a, const Evaluator::Key &b, const GraphSystem &system)
{
  return a.document.epoch == b.document.epoch &&
         a.document.clients == b.document.clients &&
         a.document.ifaceTables == b.document.ifaceTables && system.journalCurrent();
}

bool Evaluator::untouched(const Key &key) const
{
  if (key == passKey_) {
    return true;
  }
  if (!incremental) {
    return false;
  }
  if (!(key == askedKey_)) {
    askedKey_ = key;
    answer_ = !anyConcerns(key);
  }
  return answer_;
}

bool Evaluator::anyConcerns(const Key &key) const
{
  const GraphSystem &system = rt_.ext<GraphSystem>();
  if (key.own != passKey_.own || !journalled(key, passKey_, system)) {
    return true;
  }
  for (uint64_t i = seen_; i < system.journalEnd(); i++) {
    const GraphEditNote *e = system.journal(i);
    if (!e || concerns(*e)) {
      return true;
    }
  }
  return false;
}

bool Evaluator::concerns(const GraphEditNote &e) const
{
  switch (e.kind) {
  case GraphEditNote::Kind::All:
    return true;
  case GraphEditNote::Kind::Block: {
    const BlockLink *l = deps_.kernelsOf(e.block);
    while (l && !(l->aspects & e.aspects)) {
      l = l->next;
    }
    return l != nullptr;
  }
  default:
    return reaches(e.def);
  }
}

bool Evaluator::reaches(uint64_t defSerial) const
{
  if (reachablePass_ != pass_) {
    findReachable();
  }
  return reachable_.lookup_ptr(defSerial) != nullptr;
}

/* Which defs there are can't change without a new pass: references are the Library's. */
void Evaluator::findReachable() const
{
  reachablePass_ = pass_;
  reachable_.clear();
  interior_ = false;
  Vector<const GraphDef *> list;
  auto visit = [&](const GraphDef *d) {
    if (d && reachable_.add(d->serial(), true)) {
      list.append(d);
    }
  };
  visit(root_->def);
  for (int i = 0; i < int(list.size()); i++) {
    const GraphDef *d = list[i];
    const std::shared_ptr<const GraphTopology> topo = d->topology();
    for (const GraphTopology::Group &g : topo->groups) {
      interior_ |= g.interior.size() > 0;
      visit(d->groupTarget(*g.node));
    }
  }
}

bool Evaluator::absorb(const Key &key)
{
  const GraphSystem &system = rt_.ext<GraphSystem>();
  if (!incremental || ownAll_ || !root_->topo || !journalled(key, passKey_, system)) {
    return false;
  }
  for (uint64_t i = seen_; i < system.journalEnd(); i++) {
    const GraphEditNote *e = system.journal(i);
    if (!e || !absorb(*e)) {
      return false;
    }
  }
  /* This instance's own inputs, at the root's links from graph.input nodes. */
  const Vector<InterfaceSocket> &list = root_->def->interfaceSockets(SocketSide::Input);
  for (int64_t id : ownInputs_) {
    for (int in : root_->topo->fromInterface) {
      const int index = root_->topo->inputs[in].fromOutput;
      if (index >= int(list.size()) || list[index].id == id) {
        markStale(Vertex{root_.get(), in, false});
      }
    }
  }
  return true;
}

bool Evaluator::absorb(const GraphEditNote &e)
{
  if (!concerns(e)) {
    return true;
  }
  auto markForeachs = [&]() {
    for (auto &pair : deps_.foreachs) {
      KernelState &k = *pair.key;
      if (k.foreach->serials.lookup_ptr(e.def)) {
        markStale(Vertex{k.frame, k.index, true});
      }
    }
  };
  switch (e.kind) {
  case GraphEditNote::Kind::Block:
    for (const BlockLink *l = deps_.kernelsOf(e.block); l; l = l->next) {
      if (l->aspects & e.aspects) {
        markStale(Vertex{l->kernel->frame, l->kernel->index, true});
      }
    }
    return true;
  case GraphEditNote::Kind::Node:
    for (Frame *f = deps_.framesOf(e.def); f; f = f->nextOfDef) {
      const int at = f->topo ? f->topo->find(NodeId{e.node}) : -1;
      if (at < 0) {
        continue; /* a new one */
      }
      const GraphTopology::Node &n = f->topo->nodes[at];
      for (int i = 0; i < n.inCount; i++) {
        markStale(Vertex{f, n.inBegin + i, false});
      }
      markStale(Vertex{f, at, true});
    }
    markForeachs();
    return true;
  case GraphEditNote::Kind::Links:
    if (interior_) {
      return false; /* other defs' sockets may have come or gone */
    }
    for (Frame *f = deps_.framesOf(e.def); f; f = f->nextOfDef) {
      markFrame(*f);
    }
    markForeachs();
    return true;
  default:
    return false;
  }
}

void Evaluator::markFrame(Frame &f)
{
  for (int i = 0; i < f.inputs.size(); i++) {
    markStale(Vertex{&f, i, false});
  }
  /* Its topology is fetched again when something comes by: from the group node, or from
   * find, which every request needs again. */
  f.pass = 0;
  if (f.parent) {
    /* By id: its index may be of an older topology, if nothing came by since. */
    const GraphTopology &t = *f.parent->topo;
    f.parent->groups[t.nodes[t.find(f.groupNode)].group].pass = 0;
  }
  layout_++;
}

void Evaluator::markStale(const Vertex &v)
{
  marks_.append(v);
  while (marks_.size() > 0) {
    const Vertex m = marks_.pop_back();
    Frame &f = *m.frame;
    KernelState *k = m.kernel ? f.kernels[m.index] : nullptr;
    if (m.kernel && !k) {
      continue;
    }
    WalkState &w = m.kernel ? k->walk : f.inputs[m.index].walk;
    if (w.pass != pass_) {
      continue; /* not visited in this pass, or stale: then so is what depends on it */
    }
    w.pass = 0;
    if (!m.kernel) {
      const int node = f.topo->inputs[m.index].node;
      if (isOneStep(f.topo->nodes[node].role)) {
        marks_.append(Vertex{&f, node, true});
      }
    }
    for (InputLink *l = m.kernel ? k->consumers : f.links[m.index].consumers; l;
         l = l->next)
    {
      marks_.append(Vertex{l->frame, l->index(), false});
    }
  }
}

/* ---- prepared sockets ---- */

Result<SocketHandle> Evaluator::prepare(const SocketPath &socket)
{
  CDCC_TRY(rt_.busyIfEvaluating("GraphInstance::prepare"));
  uint32_t at = uint32_t(prepared_.size());
  if (freePrepared_.size() > 0) {
    at = freePrepared_.pop_back();
  } else {
    prepared_.append(Prepared());
  }
  Prepared &p = prepared_[int(at)];
  p.path = socket;
  p.used = true;
  p.generation++;
  return SocketHandle{at, p.generation};
}

const Evaluator::Pulled *Evaluator::result(SocketHandle h) const
{
  if (h.index >= uint32_t(prepared_.size())) {
    return nullptr;
  }
  const Prepared &p = prepared_[int(h.index)];
  return p.used && p.generation == h.generation ? &p.pulled : nullptr;
}

Status Evaluator::release(SocketHandle h)
{
  CDCC_TRY(rt_.busyIfEvaluating("GraphInstance::release"));
  if (!result(h)) {
    return Status::error(StatusCode::InvalidArgument,
                         "GraphInstance::release: the handle isn't valid");
  }
  NoCollectScope noCollect(lib_.get()); /* as in clear */
  Prepared &p = prepared_[int(h.index)];
  p.path = SocketPath();
  p.pulled = Pulled();
  p.reported = 0;
  p.used = false;
  freePrepared_.append(h.index);
  return Status();
}

Status Evaluator::evaluate(std::span<const SocketHandle> sockets, Vector<int> &moved)
{
  moved.clear();
  CDCC_TRY(begin());
  for (const SocketHandle &h : sockets) {
    if (!result(h)) {
      return Status::error(StatusCode::InvalidArgument,
                           "GraphInstance::evaluate: a handle isn't valid");
    }
  }
  NoCollectScope noCollect(lib_.get());
  if (currentKey() == failedKey_ && failedHandles_.size() > 0 &&
      sockets.size() >= size_t(failedHandles_.size()) &&
      std::equal(failedHandles_.begin(), failedHandles_.end(), sockets.begin()))
  {
    return failed_;
  }
  CDCC_TRY(prepareCall());
  EvaluationScope scope(rt_);
  Status ignored;
  int i = 0;
  for (; i < int(sockets.size()); i++) {
    Prepared &p = prepared_[int(sockets[size_t(i)].index)];
    pull(p.path, p.pulled, ignored);
    if (overBudget_) {
      break;
    }
    if (p.pulled.changedAt != p.reported) {
      p.reported = p.pulled.changedAt;
      moved.append(i);
    }
  }
  if (overBudget_) {
    for (int m : moved) { /* not told, so next time */
      prepared_[int(sockets[size_t(m)].index)].reported = 0;
    }
    moved.clear();
    failedPaths_.clear();
    failedHandles_.clear();
    for (int j = 0; j <= i; j++) {
      failedHandles_.append(sockets[size_t(j)]);
    }
  }
  return endCall();
}

} // namespace coredcc::graph
