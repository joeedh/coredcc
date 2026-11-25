#pragma once

/* A document for the scene tests: a Runtime, a Library, a Context whose root has `scene` and
 * `evaluated`, a data block type with bounds (test.Shape), and a host-owned Transformable
 * (test.Host) whose answers the tests choose. */

#include "scene_fixture.h"

#include "coredcc/api/change_bus.h"
#include "coredcc/api/resolve.h"
#include "coredcc/lib/pin.h"
#include "coredcc/lib/provider.h"
#include "coredcc/scene/scene.h"

#include "litestl/util/alloc.h"

#include <functional>
#include <memory>
#include <string>

namespace scene_test {

namespace lbt = litestl::binding::types;
using litestl::util::Vector;
using QueryFn = std::function<QueryAnswer(const Datablock &, InterfaceId)>;

struct Shape {
  Box3d box;
  uint32_t invalidated = 0;
  std::function<void()> onBounds;

  Box3d localBounds() const
  {
    if (onBounds) {
      onBounds();
    }
    return box;
  }
  uint32_t domains() const
  {
    return 1;
  }
  void closestElements(const NearQuery &, NearSink &) const {}
  void invalidate(uint32_t kinds)
  {
    invalidated |= kinds;
  }
  static lbt::Struct<Shape> *defineBindings()
  {
    auto *st = new lbt::Struct<Shape>("test::Shape", sizeof(Shape));
    BIND_STRUCT_METHOD(st, localBounds, MARGS());
    BIND_STRUCT_METHOD(st, domains, MARGS());
    BIND_STRUCT_METHOD(st, closestElements, MARGS("query", "sink"));
    BIND_STRUCT_METHOD(st, invalidate, MARGS("kinds"));
    return st;
  }
};

/* No setParentId and no setTransformTypeId. Its ids are plain: not RefCells. */
struct Host {
  ObjectInfo info;
  ChannelBlock channels;
  std::function<void(ObjectInfo &)> onRead;
  std::function<Status()> onWrite;

  void read(ObjectInfo &out, ChannelBlock &c) const
  {
    out = info;
    c = channels;
    if (onRead) {
      onRead(out);
    }
  }
  Status writeChannels(const ChannelBlock &c)
  {
    if (onWrite) {
      const Status s = onWrite();
      if (!s.ok()) {
        return s;
      }
    }
    channels = c;
    return Status();
  }
  static lbt::Struct<Host> *defineBindings()
  {
    auto *st = new lbt::Struct<Host>("test::Host", sizeof(Host));
    BIND_STRUCT_METHOD(st, read, MARGS("info", "channels"));
    BIND_STRUCT_METHOD(st, writeChannels, MARGS("channels"));
    return st;
  }
};

/* What the clientData of a test.Host or test.Shape block is: the host moves `data` about. */
struct Slot {
  void *data = nullptr;
  void (*free)(void *) = nullptr;
};

/* Answers no other interfaces. */
struct PlainProvider {
  void *data(const Datablock &b) const
  {
    return static_cast<Slot *>(b.clientData())->data;
  }
  void foreachRef(const Datablock &, RefVisitor &) const {}
  void destroy(const Datablock &b)
  {
    Slot *s = static_cast<Slot *>(b.clientData());
    s->free(s->data);
    litestl::alloc::Delete(s);
  }
  static lbt::Struct<PlainProvider> *defineBindings()
  {
    auto *st = new lbt::Struct<PlainProvider>("test::PlainProvider", sizeof(PlainProvider));
    BIND_STRUCT_METHOD(st, data, MARGS("block"));
    BIND_STRUCT_METHOD(st, foreachRef, MARGS("block", "visitor"));
    BIND_STRUCT_METHOD(st, destroy, MARGS("block"));
    return st;
  }
};

struct SlotProvider {
  QueryFn *query = nullptr;

  void *data(const Datablock &b) const
  {
    return static_cast<Slot *>(b.clientData())->data;
  }
  void foreachRef(const Datablock &, RefVisitor &) const {}
  void destroy(const Datablock &b)
  {
    Slot *s = static_cast<Slot *>(b.clientData());
    s->free(s->data);
    litestl::alloc::Delete(s);
  }
  QueryAnswer queryInterface(const Datablock &b, InterfaceId iface) const
  {
    return *query ? (*query)(b, iface) : QueryAnswer();
  }
  static lbt::Struct<SlotProvider> *defineBindings()
  {
    auto *st = new lbt::Struct<SlotProvider>("test::SlotProvider", sizeof(SlotProvider));
    BIND_STRUCT_METHOD(st, data, MARGS("block"));
    BIND_STRUCT_METHOD(st, foreachRef, MARGS("block", "visitor"));
    BIND_STRUCT_METHOD(st, destroy, MARGS("block"));
    BIND_STRUCT_METHOD(st, queryInterface, MARGS("block", "iface"));
    return st;
  }
};

using ModelFn = std::function<TransformState(const EvalCtx &, const TransformState &)>;

/* A transform model that does what `fn` does. */
struct FnModel {
  ModelFn fn;
  TransformState evaluate(const EvalCtx &c, const TransformState &p) const
  {
    return fn(c, p);
  }
  Status setWorld(const EvalCtx &,
                  const mat4d &,
                  const TransformState &,
                  SetWorldMode,
                  ChannelBlock &) const
  {
    return Status::error(StatusCode::Unsupported, "FnModel");
  }
  static lbt::Struct<FnModel> *defineBindings()
  {
    auto *st = new lbt::Struct<FnModel>("test::FnModel", sizeof(FnModel));
    BIND_STRUCT_METHOD(st, evaluate, MARGS("ctx", "parent"));
    BIND_STRUCT_METHOD(st, setWorld, MARGS("current", "target", "parent", "mode", "out"));
    return st;
  }
};

inline void must(const Status &s)
{
  if (!s.ok()) {
    fprintf(stderr, "failed: %s\n", s.message());
    abort();
  }
}

struct Doc {
  Runtime rt;
  QueryFn query;
  std::unique_ptr<Library> lib;
  std::unique_ptr<Context> ctx;
  BlockHandle scene;
  const TransformType *trs;
  const TransformType *fbx;
  const TransformType *matrix;

  Doc()
  {
    must(registerSceneTypes(rt));
    must(registerSceneTypes(rt)); /* idempotent */
    addSlotType("test.Shape");
    addSlotType("test.Host");
    must(registerImpl<Spatial, Shape>(rt, "test.Shape"));
    must(registerImpl<Invalidation, Shape>(rt, "test.Shape"));
    must(registerImpl<Transformable, Host>(rt, "test.Host"));

    ApiRegistry &api = rt.ext<ApiRegistry>();
    ApiStruct shapeStruct("test.Shape");
    shapeStruct.parent(StructId::of(kObjectDataStruct));
    must(api.add(std::move(shapeStruct)).status());
    ApiStruct hostStruct("test.Host");
    hostStruct.parent(StructId::of(kSceneObjectStruct));
    must(api.add(std::move(hostStruct)).status());
    ApiStruct root("test.Root");
    PointerAccessor sceneMember;
    sceneMember.target = StructId::of(kSceneType);
    sceneMember.get = [](const PointerRef &, Context &c) {
      return detail::blockRoot(c, c.scene());
    };
    root.addPointer(PropertyDef::pointerProp("scene", sceneMember.target),
                    std::move(sceneMember));
    addEvaluated(root);
    must(api.add(std::move(root)).status());
    const StructId rootId = StructId::of("test.Root");
    must(api.addRuntimeOwner("root", rootId, [this]() -> void * { return this; }).status());
    must(api.validate());

    lib = std::make_unique<Library>(&rt);
    lib->setQuarantine(true);
    ctx = std::make_unique<Context>(&rt, lib.get(), api.runtimeOwner("root"));
    scene = addScene("Scene");
    ctx->setScene(scene);
    const TransformRegistry &types = rt.ext<TransformRegistry>();
    trs = types.find(kTrsTransform);
    fbx = types.find(kFbxTransform);
    matrix = types.find(kMatrixTransform);
  }
  ~Doc()
  {
    ctx.reset();
    lib.reset();
  }

  BlockHandle addScene(const char *name)
  {
    Result<BlockHandle> s = newScene(*lib, name);
    must(s.status());
    return *s;
  }
  void addSlotType(const char *name)
  {
    SlotProvider *p = litestl::alloc::New<SlotProvider>("provider");
    p->query = &query;
    addSlotType(name, p);
  }
  template <class Provider> void addSlotType(const char *name, Provider *p)
  {
    const std::string impl = std::string(name) + "#provider";
    must(registerImpl<BlockProvider, Provider>(rt, impl));
    Result<Owned<BlockProvider>> owned = own<BlockProvider>(rt, impl, p);
    must(owned.status());
    BlockTypeInfo info;
    info.name = name;
    info.client = ClientId::of(kCoredccClient);
    info.dataStruct = StructId::of(name);
    info.storage = BlockStorage::Transient;
    must(rt.ext<BlockTypeRegistry>().add(info, std::move(owned).value()).status());
  }
  template <class T> BlockHandle addSlot(const char *type, const char *name)
  {
    Slot *s = litestl::alloc::New<Slot>("Slot");
    s->data = new T();
    s->free = [](void *p) { delete static_cast<T *>(p); };
    Result<BlockHandle> h = lib->create(TypeId::of(type), name, s);
    must(h.status());
    return *h;
  }

  Scene &sc()
  {
    return *Scene::of(*lib, scene);
  }
  BlockId idOf(BlockHandle h)
  {
    return lib->get(h)->id();
  }
  Slot &slot(BlockHandle h)
  {
    return *static_cast<Slot *>(lib->get(h)->clientData());
  }
  Object &obj(BlockHandle h)
  {
    return *static_cast<Object *>(lib->get(h)->clientData());
  }
  Host &host(BlockHandle h)
  {
    return *static_cast<Host *>(slot(h).data);
  }
  Shape &shape(BlockHandle h)
  {
    return *static_cast<Shape *>(slot(h).data);
  }

  /** A TRS object at `at` that isn't in a scene. */
  BlockHandle loose(const char *name, const double3 &at = double3(0.0, 0.0, 0.0))
  {
    Result<BlockHandle> h = newObject(*lib, name, kTrsTransform);
    must(h.status());
    put(obj(*h).channels, trs::kLocation, at);
    return *h;
  }
  /** One that is in the scene. */
  BlockHandle object(const char *name, double x = 0.0, double y = 0.0, double z = 0.0)
  {
    const BlockHandle h = loose(name, double3(x, y, z));
    must(sc().add(*lib, h));
    return h;
  }
  BlockHandle hostObject(const char *name, double x = 0.0)
  {
    const BlockHandle h = addSlot<Host>("test.Host", name);
    host(h).info.transformType = TypeId::of(kTrsTransform);
    host(h).channels = trs->defaults();
    host(h).channels.set(trs::kLocation, x);
    must(sc().add(*lib, h));
    return h;
  }
  BlockHandle shapeBlock(const char *name, double size)
  {
    const BlockHandle h = addSlot<Shape>("test.Shape", name);
    for (int i = 0; i < 3; i++) {
      shape(h).box.min[i] = -size;
      shape(h).box.max[i] = size;
    }
    return h;
  }
  void setParent(BlockHandle h, BlockHandle parent)
  {
    must(lib->writeRef(obj(h).parent.cell(), parent ? idOf(parent) : BlockId()));
  }
  void setData(BlockHandle h, BlockHandle data)
  {
    must(lib->writeRef(obj(h).data.cell(), data ? idOf(data) : BlockId()));
  }
  void evaluate()
  {
    must(evaluateScene(*ctx));
  }
  ObjectEval result(BlockHandle h)
  {
    ObjectEval e;
    const bool found = sc().evaluated(h, e);
    test_assert(found);
    return e;
  }
  EvalStatus statusOf(BlockHandle h)
  {
    return result(h).status;
  }
  bool isAt(BlockHandle h, double x, double y = 0.0, double z = 0.0)
  {
    const double3 p = solve::translationOf(result(h).state.world);
    if (p[0] == x && p[1] == y && p[2] == z) {
      return true;
    }
    fprintf(stderr, "at %g %g %g, not %g %g %g\n", p[0], p[1], p[2], x, y, z);
    return false;
  }
  void addFnModel(const char *name, ModelFn fn)
  {
    must(registerImpl<TransformModel, FnModel>(rt, name));
    FnModel *m = litestl::alloc::New<FnModel>("FnModel");
    m->fn = std::move(fn);
    Result<Owned<TransformModel>> owned = own<TransformModel>(rt, name, m);
    must(owned.status());
    TransformTypeInfo info;
    info.name = name;
    info.channels.append(PropertyDef::floatProp("x"));
    must(rt.ext<TransformRegistry>().add(std::move(info), std::move(owned).value()).status());
  }
};

inline Status boom(const char *what = "boom")
{
  return Status::error(StatusCode::Internal, what);
}

inline bool isError(const Status &s, StatusCode code, const char *text = "")
{
  if (s.code() == code && strstr(s.message(), text)) {
    return true;
  }
  fprintf(stderr, "status %d: %s\n", int(s.code()), s.message());
  return false;
}

} // namespace scene_test
