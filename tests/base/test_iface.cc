#include "coredcc/base/iface/owned.h"
#include "coredcc/base/iface/registry.h"

#include "test_util.h"

#include <cstring>
#include <string>

test_init;

using namespace coredcc;
using litestl::alloc::PermanentGuard;
namespace lbt = litestl::binding::types;
namespace binding = litestl::binding; /* BIND_STRUCT_MEMBER names binding::Bind */

/* ---- value types crossing slots ---- */

struct Ray {
  float origin = 0.0f;
  float dir = 1.0f;
  static lbt::Struct<Ray> *defineBindings()
  {
    static lbt::Struct<Ray> *st = nullptr;
    if (!st) {
      PermanentGuard guard;
      st = new lbt::Struct<Ray>("test::Ray", sizeof(Ray));
      BIND_STRUCT_MEMBER(st, origin);
      BIND_STRUCT_MEMBER(st, dir);
    }
    return st;
  }
};

/* Same name and size as Ray, different C++ type. */
struct FakeRay {
  float origin = 0.0f;
  float dir = 1.0f;
  static lbt::Struct<FakeRay> *defineBindings()
  {
    static lbt::Struct<FakeRay> *st = nullptr;
    if (!st) {
      PermanentGuard guard;
      st = new lbt::Struct<FakeRay>("test::Ray", sizeof(FakeRay));
      BIND_STRUCT_MEMBER(st, origin);
      BIND_STRUCT_MEMBER(st, dir);
    }
    return st;
  }
};

struct Hit {
  float t = -1.0f;
  static lbt::Struct<Hit> *defineBindings()
  {
    static lbt::Struct<Hit> *st = nullptr;
    if (!st) {
      PermanentGuard guard;
      st = new lbt::Struct<Hit>("test::Hit", sizeof(Hit));
      BIND_STRUCT_MEMBER(st, t);
    }
    return st;
  }
};

/* ---- interfaces ---- */

struct Spatial : IfaceRef {
  bool rayCast(const Ray &ray, Hit &hit) const
  {
    return CDCC_CALL(rayCast, ray, hit);
  }
  int count() const
  {
    return CDCC_CALL(count);
  }
  double cost() const
  {
    return CDCC_HAS(cost) ? CDCC_CALL(cost) : count() * 2.0;
  }

  CDCC_INTERFACE(Spatial, "test.Spatial")
  {
    BIND_STRUCT_METHOD(st, rayCast, MARGS("ray", "hit"));
    BIND_STRUCT_METHOD(st, count, MARGS());
    BIND_OPTIONAL_METHOD(st, cost, MARGS());
  }
};

/* Self-referencing, TransformModel-shaped: a slot takes the interface itself. */
struct Node : IfaceRef {
  int depth(Node parent) const
  {
    return CDCC_CALL(depth, parent);
  }
  int id() const
  {
    return CDCC_CALL(id);
  }
  CDCC_INTERFACE(Node, "test.Node")
  {
    BIND_STRUCT_METHOD(st, depth, MARGS("parent"));
    BIND_STRUCT_METHOD(st, id, MARGS());
  }
};

struct Named : IfaceRef {
  int nameLength() const
  {
    return CDCC_CALL(nameLength);
  }
  CDCC_INTERFACE(Named, "test.Named")
  {
    BIND_STRUCT_METHOD(st, nameLength, MARGS());
  }
};

/* Single base (slots concatenated) plus an upcast to a second base. */
struct Volume : Spatial {
  float volume() const
  {
    return CDCC_CALL(volume);
  }
  CDCC_UPCASTS(Named);
  CDCC_INTERFACE_EXTENDS(Volume, "test.Volume", Spatial)
  {
    BIND_STRUCT_METHOD(st, volume, MARGS());
  }
};

/* ---- implementations ---- */

struct MeshImpl {
  int verts = 8;
  bool rayCast(const Ray &ray, Hit &hit) const
  {
    hit.t = ray.origin + 1.0f;
    return true;
  }
  int count() const
  {
    return verts;
  }
  int extra() const
  {
    return 0;
  }
  static lbt::Struct<MeshImpl> *defineBindings()
  {
    auto *st = new lbt::Struct<MeshImpl>("test::MeshImpl", sizeof(MeshImpl));
    BIND_STRUCT_METHOD(st, rayCast, MARGS("ray", "hit"));
    BIND_STRUCT_METHOD(st, count, MARGS());
    BIND_STRUCT_METHOD(st, extra, MARGS());
    return st;
  }
};

struct CostlyMesh : MeshImpl {
  double cost() const
  {
    return 99.0;
  }
  static lbt::Struct<CostlyMesh> *defineBindings()
  {
    auto *st = new lbt::Struct<CostlyMesh>("test::CostlyMesh", sizeof(CostlyMesh));
    BIND_STRUCT_METHOD(st, rayCast, MARGS("ray", "hit"));
    BIND_STRUCT_METHOD(st, count, MARGS());
    BIND_STRUCT_METHOD(st, cost, MARGS());
    return st;
  }
};

/* Every way to get it wrong at once. */
struct BadMesh {
  bool rayCast(Ray &ray, Hit &hit) const /* const Ray& erased in the prototype */
  {
    hit.t = ray.origin;
    return false;
  }
  int64_t count() const /* int vs int64 */
  {
    return 0;
  }
  double cost() /* const slot, non-const method */
  {
    return 0.0;
  }
  static lbt::Struct<BadMesh> *defineBindings()
  {
    auto *st = new lbt::Struct<BadMesh>("test::BadMesh", sizeof(BadMesh));
    BIND_STRUCT_METHOD(st, rayCast, MARGS("ray", "hit"));
    BIND_STRUCT_METHOD(st, count, MARGS());
    BIND_STRUCT_METHOD(st, cost, MARGS());
    return st;
  }
};

/* A type bound under another type's name: accepted by name alone in the prototype. */
struct ImpostorMesh {
  bool rayCast(const FakeRay &ray, Hit &hit) const
  {
    hit.t = ray.origin;
    return true;
  }
  int count() const
  {
    return 1;
  }
  static lbt::Struct<ImpostorMesh> *defineBindings()
  {
    auto *st = new lbt::Struct<ImpostorMesh>("test::ImpostorMesh", sizeof(ImpostorMesh));
    BIND_STRUCT_METHOD(st, rayCast, MARGS("ray", "hit"));
    BIND_STRUCT_METHOD(st, count, MARGS());
    return st;
  }
};

/* A type we don't own, adapted by free functions. */
struct ThirdPartyCloud {
  int points = 5;
};
static bool cloudRayCast(const ThirdPartyCloud &c, const Ray &ray, Hit &hit)
{
  hit.t = ray.origin + float(c.points);
  return c.points > 0;
}
static int cloudCount(const ThirdPartyCloud &c)
{
  return c.points;
}

struct TreeNode {
  TreeNode *parentNode = nullptr;
  int ident = 0;
  int depth(Node parent) const
  {
    return parent ? parent.depth(Node()) + 1 : 0;
  }
  int id() const
  {
    return ident;
  }
  static lbt::Struct<TreeNode> *defineBindings()
  {
    auto *st = new lbt::Struct<TreeNode>("test::TreeNode", sizeof(TreeNode));
    BIND_STRUCT_METHOD(st, depth, MARGS("parent"));
    BIND_STRUCT_METHOD(st, id, MARGS());
    return st;
  }
};

struct Box {
  float size = 2.0f;
  bool rayCast(const Ray &, Hit &hit) const
  {
    hit.t = size;
    return true;
  }
  int count() const
  {
    return 6;
  }
  float volume() const
  {
    return size * size * size;
  }
  int nameLength() const
  {
    return 3;
  }
  static lbt::Struct<Box> *defineBindings()
  {
    auto *st = new lbt::Struct<Box>("test::Box", sizeof(Box));
    BIND_STRUCT_METHOD(st, rayCast, MARGS("ray", "hit"));
    BIND_STRUCT_METHOD(st, count, MARGS());
    BIND_STRUCT_METHOD(st, volume, MARGS());
    BIND_STRUCT_METHOD(st, nameLength, MARGS());
    return st;
  }
};

static int g_liveTracked = 0;
struct TrackedMesh : MeshImpl {
  TrackedMesh()
  {
    g_liveTracked++;
  }
  ~TrackedMesh()
  {
    g_liveTracked--;
  }
  static lbt::Struct<TrackedMesh> *defineBindings()
  {
    auto *st = new lbt::Struct<TrackedMesh>("test::TrackedMesh", sizeof(TrackedMesh));
    BIND_STRUCT_METHOD(st, rayCast, MARGS("ray", "hit"));
    BIND_STRUCT_METHOD(st, count, MARGS());
    return st;
  }
};

/* ---- a fake foreign host ---- */

/* Records calls and answers through the slot descriptors, as a real backend would. */
struct FakeDispatcher : ForeignDispatcher {
  std::string log;
  Status call(const IfaceTable &table, void *self, int slot, void **args, void *ret) override
  {
    const lbt::Method *m = table.ifaceDesc->methods[slot];
    log += std::string(m->name.c_str()) + "(";
    for (int i = 0; i < int(m->params.size()); i++) {
      const auto *ref = static_cast<const lbt::Reference *>(m->params[i].type);
      log += (i ? "," : "") + std::string(ref->isConst ? "in " : "out ") +
             ref->refType->name.c_str();
    }
    log += ")";
    int *selfValue = static_cast<int *>(self);
    if (strcmp(m->name.c_str(), "count") == 0) {
      new (ret) int(*selfValue);
    } else if (strcmp(m->name.c_str(), "rayCast") == 0) {
      const Ray &ray = *static_cast<const Ray *>(args[0]);
      static_cast<Hit *>(args[1])->t = ray.origin * 10.0f;
      new (ret) bool(true);
    }
    return Status();
  }
};

/* ---- tests ---- */

static void test_bound_class()
{
  Runtime rt;
  Status s = registerImpl<Spatial, MeshImpl>(rt, "test.MeshImpl");
  test_assert(s.ok());
  MeshImpl mesh;
  Spatial sp = query<Spatial>(rt, TypeId::of("test.MeshImpl"), &mesh);
  test_assert(bool(sp));
  test_assert(sp.count() == 8);
  Hit hit;
  const Ray ray{2.0f, 1.0f};
  test_assert(sp.rayCast(ray, hit));
  test_assert(hit.t == 3.0f);
  /* Optional slot absent: the forwarder's default runs. */
  test_assert(!sp.table()->hasSlot(2));
  test_assert(sp.cost() == 16.0);
  test_assert(sp.as<MeshImpl>() == &mesh);
  test_assert(sp.as<Box>() == nullptr);
  test_assert(!hasFlag(sp.table()->flags, IfaceFlags::ThreadAffine));

  test_assert(!query<Spatial>(rt, TypeId::of("test.Nothing"), &mesh));
  test_assert(!query<Node>(rt, TypeId::of("test.MeshImpl"), &mesh));
  test_assert(rt.validateAll().ok());
}

static void test_optional_present()
{
  Runtime rt;
  test_assert((registerImpl<Spatial, CostlyMesh>(rt, "test.CostlyMesh").ok()));
  CostlyMesh mesh;
  Spatial sp = query<Spatial>(rt, TypeId::of("test.CostlyMesh"), &mesh);
  test_assert(sp.cost() == 99.0);
  test_assert(rt.validateAll().ok());
}

static void test_adaptor()
{
  Runtime rt;
  Status s = registerAdaptor<Spatial, ThirdPartyCloud>(
      rt, "test.CloudAdaptor", [](lbt::Struct<ThirdPartyCloud> *st) {
        BIND_ADAPTOR_METHOD(st, rayCast, cloudRayCast, MARGS("ray", "hit"));
        BIND_ADAPTOR_METHOD(st, count, cloudCount, MARGS());
      });
  test_assert(s.ok());
  ThirdPartyCloud cloud;
  Spatial sp = query<Spatial>(rt, TypeId::of("test.CloudAdaptor"), &cloud);
  test_assert(sp.count() == 5);
  Hit hit;
  const Ray ray{1.0f, 0.0f};
  test_assert(sp.rayCast(ray, hit));
  test_assert(hit.t == 6.0f);
  test_assert(sp.as<ThirdPartyCloud>() == &cloud);
  test_assert(rt.validateAll().ok());
}

static void test_foreign()
{
  Runtime rt;
  FakeDispatcher host;
  const char *present[] = {"rayCast", "count"};
  Status s = registerForeign<Spatial>(rt, "test.JsMesh", &host, nullptr, present);
  test_assert(s.ok());
  int jsObject = 41;
  Spatial sp = query<Spatial>(rt, TypeId::of("test.JsMesh"), &jsObject);
  test_assert(hasFlag(sp.table()->flags, IfaceFlags::ThreadAffine));
  test_assert(sp.count() == 41);
  Hit hit;
  const Ray ray{0.5f, 0.0f};
  test_assert(sp.rayCast(ray, hit));
  test_assert(hit.t == 5.0f);
  /* Absent optional slot: the forwarder default, without calling the host. */
  test_assert(sp.cost() == 82.0);
  test_assert(host.log == "count()rayCast(in test::Ray,out test::Hit)count()");
  test_assert(sp.as<MeshImpl>() == nullptr);

  const char *missingRequired[] = {"rayCast"};
  Status bad = registerForeign<Spatial>(rt, "test.JsHalf", &host, nullptr, missingRequired);
  test_assert(bad.code() == StatusCode::TypeMismatch);
  test_assert(strstr(bad.message(), "slot count [int32 count() const]: missing") != nullptr);

  const char *unknown[] = {"rayCast", "count", "fly"};
  Status bad2 = registerForeign<Spatial>(rt, "test.JsOdd", &host, nullptr, unknown);
  test_assert(strstr(bad2.message(), "'fly' is not a slot") != nullptr);
  test_assert(rt.validateAll().ok());
}

static void test_mismatch_messages()
{
  Runtime rt;
  Status s = registerImpl<Spatial, BadMesh>(rt, "test.BadMesh");
  test_assert(s.code() == StatusCode::TypeMismatch);
  const char *msg = s.message();
  test_assert(strstr(msg, "test.BadMesh does not implement test.Spatial:") != nullptr);
  test_assert(strstr(msg,
                     "slot rayCast [boolean rayCast(const test::Ray&, test::Hit&) const]: no "
                     "matching signature") != nullptr);
  test_assert(strstr(msg, "argument 1 ('ray') is test::Ray&, slot wants const test::Ray&") !=
              nullptr);
  test_assert(strstr(msg,
                     "slot count [int32 count() const]: no matching signature\n    candidate "
                     "int64 count() const: return type int64 != int32") != nullptr);
  test_assert(strstr(msg, "candidate double cost(): slot is const but the method is not") !=
              nullptr);
  test_assert(!query<Spatial>(rt, TypeId::of("test.BadMesh"), nullptr));
  test_assert(rt.ifaces().tableCount() == 0);
  test_assert(rt.validateAll().ok());
}

static void test_impostor_type()
{
  Runtime rt;
  Status s = registerImpl<Spatial, ImpostorMesh>(rt, "test.ImpostorMesh");
  test_assert(s.code() == StatusCode::TypeMismatch);
  test_assert(strstr(s.message(), "argument 1 ('ray') is const test::Ray&, slot wants const "
                                   "test::Ray& (different C++ types with the same name)") !=
              nullptr);
  test_assert(rt.validateAll().ok());
}

static void test_self_reference()
{
  Runtime rt;
  test_assert((registerImpl<Node, TreeNode>(rt, "test.TreeNode").ok()));
  TreeNode root, child;
  child.ident = 2;
  Node rootRef = query<Node>(rt, TypeId::of("test.TreeNode"), &root);
  Node childRef = query<Node>(rt, TypeId::of("test.TreeNode"), &child);
  test_assert(childRef.id() == 2);
  test_assert(childRef.depth(rootRef) == 1);
  test_assert(childRef.depth(Node()) == 0);
  test_assert(rt.validateAll().ok());
}

static void test_inheritance_and_upcast()
{
  Runtime rt;
  test_assert((registerImpl<Volume, Box>(rt, "test.Box").ok()));
  Box box;
  Volume vol = query<Volume>(rt, TypeId::of("test.Box"), &box);
  test_assert(bool(vol));
  test_assert(vol.volume() == 8.0f);
  /* Base slots through the derived table: indices coincide. */
  test_assert(vol.count() == 6);
  Spatial asBase = vol;
  Hit hit;
  test_assert(asBase.rayCast(Ray{}, hit) && hit.t == 2.0f);
  /* The second base is reached by upcast, and is also queryable directly. */
  Named named = upcast<Named>(vol);
  test_assert(bool(named));
  test_assert(named.nameLength() == 3);
  test_assert(query<Named>(rt, TypeId::of("test.Box"), &box).nameLength() == 3);
  test_assert(!upcast<Node>(vol));
  /* The concatenated base is queryable and reachable by upcast too. */
  test_assert(query<Spatial>(rt, TypeId::of("test.Box"), &box).count() == 6);
  test_assert(upcast<Spatial>(vol).count() == 6);
  test_assert(upcast<Spatial>(named).count() == 6);
  test_assert(rt.validateAll().ok());
}

static void test_registration_rules()
{
  Runtime rt;
  test_assert((registerImpl<Spatial, MeshImpl>(rt, "test.MeshImpl").ok()));
  Status again = registerImpl<Spatial, MeshImpl>(rt, "test.MeshImpl");
  test_assert(again.code() == StatusCode::AlreadyExists);
  test_assert(rt.ifaces().tableCount() == 1);
  test_assert((registerImpl<Spatial, CostlyMesh>(rt, "test.CostlyMesh").ok()));
  test_assert(rt.validateAll().ok());
  test_assert(rt.ifaces().findInterface(InterfaceId::of("test.Spatial")) != nullptr);
}

static void test_owned()
{
  Runtime rt;
  test_assert((registerImpl<Spatial, TrackedMesh>(rt, "test.TrackedMesh").ok()));
  {
    Result<Owned<Spatial>> owned =
        own<Spatial>(rt, "test.TrackedMesh", litestl::alloc::New<TrackedMesh>("tracked"));
    test_assert(owned.ok());
    test_assert(g_liveTracked == 1);
    test_assert((*owned)->count() == 8);
    Owned<Spatial> moved = std::move(*owned);
    test_assert(g_liveTracked == 1);
    test_assert(moved->count() == 8);
  }
  test_assert(g_liveTracked == 0);

  Result<Owned<Spatial>> missing =
      own<Spatial>(rt, "test.Unregistered", litestl::alloc::New<TrackedMesh>("tracked"));
  test_assert(!missing.ok());
  test_assert(g_liveTracked == 0);
  test_assert(rt.validateAll().ok());
}

int main()
{
  test_bound_class();
  test_optional_present();
  test_adaptor();
  test_foreign();
  test_mismatch_messages();
  test_impostor_type();
  test_self_reference();
  test_inheritance_and_upcast();
  test_registration_rules();
  test_owned();
  return test_end();
}
