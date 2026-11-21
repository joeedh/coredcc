#include "scene_fixture.h"

#include <cmath>
#include <string>

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

static ScopedConnection watchOk(ChangeBus &bus, Context &ctx, const char *path, Recorder &r)
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
  test_assert(byName.calls == 1 && byActive.calls == 1 && other.calls == 0 && unrelated.calls == 0);
  test_assert(byName.last == PropValue::ofVec3(5, 0, 0));

  /* No change, no call: flushing again, or writing the same value. */
  test_assert(bus.flush() == 0);
  test_assert(setValue(ctx, "objects[0].location", PropValue::ofVec3(5, 0, 0)).ok());
  test_assert(bus.flush() == 0 && byName.calls == 1);

  /* `active` moves: only the epoch says so, and only the active watcher's value changes. */
  w.scene.active = 1;
  bus.bumpEpoch();
  test_assert(bus.flush() == 1 && byActive.calls == 2 && byActive.last == PropValue::ofVec3(0, 0, 0));
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
  test_assert(bus.flush() == 1 && r.last.isNone() && r.lastStatus.code() == StatusCode::NotFound);
  bus.bumpEpoch();
  test_assert(bus.flush() == 0);
  w.scene.active = 1;
  bus.bumpEpoch();
  test_assert(bus.flush() == 1 && r.last == PropValue::ofString("Sphere") && r.lastStatus.ok());

  /* A watch on a malformed path fails at once. */
  test_assert(!bus.watch(ctx, "objects[", r.fn()).ok());

  /* Callbacks may unwatch themselves, add watchers and notify. */
  ScopedConnection self;
  ScopedConnection added;
  Recorder late;
  int selfCalls = 0;
  const OwnerKey tool = w.rt.ext<ApiRegistry>().runtimeOwner("tool").owner;
  Result<ScopedConnection> s = bus.watch(ctx, "tool.count", [&](const PropValue &, const Status &) {
    selfCalls++;
    self.disconnect();
    added = watchOk(bus, ctx, "tool.pick", late);
    bus.notify(tool, "pick");
  });
  test_assert(s.ok());
  self = std::move(s).value();
  test_assert(setValue(ctx, "tool.count", PropValue::ofInt(9)).ok());
  test_assert(bus.flush() == 1 && selfCalls == 1 && !self.connected());
  test_assert(bus.pendingCount() == 1 && late.calls == 0); /* delivered by the next flush */
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

int main()
{
  test_aliases();
  test_hops_and_nan();
  test_failures_and_reentrancy();
  return test_end();
}
