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

/* A struct nested inside an Object block (not its root). */
struct Mod {
  double strength = 0.25;
};

struct Object {
  RefCell mesh{RefKind::OptStrong};
  float location[3] = {0, 0, 0};
  int mode = 0;
  uint32_t flags = 0;
  Mod mod;
};

struct Brush {
  double size = 1;
};

struct Tool {
  double strength = 0.5;
  double free = 0;
  int version = 3;
  int count = 4;
  int pick = 0;
  /* Brushes live inside the Tool owner; `activeBrush` is a settable pointer into them. */
  Brush brushes[2];
  int activeBrush = 0;
  bool locked = false;
  int guarded = 0;
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
inline const StructId kMod = StructId::of("Mod");
inline const StructId kBrush = StructId::of("Brush");
inline const StructId kIdBase = StructId::of("IdBase");

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
/* Calls to Object.data's and plainData's. */
inline int g_dataUpdates = 0;

inline void registerStructs(Runtime &rt, Scene &scene, Tool &tool)
{
  ApiRegistry &reg = rt.ext<ApiRegistry>();

  /* Mesh's parent: pointers declared as IdBase reach a Mesh's members (subtyping). */
  test_assert(reg.add(ApiStruct("IdBase")).ok());
  ApiStruct mod("Mod");
  mod.addField(PropertyDef::floatProp("strength"), &Mod::strength);
  test_assert(reg.add(std::move(mod)).ok());
  ApiStruct brush("Brush");
  brush.addField(PropertyDef::floatProp("size"), &Brush::size);
  test_assert(reg.add(std::move(brush)).ok());

  ApiStruct mesh("Mesh");
  mesh.parent(kIdBase);
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
    return blockRef(lib, lib.resolveRef(static_cast<Object *>(self.data)->mesh), StructId());
  };
  data.set = [](const PointerRef &self, Context &ctx, const PointerRef &target) {
    return ctx.library()->writeRef(static_cast<Object *>(self.data)->mesh, target.owner.block);
  };
  PointerAccessor plain = data;
  data.cell = [](const PointerRef &self, Context &) {
    return &static_cast<Object *>(self.data)->mesh;
  };
  const auto onUpdate = [](const PointerRef &, Context &) { g_dataUpdates++; };
  object.addPointer(PropertyDef::pointerProp("data", kMesh), std::move(data)).update(onUpdate);
  /* The same without a cell. */
  object.addPointer(PropertyDef::pointerProp("plainData", kMesh), std::move(plain))
      .update(onUpdate);
  /* The same target, declared as the base struct and left untyped. */
  PointerAccessor anyData;
  anyData.get = [](const PointerRef &self, Context &ctx) {
    Library &lib = *ctx.library();
    return blockRef(lib, lib.resolveRef(static_cast<Object *>(self.data)->mesh), StructId());
  };
  object.addPointer(PropertyDef::pointerProp("anyData", kIdBase), std::move(anyData));
  /* A fixed nested struct: no owner, no setter, so the hop is stable. */
  PointerAccessor modPtr;
  modPtr.get = [](const PointerRef &self, Context &) {
    return PointerRef{&static_cast<Object *>(self.data)->mod, kMod, OwnerKey()};
  };
  object.addPointer(PropertyDef::pointerProp("mod", kMod), std::move(modPtr));
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
  t.addField(PropertyDef::boolProp("locked"), &Tool::locked);
  t.addField(PropertyDef::intProp("guarded"), &Tool::guarded)
      .dynamic([](PropertyDef &def, const PointerRef &self, Context &) {
        if (static_cast<Tool *>(self.data)->locked) {
          def.access(PropAccess::ReadOnly);
        }
      });
  /* A getter that returns the wrong type, as a failed foreign getter would. */
  t.add(PropertyDef::vecProp("broken", 3),
        ValueAccessor{[](const PointerRef &, Context &) { return PropValue(); },
                      [](const PointerRef &, Context &, const PropValue &) { return Status(); }});
  CollectionAccessor brushes;
  brushes.length = [](const PointerRef &, Context &) { return int64_t(2); };
  brushes.at = [](const PointerRef &self, Context &, int64_t i) {
    return PointerRef{&static_cast<Tool *>(self.data)->brushes[i], kBrush, OwnerKey()};
  };
  brushes.lookup = [](const PointerRef &self, Context &, const KeySegment &key) {
    Tool *tl = static_cast<Tool *>(self.data);
    const char *k = key.text.c_str();
    if (key.kind == KeySegment::Kind::StringKey && (strcmp(k, "A") == 0 || strcmp(k, "B") == 0)) {
      return PointerRef{&tl->brushes[k[0] == 'A' ? 0 : 1], kBrush, OwnerKey()};
    }
    return PointerRef();
  };
  brushes.keyOf = [](const PointerRef &self, Context &, const PointerRef &item) {
    Tool *tl = static_cast<Tool *>(self.data);
    return KeySegment::stringKey(item.data == &tl->brushes[0] ? "A" : "B");
  };
  auto activeGet = [](const PointerRef &self, Context &) {
    Tool *tl = static_cast<Tool *>(self.data);
    return PointerRef{&tl->brushes[tl->activeBrush], kBrush, OwnerKey()};
  };
  auto activeSet = [](const PointerRef &self, Context &, const PointerRef &target) {
    Tool *tl = static_cast<Tool *>(self.data);
    tl->activeBrush = target.data == &tl->brushes[1] ? 1 : 0;
    return Status();
  };
  brushes.active = activeGet;
  brushes.activeMember = "activeBrush";
  t.addCollection(PropertyDef::collectionProp("brushes", kBrush), std::move(brushes));
  PointerAccessor active;
  active.get = activeGet;
  active.set = activeSet;
  active.address = [](const PointerRef &self, Context &, const PointerRef &target) {
    Tool *tl = static_cast<Tool *>(self.data);
    StableRef r;
    r.owner = self.owner;
    r.key.append(KeySegment::member("brushes"));
    r.key.append(KeySegment::stringKey(target.data == &tl->brushes[0] ? "A" : "B"));
    return r;
  };
  t.addPointer(PropertyDef::pointerProp("activeBrush", kBrush), std::move(active));
  /* The same pointer without an address: reading or walking it is Unsupported. */
  PointerAccessor unaddressed;
  unaddressed.get = activeGet;
  unaddressed.set = activeSet;
  t.addPointer(PropertyDef::pointerProp("activeBrushNoAddress", kBrush), std::move(unaddressed));
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
  /* Crosses into an Object block but returns a struct nested in it, not its root. */
  PointerAccessor firstMod;
  firstMod.get = [](const PointerRef &self, Context &ctx) {
    Library &lib = *ctx.library();
    PointerRef ob = blockRef(lib, static_cast<Scene *>(self.data)->objects[0], kObject);
    return PointerRef{&static_cast<Object *>(ob.data)->mod, kMod, ob.owner};
  };
  s.addPointer(PropertyDef::pointerProp("firstMod", kMod), std::move(firstMod));
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
