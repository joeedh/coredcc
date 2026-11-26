#include "coredcc/scene/evaluator.h"

#include "coredcc/api/change_bus.h"
#include "coredcc/base/iface/registry.h"
#include "coredcc/lib/pin.h"
#include "coredcc/scene/pending_writes.h"

#include <algorithm>
#include <utility>

#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#endif

namespace coredcc {

using litestl::util::Vector;

namespace detail {

namespace {

const InterfaceId kTransformableId = InterfaceId::of(Transformable::ifaceName);
const InterfaceId kSpatialId = InterfaceId::of(Spatial::ifaceName);
const InterfaceId kInvalidationId = InterfaceId::of(Invalidation::ifaceName);

/* The record, or null unless it is Live. */
const Datablock *live(const Library &lib, BlockHandle h)
{
  const Datablock *b = lib.get(h);
  return b && b->state() == BlockState::Live ? b : nullptr;
}

/* `hint`, if it still names that, saves looking for it. */
BlockHandle liveBlock(const Library &lib, const BlockId &id, BlockHandle hint)
{
  const Datablock *b = live(lib, hint);
  return b && b->id() == id ? hint : detail::liveBlock(lib, id);
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
    lib.forEachOverride(
        h, [&](InterfaceId id, const IfaceRef &) { overridden |= id == iface; });
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
I get(
    const Library &lib, CapMode mode, const IfaceTable *table, BlockHandle h, void *data)
{
  I out;
  if (mode == CapMode::Table) {
    static_cast<IfaceRef &>(out) = IfaceRef(table, data);
  } else if (mode == CapMode::Dynamic) {
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
/* Cache lines of each of PrefetchHint::at. */
constexpr int kPrefetchLines[] = {3, 3, 3, 3, 4};

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
  t.invalidation = lib.runtime().ifaces().find(type, kInvalidationId) != nullptr;
  t.object = type == TypeId::of(kObjectType);
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
  m.transformable =
      modeFor(lib, h, kTransformableId, t.transformable, t.hasQuery, m.isNull);
  m.spatial = modeFor(lib, h, kSpatialId, t.spatial, t.hasQuery, m.isNull);
  const auto tr = get<Transformable>(lib, m.transformable, t.transformable, h, data);
  if (!takeError(rt, ignored) && tr) {
    ObjectInfo info;
    tr.read(info, scratch_);
    if (!takeError(rt, ignored)) {
      m.parentId = info.parent;
      m.dataId = info.data;
      m.transformType = info.transformType;
    }
  }
  m.data = liveBlock(lib, m.dataId, before ? before->data : BlockHandle());
  if (m.data) {
    m.dataType = typeFor(lib, lib.get(m.data)->type());
    const SceneBlockType dt = types_[m.dataType];
    m.dataIsNull = dataOf(lib, m.data, dt) == nullptr;
    m.dataSpatial =
        modeFor(lib, m.data, kSpatialId, dt.spatial, dt.hasQuery, m.dataIsNull);
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
    const BlockHandle hint =
        was && was->parent >= 0 ? old[was->parent].handle : BlockHandle();
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
  keepBoxes(lib);
  indexPlaces(lib);
  lookups_++;
}

/* A block has bounds that are kept if its type implements Invalidation and nothing stands
 * in for its Spatial. */
void SceneEvaluator::keepBoxes(const Library &lib)
{
  boxOf_.clear();
  boxes_.clear();
  for (SceneMember &m : members_) {
    const bool own = m.dataSpatial == CapMode::None;
    const BlockHandle h = own ? m.handle : m.data;
    const int type = own ? m.type : m.dataType;
    if ((own ? m.spatial : m.dataSpatial) != CapMode::Table || !types_[type].invalidation)
    {
      continue;
    }
    const BlockId id = lib.get(h)->id();
    if (const int *known = boxOf_.lookup_ptr(id)) {
      m.box = *known;
      continue;
    }
    m.box = int(boxes_.size());
    boxOf_.add(id, m.box);
    boxes_.append(KeptBox());
  }
}

void SceneEvaluator::forgetBoxes()
{
  for (KeptBox &b : boxes_) {
    b.valid = false;
  }
}

/* A change while it is asked counts: it is asked again next time. */
Box3d SceneEvaluator::boundsOf(Runtime &rt,
                               const SceneMember &m,
                               Spatial &sp,
                               Status &foreign)
{
  if (m.box >= 0 && boxes_[m.box].valid) {
    return boxes_[m.box].box;
  }
  if (m.box >= 0) {
    boxes_[m.box].valid = true;
  }
  Box3d box = sp.localBounds();
  const bool failed = takeError(rt, foreign);
  if (failed || !box.allFinite()) {
    box = Box3d();
  }
  if (m.box >= 0) {
    boxes_[m.box].box = box;
    boxes_[m.box].valid = boxes_[m.box].valid && !failed;
  }
  return box;
}

void SceneEvaluator::sortByDepth()
{
  for (int i : order_) {
    SceneMember &m = members_[i];
    m.depth = m.parent < 0 || cyclic_[i] ? 0 : members_[m.parent].depth + 1;
  }
  std::stable_sort(order_.data(), order_.data() + order_.size(), [this](int a, int b) {
    return members_[a].depth < members_[b].depth;
  });
  byDepth_ = true;
}

const Vector<BlockHandle> &SceneEvaluator::recorded(const Library &lib) const
{
  if (recordedEpoch_ != lib.epoch() || recordedVersion_ != changes.version) {
    recorded_.clear();
    const EvaluatedStore &s = front();
    for (int i = 0; s.index && i < int(s.index->handles().size()); i++) {
      if (lib.get(s.index->handles()[i])) {
        recorded_.append(s.index->handles()[i]);
      }
    }
    recordedEpoch_ = lib.epoch();
    recordedVersion_ = changes.version;
  }
  return recorded_;
}

std::span<const BlockHandle> SceneEvaluator::children(BlockHandle h) const
{
  const EvaluatedStore &s = front();
  const int n = int(s.objects.size());
  if (!s.index) {
    return {};
  }
  if (childrenOf_ != frontLookups_) {
    firstChild_.clear();
    firstChild_.resize(n + 2);
    for (int &at : firstChild_) {
      at = 0;
    }
    const auto place = [n](const ObjectEval &e) { return e.parent < 0 ? n : e.parent; };
    for (const ObjectEval &e : s.objects) {
      firstChild_[place(e) + 1]++;
    }
    for (int i = 0; i <= n; i++) {
      firstChild_[i + 1] += firstChild_[i];
    }
    children_.resize(n);
    Vector<int> next = firstChild_;
    for (int i = 0; i < n; i++) {
      children_[next[place(s.objects[i])]++] = s.index->handles()[i];
    }
    childrenOf_ = frontLookups_;
  }
  const int at = h ? s.index->find(h) : n;
  if (at < 0) {
    return {};
  }
  return std::span<const BlockHandle>(children_.data() + firstChild_[at],
                                      size_t(firstChild_[at + 1] - firstChild_[at]));
}

void SceneEvaluator::sortByParent()
{
  byDepth_ = false;
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
  /* Whether `id` still names `h`, or for a null `h` still nothing that is Live. A record
   * keeps its id, and an alias renames the two together. */
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
        !sameTarget(m.parentId,
                    m.parent >= 0 ? members_[m.parent].handle : BlockHandle()) ||
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

bool SceneEvaluator::markDriven(const Library &lib,
                                const OverrideProvider::Driven &driven)
{
  if (driven.all) {
    const bool was = drivenAll_;
    drivenAll_ = true;
    return !was;
  }
  if (!drivenAll_ && driven.version == drivenVersion_ && lookups_ == drivenLookups_) {
    return false;
  }
  for (SceneMember &m : members_) {
    m.driven = false;
  }
  for (const BlockId &id : driven.ids) {
    const int i = index_ ? index_->find(lib.find(id)) : -1;
    if (i >= 0) {
      members_[i].driven = true;
    }
  }
  drivenAll_ = false;
  drivenVersion_ = driven.version;
  drivenLookups_ = lookups_;
  return true;
}

/* ---- evaluating ---- */

/* One object's state, of the channels in scratch_. */
EvalStatus SceneEvaluator::evaluateOne(const Library &lib,
                                       BlockHandle h,
                                       OverrideProvider *provider,
                                       const TransformType *type,
                                       double time,
                                       const TransformState &parent,
                                       TransformState &out,
                                       Status &foreign)
{
  Runtime &rt = lib.runtime();
  if (!provider) {
    return evaluateObject(rt, type, EvalCtx(scratch_, time), parent, out, foreign);
  }
  if (Status s = overridesOf(provider, lib, h, type, time, overrides_); !s.ok()) {
    if (foreign.ok()) {
      foreign = std::move(s);
    }
    out = parent;
    return EvalStatus::ModelFailed;
  }
  const EvalCtx ctx(scratch_, time, overrides_.values, overrides_.mask);
  return evaluateObject(rt, type, ctx, parent, out, foreign);
}

SceneEvaluator::Pass SceneEvaluator::evaluateAll(const Scene &scene,
                                                 const Library &lib,
                                                 bool incremental,
                                                 Status &foreign)
{
  Runtime &rt = lib.runtime();
  const TransformRegistry &types = rt.ext<TransformRegistry>();
  const double time = scene.time();
  const EvaluatedStore &old = stores_[front_];
  EvaluatedStore &out = stores_[1 - front_];
  const int n = int(members_.size());
  const bool others = old.index != index_;
  bool differs = others;
  if (differs) {
    out.objects.resize(n);
  }
  differing_.clear();

  OverrideProvider *provider = overrideProvider(lib);
  const TransformState root;
  const TransformType *lastType = nullptr;
  ObjectInfo info;
  ObjectEval e;
  const bool batching = types.hasEvaluateMany();
  if (batching && !byDepth_) {
    sortByDepth();
  }
  if (batching) {
    batch_.resize(n);
  }
  /* evaluateMany batches are made of whole levels: those passes are full ones. */
  incremental = incremental && !others && !batching;
  if (incremental) {
    syncBack();
    differs = true; /* the back store is the front one now, and is written in place */
    boxStale_.resize(boxes_.size());
    for (int b = 0; b < int(boxes_.size()); b++) {
      boxStale_[b] = !boxes_[b].valid;
    }
  }
  /* The bounds alone, of an object that is otherwise as it was. False: Stale. */
  const auto boundsOnly = [&](int i) {
    const SceneMember &m = members_[i];
    const SceneBlockType &t = types_[m.type];
    Spatial sp = spatialKept_[i] ? spatial_[i] : Spatial();
    void *data = sp ? nullptr : dataOf(lib, m.handle, t);
    if (!sp && (data == nullptr) != m.isNull) {
      return false;
    }
    if (!sp && m.data) {
      const SceneBlockType &dt = types_[m.dataType];
      void *blockData = dataOf(lib, m.data, dt);
      if ((blockData == nullptr) != m.dataIsNull) {
        return false;
      }
      sp = get<Spatial>(lib, m.dataSpatial, dt.spatial, m.data, blockData);
    }
    if (!sp) {
      sp = get<Spatial>(lib, m.spatial, t.spatial, m.handle, data);
    }
    Box3d local;
    if (!takeError(rt, foreign) && sp) {
      local = boundsOf(rt, m, sp, foreign);
    }
    const ObjectEval &was = old.objects[i];
    if (memcmp(&local, &was.localBounds, sizeof(Box3d)) == 0) {
      return true;
    }
    ObjectEval moved = was;
    moved.localBounds = local;
    moved.bounds = transformBox(local, static_cast<const double *>(moved.dataWorld()));
    if (!moved.bounds.allFinite()) {
      moved.bounds = Box3d();
    }
    out.objects[i] = moved;
    differing_.append(i);
    return true;
  };
  int levelEnd = 0;
  for (int k = 0; k < n; k++) {
    const int i = order_[k];
    const SceneMember &m = members_[i];
    if (batching && k == levelEnd) {
      while (levelEnd < n && members_[order_[levelEnd]].depth == m.depth) {
        levelEnd++;
      }
      evaluateLevel(
          lib, k, levelEnd, provider, time, differs ? out.objects : old.objects, foreign);
    }
    if (incremental && !dirty_[i] && old.objects[i].status != EvalStatus::ModelFailed &&
        (m.parent < 0 || cyclic_[i] || !moved_[m.parent]))
    {
      if (boundsEachPass(m) && !boundsOnly(i)) {
        return Pass::Stale;
      }
      continue;
    }
    if (k + kPrefetchAhead < n) {
      const PrefetchHint &ahead = hints_[order_[k + kPrefetchAhead]];
      for (int a = 0; a < 5; a++) {
        for (int line = 0; line < kPrefetchLines[a] && ahead.at[a]; line++) {
          prefetch(ahead.at[a] + uintptr_t(line * 64));
        }
      }
    }
    PrefetchHint &hint = hints_[i];
    hint.at[0] = reinterpret_cast<uintptr_t>(lib.get(m.handle));
    const SceneBlockType &t = types_[m.type];
    void *data = dataOf(lib, m.handle, t);
    hint.at[1] = reinterpret_cast<uintptr_t>(data);
    hint.at[4] = 0;
    if (t.object && data) {
      const uintptr_t at =
          reinterpret_cast<uintptr_t>(static_cast<const Object *>(data)->channels.data());
      const bool inside = at - hint.at[1] < sizeof(Object);
      hint.at[4] = inside ? 0 : at;
    }
    if ((data == nullptr) != m.isNull) {
      return Pass::Stale;
    }
    const TransformState &parent = m.parent < 0 || cyclic_[i] ? root
                                   : differs ? out.objects[m.parent].state
                                             : old.objects[m.parent].state;
    e.member = m.member;
    e.parent = cyclic_[i] ? -1 : m.parent;
    e.data = m.data;
    info = ObjectInfo();
    const bool batched = batching && batch_[i].done;
    const auto tr =
        batched
            ? Transformable()
            : get<Transformable>(lib, m.transformable, t.transformable, m.handle, data);
    bool failed = takeError(rt, foreign);
    if (batched) {
      info = batch_[i].info;
    } else if (tr && !failed) {
      tr.read(info, scratch_);
      failed = takeError(rt, foreign);
      members_[i].transformType = info.transformType;
    }
    e.flags = info.flags;
    if (failed) {
      e.status = EvalStatus::ModelFailed;
      e.state = parent;
    } else if (info.parent != m.parentId || info.data != m.dataId) {
      return Pass::Stale;
    } else if (batched) {
      e.state = batch_[i].state;
      e.status = batch_[i].status == EvalStatus::Ok && cyclic_[i] ? EvalStatus::Cycle
                                                                  : batch_[i].status;
    } else if (!tr) {
      e.status = EvalStatus::NoTransform;
      e.state = root;
    } else {
      if (!lastType || lastType->id() != info.transformType) {
        lastType = types.find(info.transformType);
      }
      e.status = evaluateOne(lib,
                             m.handle,
                             providerFor(i, provider),
                             lastType,
                             time,
                             parent,
                             e.state,
                             foreign);
      if (e.status == EvalStatus::Ok && cyclic_[i]) {
        e.status = EvalStatus::Cycle;
      }
    }

    /* Bounds: the data block's, or its own. */
    e.localBounds = Box3d();
    Spatial sp;
    bool table = false;
    if (m.data) {
      const SceneBlockType &dt = types_[m.dataType];
      void *blockData = dataOf(lib, m.data, dt);
      hint.at[2] = reinterpret_cast<uintptr_t>(lib.get(m.data));
      hint.at[3] = reinterpret_cast<uintptr_t>(blockData);
      if ((blockData == nullptr) != m.dataIsNull) {
        return Pass::Stale;
      }
      sp = get<Spatial>(lib, m.dataSpatial, dt.spatial, m.data, blockData);
      table = sp && m.dataSpatial == CapMode::Table;
    }
    if (!sp) {
      sp = get<Spatial>(lib, m.spatial, t.spatial, m.handle, data);
      table = sp && m.spatial == CapMode::Table;
    }
    /* A table and its data stay until the epoch moves: incremental passes reuse them. */
    spatial_[i] = table ? sp : Spatial();
    spatialKept_[i] = table;
    if (!takeError(rt, foreign) && sp) {
      e.localBounds = boundsOf(rt, m, sp, foreign);
    }
    e.bounds = transformBox(e.localBounds, static_cast<const double *>(e.dataWorld()));
    if (!e.bounds.allFinite()) {
      e.bounds = Box3d();
    }

    if (incremental && memcmp(&e.state, &old.objects[i].state, sizeof(e.state)) != 0) {
      moved_[i] = 1; /* its children are evaluated again */
      movedList_.append(i);
    }
    if (!others) {
      if (!e.same(old.objects[i])) {
        differing_.append(i);
      } else if (!differs) {
        continue;
      }
      if (!differs) {
        differs = true;
        out.objects.resize(n);
        for (int j = 0; j < k; j++) {
          out.objects[order_[j]] = old.objects[order_[j]];
        }
      }
    }
    out.objects[i] = e;
  }
  out.index = index_;
  if (incremental) {
    return differing_.size() > 0 ? Pass::Changed : Pass::Same;
  }
  return differs ? Pass::Changed : Pass::Same;
}

Status SceneEvaluator::run(const Scene &scene,
                           const Library &lib,
                           const OverrideProvider::Driven &driven,
                           bool &changed,
                           Status &foreign)
{
  EvaluationScope scope(lib.runtime());
  const LookupKey key = keyOf(lib);
  if (!changeListener_.connected()) {
    changeListener_ =
        lib.runtime().ext<ChangeBus>().addChangeListener([this](const BusChange &c) {
          constexpr ChangeAspects kBounds = ChangeAspects(InvalidateKinds::Topology) |
                                            ChangeAspects(InvalidateKinds::Positions);
          if (!c.owner) {
            forgetBoxes();
            full_ = true;
          } else if (c.owner->kind == OwnerKey::Kind::Block) {
            if (const int *box = (c.aspects & kBounds) != 0
                                     ? boxOf_.lookup_ptr(c.owner->block)
                                     : nullptr)
            {
              boxes_[*box].valid = false;
            }
            markDirty(c.owner->block);
          }
        });
    forgetBoxes();
  }
  if (!valid_ || key != key_) {
    forgetBoxes();
    full_ = true;
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
  const SceneSettings &settings = lib.runtime().ext<SceneSettings>();
  const double time = scene.time();
  for (int attempt = 0; attempt < 2; attempt++) {
    const bool drivenMoved = markDriven(lib, driven);
    const bool incremental =
        settings.incremental && !full_ && !drivenMoved && time == lastTime_;
    /* What was notified before the pass; what is notified during it stays dirty. */
    const Vector<int> before = std::move(dirtyList_);
    dirtyList_ = Vector<int>();
    for (int p : before) {
      dirty_[p] = 2;
    }
    foreign = Status();
    const bool others = stores_[front_].index != index_;
    const Pass pass = evaluateAll(scene, lib, incremental, foreign);
    if (pass != Pass::Stale) {
      clearDirty(before);
      full_ = false;
      lastTime_ = time;
      changed = pass == Pass::Changed;
      if (changed) {
        changes.version++;
        changes.all = others;
        backDiff_ = differing_; /* the back store will be these results' front */
        backAll_ = others;
        std::swap(changes.places, differing_);
        front_ = 1 - front_;
        frontLookups_ = lookups_;
      }
      if (changed && incremental && !others && bvh.valid()) {
        return bvh.update(stores_[front_],
                          settings.bvh,
                          {changes.places.data(), size_t(changes.places.size())});
      }
      return changed || !bvh.valid() ? bvh.update(stores_[front_], settings.bvh)
                                     : Status();
    }
    /* The back store has what the pass wrote. */
    for (int32_t p : differing_) {
      backDiff_.append(p);
    }
    backAll_ = backAll_ || others;
    for (int p : before) {
      dirtyList_.append(p);
      dirty_[p] = 1;
    }
    for (int p : movedList_) {
      moved_[p] = 0;
    }
    movedList_.clear();
    if (attempt == 0) {
      counters.rebuilds++;
      lookUp(scene, lib);
    }
  }
  return Status::error(StatusCode::FailedPrecondition,
                       "evaluateScene: an object's read answers otherwise each time");
}

} // namespace detail

Status evaluateScene(Context &ctx, BlockHandle h, EvaluateResult *result)
{
  if (result) {
    *result = EvaluateResult();
  }
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
    return Status::error(StatusCode::FailedPrecondition,
                         "evaluateScene: an error is pending");
  }
  rt.ext<detail::PendingWrites>().flush(
      *lib); /* what couldn't be written when its object came back */
  Scene *scene = Scene::of(*lib, h);
  if (!scene) {
    return Status::error(StatusCode::NotFound, "evaluateScene: no such scene");
  }
  Status status, foreign;
  bool changed = false;
  {
    /* The pin keeps the scene's data if a foreign capability deletes it. */
    const LibraryPin pin(*lib, h);
    /* Before the scope, so that a provider may evaluate a graph. Its error is its own to
     * give, for the objects it concerns. */
    OverrideProvider::Driven driven;
    detail::prepareOverrides(*lib, scene->time(), driven);
    if (!Scene::of(*lib, h)) {
      return Status::error(StatusCode::NotFound, "evaluateScene: the scene went away");
    }
    lib->beginNoCollect();
    scene->evaluator_->counters.evaluations++;
    status = scene->evaluator_->run(*scene, *lib, driven, changed, foreign);
    lib->endNoCollect();
  }
  if (changed) {
    rt.ext<ChangeBus>().notify(detail::evaluatedOwner(rt));
  }
  if (result) {
    result->done = status.ok();
    result->changed = changed;
  }
  return status.ok() ? foreign : status;
}

Status evaluateScene(Context &ctx, EvaluateResult *result)
{
  return evaluateScene(ctx, ctx.scene(), result);
}

} // namespace coredcc
