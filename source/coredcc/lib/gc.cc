/* Library relations and garbage collection (architecture §8a, "Lifetime and garbage
 * collection"). */
#include "coredcc/lib/attachment.h"
#include "coredcc/lib/library.h"

#include <string>

namespace coredcc {

using litestl::util::Map;
using litestl::util::Vector;

void Library::buildRelations(Relations &out, Status &status) const
{
  out.clear();
  forEach([&](BlockHandle user, const Datablock &) {
    if (!status.ok()) {
      return;
    }
    status = foreachRef(user, [&](RefCell &cell) {
      if (cell.target.isNull()) {
        return;
      }
      const BlockIdKey target = keyOf(clients_.canonical(cell.target));
      Vector<BlockHandle> *users = out.lookup_ptr(target);
      if (!users) {
        out.add(target, Vector<BlockHandle>());
        users = out.lookup_ptr(target);
      }
      if (users->size() == 0 || users->last() != user) {
        users->append(user);
      }
    });
  });
}

Vector<BlockHandle> Library::usersOf(BlockHandle target) const
{
  const Datablock *t = rec(target);
  if (!t) {
    return {};
  }
  if (relationsEpoch_ != epoch_) {
    Status status;
    buildRelations(relations_, status);
    /* A failed enumeration isn't cached; the sticky error says why. */
    relationsEpoch_ = status.ok() ? epoch_ : 0;
  }
  const Vector<BlockHandle> *users = relations_.lookup_ptr(keyOf(t->id_));
  return users ? *users : Vector<BlockHandle>();
}

bool Library::sameRelations(const Relations &a, const Relations &b)
{
  if (a.size() != b.size()) {
    return false;
  }
  for (auto &pair : a) {
    const Vector<BlockHandle> *other = b.lookup_ptr(pair.key);
    if (!other || other->size() != pair.value.size()) {
      return false;
    }
    for (int i = 0; i < int(other->size()); i++) {
      if ((*other)[i] != pair.value[i]) {
        return false;
      }
    }
  }
  return true;
}

void Library::endNoCollect()
{
  CDCC_CHECK(noCollect_ > 0, "endNoCollect without beginNoCollect");
  noCollect_--;
  releaseDoomed();
}

Result<int> Library::collect()
{
  if (noCollect_ > 0) {
    return Status::error(StatusCode::Busy,
                         "collect: an interactive command or an evaluation is running");
  }
  CDCC_TRY(busyIfSweeping("collect"));
  if (rt_.hasStickyError()) {
    return Status::error(StatusCode::FailedPrecondition,
                         "collect: a sticky error is pending; take it first");
  }
  renormalizeIds();
  /* Providers' foreachRef runs during mark: library calls from it are Busy too. */
  collecting_ = true;
  struct ClearOnExit {
    bool &flag;
    Library &lib;
    ~ClearOnExit()
    {
      flag = false;
      lib.releaseDoomed(); /* unpinned while marking */
    }
  } clearOnExit{collecting_, *this};
  auto providerFailure = [this](Status s) {
    Status sticky = rt_.takeStickyError();
    return sticky.ok() ? s : sticky;
  };

  if (audit_ && relationsEpoch_ == epoch_) {
    Relations fresh;
    Status status;
    buildRelations(fresh, status);
    if (!status.ok()) {
      return providerFailure(status);
    }
    if (!sameRelations(fresh, relations_)) {
      return Status::error(StatusCode::Internal,
                           "collect: the relations cache is stale: a reference was written "
                           "without Library::writeRef or noteRefsChanged");
    }
  }

  /* Mark. Every record that isn't Live is held by something (the history, the host, a
   * file, a Doomed block's Host pins), so it is a root too, unless it only stands for a
   * missing block. */
  Map<BlockHandle, bool> marked;
  Vector<BlockHandle> stack;
  pool_.forEach([&](BlockHandle h, Datablock &b) {
    const bool held = b.state_ != BlockState::Live &&
                      (b.state_ != BlockState::Placeholder || b.type_ ||
                       b.placeholderReason_ != PlaceholderReason::MissingBlock);
    if (held || b.hasFlag(BlockFlags::Root) ||
        b.hasFlag(BlockFlags::FakeUser) || b.pins() > 0)
    {
      marked.add(h, true);
      stack.append(h);
    }
  });
  while (stack.size() > 0) {
    const BlockHandle h = stack.pop_back();
    Status s = foreachRef(h, [&](RefCell &cell) {
      if (!isStrong(cell.kind) || cell.target.isNull()) {
        return;
      }
      const BlockHandle t = find(cell.target);
      if (t && !marked.lookup_ptr(t)) {
        marked.add(t, true);
        stack.append(t);
      }
    });
    if (!s.ok()) {
      return providerFailure(s);
    }
  }

  Vector<BlockHandle> garbage;
  forEach([&](BlockHandle h, const Datablock &) {
    if (!marked.lookup_ptr(h)) {
      garbage.append(h);
    }
  });
  collecting_ = false; /* the sweep has its own guard */
  destroyBlocks(garbage);
  if (rt_.hasStickyError()) {
    /* A provider failed during the sweep; the blocks are gone regardless. */
    return rt_.takeStickyError();
  }
  releaseDoomed();
  if (Status s = takeDeferredErrors(); !s.ok()) {
    return s;
  }
  return int(garbage.size());
}

void Library::noticeWeakLost(const Vector<BlockHandle> &lost)
{
  Map<BlockIdKey, bool> dying;
  for (BlockHandle h : lost) {
    if (rec(h)->state_ != BlockState::Doomed) {
      dying.add(keyOf(rec(h)->id_), true);
    }
  }
  if (dying.size() == 0) {
    return;
  }
  forEach([&](BlockHandle user, const Datablock &u) {
    if (dying.lookup_ptr(keyOf(u.id_))) {
      return;
    }
    BlockProvider p = types_.provider(u.type_);
    if (!p || !p.hasOnWeakTargetLost()) {
      return;
    }
    Vector<BlockHandle> hit;
    foreachRef(user, [&](RefCell &cell) {
      if (cell.kind == RefKind::Weak && !cell.target.isNull() &&
          dying.lookup_ptr(keyOf(clients_.canonical(cell.target))))
      {
        hit.append(find(cell.target));
      }
    }).ignore();
    for (BlockHandle l : hit) {
      p.onWeakTargetLost(u, *rec(l));
    }
  });
}

void Library::destroyBlocks(const Vector<BlockHandle> &garbage)
{
  if (garbage.size() == 0) {
    return;
  }
  sweeping_ = true;
  /* Phase 1a: tell survivors that hold weak refs into the garbage. */
  noticeWeakLost(garbage);
  /* Phase 1b: sever every reference out of the garbage, so no destroy reaches another
   * dying block through one. */
  for (BlockHandle h : garbage) {
    foreachRef(h, [](RefCell &cell) {
      cell.target = BlockId();
      cell.cache = BlockHandle();
    }).ignore();
  }
  /* Phase 2: destroy. */
  for (BlockHandle h : garbage) {
    Datablock *b = rec(h);
    if (BlockProvider p = types_.provider(b->type_);
        p && b->dataState() != BlockState::Placeholder)
    {
      p.destroy(*b);
    }
    for (Datablock::Attachment &a : b->attachments_) {
      AttachmentProvider p = attachmentTypes_.find(a.type)->provider.get();
      p.destroy(a.data);
    }
    if (b->state_ == BlockState::Doomed) {
      doomedCount_--; /* its id may be another record's */
    }
    else {
      byId_.remove(keyOf(b->id_));
    }
    pool_.destroy(h);
  }
  sweeping_ = false;
  epoch_++;
  runDeferred();
}

void Library::runDeferred()
{
  Vector<litestl::util::function<void()>> later = std::move(deferred_);
  deferred_.clear();
  for (auto &fn : later) {
    fn();
  }
}

} // namespace coredcc
