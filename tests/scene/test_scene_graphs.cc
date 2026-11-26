/* The graphs a scene lists (Scene::graphs), and what a provider's `prepare` tells
 * evaluateScene. scene/ doesn't look at what the cells point to, so any block stands in
 * for a graph here. */
#include "io_doc.h"

#include "coredcc/api/resolve.h"
#include "coredcc/scene/overrides.h"

test_init;

using namespace scene_test;

namespace {

double xOf(const ObjectEval &e)
{
  const double *m = e.state.world;
  return m[12];
}

bool hasCode(const Status &s, StatusCode code)
{
  return s.code() == code;
}

void testList()
{
  IoDoc d;
  const BlockHandle g1 = d.loose("g1"), g2 = d.loose("g2"), g3 = d.loose("g3");
  Scene &s = d.sc();
  test_assert(s.graphs(*d.lib).size() == 0);
  must(s.insertGraph(*d.lib, d.idOf(g1)));
  must(s.insertGraph(*d.lib, d.idOf(g2)));
  must(s.insertGraph(*d.lib, d.idOf(g3), 0));
  test_assert(hasCode(s.insertGraph(*d.lib, d.idOf(g2)), StatusCode::AlreadyExists));
  test_assert(hasCode(s.insertGraph(*d.lib, BlockId()), StatusCode::NotFound));
  const Vector<BlockHandle> &l = s.graphs(*d.lib);
  test_assert(l.size() == 3 && l[0] == g3 && l[1] == g1 && l[2] == g2);

  /* A deleted one stays in its place, and isn't listed. */
  must(d.lib->markDeleted(g1));
  test_assert(s.graphs(*d.lib).size() == 2 && s.graphCells().size() == 3);
  must(d.lib->restoreFromHistory(g1));
  test_assert(s.graphs(*d.lib).size() == 3);

  Result<int> at = s.removeGraph(*d.lib, d.idOf(g1));
  test_assert(at.ok() && *at == 1 && s.graphs(*d.lib).size() == 2);
  test_assert(hasCode(s.removeGraph(*d.lib, d.idOf(g1)).status(), StatusCode::NotFound));

  /* By path: a collection, by place and by name. */
  Result<PropValue> n = getValue(*d.ctx, "scene.graphs.length");
  test_assert(n.ok() && n->asInt() == 2);

  /* The cells keep their blocks alive, as OptStrong references. */
  must(s.insertGraph(*d.lib, d.idOf(g1)));
  must(d.lib->collect().status());
  test_assert(d.lib->get(g1) && d.lib->get(g2) && d.lib->get(g3));
  must(d.lib->validate());
}

void testFiles()
{
  IoDoc d;
  const BlockHandle g1 = d.loose("g1"), g2 = d.loose("g2");
  /* No list: no entry, so the bytes are as before. */
  const Bytes before = save(*d.lib);
  must(d.sc().insertGraph(*d.lib, d.idOf(g2)));
  must(d.sc().insertGraph(*d.lib, d.idOf(g1)));
  Loaded l(d, save(*d.lib));
  const Scene &s = *Scene::of(l.lib, l.byName("Scene"));
  test_assert(s.graphs(l.lib).size() == 2);
  test_assert(s.graphs(l.lib)[0] == l.byName("g2") && s.graphs(l.lib)[1] == l.byName("g1"));
  Loaded empty(d, before);
  test_assert(Scene::of(empty.lib, empty.byName("Scene"))->graphCells().size() == 0);
  /* A deleted graph is written as nothing, and the load drops it. */
  must(d.lib->markDeleted(g1));
  Loaded one(d, save(*d.lib));
  test_assert(Scene::of(one.lib, one.byName("Scene"))->graphCells().size() == 1);
  /* A duplicate keeps the list. */
  Scene *copy = d.sc().duplicate();
  test_assert(copy->graphCells().size() == 2);
  litestl::alloc::Delete(copy);
}

/* A provider that drives one object, and counts what it is asked. */
struct One : OverrideProvider {
  BlockId target;
  bool all = false;
  uint64_t version = 1;
  int prepared = 0;
  int asked = 0;
  double value = 5.0;

  Status prepare(const Library &, double time, Driven &driven) override
  {
    prepared++;
    test_assert(time == 2.0);
    driven.all = all;
    driven.version = version;
    driven.ids.clear();
    driven.ids.append(target);
    return Status();
  }
  Status overridesOf(const Library &lib,
                     BlockHandle object,
                     const TransformType &,
                     double,
                     ChannelOverrides &out) override
  {
    asked++;
    if (lib.get(object)->id() == target) {
      out.set(trs::kLocation, value);
    }
    return Status();
  }
};

void testPrepare()
{
  IoDoc d;
  const BlockHandle a = d.object("a", 1), b = d.object("b", 2);
  d.object("c", 3);
  d.sc().frame = 2.0 * d.sc().units.rate.num() / d.sc().units.rate.den();
  One p;
  p.target = d.idOf(b);
  must(setOverrideProvider(*d.lib, p));
  must(evaluateScene(*d.ctx));
  test_assert(p.prepared == 1 && p.asked == 1);
  ObjectEval e;
  test_assert(d.sc().evaluated(b, e) && xOf(e) == 5.0);
  test_assert(d.sc().evaluated(a, e) && xOf(e) == 1.0);

  /* Another set: the members are marked again. */
  p.target = d.idOf(a);
  p.version = 2;
  p.asked = 0;
  must(evaluateScene(*d.ctx));
  test_assert(p.asked == 1 && d.sc().evaluated(a, e) && xOf(e) == 5.0);
  test_assert(d.sc().evaluated(b, e) && xOf(e) == 2.0);

  /* `all`: every object is asked, as without `prepare`. */
  p.all = true;
  p.asked = 0;
  must(evaluateScene(*d.ctx));
  test_assert(p.asked == 3);

  /* Other lookups with the same set: a new member that the set names is asked. */
  p.all = false;
  p.version = 3;
  must(evaluateScene(*d.ctx));
  const BlockHandle n = d.object("n", 7);
  p.target = d.idOf(n);
  p.asked = 0;
  must(evaluateScene(*d.ctx)); /* the version didn't move, the lookups did */
  test_assert(p.asked == 1 && d.sc().evaluated(n, e) && xOf(e) == 5.0);
  clearOverrideProvider(*d.lib, p);
}

} // namespace

int main()
{
  testList();
  testFiles();
  testPrepare();
  return test_end();
}
