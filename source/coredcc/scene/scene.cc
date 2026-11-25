#include "coredcc/scene/scene.h"

#include "coredcc/lib/provider.h"
#include "coredcc/scene/evaluator.h"

#include "litestl/binding/binding.h"
#include "litestl/util/alloc.h"

#include <string>

namespace coredcc {

namespace lbt = litestl::binding::types;
using litestl::util::Vector;

Scene::Scene() : evaluator_(std::make_unique<detail::SceneEvaluator>()) {}
Scene::~Scene() = default;

Scene *Scene::of(const Library &lib, BlockHandle h)
{
  const Datablock *b = lib.get(h);
  return b && b->type() == TypeId::of(kSceneType) ? static_cast<Scene *>(lib.data(h)) : nullptr;
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
  for (int i = 0; i < int(cells_.size()); i++) {
    if (!cells_[i].isNull()) {
      index_.add(clients.canonical(cells_[i].target), i);
    }
  }
  indexValid_ = true;
  indexCount_ = int(cells_.size());
  indexGeneration_ = clients.generation();
}

int Scene::indexOf(const Library &lib, const BlockId &id) const
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
    for (const RefCell &c : cells_) {
      if (const BlockHandle h = lib.resolveRef(c)) {
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
  if (contains(lib, h)) {
    return Status::error(StatusCode::AlreadyExists,
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
  const int at = indexOf(lib, id);
  if (at < 0) {
    return Status::error(StatusCode::NotFound, "Scene::remove: the block isn't in the scene");
  }
  if (lib.busy()) {
    return Status::error(StatusCode::Busy, "Scene::remove: the library is busy");
  }
  cells_.remove_at(at);
  lib.noteRefsChanged();
  return at;
}

int Scene::prune(Library &lib)
{
  int kept = 0;
  const int n = int(cells_.size());
  for (int i = 0; i < n; i++) {
    if (!cells_[i].isNull() && lib.find(cells_[i].target)) {
      cells_[kept++] = cells_[i];
    }
  }
  if (kept < n) {
    cells_.resize(kept);
    lib.noteRefsChanged();
  }
  return n - kept;
}

Vector<BlockHandle> Scene::selected(const Library &lib) const
{
  Vector<BlockHandle> out;
  for (const RefCell &c : selection_) {
    const BlockHandle h = lib.resolveRef(c);
    if (h && contains(lib, h)) {
      out.append(h);
    }
  }
  return out;
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

/* ---- evaluation ---- */

bool Scene::evaluated(BlockHandle h, ObjectEval &out) const
{
  const ObjectEval *e = evaluator_->front().find(h);
  if (e) {
    out = *e;
  }
  return e != nullptr;
}

Vector<BlockHandle> Scene::evaluatedObjects() const
{
  const detail::EvaluatedStore &s = evaluator_->front();
  return s.index ? s.index->handles() : Vector<BlockHandle>();
}

const Scene::Counters &Scene::counters() const
{
  return evaluator_->counters;
}

const SceneBvh &Scene::bvh() const
{
  return evaluator_->bvh;
}

/* ---- the block ---- */

namespace {

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
  static lbt::Struct<SceneProvider> *defineBindings()
  {
    auto *st = new lbt::Struct<SceneProvider>("coredcc::SceneProvider", sizeof(SceneProvider));
    BIND_STRUCT_METHOD(st, data, MARGS("block"));
    BIND_STRUCT_METHOD(st, foreachRef, MARGS("block", "visitor"));
    BIND_STRUCT_METHOD(st, destroy, MARGS("block"));
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
  const auto list = [selectedOnly](const PointerRef &p, Context &ctx) {
    return selectedOnly ? self(p).selected(*ctx.library()) : Handles();
  };
  CollectionAccessor c;
  c.itemStruct = StructId::of(kObjectType);
  c.length = [selectedOnly, list](const PointerRef &p, Context &ctx) {
    return !ctx.library() ? int64_t(0) :
           selectedOnly   ? int64_t(list(p, ctx).size()) :
                            int64_t(self(p).objects(*ctx.library()).size());
  };
  c.at = [selectedOnly, list](const PointerRef &p, Context &ctx, int64_t i) {
    if (!ctx.library()) {
      return PointerRef();
    }
    if (selectedOnly) {
      const Handles l = list(p, ctx);
      return i >= 0 && i < int64_t(l.size()) ? blockRoot(ctx, l[int(i)]) : PointerRef();
    }
    const Handles &l = self(p).objects(*ctx.library());
    return i >= 0 && i < int64_t(l.size()) ? blockRoot(ctx, l[int(i)]) : PointerRef();
  };
  c.lookup = [selectedOnly, list](const PointerRef &p, Context &ctx, const KeySegment &key) {
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
    const Handles some = list(p, ctx);
    for (BlockHandle h : selectedOnly ? some : self(p).objects(*lib)) {
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

  const StructId object = StructId::of(kObjectType);
  CollectionAccessor objects = collectionOf(false);
  objects.active = [](const PointerRef &p, Context &ctx) {
    return ctx.library() ? blockRoot(ctx, self(p).active(*ctx.library())) : PointerRef();
  };
  objects.activeMember = "activeObject";
  st.addCollection(PropertyDef::collectionProp("objects", object), std::move(objects));
  st.addCollection(PropertyDef::collectionProp("selectedObjects", object), collectionOf(true));

  PointerAccessor active;
  active.target = object;
  active.candidates.append(StructId::of(kSceneObjectStruct));
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
      return Status::error(StatusCode::FailedPrecondition, "the object isn't in the scene");
    }
    return lib.writeRef(self(p).activeCell(), target.owner.block);
  };
  active.cell = [](const PointerRef &p, Context &) { return &self(p).activeCell(); };
  st.addPointer(PropertyDef::pointerProp("activeObject", object), std::move(active));

  ValueAccessor frame;
  frame.get = [](const PointerRef &p, Context &) { return PropValue::ofFloat(self(p).frame); };
  frame.set = [](const PointerRef &p, Context &, const PropValue &v) {
    self(p).frame = v.asFloat();
    return Status();
  };
  st.add(PropertyDef::floatProp("frame"), std::move(frame));

  ValueAccessor fps;
  fps.get = [](const PointerRef &p, Context &) { return PropValue::ofFloat(self(p).fps); };
  fps.set = [](const PointerRef &p, Context &, const PropValue &v) {
    self(p).fps = v.asFloat();
    return Status();
  };
  st.add(PropertyDef::floatProp("fps").range(0.001, 1e6).defaultValue(PropValue::ofFloat(24.0)), std::move(fps));
  return st;
}

} // namespace

Status registerSceneType(Runtime &rt)
{
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
  info.storage = BlockStorage::Transient; /* until step 11 gives it a payload */
  return rt.ext<BlockTypeRegistry>().add(std::move(info), std::move(*provider)).status();
}

} // namespace detail

} // namespace coredcc
