#include "coredcc/scene/scene.h"

#include "coredcc/api/format.h"
#include "coredcc/lib/provider.h"
#include "coredcc/scene/evaluator.h"

#include "litestl/binding/binding.h"
#include "litestl/util/alloc.h"

#include <algorithm>
#include <string>

namespace coredcc {

namespace lbt = litestl::binding::types;
using litestl::util::Vector;

Scene::Scene() : evaluator_(std::make_unique<detail::SceneEvaluator>())
{
}
Scene::~Scene() = default;

Scene *Scene::of(const Library &lib, BlockHandle h)
{
  const Datablock *b = lib.get(h);
  return b && b->type() == TypeId::of(kSceneType) ? static_cast<Scene *>(lib.data(h))
                                                  : nullptr;
}

/* ---- members ---- */

void Scene::ensureIndex(const Library &lib) const
{
  const ClientRegistry &clients = lib.runtime().ext<ClientRegistry>();
  if (indexValid_ && indexCount_ == int(cells_.size()) &&
      indexGeneration_ == clients.generation())
  {
    return;
  }
  index_.clear();
  indexDoubles_ = false;
  for (int i = 0; i < int(cells_.size()); i++) {
    if (!cells_[i].isNull() && !index_.add(clients.canonical(cells_[i].target), i)) {
      indexDoubles_ = true;
    }
  }
  indexValid_ = true;
  indexEpoch_ = lib.epoch();
  indexCount_ = int(cells_.size());
  indexGeneration_ = clients.generation();
}

int Scene::indexOf(const Library &lib, const BlockId &id) const
{
  const int i = lookup(lib, id);
  if (i >= 0 || id.isNull() || indexEpoch_ == lib.epoch()) {
    return i;
  }
  ensureFreshIndex(lib);
  return lookup(lib, id);
}

void Scene::ensureFreshIndex(const Library &lib) const
{
  indexValid_ = indexValid_ && indexEpoch_ == lib.epoch();
  ensureIndex(lib);
}

int Scene::lookup(const Library &lib, const BlockId &id) const
{
  const ClientRegistry &clients = lib.runtime().ext<ClientRegistry>();
  const BlockId want = clients.canonical(id);
  ensureIndex(lib);
  const int *i = index_.lookup_ptr(want);
  if (i && clients.canonical(cells_[*i].target) != want) {
    indexValid_ = false;
    ensureIndex(lib);
    i = index_.lookup_ptr(want);
  }
  return i && !want.isNull() ? *i : -1;
}

int Scene::indexOf(const Library &lib, BlockHandle h) const
{
  const Datablock *b = lib.get(h);
  return b ? indexOf(lib, b->id()) : -1;
}

const Vector<BlockHandle> &Scene::objects(const Library &lib) const
{
  const uint64_t clients = lib.runtime().ext<ClientRegistry>().generation();
  if (liveEpoch_ != lib.epoch() || liveGeneration_ != clients) {
    live_.clear();
    Vector<uint8_t> seen;
    for (const RefCell &c : cells_) {
      const BlockHandle h = lib.resolveRef(c);
      if (!h) {
        continue;
      }
      while (seen.size() <= h.index) {
        seen.append(0);
      }
      if (!seen[h.index]) {
        seen[h.index] = 1;
        live_.append(h);
      }
    }
    liveEpoch_ = lib.epoch();
    liveGeneration_ = clients;
  }
  return live_;
}

Status Scene::add(Library &lib, BlockHandle h, int index)
{
  const Datablock *b = lib.get(h);
  if (!b) {
    return Status::error(StatusCode::NotFound, "Scene::add: no such block");
  }
  if (lookup(lib, b->id()) >= 0) {
    return Status::error(
        StatusCode::AlreadyExists,
        ("'" + std::string(b->name().c_str()) + "' is in the scene").c_str());
  }
  RefCell cell(RefKind::OptStrong);
  CDCC_TRY(lib.writeRef(cell, b->id()));
  const int n = int(cells_.size());
  cells_.append(cell);
  if (index < 0 || index >= n) {
    index_.add(cell.target, n);
    indexCount_ = n + 1;
    return Status();
  }
  for (int i = n; i > index; i--) {
    cells_[i] = cells_[i - 1];
  }
  cells_[index] = cell;
  return Status();
}

Result<int> Scene::remove(Library &lib, const BlockId &id)
{
  Result<Vector<Place>> places = removeMany(lib, std::span<const BlockId>(&id, 1));
  return places.ok() ? Result<int>((*places)[0].index) : Result<int>(places.status());
}

Result<Vector<Scene::Place>> Scene::removeMany(Library &lib, std::span<const BlockId> ids)
{
  if (lib.busy()) {
    return Status::error(StatusCode::Busy, "Scene::remove: the library is busy");
  }
  const ClientRegistry &clients = lib.runtime().ext<ClientRegistry>();
  Vector<Place> places;
  litestl::util::Map<BlockId, char> wanted;
  const Status missing =
      Status::error(StatusCode::NotFound, "Scene::remove: the block isn't in the scene");
  for (const BlockId &id : ids) {
    if (id.isNull()) {
      return missing;
    }
    wanted.add(clients.canonical(id), 0);
  }
  if (ids.empty()) {
    return places;
  }
  /* One id, as undo has it for each step, needs no lookups. */
  const BlockId only = ids.size() == 1 ? clients.canonical(ids[0]) : BlockId();
  const auto isWanted = [&](const RefCell &c) {
    return ids.size() == 1
               ? c.target.local == only.local && clients.canonical(c.target) == only
               : wanted.lookup_ptr(clients.canonical(c.target)) != nullptr;
  };
  Vector<int> at;
  BlockId previous;
  litestl::util::Map<BlockId, char> found;
  for (int i = 0; i < int(cells_.size()); i++) {
    if (isWanted(cells_[i])) {
      at.append(i);
      if (found.add(clients.canonical(cells_[i].target), 0)) {
        places.append(Place{cells_[i].target, previous, i});
      }
    }
    previous = cells_[i].target;
  }
  if (places.size() != wanted.size()) {
    return missing;
  }
  int kept = at[0], next = 0;
  for (int i = at[0]; i < int(cells_.size()); i++) {
    if (next < int(at.size()) && at[next] == i) {
      next++;
      continue;
    }
    cells_[kept++] = cells_[i];
  }
  const bool last = at.size() == 1 && at[0] == kept;
  cells_.resize(kept);
  if (last && indexValid_ && indexCount_ == kept + 1) {
    index_.remove(clients.canonical(places[0].id)); /* the others are where they were */
    indexCount_ = kept;
  } else {
    indexValid_ = false;
  }
  lib.noteRefsChanged();
  return places;
}

Status Scene::putBack(Library &lib, std::span<const Place> places)
{
  const ClientRegistry &clients = lib.runtime().ext<ClientRegistry>();
  const auto canonical = [&](const BlockId &id) {
    return id.isNull() ? id : clients.canonical(id);
  };
  struct Item {
    RefCell cell;
    BlockId previous;
    int index;
    bool put = false;
  };
  Status first;
  Vector<Item> items;
  litestl::util::Map<BlockId, int> itemOf,
      behind; /* by its own id, and by the one before it */
  ensureFreshIndex(lib);
  for (const Place &p : places) {
    RefCell cell(RefKind::OptStrong);
    Status s;
    if (!lib.find(p.id)) {
      s = Status::error(StatusCode::NotFound, "Scene::putBack: no such block");
    } else if (lookup(lib, p.id) >= 0 || itemOf.lookup_ptr(canonical(p.id))) {
      s = Status::error(StatusCode::AlreadyExists,
                        "Scene::putBack: the block is in the scene");
    } else {
      s = lib.writeRef(cell, p.id);
    }
    if (!s.ok()) {
      first = first.ok() ? s : first;
      continue;
    }
    itemOf.add(cell.target, int(items.size()));
    items.append(Item{cell, canonical(p.previous), p.index});
  }
  Vector<int> byIndex;
  for (int i = 0; i < int(items.size()); i++) {
    const BlockId &p = items[i].previous;
    const bool known = !p.isNull() && (lookup(lib, p) >= 0 || itemOf.lookup_ptr(p));
    if (!known || !behind.add(p, i)) {
      byIndex.append(i);
    }
  }
  std::stable_sort(byIndex.data(), byIndex.data() + byIndex.size(), [&](int a, int b) {
    return items[a].index < items[b].index;
  });

  Vector<RefCell> out;
  out.ensure_capacity(cells_.size() + items.size());
  /* A cell, and those that come behind it. */
  const auto append = [&](const RefCell &cell) {
    out.append(cell);
    const int *i = cell.isNull() ? nullptr : behind.lookup_ptr(canonical(cell.target));
    while (i && !items[*i].put) {
      items[*i].put = true;
      out.append(items[*i].cell);
      i = behind.lookup_ptr(items[*i].cell.target);
    }
  };
  const auto put = [&](int i) {
    if (!items[i].put) {
      items[i].put = true;
      append(items[i].cell);
    }
  };
  int next = 0;
  for (const RefCell &c : cells_) {
    for (; next < int(byIndex.size()) && items[byIndex[next]].index <= int(out.size());
         next++)
    {
      put(byIndex[next]);
    }
    append(c);
  }
  for (; next < int(byIndex.size()); next++) {
    put(byIndex[next]);
  }
  for (int i = 0; i < int(items.size()); i++) {
    put(i); /* those that name each other */
  }
  cells_ = std::move(out);
  indexValid_ = false;
  return first;
}

Status Scene::addMany(Library &lib, std::span<const BlockHandle> blocks)
{
  if (lib.busy()) {
    return Status::error(StatusCode::Busy, "Scene::addMany: the library is busy");
  }
  litestl::util::Map<BlockHandle, char> seen;
  for (BlockHandle h : blocks) {
    const Datablock *b = lib.get(h);
    if (!b) {
      return Status::error(StatusCode::NotFound, "Scene::add: no such block");
    }
    if (lookup(lib, b->id()) >= 0 || !seen.add(h, 0)) {
      return Status::error(
          StatusCode::AlreadyExists,
          ("'" + std::string(b->name().c_str()) + "' is in the scene").c_str());
    }
  }
  for (BlockHandle h : blocks) {
    RefCell cell(RefKind::OptStrong);
    CDCC_TRY(lib.writeRef(cell, lib.get(h)->id())); /* only Busy */
    index_.add(cell.target, int(cells_.size()));
    cells_.append(cell);
    indexCount_ = int(cells_.size());
  }
  return Status();
}

int Scene::prune(Library &lib)
{
  int kept = 0;
  const int n = int(cells_.size());
  const ClientRegistry &clients = lib.runtime().ext<ClientRegistry>();
  ensureFreshIndex(lib);
  for (int i = 0; i < n; i++) {
    if (cells_[i].isNull() || !lib.find(cells_[i].target)) {
      continue;
    }
    if (!indexDoubles_ || *index_.lookup_ptr(clients.canonical(cells_[i].target)) == i) {
      cells_[kept++] = cells_[i];
    }
  }
  if (kept < n) {
    cells_.resize(kept);
    lib.noteRefsChanged();
  }
  prunedAt_ = lib.removedCount();
  return n - kept;
}

void Scene::pruneRemoved(Library &lib)
{
  if (prunedAt_ != lib.removedCount()) {
    prune(lib);
  }
}

const Vector<BlockHandle> &Scene::selected(const Library &lib) const
{
  const uint64_t clients = lib.runtime().ext<ClientRegistry>().generation();
  if (selectedEpoch_ != lib.epoch() || selectedGeneration_ != clients) {
    selected_.clear();
    for (const RefCell &c : selection_) {
      const BlockHandle h = lib.resolveRef(c);
      if (h && contains(lib, h)) {
        selected_.append(h);
      }
    }
    selectedEpoch_ = lib.epoch();
    selectedGeneration_ = clients;
  }
  return selected_;
}

void Scene::setSelection(Library &lib, const Vector<BlockId> &ids)
{
  litestl::util::Map<BlockId, int> seen;
  selection_.clear();
  for (const BlockId &id : ids) {
    if (id.isNull() || !seen.add(id, 0)) {
      continue;
    }
    RefCell cell(RefKind::Weak);
    cell.target = id;
    selection_.append(cell);
  }
  lib.noteRefsChanged();
}

/* ---- graphs ---- */

const Vector<BlockHandle> &Scene::graphs(const Library &lib) const
{
  const uint64_t clients = lib.runtime().ext<ClientRegistry>().generation();
  if (graphsEpoch_ != lib.epoch() || graphsGeneration_ != clients) {
    liveGraphs_.clear();
    litestl::util::Map<BlockHandle, char> seen;
    for (const RefCell &c : graphs_) {
      const BlockHandle h = lib.resolveRef(c);
      const Datablock *b = lib.get(h);
      if (b && b->state() == BlockState::Live && seen.add(h, 0)) {
        liveGraphs_.append(h);
      }
    }
    graphsEpoch_ = lib.epoch();
    graphsGeneration_ = clients;
  }
  return liveGraphs_;
}

Status Scene::insertGraph(Library &lib, const BlockId &id, int index)
{
  const ClientRegistry &clients = lib.runtime().ext<ClientRegistry>();
  if (lib.find(id).isNull()) {
    return Status::error(StatusCode::NotFound, "Scene::insertGraph: no such block");
  }
  for (const RefCell &c : graphs_) {
    if (clients.canonical(c.target) == clients.canonical(id)) {
      return Status::error(StatusCode::AlreadyExists, "the graph is in the scene's list");
    }
  }
  RefCell cell(RefKind::OptStrong);
  CDCC_TRY(lib.writeRef(cell, id));
  const int n = int(graphs_.size());
  graphs_.append(cell);
  if (index >= 0 && index < n) {
    for (int i = n; i > index; i--) {
      graphs_[i] = graphs_[i - 1];
    }
    graphs_[index] = cell;
  }
  lib.noteRefsChanged();
  return Status();
}

Result<int> Scene::removeGraph(Library &lib, const BlockId &id)
{
  const ClientRegistry &clients = lib.runtime().ext<ClientRegistry>();
  int first = -1;
  for (int i = int(graphs_.size()) - 1; i >= 0; i--) {
    if (clients.canonical(graphs_[i].target) == clients.canonical(id)) {
      graphs_.remove_at(i);
      first = i;
    }
  }
  if (first < 0) {
    return Status::error(StatusCode::NotFound, "the graph isn't in the scene's list");
  }
  lib.noteRefsChanged();
  return first;
}

/* ---- evaluation ---- */

bool Scene::evaluated(BlockHandle h, ObjectEval &out) const
{
  const ObjectEval *e = evaluator_->front().find(h);
  if (e) {
    out = *e;
  }
  return e != nullptr;
}

const Vector<BlockHandle> &Scene::evaluatedObjects() const
{
  const detail::EvaluatedStore &s = evaluator_->front();
  return s.index ? s.index->handles() : evaluator_->none;
}

const Vector<BlockHandle> &Scene::recordedObjects(const Library &lib) const
{
  return evaluator_->recorded(lib);
}

std::span<const ObjectEval> Scene::results() const
{
  const detail::EvaluatedStore &s = evaluator_->front();
  return std::span<const ObjectEval>(s.objects.data(), size_t(s.objects.size()));
}

std::span<const BlockHandle> Scene::children(BlockHandle h) const
{
  return evaluator_->children(h);
}

const Scene::Counters &Scene::counters() const
{
  return evaluator_->counters;
}

const Scene::Changes &Scene::lastChanges() const
{
  return evaluator_->changes;
}

const SceneBvh &Scene::bvh() const
{
  return evaluator_->bvh;
}

/* ---- the block ---- */

namespace {

constexpr uint32_t kScenePayload = 1;

struct SceneProvider {
  void *data(const Datablock &b) const
  {
    return b.clientData();
  }
  void foreachRef(const Datablock &b, RefVisitor &v) const
  {
    if (Scene *s = static_cast<Scene *>(b.clientData())) {
      s->foreachCell([&v](RefCell &cell) { v.visit(cell); });
    }
  }
  void destroy(const Datablock &b)
  {
    litestl::alloc::Delete(static_cast<Scene *>(b.clientData()));
  }
  Status serialize(const Datablock &b, BlockWriter &out) const
  {
    if (!b.clientData()) {
      return Status::error(StatusCode::FailedPrecondition, "a scene without data");
    }
    static_cast<Scene *>(b.clientData())->write(out);
    return Status(); /* or the writer's */
  }
  Status deserialize(const Datablock &, BlockReader &in)
  {
    Scene *s = litestl::alloc::New<Scene>("coredcc::Scene");
    s->readFrom(in);
    in.setData(s);
    return Status(); /* or the reader's: `destroy` gets it back */
  }
  Status duplicate(const Datablock &src, BlockCopy &out)
  {
    if (!src.clientData()) {
      return Status::error(StatusCode::FailedPrecondition, "a scene without data");
    }
    out.setData(static_cast<Scene *>(src.clientData())->duplicate());
    return Status();
  }
  uint64_t memSize(const Datablock &b) const
  {
    const Scene *s = static_cast<Scene *>(b.clientData());
    return s ? s->memSize() : 0;
  }
  static lbt::Struct<SceneProvider> *defineBindings()
  {
    auto *st =
        new lbt::Struct<SceneProvider>("coredcc::SceneProvider", sizeof(SceneProvider));
    BIND_STRUCT_METHOD(st, data, MARGS("block"));
    BIND_STRUCT_METHOD(st, foreachRef, MARGS("block", "visitor"));
    BIND_STRUCT_METHOD(st, destroy, MARGS("block"));
    BIND_STRUCT_METHOD(st, serialize, MARGS("block", "out"));
    BIND_STRUCT_METHOD(st, deserialize, MARGS("block", "in"));
    BIND_STRUCT_METHOD(st, duplicate, MARGS("src", "out"));
    BIND_STRUCT_METHOD(st, memSize, MARGS("block"));
    return st;
  }
};

} // namespace

Result<BlockHandle> newScene(Library &lib, std::string_view name)
{
  Scene *s = litestl::alloc::New<Scene>("coredcc::Scene");
  Result<BlockHandle> h = lib.create(TypeId::of(kSceneType), name, s);
  if (!h.ok()) {
    litestl::alloc::Delete(s);
    return h;
  }
  lib.setFlag(*h, BlockFlags::Root, true);
  return h;
}

Scene *activeScene(const Context &ctx)
{
  return ctx.library() ? Scene::of(*ctx.library(), ctx.scene()) : nullptr;
}

/* ---- the datapath ---- */

namespace detail {

namespace {

Scene &self(const PointerRef &p)
{
  return *static_cast<Scene *>(p.data);
}

/* A handle as a collection key. */
int64_t packed(BlockHandle h)
{
  return (int64_t(h.generation & 0x7fffffffu) << 32) | int64_t(h.index);
}

/* Both collections: the members, and the selected ones. */
CollectionAccessor collectionOf(bool selectedOnly)
{
  using Handles = Vector<BlockHandle>;
  const auto list = [selectedOnly](const PointerRef &p, Context &ctx) -> const Handles & {
    return selectedOnly ? self(p).selected(*ctx.library())
                        : self(p).objects(*ctx.library());
  };
  CollectionAccessor c;
  c.itemStruct = StructId::of(kSceneObjectStruct);
  c.length = [list](const PointerRef &p, Context &ctx) {
    return ctx.library() ? int64_t(list(p, ctx).size()) : int64_t(0);
  };
  c.at = [list](const PointerRef &p, Context &ctx, int64_t i) {
    if (!ctx.library()) {
      return PointerRef();
    }
    const Handles &l = list(p, ctx);
    return i >= 0 && i < int64_t(l.size()) ? blockRoot(ctx, l[int(i)]) : PointerRef();
  };
  c.lookup = [selectedOnly,
              list](const PointerRef &p, Context &ctx, const KeySegment &key) {
    const Library *lib = ctx.library();
    if (!lib) {
      return PointerRef();
    }
    if (key.kind == KeySegment::Kind::Handle) {
      BlockHandle h;
      h.index = uint32_t(key.handle & 0xffffffff);
      h.generation = uint32_t(key.handle >> 32);
      const bool in = selectedOnly ? list(p, ctx).contains(h) : self(p).contains(*lib, h);
      return in ? blockRoot(ctx, h) : PointerRef();
    }
    for (BlockHandle h : list(p, ctx)) {
      if (lib->get(h)->name() == key.text) {
        return blockRoot(ctx, h);
      }
    }
    return PointerRef();
  };
  c.keyOf = [](const PointerRef &, Context &ctx, const PointerRef &item) {
    return KeySegment::ofHandle(
        ctx.library() ? packed(ctx.library()->find(item.owner.block)) : 0);
  };
  return c;
}

/* The graphs that the scene lists, read-only: drive/'s commands change them. Their struct is
 * named, not linked: scene/ doesn't use graph/. */
CollectionAccessor graphsOf()
{
  CollectionAccessor c;
  c.itemStruct = StructId::of("coredcc.GraphDef");
  c.length = [](const PointerRef &p, Context &ctx) {
    return ctx.library() ? int64_t(self(p).graphs(*ctx.library()).size()) : int64_t(0);
  };
  c.at = [](const PointerRef &p, Context &ctx, int64_t i) {
    if (!ctx.library()) {
      return PointerRef();
    }
    const Vector<BlockHandle> &l = self(p).graphs(*ctx.library());
    return i >= 0 && i < int64_t(l.size()) ? blockRoot(ctx, l[int(i)]) : PointerRef();
  };
  c.lookup = [](const PointerRef &p, Context &ctx, const KeySegment &key) {
    const Library *lib = ctx.library();
    if (!lib) {
      return PointerRef();
    }
    for (BlockHandle h : self(p).graphs(*lib)) {
      const bool match = key.kind == KeySegment::Kind::Handle
                             ? packed(h) == key.handle
                             : lib->get(h)->name() == key.text;
      if (match) {
        return blockRoot(ctx, h);
      }
    }
    return PointerRef();
  };
  c.keyOf = [](const PointerRef &, Context &ctx, const PointerRef &item) {
    return KeySegment::ofHandle(
        ctx.library() ? packed(ctx.library()->find(item.owner.block)) : 0);
  };
  return c;
}

ApiStruct sceneStruct()
{
  ApiStruct st(kSceneType);
  st.uiName("Scene").description("Objects, and the time.");

  ValueAccessor name;
  name.get = [](const PointerRef &p, Context &ctx) {
    const Library *lib = ctx.library();
    const Datablock *b = lib ? lib->get(lib->find(p.owner.block)) : nullptr;
    return PropValue::ofString(b ? b->name().c_str() : "");
  };
  st.add(PropertyDef::stringProp("name"), std::move(name));

  const StructId object = StructId::of(kSceneObjectStruct);
  CollectionAccessor objects = collectionOf(false);
  objects.active = [](const PointerRef &p, Context &ctx) {
    return ctx.library() ? blockRoot(ctx, self(p).active(*ctx.library())) : PointerRef();
  };
  objects.activeMember = "activeObject";
  st.addCollection(PropertyDef::collectionProp("objects", object), std::move(objects));
  st.addCollection(PropertyDef::collectionProp("selectedObjects", object),
                   collectionOf(true));
  st.addCollection(PropertyDef::collectionProp("graphs", StructId::of("coredcc.GraphDef")),
                   graphsOf());

  PointerAccessor active;
  active.target = object;
  active.get = [](const PointerRef &p, Context &ctx) {
    return ctx.library() ? blockRoot(ctx, self(p).active(*ctx.library())) : PointerRef();
  };
  active.set = [](const PointerRef &p, Context &ctx, const PointerRef &target) {
    Library &lib = *ctx.library();
    if (target.isNull()) {
      return lib.writeRef(self(p).activeCell(), BlockId());
    }
    if (target.owner.kind != OwnerKey::Kind::Block ||
        self(p).indexOf(lib, target.owner.block) < 0)
    {
      return Status::error(StatusCode::FailedPrecondition,
                           "the object isn't in the scene");
    }
    return lib.writeRef(self(p).activeCell(), target.owner.block);
  };
  active.cell = [](const PointerRef &p, Context &) { return &self(p).activeCell(); };
  st.addPointer(PropertyDef::pointerProp("activeObject", object), std::move(active));

  ValueAccessor frame;
  frame.get = [](const PointerRef &p, Context &) {
    return PropValue::ofFloat(self(p).frame);
  };
  frame.set = [](const PointerRef &p, Context &, const PropValue &v) {
    self(p).frame = v.asFloat();
    return Status();
  };
  st.add(PropertyDef::floatProp("frame"), std::move(frame));

  addUnitSettings(st, [](const PointerRef &p) { return &self(p).units; });
  return st;
}

} // namespace

Status registerSceneType(Runtime &rt)
{
  CDCC_TRY(registerUnitSettings(rt));
  CDCC_TRY(rt.ext<ApiRegistry>().add(sceneStruct()).status());
  const std::string impl = std::string(kSceneType) + "#provider";
  CDCC_TRY((registerImpl<BlockProvider, SceneProvider>(rt, impl)));
  Result<Owned<BlockProvider>> provider = own<BlockProvider>(
      rt, impl, litestl::alloc::New<SceneProvider>("coredcc::SceneProvider"));
  if (!provider.ok()) {
    return provider.status();
  }
  BlockTypeInfo info;
  info.name = kSceneType;
  info.client = ClientId::of(kCoredccClient);
  info.dataStruct = StructId::of(kSceneType);
  info.payloadVersion = kScenePayload;
  info.storage = BlockStorage::Payload;
  return rt.ext<BlockTypeRegistry>().add(std::move(info), std::move(*provider)).status();
}

} // namespace detail

} // namespace coredcc
