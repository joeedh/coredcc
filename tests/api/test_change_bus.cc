#include "scene_fixture.h"

#include <cmath>
#include <string>

#include <chrono>
#include <memory>
#include <string>
#include <vector>

test_init;

using namespace fixture;

struct Recorder {
  int calls = 0;
  PropValue last;
  Status lastStatus;
  ChangeBus::WatchFn fn()
  {
    return [this](const PropValue &v, const Status &s) {
      calls++;
      last = v;
      lastStatus = s;
    };
  }
};

static ScopedConnection
watchOk(ChangeBus &bus, Context &ctx, const char *path, Recorder &r)
{
  Result<ScopedConnection> c = bus.watch(ctx, path, r.fn());
  test_assert(c.ok());
  return c.ok() ? std::move(c).value() : ScopedConnection();
}

static void test_aliases()
{
  World w;
  Context ctx = w.context();
  ChangeBus &bus = w.rt.ext<ChangeBus>();
  Recorder byName, byActive, other, unrelated;
  ScopedConnection a = watchOk(bus, ctx, "objects[\"Cube\"].location", byName);
  ScopedConnection b = watchOk(bus, ctx, "objects.active.location", byActive);
  ScopedConnection c = watchOk(bus, ctx, "objects[\"Sphere\"].location", other);
  ScopedConnection d = watchOk(bus, ctx, "tool.strength", unrelated);
  test_assert(bus.watcherCount() == 4 && byName.calls == 0); /* watch doesn't fire */

  test_assert(setValue(ctx, "objects.active.location[0]", PropValue::ofFloat(5)).ok());
  test_assert(bus.flush() == 2);
  test_assert(byName.calls == 1 && byActive.calls == 1 && other.calls == 0 &&
              unrelated.calls == 0);
  test_assert(byName.last == PropValue::ofVec3(5, 0, 0));

  /* No change, no call: flushing again, or writing the same value. */
  test_assert(bus.flush() == 0);
  test_assert(setValue(ctx, "objects[0].location", PropValue::ofVec3(5, 0, 0)).ok());
  test_assert(bus.flush() == 0 && byName.calls == 1);

  /* `active` moves: only the epoch says so, and only the active watcher's value changes.
   */
  w.scene.active = 1;
  bus.bumpEpoch();
  test_assert(bus.flush() == 1 && byActive.calls == 2 &&
              byActive.last == PropValue::ofVec3(0, 0, 0));
  test_assert(byName.calls == 1);

  /* Direct C++ mutation plus notify. */
  w.object(1).location[2] = 4;
  bus.notify(OwnerKey::ofBlock(w.objectId(1)), "location");
  test_assert(bus.flush() == 2 && other.calls == 1 && byActive.calls == 3);
  /* A whole-owner notify. */
  w.tool.strength = 0.25;
  bus.notify(w.rt.ext<ApiRegistry>().runtimeOwner("tool").owner);
  test_assert(bus.flush() == 1 && unrelated.last == PropValue::ofFloat(0.25));

  /* Disconnected watchers stop. */
  d.disconnect();
  w.tool.strength = 0.75;
  bus.notify(w.rt.ext<ApiRegistry>().runtimeOwner("tool").owner, "strength");
  test_assert(bus.flush() == 0 && bus.watcherCount() == 3);
  test_assert(bus.validate().ok());
}

static void test_hops_and_nan()
{
  World w;
  Context ctx = w.context();
  ChangeBus &bus = w.rt.ext<ChangeBus>();
  Recorder radius, freeVal;
  ScopedConnection a = watchOk(bus, ctx, "objects.active.data.radius", radius);
  ScopedConnection b = watchOk(bus, ctx, "tool.free", freeVal);

  /* A change on the mesh reaches a watcher that hops through `data`. */
  test_assert(setValue(ctx, "objects[\"Cube\"].data.radius", PropValue::ofFloat(3)).ok());
  test_assert(bus.flush() == 1 && radius.last == PropValue::ofFloat(3));
  /* Repointing `data` is a change on the object, which the watcher's chain touches. */
  Result<PropValue> sphereMesh = getValue(ctx, "objects[1].data");
  test_assert(setValue(ctx, "objects[0].data", *sphereMesh).ok());
  test_assert(bus.flush() == 1 && radius.last == PropValue::ofFloat(1));

  /* NaN fires once, then not again (setValue rejects NaN; C++ can still store it). */
  const OwnerKey tool = w.rt.ext<ApiRegistry>().runtimeOwner("tool").owner;
  w.tool.free = std::nan("");
  bus.notify(tool, "free");
  test_assert(bus.flush() == 1 && freeVal.calls == 1);
  bus.notify(tool, "free");
  test_assert(bus.flush() == 0 && freeVal.calls == 1);
}

static void test_failures_and_reentrancy()
{
  World w;
  Context ctx = w.context();
  ChangeBus &bus = w.rt.ext<ChangeBus>();

  /* A path that stops resolving fires once with the error, then again when it's back. */
  Recorder r;
  ScopedConnection a = watchOk(bus, ctx, "objects.active.name", r);
  w.scene.active = -1;
  bus.bumpEpoch();
  test_assert(bus.flush() == 1 && r.last.isNone() &&
              r.lastStatus.code() == StatusCode::NotFound);
  bus.bumpEpoch();
  test_assert(bus.flush() == 0);
  w.scene.active = 1;
  bus.bumpEpoch();
  test_assert(bus.flush() == 1 && r.last == PropValue::ofString("Sphere") &&
              r.lastStatus.ok());

  /* A watch on a malformed path fails at once. */
  test_assert(!bus.watch(ctx, "objects[", r.fn()).ok());

  /* Callbacks may unwatch themselves, add watchers and notify. */
  ScopedConnection self;
  ScopedConnection added;
  Recorder late;
  int selfCalls = 0;
  const OwnerKey tool = w.rt.ext<ApiRegistry>().runtimeOwner("tool").owner;
  Result<ScopedConnection> s =
      bus.watch(ctx, "tool.count", [&](const PropValue &, const Status &) {
        selfCalls++;
        self.disconnect();
        added = watchOk(bus, ctx, "tool.pick", late);
        bus.notify(tool, "pick");
      });
  test_assert(s.ok());
  self = std::move(s).value();
  test_assert(setValue(ctx, "tool.count", PropValue::ofInt(9)).ok());
  test_assert(bus.flush() == 1 && selfCalls == 1 && !self.connected());
  test_assert(bus.pendingCount() == 1 &&
              late.calls == 0); /* delivered by the next flush */
  w.tool.pick = 2;
  test_assert(bus.flush() == 1 && late.calls == 1);
  test_assert(bus.watcherCount() == 2 && bus.validate().ok());

  /* ScopedConnection outliving the bus is harmless. */
  ScopedConnection outlives;
  {
    World w2;
    Context ctx2 = w2.context();
    Recorder r2;
    outlives = watchOk(w2.rt.ext<ChangeBus>(), ctx2, "tool.strength", r2);
  }
  test_assert(!outlives.connected());
  outlives.disconnect();
}

/* Library changes refire watchers without a notify (D2). */
static void test_library_changes()
{
  World w;
  Context ctx = w.context();
  ChangeBus &bus = w.rt.ext<ChangeBus>();
  Recorder r;
  ScopedConnection a = watchOk(bus, ctx, "objects[\"Cube\"].data.radius", r);
  test_assert(w.lib.markDeleted(w.meshes[0]).ok());
  test_assert(bus.flush() == 1 && r.lastStatus.code() == StatusCode::NotFound);
  test_assert(w.lib.restoreFromHistory(w.meshes[0]).ok());
  test_assert(bus.flush() == 1 && r.last == PropValue::ofFloat(1));
  test_assert(bus.flush() == 0);
}

/* A getter that adds watchers while flush re-reads (B6). */
static ChangeBus *g_bus = nullptr;
static Context *g_ctx = nullptr;
static bool g_spawn = false;
static litestl::util::Vector<ScopedConnection> *g_spawned = nullptr;

static void test_reentrant_getter()
{
  Runtime rt;
  ApiRegistry &reg = rt.ext<ApiRegistry>();
  struct Box {
    int v = 1;
  } box;
  ApiStruct st("Box");
  st.add(PropertyDef::intProp("v"),
         ValueAccessor{[](const PointerRef &s, Context &) {
                         if (g_spawn) {
                           g_spawn = false;
                           for (int i = 0; i < 64; i++) {
                             Result<ScopedConnection> c = g_bus->watch(
                                 *g_ctx, "v", [](const PropValue &, const Status &) {});
                             g_spawned->append(std::move(c).value());
                           }
                         }
                         return PropValue::ofInt(static_cast<Box *>(s.data)->v);
                       },
                       {}});
  test_assert(reg.add(std::move(st)).ok());
  Result<OwnerKey> owner =
      reg.addRuntimeOwner("box", StructId::of("Box"), [&box]() { return (void *)&box; });
  test_assert(owner.ok());
  Context ctx(&rt, nullptr, reg.runtimeOwner("box"));
  ChangeBus &bus = rt.ext<ChangeBus>();
  litestl::util::Vector<ScopedConnection> spawned;
  g_bus = &bus;
  g_ctx = &ctx;
  g_spawned = &spawned;
  Recorder r;
  ScopedConnection a = watchOk(bus, ctx, "v", r);
  box.v = 2;
  g_spawn = true;
  bus.notify(*owner, "v");
  test_assert(bus.flush() == 1 && r.calls == 1 && r.last == PropValue::ofInt(2));
  test_assert(bus.watcherCount() == 65);
  spawned.clear();
  test_assert(bus.watcherCount() == 1 && bus.validate().ok());
}

static void test_listeners()
{
  World w;
  Context ctx = w.context();
  ChangeBus &bus = w.rt.ext<ChangeBus>();
  const OwnerKey tool = w.rt.ext<ApiRegistry>().runtimeOwner("tool").owner;
  int calls = 0, alls = 0;
  OwnerKey last;
  double seen = -1;
  int pendingSeen = -1;
  ScopedConnection a = bus.addListener([&](const OwnerKey *owner) {
    calls++;
    alls += owner ? 0 : 1;
    last = owner ? *owner : OwnerKey();
    seen = w.tool.strength;
    pendingSeen = bus.pendingCount();
  });

  /* Called at once, after the write and the queueing. */
  test_assert(setValue(ctx, "tool.strength", PropValue::ofFloat(0.25)).ok());
  test_assert(calls == 1 && last == tool && seen == 0.25 && pendingSeen == 1);
  bus.notify(OwnerKey::ofBlock(w.meshId(0)), "radius");
  test_assert(calls == 2 && last == OwnerKey::ofBlock(w.meshId(0)));
  /* Not by a null owner, a failed set, an epoch bump or a flush. */
  bus.notify(OwnerKey());
  test_assert(!setValue(ctx, "tool.version", PropValue::ofInt(1)).ok());
  bus.bumpEpoch();
  bus.flush();
  test_assert(calls == 2);

  /* A listener may connect, disconnect and notify. One connected during a notify is not
   * called by it. */
  ScopedConnection self, added;
  int selfCalls = 0, addedCalls = 0;
  self = bus.addListener([&](const OwnerKey *) {
    selfCalls++;
    self.disconnect();
    added = bus.addListener([&](const OwnerKey *) { addedCalls++; });
  });
  bus.notify(tool);
  test_assert(calls == 3 && selfCalls == 1 && addedCalls == 0);
  bus.notify(tool);
  test_assert(calls == 4 && selfCalls == 1 && addedCalls == 1);
  added.disconnect();
  bool again = true;
  ScopedConnection nested = bus.addListener([&](const OwnerKey *) {
    if (again) {
      again = false;
      bus.notify(OwnerKey::ofBlock(w.meshId(1)));
    }
  });
  bus.notify(tool);
  test_assert(calls == 6 && last == OwnerKey::ofBlock(w.meshId(1)) && addedCalls == 1);

  a.disconnect();
  bus.notify(tool);
  test_assert(calls == 6 && alls == 0);

  /* A connection may outlive the bus. */
  ScopedConnection outlives;
  {
    Runtime rt2;
    outlives = rt2.ext<ChangeBus>().addListener([](const OwnerKey *) {});
    test_assert(outlives.connected());
  }
  test_assert(!outlives.connected());
}

/* Change listeners get all of it. */
static void test_change_listeners()
{
  World w;
  Context ctx = w.context();
  ChangeBus &bus = w.rt.ext<ChangeBus>();
  int calls = 0;
  OwnerKey owner;
  std::string member;
  ChangeAspects aspects = 0;
  ScopedConnection l = bus.addChangeListener([&](const BusChange &c) {
    calls++;
    owner = c.owner ? *c.owner : OwnerKey();
    member = std::string(c.member);
    aspects = c.aspects;
  });
  const OwnerKey mesh = OwnerKey::ofBlock(w.meshId(0));
  bus.notify(mesh, "radius", 6);
  test_assert(calls == 1 && owner == mesh && member == "radius" && aspects == 6);
  bus.notify(mesh);
  test_assert(calls == 2 && member.empty() && aspects == kAllAspects);
  test_assert(setValue(ctx, "tool.strength", PropValue::ofFloat(0.5)).ok());
  test_assert(calls == 3 && member == "strength" && aspects == kAllAspects);
  bus.notifyAll();
  test_assert(calls == 4 && owner == OwnerKey() && member.empty() &&
              aspects == kAllAspects);
  bus.bumpEpoch();
  l.disconnect();
  bus.notifyAll();
  test_assert(calls == 4);
}

static void test_notify_all()
{
  World w;
  Context ctx = w.context();
  ChangeBus &bus = w.rt.ext<ChangeBus>();
  int alls = 0, owners = 0;
  ScopedConnection l =
      bus.addListener([&](const OwnerKey *owner) { (owner ? owners : alls)++; });
  Recorder strength, radius;
  ScopedConnection a = watchOk(bus, ctx, "tool.strength", strength);
  ScopedConnection b = watchOk(bus, ctx, "objects[0].data.radius", radius);
  w.tool.strength = 0.75;
  w.mesh(0).radius = 4;
  test_assert(bus.flush() == 0); /* nobody said */
  bus.notifyAll();
  test_assert(alls == 1 && owners == 0);
  test_assert(bus.flush() == 2 && strength.last == PropValue::ofFloat(0.75) &&
              radius.last == PropValue::ofFloat(4));
  test_assert(bus.flush() == 0);
}

/* The update hook runs after the listeners, so a graph it evaluates is not stale. */
static void test_notified_before_update()
{
  World w;
  Context ctx = w.context();
  ChangeBus &bus = w.rt.ext<ChangeBus>();
  int hooksWhenCalled = -1;
  g_radiusUpdates = 0;
  ScopedConnection l =
      bus.addListener([&](const OwnerKey *) { hooksWhenCalled = g_radiusUpdates; });
  test_assert(setValue(ctx, "objects[0].data.radius", PropValue::ofFloat(2)).ok());
  test_assert(hooksWhenCalled == 0 && g_radiusUpdates == 1);
}

static void test_busy_while_evaluating()
{
  World w;
  Context ctx = w.context();
  ChangeBus &bus = w.rt.ext<ChangeBus>();
  int calls = 0;
  ScopedConnection l = bus.addListener([&](const OwnerKey *) { calls++; });
  g_radiusUpdates = 0;
  Result<PropValue> otherMesh = getValue(ctx, "objects[1].data");
  {
    EvaluationScope scope(w.rt);
    test_assert(setValue(ctx, "objects[0].data.radius", PropValue::ofFloat(5)).code() ==
                StatusCode::Busy);
    test_assert(setValue(ctx, "objects[0].location[1]", PropValue::ofFloat(5)).code() ==
                StatusCode::Busy);
    test_assert(setValue(ctx, "objects[0].data", *otherMesh).code() == StatusCode::Busy);
    /* Reads still work. */
    test_assert(getValue(ctx, "objects[0].data.radius").ok());
  }
  test_assert(w.mesh(0).radius == 1 && w.object(0).location[1] == 0 &&
              g_radiusUpdates == 0);
  test_assert(w.object(0).mesh.target == w.meshId(0) && calls == 0 &&
              bus.pendingCount() == 0);
  test_assert(setValue(ctx, "objects[0].data.radius", PropValue::ofFloat(5)).ok());
}

/* Watchers that go, many of them, and during a flush. */
static void test_unwatching()
{
  World w;
  Context ctx = w.context();
  ChangeBus &bus = w.rt.ext<ChangeBus>();
  std::vector<ScopedConnection> all(4);
  std::string log;
  for (int i = 0; i < 4; i++) {
    all[size_t(i)] = bus.watch(ctx,
                               "tool.count",
                               [&, i](const PropValue &, const Status &) {
                                 log += std::to_string(i);
                                 if (i == 0) {
                                   all[0].disconnect();
                                   all[1].disconnect();
                                   all[2].disconnect();
                                 }
                               })
                         .value();
  }
  test_assert(setValue(ctx, "tool.count", PropValue::ofInt(77)).ok());
  test_assert(bus.flush() == 2 && log == "03" && bus.watcherCount() == 1 &&
              bus.validate().ok());
  test_assert(setValue(ctx, "tool.count", PropValue::ofInt(78)).ok());
  test_assert(bus.flush() == 1 && log == "033");
  /* What a callback holds goes with it at once. */
  auto held = std::make_shared<int>(1);
  all[0] =
      bus.watch(ctx, "tool.count", [held](const PropValue &, const Status &) {}).value();
  all[1] = bus.watch(ctx, "tool.count", [](const PropValue &, const Status &) {}).value();
  test_assert(held.use_count() == 2);
  all[0].disconnect();
  test_assert(held.use_count() == 1);
  /* Those that are gone take no time. */
  const auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < 100000; i++) {
    all[0] =
        bus.watch(ctx, "tool.count", [](const PropValue &, const Status &) {}).value();
  }
  test_assert(std::chrono::steady_clock::now() - start < std::chrono::seconds(10));
  test_assert(bus.watcherCount() == 3);
}

/* However many notifications: distinct ones are kept, up to a limit. */
static void test_pending()
{
  World w;
  Context ctx = w.context();
  ChangeBus &bus = w.rt.ext<ChangeBus>();
  const OwnerKey tool = w.rt.ext<ApiRegistry>().runtimeOwner("tool").owner;
  Recorder count;
  Recorder strength;
  ScopedConnection c1 = watchOk(bus, ctx, "tool.count", count);
  ScopedConnection c2 = watchOk(bus, ctx, "tool.strength", strength);
  bus.flush();
  int heard = 0;
  ScopedConnection listener = bus.addListener([&](const OwnerKey *) { heard++; });
  for (int i = 0; i < 100000; i++) {
    bus.notify(tool, "count");
  }
  bus.notify(tool, "other");
  test_assert(bus.pendingCount() == 2 && heard == 100001);
  /* Another member, then the owner. */
  w.tool.strength = 0.125f;
  test_assert(bus.flush() == 0);
  bus.notify(tool, "");
  test_assert(bus.flush() == 1 && strength.calls == 1 && count.calls == 0);
  bus.notify(OwnerKey::ofRuntime(tool.nameHash + 1), "");
  bus.notify(OwnerKey::ofRuntime(tool.nameHash + 1), "strength");
  w.tool.strength = 0.25f;
  test_assert(bus.flush() == 0);
  bus.notify(tool, "strength");
  test_assert(bus.flush() == 1 && strength.calls == 2);

  /* Below the limit, what isn't named isn't read; past it, everything is. */
  for (const int n : {ChangeBus::kMaxPending, ChangeBus::kMaxPending + 1}) {
    const uint64_t epoch = bus.epoch();
    for (int i = 0; i < n; i++) {
      bus.notify(tool, "m" + std::to_string(i));
    }
    const bool past = n > ChangeBus::kMaxPending;
    bus.notify(tool, "m" + std::to_string(n - 1));
    test_assert(bus.epoch() == epoch + past && bus.pendingCount() == (past ? 1 : n));
    w.tool.strength += 0.125f;
    test_assert(bus.flush() == (past ? 1 : 0) && bus.pendingCount() == 0 &&
                bus.flush() == 0);
  }
  heard = 0;
  for (int i = 0; i < 100000; i++) {
    bus.notify(tool, "m" + std::to_string(i));
  }
  test_assert(heard == 100000 && bus.pendingCount() <= ChangeBus::kMaxPending &&
              bus.validate().ok());
  bus.flush();
}

/* Many changes against many watchers that they don't touch. */
static void test_many()
{
  World w;
  Context ctx = w.context();
  ChangeBus &bus = w.rt.ext<ChangeBus>();
  const OwnerKey tool = w.rt.ext<ApiRegistry>().runtimeOwner("tool").owner;
  Recorder r;
  std::vector<ScopedConnection> watchers;
  for (int i = 0; i < 1000; i++) {
    watchers.push_back(watchOk(bus, ctx, "tool.count", r));
  }
  const auto start = std::chrono::steady_clock::now();
  for (int round = 0; round < 10; round++) {
    for (int i = 0; i < 4000; i++) {
      bus.notify(tool, "m" + std::to_string(i));
    }
    test_assert(bus.flush() == 0);
  }
  const auto took = std::chrono::steady_clock::now() - start;
  test_assert(r.calls == 0 && took < std::chrono::seconds(1));
}

/* A widget shows the error, so it fires for another one. */
static void test_other_errors()
{
  World w;
  Context ctx = w.context();
  ChangeBus &bus = w.rt.ext<ChangeBus>();
  Recorder r;
  ScopedConnection c = watchOk(bus, ctx, "objects[\"Cube\"].data.radius", r);
  test_assert(w.lib.markDeleted(w.meshes[0]).ok());
  test_assert(bus.flush() == 1 && !r.lastStatus.ok());
  const Status first = r.lastStatus;
  bus.notifyAll();
  test_assert(bus.flush() == 0);
  /* No such object now. */
  w.scene.objects.clear();
  bus.notifyAll();
  test_assert(bus.flush() == 1 && !r.lastStatus.ok());
  test_assert(std::string(first.message()) != r.lastStatus.message());
  bus.notifyAll();
  test_assert(bus.flush() == 0);
}

int main()
{
  test_pending();
  test_many();
  test_other_errors();
  test_listeners();
  test_change_listeners();
  test_notify_all();
  test_notified_before_update();
  test_busy_while_evaluating();
  test_library_changes();
  test_reentrant_getter();
  test_aliases();
  test_hops_and_nan();
  test_failures_and_reentrancy();
  test_unwatching();
  return test_end();
}
