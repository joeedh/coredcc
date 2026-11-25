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
  return lib.runtime().hasStickyError()
             ? failed(std::string(what) + ": an error is pending")
             : Status();
}

} // namespace

/* ---- Object ---- */

/* Six cache lines. Evaluating goes through all objects: what most of them do not use
 * belongs in FileData, or in another place. */
static_assert(sizeof(Object) <= 384);

void Object::setTransformType(std::string_view name)
{
  if (TypeId::of(name) != type_) {
    locks_ = 0;
  }
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
  if (file_ && file_->pending) {
    const_cast<Object *>(this)->resolve(); /* the type came after the object */
  }
  out = channels;
  info.locks = locks_;
}

Status Object::writeChannels(const ChannelBlock &in)
{
  /* No channels are what read() gave of an object that waits for its type: an undo. */
  if (fileOfType() && file_->pending && in.size() > 0) {
    file_->pending = false;
    file_->unread.clear();
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

Status Object::setFlags(uint32_t f)
{
  flags = ObjectFlags(f);
  return Status();
}

Status Object::setLocks(uint64_t locks)
{
  locks_ = locks;
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
    BIND_STRUCT_METHOD(st, setFlags, MARGS("flags"));
    BIND_STRUCT_METHOD(st, setLocks, MARGS("locks"));
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
    auto *st = new lbt::Struct<ObjectProvider>("coredcc::ObjectProvider",
                                               sizeof(ObjectProvider));
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

Result<BlockHandle>
newObject(Library &lib, std::string_view name, std::string_view transformType)
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
                          const EvalCtx &ctx,
                          const TransformState &parent,
                          TransformState &out,
                          Status &error)
{
  EvalStatus status = EvalStatus::MissingModel;
  if (type) {
    status = EvalStatus::Invalid;
    if (ctx.channelCount() == type->channelCount()) {
      out = type->model().evaluate(ctx, parent);
      if (rt.hasStickyError()) {
        Status s = rt.takeStickyError();
        if (error.ok()) {
          error = std::move(s);
        }
        status = EvalStatus::ModelFailed;
      } else if (out.finite()) {
        return EvalStatus::Ok;
      }
    }
  }
  out = parent;
  return status;
}

BlockHandle detail::liveBlock(const Library &lib, const BlockId &id)
{
  const BlockHandle h = id.isNull() ? BlockHandle() : lib.find(id);
  const Datablock *b = lib.get(h);
  return b && b->state() == BlockState::Live ? h : BlockHandle();
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
    return failed(b ? "'" + std::string(b->name().c_str()) + "' isn't " +
                          (b->state() == BlockState::Live ? "Transformable" : "Live")
                    : "there is no such block");
  }
  r.iface.read(r.info, r.channels);
  if (Status s = thrown(lib.runtime()); !s.ok()) {
    return s;
  }
  r.type = lib.runtime().ext<TransformRegistry>().find(r.info.transformType);
  r.owner = OwnerKey::ofBlock(lib.get(object)->id());
  return r;
}

Result<ObjectRead> readObjectAt(const Library &lib, BlockHandle object, double time)
{
  Result<ObjectRead> r = readObject(lib, object);
  if (!r.ok()) {
    return r;
  }
  r->time = time;
  if (Status s = detail::overridesOf(lib, object, r->type, time, r->overrides); !s.ok()) {
    return s;
  }
  return r;
}

namespace {

/* The Live parent of an object, or null. A foreign slot's error stays sticky. */
BlockHandle parentOf(const Library &lib, BlockHandle h)
{
  const Transformable t = lib.query<Transformable>(h);
  ObjectInfo info;
  if (t) {
    ChannelBlock channels;
    t.read(info, channels);
  }
  return detail::liveBlock(lib, info.parent);
}

/* The objects from `h` up to the root, or to the first one that comes again. One that
 * isn't Transformable is the last, and isn't there: the identity. */
Status walkUp(const Library &lib, BlockHandle h, double time, Vector<ObjectRead> &steps)
{
  litestl::util::Map<BlockHandle, int> seen;
  while (h) {
    if (const int *first = seen.lookup_ptr(h)) {
      /* A cycle: its members have no parent, so what is above its first one doesn't
       * count. */
      while (int(steps.size()) > *first + 1) {
        steps.pop_back();
      }
      return Status();
    }
    seen.add(h, int(steps.size()));
    const bool transformable = bool(lib.query<Transformable>(h));
    CDCC_TRY(thrown(lib.runtime()));
    if (!transformable) {
      return Status();
    }
    Result<ObjectRead> r = readObjectAt(lib, h, time);
    if (!r.ok()) {
      return r.status();
    }
    h = detail::liveBlock(lib, r->info.parent);
    steps.append(std::move(*r));
  }
  return Status();
}

Result<TransformState> stateFrom(const Library &lib, BlockHandle h, double time)
{
  Vector<ObjectRead> steps;
  if (Status s = walkUp(lib, h, time, steps); !s.ok()) {
    return s;
  }
  TransformState state;
  for (int i = int(steps.size()) - 1; i >= 0; i--) {
    const TransformState parent = state;
    Status error;
    evaluateObject(lib.runtime(), steps[i].type, steps[i].ctx(), parent, state, error);
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

Result<TransformState>
computeParentState(const Library &lib, BlockHandle object, double time)
{
  if (Status s = ready(lib, "computeParentState"); !s.ok()) {
    return s;
  }
  Result<ObjectRead> r = readObject(lib, object);
  if (!r.ok()) {
    return r.status();
  }
  const BlockHandle parent = detail::liveBlock(lib, r->info.parent);
  /* On a cycle an object has no parent. */
  if (!parent || isSelfOrAncestor(lib, object, parent)) {
    return TransformState();
  }
  return stateFrom(lib, parent, time);
}

Result<double3> computePivot(const Library &lib, BlockHandle object, double time)
{
  if (Status s = ready(lib, "computePivot"); !s.ok()) {
    return s;
  }
  Result<TransformState> parent = computeParentState(lib, object, time);
  if (!parent.ok()) {
    return parent.status();
  }
  Result<ObjectRead> r = readObjectAt(lib, object, time);
  if (!r.ok()) {
    return r.status();
  }
  if (!r->type) {
    return failed("computePivot: the object's transform type isn't registered");
  }
  return transformPivot(*r->type, r->ctx(), *parent);
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
  Result<ObjectRead> r = readObjectAt(lib, object, time);
  if (!r.ok()) {
    return r.status();
  }
  if (!r->type) {
    return failed("setWorldOf: the object's transform type isn't registered");
  }
  return solveWorld(*r->type, r->solveCtx(), target, *parent, mode);
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
    return Status::error(StatusCode::Unsupported,
                         "this object's transform type can't be changed");
  }
  const Status s = r->iface.setTransformTypeId(type);
  CDCC_TRY(thrown(ctx.runtime()));
  CDCC_TRY(s);
  ctx.library()->noteRefsChanged(); /* those of Object::unread */
  return writeTransform(ctx, object, channels);
}

Status writeFlags(Context &ctx, BlockHandle object, ObjectFlags flags)
{
  CDCC_TRY(writable(ctx));
  Result<ObjectRead> r = readObject(*ctx.library(), object);
  if (!r.ok()) {
    return r.status();
  }
  if (!r->iface.hasSetFlags()) {
    return Status::error(StatusCode::Unsupported, "this object's flags can't be changed");
  }
  const Status s = r->iface.setFlags(uint32_t(flags));
  CDCC_TRY(thrown(ctx.runtime()));
  CDCC_TRY(s);
  ctx.runtime().ext<ChangeBus>().notify(r->owner, "flags");
  return Status();
}

Status writeLocks(Context &ctx, BlockHandle object, uint64_t locks)
{
  CDCC_TRY(writable(ctx));
  Result<ObjectRead> r = readObject(*ctx.library(), object);
  if (!r.ok()) {
    return r.status();
  }
  if (!r->iface.hasSetLocks()) {
    return Status::error(StatusCode::Unsupported,
                         "this object's channels can't be locked");
  }
  const Status s = r->iface.setLocks(locks);
  CDCC_TRY(thrown(ctx.runtime()));
  CDCC_TRY(s);
  ctx.runtime().ext<ChangeBus>().notify(r->owner, "locks");
  return Status();
}

Status writeParent(Context &ctx, BlockHandle object, const BlockId &parent)
{
  CDCC_TRY(writable(ctx));
  Result<ObjectRead> r = readObject(*ctx.library(), object);
  if (!r.ok()) {
    return r.status();
  }
  if (!r->iface.hasSetParentId()) {
    return Status::error(StatusCode::Unsupported,
                         "this object's parent can't be changed");
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

namespace detail {

namespace {

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
  CDCC_TRY(api.add(sceneObjectStruct()).status());
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
