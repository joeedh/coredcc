#include "coredcc/scene/evaluator.h"

#include "coredcc/api/change_bus.h"
#include "coredcc/base/iface/registry.h"
#include "coredcc/lib/pin.h"

#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#endif

namespace coredcc {

using litestl::util::Vector;

namespace detail {

namespace {

const InterfaceId kTransformableId = InterfaceId::of(Transformable::ifaceName);
const InterfaceId kSpatialId = InterfaceId::of(Spatial::ifaceName);

/* The record, or null unless it is Live. */
const Datablock *live(const Library &lib, BlockHandle h)
{
  const Datablock *b = lib.get(h);
  return b && b->state() == BlockState::Live ? b : nullptr;
}

/* The Live block an id names, or null. */
BlockHandle liveBlock(const Library &lib, const BlockId &id)
{
  const BlockHandle h = id.isNull() ? BlockHandle() : lib.find(id);
  return live(lib, h) ? h : BlockHandle();
}

/* `hint`, if it still names that, saves looking for it. */
BlockHandle liveBlock(const Library &lib, const BlockId &id, BlockHandle hint)
{
  const Datablock *b = live(lib, hint);
  return b && b->id() == id ? hint : liveBlock(lib, id);
}

/* Any address, also one that is free by now. */
void prefetch(uintptr_t at)
{
#if defined(_MSC_VER) && !defined(__clang__)
#if defined(_M_X64)
  _mm_prefetch(reinterpret_cast<const char *>(at), _MM_HINT_T0);
#else
  (void)at;
#endif
#else
  __builtin_prefetch(reinterpret_cast<const void *>(at));
#endif
}

/* Library::data, with the provider known. */
void *dataOf(const Library &lib, BlockHandle h, const SceneBlockType &t)
{
  const Datablock *b = live(lib, h);
  return b && t.provider ? t.provider.data(*b) : nullptr;
}

/* What Library::queryRaw would do. */
CapMode modeFor(const Library &lib,
                BlockHandle h,
                InterfaceId iface,
                const IfaceTable *table,
                bool hasQuery,
                bool isNull)
{
  const Datablock *b = lib.get(h);
  if (b && b->hasFlag(BlockFlags::HasOverrides)) {
    bool overridden = false;
    lib.forEachOverride(h, [&](InterfaceId id, const IfaceRef &) { overridden |= id == iface; });
    if (overridden) {
      return CapMode::Dynamic;
    }
  }
  if (table && !isNull) {
    return CapMode::Table;
  }
  return hasQuery ? CapMode::Dynamic : CapMode::None;
}

/* The interface, for a block whose data is `data` now. */
template <class I>
I get(const Library &lib, CapMode mode, const IfaceTable *table, BlockHandle h, void *data)
{
  I out;
  if (mode == CapMode::Table) {
    static_cast<IfaceRef &>(out) = IfaceRef(table, data);
  }
  else if (mode == CapMode::Dynamic) {
    out = lib.query<I>(h);
  }
  return out;
}

/* A sticky error, as the first one of the evaluation. */
bool takeError(Runtime &rt, Status &foreign)
{
  if (!rt.hasStickyError()) {
    return false;
  }
  Status s = rt.takeStickyError();
  if (foreign.ok()) {
    foreign = std::move(s);
  }
  return true;
}

/* A few objects' worth of work: enough for the memory to arrive. */
constexpr int kPrefetchAhead = 8;

LookupKey keyOf(const Library &lib)
{
  LookupKey k;
  k.epoch = lib.epoch();
  k.tables = lib.runtime().ifaces().tableCount();
  k.clients = lib.runtime().ext<ClientRegistry>().generation();
  return k;
}

} // namespace

/* ---- looking up ---- */

int SceneEvaluator::typeFor(const Library &lib, TypeId type)
{
  for (int i = 0; i < int(types_.size()); i++) {
    if (types_[i].type == type) {
      return i;
    }
  }
  SceneBlockType t;
  t.type = type;
  t.provider = lib.runtime().ext<BlockTypeRegistry>().provider(type);
  t.hasQuery = t.provider && t.provider.hasQueryInterface();
  t.transformable = lib.runtime().ifaces().find(type, kTransformableId);
  t.spatial = lib.runtime().ifaces().find(type, kSpatialId);
  types_.append(t);
  return int(types_.size()) - 1;
}

SceneMember SceneEvaluator::memberFor(const Library &lib,
                                      BlockHandle h,
                                      bool inScene,
                                      const SceneMember *before)
{
  Runtime &rt = lib.runtime();
  Status ignored; /* what throws says so when it is evaluated */
  SceneMember m;
  m.handle = h;
  m.member = inScene;
  m.type = typeFor(lib, lib.get(h)->type());
  const SceneBlockType t = types_[m.type];
  void *data = dataOf(lib, h, t);
  m.isNull = data == nullptr;
  m.transformable = modeFor(lib, h, kTransformableId, t.transformable, t.hasQuery, m.isNull);
  m.spatial = modeFor(lib, h, kSpatialId, t.spatial, t.hasQuery, m.isNull);
  const auto tr = get<Transformable>(lib, m.transformable, t.transformable, h, data);
  if (!takeError(rt, ignored) && tr) {
    ObjectInfo info;
    tr.read(info, scratch_);
    if (!takeError(rt, ignored)) {
      m.parentId = info.parent;
      m.dataId = info.data;
    }
  }
  m.data = liveBlock(lib, m.dataId, before ? before->data : BlockHandle());
  if (m.data) {
    m.dataType = typeFor(lib, lib.get(m.data)->type());
    const SceneBlockType dt = types_[m.dataType];
    m.dataIsNull = dataOf(lib, m.data, dt) == nullptr;
    m.dataSpatial = modeFor(lib, m.data, kSpatialId, dt.spatial, dt.hasQuery, m.dataIsNull);
    takeError(rt, ignored);
  }
  return m;
}

void SceneEvaluator::lookUp(const Scene &scene, const Library &lib)
{
  const Vector<SceneMember> old = std::move(members_);
  members_ = Vector<SceneMember>();
  const std::shared_ptr<const HandleIndex> oldIndex = index_;
  const auto before = [&](BlockHandle h) {
    const int i = oldIndex ? oldIndex->find(h) : -1;
    return i >= 0 ? &old[i] : nullptr;
  };
  auto index = std::make_shared<HandleIndex>();
  const auto add = [&](BlockHandle h, bool inScene) {
    const int i = index->add(h);
    members_.append(memberFor(lib, h, inScene, before(h)));
    return i;
  };
  /* A copy: a foreign read may change the scene. */
  const Vector<BlockHandle> objects = scene.objects(lib);
  members_.ensure_capacity(objects.size());
  for (BlockHandle h : objects) {
    if (live(lib, h) && index->find(h) < 0) {
      add(h, true);
    }
  }
  inScene_ = int(members_.size());
  /* Parents. This adds those that aren't members to the end, so theirs are found too. */
  for (int i = 0; i < int(members_.size()); i++) {
    const SceneMember *was = before(members_[i].handle);
    const BlockHandle hint = was && was->parent >= 0 ? old[was->parent].handle : BlockHandle();
    if (const BlockHandle p = liveBlock(lib, members_[i].parentId, hint)) {
      const int known = index->find(p);
      const int parent = known >= 0 ? known : add(p, false);
      members_[i].parent = parent;
    }
  }
  if (!index_ || !index_->same(*index)) {
    index_ = std::move(index);
  }
  sortByParent();
  hints_.clear();
  hints_.resize(members_.size());
}

void SceneEvaluator::sortByParent()
{
  const int n = int(members_.size());
  enum : uint8_t { Unseen, OnPath, Sorted };
  Vector<uint8_t> seen;
  seen.resize(n);
  cyclic_.resize(n);
  for (int i = 0; i < n; i++) {
    seen[i] = Unseen;
    cyclic_[i] = 0;
  }
  order_.clear();
  order_.ensure_capacity(n);
  Vector<int> path;
  for (int i = 0; i < n; i++) {
    path.clear();
    int at = i;
    while (at >= 0 && seen[at] == Unseen) {
      seen[at] = OnPath;
      path.append(at);
      at = members_[at].parent;
    }
    if (at >= 0 && seen[at] == OnPath) {
      for (int k = int(path.size()) - 1; k >= 0; k--) {
        cyclic_[path[k]] = 1;
        if (path[k] == at) {
          break;
        }
      }
    }
    for (int k = int(path.size()) - 1; k >= 0; k--) {
      seen[path[k]] = Sorted;
      order_.append(path[k]);
    }
  }
}

/* Whether lookUp would find the same, as far as `read` and `data` don't come into it:
 * evaluateAll compares those. */
bool SceneEvaluator::stillValid(const Scene &scene, const Library &lib) const
{
  const Vector<BlockHandle> &objects = scene.objects(lib);
  if (int(objects.size()) != inScene_) {
    return false;
  }
  /* Whether `id` still names `h`, or for a null `h` still nothing that is Live. A record keeps
   * its id, and an alias renames the two together. */
  const auto sameTarget = [&](const BlockId &id, BlockHandle h) {
    return h ? live(lib, h) != nullptr : !liveBlock(lib, id);
  };
  for (int i = 0; i < int(members_.size()); i++) {
    const SceneMember &m = members_[i];
    if (!live(lib, m.handle) || (i < inScene_ && objects[i] != m.handle)) {
      return false;
    }
    const BlockHandle h = m.handle;
    const SceneBlockType &t = types_[m.type];
    if (modeFor(lib, h, kTransformableId, t.transformable, t.hasQuery, m.isNull) !=
            m.transformable ||
        modeFor(lib, h, kSpatialId, t.spatial, t.hasQuery, m.isNull) != m.spatial ||
        !sameTarget(m.parentId, m.parent >= 0 ? members_[m.parent].handle : BlockHandle()) ||
        !sameTarget(m.dataId, m.data))
    {
      return false;
    }
    if (m.data) {
      const SceneBlockType &dt = types_[m.dataType];
      if (modeFor(lib, m.data, kSpatialId, dt.spatial, dt.hasQuery, m.dataIsNull) !=
          m.dataSpatial)
      {
        return false;
      }
    }
  }
  return true;
}

/* ---- evaluating ---- */

SceneEvaluator::Pass SceneEvaluator::evaluateAll(const Scene &scene,
                                                 const Library &lib,
                                                 Status &foreign)
{
  Runtime &rt = lib.runtime();
  const TransformRegistry &types = rt.ext<TransformRegistry>();
  const double time = scene.time();
  const EvaluatedStore &old = stores_[front_];
  EvaluatedStore &out = stores_[1 - front_];
  const int n = int(members_.size());
  bool differs = old.index != index_;
  if (differs) {
    out.objects.resize(n);
  }

  const TransformState root;
  const TransformType *lastType = nullptr;
  ObjectInfo info;
  ObjectEval e;
  for (int k = 0; k < n; k++) {
    const int i = order_[k];
    const SceneMember &m = members_[i];
    if (k + kPrefetchAhead < n) {
      for (uintptr_t at : hints_[order_[k + kPrefetchAhead]].at) {
        for (int line = 0; line < 3; line++) {
          prefetch(at + uintptr_t(line * 64));
        }
      }
    }
    PrefetchHint &hint = hints_[i];
    hint.at[0] = reinterpret_cast<uintptr_t>(lib.get(m.handle));
    const SceneBlockType &t = types_[m.type];
    void *data = dataOf(lib, m.handle, t);
    hint.at[1] = reinterpret_cast<uintptr_t>(data);
    if ((data == nullptr) != m.isNull) {
      return Pass::Stale;
    }
    const TransformState &parent = m.parent < 0 || cyclic_[i] ? root :
                                   differs ? out.objects[m.parent].state :
                                             old.objects[m.parent].state;
    e.member = m.member;
    info.parent = info.data = BlockId();
    const auto tr = get<Transformable>(lib, m.transformable, t.transformable, m.handle, data);
    bool failed = takeError(rt, foreign);
    if (tr && !failed) {
      tr.read(info, scratch_);
      failed = takeError(rt, foreign);
    }
    if (failed) {
      e.status = EvalStatus::ModelFailed;
      e.state = parent;
    }
    else if (info.parent != m.parentId || info.data != m.dataId) {
      return Pass::Stale;
    }
    else if (!tr) {
      e.status = EvalStatus::NoTransform;
      e.state = root;
    }
    else {
      if (!lastType || lastType->id() != info.transformType) {
        lastType = types.find(info.transformType);
      }
      const EvalStatus s = evaluateObject(rt, lastType, scratch_, time, parent, e.state, foreign);
      e.status = s == EvalStatus::Ok && cyclic_[i] ? EvalStatus::Cycle : s;
    }

    /* Bounds: the data block's, or its own. */
    e.localBounds = Box3d();
    Spatial sp;
    if (m.data) {
      const SceneBlockType &dt = types_[m.dataType];
      void *blockData = dataOf(lib, m.data, dt);
      hint.at[2] = reinterpret_cast<uintptr_t>(lib.get(m.data));
      hint.at[3] = reinterpret_cast<uintptr_t>(blockData);
      if ((blockData == nullptr) != m.dataIsNull) {
        return Pass::Stale;
      }
      sp = get<Spatial>(lib, m.dataSpatial, dt.spatial, m.data, blockData);
    }
    if (!sp) {
      sp = get<Spatial>(lib, m.spatial, t.spatial, m.handle, data);
    }
    if (!takeError(rt, foreign) && sp) {
      const Box3d box = sp.localBounds();
      e.localBounds = !takeError(rt, foreign) && box.allFinite() ? box : Box3d();
    }
    e.bounds = transformBox(e.localBounds, static_cast<const double *>(e.state.world));
    if (!e.bounds.allFinite()) {
      e.bounds = Box3d();
    }

    if (!differs) {
      if (e.same(old.objects[i])) {
        continue;
      }
      differs = true;
      out.objects.resize(n);
      for (int j = 0; j < k; j++) {
        out.objects[order_[j]] = old.objects[order_[j]];
      }
    }
    out.objects[i] = e;
  }
  out.index = index_;
  return differs ? Pass::Changed : Pass::Same;
}

Status SceneEvaluator::run(const Scene &scene, const Library &lib, bool &changed, Status &foreign)
{
  EvaluationScope scope(lib.runtime());
  const LookupKey key = keyOf(lib);
  if (!valid_ || key != key_) {
    counters.revalidations++;
    if (key.tables != key_.tables) {
      types_.clear();
      valid_ = false;
    }
    if (!valid_ || !stillValid(scene, lib)) {
      counters.rebuilds++;
      lookUp(scene, lib);
    }
    valid_ = true;
    key_ = key;
  }
  for (int attempt = 0; attempt < 2; attempt++) {
    foreign = Status();
    const Pass pass = evaluateAll(scene, lib, foreign);
    if (pass != Pass::Stale) {
      changed = pass == Pass::Changed;
      front_ = changed ? 1 - front_ : front_;
      return changed ? bvh.update(stores_[front_]) : Status();
    }
    if (attempt == 0) {
      counters.rebuilds++;
      lookUp(scene, lib);
    }
  }
  return Status::error(StatusCode::FailedPrecondition,
                       "evaluateScene: an object's read answers otherwise each time");
}

} // namespace detail

Status evaluateScene(Context &ctx, BlockHandle h)
{
  Library *lib = ctx.library();
  if (!lib) {
    return Status::error(StatusCode::FailedPrecondition, "the Context has no Library");
  }
  Runtime &rt = ctx.runtime();
  CDCC_TRY(rt.busyIfEvaluating("evaluateScene"));
  if (lib->busy()) {
    return Status::error(StatusCode::Busy, "evaluateScene: the library is busy");
  }
  if (rt.hasStickyError()) {
    return Status::error(StatusCode::FailedPrecondition, "evaluateScene: an error is pending");
  }
  Scene *scene = Scene::of(*lib, h);
  if (!scene) {
    return Status::error(StatusCode::NotFound, "evaluateScene: no such scene");
  }
  Status status, foreign;
  bool changed = false;
  {
    /* The pin keeps the scene's data if a foreign capability deletes it. */
    const LibraryPin pin(*lib, h);
    lib->beginNoCollect();
    scene->evaluator_->counters.evaluations++;
    status = scene->evaluator_->run(*scene, *lib, changed, foreign);
    lib->endNoCollect();
  }
  if (changed) {
    rt.ext<ChangeBus>().notify(detail::evaluatedOwner(rt));
  }
  return status.ok() ? foreign : status;
}

Status evaluateScene(Context &ctx)
{
  return evaluateScene(ctx, ctx.scene());
}

} // namespace coredcc
