/* Objects by path. */
#include "coredcc/api/context.h"
#include "coredcc/scene/channel_io.h"
#include "coredcc/scene/host_channels.h"
#include "coredcc/scene/object.h"

#include <string>

namespace coredcc::detail {

PointerRef blockRoot(Context &ctx, BlockHandle h)
{
  const Library *lib = ctx.library();
  const Datablock *b = lib ? lib->get(h) : nullptr;
  if (!b || b->state() != BlockState::Live) {
    return PointerRef();
  }
  const BlockTypeRegistry::Entry *e =
      ctx.runtime().ext<BlockTypeRegistry>().find(b->type());
  void *data = lib->data(h);
  return e && data ? PointerRef{data, e->info.dataStruct, OwnerKey::ofBlock(b->id())}
                   : PointerRef();
}

namespace {

Object &self(const PointerRef &p)
{
  return *static_cast<Object *>(p.data);
}

Status setCell(Context &ctx, RefCell &cell, const PointerRef &target)
{
  if (!target.isNull() && target.owner.kind != OwnerKey::Kind::Block) {
    return Status::error(StatusCode::TypeMismatch, "the target isn't a block");
  }
  return ctx.library()->writeRef(cell, target.isNull() ? BlockId() : target.owner.block);
}

/* The block of an instance, of any struct. */
BlockHandle blockOf(const PointerRef &p, Context &ctx)
{
  return ctx.library() && p.owner.kind == OwnerKey::Kind::Block
             ? ctx.library()->find(p.owner.block)
             : BlockHandle();
}

/* What the block's Transformable says. An exception stays sticky: whoever reads gets it.
 */
ObjectRead readOf(const PointerRef &p, Context &ctx)
{
  ObjectRead r;
  const BlockHandle h = blockOf(p, ctx);
  if (h) {
    r.iface = ctx.library()->query<Transformable>(h);
  }
  if (r.iface && !ctx.runtime().hasStickyError()) {
    r.iface.read(r.info, r.channels);
    r.type = ctx.runtime().ext<TransformRegistry>().find(r.info.transformType);
  }
  return r;
}

void addName(ApiStruct &st)
{
  ValueAccessor name;
  name.get = [](const PointerRef &p, Context &ctx) {
    const Library *lib = ctx.library();
    const Datablock *b = lib ? lib->get(lib->find(p.owner.block)) : nullptr;
    return PropValue::ofString(b ? b->name().c_str() : "");
  };
  st.add(PropertyDef::stringProp("name"), std::move(name));
}

PropertyDef flagsDef()
{
  return PropertyDef::flagsProp("flags")
      .item("HIDDEN", int64_t(ObjectFlags::Hidden))
      .item("UNSELECTABLE", int64_t(ObjectFlags::Unselectable));
}

Status checkParent(const PointerRef &p, Context &ctx, const PointerRef &target)
{
  const Library *lib = ctx.library();
  return !target.isNull() && target.owner.kind == OwnerKey::Kind::Block &&
                 isSelfOrAncestor(*lib, blockOf(p, ctx), lib->find(target.owner.block))
             ? Status::error(StatusCode::FailedPrecondition,
                             "an object can't be its own ancestor")
             : Status();
}

Status notABlock()
{
  return Status::error(StatusCode::TypeMismatch, "the target isn't a block");
}

} // namespace

/* Setting writes without notifying: setValue does that. */
ApiStruct sceneObjectStruct()
{
  ApiStruct st(kSceneObjectStruct);
  st.uiName("Scene Object").description("Something placed in a scene.");
  addName(st);

  ValueAccessor flags;
  flags.get = [](const PointerRef &p, Context &ctx) {
    return PropValue::ofFlags(readOf(p, ctx).info.flags);
  };
  flags.set = [](const PointerRef &p, Context &ctx, const PropValue &v) {
    ObjectRead r = readOf(p, ctx);
    return r.iface && r.iface.hasSetFlags()
               ? r.iface.setFlags(uint32_t(v.asFlags()))
               : Status::error(StatusCode::Unsupported,
                               "this object's flags can't be changed");
  };
  st.add(flagsDef(), std::move(flags));

  PointerAccessor parent;
  parent.target = StructId::of(kSceneObjectStruct);
  parent.get = [](const PointerRef &p, Context &ctx) {
    return ctx.library()
               ? blockRoot(ctx, liveBlock(*ctx.library(), readOf(p, ctx).info.parent))
               : PointerRef();
  };
  parent.set = [](const PointerRef &p, Context &ctx, const PointerRef &target) {
    ObjectRead r = readOf(p, ctx);
    if (!r.iface || !r.iface.hasSetParentId()) {
      return Status::error(StatusCode::Unsupported,
                           "this object's parent can't be changed");
    }
    if (!target.isNull() && target.owner.kind != OwnerKey::Kind::Block) {
      return notABlock();
    }
    CDCC_TRY(checkParent(p, ctx, target));
    CDCC_TRY(r.iface.setParentId(target.isNull() ? BlockId() : target.owner.block));
    ctx.library()->noteRefsChanged();
    return Status();
  };
  st.addPointer(PropertyDef::pointerProp("parent", parent.target), std::move(parent));

  PointerAccessor data;
  data.target = StructId::of(kObjectDataStruct);
  data.get = [](const PointerRef &p, Context &ctx) {
    return ctx.library()
               ? blockRoot(ctx, liveBlock(*ctx.library(), readOf(p, ctx).info.data))
               : PointerRef();
  };
  st.addPointer(PropertyDef::pointerProp("data", data.target), std::move(data));

  PointerAccessor transform;
  transform.get = [](const PointerRef &p, Context &ctx) {
    const ObjectRead r = readOf(p, ctx);
    return r.type ? PointerRef{ctx.runtime().ext<HostChannels>().put(
                                   *ctx.library(), p.owner.block, r.channels),
                               r.type->structId(),
                               OwnerKey()}
                  : PointerRef();
  };
  transform.target = StructId::of(TransformRegistry::kChannelsStruct);
  st.addPointer(PropertyDef::pointerProp("transform", transform.target),
                std::move(transform));

  ValueAccessor type;
  type.get = [](const PointerRef &p, Context &ctx) {
    const ObjectRead r = readOf(p, ctx);
    const char *known = r.iface ? ctx.runtime().nameOf(r.info.transformType) : nullptr;
    return PropValue::ofString(known ? known : "");
  };
  st.add(PropertyDef::stringProp("transformType"), std::move(type));

  ValueAccessor locks;
  locks.get = [](const PointerRef &p, Context &ctx) {
    const ObjectRead r = readOf(p, ctx);
    return PropValue::ofString(r.type ? lockNames(*r.type, r.info.locks).c_str() : "");
  };
  locks.set = [](const PointerRef &p, Context &ctx, const PropValue &v) {
    ObjectRead r = readOf(p, ctx);
    if (!r.iface || !r.iface.hasSetLocks() || !r.type) {
      return Status::error(StatusCode::Unsupported,
                           "this object's channels can't be locked");
    }
    std::string unknown;
    const uint64_t mask = lockMask(*r.type, v.asString().c_str(), &unknown);
    return unknown.empty() ? r.iface.setLocks(mask)
                           : Status::error(StatusCode::InvalidArgument,
                                           ("the transform has no channel '" + unknown +
                                            "' that can be locked")
                                               .c_str());
  };
  st.add(PropertyDef::stringProp("locks").description(
             "Names of locked channels; one component is like location.0."),
         std::move(locks));
  return st;
}

ApiStruct objectStruct()
{
  ApiStruct st(kObjectType);
  st.parent(StructId::of(kSceneObjectStruct))
      .uiName("Object")
      .description("Something placed in a scene.");

  PointerAccessor parent;
  parent.target = StructId::of(kSceneObjectStruct);
  parent.get = [](const PointerRef &p, Context &ctx) {
    return ctx.library()
               ? blockRoot(ctx, ctx.library()->resolveRef(self(p).parent.cell()))
               : PointerRef();
  };
  parent.set = [](const PointerRef &p, Context &ctx, const PointerRef &target) {
    CDCC_TRY(checkParent(p, ctx, target));
    return setCell(ctx, self(p).parent.cell(), target);
  };
  parent.cell = [](const PointerRef &p, Context &) { return &self(p).parent.cell(); };
  st.addPointer(PropertyDef::pointerProp("parent", parent.target), std::move(parent));

  PointerAccessor data;
  data.target = StructId::of(kObjectDataStruct);
  data.get = [](const PointerRef &p, Context &ctx) {
    return ctx.library() ? blockRoot(ctx, ctx.library()->resolveRef(self(p).data.cell()))
                         : PointerRef();
  };
  data.set = [](const PointerRef &p, Context &ctx, const PointerRef &target) {
    return setCell(ctx, self(p).data.cell(), target);
  };
  data.cell = [](const PointerRef &p, Context &) { return &self(p).data.cell(); };
  st.addPointer(PropertyDef::pointerProp("data", data.target), std::move(data));

  PointerAccessor transform;
  transform.get = [](const PointerRef &p, Context &ctx) {
    const TransformType *t =
        ctx.runtime().ext<TransformRegistry>().find(self(p).transformTypeId());
    self(p).resolve();
    return t ? PointerRef{&self(p).channels, t->structId(), OwnerKey()} : PointerRef();
  };
  transform.target = StructId::of(TransformRegistry::kChannelsStruct);
  st.addPointer(PropertyDef::pointerProp("transform", transform.target),
                std::move(transform));

  ValueAccessor type; /* also one that isn't registered */
  type.get = [](const PointerRef &p, Context &) {
    return PropValue::ofString(self(p).transformType().c_str());
  };
  st.add(PropertyDef::stringProp("transformType"), std::move(type));
  return st;
}

} // namespace coredcc::detail
