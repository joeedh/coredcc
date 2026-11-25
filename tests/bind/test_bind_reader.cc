/* Reading and watching many paths for scripts. */
#include "script_fixture.h"

#include "coredcc/bind/path_reader.h"

#include <chrono>
#include <optional>

test_init;

namespace {

int64_t add(const sc::PathReader &r, const char *path)
{
  sc::IntResult i = sc::readerAdd(r, path);
  test_assert(i.status.ok());
  return i.value;
}

bool holds(const sc::PathReader &r, int i, const PropValue &want)
{
  sc::ValueResult v = sc::readerValue(r, i);
  return v.status.ok() && v.value == want;
}

} // namespace

static void test_read()
{
  Setup s;
  sc::PathReader r = sc::newPathReader();
  test_assert(sc::readerCount(r) == 0 && sc::readerRead(s.ctx, r).ok());
  test_assert(add(r, "tool.strength") == 0 && add(r, "tool.count") == 1 &&
              add(r, "objects[99].name") == 2 && sc::readerCount(r) == 3);
  test_assert(!sc::readerAdd(r, "a..b").status.ok() && sc::readerCount(r) == 3);
  test_assert(sc::readerValue(r, 0).status.code() == StatusCode::FailedPrecondition);
  test_assert(sc::readerRead(s.ctx, r).ok());
  test_assert(holds(r, 0, PropValue::ofFloat(0.5)) && holds(r, 1, PropValue::ofInt(4)));
  sc::ValueResult bad = sc::readerValue(r, 2);
  test_assert(!bad.status.ok() && bad.status.code() != StatusCode::FailedPrecondition &&
              bad.value.isNone());
  for (int i : {-1, 3}) {
    test_assert(sc::readerValue(r, i).status.code() == StatusCode::OutOfRange);
  }
  /* As of the last read. */
  s.w.tool.strength = 0.25;
  test_assert(holds(r, 0, PropValue::ofFloat(0.5)));
  sc::PathReader copy = r; /* shared */
  test_assert(sc::readerRead(s.ctx, copy).ok() && holds(r, 0, PropValue::ofFloat(0.25)));
  test_assert(sc::readerTakeChanged(r) == -1 && sc::watchedCount(s.ctx) == 0);
  /* A value goes when its path fails. */
  s.ctx.setPrefix(*parsePath("objects[0]"));
  test_assert(sc::readerRead(s.ctx, r).ok() && !sc::readerValue(r, 0).status.ok());
}

static void test_watch()
{
  Setup s;
  Context &ctx = s.ctx;
  ChangeBus &bus = s.w.rt.ext<ChangeBus>();
  sc::PathReader r = sc::newPathReader();
  add(r, "tool.count");
  add(r, "tool.strength");
  add(r, "objects.active.name");
  add(r, "tool.locked");
  test_assert(sc::readerWatch(ctx, r).ok() && sc::watchedCount(ctx) == 4);
  test_assert(holds(r, 1, PropValue::ofFloat(0.5)));
  test_assert(sc::flushChanges(ctx).value == 0 && sc::readerTakeChanged(r) == -1);
  /* Two changes, and a write that changes nothing. */
  test_assert(sc::setValue(ctx, "tool.strength", sc::makeFloat(0.75)).ok());
  test_assert(sc::setValue(ctx, "tool.count", sc::makeInt(9)).ok());
  test_assert(sc::setValue(ctx, "tool.locked", sc::makeBool(false)).ok());
  test_assert(holds(r, 1, PropValue::ofFloat(0.5)) && sc::readerTakeChanged(r) == -1);
  sc::IntResult n = sc::flushChanges(ctx);
  test_assert(n.status.ok() && n.value == 2 && holds(r, 1, PropValue::ofFloat(0.75)));
  test_assert(sc::readerTakeChanged(r) == 0 && sc::readerTakeChanged(r) == 1 &&
              sc::readerTakeChanged(r) == -1 && sc::readerTakeChangedMeta(r) == -1);
  /* Undo, and a change made in C++. */
  test_assert(sc::undo(ctx).ok() && sc::flushChanges(ctx).value == 0);
  test_assert(sc::undo(ctx).ok() && sc::flushChanges(ctx).value == 1);
  test_assert(sc::readerTakeChanged(r) == 0 && holds(r, 0, PropValue::ofInt(4)));
  s.w.tool.strength = 0.125;
  bus.notifyAll();
  test_assert(sc::flushChanges(ctx).value == 1 && sc::readerTakeChanged(r) == 1 &&
              holds(r, 1, PropValue::ofFloat(0.125)));
  /* A path that fails, and works again. */
  const int active = s.w.scene.active;
  s.w.scene.active = -1;
  bus.notifyAll();
  test_assert(sc::flushChanges(ctx).value == 1 && sc::readerTakeChanged(r) == 2 &&
              !sc::readerValue(r, 2).status.ok() && sc::readerTakeChangedMeta(r) == 2 &&
              sc::readerTakeChangedMeta(r) == -1);
  s.w.scene.active = active;
  bus.notifyAll();
  test_assert(sc::flushChanges(ctx).value == 1 && sc::readerValue(r, 2).status.ok());
  /* A path added later waits for the next watch, which forgets what had changed. */
  add(r, "tool.pick");
  test_assert(sc::watchedCount(ctx) == 4 && sc::readerWatch(ctx, r).ok() &&
              sc::watchedCount(ctx) == 5 && sc::readerTakeChanged(r) == -1 &&
              sc::readerTakeChangedMeta(r) == -1);
  sc::readerUnwatch(r);
  test_assert(sc::watchedCount(ctx) == 0 && sc::setValue(ctx, "tool.count", sc::makeInt(1)).ok() &&
              sc::flushChanges(ctx).value == 0 && holds(r, 0, PropValue::ofInt(4)));
}

static void test_lifetime()
{
  Setup s;
  std::optional<sc::PathReader> r = sc::newPathReader();
  add(*r, "tool.count");
  test_assert(sc::readerWatch(s.ctx, *r).ok());
  std::optional<sc::PathReader> copy = *r;
  r.reset();
  test_assert(sc::watchedCount(s.ctx) == 1);
  copy.reset();
  test_assert(sc::watchedCount(s.ctx) == 0);
  test_assert(sc::setValue(s.ctx, "tool.count", sc::makeInt(1)).ok());
  test_assert(sc::flushChanges(s.ctx).value == 0);
  /* After the Runtime. */
  sc::PathReader late = sc::newPathReader();
  {
    Setup other;
    add(late, "tool.count");
    test_assert(sc::readerWatch(other.ctx, late).ok());
  }
  sc::readerUnwatch(late);
  test_assert(holds(late, 0, PropValue::ofInt(4)));
}

static void test_slots()
{
  Setup s;
  Host host;
  Script script;
  test_assert(registerScript(s, host, &script, "js.tool", kCore).ok());
  sc::PathReader r = sc::newPathReader();
  add(r, "tool.count");
  int ran = 0;
  script.onExec = [&](Context &ctx) {
    ran++;
    test_assert(sc::readerRead(ctx, r).ok() && holds(r, 0, PropValue::ofInt(4)));
    test_assert(sc::readerWatch(ctx, r).code() == StatusCode::Busy);
    test_assert(sc::flushChanges(ctx).status.code() == StatusCode::Busy);
  };
  test_assert(sc::invoke(s.ctx, "js.tool()").status.ok() && ran == 1);
  test_assert(sc::watchedCount(s.ctx) == 0);
}

/* As many as a layout can have, in time. */
static void test_many()
{
  Setup s;
  sc::PathReader r = sc::newPathReader();
  for (int i = 0; i < 16384; i++) {
    add(r, i % 2 ? "tool.count" : "tool.strength");
  }
  const auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < 2; i++) {
    test_assert(sc::readerWatch(s.ctx, r).ok() && sc::watchedCount(s.ctx) == 16384);
  }
  test_assert(sc::setValue(s.ctx, "tool.count", sc::makeInt(1)).ok());
  test_assert(sc::flushChanges(s.ctx).value == 8192);
  int n = 0;
  while (sc::readerTakeChanged(r) >= 0) {
    n++;
  }
  sc::readerUnwatch(r);
  test_assert(n == 8192 && sc::watchedCount(s.ctx) == 0);
  const std::chrono::duration<double> took = std::chrono::steady_clock::now() - start;
  if (took.count() > 5) {
    fprintf(stderr, "test_many took %g s\n", took.count());
  }
  test_assert(took.count() < 20);
}

int main()
{
  test_read();
  test_watch();
  test_lifetime();
  test_slots();
  test_many();
  return test_end();
}
