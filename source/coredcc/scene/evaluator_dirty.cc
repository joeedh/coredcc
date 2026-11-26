/* Part B of P5c: what has changed since the last pass, so that a pass evaluates only that
 * (docs/plans/2026-11-25-p5c-scene-evaluator.md, "Design, part B"). It relies on the
 * notification rule: what a block answers changes only with a notification of the block,
 * or a move of the Library's epoch. */
#include "coredcc/scene/evaluator.h"

namespace coredcc::detail {

using litestl::util::Vector;

void SceneEvaluator::markDirty(const BlockId &id)
{
  /* 1: notified; 2: notified before the pass that runs, which evaluates it. */
  const int *p = placeOf_.lookup_ptr(id);
  if (p && dirty_[*p] != 1) {
    dirty_[*p] = 1;
    dirtyList_.append(*p);
  }
  /* A data block: its users' Spatial is looked up again (its data may be other data). */
  if (const int *user = dataUsers_.lookup_ptr(id)) {
    for (int u = *user; u >= 0; u = nextUser_[u]) {
      spatialKept_[u] = 0;
    }
  }
}

void SceneEvaluator::indexPlaces(const Library &lib)
{
  const int n = int(members_.size());
  placeOf_.clear();
  dataUsers_.clear();
  nextUser_.clear();
  nextUser_.resize(n);
  for (int i = 0; i < n; i++) {
    placeOf_.add(lib.get(members_[i].handle)->id(), i);
    nextUser_[i] = -1;
    if (const BlockHandle data = members_[i].data) {
      int *head = nullptr;
      if (!dataUsers_.add_uninitialized(lib.get(data)->id(), &head)) {
        nextUser_[i] = *head;
      }
      *head = i;
    }
  }
  dirty_.clear();
  dirty_.resize(n);
  dirtyList_.clear();
  moved_.clear();
  moved_.resize(n);
  movedList_.clear();
  spatial_.clear();
  spatial_.resize(n);
  spatialKept_.clear();
  spatialKept_.resize(n);
  full_ = true;
}

void SceneEvaluator::syncBack()
{
  const EvaluatedStore &front = stores_[front_];
  EvaluatedStore &back = stores_[1 - front_];
  if (backAll_ || back.objects.size() != front.objects.size()) {
    back.objects = front.objects;
  } else {
    for (int32_t p : backDiff_) {
      back.objects[p] = front.objects[p];
    }
  }
  back.index = front.index;
  backDiff_.clear();
  backAll_ = false;
}

void SceneEvaluator::clearDirty(const Vector<int> &before)
{
  for (int p : before) {
    if (dirty_[p] == 2) {
      dirty_[p] = 0; /* else notified again while the pass ran: still dirty */
    }
  }
  for (int p : movedList_) {
    moved_[p] = 0;
  }
  movedList_.clear();
}

} // namespace coredcc::detail
