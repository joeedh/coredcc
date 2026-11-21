/* Library relations and garbage collection (architecture §8a, "Lifetime and garbage
 * collection"). */
#include "coredcc/lib/library.h"

#include <string>

namespace coredcc {

using litestl::util::Map;
using litestl::util::Vector;

void Library::buildRelations(Map<BlockId, Vector<BlockHandle>> &out, Status &status) const
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
      const BlockId target = clients_.canonical(cell.target);
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
  const Vector<BlockHandle> *users = relations_.lookup_ptr(t->id_);
  return users ? *users : Vector<BlockHandle>();
}

namespace {

bool sameRelations(const Map<BlockId, Vector<BlockHandle>> &a,
                   const Map<BlockId, Vector<BlockHandle>> &b)
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

} // namespace

void Library::endNoCollect()
{
  CDCC_CHECK(noCollect_ > 0, "endNoCollect without beginNoCollect");
  noCollect_--;
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
  auto providerFailure = [this](Status s) {
    Status sticky = rt_.takeStickyError();
    return sticky.ok() ? s : sticky;
  };

  if (audit_ && relationsEpoch_ == epoch_) {
    Map<BlockId, Vector<BlockHandle>> fresh;
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
   * file), so it is a root too. */
  Map<BlockHandle, bool> marked;
  Vector<BlockHandle> stack;
  forEach([&](BlockHandle h, const Datablock &b) {
    if (b.state_ != BlockState::Live || b.hasFlag(BlockFlags::Root) ||
        b.hasFlag(BlockFlags::FakeUser) || b.pins_ > 0)
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
  destroyBlocks(garbage);
  if (rt_.hasStickyError()) {
    /* A provider failed during the sweep; the blocks are gone regardless. */
    return rt_.takeStickyError();
  }
  return int(garbage.size());
}

void Library::destroyBlocks(const Vector<BlockHandle> &garbage)
{
  if (garbage.size() == 0) {
    return;
  }
  sweeping_ = true;
  Map<BlockId, bool> dying;
  for (BlockHandle h : garbage) {
    dying.add(rec(h)->id_, true);
  }

  /* Phase 1a: tell survivors that hold weak refs into the garbage. */
  forEach([&](BlockHandle user, const Datablock &u) {
    if (dying.lookup_ptr(u.id_)) {
      return;
    }
    BlockProvider p = types_.provider(u.type_);
    if (!p || !p.hasOnWeakTargetLost()) {
      return;
    }
    Vector<BlockHandle> lost;
    foreachRef(user, [&](RefCell &cell) {
      if (cell.kind == RefKind::Weak && !cell.target.isNull() &&
          dying.lookup_ptr(clients_.canonical(cell.target)))
      {
        lost.append(find(cell.target));
      }
    }).ignore();
    for (BlockHandle l : lost) {
      p.onWeakTargetLost(u, *rec(l));
    }
  });
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
        p && b->state_ != BlockState::Placeholder)
    {
      p.destroy(*b);
    }
    for (Datablock::Attachment &a : b->attachments_) {
      if (a.destroy) {
        a.destroy(a.data);
      }
    }
    byId_.remove(b->id_);
    pool_.destroy(h);
  }
  sweeping_ = false;
  epoch_++;

  Vector<litestl::util::function<void()>> later = std::move(deferred_);
  deferred_.clear();
  for (auto &fn : later) {
    fn();
  }
}

} // namespace coredcc
