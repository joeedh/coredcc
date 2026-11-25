#include "coredcc/scene/object.h"

#include "coredcc/api/change_bus.h"
#include "coredcc/lib/provider.h"
#include "coredcc/scene/builtins.h"

#include "litestl/binding/binding.h"
#include "litestl/util/alloc.h"
#include "litestl/util/map.h"

#include <string>

namespace coredcc {

namespace lbt = litestl::binding::types;
using litestl::util::Vector;

namespace {

Status failed(const std::string &what)
{
  return Status::error(StatusCode::FailedPrecondition, what.c_str());
}

/* An exception of a foreign slot, right after the call. */
Status thrown(Runtime &rt)
{
  return rt.hasStickyError() ? rt.takeStickyError() : Status();
}

Status ready(const Library &lib, const char *what)
{
  CDCC_TRY(lib.runtime().busyIfEvaluating(what));
  return lib.runtime().hasStickyError() ? failed(std::string(what) + ": an error is pending") :
                                          Status();
}

} // namespace

/* ---- Object ---- */

void Object::setTransformType(std::string_view name)
{
  typeName_ = litestl::util::string(std::string(name).c_str());
  type_ = TypeId::of(name);
  /* For setTransformTypeId. Two names with one hash: the first one wins. */
  rt_->registerId<IdDomain::Type>(std::string(name)).status().ignore();
}

void Object::read(ObjectInfo &info, ChannelBlock &out) const
{
  info.transformType = type_;
  info.parent = parent.target();
  info.data = data.target();
  info.flags = uint32_t(flags);
  if (pending_) {
    const_cast<Object *>(this)->resolve(); /* the type came after the object */
  }
  out = channels;
}

Status Object::writeChannels(const ChannelBlock &in)
{
  /* No channels are what read() gave of an object that waits for its type: an undo. */
  if (pending_ && unreadType_ == typeName_ && in.size() > 0) {
    pending_ = false;
    unread_.clear();
  }
  channels = in;
  return Status();
}

Status Object::setParentId(const BlockId &id)
{
  parent.cell().target = id;
  return Status();
}

Status Object::setTransformTypeId(const TypeId &type)
{
  const char *name = rt_->nameOf(type);
  if (!name) {
    return Status::error(StatusCode::NotFound, "setTransformTypeId: no type has this id");
  }
  setTransformType(name);
  return Status();
}

lbt::Struct<Object> *Object::defineBindings()
{
  static lbt::Struct<Object> *st = nullptr;
  if (!st) {
    litestl::alloc::PermanentGuard guard;
    st = new lbt::Struct<Object>("coredcc::Object", sizeof(Object));
    BIND_STRUCT_METHOD(st, read, MARGS("info", "channels"));
    BIND_STRUCT_METHOD(st, writeChannels, MARGS("channels"));
    BIND_STRUCT_METHOD(st, setParentId, MARGS("parent"));
    BIND_STRUCT_METHOD(st, setTransformTypeId, MARGS("type"));
  }
  return st;
}

namespace {

constexpr uint32_t kObjectPayload = 1;

struct ObjectProvider {
  void *data(const Datablock &b) const
  {
    return b.clientData();
  }
  void foreachRef(const Datablock &b, RefVisitor &v) const
  {
    if (Object *o = static_cast<Object *>(b.clientData())) {
      o->foreachCell([&v](RefCell &cell) { v.visit(cell); });
    }
  }
  void destroy(const Datablock &b)
  {
    litestl::alloc::Delete(static_cast<Object *>(b.clientData()));
  }
  Status serialize(const Datablock &b, BlockWriter &out) const
  {
    if (!b.clientData()) {
      return failed("an object without data");
    }
    static_cast<Object *>(b.clientData())->write(out);
    return Status(); /* or the writer's */
  }
  Status deserialize(const Datablock &, BlockReader &in)
  {
    Object *o = litestl::alloc::New<Object>("coredcc::Object", &in.library().runtime());
    o->readFrom(in);
    in.setData(o);
    return Status(); /* or the reader's: `destroy` gets it back */
  }
  Status duplicate(const Datablock &src, BlockCopy &out)
  {
    if (!src.clientData()) {
      return failed("an object without data");
    }
    out.setData(litestl::alloc::New<Object>("coredcc::Object",
                                            *static_cast<Object *>(src.clientData())));
    return Status();
  }
  uint64_t memSize(const Datablock &b) const
  {
    const Object *o = static_cast<Object *>(b.clientData());
    return o ? o->memSize() : 0;
  }
  static lbt::Struct<ObjectProvider> *defineBindings()
  {
    auto *st = new lbt::Struct<ObjectProvider>("coredcc::ObjectProvider", sizeof(ObjectProvider));
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

Result<BlockHandle> newObject(Library &lib, std::string_view name, std::string_view transformType)
{
  Runtime &rt = lib.runtime();
  const TransformType *type = rt.ext<TransformRegistry>().find(transformType);
  if (!type) {
    return Status::error(
        StatusCode::NotFound,
        ("no transform type is named '" + std::string(transformType) + "'").c_str());
  }
  Object *o = litestl::alloc::New<Object>("coredcc::Object", &rt);
  o->setTransformType(transformType);
  o->channels = type->defaults();
  Result<BlockHandle> h = lib.create(TypeId::of(kObjectType), name, o);
  if (!h.ok()) {
    litestl::alloc::Delete(o);
  }
  return h;
}

/* ---- evaluating one object ---- */

EvalStatus evaluateObject(Runtime &rt,
                          const TransformType *type,
                          const ChannelBlock &channels,
                          double time,
                          const TransformState &parent,
                          TransformState &out,
                          Status &error)
{
  EvalStatus status = EvalStatus::MissingModel;
  if (type) {
    status = EvalStatus::Invalid;
    if (channels.size() == type->channelCount()) {
      out = type->model().evaluate(EvalCtx(channels, time), parent);
      if (rt.hasStickyError()) {
        Status s = rt.takeStickyError();
        if (error.ok()) {
          error = std::move(s);
        }
        status = EvalStatus::ModelFailed;
      }
      else if (out.finite()) {
        return EvalStatus::Ok;
      }
    }
  }
  out = parent;
  return status;
}

Result<ObjectRead> readObject(const Library &lib, BlockHandle object)
{
  if (lib.runtime().hasStickyError()) {
    return failed("readObject: an error is pending");
  }
  ObjectRead r;
  r.iface = lib.query<Transformable>(object);
  if (Status s = thrown(lib.runtime()); !s.ok()) {
    return s;
  }
  if (!r.iface) {
    const Datablock *b = lib.get(object);
    return failed("'" + std::string(b ? b->name().c_str() : "?") + "' isn't a Live object");
  }
  r.iface.read(r.info, r.channels);
  if (Status s = thrown(lib.runtime()); !s.ok()) {
    return s;
  }
  r.type = lib.runtime().ext<TransformRegistry>().find(r.info.transformType);
  r.owner = OwnerKey::ofBlock(lib.get(object)->id());
  return r;
}

namespace {

/* The Live block an id names, or null. */
BlockHandle liveBlock(const Library &lib, const BlockId &id)
{
  const BlockHandle h = id.isNull() ? BlockHandle() : lib.find(id);
  const Datablock *b = lib.get(h);
  return b && b->state() == BlockState::Live ? h : BlockHandle();
}

/* The Live parent of an object, or null. A foreign slot's error stays sticky. */
BlockHandle parentOf(const Library &lib, BlockHandle h)
{
  const Transformable t = lib.query<Transformable>(h);
  ObjectInfo info;
  if (t) {
    ChannelBlock channels;
    t.read(info, channels);
  }
  return liveBlock(lib, info.parent);
}

/* One object on the way up. */
struct Step {
  bool transformable = false;
  const TransformType *type = nullptr;
  ChannelBlock channels;
};

/* The objects from `h` up to the root, or to the first one that comes again. */
Status walkUp(const Library &lib, BlockHandle h, Vector<Step> &steps)
{
  litestl::util::Map<BlockHandle, int> seen;
  while (h) {
    if (const int *first = seen.lookup_ptr(h)) {
      /* A cycle: its members have no parent, so what is above its first one doesn't count. */
      while (int(steps.size()) > *first + 1) {
        steps.pop_back();
      }
      return Status();
    }
    seen.add(h, int(steps.size()));
    Step step;
    BlockId parent;
    const Transformable t = lib.query<Transformable>(h);
    if (t) {
      ObjectInfo info;
      t.read(info, step.channels);
      step.transformable = true;
      step.type = lib.runtime().ext<TransformRegistry>().find(info.transformType);
      parent = info.parent;
    }
    CDCC_TRY(thrown(lib.runtime()));
    steps.append(std::move(step));
    h = liveBlock(lib, parent);
  }
  return Status();
}

Result<TransformState> stateFrom(const Library &lib, BlockHandle h, double time)
{
  Vector<Step> steps;
  if (Status s = walkUp(lib, h, steps); !s.ok()) {
    return s;
  }
  TransformState state;
  for (int i = int(steps.size()) - 1; i >= 0; i--) {
    if (!steps[i].transformable) {
      continue; /* the last one: the identity */
    }
    const TransformState parent = state;
    Status error;
    evaluateObject(
        lib.runtime(), steps[i].type, steps[i].channels, time, parent, state, error);
    if (!error.ok()) {
      return error;
    }
  }
  return state;
}

} // namespace

Result<TransformState> computeState(const Library &lib, BlockHandle object, double time)
{
  if (Status s = ready(lib, "computeState"); !s.ok()) {
    return s;
  }
  Result<ObjectRead> r = readObject(lib, object);
  if (!r.ok()) {
    return r.status();
  }
  return stateFrom(lib, object, time);
}

Result<TransformState> computeParentState(const Library &lib, BlockHandle object, double time)
{
  if (Status s = ready(lib, "computeParentState"); !s.ok()) {
    return s;
  }
  Result<ObjectRead> r = readObject(lib, object);
  if (!r.ok()) {
    return r.status();
  }
  const BlockHandle parent = liveBlock(lib, r->info.parent);
  /* On a cycle an object has no parent. */
  if (!parent || isSelfOrAncestor(lib, object, parent)) {
    return TransformState();
  }
  return stateFrom(lib, parent, time);
}

Result<ChannelBlock> setWorldOf(const Library &lib,
                                BlockHandle object,
                                const mat4d &target,
                                SetWorldMode mode,
                                double time)
{
  Result<TransformState> parent = computeParentState(lib, object, time);
  if (!parent.ok()) {
    return parent.status();
  }
  Result<ObjectRead> r = readObject(lib, object);
  if (!r.ok()) {
    return r.status();
  }
  if (!r->type) {
    return failed("setWorldOf: the object's transform type isn't registered");
  }
  return solveWorld(*r->type, EvalCtx(r->channels, time), target, *parent, mode);
}

bool isSelfOrAncestor(const Library &lib, BlockHandle ancestor, BlockHandle object)
{
  litestl::util::Map<BlockHandle, int> seen;
  while (object && !seen.lookup_ptr(object)) {
    if (object == ancestor) {
      return true;
    }
    seen.add(object, 0);
    object = parentOf(lib, object);
  }
  return false;
}

/* ---- writing ---- */

namespace {

Status writable(Context &ctx)
{
  if (!ctx.library()) {
    return failed("the Context has no Library");
  }
  return ready(*ctx.library(), "writing an object");
}

} // namespace

Status writeTransform(Context &ctx, BlockHandle object, const ChannelBlock &channels)
{
  CDCC_TRY(writable(ctx));
  Result<ObjectRead> r = readObject(*ctx.library(), object);
  if (!r.ok()) {
    return r.status();
  }
  const Status s = r->iface.writeChannels(channels);
  CDCC_TRY(thrown(ctx.runtime()));
  CDCC_TRY(s);
  ctx.runtime().ext<ChangeBus>().notify(r->owner, "transform");
  return Status();
}

Status writeTransformType(Context &ctx,
                          BlockHandle object,
                          TypeId type,
                          const ChannelBlock &channels)
{
  CDCC_TRY(writable(ctx));
  Result<ObjectRead> r = readObject(*ctx.library(), object);
  if (!r.ok()) {
    return r.status();
  }
  if (!r->iface.hasSetTransformTypeId()) {
    return Status::error(StatusCode::Unsupported, "this object's transform type can't be changed");
  }
  const Status s = r->iface.setTransformTypeId(type);
  CDCC_TRY(thrown(ctx.runtime()));
  CDCC_TRY(s);
  ctx.library()->noteRefsChanged(); /* those of Object::unread */
  return writeTransform(ctx, object, channels);
}

Status writeParent(Context &ctx, BlockHandle object, const BlockId &parent)
{
  CDCC_TRY(writable(ctx));
  Result<ObjectRead> r = readObject(*ctx.library(), object);
  if (!r.ok()) {
    return r.status();
  }
  if (!r->iface.hasSetParentId()) {
    return Status::error(StatusCode::Unsupported, "this object's parent can't be changed");
  }
  const Status s = r->iface.setParentId(parent);
  CDCC_TRY(thrown(ctx.runtime()));
  CDCC_TRY(s);
  ctx.library()->noteRefsChanged();
  ctx.runtime().ext<ChangeBus>().notify(r->owner, "parent");
  return Status();
}

Status invalidateData(Context &ctx, BlockHandle block, InvalidateKinds kinds)
{
  Library *lib = ctx.library();
  const Datablock *b = lib ? lib->get(block) : nullptr;
  if (!b) {
    return Status::error(StatusCode::NotFound, "invalidateData: no such block");
  }
  const OwnerKey owner = OwnerKey::ofBlock(b->id());
  Status s;
  if (Invalidation i = lib->query<Invalidation>(block)) {
    const bool pending = ctx.runtime().hasStickyError();
    i.invalidate(uint32_t(kinds));
    s = pending ? Status() : thrown(ctx.runtime());
  }
  ctx.runtime().ext<ChangeBus>().notify(owner, {}, ChangeAspects(kinds));
  return s;
}

/* ---- the datapath ---- */

namespace detail {

PointerRef blockRoot(Context &ctx, BlockHandle h)
{
  const Library *lib = ctx.library();
  const Datablock *b = lib ? lib->get(h) : nullptr;
  if (!b || b->state() != BlockState::Live) {
    return PointerRef();
  }
  const BlockTypeRegistry::Entry *e = ctx.runtime().ext<BlockTypeRegistry>().find(b->type());
  void *data = lib->data(h);
  return e && data ? PointerRef{data, e->info.dataStruct, OwnerKey::ofBlock(b->id())} :
                     PointerRef();
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

ApiStruct objectStruct()
{
  ApiStruct st(kObjectType);
  st.parent(StructId::of(kSceneObjectStruct)).uiName("Object").description("Something placed in a scene.");

  ValueAccessor name;
  name.get = [](const PointerRef &p, Context &ctx) {
    const Library *lib = ctx.library();
    const Datablock *b = lib ? lib->get(lib->find(p.owner.block)) : nullptr;
    return PropValue::ofString(b ? b->name().c_str() : "");
  };
  st.add(PropertyDef::stringProp("name"), std::move(name));

  ValueAccessor flags;
  flags.get = [](const PointerRef &p, Context &) {
    return PropValue::ofFlags(uint32_t(self(p).flags));
  };
  flags.set = [](const PointerRef &p, Context &, const PropValue &v) {
    self(p).flags = ObjectFlags(uint32_t(v.asFlags()));
    return Status();
  };
  st.add(PropertyDef::flagsProp("flags")
             .item("HIDDEN", int64_t(ObjectFlags::Hidden))
             .item("UNSELECTABLE", int64_t(ObjectFlags::Unselectable)),
         std::move(flags));

  PointerAccessor parent;
  parent.target = StructId::of(kObjectType);
  parent.candidates.append(StructId::of(kSceneObjectStruct));
  parent.get = [](const PointerRef &p, Context &ctx) {
    return ctx.library() ? blockRoot(ctx, ctx.library()->resolveRef(self(p).parent.cell())) :
                           PointerRef();
  };
  parent.set = [](const PointerRef &p, Context &ctx, const PointerRef &target) {
    Library *lib = ctx.library();
    if (!target.isNull() && target.owner.kind == OwnerKey::Kind::Block &&
        isSelfOrAncestor(*lib, lib->find(p.owner.block), lib->find(target.owner.block)))
    {
      return Status::error(StatusCode::FailedPrecondition, "an object can't be its own ancestor");
    }
    return setCell(ctx, self(p).parent.cell(), target);
  };
  parent.cell = [](const PointerRef &p, Context &) { return &self(p).parent.cell(); };
  st.addPointer(PropertyDef::pointerProp("parent", parent.target), std::move(parent));

  PointerAccessor data;
  data.target = StructId::of(kObjectDataStruct);
  data.get = [](const PointerRef &p, Context &ctx) {
    return ctx.library() ? blockRoot(ctx, ctx.library()->resolveRef(self(p).data.cell())) :
                           PointerRef();
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
  st.addPointer(PropertyDef::pointerProp("transform", transform.target), std::move(transform));

  ValueAccessor type;
  type.get = [](const PointerRef &p, Context &) {
    return PropValue::ofString(self(p).transformType().c_str());
  };
  st.add(PropertyDef::stringProp("transformType"), std::move(type));
  return st;
}

Status addObjectBlock(Runtime &rt)
{
  const std::string impl = std::string(kObjectType) + "#provider";
  CDCC_TRY((registerImpl<BlockProvider, ObjectProvider>(rt, impl)));
  CDCC_TRY((registerImpl<Transformable, Object>(rt, kObjectType)));
  Result<Owned<BlockProvider>> provider = own<BlockProvider>(
      rt, impl, litestl::alloc::New<ObjectProvider>("coredcc::ObjectProvider"));
  if (!provider.ok()) {
    return provider.status();
  }
  BlockTypeInfo info;
  info.name = kObjectType;
  info.client = ClientId::of(kCoredccClient);
  info.dataStruct = StructId::of(kObjectType);
  info.payloadVersion = kObjectPayload;
  info.storage = BlockStorage::Payload;
  return rt.ext<BlockTypeRegistry>().add(std::move(info), std::move(*provider)).status();
}

} // namespace

Status registerObjectType(Runtime &rt)
{
  CDCC_TRY(registerInterface<Spatial>(rt));
  CDCC_TRY(registerInterface<Invalidation>(rt));
  CDCC_TRY(registerInterface<ElementAccess>(rt));
  ApiRegistry &api = rt.ext<ApiRegistry>();
  ApiStruct dataStruct(kObjectDataStruct);
  dataStruct.uiName("Object Data");
  CDCC_TRY(api.add(std::move(dataStruct)).status());
  ApiStruct sceneObject(kSceneObjectStruct);
  sceneObject.uiName("Scene Object");
  CDCC_TRY(api.add(std::move(sceneObject)).status());
  CDCC_TRY(api.add(objectStruct()).status());
  return addObjectBlock(rt);
}

} // namespace detail

Status registerSceneTypes(Runtime &rt)
{
  CDCC_TRY(registerTransformTypes(rt));
  if (detail::evaluatedRegistered(rt)) {
    return Status();
  }
  CDCC_TRY(detail::registerObjectType(rt));
  CDCC_TRY(detail::registerSceneType(rt));
  return detail::registerEvaluated(rt);
}

} // namespace coredcc
