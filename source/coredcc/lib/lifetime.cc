/* Pins, and destroying blocks the history drops (architecture §8a; the design is in
 * docs/plans/2026-11-24-p2-layout-library-io.md, "Library lifetime"). */
#include "coredcc/lib/library.h"
#include "coredcc/lib/pin.h"

#include <string>

namespace coredcc {

using litestl::util::Map;
using litestl::util::Vector;

namespace {
constexpr int kMaxDeferredErrors = 16;
}

/* ---- refs and pins ---- */

LibraryRef Library::ref() const
{
  return LibraryRef(token_);
}

void Library::pin(BlockHandle h, PinKind kind)
{
  runOrDefer([this, h, kind]() {
    if (Datablock *b = rec(h)) {
      b->pins_[int(kind)]++;
    }
  });
}

void Library::unpin(BlockHandle h, PinKind kind)
{
  runOrDefer([this, h, kind]() {
    Datablock *b = rec(h);
    if (!b) {
      return;
    }
    CDCC_CHECK(b->pins_[int(kind)] > 0, "unpin without a pin");
    b->pins_[int(kind)]--;
    if (kind != PinKind::Host || b->pins(kind) > 0) {
      return;
    }
    releaseLent(*b);
    b = rec(h);
    if (b && b->state_ == BlockState::Doomed) {
      released_.append(h);
      releaseDoomed();
    }
  });
}

LibraryPin::LibraryPin(Library &lib, BlockHandle h, PinKind kind)
    : ref_(lib.ref()), handle_(h), kind_(kind)
{
  lib.pin(h, kind);
}

LibraryPin &LibraryPin::operator=(LibraryPin &&b) noexcept
{
  if (this != &b) {
    reset();
    ref_ = std::exchange(b.ref_, LibraryRef());
    handle_ = std::exchange(b.handle_, BlockHandle());
    kind_ = b.kind_;
  }
  return *this;
}

LibraryPin LibraryPin::copy() const
{
  Library *lib = ref_.get();
  return lib ? LibraryPin(*lib, handle_, kind_) : LibraryPin();
}

void LibraryPin::reset()
{
  Library *lib = ref_.get();
  const BlockHandle h = handle_;
  ref_ = LibraryRef();
  handle_ = BlockHandle();
  if (lib) {
    lib->unpin(h, kind_);
  }
}

bool LibraryPin::dataAlive() const
{
  const Library *lib = ref_.get();
  const Datablock *b = lib ? lib->get(handle_) : nullptr;
  return b && b->dataState() == BlockState::Live;
}

/* ---- leaving the history ---- */

const Datablock *Library::strongUserOf(BlockId id) const
{
  const Datablock *found = nullptr;
  forEach([&](BlockHandle h, const Datablock &u) {
    if (found || u.state_ == BlockState::InHistory) {
      return;
    }
    foreachRef(h, [&](RefCell &cell) {
      if (cell.kind == RefKind::Strong && !cell.target.isNull() &&
          clients_.canonical(cell.target) == id)
      {
        found = &u;
      }
    }).ignore();
  });
  return found;
}

Map<BlockIdKey, const Datablock *> Library::strongUsersOf(std::span<const BlockHandle> blocks) const
{
  Map<BlockIdKey, const Datablock *> users;
  for (BlockHandle h : blocks) {
    if (const Datablock *b = rec(h)) {
      users.add_overwrite(keyOf(b->id_), nullptr);
    }
  }
  forEach([&](BlockHandle h, const Datablock &u) {
    if (u.state_ == BlockState::InHistory) {
      return;
    }
    foreachRef(h, [&](RefCell &cell) {
      if (cell.kind == RefKind::Strong && !cell.target.isNull()) {
        const Datablock **user = users.lookup_ptr(keyOf(clients_.canonical(cell.target)));
        if (user && !*user) {
          *user = &u;
        }
      }
    }).ignore();
  });
  return users;
}

Status Library::destroyFromHistory(BlockHandle h)
{
  return destroyFromHistory(std::span<const BlockHandle>(&h, 1));
}

Status Library::destroyFromHistory(std::span<const BlockHandle> blocks)
{
  CDCC_TRY(busyIfSweeping("destroyFromHistory"));
  Status first;
  const auto refuse = [&first](StatusCode code, const std::string &what) {
    if (first.ok()) {
      first = Status::error(code, ("destroyFromHistory: " + what).c_str());
    }
  };
  const auto users = strongUsersOf(blocks);
  Vector<BlockHandle> pinned, unpinned;
  Map<BlockHandle, bool> seen;
  for (BlockHandle h : blocks) {
    const Datablock *b = rec(h);
    if (!b) {
      refuse(StatusCode::NotFound, "no such block");
      continue;
    }
    if (b->state_ != BlockState::InHistory) {
      refuse(StatusCode::FailedPrecondition,
             std::string("block '") + b->name_.c_str() + "' is " + blockStateName(b->state_) +
                 ", not InHistory");
      continue;
    }
    /* Its Strong ref would dangle: the delete should have taken that user too. Refuse; the
     * block stays InHistory, which GC treats as a root. */
    if (const Datablock *u = *users.lookup_ptr(keyOf(b->id_))) {
      refuse(StatusCode::FailedPrecondition, "'" + std::string(u->name_.c_str()) +
                                                 "' still holds a Strong ref to '" +
                                                 b->name_.c_str() + "'");
      continue;
    }
    if (!seen.lookup_ptr(h)) {
      seen.add(h, true);
      (b->pins(PinKind::Host) > 0 ? pinned : unpinned).append(h);
    }
  }
  doom(pinned);
  destroyBlocks(unpinned);
  if (rt_.hasStickyError()) {
    Status s = rt_.takeStickyError(); /* a provider's destroy failed; the block is gone */
    return first.ok() ? s : first;
  }
  return first;
}

void Library::doom(const Vector<BlockHandle> &blocks)
{
  if (blocks.size() == 0) {
    return;
  }
  sweeping_ = true; /* the providers' Busy rules, as in a sweep */
  noticeWeakLost(blocks);
  sweeping_ = false;
  for (BlockHandle h : blocks) {
    Datablock *b = rec(h);
    byId_.remove(keyOf(b->id_));
    setState(*b, BlockState::Doomed);
    doomedCount_++;
  }
  /* destroyBlocks of nothing would return early: run what the notices deferred here. */
  runDeferred();
}

void Library::releaseDoomed()
{
  if (releasing_ || busy() || noCollect_ > 0) {
    return;
  }
  releasing_ = true;
  while (released_.size() > 0) {
    releaseOneDoomed(released_.pop_back());
  }
  releasing_ = false;
}

void Library::releaseOneDoomed(BlockHandle h)
{
  Datablock *b = rec(h);
  if (!b || b->state_ != BlockState::Doomed || b->pins(PinKind::Host) > 0) {
    return; /* pinned again, or destroyed already */
  }
  /* A sticky error from here would fail an unrelated command: keep it on the Library. */
  Status before = rt_.hasStickyError() ? rt_.takeStickyError() : Status();
  const BlockId id = clients_.canonical(b->id_);
  if (!find(id) && strongUserOf(id)) {
    b->id_ = id;
    byId_.add(keyOf(id), h);
    doomedCount_--;
    setState(*b, BlockState::InHistory);
  }
  else {
    Vector<BlockHandle> one;
    one.append(h);
    destroyBlocks(one);
  }
  if (rt_.hasStickyError()) {
    Status s = rt_.takeStickyError();
    if (deferredErrorCount_++ < kMaxDeferredErrors) {
      deferredErrors_.append(std::move(s));
    }
  }
  if (!before.ok()) {
    rt_.setStickyError(std::move(before));
  }
}

Status Library::takeDeferredErrors()
{
  if (deferredErrorCount_ == 0) {
    return Status();
  }
  std::string msg = "destroying a block that was pinned when the history dropped it failed: ";
  msg += deferredErrors_[0].message();
  if (deferredErrorCount_ > 1) {
    msg += " (and " + std::to_string(deferredErrorCount_ - 1) + " more)";
  }
  Status out = Status::error(deferredErrors_[0].code(), msg.c_str());
  deferredErrors_.clear();
  deferredErrorCount_ = 0;
  return out;
}

} // namespace coredcc
