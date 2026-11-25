/* Pin kinds, Doomed blocks, LibraryRef and LibraryPin. */
#include "coredcc/lib/pin.h"

#include "thing_fixture.h"

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
  }
  BlockState state(BlockHandle h) const
  {
    return lib.get(h)->state();
  }
  /** Deletes `h` and drops the delete from the history. */
  Status drop(BlockHandle h)
  {
    test_assert(lib.markDeleted(h).ok());
    return lib.destroyFromHistory(h);
  }
  bool valid() const
  {
    return lib.validate().ok() && rt.validateAll().ok() && !rt.hasStickyError();
  }
};

} // namespace

static void test_pin_kinds()
{
  World w;
  const BlockHandle a = makeThing(w.lib, "a");
  w.lib.pin(a, PinKind::History);
  w.lib.pin(a);
  w.lib.pin(a);
  test_assert(w.lib.get(a)->pins() == 3 && w.lib.get(a)->pins(PinKind::Host) == 2 &&
              w.lib.get(a)->pins(PinKind::History) == 1);
  w.lib.unpin(a);
  w.lib.unpin(a);
  test_assert(w.lib.get(a)->pins() == 1 && *w.lib.collect() == 0); /* a History pin is a root */
  w.lib.unpin(a, PinKind::History);
  test_assert(*w.lib.collect() == 1 && !w.lib.get(a));
  w.lib.unpin(a); /* a stale handle: ignored */
  test_assert(w.valid());
}

static void test_doomed_block()
{
  World w;
  const BlockHandle a = makeThing(w.lib, "a", 5);
  const BlockHandle user = makeThing(w.lib, "weak");
  const BlockId id = w.lib.get(a)->id();
  test_assert(w.lib.writeRef(thing(w.lib, user)->parent, id).ok());
  w.lib.setFlag(user, BlockFlags::Root, true);
  w.lib.pin(a);
  test_assert(w.drop(a).ok());

  /* The data stays; the id is free, and nothing lists the block. */
  test_assert(w.state(a) == BlockState::Doomed && thing(w.lib, a)->value == 5);
  test_assert(w.host.log == "lost weak->a;"); /* told once, now, not when it goes */
  test_assert(!w.lib.find(id) && w.lib.size() == 1 && !w.lib.data(a));
  int listed = 0;
  w.lib.forEach([&](BlockHandle h, const Datablock &) { listed += h == a ? 1 : 0; });
  test_assert(listed == 0 && w.valid());
  test_assert(w.lib.destroyFromHistory(a).code() == StatusCode::FailedPrecondition);
  test_assert(w.lib.restoreFromHistory(a).code() == StatusCode::FailedPrecondition);
  test_assert(*w.lib.collect() == 0 && w.lib.get(a));

  /* Another block takes the id; refs find it, also through a cached handle. */
  thing(w.lib, user)->parent.cache = a;
  Result<BlockHandle> b = w.lib.reviveAs(kThing, id, "b", litestl::alloc::New<Thing>("Thing"));
  test_assert(b.ok() && w.lib.find(id) == *b);
  test_assert(w.lib.resolveRef(thing(w.lib, user)->parent) == *b);
  test_assert(w.valid());

  /* Gone with its last Host pin, leaving the new block's index entry alone. */
  w.lib.pin(a);
  w.lib.unpin(a);
  test_assert(w.lib.get(a));
  w.host.log.clear();
  w.lib.unpin(a);
  test_assert(!w.lib.get(a) && w.host.log == "destroy a;");
  test_assert(w.lib.find(id) == *b && w.lib.size() == 2 && w.valid());

  /* createWithId works as reviveAs does. */
  const BlockHandle c = makeThing(w.lib, "c");
  const BlockId cid = w.lib.get(c)->id();
  w.lib.pin(c);
  test_assert(w.drop(c).ok());
  test_assert(w.lib.createWithId(kThing, cid, "c2", nullptr).ok());
  w.lib.unpin(c);
  test_assert(!w.lib.get(c) && w.valid());
}

static void test_history_pins_do_not_doom()
{
  World w;
  const BlockHandle a = makeThing(w.lib, "a");
  w.lib.pin(a, PinKind::History);
  test_assert(w.drop(a).ok() && !w.lib.get(a) && w.host.log == "destroy a;");

  /* A History pin on a Doomed block doesn't hold it either. */
  const BlockHandle b = makeThing(w.lib, "b");
  w.lib.pin(b);
  w.lib.pin(b, PinKind::History);
  test_assert(w.drop(b).ok() && w.state(b) == BlockState::Doomed);
  w.lib.unpin(b);
  test_assert(!w.lib.get(b) && w.valid());
}

static void test_doomed_keeps_targets()
{
  World w;
  const BlockHandle a = makeThing(w.lib, "a");
  const BlockHandle mesh = makeThing(w.lib, "mesh");
  test_assert(w.lib.writeRef(thing(w.lib, a)->owner, w.lib.get(mesh)->id()).ok());
  w.lib.pin(a);
  test_assert(w.drop(a).ok());
  test_assert(*w.lib.collect() == 0 && w.lib.get(mesh));
  test_assert(w.lib.usersOf(mesh).size() == 0); /* not a user any more */
  w.lib.unpin(a);
  test_assert(*w.lib.collect() == 1 && !w.lib.get(mesh) && w.valid());
}

static void test_release_waits()
{
  /* In a no-collect scope. */
  World w;
  const BlockHandle a = makeThing(w.lib, "a");
  const BlockHandle b = makeThing(w.lib, "b");
  w.lib.pin(a);
  w.lib.pin(b);
  test_assert(w.drop(a).ok() && w.drop(b).ok());
  w.lib.beginNoCollect();
  w.lib.beginNoCollect();
  w.lib.unpin(a);
  w.lib.unpin(b);
  w.lib.pin(b); /* pinned again before the scope ends: stays */
  w.lib.endNoCollect();
  test_assert(w.lib.get(a));
  w.lib.endNoCollect();
  test_assert(!w.lib.get(a) && w.lib.get(b));
  w.lib.unpin(b);
  test_assert(!w.lib.get(b) && w.valid());

  /* From a sweep: after it, and only once. */
  const BlockHandle c = makeThing(w.lib, "c");
  makeThing(w.lib, "garbage");
  w.lib.pin(c);
  test_assert(w.drop(c).ok());
  w.host.log.clear();
  bool busyInside = false;
  w.host.onDestroy = [&](const Datablock &block) {
    if (strcmp(block.name().c_str(), "garbage") == 0) {
      busyInside = w.lib.busy();
      w.lib.unpin(c);
      test_assert(w.lib.get(c));
    }
  };
  test_assert(*w.lib.collect() == 1);
  test_assert(busyInside && !w.lib.busy());
  test_assert(w.host.log == "destroy garbage;destroy c;" && !w.lib.get(c));
  w.host.onDestroy = {};

  /* While marking: after the collection. A failing destroy then is that collect's error. */
  for (int fail = 0; fail < 3; fail++) {
    const BlockHandle m = makeThing(w.lib, "m");
    w.lib.pin(m);
    test_assert(w.drop(m).ok());
    bool once = false, there = false;
    w.host.onForeach = [&]() {
      if (!once) {
        once = true;
        w.lib.unpin(m);
        w.host.failDestroy = fail == 1;
        w.host.failForeach = fail == 2; /* collect() gives up while marking */
        there = w.lib.get(m) != nullptr;
      }
    };
    Result<int> n = w.lib.collect();
    w.host.onForeach = {};
    w.host.failDestroy = w.host.failForeach = false;
    test_assert(once && there && !w.lib.get(m) && n.ok() == (fail == 0) && w.valid());
    w.lib.collect().status().ignore(); /* takes what was kept */
  }

  /* From the notice that dooms it. */
  const BlockHandle d = makeThing(w.lib, "d");
  const BlockHandle user = makeThing(w.lib, "user");
  test_assert(w.lib.writeRef(thing(w.lib, user)->parent, w.lib.get(d)->id()).ok());
  w.lib.pin(d);
  w.host.onLost = [&]() { w.lib.unpin(d); };
  test_assert(w.drop(d).ok());
  test_assert(!w.lib.get(d) && w.valid());
  w.host.onLost = {};

  /* A destroy that unpins another Doomed block. */
  const BlockHandle e = makeThing(w.lib, "e");
  const BlockHandle f = makeThing(w.lib, "f");
  w.lib.pin(e);
  w.lib.pin(f);
  test_assert(w.drop(e).ok() && w.drop(f).ok());
  w.host.onDestroy = [&](const Datablock &block) {
    if (strcmp(block.name().c_str(), "e") == 0) {
      w.lib.unpin(f);
    }
  };
  w.lib.unpin(e);
  test_assert(!w.lib.get(e) && !w.lib.get(f) && w.valid());
}

static void test_release_rechecks_users()
{
  for (int taken = 0; taken < 2; taken++) {
    World w;
    const BlockHandle user = makeThing(w.lib, "user");
    const BlockHandle a = makeThing(w.lib, "a", 9);
    const BlockId id = w.lib.get(a)->id();
    test_assert(w.lib.writeRef(thing(w.lib, user)->owner, id).ok());
    test_assert(w.lib.markDeleted(user).ok()); /* InHistory users don't count */
    w.lib.pin(a);
    test_assert(w.drop(a).ok() && w.state(a) == BlockState::Doomed);
    test_assert(w.lib.restoreFromHistory(user).ok());
    if (taken) {
      test_assert(w.lib.createWithId(kThing, id, "new", nullptr).ok());
    }
    w.lib.unpin(a);
    if (taken) {
      /* The ref means the new block: the old one goes. */
      test_assert(!w.lib.get(a) && w.lib.resolveRef(thing(w.lib, user)->owner) == w.lib.find(id));
    }
    else {
      /* As if destroyFromHistory had been refused. */
      test_assert(w.lib.get(a) && w.state(a) == BlockState::InHistory && w.lib.find(id) == a);
      test_assert(w.lib.destroyFromHistory(a).code() == StatusCode::FailedPrecondition);
      test_assert(w.lib.restoreFromHistory(a).ok());
      test_assert(static_cast<Thing *>(w.lib.resolveData(thing(w.lib, user)->owner))->value == 9);
    }
    test_assert(w.valid());
  }
}

static void test_deferred_errors()
{
  World w;
  BlockHandle h[20];
  for (int i = 0; i < 20; i++) {
    h[i] = makeThing(w.lib, "x");
    w.lib.pin(h[i]);
    test_assert(w.drop(h[i]).ok());
  }
  w.host.failDestroy = true;
  for (BlockHandle x : h) {
    w.lib.unpin(x);
    test_assert(!w.rt.hasStickyError());
  }
  w.host.failDestroy = false;
  makeThing(w.lib, "garbage");
  w.host.log.clear();
  Result<int> n = w.lib.collect();
  test_assert(!n.ok() && hasMsg(n.status(), "destroy threw") && hasMsg(n.status(), "19 more"));
  test_assert(w.host.log == "destroy garbage;"); /* it did its own work first */
  test_assert(*w.lib.collect() == 0 && w.valid()); /* reported once */

  /* A sticky error that was pending stays. */
  const BlockHandle a = makeThing(w.lib, "a");
  w.lib.pin(a);
  test_assert(w.drop(a).ok());
  w.rt.setStickyError(Status::error(StatusCode::Cancelled, "mine"));
  w.host.failDestroy = true;
  w.lib.unpin(a);
  w.host.failDestroy = false;
  test_assert(w.rt.takeStickyError().code() == StatusCode::Cancelled);
  test_assert(hasMsg(w.lib.collect().status(), "destroy threw"));

  /* A failing enumeration while rechecking: the same. */
  const BlockHandle b = makeThing(w.lib, "b");
  makeThing(w.lib, "other");
  w.lib.pin(b);
  test_assert(w.drop(b).ok());
  w.host.failForeach = true;
  w.lib.unpin(b);
  w.host.failForeach = false;
  test_assert(!w.rt.hasStickyError());
  test_assert(hasMsg(w.lib.collect().status(), "foreachRef threw"));
}

static void test_library_gone()
{
  ThingHost host;
  Runtime rt;
  registerThing(rt, host);
  LibraryRef ref;
  LibraryPin outer;
  test_assert(!ref.get() && !outer.library() && !outer.dataAlive() && !outer.copy().library());
  bool sawNull = false;
  {
    Library lib(&rt);
    ref = lib.ref();
    test_assert(ref.get() == &lib);
    const BlockHandle a = makeThing(lib, "a");
    outer = LibraryPin(lib, a);
    test_assert(lib.get(a)->pins(PinKind::Host) == 1 && outer.dataAlive());
    const BlockHandle doomed = makeThing(lib, "doomed");
    lib.pin(doomed);
    test_assert(lib.markDeleted(doomed).ok() && lib.destroyFromHistory(doomed).ok());
    host.onDestroy = [&](const Datablock &) {
      sawNull = ref.get() == nullptr && !outer.dataAlive();
      outer.reset(); /* a payload owned by the block's data */
    };
  }
  test_assert(sawNull && !ref.get());
  test_assert(host.log == "destroy a;destroy doomed;");
  test_assert(!rt.hasStickyError());
}

static void test_library_pin()
{
  World w;
  const BlockHandle a = makeThing(w.lib, "a");
  auto pins = [&]() { return w.lib.get(a)->pins(PinKind::Host); };
  {
    LibraryPin p(w.lib, a);
    LibraryPin q = p.copy();
    test_assert(pins() == 2 && q.handle() == a && q.library() == &w.lib);
    LibraryPin r = std::move(p);
    test_assert(pins() == 2 && !p.library() && r.library());
    q = std::move(r); /* releases what q held */
    test_assert(pins() == 1);
    q.reset();
    q.reset();
    test_assert(pins() == 0 && !q.dataAlive());
    LibraryPin h(w.lib, a, PinKind::History);
    LibraryPin h2 = h.copy();
    test_assert(pins() == 0 && w.lib.get(a)->pins(PinKind::History) == 2);
  }
  test_assert(w.lib.get(a)->pins() == 0);

  /* dataAlive in every state. */
  LibraryPin p(w.lib, a);
  const BlockId id = w.lib.get(a)->id();
  test_assert(p.dataAlive());
  test_assert(w.lib.onHostDestroyed(id).ok() && !p.dataAlive()); /* pins don't stop a host */
  test_assert(w.lib.onHostRestored(id).ok() && p.dataAlive());
  test_assert(w.drop(a).ok() && w.state(a) == BlockState::Doomed && p.dataAlive());
  Result<BlockHandle> ph = w.lib.addPlaceholder(BlockId{ClientId::of(kCoredccClient), {1, 2}},
                                                "gone.T", "ph", {});
  test_assert(ph.ok() && !LibraryPin(w.lib, *ph).dataAlive());
  p.reset();
  test_assert(!w.lib.get(a) && w.valid());
}

static void test_state_changes_and_epoch()
{
  World w;
  const BlockHandle a = makeThing(w.lib, "a");
  const BlockId id = w.lib.get(a)->id();
  uint64_t last = w.lib.get(a)->stateChanges();
  uint64_t epoch = w.lib.epoch();
  auto moved = [&]() {
    const uint64_t s = w.lib.get(a)->stateChanges(), e = w.lib.epoch();
    const bool ok = s > last && e > epoch;
    last = s;
    epoch = e;
    return ok;
  };
  test_assert(w.lib.onHostDestroyed(id).ok() && moved());
  test_assert(w.lib.onHostRestored(id).ok() && moved());
  test_assert(w.lib.markDeleted(a).ok() && moved());
  test_assert(w.lib.restoreFromHistory(a).ok() && moved());
  w.lib.pin(a);
  test_assert(w.lib.markDeleted(a).ok() && moved());
  test_assert(w.lib.destroyFromHistory(a).ok() && moved()); /* doomed */

  /* What changes query()'s answers bumps the epoch too; what fails doesn't. */
  const BlockHandle b = makeThing(w.lib, "b");
  auto bumped = [&, at = w.lib.epoch()]() mutable {
    const bool yes = w.lib.epoch() > at;
    at = w.lib.epoch();
    return yes;
  };
  int x = 0;
  const TypeId att = TypeId::of("test.Att");
  test_assert(w.lib.attach(b, att, &x, nullptr).ok() && bumped());
  test_assert(!w.lib.attach(b, att, &x, nullptr).ok() && !bumped());
  test_assert(w.lib.detach(b, att).ok() && bumped());
  test_assert(!w.lib.detach(b, att).ok() && !bumped());
  const BlockProvider p = w.rt.ext<BlockTypeRegistry>().provider(kThing);
  const InterfaceId iface = InterfaceId::of(BlockProvider::ifaceName);
  test_assert(w.lib.setOverride(b, iface, p).ok() && bumped());
  test_assert(w.lib.setOverride(b, iface, p).ok() && bumped());
  w.lib.clearOverride(b, iface);
  test_assert(bumped());
  w.lib.clearOverride(b, iface);
  test_assert(!bumped());
  w.lib.unpin(a);
  test_assert(w.valid());
}

int main()
{
  test_pin_kinds();
  test_doomed_block();
  test_history_pins_do_not_doom();
  test_doomed_keeps_targets();
  test_release_waits();
  test_release_rechecks_users();
  test_deferred_errors();
  test_library_gone();
  test_library_pin();
  test_state_changes_and_epoch();
  return test_end();
}
