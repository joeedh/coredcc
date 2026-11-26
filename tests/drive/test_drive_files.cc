/* A scene's graphs in files, and SceneDrivers' lifetime. */
#include "drive_doc.h"

test_init;

using namespace drive_test;

namespace {

/* Saved and loaded, the drivers give the same states, bitwise. */
void testRoundTrip()
{
  DriveDoc d;
  const BlockHandle p = d.object("p", 3), b = d.object("b", 1), a = d.object("a");
  d.setParent(b, p);
  const BlockHandle g = d.graph("g");
  d.follow(g, a, b, 2);
  const BlockHandle c = d.object("c");
  d.driveLocation(d.graph("h"), c, 9);
  d.evaluate();
  test_assert(d.evaluatedAt(a, 6) && d.evaluatedAt(c, 9));

  Reloaded r(d);
  must(r.evaluate());
  for (const char *name : {"p", "b", "a", "c"}) {
    ObjectEval e;
    test_assert(r.evaluated(name, e));
    test_assert(sameState(e.state, d.result(d.byName(name)).state));
  }
  test_assert(r.drivers->transformCount() == 2);

  /* Without SceneDrivers, the listed graphs drive nothing. */
  Loaded plain(d, save(*d.lib));
  must(evaluateScene(plain.ctx));
  ObjectEval e;
  test_assert(
      Scene::of(plain.lib, plain.byName("Scene"))->evaluated(plain.byName("a"), e));
  test_assert(at(e.state)[0] == 0);
}

/* One provider per Library; destroyed before the Library, or after it. */
void testLifetime()
{
  DriveDoc d;
  Result<std::unique_ptr<SceneDrivers>> second = SceneDrivers::make(*d.lib);
  test_assert(!second.ok());
  d.drivers.reset();
  Result<std::unique_ptr<SceneDrivers>> again = SceneDrivers::make(*d.lib);
  test_assert(again.ok());
  d.drivers = std::move(again).value();
  const BlockHandle a = d.object("a");
  d.driveLocation(d.graph("g"), a, 4);
  d.evaluate();
  test_assert(d.evaluatedAt(a, 4));

  /* The Library goes first: the drivers do nothing after it. */
  std::unique_ptr<SceneDrivers> kept;
  {
    Library lib(&d.rt);
    Result<std::unique_ptr<SceneDrivers>> s = SceneDrivers::make(lib);
    must(s.status());
    kept = std::move(s).value();
    must(kept->update(0));
  }
  test_assert(!kept->update(0).ok());
  kept.reset();
}

} // namespace

int main()
{
  testRoundTrip();
  testLifetime();
  return test_end();
}
