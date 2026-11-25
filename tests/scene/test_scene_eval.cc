/* Object, Scene and evaluateScene. test_scene_cache.cc has what the lookup cache must notice;
 * test_scene_paths.cc has the datapath. */
#include "scene_doc.h"

#include <algorithm>

test_init;

using namespace scene_test;

namespace {

/* Boxes in the world are rounded outward. */
bool closeTo(double a, double b)
{
  return std::fabs(a - b) < 1e-9;
}

/* ---- members ---- */

void testMembers()
{
  Doc d;
  Scene &sc = d.sc();
  const BlockHandle a = d.object("a"), b = d.object("b"), c = d.object("c");
  test_assert(sc.objects(*d.lib).size() == 3 && sc.indexOf(*d.lib, b) == 1);
  test_assert(isError(sc.add(*d.lib, b), StatusCode::AlreadyExists, "'b'"));
  test_assert(isError(sc.add(*d.lib, BlockHandle()), StatusCode::NotFound));
  Result<int> at = sc.remove(*d.lib, d.idOf(b));
  test_assert(at.ok() && *at == 1 && !sc.contains(*d.lib, b));
  test_assert(sc.indexOf(*d.lib, c) == 1 && sc.objects(*d.lib)[1] == c);
  test_assert(isError(sc.remove(*d.lib, d.idOf(b)).status(), StatusCode::NotFound));
  test_assert(isError(sc.remove(*d.lib, BlockId()).status(), StatusCode::NotFound));
  must(sc.add(*d.lib, b, 0));
  test_assert(sc.objects(*d.lib)[0] == b && sc.indexOf(*d.lib, a) == 1 &&
              sc.indexOf(*d.lib, c) == 2);
  must(sc.remove(*d.lib, d.idOf(b)).status());
  must(sc.add(*d.lib, b, 99));
  test_assert(sc.indexOf(*d.lib, b) == 2);

  /* Deleted members keep their place. */
  must(d.lib->markDeleted(a));
  test_assert(sc.objects(*d.lib).size() == 2 && sc.contains(*d.lib, a) && sc.cells().size() == 3);
  test_assert(sc.prune(*d.lib) == 0);
  must(d.lib->restoreFromHistory(a));
  test_assert(sc.objects(*d.lib).size() == 3 && sc.objects(*d.lib)[0] == a);
  must(d.lib->onHostDestroyed(d.idOf(c)));
  test_assert(sc.objects(*d.lib).size() == 2);
  must(d.lib->onHostRestored(d.idOf(c)));
  test_assert(sc.objects(*d.lib).size() == 3);

  /* Without a record they go, with the next prune. */
  must(d.lib->markDeleted(a));
  must(d.lib->destroyFromHistory(a));
  test_assert(sc.cells().size() == 3 && sc.objects(*d.lib).size() == 2);
  test_assert(sc.prune(*d.lib) == 1 && sc.cells().size() == 2 && sc.prune(*d.lib) == 0);
  test_assert(sc.indexOf(*d.lib, c) == 0 && sc.indexOf(*d.lib, b) == 1);

  /* A cell that something else writes. */
  must(sc.add(*d.lib, d.loose("e")));
  const BlockHandle f = d.loose("f");
  test_assert(sc.indexOf(*d.lib, c) == 0);
  must(d.lib->writeRef(const_cast<RefCell &>(sc.cells()[0]), d.idOf(f)));
  test_assert(sc.indexOf(*d.lib, c) == -1 && sc.indexOf(*d.lib, f) == 0);
}

void testSelection()
{
  Doc d;
  Scene &sc = d.sc();
  const BlockHandle a = d.object("a"), b = d.object("b"), c = d.object("c"),
                    outside = d.loose("outside");
  Vector<BlockId> ids;
  for (BlockHandle h : {c, a, c, outside}) {
    ids.append(d.idOf(h));
  }
  ids.append(BlockId());
  sc.setSelection(*d.lib, ids);
  test_assert(sc.selection().size() == 3);
  Vector<BlockHandle> s = sc.selected(*d.lib);
  test_assert(s.size() == 2 && s[0] == c && s[1] == a);
  must(d.lib->markDeleted(c));
  test_assert(sc.selected(*d.lib).size() == 1);
  must(d.lib->restoreFromHistory(c));
  test_assert(sc.selected(*d.lib).size() == 2);
  must(d.lib->writeRef(sc.activeCell(), d.idOf(b)));
  test_assert(sc.active(*d.lib) == b);
  must(d.lib->onHostDestroyed(d.idOf(b)));
  test_assert(!sc.active(*d.lib));
  must(d.lib->onHostRestored(d.idOf(b)));
  test_assert(sc.active(*d.lib) == b);
  must(sc.remove(*d.lib, d.idOf(b)).status());
  test_assert(!sc.active(*d.lib) && !sc.activeCell().isNull());
}

/* A scene is a root, its members live through it, and their parents; the selection is weak. */
void testCollect()
{
  Doc d;
  const BlockHandle a = d.object("a"), parent = d.loose("parent"), picked = d.loose("picked");
  const BlockHandle data = d.shapeBlock("data", 1.0);
  d.setParent(a, parent);
  d.setData(a, data);
  Vector<BlockId> ids;
  ids.append(d.idOf(picked));
  d.sc().setSelection(*d.lib, ids);
  must(d.lib->writeRef(d.sc().activeCell(), d.idOf(picked)));
  test_assert(d.lib->usersOf(parent).size() == 1 && d.lib->usersOf(data).size() == 1);
  Result<int> n = d.lib->collect();
  test_assert(n.ok() && *n == 1);
  test_assert(d.lib->get(a) && d.lib->get(data) && d.lib->get(parent) && !d.lib->get(picked));
  test_assert(d.lib->get(d.scene));
  d.evaluate();
  test_assert(closeTo(d.result(a).bounds.max[0], 1.0));
  test_assert(d.lib->validate().ok());
}

/* ---- evaluation ---- */

void testHierarchy()
{
  Doc d;
  /* Children before parents in the scene. */
  const BlockHandle c = d.object("c", 1), b = d.object("b", 10), a = d.object("a", 100);
  d.setParent(c, b);
  d.setParent(b, a);
  const BlockHandle data = d.shapeBlock("data", 2.0);
  d.setData(c, data);
  ObjectEval e;
  test_assert(!d.sc().evaluated(c, e) && d.sc().evaluatedObjects().size() == 0);
  d.evaluate();
  test_assert(d.isAt(a, 100) && d.isAt(b, 110) && d.isAt(c, 111));
  test_assert(d.statusOf(c) == EvalStatus::Ok && d.result(c).member);
  e = d.result(c);
  test_assert(e.localBounds.min[0] == -2.0 && closeTo(e.bounds.min[0], 109.0) && closeTo(e.bounds.max[2], 2.0));
  test_assert(d.result(a).bounds.empty() && d.result(a).localBounds.empty());

  put(d.obj(a).channels, trs::kLocation, double3(200, 0, 0));
  d.shape(data).box.max[0] = 5.0;
  test_assert(d.isAt(c, 111)); /* until the next evaluation */
  d.evaluate();
  test_assert(d.isAt(c, 211) && closeTo(d.result(c).bounds.max[0], 216.0));
  const Scene::Counters n = d.sc().counters();
  test_assert(n.evaluations == 2 && n.revalidations == 1 && n.rebuilds == 1);

  /* Time. */
  d.addFnModel("test.time", [](const EvalCtx &ctx, const TransformState &p) {
    TransformState s = p;
    double *m = s.world;
    m[12] += ctx.time();
    return s;
  });
  d.obj(a).setTransformType("test.time");
  d.obj(a).channels = ChannelBlock(1);
  d.sc().frame = 36.0;
  d.sc().units.rate = FrameRate::nearest(24.0);
  d.evaluate();
  test_assert(d.isAt(a, 1.5) && d.isAt(c, 12.5));
  d.sc().units.startFrame = 12.0;
  d.evaluate();
  test_assert(d.isAt(a, 1.0) && d.sc().time() == 1.0);
}

void testOutsideParent()
{
  Doc d;
  const BlockHandle top = d.loose("top", double3(1000, 0, 0)),
                    parent = d.loose("parent", double3(100, 0, 0)), a = d.object("a", 1);
  d.setParent(a, parent);
  d.setParent(parent, top);
  d.evaluate();
  test_assert(d.isAt(a, 1101) && d.isAt(parent, 1100));
  test_assert(d.result(a).member && !d.result(parent).member && !d.result(top).member);
  const Vector<BlockHandle> all = d.sc().evaluatedObjects();
  test_assert(all.size() == 3 && all[0] == a && all[1] == parent && all[2] == top);

  /* A deleted parent counts as none, until it comes back. */
  must(d.lib->markDeleted(parent));
  d.evaluate();
  ObjectEval e;
  test_assert(d.isAt(a, 1) && !d.sc().evaluated(parent, e));
  must(d.lib->restoreFromHistory(parent));
  d.evaluate();
  test_assert(d.isAt(a, 1101));
  must(d.lib->onHostDestroyed(d.idOf(parent)));
  d.evaluate();
  test_assert(d.isAt(a, 1));
  must(d.lib->onHostRestored(d.idOf(parent)));
  d.evaluate();
  test_assert(d.isAt(a, 1101));

  /* A member that isn't Transformable is at the origin, also for its children. */
  const BlockHandle shape = d.shapeBlock("shape", 3.0);
  must(d.sc().add(*d.lib, shape));
  d.setParent(a, shape);
  d.evaluate();
  test_assert(d.statusOf(shape) == EvalStatus::NoTransform && d.isAt(a, 1));
  test_assert(closeTo(d.result(shape).bounds.max[0], 3.0));

  /* In another scene, an object has another place in the list, and the same world. */
  const BlockHandle other = d.addScene("Other");
  must(Scene::of(*d.lib, other)->add(*d.lib, a));
  must(evaluateScene(*d.ctx, other));
  test_assert(Scene::of(*d.lib, other)->evaluated(a, e) && e.member);
  test_assert(Scene::of(*d.lib, other)->evaluated(shape, e) && !e.member);
  test_assert(d.result(shape).member);
}

void testCycles()
{
  Doc d;
  const BlockHandle a = d.object("a", 1), b = d.object("b", 10), c = d.object("c", 100),
                    tail = d.object("tail", 1000), self = d.object("self", 5);
  d.setParent(a, b);
  d.setParent(b, c);
  d.setParent(c, a);
  d.setParent(tail, b);
  d.setParent(self, self);
  d.evaluate();
  for (BlockHandle h : {a, b, c, self}) {
    test_assert(d.statusOf(h) == EvalStatus::Cycle);
  }
  test_assert(d.isAt(a, 1) && d.isAt(b, 10) && d.isAt(c, 100) && d.isAt(self, 5));
  test_assert(d.statusOf(tail) == EvalStatus::Ok && d.isAt(tail, 1010));
  for (BlockHandle h : {a, b, c, self, tail}) {
    Result<TransformState> s = computeState(*d.lib, h, 0.0);
    test_assert(s.ok() && difference(s->world, d.result(h).state.world) == 0.0);
  }
  Result<TransformState> none = computeParentState(*d.lib, a, 0.0);
  test_assert(none.ok() && difference(none->world, TransformState().world) == 0.0);
  test_assert(solve::translationOf(computeParentState(*d.lib, tail, 0.0)->world)[0] == 10.0);
  test_assert(isSelfOrAncestor(*d.lib, a, a) && isSelfOrAncestor(*d.lib, c, a) &&
              isSelfOrAncestor(*d.lib, a, tail) && !isSelfOrAncestor(*d.lib, tail, a) &&
              !isSelfOrAncestor(*d.lib, self, a));
  /* Opened again. */
  d.setParent(c, BlockHandle());
  d.evaluate();
  test_assert(d.isAt(a, 111) && d.isAt(tail, 1110) && d.statusOf(a) == EvalStatus::Ok);
}

void testFailures()
{
  Doc d;
  const double nan = std::numeric_limits<double>::quiet_NaN();
  int throws = 0;
  d.addFnModel("test.throws", [&](const EvalCtx &, const TransformState &) {
    throws++;
    d.rt.setStickyError(boom(throws == 1 ? "first" : "later"));
    return TransformState();
  });
  const BlockHandle a = d.object("a", 1), b = d.object("b", 10), c = d.object("c", 100);
  d.setParent(b, a);
  d.setParent(c, b);
  const BlockHandle data = d.shapeBlock("data", 1.0);
  d.setData(b, data);
  const ChannelBlock good = d.obj(b).channels;

  d.obj(b).channels.set(trs::kLocation + 1, nan);
  d.evaluate();
  test_assert(d.statusOf(b) == EvalStatus::Invalid && d.isAt(b, 1) && d.isAt(c, 101));
  test_assert(d.statusOf(c) == EvalStatus::Ok && closeTo(d.result(b).bounds.max[0], 2.0));
  Result<TransformState> s = computeState(*d.lib, c, 0.0);
  test_assert(s.ok() && difference(s->world, d.result(c).state.world) == 0.0);

  d.obj(b).channels = ChannelBlock(2); /* another type's */
  d.evaluate();
  test_assert(d.statusOf(b) == EvalStatus::Invalid && d.isAt(c, 101));

  d.obj(b).channels = good;
  d.obj(b).setTransformType("test.missing");
  d.evaluate();
  test_assert(d.statusOf(b) == EvalStatus::MissingModel && d.isAt(c, 101));
  test_assert(isError(setWorldOf(*d.lib, b, mat4d(), SetWorldMode::Exact, 0.0).status(),
                      StatusCode::FailedPrecondition,
                      "isn't registered"));

  /* Overflow. */
  d.obj(b).setTransformType(kTrsTransform);
  put(d.obj(b).channels, trs::kScale, double3(1e200, 1e200, 1e200));
  put(d.obj(c).channels, trs::kScale, double3(1e200, 1e200, 1e200));
  d.evaluate();
  test_assert(d.statusOf(b) == EvalStatus::Ok && d.statusOf(c) == EvalStatus::Invalid);
  /* Finite corners, and a world that is, but not their product. */
  d.shape(data).box.max[0] = 1e200;
  d.evaluate();
  test_assert(d.result(b).bounds.empty() && d.result(b).localBounds.max[0] == 1e200);
  d.shape(data).box.max[0] = nan;
  d.evaluate();
  test_assert(d.result(b).localBounds.empty() && d.result(b).bounds.empty());
  put(d.obj(c).channels, trs::kScale, double3(1, 1, 1));
  d.obj(b).channels = good;

  /* A model that throws: everything else is computed, and the first error comes back. */
  for (BlockHandle h : {b, a}) {
    d.obj(h).setTransformType("test.throws");
    d.obj(h).channels = ChannelBlock(1);
  }
  test_assert(isError(evaluateScene(*d.ctx), StatusCode::Internal, "first") && throws == 2);
  test_assert(!d.rt.hasStickyError());
  test_assert(d.statusOf(a) == EvalStatus::ModelFailed &&
              d.statusOf(b) == EvalStatus::ModelFailed && d.isAt(c, 100));
  test_assert(isError(computeState(*d.lib, c, 0.0).status(), StatusCode::Internal, "later"));
  test_assert(!d.rt.hasStickyError());

  /* So is a throwing read, and bounds. */
  for (BlockHandle h : {a, b}) {
    d.obj(h).setTransformType(kTrsTransform);
    d.obj(h).channels = good;
  }
  const BlockHandle h = d.hostObject("host", 7.0);
  d.setParent(c, h);
  d.host(h).onRead = [&](ObjectInfo &) { d.rt.setStickyError(boom("read")); };
  test_assert(isError(evaluateScene(*d.ctx), StatusCode::Internal, "read"));
  test_assert(d.statusOf(h) == EvalStatus::ModelFailed && d.isAt(h, 0) && d.isAt(c, 100));
  test_assert(isError(computeState(*d.lib, h, 0.0).status(), StatusCode::Internal, "read"));
  test_assert(isError(readObject(*d.lib, h).status(), StatusCode::Internal, "read"));
  /* What a throwing read answered doesn't count. */
  const BlockHandle h2 = d.hostObject("host2", 7.0);
  d.host(h2).onRead = [&](ObjectInfo &info) {
    info.parent = d.idOf(b);
    d.rt.setStickyError(boom("read"));
  };
  test_assert(isError(evaluateScene(*d.ctx), StatusCode::Internal, "read"));
  test_assert(d.isAt(h2, 0) && !d.isAt(b, 0));
  d.host(h2).onRead = nullptr;
  d.host(h).onRead = nullptr;
  d.evaluate();
  test_assert(d.isAt(h, 7) && d.isAt(c, 107));
  d.shape(data).box.max[0] = 2.0;
  d.shape(data).onBounds = [&] { d.rt.setStickyError(boom("bounds")); };
  test_assert(isError(evaluateScene(*d.ctx), StatusCode::Internal, "bounds"));
  test_assert(d.statusOf(b) == EvalStatus::Ok && d.result(b).bounds.empty());
  d.shape(data).onBounds = nullptr;
  d.evaluate();
  test_assert(d.result(b).localBounds.max[0] == 2.0);

  /* Refusals keep the results. */
  d.rt.setStickyError(boom("pending"));
  test_assert(isError(evaluateScene(*d.ctx), StatusCode::FailedPrecondition, "pending"));
  test_assert(isError(computeState(*d.lib, a, 0.0).status(), StatusCode::FailedPrecondition));
  test_assert(isError(readObject(*d.lib, a).status(), StatusCode::FailedPrecondition));
  test_assert(isError(writeTransform(*d.ctx, a, good), StatusCode::FailedPrecondition));
  test_assert(d.rt.hasStickyError() && d.rt.takeStickyError().code() == StatusCode::Internal);
  test_assert(isError(evaluateScene(*d.ctx, a), StatusCode::NotFound));
  Context none(&d.rt, nullptr, PointerRef());
  test_assert(isError(evaluateScene(none), StatusCode::FailedPrecondition));
  test_assert(isError(computeState(*d.lib, data, 0.0).status(),
                      StatusCode::FailedPrecondition,
                      "'data'"));
  test_assert(d.isAt(h, 7));
  must(d.lib->markDeleted(d.scene));
  test_assert(isError(evaluateScene(*d.ctx), StatusCode::NotFound));
  test_assert(!activeScene(*d.ctx));
  must(d.lib->restoreFromHistory(d.scene));
  d.evaluate();
}

/* What a slot may do while a scene evaluates. */
void testReentry()
{
  Doc d;
  const BlockHandle a = d.hostObject("a", 1.0), b = d.object("b", 10.0);
  d.evaluate();
  d.host(a).channels.set(trs::kLocation, 2.0);
  put(d.obj(b).channels, trs::kLocation, double3(20, 0, 0));
  int calls = 0;
  d.host(a).onRead = [&](ObjectInfo &) {
    calls++;
    /* The results are the previous ones, also those already computed. */
    ObjectEval e;
    test_assert(d.sc().evaluated(a, e) && solve::translationOf(e.state.world)[0] == 1.0);
    test_assert(d.sc().evaluated(b, e) && solve::translationOf(e.state.world)[0] == 10.0);
    test_assert(isError(evaluateScene(*d.ctx), StatusCode::Busy));
    test_assert(isError(computeState(*d.lib, b, 0.0).status(), StatusCode::Busy));
    test_assert(isError(computeParentState(*d.lib, b, 0.0).status(), StatusCode::Busy));
    test_assert(isError(
        setWorldOf(*d.lib, b, mat4d(), SetWorldMode::Exact, 0.0).status(), StatusCode::Busy));
    test_assert(isError(writeTransform(*d.ctx, b, ChannelBlock()), StatusCode::Busy));
    test_assert(isError(writeParent(*d.ctx, b, BlockId()), StatusCode::Busy));
    test_assert(isError(setValue(*d.ctx, "scene.frame", PropValue::ofFloat(3.0)),
                        StatusCode::Busy));
    test_assert(isError(d.lib->collect().status(), StatusCode::Busy));
  };
  d.evaluate();
  test_assert(calls == 1 && d.isAt(a, 2) && d.isAt(b, 20));
  test_assert(d.lib->collect().ok() && !d.rt.evaluating());

  /* One that deletes the scene and drops it from the history, or adds and removes members. */
  BlockHandle added;
  d.host(a).onRead = [&](ObjectInfo &) {
    if (!added) {
      added = d.object("added", 5.0);
      must(d.sc().remove(*d.lib, d.idOf(b)).status());
    }
  };
  d.evaluate();
  ObjectEval e;
  test_assert(d.sc().evaluated(b, e) && !d.sc().evaluated(added, e));
  d.evaluate();
  test_assert(!d.sc().evaluated(b, e) && d.isAt(added, 5));
  /* One that destroys a member that wasn't looked up yet. */
  const BlockHandle last = d.object("last");
  bool destroyed = false;
  d.host(a).onRead = [&](ObjectInfo &) {
    if (!destroyed) {
      destroyed = true;
      must(d.lib->markDeleted(last));
      must(d.lib->destroyFromHistory(last));
    }
  };
  d.evaluate();
  test_assert(destroyed && !d.sc().evaluated(last, e) && d.isAt(added, 5));
  bool deleted = false;
  d.host(a).onRead = [&](ObjectInfo &) {
    if (!deleted) {
      deleted = true;
      must(d.lib->markDeleted(d.scene));
      must(d.lib->destroyFromHistory(d.scene));
    }
  };
  evaluateScene(*d.ctx).ignore();
  test_assert(!d.lib->get(d.scene));
}

/* Deeper than a stack would take. */
void testDeep()
{
  Doc d;
  constexpr int n = 100000;
  Vector<BlockHandle> all;
  for (int i = 0; i < n; i++) {
    all.append(d.object("o", 1.0));
    if (i > 0) {
      d.setParent(all[i - 1], all[i]); /* the last one is the root */
    }
  }
  d.evaluate();
  test_assert(d.isAt(all[0], n) && d.isAt(all[n - 1], 1));
  Result<TransformState> s = computeState(*d.lib, all[0], 0.0);
  test_assert(s.ok() && solve::translationOf(s->world)[0] == n);
  test_assert(isSelfOrAncestor(*d.lib, all[n - 1], all[0]));
}

/* ---- capabilities ---- */

void testCapabilities()
{
  Doc d;
  const BlockHandle a = d.object("a", 3.0), data = d.shapeBlock("data", 1.0);
  test_assert(!objectQuery<Spatial>(*d.lib, a));
  d.setData(a, data);
  const Spatial sp = objectQuery<Spatial>(*d.lib, a);
  test_assert(sp && sp.localBounds().max[0] == 1.0);
  test_assert(bool(objectQuery<Transformable>(*d.lib, a)));
  test_assert(bool(objectQuery<Spatial>(*d.lib, data)) && !objectQuery<ElementAccess>(*d.lib, a));

  ChangeBus &bus = d.rt.ext<ChangeBus>();
  Vector<OwnerKey> heard;
  ScopedConnection c = bus.addListener([&](const OwnerKey *k) { heard.append(*k); });
  must(invalidateData(*d.ctx, data, InvalidateKinds::Positions | InvalidateKinds::Selection));
  test_assert(d.shape(data).invalidated ==
              uint32_t(InvalidateKinds::Positions | InvalidateKinds::Selection));
  test_assert(heard.size() == 1 && heard[0] == OwnerKey::ofBlock(d.idOf(data)));
  must(invalidateData(*d.ctx, a, InvalidateKinds::Topology)); /* has no Invalidation */
  test_assert(heard.size() == 2 && heard[1] == OwnerKey::ofBlock(d.idOf(a)));
  test_assert(
      isError(invalidateData(*d.ctx, BlockHandle(), InvalidateKinds::None), StatusCode::NotFound));

  /* Writing. */
  ChannelBlock ch = d.trs->defaults();
  put(ch, trs::kLocation, double3(8, 0, 0));
  must(writeTransform(*d.ctx, a, ch));
  test_assert(d.obj(a).channels == ch && heard.size() == 3);
  must(writeTransformType(*d.ctx, a, d.matrix->id(), d.matrix->defaults()));
  test_assert(d.obj(a).transformTypeId() == d.matrix->id() &&
              d.obj(a).channels == d.matrix->defaults());
  d.obj(a).setTransformType("test.notHere"); /* a missing type's name can be set again */
  must(writeTransformType(*d.ctx, a, d.matrix->id(), d.matrix->defaults()));
  must(writeTransformType(*d.ctx, a, TypeId::of("test.notHere"), ch));
  test_assert(!strcmp(d.obj(a).transformType().c_str(), "test.notHere"));
  test_assert(isError(writeTransformType(*d.ctx, a, TypeId::of("test.unknown"), ch),
                      StatusCode::NotFound));
  const BlockHandle h = d.hostObject("host");
  test_assert(isError(writeTransformType(*d.ctx, h, d.matrix->id(), ch), StatusCode::Unsupported));
  test_assert(isError(writeParent(*d.ctx, h, d.idOf(a)), StatusCode::Unsupported));
  d.host(h).onWrite = [] { return boom("refused"); };
  heard.clear();
  test_assert(isError(writeTransform(*d.ctx, h, ch), StatusCode::Internal, "refused"));
  d.host(h).onWrite = [&] {
    d.rt.setStickyError(boom("threw"));
    return Status();
  };
  test_assert(isError(writeTransform(*d.ctx, h, ch), StatusCode::Internal, "threw"));
  test_assert(heard.size() == 0 && !d.rt.hasStickyError());
  const uint64_t epoch = d.lib->epoch();
  must(writeParent(*d.ctx, a, d.idOf(h)));
  test_assert(d.lib->epoch() != epoch && heard.size() == 1);
  test_assert(d.lib->resolveRef(d.obj(a).parent.cell()) == h);
  must(writeParent(*d.ctx, a, BlockId()));
  test_assert(d.obj(a).parent.cell().isNull());
  test_assert(isError(newObject(*d.lib, "x", "test.unknown").status(), StatusCode::NotFound));
}

/* setWorldOf puts an object where it is asked to, under its parents as they are now. */
void testSetWorld()
{
  Doc d;
  Rand r(11);
  for (int i = 0; i < 200; i++) {
    const BlockHandle p = d.object("p"), q = d.object("q"), o = d.object("o");
    d.setParent(q, p);
    d.setParent(o, q);
    for (BlockHandle h : {p, q, o}) {
      /* Uniform, so that a TRS child can be anywhere. */
      const double s = r.range(0.2, 3.0);
      d.obj(h).channels = trsChannels(r, *d.trs, double3(s, s, s), 10.0);
    }
    const BlockHandle other = d.object("other");
    d.obj(other).channels = trsChannels(r, *d.trs, r.scale(4.0, true), 10.0);
    const TransformState want = *computeState(*d.lib, other, 0.0);
    Result<ChannelBlock> ch = setWorldOf(*d.lib, o, want.world, SetWorldMode::Exact, 0.0);
    test_assert(ch.ok());
    if (ch.ok()) {
      must(writeTransform(*d.ctx, o, *ch));
      const TransformState got = *computeState(*d.lib, o, 0.0);
      test_assert(difference(got.world, want.world) < 1e-7);
    }
  }
}

/* ---- at random, against a recursion over the same data ---- */

struct Oracle {
  Doc &d;
  Vector<BlockHandle> path;

  bool live(BlockHandle h)
  {
    const Datablock *b = d.lib->get(h);
    return b && b->state() == BlockState::Live;
  }
  BlockHandle parentOf(BlockHandle h)
  {
    const BlockId id = d.lib->get(h)->type() == TypeId::of(kObjectType) ?
                           d.obj(h).parent.target() :
                           d.host(h).info.parent;
    const BlockHandle p = id.isNull() ? BlockHandle() : d.lib->find(id);
    return live(p) ? p : BlockHandle();
  }
  bool onCycle(BlockHandle h)
  {
    BlockHandle at = parentOf(h);
    for (int i = 0; at && i < 1000; i++) {
      if (at == h) {
        return true;
      }
      at = parentOf(at);
    }
    return false;
  }
  TransformState state(BlockHandle h, EvalStatus &status)
  {
    const bool cycle = onCycle(h);
    TransformState parent;
    const BlockHandle p = cycle ? BlockHandle() : parentOf(h);
    if (p) {
      EvalStatus ignored;
      parent = state(p, ignored);
    }
    const bool own = d.lib->get(h)->type() == TypeId::of(kObjectType);
    const TypeId type = own ? d.obj(h).transformTypeId() : d.host(h).info.transformType;
    const TransformType *t = d.rt.ext<TransformRegistry>().find(type);
    if (!t) {
      status = EvalStatus::MissingModel;
      return parent;
    }
    Result<TransformState> s =
        evaluateTransform(*t, EvalCtx(own ? d.obj(h).channels : d.host(h).channels), parent);
    if (!s.ok() || !s->finite()) {
      status = EvalStatus::Invalid;
      return parent;
    }
    status = cycle ? EvalStatus::Cycle : EvalStatus::Ok;
    return *s;
  }
};

void testRandom()
{
  const int steps = 400 * coredcc_test::testScale();
  Doc d;
  Rand r(5);
  Vector<BlockHandle> all, shapes;
  for (int i = 0; i < 4; i++) {
    shapes.append(d.shapeBlock("shape", i + 1.0));
    d.lib->setFlag(shapes[i], BlockFlags::FakeUser, true);
  }
  for (int i = 0; i < 30; i++) {
    const BlockHandle h = r.below(4) == 0 ? d.hostObject("h", r.range(-5, 5)) : d.object("o");
    d.lib->setFlag(h, BlockFlags::FakeUser, true);
    all.append(h);
  }
  Oracle oracle{d, {}};
  int cycles = 0, outsiders = 0, invalid = 0, every = 0, some = 0, none = 0;
  for (int step = 0; step < steps; step++) {
    const int changes = int(r.below(4));
    for (int k = 0; k < changes; k++) {
      const BlockHandle h = all[r.below(30)];
      const bool own = d.lib->get(h)->type() == TypeId::of(kObjectType);
      const BlockHandle other = r.below(4) == 0 ? BlockHandle() : all[r.below(30)];
      const BlockState state = d.lib->get(h)->state();
      switch (r.below(8)) {
        case 0:
        case 1: /* a host's parent changes without anyone hearing of it */
          if (own) {
            d.setParent(h, other);
          }
          else {
            d.host(h).info.parent = other ? d.idOf(other) : BlockId();
          }
          break;
        case 2:
          if (d.sc().contains(*d.lib, h)) {
            must(d.sc().remove(*d.lib, d.idOf(h)).status());
          }
          else {
            must(d.sc().add(*d.lib, h, int(r.below(40)) - 1));
          }
          break;
        case 3:
          if (state == BlockState::Live) {
            must(r.coin() ? d.lib->markDeleted(h) : d.lib->onHostDestroyed(d.idOf(h)));
          }
          else if (state == BlockState::InHistory) {
            must(d.lib->restoreFromHistory(h));
          }
          else {
            must(d.lib->onHostRestored(d.idOf(h)));
          }
          break;
        case 4:
          if (own) {
            const bool fbx = r.coin();
            d.obj(h).setTransformType(fbx ? kFbxTransform : kTrsTransform);
            d.obj(h).channels = fbx ? fbxChannels(r, *d.fbx, int(r.below(3)), r.scale(2, true), 2.0) :
                                      trsChannels(r, *d.trs, r.scale(2, true), 5.0);
            if (r.below(10) == 0) {
              d.obj(h).channels.set(0, std::numeric_limits<double>::infinity());
            }
          }
          break;
        case 5:
          if (own) {
            d.setData(h, r.coin() ? shapes[r.below(4)] : BlockHandle());
          }
          else {
            d.host(h).info.data = r.coin() ? d.idOf(shapes[r.below(4)]) : BlockId();
          }
          break;
        case 6:
          d.shape(shapes[r.below(4)]).box.max[r.below(3)] = r.range(0, 9);
          break;
        default: {
          Vector<BlockId> ids;
          ids.append(d.idOf(h));
          d.sc().setSelection(*d.lib, ids);
        }
      }
    }
    const Vector<BlockHandle> wasThere = d.sc().evaluatedObjects();
    Vector<ObjectEval> was;
    for (BlockHandle h : wasThere) {
      was.append(d.result(h));
    }
    const uint64_t version = d.sc().lastChanges().version;
    d.evaluate();

    /* What changed, by comparing all of it. */
    const Scene::Changes &last = d.sc().lastChanges();
    const Vector<BlockHandle> isThere = d.sc().evaluatedObjects();
    bool others = step == 0 || isThere.size() != wasThere.size();
    for (int i = 0; !others && i < int(isThere.size()); i++) {
      others = isThere[i] != wasThere[i];
    }
    if (others) {
      test_assert(last.version == version + 1 && last.all && last.places.size() == 0);
      every++;
    }
    else {
      Vector<int32_t> differ;
      for (int i = 0; i < int(isThere.size()); i++) {
        if (!d.result(isThere[i]).same(was[i])) {
          differ.append(i);
        }
      }
      test_assert(last.version == version + (differ.size() ? 1 : 0));
      if (differ.size()) {
        Vector<int32_t> places = last.places;
        std::sort(places.data(), places.data() + places.size());
        test_assert(!last.all && places.size() == differ.size() &&
                    std::equal(places.data(), places.data() + places.size(), differ.data()));
        /* Parents before children. */
        for (int i = 0; i < int(last.places.size()); i++) {
          const BlockHandle h = isThere[last.places[i]];
          if (d.result(h).status != EvalStatus::Cycle) {
            const BlockHandle p = oracle.parentOf(h);
            for (int k = i + 1; k < int(last.places.size()); k++) {
              test_assert(isThere[last.places[k]] != p);
            }
          }
        }
        some += int(differ.size()) < int(isThere.size());
      }
      none += differ.size() == 0;
    }

    /* The members, in order, then every ancestor, once. */
    Vector<BlockHandle> want;
    for (const RefCell &c : d.sc().cells()) {
      const BlockHandle h = d.lib->find(c.target);
      if (oracle.live(h)) {
        want.append(h);
      }
    }
    const int members = int(want.size());
    const Vector<BlockHandle> got = d.sc().evaluatedObjects();
    bool same = got.size() >= want.size();
    for (int i = 0; same && i < members; i++) {
      same = got[i] == want[i];
    }
    for (int i = 0; i < int(want.size()); i++) {
      const BlockHandle p = oracle.parentOf(want[i]);
      if (p && !want.contains(p)) {
        want.append(p);
      }
    }
    same = same && got.size() == want.size();
    for (int i = 0; same && i < int(want.size()); i++) {
      same = got.contains(want[i]);
    }
    test_assert(same);
    outsiders += int(want.size()) - members;
    for (int i = 0; same && i < int(want.size()); i++) {
      const BlockHandle h = want[i];
      EvalStatus status;
      const TransformState s = oracle.state(h, status);
      const ObjectEval e = d.result(h);
      test_assert(e.status == status && e.member == (i < members) && sameBits(e.state, s));
      cycles += status == EvalStatus::Cycle;
      invalid += status == EvalStatus::Invalid;
      const bool own = d.lib->get(h)->type() == TypeId::of(kObjectType);
      const BlockId data = own ? d.obj(h).data.target() : d.host(h).info.data;
      const Box3d local = data.isNull() ? Box3d() : d.shape(d.lib->find(data)).box;
      const Box3d world = transformBox(local, static_cast<const double *>(s.world));
      test_assert(!memcmp(&e.localBounds, &local, sizeof(Box3d)) &&
                  !memcmp(&e.bounds, &world, sizeof(Box3d)));
      Result<TransformState> c = computeState(*d.lib, h, 0.0);
      test_assert(c.ok() && sameBits(*c, s));
    }
  }
  const Scene::Counters n = d.sc().counters();
  fprintf(stderr,
          "random: %d cycles, %d outsiders, %d invalid, %d revalidations, %d rebuilds\n",
          cycles,
          outsiders,
          invalid,
          n.revalidations,
          n.rebuilds);
  fprintf(stderr, "changes: %d times all, %d some, %d none\n", every, some, none);
  test_assert(every > 50 && some > 20 && none > 20);
  test_assert(cycles > 50 && outsiders > 50 && invalid > 50 && n.revalidations > n.rebuilds);
}

} // namespace

int main()
{
  testMembers();
  testSelection();
  testCollect();
  testHierarchy();
  testOutsideParent();
  testCycles();
  testFailures();
  testReentry();
  testDeep();
  testCapabilities();
  testSetWorld();
  testRandom();
  return test_end();
}
