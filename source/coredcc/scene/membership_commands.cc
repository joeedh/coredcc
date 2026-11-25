#include "coredcc/scene/commands.h"

#include "coredcc/api/change_bus.h"
#include "coredcc/api/ref_text.h"
#include "coredcc/command/block_commands.h"
#include "coredcc/command/block_edits.h"
#include "coredcc/scene/builtins.h"

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
  return PropertyDef::stringProp("objects")
      .access(PropAccess::Optional | PropAccess::NotRemembered);
}

Result<Vector<BlockHandle>> objectsOf(Context &ctx)
{
  Result<SceneOf> s = sceneOf(ctx);
  if (!s.ok()) {
    return s.status();
  }
  const litestl::util::string text = ctx.inputs()->get("objects")->asString();
  Result<Vector<BlockId>> ids = parseBlockIds(ctx.runtime(), text.c_str());
  if (!ids.ok()) {
    return Status::error(ids.status().code(),
                         (std::string("objects: ") + ids.status().message()).c_str());
  }
  Vector<BlockHandle> out;
  litestl::util::Map<BlockHandle, char> seen;
  for (const BlockId &id : *ids) {
    const BlockHandle h = s->lib->find(id);
    const Datablock *b = s->lib->get(h);
    if (!b || b->state() != BlockState::Live) {
      return Status::error(
          StatusCode::NotFound,
          ("objects: there is no block " + formatBlockId(ctx.runtime(), id)).c_str());
    }
    if (!s->scene->contains(*s->lib, h)) {
      return about(*s->lib, h, Status::error(StatusCode::FailedPrecondition, "isn't in the scene"));
    }
    if (!seen.lookup_ptr(h)) {
      seen.add(h, 0);
      out.append(h);
    }
  }
  return out;
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
  Vector<BlockHandle> selected = s->scene->selected(*s->lib);
  if (selected.size() == 0) {
    return Status::error(StatusCode::FailedPrecondition, "no object is selected");
  }
  return selected;
}

Result<BlockHandle> blockOf(Context &ctx, const char *input)
{
  const StableRef *ref = ctx.inputs()->get(input)->tryPointer();
  if (!ref || ref->owner.isNull()) {
    return BlockHandle();
  }
  const Library *lib = ctx.library();
  const BlockHandle h = lib && ref->owner.kind == OwnerKey::Kind::Block ?
                            lib->find(ref->owner.block) :
                            BlockHandle();
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
  const BlockTypeRegistry::Entry *e = lib.runtime().ext<BlockTypeRegistry>().find(b->type());
  StableRef ref;
  ref.owner = OwnerKey::ofBlock(b->id());
  ref.type = e ? e->info.dataStruct : StructId();
  return PropValue::ofPointer(ref);
}

Status about(const Library &lib, BlockHandle h, const Status &s)
{
  const Datablock *b = lib.get(h);
  return Status::error(
      s.code(), ("'" + std::string(b ? b->name().c_str() : "?") + "': " + s.message()).c_str());
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
  HistoryBlock object;

  Status canRun(Context &ctx) const
  {
    Result<SceneOf> s = detail::sceneOf(ctx);
    if (!s.ok()) {
      return s.status();
    }
    Result<BlockHandle> h = detail::blockOf(ctx, "object");
    if (!h.ok()) {
      return h.status();
    }
    return *h ? readObject(*s->lib, *h).status() : failed("object: no such block");
  }
  Status exec(Context &ctx)
  {
    Result<SceneOf> s = detail::sceneOf(ctx);
    const BlockHandle h = *detail::blockOf(ctx, "object");
    s->scene->prune(*s->lib);
    CDCC_TRY(s->scene->add(*s->lib, h));
    scene.block = HistoryBlock(*s->lib, s->handle);
    object = HistoryBlock(*s->lib, h);
    scene.changed(ctx, kObjects);
    return Status();
  }
  void undo(Context &ctx)
  {
    Result<SceneOf> s = scene.get(ctx);
    report(ctx, s.ok() ? s->scene->remove(*s->lib, object.id()).status() : s.status());
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
    s->scene->prune(*s->lib);
    CDCC_TRY(s->scene->add(*s->lib, object.get()));
    scene.changed(ctx, kObjects);
    return Status();
  }
  uint64_t memSize() const
  {
    return sizeof(*this);
  }
  void discard()
  {
    scene.block.release();
    object.release();
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
  struct Removed {
    HistoryBlock object;
    /* The member before it, or null, and its place: just before it was removed. */
    BlockId previous;
    int index = 0;
  };
  HeldScene scene;
  Vector<Removed> removed;

  Status canRun(Context &ctx) const
  {
    return detail::targetObjects(ctx).status();
  }
  Status exec(Context &ctx)
  {
    Result<SceneOf> s = detail::sceneOf(ctx);
    Result<Vector<BlockHandle>> selected = detail::targetObjects(ctx);
    s->scene->prune(*s->lib);
    scene.block = HistoryBlock(*s->lib, s->handle);
    for (BlockHandle h : *selected) {
      Removed r;
      r.object = HistoryBlock(*s->lib, h);
      removed.append(std::move(r));
    }
    CDCC_TRY(apply(ctx));
    ctx.outputs()->set("count", PropValue::ofInt(int64_t(removed.size()))).ignore();
    return Status();
  }
  Status apply(Context &ctx)
  {
    Result<SceneOf> s = scene.get(ctx);
    if (!s.ok()) {
      return s.status();
    }
    for (int i = 0; i < int(removed.size()); i++) {
      Removed &r = removed[i];
      const int at = s->scene->indexOf(*s->lib, r.object.id());
      r.index = at;
      r.previous = at > 0 ? s->scene->cells()[at - 1].target : BlockId();
      if (Status e = s->scene->remove(*s->lib, r.object.id()).status(); !e.ok()) {
        putBack(*s, i).ignore();
        return e;
      }
    }
    scene.changed(ctx, kObjects);
    return Status();
  }
  /* The first `count` of them. */
  Status putBack(const SceneOf &s, int count) const
  {
    Status first;
    for (int i = count - 1; i >= 0; i--) {
      const Removed &r = removed[i];
      const int previous = s.scene->indexOf(*s.lib, r.previous);
      const int at = previous >= 0 ? previous + 1 : r.index;
      if (Status e = s.scene->add(*s.lib, r.object.get(), at); !e.ok() && first.ok()) {
        first = std::move(e);
      }
    }
    return first;
  }
  void undo(Context &ctx)
  {
    Result<SceneOf> s = scene.get(ctx);
    report(ctx, s.ok() ? putBack(*s, int(removed.size())) : s.status());
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
    return sizeof(*this) + uint64_t(removed.size()) * sizeof(Removed);
  }
  void discard()
  {
    scene.block.release();
    removed.clear();
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
    Result<Vector<BlockHandle>> named = namedObjects(ctx);
    if (!named.ok()) {
      return named.status();
    }
    if (m == SelectMode::Remove) {
      return Status();
    }
    for (BlockHandle h : *named) {
      if ((flagsOf(*s->lib, h) & uint32_t(ObjectFlags::Unselectable)) != 0) {
        return detail::about(*s->lib, h, failed("isn't selectable"));
      }
    }
    return Status();
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
        return Status::error(StatusCode::InvalidArgument, "both object and objects are given");
      }
      return detail::objectsOf(ctx);
    }
    Vector<BlockHandle> out;
    if (*h) {
      out.append(*h);
    }
    else if (mode(ctx) != SelectMode::Set) {
      return failed("object: no such block");
    }
    return out;
  }
  /* A block that isn't Transformable has none. */
  static uint32_t flagsOf(const Library &lib, BlockHandle h)
  {
    Result<ObjectRead> r = readObject(lib, h);
    return r.ok() ? r->info.flags : 0;
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
      constexpr uint32_t skip = uint32_t(ObjectFlags::Hidden | ObjectFlags::Unselectable);
      const Vector<BlockHandle> objects = s->scene->objects(lib);
      for (BlockHandle h : objects) {
        if ((flagsOf(lib, h) & skip) == 0) {
          after.append(lib.get(h)->id());
        }
      }
    }
    else if (m != SelectMode::None) {
      const Vector<BlockHandle> named = *namedObjects(ctx);
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

  Status canRun(Context &ctx) const
  {
    return memberOf(ctx, "parent").status();
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
    Result<ObjectRead> r = readObject(*lib, h);
    if (!r.ok()) {
      return r.status();
    }
    const PropValue location = *ctx.inputs()->get("location");
    mat4d target;
    target.identity();
    double *m = target;
    for (int i = 0; i < 3; i++) {
      m[12 + i] = location.component(i);
    }
    Result<ChannelBlock> c = solveWorld(
        *r->type, EvalCtx(r->channels, time), target, parentState, SetWorldMode::Closest);
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
    Status e = place(ctx, *h, *memberOf(ctx, "parent"), s->scene->time());
    if (e.ok()) {
      s->scene->prune(*l);
      e = s->scene->add(*l, *h);
    }
    if (!e.ok()) {
      l->markDeleted(*h).ignore();
      l->destroyFromHistory(*h).ignore();
      return e;
    }
    created.add(*l, *h);
    scene.changed(ctx, kObjects);
    return ctx.outputs()->set("object", detail::pointerTo(*l, *h));
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
    s->scene->prune(*s->lib);
    if (Status e = s->scene->add(*s->lib, s->lib->find(object)); !e.ok()) {
      created.undo(*s->lib);
      return e;
    }
    scene.changed(ctx, kObjects);
    return Status();
  }
  uint64_t memSize() const
  {
    return sizeof(*this) + created.memSize();
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
  scene.uiName("New Scene").input(stringInput("name", "Scene")).output(blockInputDef("scene"));
  CDCC_TRY(addCommandOnce<NewScene>(rt, std::move(scene)));

  CommandDef link(kLinkCommand);
  link.uiName("Link to Scene").input(blockInputDef("object"));
  CDCC_TRY(addCommandOnce<Link>(rt, std::move(link)));

  CommandDef unlink(kUnlinkCommand);
  unlink.uiName("Unlink from Scene")
      .input(detail::objectsInput())
      .output(PropertyDef::intProp("count"));
  CDCC_TRY(addCommandOnce<Unlink>(rt, std::move(unlink)));

  CommandDef active(kSetActiveCommand);
  active.uiName("Set Active Object").input(blockInputDef("object"));
  CDCC_TRY(addCommandOnce<SetActive>(rt, std::move(active)));

  CommandDef select(kSelectCommand);
  select.uiName("Select")
      .input(PropertyDef::enumProp("mode")
                 .item("SET", int64_t(SelectMode::Set))
                 .item("ADD", int64_t(SelectMode::Add))
                 .item("REMOVE", int64_t(SelectMode::Remove))
                 .item("TOGGLE", int64_t(SelectMode::Toggle))
                 .item("ALL", int64_t(SelectMode::All))
                 .item("NONE", int64_t(SelectMode::None)))
      .input(blockInputDef("object"))
      .input(detail::objectsInput());
  CDCC_TRY(addCommandOnce<Select>(rt, std::move(select)));

  CommandDef add(kAddObjectCommand);
  add.uiName("Add Object")
      .input(stringInput("type", kTrsTransform))
      .input(stringInput("name", "Object"))
      .input(PropertyDef::vecProp("location", 3)
                 .subtype(PropSubtype::Distance)
                 .unit(UnitFamily::Length))
      .input(blockInputDef("parent"))
      .output(blockInputDef("object"));
  return addCommandOnce<AddObject>(rt, std::move(add));
}

Status registerSceneCommands(Runtime &rt)
{
  CDCC_TRY(registerSceneTypes(rt));
  CDCC_TRY(registerBlockCommands(rt));
  CDCC_TRY(detail::registerMembershipCommands(rt));
  return detail::registerTransformCommands(rt);
}

} // namespace coredcc
