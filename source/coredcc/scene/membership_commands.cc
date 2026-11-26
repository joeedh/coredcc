#include "coredcc/scene/commands.h"

#include "coredcc/api/change_bus.h"
#include "coredcc/api/ref_text.h"
#include "coredcc/command/block_commands.h"
#include "coredcc/command/block_edits.h"
#include "coredcc/scene/builtins.h"
#include "coredcc/scene/solve.h"

#include "litestl/binding/binding.h"
#include "litestl/util/map.h"

#include <string>

namespace coredcc {

namespace lbt = litestl::binding::types;
using litestl::util::Vector;

namespace detail {

Result<SceneOf> sceneOf(Context &ctx)
{
  SceneOf s;
  s.lib = ctx.library();
  s.handle = ctx.scene();
  s.scene = activeScene(ctx);
  if (!s.scene) {
    return Status::error(StatusCode::FailedPrecondition, "there is no scene");
  }
  return s;
}

PropertyDef objectsInput()
{
  return PropertyDef::stringProp("objects").access(PropAccess::Optional |
                                                   PropAccess::NotRemembered);
}

Result<Vector<BlockHandle>> blocksNamed(Context &ctx)
{
  const Library &lib = *ctx.library();
  const litestl::util::string text = ctx.inputs()->get("objects")->asString();
  Result<Vector<BlockId>> ids = parseBlockIds(ctx.runtime(), text.c_str());
  if (!ids.ok()) {
    return Status::error(ids.status().code(),
                         (std::string("objects: ") + ids.status().message()).c_str());
  }
  Vector<BlockHandle> out;
  litestl::util::Map<BlockHandle, char> seen;
  for (const BlockId &id : *ids) {
    const BlockHandle h = lib.find(id);
    const Datablock *b = lib.get(h);
    if (!b || b->state() != BlockState::Live) {
      return Status::error(
          StatusCode::NotFound,
          ("objects: there is no block " + formatBlockId(ctx.runtime(), id)).c_str());
    }
    if (!seen.lookup_ptr(h)) {
      seen.add(h, 0);
      out.append(h);
    }
  }
  return out;
}

Result<Vector<BlockHandle>> objectsOf(Context &ctx)
{
  Result<SceneOf> s = sceneOf(ctx);
  if (!s.ok()) {
    return s.status();
  }
  Result<Vector<BlockHandle>> named = blocksNamed(ctx);
  if (!named.ok()) {
    return named;
  }
  for (BlockHandle h : *named) {
    if (!s->scene->contains(*s->lib, h)) {
      return about(*s->lib,
                   h,
                   Status::error(StatusCode::FailedPrecondition, "isn't in the scene"));
    }
  }
  return named;
}

Result<Vector<BlockHandle>> targetObjects(Context &ctx)
{
  Result<SceneOf> s = sceneOf(ctx);
  if (!s.ok()) {
    return s.status();
  }
  if (!ctx.inputs()->get("objects")->isNone()) {
    Result<Vector<BlockHandle>> named = objectsOf(ctx);
    if (named.ok() && named->size() == 0) {
      return Status::error(StatusCode::FailedPrecondition, "objects: no object is named");
    }
    return named;
  }
  const Vector<BlockHandle> selected = s->scene->selected(*s->lib);
  Vector<BlockHandle> out;
  for (BlockHandle h : selected) {
    const ObjectFlags unreachable = ObjectFlags::Hidden | ObjectFlags::Unselectable;
    if ((flagsOf(*s->lib, h) & unreachable) == ObjectFlags::None) {
      out.append(h);
    }
  }
  if (out.size() == 0) {
    return Status::error(StatusCode::FailedPrecondition,
                         selected.size() == 0
                             ? "no object is selected"
                             : "what is selected is hidden or unselectable");
  }
  return out;
}

ObjectFlags flagsOf(const Library &lib, BlockHandle h)
{
  Result<ObjectRead> r = readObject(lib, h);
  return r.ok() ? ObjectFlags(r->info.flags) : ObjectFlags::None;
}

Status Skips::add(const Library &lib, BlockHandle h, const Status &why)
{
  Status s = about(lib, h, why);
  if (why.code() != StatusCode::FailedPrecondition &&
      why.code() != StatusCode::Unsupported)
  {
    return s;
  }
  if (count_++ == 0) {
    first_ = std::move(s);
  }
  return Status();
}

void Skips::output(Context &ctx) const
{
  ctx.outputs()->set("skipped", PropValue::ofInt(count_)).ignore();
}

Result<BlockHandle> blockOf(Context &ctx, const char *input)
{
  const StableRef *ref = ctx.inputs()->get(input)->tryPointer();
  if (!ref || ref->owner.isNull()) {
    return BlockHandle();
  }
  const Library *lib = ctx.library();
  const BlockHandle h = lib && ref->owner.kind == OwnerKey::Kind::Block
                            ? lib->find(ref->owner.block)
                            : BlockHandle();
  const Datablock *b = lib ? lib->get(h) : nullptr;
  if (!b || b->state() != BlockState::Live) {
    return Status::error(StatusCode::FailedPrecondition,
                         (std::string(input) + ": no such block").c_str());
  }
  return h;
}

PropValue pointerTo(const Library &lib, BlockHandle h)
{
  const Datablock *b = lib.get(h);
  const BlockTypeRegistry::Entry *e =
      lib.runtime().ext<BlockTypeRegistry>().find(b->type());
  StableRef ref;
  ref.owner = OwnerKey::ofBlock(b->id());
  ref.type = e ? e->info.dataStruct : StructId();
  return PropValue::ofPointer(ref);
}

Status about(const Library &lib, BlockHandle h, const Status &s)
{
  const Datablock *b = lib.get(h);
  const std::string name = "'" + std::string(b ? b->name().c_str() : "?") + "'";
  const std::string message = s.message();
  return message.rfind(name, 0) == 0 ? s : /* readObject's have it */
             Status::error(s.code(), (name + ": " + message).c_str());
}

} // namespace detail

namespace {

using detail::SceneOf;

constexpr const char *kObjects = "objects";

Status failed(const std::string &what)
{
  return Status::error(StatusCode::FailedPrecondition, what.c_str());
}

/* A scene, as an undo record keeps it. */
struct HeldScene {
  HistoryBlock block;

  Result<SceneOf> get(const Context &ctx) const
  {
    Result<Library *> lib = detail::libraryOf(ctx, block.library());
    if (!lib.ok()) {
      return lib.status();
    }
    SceneOf s;
    s.lib = *lib;
    s.handle = block.get();
    s.scene = Scene::of(*s.lib, s.handle);
    if (!s.scene) {
      return failed("the scene is gone");
    }
    return s;
  }
  void changed(Context &ctx, const char *member) const
  {
    ctx.runtime().ext<ChangeBus>().notify(OwnerKey::ofBlock(block.id()), member);
  }
};

void report(Context &ctx, const Status &s)
{
  if (!s.ok() && !ctx.runtime().hasStickyError()) {
    ctx.runtime().setStickyError(s); /* undo can't return a Status */
  }
}

/* A Pointer input that must be a member, or null. */
Result<BlockHandle> memberOf(Context &ctx, const char *input)
{
  Result<SceneOf> s = detail::sceneOf(ctx);
  if (!s.ok()) {
    return s.status();
  }
  Result<BlockHandle> h = detail::blockOf(ctx, input);
  if (h.ok() && *h && !s->scene->contains(*s->lib, *h)) {
    return detail::about(*s->lib, *h, failed("isn't in the scene"));
  }
  return h;
}

#define BIND_SCENE_COMMAND(st)                                                           \
  BIND_STRUCT_METHOD(st, canRun, MARGS("ctx"));                                          \
  BIND_STRUCT_METHOD(st, exec, MARGS("ctx"));                                            \
  BIND_STRUCT_METHOD(st, undo, MARGS("ctx"));                                            \
  BIND_STRUCT_METHOD(st, redo, MARGS("ctx"));                                            \
  BIND_STRUCT_METHOD(st, memSize, MARGS());                                              \
  BIND_STRUCT_METHOD(st, discard, MARGS())

/* ---- new ---- */

struct NewScene {
  LibraryRef lib;
  CreatedBlocks created;

  Status canRun(Context &ctx) const
  {
    return ctx.library() ? Status() : failed("the Context has no Library");
  }
  Status exec(Context &ctx)
  {
    Library &l = *ctx.library();
    lib = l.ref();
    Result<BlockHandle> h = newScene(l, ctx.inputs()->get("name")->asString().c_str());
    if (!h.ok()) {
      return h.status();
    }
    created.add(l, *h);
    return ctx.outputs()->set("scene", detail::pointerTo(l, *h));
  }
  void undo(Context &ctx)
  {
    Result<Library *> l = detail::libraryOf(ctx, lib);
    if (!l.ok()) {
      report(ctx, l.status());
      return;
    }
    created.undo(**l);
  }
  Status redo(Context &ctx)
  {
    Result<Library *> l = detail::libraryOf(ctx, lib);
    return l.ok() ? created.redo(**l) : l.status();
  }
  uint64_t memSize() const
  {
    return sizeof(*this) + created.memSize();
  }
  void discard()
  {
    if (Library *l = lib.get()) {
      created.discard(*l);
    }
  }
  static lbt::Struct<NewScene> *defineBindings()
  {
    auto *st = new lbt::Struct<NewScene>("coredcc::NewSceneCommand", sizeof(NewScene));
    BIND_SCENE_COMMAND(st);
    return st;
  }
};

/* ---- link ---- */

struct Link {
  HeldScene scene;
  Vector<HistoryBlock> objects;

  Status canRun(Context &ctx) const
  {
    Result<SceneOf> s = detail::sceneOf(ctx);
    if (!s.ok()) {
      return s.status();
    }
    Result<Vector<BlockHandle>> named = namedBlocks(ctx);
    if (!named.ok()) {
      return named.status();
    }
    for (BlockHandle h : *named) {
      CDCC_TRY(readObject(*s->lib, h).status());
    }
    return Status();
  }
  /* What `object` or `objects` names: Live blocks. */
  static Result<Vector<BlockHandle>> namedBlocks(Context &ctx)
  {
    Result<BlockHandle> h = detail::blockOf(ctx, "object");
    if (!h.ok()) {
      return h.status();
    }
    Vector<BlockHandle> out;
    if (ctx.inputs()->get("objects")->isNone()) {
      if (!*h) {
        return failed("object: no such block");
      }
      out.append(*h);
      return out;
    }
    if (*h) {
      return Status::error(StatusCode::InvalidArgument,
                           "both object and objects are given");
    }
    Result<Vector<BlockHandle>> named = detail::blocksNamed(ctx);
    return named.ok() && named->size() == 0 ? failed("objects: no object is named")
                                            : named;
  }
  Status exec(Context &ctx)
  {
    Result<SceneOf> s = detail::sceneOf(ctx);
    const Vector<BlockHandle> named = *namedBlocks(ctx);
    s->scene->pruneRemoved(*s->lib);
    CDCC_TRY(s->scene->addMany(*s->lib, {named.data(), size_t(named.size())}));
    scene.block = HistoryBlock(*s->lib, s->handle);
    for (BlockHandle h : named) {
      objects.append(HistoryBlock(*s->lib, h));
    }
    scene.changed(ctx, kObjects);
    return Status();
  }
  void undo(Context &ctx)
  {
    Result<SceneOf> s = scene.get(ctx);
    Vector<BlockId> ids;
    for (const HistoryBlock &o : objects) {
      ids.append(o.id());
    }
    report(ctx,
           s.ok()
               ? s->scene->removeMany(*s->lib, {ids.data(), size_t(ids.size())}).status()
               : s.status());
    if (s.ok()) {
      scene.changed(ctx, kObjects);
    }
  }
  Status redo(Context &ctx)
  {
    Result<SceneOf> s = scene.get(ctx);
    if (!s.ok()) {
      return s.status();
    }
    s->scene->pruneRemoved(*s->lib);
    Vector<BlockHandle> handles;
    for (const HistoryBlock &o : objects) {
      handles.append(o.get());
    }
    CDCC_TRY(s->scene->addMany(*s->lib, {handles.data(), size_t(handles.size())}));
    scene.changed(ctx, kObjects);
    return Status();
  }
  uint64_t memSize() const
  {
    return sizeof(*this) + uint64_t(objects.size()) * sizeof(HistoryBlock);
  }
  void discard()
  {
    scene.block.release();
    objects.clear();
  }
  static lbt::Struct<Link> *defineBindings()
  {
    auto *st = new lbt::Struct<Link>("coredcc::LinkCommand", sizeof(Link));
    BIND_SCENE_COMMAND(st);
    return st;
  }
};

/* ---- unlink ---- */

struct Unlink {
  HeldScene scene;
  Vector<HistoryBlock> objects;
  /* Where they were. */
  Vector<Scene::Place> places;

  Status canRun(Context &ctx) const
  {
    return detail::targetObjects(ctx).status();
  }
  Status exec(Context &ctx)
  {
    Result<SceneOf> s = detail::sceneOf(ctx);
    Result<Vector<BlockHandle>> targets = detail::targetObjects(ctx);
    s->scene->pruneRemoved(*s->lib);
    scene.block = HistoryBlock(*s->lib, s->handle);
    for (BlockHandle h : *targets) {
      objects.append(HistoryBlock(*s->lib, h));
    }
    CDCC_TRY(apply(ctx));
    ctx.outputs()->set("count", PropValue::ofInt(int64_t(objects.size()))).ignore();
    return Status();
  }
  Status apply(Context &ctx)
  {
    Result<SceneOf> s = scene.get(ctx);
    if (!s.ok()) {
      return s.status();
    }
    Vector<BlockId> ids;
    for (const HistoryBlock &o : objects) {
      ids.append(o.id());
    }
    Result<Vector<Scene::Place>> removed =
        s->scene->removeMany(*s->lib, {ids.data(), size_t(ids.size())});
    if (!removed.ok()) {
      return removed.status();
    }
    places = std::move(*removed);
    scene.changed(ctx, kObjects);
    return Status();
  }
  void undo(Context &ctx)
  {
    Result<SceneOf> s = scene.get(ctx);
    report(ctx,
           s.ok() ? s->scene->putBack(*s->lib, {places.data(), size_t(places.size())})
                  : s.status());
    if (s.ok()) {
      scene.changed(ctx, kObjects);
    }
  }
  Status redo(Context &ctx)
  {
    return apply(ctx);
  }
  uint64_t memSize() const
  {
    return sizeof(*this) + uint64_t(objects.size()) * sizeof(HistoryBlock) +
           uint64_t(places.size()) * sizeof(Scene::Place);
  }
  void discard()
  {
    scene.block.release();
    objects.clear();
  }
  static lbt::Struct<Unlink> *defineBindings()
  {
    auto *st = new lbt::Struct<Unlink>("coredcc::UnlinkCommand", sizeof(Unlink));
    BIND_SCENE_COMMAND(st);
    return st;
  }
};

/* ---- setActive ---- */

struct SetActive {
  HeldScene scene;
  BlockId before, after;

  Status canRun(Context &ctx) const
  {
    return memberOf(ctx, "object").status();
  }
  Status exec(Context &ctx)
  {
    Result<SceneOf> s = detail::sceneOf(ctx);
    const BlockHandle h = *memberOf(ctx, "object");
    before = s->scene->activeCell().target;
    after = h ? s->lib->get(h)->id() : BlockId();
    scene.block = HistoryBlock(*s->lib, s->handle);
    return write(ctx, after);
  }
  Status write(Context &ctx, const BlockId &id) const
  {
    Result<SceneOf> s = scene.get(ctx);
    if (!s.ok()) {
      return s.status();
    }
    CDCC_TRY(s->lib->writeRef(s->scene->activeCell(), id));
    scene.changed(ctx, "activeObject");
    return Status();
  }
  void undo(Context &ctx)
  {
    report(ctx, write(ctx, before));
  }
  Status redo(Context &ctx)
  {
    return write(ctx, after);
  }
  uint64_t memSize() const
  {
    return sizeof(*this);
  }
  void discard()
  {
    scene.block.release();
  }
  static lbt::Struct<SetActive> *defineBindings()
  {
    auto *st = new lbt::Struct<SetActive>("coredcc::SetActiveCommand", sizeof(SetActive));
    BIND_SCENE_COMMAND(st);
    return st;
  }
};

/* ---- select ---- */

enum class SelectMode : int64_t { Set, Add, Remove, Toggle, All, None };

struct Select {
  HeldScene scene;
  Vector<BlockId> before, after;

  static SelectMode mode(Context &ctx)
  {
    return SelectMode(ctx.inputs()->get("mode")->asEnum());
  }
  Status canRun(Context &ctx) const
  {
    Result<SceneOf> s = detail::sceneOf(ctx);
    if (!s.ok()) {
      return s.status();
    }
    const SelectMode m = mode(ctx);
    if (m == SelectMode::All || m == SelectMode::None) {
      return Status();
    }
    return namedObjects(ctx).status();
  }
  /* What `object` or `objects` names. */
  static Result<Vector<BlockHandle>> namedObjects(Context &ctx)
  {
    Result<BlockHandle> h = memberOf(ctx, "object");
    if (!h.ok()) {
      return h.status();
    }
    if (!ctx.inputs()->get("objects")->isNone()) {
      if (*h) {
        return Status::error(StatusCode::InvalidArgument,
                             "both object and objects are given");
      }
      return detail::objectsOf(ctx);
    }
    Vector<BlockHandle> out;
    if (*h) {
      out.append(*h);
    } else if (mode(ctx) != SelectMode::Set) {
      return failed("object: no such block");
    }
    return out;
  }
  Status exec(Context &ctx)
  {
    Result<SceneOf> s = detail::sceneOf(ctx);
    const Library &lib = *s->lib;
    scene.block = HistoryBlock(*s->lib, s->handle);
    for (const RefCell &c : s->scene->selection()) {
      before.append(c.target);
    }
    const SelectMode m = mode(ctx);
    if (m == SelectMode::All) {
      constexpr ObjectFlags skip = ObjectFlags::Hidden | ObjectFlags::Unselectable;
      const Vector<BlockHandle> objects = s->scene->objects(lib);
      for (BlockHandle h : objects) {
        if ((detail::flagsOf(lib, h) & skip) == ObjectFlags::None) {
          after.append(lib.get(h)->id());
        }
      }
    } else if (m != SelectMode::None) {
      Vector<BlockHandle> named;
      const Vector<BlockHandle> all = *namedObjects(ctx);
      for (BlockHandle h : all) {
        const ObjectFlags f = detail::flagsOf(lib, h) & ObjectFlags::Unselectable;
        if (m == SelectMode::Remove || f == ObjectFlags::None) {
          named.append(h);
        }
      }
      ctx.outputs()->set("skipped", PropValue::ofInt(all.size() - named.size())).ignore();
      const Vector<BlockHandle> selected =
          m == SelectMode::Set ? Vector<BlockHandle>() : s->scene->selected(lib);
      litestl::util::Map<BlockHandle, char> isNamed, isSelected;
      for (BlockHandle h : named) {
        isNamed.add(h, 0);
      }
      for (BlockHandle h : selected) {
        isSelected.add(h, 0);
        if (m == SelectMode::Add || !isNamed.lookup_ptr(h)) {
          after.append(lib.get(h)->id());
        }
      }
      for (BlockHandle h : named) {
        if (m != SelectMode::Remove && !isSelected.lookup_ptr(h)) {
          after.append(lib.get(h)->id());
        }
      }
    }
    return write(ctx, after);
  }
  Status write(Context &ctx, const Vector<BlockId> &ids) const
  {
    Result<SceneOf> s = scene.get(ctx);
    if (!s.ok()) {
      return s.status();
    }
    s->scene->setSelection(*s->lib, ids);
    scene.changed(ctx, "selectedObjects");
    return Status();
  }
  void undo(Context &ctx)
  {
    report(ctx, write(ctx, before));
  }
  Status redo(Context &ctx)
  {
    return write(ctx, after);
  }
  uint64_t memSize() const
  {
    return sizeof(*this) + uint64_t(before.size() + after.size()) * sizeof(BlockId);
  }
  void discard()
  {
    scene.block.release();
  }
  static lbt::Struct<Select> *defineBindings()
  {
    auto *st = new lbt::Struct<Select>("coredcc::SelectCommand", sizeof(Select));
    BIND_SCENE_COMMAND(st);
    return st;
  }
};

/* ---- add ---- */

struct AddObject {
  HeldScene scene;
  LibraryRef lib;
  CreatedBlocks created;
  BlockId object;
  /* With `select`: the selection and the active object as they were. */
  bool select = false;
  Vector<BlockId> selected;
  BlockId active;

  Status canRun(Context &ctx) const
  {
    CDCC_TRY(memberOf(ctx, "parent").status());
    Result<BlockHandle> data = detail::blockOf(ctx, "data");
    if (!data.ok() || !*data) {
      return data.status();
    }
    const Datablock *b = ctx.library()->get(*data);
    const BlockTypeRegistry::Entry *e =
        ctx.runtime().ext<BlockTypeRegistry>().find(b->type());
    return e && ctx.runtime().ext<ApiRegistry>().isA(e->info.dataStruct,
                                                     StructId::of(kObjectDataStruct))
               ? Status()
               : detail::about(
                     *ctx.library(), *data, failed("can't be an object's data"));
  }
  /* Where it is to be, in the world. */
  static mat4d targetOf(Context &ctx)
  {
    const PropValue location = *ctx.inputs()->get("location");
    const PropValue rotation = *ctx.inputs()->get("rotation");
    const PropValue scale = *ctx.inputs()->get("scale");
    quatd q;
    for (int i = 0; i < 4; i++) {
      q[i] = rotation.component(i);
    }
    double3 t, f;
    for (int i = 0; i < 3; i++) {
      t[i] = location.component(i);
      f[i] = scale.component(i);
    }
    return solve::affine(solve::Linear::of(q.normalized()).scaled(f), t);
  }
  /* The object's channels. */
  static Status place(Context &ctx, BlockHandle h, BlockHandle parent, double time)
  {
    Library *lib = ctx.library();
    TransformState parentState;
    if (parent) {
      Result<TransformState> s = computeState(*lib, parent, time);
      if (!s.ok()) {
        return s.status();
      }
      parentState = *s;
      CDCC_TRY(writeParent(ctx, h, lib->get(parent)->id()));
    }
    Result<ObjectRead> r = readObjectAt(*lib, h, time);
    if (!r.ok()) {
      return r.status();
    }
    Result<ChannelBlock> c =
        solveWorld(*r->type, r->ctx(), targetOf(ctx), parentState, SetWorldMode::Closest);
    return c.ok() ? writeTransform(ctx, h, *c) : c.status();
  }
  Status exec(Context &ctx)
  {
    Result<SceneOf> s = detail::sceneOf(ctx);
    Library *l = s->lib;
    lib = l->ref();
    const litestl::util::string type = ctx.inputs()->get("type")->asString();
    const litestl::util::string name = ctx.inputs()->get("name")->asString();
    Result<BlockHandle> h = newObject(*l, name.c_str(), type.c_str());
    if (!h.ok()) {
      return h.status();
    }
    scene.block = HistoryBlock(*l, s->handle);
    object = l->get(*h)->id();
    if (const BlockHandle data = *detail::blockOf(ctx, "data")) {
      RefCell &cell = static_cast<Object *>(l->get(*h)->clientData())->data.cell();
      l->writeRef(cell, l->get(data)->id()).ignore();
    }
    Status e = place(ctx, *h, *memberOf(ctx, "parent"), s->scene->time());
    if (e.ok()) {
      s->scene->pruneRemoved(*l);
      e = s->scene->add(*l, *h);
    }
    if (!e.ok()) {
      l->markDeleted(*h).ignore();
      l->destroyFromHistory(*h).ignore();
      return e;
    }
    created.add(*l, *h);
    scene.changed(ctx, kObjects);
    select = ctx.inputs()->get("select")->asBool();
    if (select) {
      for (const RefCell &c : s->scene->selection()) {
        selected.append(c.target);
      }
      active = s->scene->activeCell().target;
      Vector<BlockId> one;
      one.append(object);
      chosen(ctx, *s, one, object);
    }
    return ctx.outputs()->set("object", detail::pointerTo(*l, *h));
  }
  void chosen(Context &ctx,
              const SceneOf &s,
              const Vector<BlockId> &ids,
              const BlockId &id) const
  {
    s.scene->setSelection(*s.lib, ids);
    report(ctx, s.lib->writeRef(s.scene->activeCell(), id));
    scene.changed(ctx, "selectedObjects");
    scene.changed(ctx, "activeObject");
  }
  void undo(Context &ctx)
  {
    Result<SceneOf> s = scene.get(ctx);
    if (!s.ok()) {
      Library *l = lib.get();
      report(ctx, s.status());
      if (l && l == ctx.library()) {
        created.undo(*l); /* only the scene is gone */
      }
      return;
    }
    if (select) {
      chosen(ctx, *s, selected, active);
    }
    report(ctx, s->scene->remove(*s->lib, object).status());
    created.undo(*s->lib);
    scene.changed(ctx, kObjects);
  }
  Status redo(Context &ctx)
  {
    Result<SceneOf> s = scene.get(ctx);
    if (!s.ok()) {
      return s.status();
    }
    CDCC_TRY(created.redo(*s->lib));
    s->scene->pruneRemoved(*s->lib);
    if (Status e = s->scene->add(*s->lib, s->lib->find(object)); !e.ok()) {
      created.undo(*s->lib);
      return e;
    }
    scene.changed(ctx, kObjects);
    if (select) {
      Vector<BlockId> one;
      one.append(object);
      chosen(ctx, *s, one, object);
    }
    return Status();
  }
  uint64_t memSize() const
  {
    return sizeof(*this) + created.memSize() +
           uint64_t(selected.size()) * sizeof(BlockId);
  }
  void discard()
  {
    scene.block.release();
    if (Library *l = lib.get()) {
      created.discard(*l);
    }
  }
  static lbt::Struct<AddObject> *defineBindings()
  {
    auto *st = new lbt::Struct<AddObject>("coredcc::AddObjectCommand", sizeof(AddObject));
    BIND_SCENE_COMMAND(st);
    return st;
  }
};

PropertyDef blockInputDef(const char *name)
{
  return PropertyDef::pointerProp(name, StructId());
}

PropertyDef stringInput(const char *name, const char *value)
{
  return PropertyDef::stringProp(name).defaultValue(PropValue::ofString(value));
}

} // namespace

Status detail::registerMembershipCommands(Runtime &rt)
{
  CommandDef scene(kNewSceneCommand);
  scene.uiName("New Scene")
      .input(stringInput("name", "Scene"))
      .output(blockInputDef("scene"));
  CDCC_TRY(addCommandOnce<NewScene>(rt, std::move(scene)));

  CommandDef link(kLinkSceneCommand);
  link.uiName("Link to Scene")
      .input(blockInputDef("object"))
      .input(detail::objectsInput());
  CDCC_TRY(addCommandOnce<Link>(rt, std::move(link)));

  CommandDef unlink(kUnlinkSceneCommand);
  unlink.uiName("Unlink from Scene")
      .input(detail::objectsInput())
      .output(PropertyDef::intProp("count"));
  CDCC_TRY(addCommandOnce<Unlink>(rt, std::move(unlink)));

  CommandDef active(kSetSceneActiveCommand);
  active.uiName("Set Active Object").input(blockInputDef("object"));
  CDCC_TRY(addCommandOnce<SetActive>(rt, std::move(active)));

  CommandDef select(kSelectSceneCommand);
  select.uiName("Select")
      .input(PropertyDef::enumProp("mode")
                 .item("SET", int64_t(SelectMode::Set))
                 .item("ADD", int64_t(SelectMode::Add))
                 .item("REMOVE", int64_t(SelectMode::Remove))
                 .item("TOGGLE", int64_t(SelectMode::Toggle))
                 .item("ALL", int64_t(SelectMode::All))
                 .item("NONE", int64_t(SelectMode::None)))
      .input(blockInputDef("object"))
      .input(detail::objectsInput())
      .output(PropertyDef::intProp("skipped"));
  CDCC_TRY(addCommandOnce<Select>(rt, std::move(select)));

  CommandDef add(kNewObjectCommand);
  add.uiName("Add Object")
      .input(stringInput("type", kTrsTransform))
      .input(stringInput("name", "Object"))
      .input(PropertyDef::vecProp("location", 3)
                 .subtype(PropSubtype::Distance)
                 .unit(UnitFamily::Length))
      .input(
          PropertyDef::quatProp("rotation").defaultValue(PropValue::ofQuat(0, 0, 0, 1)))
      .input(PropertyDef::vecProp("scale", 3).defaultValue(PropValue::ofVec3(1, 1, 1)))
      .input(blockInputDef("parent"))
      .input(blockInputDef("data"))
      .input(PropertyDef::boolProp("select"))
      .output(blockInputDef("object"));
  CDCC_TRY(addCommandOnce<AddObject>(rt, std::move(add)));
  return registerDuplicateCommands(rt);
}

Status registerSceneCommands(Runtime &rt)
{
  CDCC_TRY(registerSceneTypes(rt));
  CDCC_TRY(registerBlockCommands(rt));
  CDCC_TRY(detail::registerMembershipCommands(rt));
  return detail::registerTransformCommands(rt);
}

} // namespace coredcc
