#include "coredcc/api/change_bus.h"

#include "coredcc/api/resolve.h"
#include "coredcc/base/text.h"
#include "coredcc/lib/library.h"
#include "coredcc/parse/path_cache.h"

#include <cstring>
#include <optional>

namespace coredcc {

using litestl::util::Vector;

namespace {

struct Change {
  OwnerKey owner;
  litestl::util::string member; /* empty: the whole owner */
};

/* What a UI made a widget for. */
struct Meta {
  bool resolves = false;
  ResolvedPath::Kind kind = ResolvedPath::Kind::Struct;
  StructId type;
  const ApiMember *member = nullptr;
  /** Of a dynamic member. Others' don't change. */
  std::optional<PropertyDef> def;

  bool operator==(const Meta &b) const
  {
    return resolves == b.resolves && kind == b.kind && type == b.type && member == b.member &&
           def.has_value() == b.def.has_value() && (!def || def->sameAs(*b.def));
  }
};

struct Watcher {
  uint64_t id = 0;
  bool live = true;
  Context *ctx = nullptr;
  Path path;
  Context::Prefix prefix;
  std::shared_ptr<ChangeBus::WatchFn> fn;
  /** Null without one. */
  std::shared_ptr<ChangeBus::MetaFn> metaFn;
  Meta meta;
  PropValue last;
  Status lastStatus;
  Vector<PathTouch, 4> touched;
  /** The Library epoch at the last read: blocks created, deleted, restored or
   * detached, and pointer writes, re-route paths without a member change. */
  uint64_t libEpoch = 0;
};

/* One read of a watcher's path, into locals: getters run user code that may add
 * watchers and reallocate the list. */
struct Reading {
  Meta meta;
  PropValue value;
  Status status;
  Vector<PathTouch, 4> touched;
  uint64_t libEpoch = 0;
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

uint64_t libraryEpoch(const Context &ctx)
{
  return ctx.library() ? ctx.library()->epoch() : 0;
}

/* Reads a path now. A failure records no touches: a failing watcher is re-read at
 * every flush that has changes. */
Reading read(Context &ctx, const Path &path)
{
  Reading r;
  r.libEpoch = libraryEpoch(ctx);
  Result<ResolvedPath> rp = resolve(ctx, path);
  if (!rp.ok()) {
    r.status = rp.status();
    return r;
  }
  r.meta.resolves = true;
  r.meta.kind = rp->kind;
  r.meta.type = rp->ptr.type;
  r.meta.member = rp->member;
  if (rp->member && rp->member->isDynamic()) {
    Result<PropertyDef> def = getMemberMeta(ctx, *rp);
    r.meta.def = def.ok() ? std::move(def).value() : rp->member->def();
  }
  Result<PropValue> v = getValue(ctx, *rp);
  r.touched = std::move(rp->touched);
  if (v.ok()) {
    r.value = *v;
  }
  else {
    r.status = v.status();
  }
  return r;
}

void store(Watcher &w, Reading r)
{
  w.meta = std::move(r.meta);
  w.last = std::move(r.value);
  w.lastStatus = std::move(r.status);
  w.touched = std::move(r.touched);
  w.libEpoch = r.libEpoch;
}

} // namespace

struct ChangeBus::State final : detail::SignalStateBase {
  Vector<Watcher> watchers;
  Vector<Change> pending;
  Signal<const OwnerKey *> listeners;
  uint64_t nextId = 1;
  uint64_t epoch = 0;
  uint64_t flushedEpoch = 0;
  int flushDepth = 0;
  /** Dead watchers, which go when they are half of the list: many go at a time. */
  int dead = 0;

  void disconnect(uint64_t id) override
  {
    for (Watcher &w : watchers) {
      if (w.id == id && w.live) {
        w.live = false;
        w.fn.reset();
        w.metaFn.reset();
        dead++;
        break;
      }
    }
    compact();
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
    if (flushDepth > 0 || dead * 2 < int(watchers.size())) {
      return;
    }
    Vector<Watcher> kept;
    for (Watcher &w : watchers) {
      if (w.live) {
        kept.append(std::move(w));
      }
    }
    watchers = std::move(kept);
    dead = 0;
  }
};

ChangeBus::ChangeBus(Runtime *) : state_(std::make_shared<State>()) {}

ChangeBus::~ChangeBus() = default;

void ChangeBus::notify(const OwnerKey &owner, std::string_view member)
{
  if (owner.isNull()) {
    return;
  }
  /* Queued first, so a listener that flushes sees it. */
  state_->pending.append(Change{owner, lsString(member)});
  state_->listeners.emit(&owner);
}

void ChangeBus::notifyAll()
{
  state_->epoch++;
  state_->listeners.emit(nullptr);
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

Result<ScopedConnection> ChangeBus::watch(Context &ctx,
                                          const Path &path,
                                          WatchFn fn,
                                          MetaFn meta)
{
  CDCC_CHECK(bool(fn), "ChangeBus::watch: an empty callback");
  Watcher w;
  w.id = state_->nextId++;
  w.ctx = &ctx;
  w.path = path;
  w.prefix = ctx.anyPrefix();
  w.fn = std::make_shared<WatchFn>(std::move(fn));
  if (meta) {
    w.metaFn = std::make_shared<MetaFn>(std::move(meta));
  }
  store(w, read(ctx, path));
  const uint64_t id = w.id;
  state_->watchers.append(std::move(w));
  return ScopedConnection(Connection(state_, id));
}

Result<ScopedConnection> ChangeBus::watch(Context &ctx,
                                          std::string_view path,
                                          WatchFn fn,
                                          MetaFn meta)
{
  Result<PathPtr> p = ctx.runtime().ext<PathCache>().get(path);
  if (!p.ok()) {
    return p.status();
  }
  return watch(ctx, **p, std::move(fn), std::move(meta));
}

ScopedConnection ChangeBus::addListener(ListenFn fn)
{
  CDCC_CHECK(bool(fn), "ChangeBus::addListener: an empty callback");
  return ScopedConnection(state_->listeners.connect(std::move(fn)));
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
  st->flushDepth++;
  int fired = 0;
  /* Watchers added by callbacks are past `n` and wait for the next flush. Index, don't
   * hold references: callbacks can append and reallocate. */
  const int n = int(st->watchers.size());
  for (int i = 0; i < n; i++) {
    if (!st->watchers[i].live) {
      continue;
    }
    {
      const Watcher &w = st->watchers[i];
      const bool libMoved = libraryEpoch(*w.ctx) != w.libEpoch;
      const bool always = (!w.lastStatus.ok() || w.meta.def) && changes.size() > 0;
      if (!epochMoved && !libMoved && !always && !hits(w.touched, changes)) {
        continue;
      }
    }
    const PropValue before = st->watchers[i].last;
    const bool wasOk = st->watchers[i].lastStatus.ok();
    const Meta metaBefore = st->watchers[i].meta;
    Context *ctx = st->watchers[i].ctx;
    const Path path = st->watchers[i].path;
    {
      const Context::Prefix prefix = st->watchers[i].prefix;
      PrefixScope scope(*ctx, prefix);
      Reading now = read(*ctx, path);
      store(st->watchers[i], std::move(now));
    }
    bool metaFired = false;
    if (!(st->watchers[i].meta == metaBefore)) {
      if (std::shared_ptr<MetaFn> metaFn = st->watchers[i].metaFn) {
        metaFired = true;
        (*metaFn)();
      }
    }
    const Watcher &w = st->watchers[i];
    if (!w.live || (w.last == before && w.lastStatus.ok() == wasOk)) {
      fired += metaFired;
      continue;
    }
    std::shared_ptr<WatchFn> fn = w.fn;
    const PropValue value = w.last;
    const Status status = w.lastStatus;
    (*fn)(value, status);
    fired++;
  }
  st->flushDepth--;
  st->compact();
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
