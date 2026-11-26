/* core.object.duplicate and core.scene.duplicate. They run other commands. */
#include "coredcc/api/ref_text.h"
#include "coredcc/command/block_edits.h"
#include "coredcc/command/executor.h"
#include "coredcc/command/library_commands.h"
#include "coredcc/lib/library.h"
#include "coredcc/scene/commands.h"
#include "coredcc/scene/state_cache.h"

#include "litestl/binding/binding.h"

#include <string>

namespace coredcc {

namespace lbt = litestl::binding::types;
using detail::HandleSet;
using detail::SceneOf;
using litestl::util::Vector;

namespace {

Status failed(const std::string &what)
{
  return Status::error(StatusCode::FailedPrecondition, what.c_str());
}

/* Blocks to copy, in the order they came. */
struct Copied {
  Vector<BlockHandle> blocks;
  HandleSet set;

  void add(BlockHandle h)
  {
    if (h && set.add(h, int(blocks.size()))) {
      blocks.append(h);
    }
  }
};

/* Their data blocks. */
Status addData(const Library &lib, Copied &copied)
{
  const int n = int(copied.blocks.size());
  for (int i = 0; i < n; i++) {
    Result<ObjectRead> r = readObject(lib, copied.blocks[i]);
    if (r.ok()) {
      copied.add(detail::liveBlock(lib, r->info.data));
    } else if (lib.query<Transformable>(copied.blocks[i])) {
      return detail::about(lib, copied.blocks[i], r.status());
    }
  }
  return Status();
}

/* Runs a sub-command. */
Result<PropertyBag> run(Context &ctx,
                        const char *command,
                        std::initializer_list<std::pair<const char *, PropValue>> inputs)
{
  const CommandDef *def = ctx.runtime().ext<CommandRegistry>().find(command);
  PropertyBag in = def->newInputs();
  for (const auto &[name, value] : inputs) {
    if (Status s = in.set(name, value); !s.ok()) {
      return s;
    }
  }
  return ctx.executor()->runSub(ctx, *def, std::move(in));
}

PropValue idsOf(const Library &lib, const Vector<BlockHandle> &blocks)
{
  Vector<BlockId> ids;
  for (BlockHandle h : blocks) {
    ids.append(lib.get(h)->id());
  }
  const std::span<const BlockId> all(ids.data(), size_t(ids.size()));
  return PropValue::ofString(formatBlockIds(lib.runtime(), all).c_str());
}

/* The copies, in the order of the blocks. */
Result<Vector<BlockHandle>> copy(Context &ctx, const Copied &copied)
{
  const Library &lib = *ctx.library();
  Result<PropertyBag> out =
      run(ctx, kDuplicateBlockCommand, {{"blocks", idsOf(lib, copied.blocks)}});
  if (!out.ok()) {
    return out.status();
  }
  Result<Vector<BlockId>> ids =
      parseBlockIds(ctx.runtime(), out->get("blocks")->asString().c_str());
  if (!ids.ok()) {
    return ids.status();
  }
  Vector<BlockHandle> copies;
  for (const BlockId &id : *ids) {
    copies.append(lib.find(id));
  }
  return copies;
}

/* Everything that they do is a sub-command's. */
struct Composite {
  void undo(Context &)
  {
  }
  Status redo(Context &)
  {
    return Status();
  }
  uint64_t memSize() const
  {
    return sizeof(*this);
  }
  void discard()
  {
  }
};

#define BIND_COMPOSITE(st)                                                               \
  BIND_STRUCT_METHOD(st, canRun, MARGS("ctx"));                                          \
  BIND_STRUCT_METHOD(st, exec, MARGS("ctx"));                                            \
  BIND_STRUCT_METHOD(st, undo, MARGS("ctx"));                                            \
  BIND_STRUCT_METHOD(st, redo, MARGS("ctx"));                                            \
  BIND_STRUCT_METHOD(st, memSize, MARGS());                                              \
  BIND_STRUCT_METHOD(st, discard, MARGS())

/* ---- objects ---- */

struct DuplicateObjects : Composite {
  Status canRun(Context &ctx) const
  {
    return detail::targetObjects(ctx).status();
  }
  Status exec(Context &ctx)
  {
    const SceneOf s = *detail::sceneOf(ctx);
    const PropertyBag &in = *ctx.inputs();
    Copied copied;
    const Vector<BlockHandle> targets = *detail::targetObjects(ctx);
    for (BlockHandle h : targets) {
      copied.add(h);
    }
    if (in.get("children")->asBool()) {
      const HandleSet named = copied.set;
      detail::Ancestry ancestry(*s.lib, named);
      const Vector<BlockHandle> members = s.scene->objects(*s.lib);
      for (BlockHandle h : members) {
        Result<bool> below = ancestry.below(h);
        if (!below.ok()) {
          return detail::about(*s.lib, h, below.status());
        }
        if (*below) {
          copied.add(h);
        }
      }
    }
    const int count = int(copied.blocks.size());
    if (!in.get("linkData")->asBool()) {
      CDCC_TRY(addData(*s.lib, copied));
    }
    const BlockHandle active = s.scene->active(*s.lib);
    Result<Vector<BlockHandle>> copies = copy(ctx, copied);
    if (!copies.ok()) {
      return copies.status();
    }
    BlockHandle newActive;
    Vector<BlockHandle> objects;
    for (int i = 0; i < count; i++) {
      objects.append((*copies)[i]);
      newActive = copied.blocks[i] == active ? (*copies)[i] : newActive;
    }
    const PropValue ids = idsOf(*s.lib, objects);
    CDCC_TRY(run(ctx, kLinkSceneCommand, {{"objects", ids}}).status());
    if (in.get("select")->asBool()) {
      CDCC_TRY(run(ctx, kSelectSceneCommand, {{"objects", ids}}).status());
      if (newActive) {
        const PropValue p = detail::pointerTo(*s.lib, newActive);
        CDCC_TRY(run(ctx, kSetSceneActiveCommand, {{"object", p}}).status());
      }
    }
    ctx.outputs()->set("count", PropValue::ofInt(count)).ignore();
    return ctx.outputs()->set("objects", ids);
  }
  static lbt::Struct<DuplicateObjects> *defineBindings()
  {
    auto *st = new lbt::Struct<DuplicateObjects>("coredcc::DuplicateObjectsCommand",
                                                 sizeof(DuplicateObjects));
    BIND_COMPOSITE(st);
    return st;
  }
};

/* ---- scenes ---- */

struct DuplicateScene : Composite {
  static Result<BlockHandle> source(Context &ctx)
  {
    Result<BlockHandle> h = detail::blockOf(ctx, "scene");
    if (!h.ok()) {
      return h;
    }
    const BlockHandle scene = *h ? *h : ctx.scene();
    if (!ctx.library() || !Scene::of(*ctx.library(), scene)) {
      return failed(*h ? "scene: the block isn't a scene" : "there is no scene");
    }
    return scene;
  }
  Status canRun(Context &ctx) const
  {
    return source(ctx).status();
  }
  Status exec(Context &ctx)
  {
    Result<BlockHandle> from = source(ctx);
    if (!from.ok()) {
      return from.status();
    }
    Library &lib = *ctx.library();
    Copied copied;
    copied.add(*from);
    if (ctx.inputs()->get("objects")->asBool()) {
      const Vector<BlockHandle> members = Scene::of(lib, *from)->objects(lib);
      for (BlockHandle h : members) {
        copied.add(h);
      }
      if (!ctx.inputs()->get("linkData")->asBool()) {
        CDCC_TRY(addData(lib, copied));
      }
    }
    Result<Vector<BlockHandle>> copies = copy(ctx, copied);
    if (!copies.ok()) {
      return copies.status();
    }
    const BlockHandle scene = (*copies)[0];
    lib.setFlag(scene, BlockFlags::Root, true); /* it stays with the record */
    const PropValue p = detail::pointerTo(lib, scene);
    const litestl::util::string name = ctx.inputs()->get("name")->asString();
    if (name.size() > 0) {
      CDCC_TRY(run(ctx,
                   kRenameBlockCommand,
                   {{"block", p}, {"name", PropValue::ofString(name.c_str())}})
                   .status());
    }
    return ctx.outputs()->set("scene", p);
  }
  static lbt::Struct<DuplicateScene> *defineBindings()
  {
    auto *st = new lbt::Struct<DuplicateScene>("coredcc::DuplicateSceneCommand",
                                               sizeof(DuplicateScene));
    BIND_COMPOSITE(st);
    return st;
  }
};

} // namespace

Status detail::registerDuplicateCommands(Runtime &rt)
{
  const auto flag = [](const char *name, bool value) {
    return PropertyDef::boolProp(name).defaultValue(PropValue::ofBool(value));
  };
  CommandDef objects(kDuplicateObjectCommand);
  objects.uiName("Duplicate Objects")
      .input(objectsInput())
      .input(flag("linkData", true))
      .input(flag("children", false))
      .input(flag("select", true))
      .output(PropertyDef::stringProp("objects"))
      .output(PropertyDef::intProp("count"));
  CDCC_TRY(addCommandOnce<DuplicateObjects>(rt, std::move(objects)));

  CommandDef scene(kDuplicateSceneCommand);
  scene.uiName("Duplicate Scene")
      .input(PropertyDef::pointerProp("scene", StructId()))
      .input(PropertyDef::stringProp("name"))
      .input(flag("objects", true))
      .input(flag("linkData", true))
      .output(PropertyDef::pointerProp("scene", StructId()));
  return addCommandOnce<DuplicateScene>(rt, std::move(scene));
}

} // namespace coredcc
