#pragma once

/* A small scene for the datapath tests. Scene (runtime owner "scene") has `objects`, a
 * collection of Object blocks with an active item, and `tool`, a pointer to the Tool
 * runtime owner. Each Object points at a Mesh block through `data`. */

#include "coredcc/api/change_bus.h"
#include "coredcc/api/resolve.h"
#include "coredcc/api/struct.h"
#include "coredcc/base/iface/registry.h"
#include "coredcc/lib/library.h"

#include "test_util.h"

#include <cstdio>
#include <cstring>

namespace fixture {

using namespace coredcc;
namespace lbt = litestl::binding::types;

struct Mesh {
  double radius = 1;
};

struct Object {
  RefCell mesh{RefKind::OptStrong};
  float location[3] = {0, 0, 0};
  int mode = 0;
  uint32_t flags = 0;
};

struct Tool {
  double strength = 0.5;
  double free = 0;
  int version = 3;
  int count = 4;
  int pick = 0;
};

struct Scene {
  litestl::util::Vector<BlockHandle> objects;
  int active = 0;
};

template <class T> struct Provider {
  void *data(const Datablock &b) const
  {
    return b.clientData();
  }
  void foreachRef(const Datablock &b, RefVisitor &v) const
  {
    if constexpr (std::is_same_v<T, Object>) {
      if (Object *o = static_cast<Object *>(b.clientData())) {
        v.visit(o->mesh);
      }
    }
  }
  void destroy(const Datablock &b)
  {
    litestl::alloc::Delete(static_cast<T *>(b.clientData()));
  }
  static lbt::Struct<Provider> *defineBindings()
  {
    auto *st = new lbt::Struct<Provider>(
        std::is_same_v<T, Object> ? "test::ObjectProvider" : "test::MeshProvider",
        sizeof(Provider));
    BIND_STRUCT_METHOD(st, data, MARGS("block"));
    BIND_STRUCT_METHOD(st, foreachRef, MARGS("block", "visitor"));
    BIND_STRUCT_METHOD(st, destroy, MARGS("block"));
    return st;
  }
};

inline const TypeId kObjectType = TypeId::of("test.Object");
inline const TypeId kMeshType = TypeId::of("test.Mesh");
inline const StructId kObject = StructId::of("Object");
inline const StructId kMesh = StructId::of("Mesh");
inline const StructId kTool = StructId::of("Tool");
inline const StructId kScene = StructId::of("Scene");

template <class T> void addBlockType(Runtime &rt, const char *name, StructId st)
{
  const std::string impl = std::string(name) + "#provider";
  test_assert((registerImpl<BlockProvider, Provider<T>>(rt, impl).ok()));
  Result<Owned<BlockProvider>> p =
      own<BlockProvider>(rt, impl, litestl::alloc::New<Provider<T>>("provider"));
  test_assert(p.ok());
  BlockTypeInfo info;
  info.name = name;
  info.client = ClientId::of(kCoredccClient);
  info.dataStruct = st;
  test_assert(rt.ext<BlockTypeRegistry>().add(info, std::move(p).value()).ok());
}

inline PointerRef blockRef(Library &lib, BlockHandle h, StructId type)
{
  const Datablock *b = lib.get(h);
  return b ? PointerRef{lib.data(h), type, OwnerKey::ofBlock(b->id())} : PointerRef();
}

/* Counts calls to Mesh.radius's update hook. */
inline int g_radiusUpdates = 0;

inline void registerStructs(Runtime &rt, Scene &scene, Tool &tool)
{
  ApiRegistry &reg = rt.ext<ApiRegistry>();

  ApiStruct mesh("Mesh");
  mesh.addField(PropertyDef::floatProp("radius").range(0, 10), &Mesh::radius)
      .update([](const PointerRef &, Context &) { g_radiusUpdates++; });
  test_assert(reg.add(std::move(mesh)).ok());

  ApiStruct object("Object");
  object.add(PropertyDef::stringProp("name"),
             ValueAccessor{[](const PointerRef &self, Context &ctx) {
                             Library &lib = *ctx.library();
                             return PropValue::ofString(
                                 lib.get(lib.find(self.owner.block))->name().c_str());
                           },
                           {}});
  object.addField(PropertyDef::vecProp("location", 3), &Object::location);
  object.addField(PropertyDef::enumProp("mode").item("OBJECT", 0).item("EDIT", 1), &Object::mode);
  object.addField(PropertyDef::flagsProp("flags").item("SELECT", 1).item("HIDE", 2),
                  &Object::flags);
  PointerAccessor data;
  data.get = [](const PointerRef &self, Context &ctx) {
    Library &lib = *ctx.library();
    return blockRef(lib, lib.resolve(static_cast<Object *>(self.data)->mesh), StructId());
  };
  data.set = [](const PointerRef &self, Context &ctx, const PointerRef &target) {
    return ctx.library()->writeRef(static_cast<Object *>(self.data)->mesh, target.owner.block);
  };
  object.addPointer(PropertyDef::pointerProp("data", kMesh), std::move(data));
  test_assert(reg.add(std::move(object)).ok());

  ApiStruct t("Tool");
  t.addField(PropertyDef::floatProp("strength").range(0, 1), &Tool::strength);
  t.addField(PropertyDef::floatProp("free"), &Tool::free);
  t.addField(PropertyDef::intProp("version").access(PropAccess::ReadOnly), &Tool::version);
  t.addField(PropertyDef::intProp("count"), &Tool::count);
  t.addField(PropertyDef::intProp("pick"), &Tool::pick)
      .dynamic([](PropertyDef &def, const PointerRef &self, Context &) {
        def.range(0, static_cast<Tool *>(self.data)->count - 1);
      });
  test_assert(reg.add(std::move(t)).ok());
  Result<OwnerKey> toolOwner = reg.addRuntimeOwner("tool", kTool, [&tool]() { return (void *)&tool; });
  test_assert(toolOwner.ok());

  ApiStruct s("Scene");
  CollectionAccessor objects;
  objects.length = [](const PointerRef &self, Context &) {
    return int64_t(static_cast<Scene *>(self.data)->objects.size());
  };
  objects.at = [](const PointerRef &self, Context &ctx, int64_t i) {
    return blockRef(*ctx.library(), static_cast<Scene *>(self.data)->objects[int(i)], kObject);
  };
  objects.lookup = [](const PointerRef &self, Context &ctx, const KeySegment &key) {
    Library &lib = *ctx.library();
    for (BlockHandle h : static_cast<Scene *>(self.data)->objects) {
      const Datablock *b = lib.get(h);
      if (key.kind == KeySegment::Kind::StringKey && b &&
          strcmp(b->name().c_str(), key.text.c_str()) == 0)
      {
        return blockRef(lib, h, kObject);
      }
    }
    return PointerRef();
  };
  objects.keyOf = [](const PointerRef &, Context &ctx, const PointerRef &item) {
    Library &lib = *ctx.library();
    return KeySegment::stringKey(lib.get(lib.find(item.owner.block))->name().c_str());
  };
  objects.active = [](const PointerRef &self, Context &ctx) {
    Scene *sc = static_cast<Scene *>(self.data);
    if (sc->active < 0 || sc->active >= int(sc->objects.size())) {
      return PointerRef();
    }
    return blockRef(*ctx.library(), sc->objects[sc->active], kObject);
  };
  s.addCollection(PropertyDef::collectionProp("objects", kObject), std::move(objects));
  PointerAccessor toolPtr;
  const OwnerKey tk = *toolOwner;
  toolPtr.get = [&tool, tk](const PointerRef &, Context &) {
    return PointerRef{&tool, kTool, tk};
  };
  s.addPointer(PropertyDef::pointerProp("tool", kTool), std::move(toolPtr));
  test_assert(reg.add(std::move(s)).ok());
  test_assert(reg.addRuntimeOwner("scene", kScene, [&scene]() { return (void *)&scene; }).ok());
}

/* Everything a test needs, built in order and destroyed in reverse. */
struct World {
  Runtime rt;
  Scene scene;
  Tool tool;
  Library lib{&rt};
  litestl::util::Vector<BlockHandle> meshes;

  World()
  {
    addBlockType<Object>(rt, "test.Object", kObject);
    addBlockType<Mesh>(rt, "test.Mesh", kMesh);
    registerStructs(rt, scene, tool);
    for (const char *name : {"Cube", "Sphere"}) {
      Result<BlockHandle> m =
          lib.create(kMeshType, std::string(name) + "Mesh", litestl::alloc::New<Mesh>("Mesh"));
      Object *o = litestl::alloc::New<Object>("Object");
      Result<BlockHandle> ob = lib.create(kObjectType, name, o);
      test_assert(m.ok() && ob.ok());
      test_assert(lib.writeRef(o->mesh, lib.get(*m)->id()).ok());
      meshes.append(*m);
      scene.objects.append(*ob);
    }
  }

  Context context()
  {
    return Context(&rt, &lib, rt.ext<ApiRegistry>().runtimeOwner("scene"));
  }
  BlockId objectId(int i)
  {
    return lib.get(scene.objects[i])->id();
  }
  BlockId meshId(int i)
  {
    return lib.get(meshes[i])->id();
  }
  Object &object(int i)
  {
    return *static_cast<Object *>(lib.data(scene.objects[i]));
  }
  Mesh &mesh(int i)
  {
    return *static_cast<Mesh *>(lib.data(meshes[i]));
  }
};

inline bool hasMsg(const Status &s, const char *needle)
{
  if (strstr(s.message(), needle)) {
    return true;
  }
  fprintf(stderr, "message was: %s\n", s.message());
  return false;
}

} // namespace fixture
