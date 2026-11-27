/* What evaluateScene caches: changes that don't move the Library's epoch, and what it
 * keeps when nothing differs. */
#include "scene_doc.h"

test_init;

using namespace scene_test;

namespace {

/* A provider that begins to answer, stops, and answers something else. */
void testQueryInterface()
{
  Doc d;
  const BlockHandle a = d.hostObject("a", 1.0);
  Shape one, two;
  for (int i = 0; i < 3; i++) {
    one.box.min[i] = -1.0;
    one.box.max[i] = 1.0;
  }
  two = one;
  two.box.max[1] = 4.0;
  Shape *answer = nullptr;
  d.query = [&](const Datablock &b, InterfaceId iface) {
    QueryAnswer q;
    if (!strcmp(b.name().c_str(), "a") && iface == InterfaceId::of(Spatial::ifaceName) &&
        answer)
    {
      q.implType = TypeId::of("test.Shape");
      q.self = answer;
    }
    return q;
  };
  d.evaluate();
  test_assert(d.result(a).localBounds.empty());
  const uint64_t epoch = d.lib->epoch();
  answer = &one;
  d.evaluate();
  test_assert(d.result(a).localBounds.max[1] == 1.0);
  answer = &two;
  d.evaluate();
  test_assert(d.result(a).localBounds.max[1] == 4.0);
  answer = nullptr;
  d.evaluate();
  test_assert(d.result(a).localBounds.empty());
  test_assert(d.lib->epoch() == epoch && d.sc().counters().revalidations == 1);
  /* One that throws. */
  d.query = [&](const Datablock &, InterfaceId) {
    d.rt.setStickyError(boom("query"));
    return QueryAnswer();
  };
  test_assert(isError(evaluateScene(*d.ctx), StatusCode::Internal, "query"));
  test_assert(d.result(a).localBounds.empty() && d.isAt(a, 1) && !d.rt.hasStickyError());
  d.query = nullptr;
  d.evaluate();
}

/* An implementation that is registered after an evaluation. */
void testLateImpl()
{
  Doc d;
  d.addSlotType("test.Late", litestl::alloc::New<PlainProvider>("provider"));
  const BlockHandle a = d.addSlot<Host>("test.Late", "late");
  d.host(a).info.transformType = TypeId::of(kTrsTransform);
  d.host(a).channels = d.trs->defaults();
  d.host(a).channels.set(trs::kLocation, 3.0);
  must(d.sc().add(*d.lib, a));
  d.evaluate();
  test_assert(d.statusOf(a) == EvalStatus::NoTransform);
  const uint64_t epoch = d.lib->epoch();
  must(registerImpl<Transformable, Host>(d.rt, "test.Late"));
  d.evaluate();
  test_assert(d.lib->epoch() == epoch && d.statusOf(a) == EvalStatus::Ok && d.isAt(a, 3));
}

/* Data that a host moves, takes away and brings back, while the block is Live. */
void testData()
{
  Doc d;
  const BlockHandle a = d.hostObject("a", 1.0), b = d.object("b", 10.0);
  const BlockHandle shape = d.shapeBlock("shape", 1.0);
  d.setData(b, shape);
  d.setParent(b, a);
  d.evaluate();
  test_assert(d.isAt(b, 11));
  const uint64_t epoch = d.lib->epoch();
  /* The host notifies the block whose data it changed (evaluateScene evaluates only
   * those). */
  const auto swapIn = [&](BlockHandle h, void *data) {
    d.slot(h).data = data;
    d.rt.ext<ChangeBus>().notify(OwnerKey::ofBlock(d.idOf(h)));
  };
  Host moved = d.host(a);
  moved.channels.set(trs::kLocation, 2.0);
  Shape bigger = d.shape(shape);
  bigger.box.max[0] = 7.0;
  void *oldHost = d.slot(a).data, *oldShape = d.slot(shape).data;
  swapIn(a, &moved);
  swapIn(shape, &bigger);
  d.evaluate();
  test_assert(d.isAt(b, 12) && d.result(b).localBounds.max[0] == 7.0);
  test_assert(d.sc().counters().rebuilds == 1);

  swapIn(a, nullptr);
  d.evaluate();
  test_assert(d.statusOf(a) == EvalStatus::NoTransform && d.isAt(b, 10));
  swapIn(shape, nullptr);
  d.evaluate();
  test_assert(d.result(b).localBounds.empty());
  swapIn(a, oldHost);
  swapIn(shape, oldShape);
  d.evaluate();
  test_assert(d.isAt(b, 11) && d.result(b).localBounds.max[0] == 1.0);
  test_assert(d.lib->epoch() == epoch);
}

/* A client name that becomes an alias makes raw ids resolve. */
void testAlias()
{
  Doc d;
  ClientRegistry &clients = d.rt.ext<ClientRegistry>();
  const BlockHandle parent = d.loose("parent", double3(100, 0, 0));
  const BlockHandle a = d.object("a", 1.0), b = d.object("b", 2.0);
  BlockId old = d.idOf(parent);
  old.client = ClientId::of("old.client");
  d.obj(a).parent.cell().target = old;
  must(d.sc().add(*d.lib, parent));
  must(d.lib->writeRef(const_cast<RefCell &>(d.sc().cells()[2]), old));
  Vector<BlockId> chosen;
  chosen.append(old);
  d.sc().setSelection(*d.lib, chosen);
  test_assert(d.sc().selected(*d.lib).size() == 0);
  d.evaluate();
  ObjectEval e;
  test_assert(d.isAt(a, 1) && d.isAt(b, 2) && !d.sc().evaluated(parent, e));
  test_assert(!d.sc().contains(*d.lib, parent));
  const uint64_t epoch = d.lib->epoch();
  const char *aliases[] = {"old.client"};
  test_assert(d.sc().selected(*d.lib).size() == 0);
  must(clients.add(kCoredccClient, aliases).status());
  test_assert(d.sc().selected(*d.lib).size() == 1 &&
              d.sc().selected(*d.lib)[0] == parent);
  d.evaluate();
  test_assert(d.lib->epoch() == epoch);
  test_assert(d.isAt(a, 101) && d.isAt(parent, 100) && d.result(parent).member);
  test_assert(d.sc().contains(*d.lib, parent) && d.sc().objects(*d.lib).size() == 3);
  test_assert(isError(d.sc().add(*d.lib, parent), StatusCode::AlreadyExists));
}

/* Overrides. */
void testOverride()
{
  Doc d;
  const BlockHandle a = d.object("a", 1.0);
  d.evaluate();
  Host host;
  host.info.transformType = TypeId::of(kTrsTransform);
  host.channels = d.trs->defaults();
  host.channels.set(trs::kLocation, 50.0);
  const InterfaceId iface = InterfaceId::of(Transformable::ifaceName);
  const IfaceTable *table = d.rt.ifaces().find(TypeId::of("test.Host"), iface);
  must(d.lib->setOverride(a, iface, IfaceRef(table, &host)));
  d.evaluate();
  test_assert(d.isAt(a, 50));
  Host second = host;
  second.channels.set(trs::kLocation, 60.0);
  must(d.lib->setOverride(a, iface, IfaceRef(table, &second)));
  d.evaluate();
  test_assert(d.isAt(a, 60));
  d.lib->clearOverride(a, iface);
  d.evaluate();
  test_assert(d.isAt(a, 1));
  /* Another interface's. */
  Shape shape;
  shape.box.min[0] = shape.box.min[1] = shape.box.min[2] = 0.0;
  shape.box.max[0] = shape.box.max[1] = shape.box.max[2] = 2.0;
  const InterfaceId spatial = InterfaceId::of(Spatial::ifaceName);
  const IfaceTable *shapes = d.rt.ifaces().find(TypeId::of("test.Shape"), spatial);
  must(d.lib->setOverride(a, spatial, IfaceRef(shapes, &shape)));
  d.evaluate();
  test_assert(d.isAt(a, 1) && d.result(a).localBounds.max[2] == 2.0);
  d.lib->clearOverride(a, spatial);
}

void testKeeps()
{
  Doc d;
  const BlockHandle a = d.object("a", 1.0), b = d.object("b", 2.0),
                    c = d.object("c", 3.0);
  d.setParent(a, b);
  d.evaluate();
  /* Selecting, and other blocks, move the epoch; nothing is sorted again. */
  Vector<BlockId> ids;
  ids.append(d.idOf(c));
  d.sc().setSelection(*d.lib, ids);
  d.evaluate();
  d.loose("elsewhere");
  d.evaluate();
  const Scene::Counters n = d.sc().counters();
  test_assert(n.evaluations == 3 && n.revalidations == 3 && n.rebuilds == 1);
  /* Same parents, other order. */
  must(d.sc().remove(*d.lib, d.idOf(b)).status());
  must(d.sc().add(*d.lib, b));
  d.evaluate();
  test_assert(d.sc().counters().rebuilds == 2 && d.sc().evaluatedObjects()[2] == b);

  /* A parent link to another object of the same scene. */
  d.setParent(a, c);
  d.evaluate();
  test_assert(d.isAt(a, 4) && d.sc().counters().rebuilds == 3);

  /* Unlinked objects keep their last result until the next evaluation. */
  must(d.sc().remove(*d.lib, d.idOf(b)).status());
  ObjectEval e;
  test_assert(d.sc().evaluated(b, e) && d.isAt(b, 2));
  d.evaluate();
  test_assert(!d.sc().evaluated(b, e) && e.member);

  /* So do deleted ones; the handle can't name another block. */
  must(d.lib->markDeleted(c));
  must(d.lib->destroyFromHistory(c));
  test_assert(d.sc().evaluated(c, e));
  d.evaluate();
  test_assert(!d.sc().evaluated(c, e) && d.isAt(a, 1));
  /* Emptied. */
  must(d.sc().remove(*d.lib, d.idOf(a)).status());
  d.evaluate();
  test_assert(d.sc().evaluatedObjects().size() == 0 && d.sc().prune(*d.lib) == 1);

  /* The same list, place and transform, but another object. */
  const BlockHandle x = d.object("x", 5.0), y = d.loose("y", double3(5, 0, 0));
  d.evaluate();
  must(d.sc().remove(*d.lib, d.idOf(x)).status());
  must(d.sc().add(*d.lib, y));
  d.evaluate();
  test_assert(d.isAt(y, 5) && !d.sc().evaluated(x, e));
  /* A member that is only a parent from now on, at the same place of the list. */
  must(d.sc().add(*d.lib, x));
  d.setParent(y, x);
  d.evaluate();
  test_assert(d.result(x).member && d.isAt(y, 10));
  must(d.sc().remove(*d.lib, d.idOf(x)).status());
  d.evaluate();
  test_assert(!d.result(x).member && d.isAt(y, 10));
  /* Local bounds that make no difference in the world. */
  const BlockHandle flat = d.shapeBlock("flat", 1.0);
  d.setData(y, flat);
  put(d.obj(y).channels, trs::kScale, double3(0, 0, 0));
  d.evaluate();
  d.shape(flat).box.max[0] = 3.0;
  d.evaluate();
  test_assert(d.result(y).localBounds.max[0] == 3.0);

  /* A handle's slot that another block has by now. */
  d.lib->setQuarantine(false);
  const BlockHandle first = d.object("first");
  d.evaluate();
  must(d.lib->markDeleted(first));
  must(d.lib->destroyFromHistory(first));
  const BlockHandle second = d.loose("second");
  test_assert(second.index == first.index && second != first);
  test_assert(d.sc().evaluated(first, e) && !d.sc().evaluated(second, e));

  /* Two Libraries of one Runtime. */
  Library other(&d.rt);
  Result<BlockHandle> s2 = newScene(other, "Scene");
  must(s2.status());
  Result<BlockHandle> o2 = newObject(other, "o", kTrsTransform);
  must(o2.status());
  must(Scene::of(other, *s2)->add(other, *o2));
  Context ctx2(&d.rt, &other, d.ctx->root());
  ctx2.setScene(*s2);
  must(evaluateScene(ctx2));
  test_assert(Scene::of(other, *s2)->evaluated(*o2, e));
  test_assert(activeScene(ctx2) == Scene::of(other, *s2));
}

/* A read that says something else each time. */
void testUnstable()
{
  Doc d;
  const BlockHandle a = d.object("a", 1.0), b = d.object("b", 2.0),
                    h = d.hostObject("h", 5.0);
  d.evaluate();
  d.host(h).channels.set(trs::kLocation, 6.0);
  int calls = 0;
  d.host(h).onRead = [&](ObjectInfo &info) { info.parent = d.idOf(calls++ % 2 ? a : b); };
  test_assert(
      isError(evaluateScene(*d.ctx), StatusCode::FailedPrecondition, "each time"));
  test_assert(d.isAt(h, 5) && !d.rt.evaluating() && d.lib->collect().ok());
  d.host(h).onRead = [&](ObjectInfo &info) {
    info.data = calls++ % 2 ? d.idOf(a) : BlockId();
  };
  test_assert(
      isError(evaluateScene(*d.ctx), StatusCode::FailedPrecondition, "each time"));
  d.host(h).onRead = nullptr;
  d.evaluate();
  test_assert(d.isAt(h, 6));
  /* Once is fine: the second pass looks again. */
  const int before = d.sc().counters().rebuilds;
  d.host(h).info.parent = d.idOf(a);
  d.evaluate();
  test_assert(d.isAt(h, 7) && d.sc().counters().rebuilds == before + 1);
  /* Errors of the pass that was thrown away aren't returned. */
  bool threw = false;
  d.addFnModel("test.once", [&](const EvalContext &, const TransformState &p) {
    if (!threw) {
      threw = true;
      d.rt.setStickyError(boom("once"));
    }
    return p;
  });
  const BlockHandle first = d.object("first");
  must(d.sc().remove(*d.lib, d.idOf(first)).status());
  must(d.sc().add(*d.lib, first, 0));
  d.evaluate();
  d.obj(first).setTransformType("test.once");
  d.obj(first).channels = ChannelBlock(1);
  d.host(h).info.parent = BlockId();
  d.evaluate();
  test_assert(threw && d.statusOf(first) == EvalStatus::Ok && d.isAt(h, 6));
}

} // namespace

int main()
{
  testQueryInterface();
  testLateImpl();
  testData();
  testAlias();
  testOverride();
  testKeeps();
  testUnstable();
  return test_end();
}
