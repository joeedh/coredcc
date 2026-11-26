/* Part B of P5c: evaluateScene evaluates only what was notified, what is below it, and
 * bounds it can't keep. Random edits that notify, on two documents made alike, one of
 * them with SceneSettings::incremental off: the results, the changes and what the BVH
 * finds must be the same, bitwise. */
#include "scene_doc.h"

#include "coredcc/api/change_bus.h"
#include "fuzz.h"

#include <string>
#include <vector>

test_init;

using namespace scene_test;

namespace {

struct Twin {
  Doc d;
  std::vector<BlockHandle> objects, shapes;

  explicit Twin(bool incremental)
  {
    d.rt.ext<SceneSettings>().incremental = incremental;
  }
};

bool sameResults(Twin &a, Twin &b, int seed, int step)
{
  const Scene &sa = a.d.sc(), &sb = b.d.sc();
  const Vector<BlockHandle> &oa = sa.evaluatedObjects(), &ob = sb.evaluatedObjects();
  if (oa.size() != ob.size()) {
    fprintf(stderr,
            "seed %d step %d: %d objects, not %d\n",
            seed,
            step,
            int(oa.size()),
            int(ob.size()));
    return false;
  }
  for (int i = 0; i < int(oa.size()); i++) {
    ObjectEval x, y;
    test_assert(sa.evaluated(oa[i], x) && sb.evaluated(ob[i], y));
    if (!x.same(y)) {
      fprintf(stderr, "seed %d step %d: place %d differs\n", seed, step, i);
      return false;
    }
  }
  const Scene::Changes &ca = sa.lastChanges(), &cb = sb.lastChanges();
  if (ca.version != cb.version ||
      (!ca.all && !cb.all && ca.places.size() != cb.places.size()))
  {
    fprintf(stderr, "seed %d step %d: changes differ\n", seed, step);
    return false;
  }
  for (int i = 0; !ca.all && !cb.all && i < int(ca.places.size()); i++) {
    test_assert(ca.places[i] == cb.places[i]);
  }
  /* The BVH: what is near a few points. Handles differ between the documents, so the
   * places are compared. */
  for (int k = -6; k <= 6; k += 3) {
    Vector<BlockHandle> na, nb;
    must(sa.bvh().objectsNear(double3(k, 0, 0), 2.5, na));
    must(sb.bvh().objectsNear(double3(k, 0, 0), 2.5, nb));
    if (na.size() != nb.size()) {
      fprintf(stderr, "seed %d step %d: the BVH differs\n", seed, step);
      return false;
    }
  }
  return true;
}

void edit(Twin &t, Rand &r, int kind, int pick, double value)
{
  Doc &d = t.d;
  const BlockHandle o = t.objects[size_t(pick) % t.objects.size()];
  const BlockHandle other = t.objects[size_t(pick / 7) % t.objects.size()];
  (void)r;
  switch (kind) {
  case 0: /* a channel */
    d.obj(o).channels.set(trs::kLocation + pick % 3, value);
    break;
  case 1: /* a parent: the epoch moves */
    d.setParent(o, o == other ? BlockHandle() : other);
    break;
  case 2: /* a shape's size, notified */
    d.shape(t.shapes[size_t(pick) % t.shapes.size()]).box.max[0] = 1 + value;
    break;
  case 3: /* data */
    d.setData(o, pick % 3 ? t.shapes[size_t(pick) % t.shapes.size()] : BlockHandle());
    break;
  case 4: /* the time */
    d.sc().frame = double(pick % 5);
    break;
  case 5: /* nothing */
    break;
  case 6: /* a rotation */
    d.obj(o).channels.set(trs::kRotationEuler + pick % 3, value * 0.1);
    break;
  default: /* a scale of 0, then back */
    d.obj(o).channels.set(trs::kScale, pick % 2 ? 0.0 : 1.0);
    break;
  }
}

void testGrowth()
{
  /* Growth: one object moved costs the same in a large scene as in a small one, and
   * evaluates only it and what is below it. */
  Doc d;
  const BlockHandle shape = d.shapeBlock("shape", 0.25);
  std::vector<BlockHandle> all;
  for (int i = 0; i < 3000; i++) {
    all.push_back(d.object("o", i));
    d.setData(all.back(), shape);
    if (i % 3) {
      d.setParent(all.back(), all[size_t(i - 1)]);
    }
  }
  d.evaluate();
  const auto near = [&](const double3 &at) {
    Vector<BlockHandle> found;
    must(d.sc().bvh().objectsNear(at, 0.1, found));
    return found;
  };
  const double3 was = solve::translationOf(d.result(all[31]).state.world);
  test_assert(near(was).size() == 1);
  const uint64_t before = d.sc().lastChanges().version;
  const int builds = d.sc().bvh().counters().builds;
  d.obj(all[30]).channels.set(trs::kLocation + 1, 5.0);
  d.evaluate();
  const Scene::Changes &c = d.sc().lastChanges();
  test_assert(c.version == before + 1 && !c.all && c.places.size() == 3);
  /* The BVH was refitted at those places, box by box. */
  const double3 now = solve::translationOf(d.result(all[31]).state.world);
  test_assert(now[1] == was[1] + 5.0 && near(was).size() == 0);
  const Vector<BlockHandle> found = near(now);
  test_assert(found.size() == 1 && found[0] == all[31]);
  test_assert(d.sc().bvh().counters().builds == builds);
}

} // namespace

int main()
{
  const int seeds = 30 * coredcc_test::testScale();
  for (int seed = 0; seed < seeds; seed++) {
    Twin a(true), b(false);
    for (Twin *t : {&a, &b}) {
      Rand r{uint64_t(seed)};
      const int n = 5 + int(r.below(30));
      for (int i = 0; i < 4; i++) {
        t->shapes.push_back(t->d.shapeBlock(("s" + std::to_string(i)).c_str(), 1 + i));
      }
      for (int i = 0; i < n; i++) {
        t->objects.push_back(
            t->d.object(("o" + std::to_string(i)).c_str(), r.range(-5, 5)));
        if (i > 0 && r.coin()) {
          t->d.setParent(t->objects.back(), t->objects[r.below(uint32_t(i))]);
        }
        if (r.coin()) {
          t->d.setData(t->objects.back(), t->shapes[r.below(4)]);
        }
      }
      t->d.evaluate();
    }
    test_assert(sameResults(a, b, seed, -1));
    Rand edits{uint64_t(seed) * 7919 + 1};
    for (int step = 0; step < 40; step++) {
      const int count = 1 + int(edits.below(3));
      std::vector<int> kinds, picks;
      std::vector<double> values;
      for (int k = 0; k < count; k++) {
        kinds.push_back(int(edits.below(8)));
        picks.push_back(int(edits.below(1000)));
        values.push_back(edits.range(-10, 10));
      }
      for (Twin *t : {&a, &b}) {
        for (int k = 0; k < count; k++) {
          edit(*t, edits, kinds[size_t(k)], picks[size_t(k)], values[size_t(k)]);
        }
        (void)evaluateScene(*t->d.ctx);
      }
      if (!sameResults(a, b, seed, step)) {
        test_assert(false);
        break;
      }
    }
  }

  testGrowth();
  return test_end();
}
