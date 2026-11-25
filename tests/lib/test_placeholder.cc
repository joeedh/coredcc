/* Placeholders, deleting blocks in any state, rename, the listing calls, the id index. */
#include "coredcc/lib/attachment.h"
#include "coredcc/lib/pin.h"

#include "thing_fixture.h"

#include <chrono>

test_init;

using namespace coredcc;
using namespace coredcc_test;

namespace {

struct World {
  ThingHost host;
  Runtime rt;
  Library lib{&rt};
  World()
  {
    registerThing(rt, host);
    lib.setQuarantine(true);
  }
  BlockId id(BlockHandle h) const
  {
    return lib.get(h)->id();
  }
  BlockState state(BlockHandle h) const
  {
    return lib.get(h)->state();
  }
  BlockHandle missingType(const char *name, BlockId ref = BlockId(), RefKind kind = RefKind::Strong)
  {
    PlaceholderInfo info;
    info.typeName = "gone.Widget";
    info.name = name;
    info.sourceName = "old.cdcc";
    info.payload.version = 7;
    info.payload.bytes.append(uint8_t(5));
    if (!ref.isNull()) {
      RefCell cell(kind);
      cell.target = ref;
      info.payload.refs.append(cell);
    }
    Result<BlockHandle> h = lib.addPlaceholder(newId("gone"), std::move(info));
    test_assert(h.ok());
    return h.ok() ? *h : BlockHandle();
  }
  BlockHandle missingBlock(BlockId id, const char *type = "", BlockId ref = BlockId())
  {
    PlaceholderInfo info;
    info.reason = PlaceholderReason::MissingBlock;
    info.typeName = type;
    info.name = "lost";
    if (!ref.isNull()) {
      RefCell cell(RefKind::Strong);
      cell.target = ref;
      info.payload.refs.append(cell);
    }
    Result<BlockHandle> h = lib.addPlaceholder(id, std::move(info));
    test_assert(h.ok());
    return h.ok() ? *h : BlockHandle();
  }
  BlockId newId(const char *client = kCoredccClient)
  {
    return BlockId{ClientId::of(client), rt.newLocalId()};
  }
  bool valid() const
  {
    return lib.validate().ok() && rt.validateAll().ok() && !rt.hasStickyError();
  }
};

} // namespace

static void test_delete_any_state()
{
  World w;
  const BlockHandle target = makeThing(w.lib, "target");
  const BlockHandle ph = w.missingType("ph", w.id(target));
  test_assert(w.lib.get(ph)->dataState() == BlockState::Placeholder);
  test_assert(w.lib.markDeleted(ph).ok() && w.state(ph) == BlockState::InHistory);
  test_assert(w.lib.get(ph)->dataState() == BlockState::Placeholder);
  test_assert(w.lib.markDeleted(ph).code() == StatusCode::FailedPrecondition);
  test_assert(!LibraryPin(w.lib, ph).dataAlive());
  /* Its refs still count while the history holds it. */
  test_assert(w.lib.usersOf(target).size() == 1 && *w.lib.collect() == 0);
  test_assert(w.lib.restoreFromHistory(ph).ok() && w.state(ph) == BlockState::Placeholder);
  test_assert(w.lib.get(ph)->placeholderPayload().size() == 1);

  /* A Placeholder user keeps a strong target from leaving the history. */
  test_assert(w.lib.markDeleted(target).ok());
  test_assert(hasMsg(w.lib.destroyFromHistory(target), "'ph' still holds"));
  test_assert(w.lib.markDeleted(ph).ok() && w.lib.destroyFromHistory(ph).ok());
  test_assert(!w.lib.get(ph) && w.host.log.empty()); /* no provider was called for it */
  test_assert(w.lib.destroyFromHistory(target).ok() && w.valid());

  /* Nor for one whose type is registered here. */
  PlaceholderInfo known;
  known.typeName = "test.Thing";
  Result<BlockHandle> ph0 = w.lib.addPlaceholder(w.newId(), known);
  test_assert(ph0.ok() && w.lib.markDeleted(*ph0).ok() && w.lib.destroyFromHistory(*ph0).ok());
  test_assert(!w.lib.get(*ph0) && w.host.log.find("destroy ;") == std::string::npos);

  /* Detached blocks, and the host changing its mind while they are deleted. */
  const BlockHandle d = makeThing(w.lib, "d");
  LibraryPin pin(w.lib, d, PinKind::History);
  test_assert(w.lib.onHostDestroyed(w.id(d)).ok() && w.lib.markDeleted(d).ok());
  test_assert(w.lib.get(d)->dataState() == BlockState::Detached && !pin.dataAlive());
  test_assert(w.lib.onHostDestroyed(w.id(d)).code() == StatusCode::FailedPrecondition);
  test_assert(w.lib.restoreFromHistory(d).ok() && w.state(d) == BlockState::Detached);
  test_assert(w.lib.markDeleted(d).ok());
  uint64_t changes = w.lib.get(d)->stateChanges();
  test_assert(w.lib.onHostRestored(w.id(d)).ok() && w.state(d) == BlockState::InHistory);
  test_assert(w.lib.get(d)->dataState() == BlockState::Live && pin.dataAlive());
  test_assert(w.lib.get(d)->stateChanges() > changes);
  test_assert(w.lib.onHostDestroyed(w.id(d)).ok() && !pin.dataAlive());
  test_assert(w.lib.onHostRestored(w.id(d)).ok());
  test_assert(w.lib.restoreFromHistory(d).ok() && w.state(d) == BlockState::Live);

  /* A placeholder has no host. */
  const BlockHandle ph2 = w.missingType("ph2");
  test_assert(w.lib.onHostDestroyed(w.id(ph2)).code() == StatusCode::FailedPrecondition);
  test_assert(w.lib.markDeleted(ph2).ok());
  test_assert(w.lib.onHostRestored(w.id(ph2)).code() == StatusCode::FailedPrecondition);

  /* A Doomed placeholder still has its refs, and no provider destroy. */
  const BlockHandle t2 = makeThing(w.lib, "t2");
  const BlockHandle ph3 = w.missingType("ph3", w.id(t2));
  w.lib.pin(ph3);
  test_assert(w.lib.markDeleted(ph3).ok() && w.lib.destroyFromHistory(ph3).ok());
  test_assert(w.state(ph3) == BlockState::Doomed && *w.lib.collect() >= 0 && w.lib.get(t2));
  w.host.log.clear();
  w.lib.unpin(ph3);
  test_assert(!w.lib.get(ph3) && w.host.log.empty() && w.valid());
}

static void test_placeholder_refs()
{
  World w;
  const BlockHandle a = makeThing(w.lib, "a"), b = makeThing(w.lib, "b");
  const BlockHandle ph = w.missingType("ph", w.id(a));
  const Datablock *p = w.lib.get(ph);
  test_assert(p->placeholderReason() == PlaceholderReason::MissingType &&
              p->placeholderData().version == 7 && p->placeholderData().refs.size() == 1);
  test_assert(strcmp(p->sourceName().c_str(), "old.cdcc") == 0);
  test_assert(w.lib.usersOf(a).size() == 1);
  int seen = 0;
  test_assert(w.lib.foreachDataRef(ph, [&](RefCell &) { seen++; }).ok() && seen == 1);
  test_assert(*w.lib.collect() == 1 && w.lib.get(a) && !w.lib.get(b)); /* keeps its target */

  uint64_t epoch = w.lib.epoch();
  test_assert(w.lib.unlinkRef(ph, 1).code() == StatusCode::OutOfRange);
  test_assert(w.lib.unlinkRef(ph, -1).code() == StatusCode::OutOfRange);
  test_assert(w.lib.unlinkRef(a, 0).code() == StatusCode::FailedPrecondition);
  test_assert(w.lib.unlinkRef(BlockHandle(), 0).code() == StatusCode::NotFound);
  test_assert(w.lib.epoch() == epoch);
  test_assert(w.lib.unlinkRef(ph, 0).ok() && w.lib.epoch() > epoch);
  test_assert(p->placeholderData().refs.size() == 1 && p->placeholderData().refs[0].isNull());
  test_assert(p->placeholderData().refs[0].kind == RefKind::Strong);
  test_assert(w.lib.usersOf(a).size() == 0);
  test_assert(w.lib.relinkRef(ph, 0, w.id(a)).ok() && w.lib.usersOf(a).size() == 1);
  test_assert(w.lib.unlinkRef(ph, 0).ok() && *w.lib.collect() == 1 && w.valid());

  PlaceholderInfo none;
  test_assert(w.lib.addPlaceholder(w.newId(), none).status().code() ==
              StatusCode::InvalidArgument); /* MissingType without a type */
  none.typeName = "x";
  test_assert(w.lib.addPlaceholder(w.id(ph), none).status().code() == StatusCode::AlreadyExists);
}

static void test_missing_blocks()
{
  World w;
  /* Never a record: kept only by what references it. */
  const BlockId lostId = w.newId();
  const BlockHandle user = makeThing(w.lib, "user");
  w.lib.setFlag(user, BlockFlags::Root, true);
  test_assert(w.lib.writeRef(thing(w.lib, user)->owner, lostId).ok());
  const BlockHandle lost = w.missingBlock(lostId);
  test_assert(w.state(lost) == BlockState::Placeholder && *w.lib.collect() == 0);
  test_assert(w.lib.usersOf(lost).size() == 1 && !w.lib.resolveRef(thing(w.lib, user)->owner));
  const BlockHandle unused = w.missingBlock(w.newId());
  test_assert(*w.lib.collect() == 1 && !w.lib.get(unused));
  /* A record: a root, and it keeps what it referenced. */
  const BlockHandle kept = makeThing(w.lib, "kept");
  const BlockHandle record = w.missingBlock(w.newId(), "test.Thing", w.id(kept));
  test_assert(*w.lib.collect() == 0 && w.lib.get(record) && w.lib.get(kept));
  /* Pins and deletion hold the other kind too. */
  const BlockHandle pinned = w.missingBlock(w.newId());
  w.lib.pin(pinned);
  const BlockHandle deleted = w.missingBlock(w.newId());
  test_assert(w.lib.markDeleted(deleted).ok());
  test_assert(*w.lib.collect() == 0 && w.lib.get(pinned) && w.lib.get(deleted));
  test_assert(w.valid());
}

static void test_adoption()
{
  World w;
  const TypeId att = TypeId::of("x.Slots");
  const BlockHandle kept = makeThing(w.lib, "kept"), user = makeThing(w.lib, "user");
  const BlockId id = w.newId();
  test_assert(w.lib.writeRef(thing(w.lib, user)->owner, id).ok());
  const BlockHandle lost = w.missingBlock(id, "test.Thing", w.id(kept));
  w.lib.pin(lost);
  w.lib.setFlag(lost, BlockFlags::FakeUser, true);
  test_assert(w.lib.setSlot(lost, att, 3, w.id(kept), RefKind::Weak).ok());
  const uint64_t epoch = w.lib.epoch(), changes = w.lib.get(lost)->stateChanges();

  Thing *t = litestl::alloc::New<Thing>("Thing");
  t->value = 4;
  Result<BlockHandle> h = w.lib.createWithId(kThing, id, "found", t);
  test_assert(h.ok() && *h == lost && w.state(lost) == BlockState::Live);
  const Datablock *b = w.lib.get(lost);
  test_assert(strcmp(b->name().c_str(), "found") == 0 && b->clientData() == t && b->type() == kThing);
  test_assert(b->pins(PinKind::Host) == 1 && b->hasFlag(BlockFlags::FakeUser));
  test_assert(w.lib.slot(lost, att, 3) != nullptr);
  test_assert(b->placeholderData().refs.size() == 0 && b->placeholderType().size() == 0);
  test_assert(w.lib.epoch() > epoch && b->stateChanges() > changes);
  test_assert(w.lib.resolveRef(thing(w.lib, user)->owner) == lost);
  w.lib.removeSlot(lost, att, 3).ignore();
  test_assert(w.lib.usersOf(kept).size() == 0); /* the ref table is gone */
  test_assert(w.lib.createWithId(kThing, id, "again", nullptr).status().code() ==
              StatusCode::AlreadyExists);

  /* reviveAs, and a record that had no type. */
  const BlockId id2 = w.newId();
  const BlockHandle lost2 = w.missingBlock(id2);
  Result<BlockHandle> r = w.lib.reviveAs(kThing, id2, "r", nullptr);
  test_assert(r.ok() && *r == lost2 && w.lib.get(lost2)->type() == kThing);

  /* Another type than the stored one; MissingType; a deleted MissingBlock: refused. */
  const BlockId id3 = w.newId();
  const BlockHandle lost3 = w.missingBlock(id3, "test.Other");
  test_assert(hasMsg(w.lib.createWithId(kThing, id3, "x", nullptr).status(), "'test.Other'"));
  test_assert(w.state(lost3) == BlockState::Placeholder);
  PlaceholderInfo info;
  info.typeName = "test.Thing";
  const BlockId id4 = w.newId();
  test_assert(w.lib.addPlaceholder(id4, info).ok());
  test_assert(w.lib.createWithId(kThing, id4, "x", nullptr).status().code() ==
              StatusCode::AlreadyExists);
  test_assert(w.lib.reviveAs(kThing, id4, "x", nullptr).status().code() ==
              StatusCode::FailedPrecondition);
  const BlockId id5 = w.newId();
  test_assert(w.lib.markDeleted(w.missingBlock(id5)).ok());
  test_assert(w.lib.createWithId(kThing, id5, "x", nullptr).status().code() ==
              StatusCode::AlreadyExists);
  test_assert(w.lib.reviveAs(kThing, id5, "x", nullptr).status().code() ==
              StatusCode::FailedPrecondition);
  test_assert(w.valid());
}

static void test_rename_and_listing()
{
  World w;
  const BlockHandle a = makeThing(w.lib, "a"), b = makeThing(w.lib, "b");
  const uint64_t epoch = w.lib.epoch();
  test_assert(w.lib.rename(a, "b").ok() && w.lib.epoch() > epoch); /* not unique */
  test_assert(strcmp(w.lib.get(a)->name().c_str(), "b") == 0);
  test_assert(w.lib.rename(BlockHandle(), "x").code() == StatusCode::NotFound);

  const TypeId ns1 = TypeId::of("ns.1"), ns2 = TypeId::of("ns.2");
  test_assert(w.lib.setSlot(a, ns1, 4, w.id(b), RefKind::Weak).ok());
  test_assert(w.lib.setSlot(a, ns2, 5, w.id(b), RefKind::Strong).ok());
  std::string log;
  w.lib.forEachSlot(a, [&](TypeId ns, int64_t key, const RefCell &cell) {
    log += (ns == ns1 ? "1:" : "2:") + std::to_string(key) +
           (cell.target == w.id(b) && cell.kind == RefKind::Strong ? "s;" : "w;");
  });
  test_assert(log == "1:4w;2:5s;");
  w.lib.forEachSlot(BlockHandle(), [&](TypeId, int64_t, const RefCell &) { log.clear(); });
  w.lib.forEachSlot(b, [&](TypeId, int64_t, const RefCell &) { log.clear(); });
  test_assert(!log.empty());

  const InterfaceId iface = InterfaceId::of(BlockProvider::ifaceName);
  const BlockProvider p = w.rt.ext<BlockTypeRegistry>().provider(kThing);
  test_assert(w.lib.setOverride(a, iface, p).ok());
  int overrides = 0, attachments = 0;
  w.lib.forEachOverride(a, [&](InterfaceId i, const IfaceRef &r) {
    overrides += i == iface && r.self() == p.self() ? 1 : 0;
  });
  w.lib.forEachOverride(b, [&](InterfaceId, const IfaceRef &) { overrides += 10; });
  w.lib.forEachAttachment(a, [&](TypeId, void *) { attachments++; });
  test_assert(overrides == 1 && attachments == 0 && w.valid());
}

/* Ids a file could choose so that BlockId's own hash is the same for all of them. */
static void test_colliding_ids()
{
  const auto build = [](bool collide, int n) {
    World w;
    w.lib.setAuditRelations(false);
    const auto start = std::chrono::steady_clock::now();
    BlockHandle first;
    for (int i = 1; i <= n; i++) {
      LocalId local{uint64_t(i), uint64_t(i)};
      if (collide) {
        local.lo = local.hi * 0x9E3779B97F4A7C15ull ^ 42;
        test_assert(LocalId(local).computeHash() == 42);
      }
      Thing *t = litestl::alloc::New<Thing>("Thing");
      t->parent.target = BlockId{ClientId::of(kCoredccClient), local}; /* the relations map */
      Result<BlockHandle> h =
          w.lib.createWithId(kThing, BlockId{ClientId::of(kCoredccClient), local}, "x", t);
      test_assert(h.ok());
      first = i == 1 && h.ok() ? *h : first;
    }
    test_assert(w.lib.usersOf(first).size() == 1);
    w.lib.setFlag(first, BlockFlags::Root, true);
    test_assert(*w.lib.collect() == n - 1);
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  };
  const int n = 20000;
  const double plain = build(false, n), hostile = build(true, n);
  test_assert(hostile < plain * 5 + 0.5);

  /* Keys depend on the seed, and on every part of the id. */
  const BlockId id{ClientId{1}, LocalId{2, 3}};
  const BlockIdHasher h1(LocalId{1, 1}), h2(LocalId{1, 2});
  test_assert(h1.key(id).hash == h1.key(id).hash && h1.key(id).hash != h2.key(id).hash);
  test_assert(h1.key(id).hash != h1.key(BlockId{ClientId{2}, LocalId{2, 3}}).hash);
  test_assert(h1.key(id).hash != h1.key(BlockId{ClientId{1}, LocalId{3, 3}}).hash);
  test_assert(h1.key(id).hash != h1.key(BlockId{ClientId{1}, LocalId{2, 4}}).hash);
}

int main()
{
  test_delete_any_state();
  test_placeholder_refs();
  test_missing_blocks();
  test_adoption();
  test_rename_and_listing();
  test_colliding_ids();
  return test_end();
}
