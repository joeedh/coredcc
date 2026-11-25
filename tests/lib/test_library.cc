#include "coredcc/base/iface/registry.h"
#include "coredcc/lib/library.h"

#include "test_util.h"

#include <cstdio>
#include <cstring>
#include <string>

test_init;

using namespace coredcc;
using litestl::alloc::PermanentGuard;
namespace lbt = litestl::binding::types;

static bool hasMsg(const Status &s, const char *needle)
{
  if (strstr(s.message(), needle)) {
    return true;
  }
  fprintf(stderr, "message was: %s\n", s.message());
  return false;
}

/* ---- a native test client ---- */

/* A block's data: three refs of each kind, plus a value. */
struct Thing {
  RefCell owner{RefKind::Strong};
  RefCell material{RefKind::OptStrong};
  RefCell parent{RefKind::Weak};
  int value = 0;
};

static std::string g_log;
static Library *g_lib = nullptr; /* for the re-entrant destroy test */
static bool g_reenter = false;
static bool g_createInMark = false;

struct ThingProvider {
  void *data(const Datablock &b) const
  {
    return b.clientData();
  }
  void foreachRef(const Datablock &b, RefVisitor &v) const
  {
    if (g_createInMark && g_lib) {
      g_createInMark = false;
      Result<BlockHandle> c = g_lib->create(TypeId::of("test.Thing"), "mark", nullptr);
      g_log +=
          c.ok() ? "mark-create ok;"
                 : std::string("mark-create ") + statusCodeName(c.status().code()) + ";";
    }
    if (Thing *t = static_cast<Thing *>(b.clientData())) {
      v.visit(t->owner);
      v.visit(t->material);
      v.visit(t->parent);
    }
  }
  void destroy(const Datablock &b)
  {
    g_log += std::string("destroy ") + b.name().c_str() + ";";
    if (g_reenter && g_lib) {
      /* Reads work, creates are Busy, void mutations wait for the sweep's end. */
      g_log += g_lib->get(g_lib->find(b.id())) ? "read ok;" : "read failed;";
      Result<BlockHandle> c = g_lib->create(TypeId::of("test.Thing"), "late", nullptr);
      g_log += c.ok() ? "create ok;"
                      : std::string("create ") + statusCodeName(c.status().code()) + ";";
      g_lib->forEach([](BlockHandle h, const Datablock &other) {
        if (strcmp(other.name().c_str(), "survivor") == 0) {
          g_lib->setFlag(h, BlockFlags::FakeUser, true);
        }
      });
    }
    if (b.clientData()) {
      litestl::alloc::Delete(static_cast<Thing *>(b.clientData()));
    }
  }
  void onWeakTargetLost(const Datablock &user, const Datablock &lost)
  {
    g_log +=
        std::string("lost ") + user.name().c_str() + "->" + lost.name().c_str() + ";";
  }
  static lbt::Struct<ThingProvider> *defineBindings()
  {
    auto *st =
        new lbt::Struct<ThingProvider>("test::ThingProvider", sizeof(ThingProvider));
    BIND_STRUCT_METHOD(st, data, MARGS("block"));
    BIND_STRUCT_METHOD(st, foreachRef, MARGS("block", "visitor"));
    BIND_STRUCT_METHOD(st, destroy, MARGS("block"));
    BIND_STRUCT_METHOD(st, onWeakTargetLost, MARGS("user", "lost"));
    return st;
  }
};

/* A capability of Thing's data, registered under the block type's name. */
struct Valued : IfaceRef {
  int value() const
  {
    return CDCC_CALL(value);
  }
  CDCC_INTERFACE(Valued, "test.Valued")
  {
    BIND_STRUCT_METHOD(st, value, MARGS());
  }
};

struct ThingValue {
  int value() const
  {
    return reinterpret_cast<const Thing *>(this)->value;
  }
  static lbt::Struct<ThingValue> *defineBindings()
  {
    auto *st = new lbt::Struct<ThingValue>("test::ThingValue", sizeof(ThingValue));
    BIND_STRUCT_METHOD(st, value, MARGS());
    return st;
  }
};

struct Constant {
  int v = 0;
  int value() const
  {
    return v;
  }
  static lbt::Struct<Constant> *defineBindings()
  {
    auto *st = new lbt::Struct<Constant>("test::Constant", sizeof(Constant));
    BIND_STRUCT_METHOD(st, value, MARGS());
    return st;
  }
};

static const TypeId kThing = TypeId::of("test.Thing");

static void setup(Runtime &rt)
{
  test_assert(
      (registerImpl<BlockProvider, ThingProvider>(rt, "test.Thing#provider").ok()));
  Result<Owned<BlockProvider>> p = own<BlockProvider>(
      rt, "test.Thing#provider", litestl::alloc::New<ThingProvider>("provider"));
  test_assert(p.ok());
  BlockTypeInfo info;
  info.name = "test.Thing";
  info.client = ClientId::of(kCoredccClient);
  Result<TypeId> t = rt.ext<BlockTypeRegistry>().add(info, std::move(p).value());
  test_assert(t.ok() && *t == kThing);
}

static BlockHandle make(Library &lib, const char *name, int value = 0)
{
  Thing *t = litestl::alloc::New<Thing>("Thing");
  t->value = value;
  Result<BlockHandle> h = lib.create(kThing, name, t);
  test_assert(h.ok());
  return h.ok() ? *h : BlockHandle();
}

static Thing *thing(Library &lib, BlockHandle h)
{
  return static_cast<Thing *>(lib.get(h)->clientData());
}

/* ---- tests ---- */

static void test_create_and_find()
{
  Runtime rt;
  setup(rt);
  Library lib(&rt);
  BlockHandle a = make(lib, "a");
  const Datablock *b = lib.get(a);
  test_assert(b && b->state() == BlockState::Live && strcmp(b->name().c_str(), "a") == 0);
  test_assert(b->id().client == ClientId::of(kCoredccClient) && !b->id().local.isNull());
  test_assert(lib.find(b->id()) == a);
  test_assert(lib.data(a) == b->clientData());

  /* Client-built ids. */
  BlockId id{ClientId::of(kCoredccClient), LocalId{7, 7}};
  test_assert(lib.createWithId(kThing, id, "x", nullptr).ok());
  test_assert(lib.createWithId(kThing, id, "y", nullptr).status().code() ==
              StatusCode::AlreadyExists);
  test_assert(lib.createWithId(
                     kThing, BlockId{ClientId::of("other"), LocalId{1, 1}}, "z", nullptr)
                  .status()
                  .code() == StatusCode::InvalidArgument);
  test_assert(
      lib.createWithId(
             kThing, BlockId{ClientId::of(kCoredccClient), LocalId{}}, "z", nullptr)
          .status()
          .code() == StatusCode::InvalidArgument);
  test_assert(lib.create(TypeId::of("test.Nope"), "n", nullptr).status().code() ==
              StatusCode::NotFound);

  /* A deterministic GUID source. */
  uint64_t next = 100;
  rt.setGuidSource([&next]() { return LocalId{0, next++}; });
  test_assert(lib.get(make(lib, "g"))->id().local.lo == 100);
  rt.setGuidSource({});
  test_assert(lib.validate().ok() && rt.validateAll().ok());
}

static void test_ref_kinds_and_states()
{
  Runtime rt;
  setup(rt);
  Library lib(&rt);
  BlockHandle user = make(lib, "user");
  BlockHandle target = make(lib, "target", 42);
  const BlockId tid = lib.get(target)->id();
  Thing *u = thing(lib, user);

  test_assert(lib.writeRef(u->parent, tid).ok());
  test_assert(lib.writeRef(u->owner, tid).ok());
  test_assert(lib.writeRef(u->owner, BlockId()).code() == StatusCode::InvalidArgument);
  test_assert(lib.writeRef(u->material, BlockId()).ok()); /* nullable */
  test_assert(lib.resolveRef(u->parent) == target);

  Ref<Thing> typed;
  test_assert(lib.writeRef(typed.cell(), tid).ok());
  test_assert(typed.get(lib)->value == 42);

  /* Every non-Live state resolves to null, and Live again resolves again. */
  test_assert(lib.markDeleted(target).ok());
  test_assert(lib.resolveRef(u->parent).isNull() && lib.resolveRef(u->owner).isNull());
  test_assert(typed.get(lib) == nullptr);
  test_assert(lib.get(target)->state() == BlockState::InHistory);
  test_assert(lib.markDeleted(target).code() == StatusCode::FailedPrecondition);
  test_assert(lib.restoreFromHistory(target).ok());
  test_assert(lib.resolveRef(u->parent) == target);

  test_assert(lib.onHostDestroyed(tid).ok());
  test_assert(lib.resolveRef(u->owner).isNull() && lib.data(target) == nullptr);
  test_assert(lib.onHostRestored(tid).ok());
  test_assert(lib.resolveRef(u->owner) == target);
  test_assert(lib.onHostRestored(tid).code() == StatusCode::FailedPrecondition);
  test_assert(lib.validate().ok());
}

static void test_cross_library_aliasing()
{
  Runtime rt;
  setup(rt);
  Library a(&rt);
  Library b(&rt);
  BlockHandle ha = make(a, "in a");
  BlockHandle hb = make(b, "in b");
  test_assert(ha == hb); /* the same first handle in both */
  RefCell cell(RefKind::Weak);
  test_assert(a.writeRef(cell, a.get(ha)->id()).ok());
  test_assert(a.resolveRef(cell) == ha);
  /* The cached handle names b's first block, but b's record has another id. */
  test_assert(b.resolveRef(cell).isNull());
  test_assert(a.resolveRef(cell) == ha);
}

static void test_revive()
{
  Runtime rt;
  setup(rt);
  Library lib(&rt);
  BlockHandle live = make(lib, "live");
  const BlockId liveId = lib.get(live)->id();
  test_assert(lib.reviveAs(kThing, liveId, "again", nullptr).status().code() ==
              StatusCode::AlreadyExists);

  BlockHandle hist = make(lib, "hist");
  test_assert(lib.markDeleted(hist).ok());
  Status s = lib.reviveAs(kThing, lib.get(hist)->id(), "again", nullptr).status();
  test_assert(s.code() == StatusCode::FailedPrecondition && hasMsg(s, "InHistory"));

  BlockHandle det = make(lib, "det");
  test_assert(lib.onHostDestroyed(lib.get(det)->id()).ok());
  test_assert(hasMsg(lib.reviveAs(kThing, lib.get(det)->id(), "x", nullptr).status(),
                     "Detached"));

  const BlockId phId{ClientId::of("missing.client"), LocalId{3, 3}};
  test_assert(lib.addPlaceholder(phId, "missing.Type", "ph", {}).ok());
  test_assert(hasMsg(lib.reviveAs(kThing, phId, "x", nullptr).status(), "Placeholder"));

  /* Undo a create (destroy it), then redo with the same id: one record, same id. */
  BlockHandle made = make(lib, "made");
  const BlockId madeId = lib.get(made)->id();
  test_assert(lib.markDeleted(made).ok());
  test_assert(lib.destroyFromHistory(made).ok());
  test_assert(lib.find(madeId).isNull() && lib.get(made) == nullptr);
  Result<BlockHandle> revived =
      lib.reviveAs(kThing, madeId, "made", litestl::alloc::New<Thing>("Thing"));
  test_assert(revived.ok() && lib.get(*revived)->id() == madeId);
  test_assert(lib.validate().ok());
}

static void test_gc()
{
  g_log.clear();
  Runtime rt;
  setup(rt);
  Library lib(&rt);
  lib.setQuarantine(true);
  BlockHandle root = make(lib, "root");
  lib.setFlag(root, BlockFlags::Root, true);
  BlockHandle kept = make(lib, "kept");
  BlockHandle chained = make(lib, "chained");
  BlockHandle weakOnly = make(lib, "weakOnly");
  BlockHandle cycleA = make(lib, "cycleA");
  BlockHandle cycleB = make(lib, "cycleB");
  BlockHandle pinned = make(lib, "pinned");
  BlockHandle fake = make(lib, "fake");
  BlockHandle hist = make(lib, "hist");
  lib.pin(pinned);
  lib.setFlag(fake, BlockFlags::FakeUser, true);
  test_assert(lib.markDeleted(hist).ok());

  auto id = [&lib](BlockHandle h) { return lib.get(h)->id(); };
  test_assert(lib.writeRef(thing(lib, root)->owner, id(kept)).ok());
  test_assert(lib.writeRef(thing(lib, kept)->material, id(chained)).ok());
  test_assert(lib.writeRef(thing(lib, root)->parent, id(weakOnly)).ok());
  test_assert(lib.writeRef(thing(lib, cycleA)->owner, id(cycleB)).ok());
  test_assert(lib.writeRef(thing(lib, cycleB)->owner, id(cycleA)).ok());
  /* A slot ref keeps its target too. */
  BlockHandle bySlot = make(lib, "bySlot");
  test_assert(lib.setSlot(root, kThing, 0, id(bySlot), RefKind::OptStrong).ok());

  test_assert(lib.usersOf(kept).size() == 1 && lib.usersOf(kept)[0] == root);
  Result<int> n = lib.collect();
  test_assert(n.ok() && *n == 3); /* weakOnly and the unrooted cycle */
  test_assert(lib.get(weakOnly) == nullptr && lib.get(cycleA) == nullptr);
  test_assert(lib.get(kept) && lib.get(chained) && lib.get(pinned) && lib.get(fake));
  test_assert(lib.get(hist) && lib.get(bySlot));
  test_assert(strstr(g_log.c_str(), "lost root->weakOnly;") != nullptr);
  /* The weak ref keeps its id and resolves to null. */
  test_assert(lib.resolveRef(thing(lib, root)->parent).isNull());

  lib.unpin(pinned);
  lib.setFlag(fake, BlockFlags::FakeUser, false);
  n = lib.collect();
  test_assert(n.ok() && *n == 2);

  lib.beginNoCollect();
  test_assert(lib.collect().status().code() == StatusCode::Busy);
  lib.endNoCollect();
  test_assert(lib.validate().ok());
}

static void test_reentrant_destroy()
{
  g_log.clear();
  Runtime rt;
  setup(rt);
  Library lib(&rt);
  g_lib = &lib;
  g_reenter = true;
  make(lib, "doomed");
  BlockHandle survivor = make(lib, "survivor");
  lib.setFlag(survivor, BlockFlags::Root, true);
  test_assert(*lib.collect() == 1);
  test_assert(g_log == "destroy doomed;read ok;create Busy;");
  test_assert(lib.get(survivor)->hasFlag(BlockFlags::FakeUser)); /* applied after */
  g_reenter = false;
  g_lib = nullptr;
}

static void test_relations_audit()
{
  Runtime rt;
  setup(rt);
  Library lib(&rt);
  lib.setAuditRelations(true);
  BlockHandle a = make(lib, "a");
  BlockHandle b = make(lib, "b");
  lib.setFlag(a, BlockFlags::Root, true);
  test_assert(lib.usersOf(b).size() == 0); /* caches the relations */
  /* A client writes a ref without telling the library. */
  thing(lib, a)->material.target = lib.get(b)->id();
  Result<int> n = lib.collect();
  test_assert(n.status().code() == StatusCode::Internal && hasMsg(n.status(), "stale"));
  test_assert(lib.get(b) != nullptr); /* nothing was swept */
  lib.noteRefsChanged();
  test_assert(lib.usersOf(b).size() == 1);
  test_assert(lib.collect().ok() && lib.get(b) != nullptr);
}

static void test_placeholders_and_slots()
{
  Runtime rt;
  setup(rt);
  Library lib(&rt);
  BlockHandle target = make(lib, "target");
  litestl::util::Vector<uint8_t> payload;
  payload.append(1);
  payload.append(2);
  const BlockId phId{ClientId::of("gone"), LocalId{9, 9}};
  Result<BlockHandle> ph = lib.addPlaceholder(phId, "gone.Widget", "widget", payload);
  test_assert(ph.ok());
  const Datablock *p = lib.get(*ph);
  test_assert(p->state() == BlockState::Placeholder);
  test_assert(strcmp(p->placeholderType().c_str(), "gone.Widget") == 0);
  test_assert(p->placeholderPayload().size() == 2 && lib.data(*ph) == nullptr);
  /* The placeholder's ref keeps the target alive, until it is force-unlinked. */
  test_assert(
      lib.setSlot(
             *ph, TypeId::of("gone.Widget"), 4, lib.get(target)->id(), RefKind::Strong)
          .ok());
  test_assert(lib.slot(*ph, TypeId::of("gone.Widget"), 4) != nullptr);
  test_assert(lib.slot(*ph, TypeId::of("other.Ns"), 4) == nullptr); /* namespaced */
  test_assert(*lib.collect() == 0);
  test_assert(lib.removeSlot(*ph, TypeId::of("gone.Widget"), 4).ok());
  test_assert(lib.removeSlot(*ph, TypeId::of("gone.Widget"), 4).code() ==
              StatusCode::NotFound);
  test_assert(*lib.collect() == 1 && lib.get(target) == nullptr);
  test_assert(lib.get(*ph) != nullptr); /* placeholders are roots */
}

static void test_client_aliases()
{
  Runtime rt;
  ClientRegistry &clients = rt.ext<ClientRegistry>();
  const char *aliases[] = {"acme.old"};
  Result<ClientId> c = clients.registerClient("acme.new", aliases);
  test_assert(c.ok());
  test_assert(clients.canonical(ClientId::of("acme.old")) == *c);
  test_assert(clients.isRegistered(ClientId::of("acme.old")));
  const char *taken[] = {"acme.old"};
  test_assert(clients.registerClient("other", taken).status().code() ==
              StatusCode::AlreadyExists);
  test_assert(clients.registerClient("acme.old").status().code() ==
              StatusCode::AlreadyExists);
  test_assert(clients.registerClient("acme.new", aliases).ok()); /* idempotent */

  setup(rt);
  BlockTypeInfo info;
  info.name = "acme.Part";
  info.client = ClientId::of("acme.old"); /* normalized on registration */
  test_assert(
      (registerImpl<BlockProvider, ThingProvider>(rt, "acme.Part#provider").ok()));
  Result<Owned<BlockProvider>> p = own<BlockProvider>(
      rt, "acme.Part#provider", litestl::alloc::New<ThingProvider>("provider"));
  test_assert(rt.ext<BlockTypeRegistry>().add(info, std::move(p).value()).ok());
  test_assert(rt.ext<BlockTypeRegistry>().find(TypeId::of("acme.Part"))->info.client ==
              *c);

  Library lib(&rt);
  /* An id saved under the old name resolves to the block. */
  const BlockId oldId{ClientId::of("acme.old"), LocalId{5, 5}};
  Result<BlockHandle> h = lib.createWithId(
      TypeId::of("acme.Part"), oldId, "part", litestl::alloc::New<Thing>("T"));
  test_assert(h.ok() && lib.get(*h)->id().client == *c);
  test_assert(lib.find(oldId) == *h);
  RefCell cell(RefKind::Weak);
  cell.target = oldId;
  test_assert(lib.resolveRef(cell) == *h);
  test_assert(rt.validateAll().ok());
}

static void test_query_order()
{
  Runtime rt;
  setup(rt);
  test_assert((registerImpl<Valued, ThingValue>(rt, "test.Thing").ok()));
  Library lib(&rt);
  BlockHandle h = make(lib, "valued", 5);
  test_assert(lib.query<Valued>(h).value() == 5);

  Constant over{99};
  test_assert((registerImpl<Valued, Constant>(rt, "test.Constant").ok()));
  Valued overRef = query<Valued>(rt, TypeId::of("test.Constant"), &over);
  test_assert(lib.setOverride(h, InterfaceId::of("test.Valued"), overRef).ok());
  test_assert(lib.get(h)->hasFlag(BlockFlags::HasOverrides));
  test_assert(lib.query<Valued>(h).value() == 99);
  lib.clearOverride(h, InterfaceId::of("test.Valued"));
  test_assert(!lib.get(h)->hasFlag(BlockFlags::HasOverrides));
  test_assert(lib.query<Valued>(h).value() == 5);
  test_assert(lib.setOverride(h, InterfaceId::of("test.Other"), overRef).code() ==
              StatusCode::TypeMismatch);
  test_assert(lib.markDeleted(h).ok());
  test_assert(!lib.query<Valued>(h)); /* not Live */
  test_assert(lib.validate().ok());
}

/* ---- a foreign provider through a fake dispatcher ---- */

struct FakeHost : ForeignDispatcher {
  bool failForeach = false;
  int destroyed = 0;
  Status call(const IfaceTable &table, void *, int slot, void **args, void *ret) override
  {
    const char *name = table.ifaceDesc->methods[slot]->name.c_str();
    const Datablock &b = *static_cast<const Datablock *>(args[0]);
    if (strcmp(name, "data") == 0) {
      new (ret)(void *)(b.clientData());
    } else if (strcmp(name, "foreachRef") == 0) {
      if (failForeach) {
        return Status::error(StatusCode::Internal, "TypeError: foreachRef threw");
      }
      RefVisitor &v = *static_cast<RefVisitor *>(args[1]);
      if (Thing *t = static_cast<Thing *>(b.clientData())) {
        v.visit(t->owner);
      }
    } else if (strcmp(name, "destroy") == 0) {
      destroyed++;
      litestl::alloc::Delete(static_cast<Thing *>(b.clientData()));
    }
    return Status();
  }
};

static void test_foreign_provider()
{
  Runtime rt;
  FakeHost host;
  const char *slots[] = {"data", "foreachRef", "destroy"};
  test_assert(
      registerForeign<BlockProvider>(rt, "test.Js#provider", &host, nullptr, slots).ok());
  int jsObject = 1;
  BlockProvider p = query<BlockProvider>(rt, TypeId::of("test.Js#provider"), &jsObject);
  test_assert(bool(p) && !p.hasQueryInterface());
  BlockTypeInfo info;
  info.name = "test.Js";
  info.client = ClientId::of(kCoredccClient);
  test_assert(
      rt.ext<BlockTypeRegistry>().add(info, Owned<BlockProvider>(p, nullptr)).ok());
  const TypeId js = TypeId::of("test.Js");

  {
    Library lib(&rt);
    Result<BlockHandle> a = lib.create(js, "a", litestl::alloc::New<Thing>("T"));
    Result<BlockHandle> b = lib.create(js, "b", litestl::alloc::New<Thing>("T"));
    test_assert(a.ok() && b.ok());
    lib.setFlag(*a, BlockFlags::Root, true);
    test_assert(lib.writeRef(thing(lib, *a)->owner, lib.get(*b)->id()).ok());
    test_assert(lib.data(*a) == lib.get(*a)->clientData());
    test_assert(lib.usersOf(*b).size() == 1);

    /* A foreachRef that throws aborts the collection before anything is destroyed. */
    host.failForeach = true;
    lib.noteRefsChanged();
    Result<int> n = lib.collect();
    test_assert(!n.ok() && hasMsg(n.status(), "TypeError"));
    test_assert(!rt.hasStickyError()); /* collect() took and returned it */
    test_assert(host.destroyed == 0 && lib.size() == 2);
    host.failForeach = false;
    test_assert(*lib.collect() == 0);
    test_assert(lib.markDeleted(*a).ok() && lib.restoreFromHistory(*a).ok());
  }
  test_assert(host.destroyed == 2); /* the Library's destructor destroys everything */
}

/* Review findings for lib/api (2026-11-21). */
static void test_review_fixes()
{
  {
    /* B3: a Detached block's strong targets survive GC until the host restores it. */
    Runtime rt;
    setup(rt);
    Library lib(&rt);
    BlockHandle actor = make(lib, "actor");
    BlockHandle mesh = make(lib, "mesh");
    lib.setFlag(actor, BlockFlags::Root, true);
    test_assert(lib.writeRef(thing(lib, actor)->owner, lib.get(mesh)->id()).ok());
    const BlockId actorId = lib.get(actor)->id();
    test_assert(lib.onHostDestroyed(actorId).ok());
    test_assert(lib.usersOf(mesh).size() == 1);
    test_assert(lib.collect().ok() && lib.get(mesh));
    test_assert(lib.onHostRestored(actorId).ok());
    test_assert(lib.resolveRef(thing(lib, actor)->owner) == mesh);
  }
  {
    /* B4: a placeholder saved under a name that later becomes an alias. */
    Runtime rt;
    setup(rt);
    Library lib(&rt);
    const BlockId oldId{ClientId::of("acme.old"), LocalId{3, 4}};
    Result<BlockHandle> ph = lib.addPlaceholder(oldId, "acme.Gadget", "gadget", {});
    test_assert(ph.ok() && lib.find(oldId) == *ph);
    const char *aliases[] = {"acme.old"};
    test_assert(rt.ext<ClientRegistry>().registerClient("acme.new", aliases).ok());
    test_assert(lib.find(oldId) == *ph);
    test_assert(lib.find(BlockId{ClientId::of("acme.new"), oldId.local}) == *ph);
    test_assert(lib.get(*ph)->id().client == ClientId::of("acme.new"));
    test_assert(lib.validate().ok());
  }
  {
    /* B8: a GUID source that repeats itself can't corrupt the index. */
    Runtime rt;
    setup(rt);
    Library lib(&rt);
    rt.setGuidSource([]() { return LocalId{0, 77}; });
    BlockHandle a = make(lib, "a");
    Result<BlockHandle> b = lib.create(kThing, "b", nullptr);
    test_assert(b.status().code() == StatusCode::Internal);
    test_assert(lib.find(lib.get(a)->id()) == a && lib.validate().ok());
  }
  {
    /* B9: destroyFromHistory refuses while a Live block holds a Strong ref. */
    Runtime rt;
    setup(rt);
    Library lib(&rt);
    BlockHandle user = make(lib, "user");
    BlockHandle t = make(lib, "t");
    test_assert(lib.writeRef(thing(lib, user)->owner, lib.get(t)->id()).ok());
    test_assert(lib.markDeleted(t).ok());
    Status s = lib.destroyFromHistory(t);
    test_assert(s.code() == StatusCode::FailedPrecondition &&
                hasMsg(s, "'user' still holds"));
    test_assert(lib.get(t) && lib.markDeleted(user).ok() &&
                lib.destroyFromHistory(t).ok());
    test_assert(!lib.get(t) && lib.validate().ok());
  }
  {
    /* B11: library calls from a provider during GC's mark phase are Busy. */
    Runtime rt;
    setup(rt);
    Library lib(&rt);
    BlockHandle root = make(lib, "root");
    lib.setFlag(root, BlockFlags::Root, true);
    g_lib = &lib;
    g_log.clear();
    g_createInMark = true;
    test_assert(lib.collect().ok());
    test_assert(g_log == "mark-create Busy;");
    g_lib = nullptr;
  }
}

int main()
{
  test_review_fixes();
  test_create_and_find();
  test_ref_kinds_and_states();
  test_cross_library_aliasing();
  test_revive();
  test_gc();
  test_reentrant_destroy();
  test_relations_audit();
  test_placeholders_and_slots();
  test_client_aliases();
  test_query_order();
  test_foreign_provider();
  return test_end();
}
