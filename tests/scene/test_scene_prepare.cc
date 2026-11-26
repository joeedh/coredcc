/* What a provider's `prepare` tells evaluateScene. */
#include "io_doc.h"

#include "coredcc/scene/overrides.h"

test_init;

using namespace scene_test;

namespace {

double xOf(const ObjectEval &e)
{
  const double *m = e.state.world;
  return m[12];
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
  testPrepare();
  return test_end();
}
