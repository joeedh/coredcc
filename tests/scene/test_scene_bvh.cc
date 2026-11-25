/* SceneBvh, and what pickScene answers when things change between an evaluation and a pick. */
#include "pick_doc.h"

test_init;

using namespace scene_test;

namespace {

/* The scene goes away while its objects are asked. */
void testSceneGoes()
{
  PickDoc d;
  const ViewMath view = viewOf(ViewCase());
  const BlockHandle data = d.geoBlock("ball", {ball(double3(), 0.5)});
  const BlockHandle a = d.placed("a", identity(), data);
  d.placed("b", identity(), data);
  d.evaluate();
  const ViewPoint p = view.project(double3());
  Status deleted;
  int calls = 0;
  d.geo(data).onPick = [&](const PickQuery &, PickSink &sink) {
    if (calls++ == 0) {
      deleted = d.lib->markDeleted(d.scene);
      deleted.ignore();
      d.lib->destroyFromHistory(d.scene).ignore();
      test_assert(d.lib->get(d.scene) != nullptr);
      d.lib->markDeleted(a).ignore();
    }
    sink.sphere(double3(), 0.5);
  };
  Vector<PickHit> hits;
  must(d.tryPick(view, p.x, p.y, within(5.0), hits));
  test_assert(deleted.ok() && calls == 2 && hits.size() == 2);
  test_assert(isError(d.tryPick(view, p.x, p.y, within(5.0), hits), StatusCode::NotFound));
}

/* ---- changes after an evaluation ---- */

void testChanges()
{
  PickDoc d;
  const ViewMath view = viewOf(ViewCase());
  const ViewPoint p = view.project(double3()), q = view.project(double3(3.0, 0.0, 0.0));
  const BlockHandle data = d.geoBlock("ball", {ball(double3(), 0.5)});
  const BlockHandle a = d.placed("a", identity(), data), b = d.placed("b", identity(), data);
  const BlockHandle c = d.placed("c", identity(), d.geoBlock("other", {ball(double3(), 0.4)}));
  d.evaluate();
  test_assert(d.picks(view, p.x, p.y, within(1.0)).size() == 3);

  /* Moved: the answer is for what was drawn. */
  d.obj(a).channels.set(matrix_transform::kTranslation, 3.0);
  test_assert(d.picks(view, p.x, p.y, within(1.0)).size() == 3 && d.picks(view, q.x, q.y, within(1.0)).size() == 0);
  d.evaluate();
  test_assert(d.picks(view, p.x, p.y, within(1.0)).size() == 2 && d.picks(view, q.x, q.y, within(1.0)).size() == 1);
  /* Unlinked, and new ones. */
  must(d.sc().remove(*d.lib, d.idOf(b)).status());
  const BlockHandle late = d.placed("late", identity(), data);
  test_assert(d.picks(view, p.x, p.y, within(1.0)).size() == 2);
  d.evaluate();
  Vector<PickHit> hits = d.picks(view, p.x, p.y, within(1.0));
  test_assert(hits.size() == 2 && hits[0].object == late && hits[1].object == c);

  /* Deleted, then gone, then its slot another block's. */
  must(d.lib->markDeleted(late));
  test_assert(d.picks(view, p.x, p.y, within(1.0)).size() == 1);
  d.lib->setQuarantine(false);
  must(d.lib->destroyFromHistory(late));
  test_assert(d.picks(view, p.x, p.y, within(1.0)).size() == 1);
  const BlockHandle second = d.placed("second", identity(), data);
  test_assert(second.index == late.index);
  test_assert(d.picks(view, p.x, p.y, within(1.0)).size() == 1);
  d.evaluate();

  /* The data block deleted and freed. */
  must(d.lib->markDeleted(data));
  hits = d.picks(view, p.x, p.y, within(1.0));
  test_assert(hits.size() == 1 && hits[0].object == c);
  must(d.lib->destroyFromHistory(data));
  test_assert(d.picks(view, p.x, p.y, within(1.0)).size() == 1);

  /* Moved by its host, taken away, and brought back. */
  const BlockHandle other = d.lib->find(d.obj(c).data.target());
  Geo moved;
  moved.prims = {ball(double3(0.0, 0.0, 0.1), 0.2)};
  moved.build();
  void *old = d.slot(other).data;
  d.slot(other).data = &moved;
  hits = d.picks(view, p.x, p.y, within(1.0));
  test_assert(hits.size() == 1 && std::fabs(hits[0].point[2] - 0.3) < 1e-9 && moved.given == 1);
  d.slot(other).data = nullptr;
  test_assert(d.picks(view, p.x, p.y, within(1.0)).size() == 0);
  d.slot(other).data = old;
  test_assert(d.picks(view, p.x, p.y, within(1.0)).size() == 1);
  /* Another data block. */
  const BlockHandle shape = d.shapeBlock("shape", 1.0);
  d.setData(c, shape);
  hits = d.picks(view, p.x, p.y, within(1.0));
  test_assert(hits.size() == 1 && hits[0].element == -1);
}

/* ---- which objects ---- */

void testMembers()
{
  PickDoc d;
  const ViewMath view = viewOf(ViewCase());
  const ViewPoint p = view.project(double3());
  const BlockHandle data = d.geoBlock("ball", {ball(double3(), 0.5)});
  const BlockHandle parent = d.loose("parent"), child = d.object("child");
  d.setData(parent, data);
  d.setData(child, data);
  d.setParent(child, parent);
  d.evaluate();
  Vector<PickHit> hits = d.picks(view, p.x, p.y, within(1.0));
  test_assert(hits.size() == 1 && hits[0].object == child);
  /* Channels that don't fit, and fit again. */
  const ChannelBlock good = d.obj(child).channels;
  d.obj(child).channels = ChannelBlock(2);
  d.evaluate();
  test_assert(d.statusOf(child) == EvalStatus::Invalid && !d.result(child).bounds.empty());
  test_assert(d.picks(view, p.x, p.y, within(1.0)).size() == 0);
  d.obj(child).channels = good;
  d.evaluate();
  test_assert(d.picks(view, p.x, p.y, within(1.0)).size() == 1);
  /* A missing transform type has its parent's place. */
  d.obj(child).setTransformType("test.missing");
  d.evaluate();
  test_assert(d.statusOf(child) != EvalStatus::Ok);
  /* Its own Spatial, without a Transformable and without a data block. */
  const BlockHandle plain = d.addSlot<Geo>("test.Geo", "plain");
  d.geo(plain).prims = {ball(double3(), 0.6)};
  d.geo(plain).build();
  must(d.sc().add(*d.lib, plain));
  d.evaluate();
  hits = d.picks(view, p.x, p.y, within(1.0));
  test_assert(d.statusOf(plain) == EvalStatus::NoTransform && hits[0].object == plain);
}

/* ---- the BVH ---- */

void testBvh()
{
  PickDoc d;
  const ViewMath view = viewOf(ViewCase());
  const ViewPoint p = view.project(double3());
  test_assert(!d.sc().bvh().valid());
  const BlockHandle shape = d.shapeBlock("shape", 1.0);
  std::vector<BlockHandle> all;
  for (int i = 0; i < 300; i++) {
    all.push_back(d.object("o", 10.0 * (i % 20), 10.0 * (i / 20)));
    d.setData(all.back(), shape);
  }
  const auto is = [&](int builds, int refits) {
    const SceneBvh::Counters &n = d.sc().bvh().counters();
    if (n.builds != builds || n.refits != refits) {
      fprintf(stderr, "%d builds, %d refits\n", n.builds, n.refits);
    }
    return n.builds == builds && n.refits == refits;
  };
  const auto fits = [&] {
    const SceneBvh &b = d.sc().bvh();
    bool ok = true;
    int there = 0;
    /* A region without planes is everything. */
    must(b.boxes().region({}, [&](int i) {
      ok = ok && d.result(b.object(i)).bounds.contains(
                     solve::translationOf(d.result(b.object(i)).state.world));
      there++;
      return true;
    }));
    for (BlockHandle h : all) {
      const Box3d &box = d.result(h).bounds;
      ok = ok && (box.empty() || (b.boxes().bvh().bounds().min[0] <= box.min[0] &&
                                  b.boxes().bvh().bounds().max[0] >= box.max[0]));
    }
    int bounded = 0;
    for (BlockHandle h : d.sc().evaluatedObjects()) {
      bounded += !d.result(h).bounds.empty();
    }
    return ok && there == bounded;
  };
  d.evaluate();
  test_assert(is(1, 0) && fits() && d.sc().bvh().valid());
  /* Nothing differs. */
  d.evaluate();
  Vector<BlockId> ids;
  ids.append(d.idOf(all[3]));
  d.sc().setSelection(*d.lib, ids);
  d.evaluate();
  test_assert(is(1, 0));
  /* Moved; a parent link, which keeps the list. */
  d.obj(all[5]).channels.set(trs::kLocation + 2, 3.0);
  d.evaluate();
  test_assert(is(1, 1) && fits());
  d.setParent(all[7], all[8]);
  d.evaluate();
  test_assert(is(1, 2) && fits());
  /* The same objects in another order. */
  must(d.sc().remove(*d.lib, d.idOf(all[6])).status());
  must(d.sc().add(*d.lib, all[6]));
  d.evaluate();
  test_assert(is(2, 2) && fits());
  must(d.sc().remove(*d.lib, d.idOf(all[6])).status());
  must(d.sc().add(*d.lib, all[6], 6));
  d.evaluate();
  test_assert(is(3, 2));
  /* Another list. */
  must(d.sc().remove(*d.lib, d.idOf(all[9])).status());
  d.evaluate();
  test_assert(is(4, 2) && d.sc().bvh().boxes().bvh().size() == 299);
  must(d.sc().add(*d.lib, all[9]));
  d.evaluate();
  test_assert(is(5, 2) && fits());
  /* One without bounds, still without, and with them again. */
  d.setData(all[2], BlockHandle());
  d.evaluate();
  test_assert(is(5, 3) && d.sc().bvh().boxes().bvh().skipped() == 0 && fits());
  d.obj(all[4]).channels.set(trs::kLocation + 2, 1.0);
  d.evaluate();
  test_assert(is(5, 4) && fits());
  d.setData(all[2], shape);
  d.evaluate();
  test_assert(is(5, 5) && fits());
  /* Two that swap being without. */
  d.setData(all[2], BlockHandle());
  d.evaluate();
  d.setData(all[2], shape);
  d.setData(all[3], BlockHandle());
  d.evaluate();
  test_assert(is(5, 7) && fits());
  d.setData(all[3], shape);
  d.evaluate();
  test_assert(is(5, 8) && fits());
  /* One that the tree never had. */
  const BlockHandle late = d.object("late");
  d.evaluate();
  test_assert(is(6, 8) && d.sc().bvh().boxes().bvh().skipped() == 1 && fits());
  d.setData(late, shape);
  d.evaluate();
  test_assert(is(7, 8) && d.sc().bvh().boxes().bvh().skipped() == 0 && fits());
  must(d.sc().remove(*d.lib, d.idOf(late)).status());
  d.evaluate();
  test_assert(is(8, 8));
  /* One that flies away, and is followed. */
  for (int k = 1; k < 20; k++) {
    d.obj(all[4]).channels.set(trs::kLocation + 2, k * 500.0);
    d.evaluate();
  }
  test_assert(is(8, 27) && fits());
  d.obj(all[4]).channels.set(trs::kLocation + 2, 1.0);
  d.evaluate();
  test_assert(is(8, 28) && fits());

  /* Scattered: the old tree would be slow. */
  Rand r(41);
  for (BlockHandle h : all) {
    put(d.obj(h).channels, trs::kLocation, r.vec(200.0));
  }
  d.evaluate();
  test_assert(is(9, 28) && fits());
  /* All a bit, twice: refits are compared with the last build. */
  for (int k = 0; k < 2; k++) {
    for (BlockHandle h : all) {
      d.obj(h).channels.set(trs::kLocation, d.obj(h).channels.get(trs::kLocation) + 1.0);
    }
    d.evaluate();
  }
  test_assert(is(9, 30) && fits());
  put(d.obj(all[0]).channels, trs::kLocation, double3());
  d.evaluate();
  test_assert(d.picks(view, p.x, p.y, within(0.0)).size() >= 1);
  /* The scene got larger: refits are compared with the last build, not the first. */
  PickDoc grown;
  const BlockHandle other = grown.shapeBlock("shape", 1.0), first = grown.object("first");
  grown.setData(first, other);
  grown.evaluate();
  for (int i = 0; i < 2000; i++) {
    grown.setData(grown.object("o", 10.0 * (i % 40), 10.0 * (i / 40)), other);
  }
  grown.evaluate();
  grown.obj(first).channels.set(trs::kLocation + 2, 1.0);
  grown.evaluate();
  const SceneBvh::Counters &counts = grown.sc().bvh().counters();
  test_assert(counts.builds == 2 && counts.refits == 1);
  /* None left. */
  for (BlockHandle h : all) {
    must(d.sc().remove(*d.lib, d.idOf(h)).status());
  }
  d.evaluate();
  test_assert(d.picks(view, p.x, p.y, within(0.0)).size() == 0 && d.sc().bvh().valid());
}

} // namespace

int main()
{
  testSceneGoes();
  testChanges();
  testMembers();
  testBvh();
  return test_end();
}
