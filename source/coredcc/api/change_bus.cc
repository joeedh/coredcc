#include "coredcc/api/change_bus.h"

#include "coredcc/api/resolve.h"
#include "coredcc/base/text.h"
#include "coredcc/parse/path_cache.h"

#include <cstring>

namespace coredcc {

using litestl::util::Vector;

namespace {

struct Change {
  OwnerKey owner;
  litestl::util::string member; /* empty: the whole owner */
};

struct Watcher {
  uint64_t id = 0;
  bool live = true;
  Context *ctx = nullptr;
  Path path;
  std::shared_ptr<ChangeBus::WatchFn> fn;
  PropValue last;
  Status lastStatus;
  Vector<PathTouch, 4> touched;
};

bool hits(const Vector<PathTouch, 4> &touched, const Vector<Change> &changes)
{
  for (const Change &c : changes) {
    for (const PathTouch &t : touched) {
      if (t.owner == c.owner &&
          (c.member.size() == 0 || strcmp(c.member.c_str(), t.member.c_str()) == 0))
      {
        return true;
      }
    }
  }
  return false;
}

/* Reads the watcher's value now, recording what the resolution touched. A failure
 * keeps the touches up to the failing segment, so a fix along them refires. */
void read(Watcher &w)
{
  Result<ResolvedPath> rp = resolve(*w.ctx, w.path);
  if (!rp.ok()) {
    w.last = PropValue();
    w.lastStatus = rp.status();
    w.touched.clear();
    return;
  }
  Result<PropValue> v = getValue(*w.ctx, *rp);
  w.touched = std::move(rp->touched);
  w.last = v.ok() ? *v : PropValue();
  w.lastStatus = v.ok() ? Status() : v.status();
}

} // namespace

struct ChangeBus::State final : detail::SignalStateBase {
  Vector<Watcher> watchers;
  Vector<Change> pending;
  uint64_t nextId = 1;
  uint64_t epoch = 0;
  uint64_t flushedEpoch = 0;
  int flushDepth = 0;
  bool needsCompact = false;

  void disconnect(uint64_t id) override
  {
    for (Watcher &w : watchers) {
      if (w.id == id && w.live) {
        w.live = false;
        needsCompact = true;
      }
    }
    if (flushDepth == 0) {
      compact();
    }
  }
  bool connected(uint64_t id) const override
  {
    for (const Watcher &w : watchers) {
      if (w.id == id) {
        return w.live;
      }
    }
    return false;
  }
  void compact()
  {
    if (!needsCompact) {
      return;
    }
    Vector<Watcher> kept;
    for (Watcher &w : watchers) {
      if (w.live) {
        kept.append(std::move(w));
      }
    }
    watchers = std::move(kept);
    needsCompact = false;
  }
};

ChangeBus::ChangeBus(Runtime *) : state_(std::make_shared<State>()) {}

ChangeBus::~ChangeBus() = default;

void ChangeBus::notify(const OwnerKey &owner, std::string_view member)
{
  if (owner.isNull()) {
    return;
  }
  state_->pending.append(Change{owner, lsString(member)});
}

void ChangeBus::bumpEpoch()
{
  state_->epoch++;
}

uint64_t ChangeBus::epoch() const
{
  return state_->epoch;
}

int ChangeBus::pendingCount() const
{
  return int(state_->pending.size());
}

Result<ScopedConnection> ChangeBus::watch(Context &ctx, const Path &path, WatchFn fn)
{
  CDCC_CHECK(bool(fn), "ChangeBus::watch: an empty callback");
  Watcher w;
  w.id = state_->nextId++;
  w.ctx = &ctx;
  w.path = path;
  w.fn = std::make_shared<WatchFn>(std::move(fn));
  read(w);
  const uint64_t id = w.id;
  state_->watchers.append(std::move(w));
  return ScopedConnection(Connection(state_, id));
}

Result<ScopedConnection> ChangeBus::watch(Context &ctx, std::string_view path, WatchFn fn)
{
  Result<PathPtr> p = ctx.runtime().ext<PathCache>().get(path);
  if (!p.ok()) {
    return p.status();
  }
  return watch(ctx, **p, std::move(fn));
}

int ChangeBus::watcherCount() const
{
  int n = 0;
  for (const Watcher &w : state_->watchers) {
    n += w.live ? 1 : 0;
  }
  return n;
}

int ChangeBus::flush()
{
  /* Keep the state alive even if a callback destroys the bus's owner. */
  std::shared_ptr<State> st = state_;
  Vector<Change> changes = std::move(st->pending);
  st->pending.clear();
  const bool epochMoved = st->epoch != st->flushedEpoch;
  st->flushedEpoch = st->epoch;
  if (changes.size() == 0 && !epochMoved) {
    return 0;
  }
  st->flushDepth++;
  int fired = 0;
  /* Watchers added by callbacks are past `n` and wait for the next flush. Index, don't
   * hold references: callbacks can append and reallocate. */
  const int n = int(st->watchers.size());
  for (int i = 0; i < n; i++) {
    if (!st->watchers[i].live) {
      continue;
    }
    if (!epochMoved && !hits(st->watchers[i].touched, changes) &&
        st->watchers[i].lastStatus.ok())
    {
      continue;
    }
    const PropValue before = st->watchers[i].last;
    const bool wasOk = st->watchers[i].lastStatus.ok();
    read(st->watchers[i]);
    const Watcher &w = st->watchers[i];
    if (w.last == before && w.lastStatus.ok() == wasOk) {
      continue;
    }
    std::shared_ptr<WatchFn> fn = w.fn;
    const PropValue value = w.last;
    const Status status = w.lastStatus;
    (*fn)(value, status);
    fired++;
  }
  st->flushDepth--;
  if (st->flushDepth == 0) {
    st->compact();
  }
  return fired;
}

Status ChangeBus::validate() const
{
  for (const Watcher &w : state_->watchers) {
    if (w.live && (!w.ctx || !w.fn)) {
      return Status::error(StatusCode::Internal, "ChangeBus: a live watcher has no context");
    }
  }
  return Status();
}

} // namespace coredcc
