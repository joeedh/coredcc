/* Regressions from the review of P5c (docs/reports/feature-report-scene-evaluator-
 * 2026-11-25.md): an inner provider's notifications, a failure of the whole call under
 * the incremental pass, a type named "missing", alias ids. The list's commands are in
 * test_drive_list. */
#include "drive_doc.h"

#include "coredcc/drive/commands.h"
#include "coredcc/drive/node_types.h"
#include "coredcc/lib/registry.h"

#include <string>
#include <vector>

test_init;

using namespace drive_test;

namespace {

struct Inner : OverrideProvider {
  BlockHandle object;
  int32_t channel = 0;
  double base = 0;
  Status overridesOf(const Library &,
                     BlockHandle h,
                     const TransformType &,
                     double,
                     ChannelOverrides &out) override
  {
    if (h == object) {
      out.set(channel, base);
    }
    return Status();
  }
};

/* An inner provider notifies its object with kDrivenAspect; a graph reading the object
 * sees the new value. */
void testInnerNotification()
{
  DriveDoc d;
  const BlockHandle a = d.object("a"), b = d.object("b");
  Inner inner;
  inner.object = b;
  inner.channel = trs::kLocation + 1;
  inner.base = 5;
  d.drivers->setInner(&inner);
  const BlockHandle g = d.graph("g");
  d.follow(g, a, b);
  d.evaluate();
  test_assert(d.evaluatedAt(b, 0, 5) && d.evaluatedAt(a, 0, 5));
  inner.base = 9;
  d.rt.ext<ChangeBus>().notify(OwnerKey::ofBlock(d.idOf(b)), {}, kDrivenAspect);
  d.evaluate();
  test_assert(d.evaluatedAt(b, 0, 9) && d.evaluatedAt(a, 0, 9));
  test_assert(d.stateAt(a)[1] == 9);
  d.drivers->setInner(nullptr);
}

/* A graph whose evaluation takes 2^levels frames, with output `o` of graph gs[0]. */
struct Nest {
  std::vector<BlockHandle> gs;
  std::vector<int64_t> outs;
};

Nest doublingNest(DriveDoc &d, int levels)
{
  Nest n;
  for (int i = 0; i <= levels; i++) {
    n.gs.push_back(d.graph(("n" + std::to_string(i)).c_str(), false));
    n.outs.push_back(*d.def(n.gs[i]).addInterface(SocketSide::Output, "o", "float"));
  }
  for (int i = levels; i >= 0; i--) {
    GraphDef &def = d.def(n.gs[i]);
    const NodeId o = *def.addNode(kGraphOutputType);
    const NodeId s = *def.addNode("math.float");
    must(def.setParam(s, "op", PropValue::ofEnum(0)));
    must(def.link({s, "value"}, {o, interfaceKey(n.outs[i]).c_str()}).status());
    if (i == levels) {
      must(def.setLiteral(s, "a", PropValue::ofFloat(1)));
    }
    for (int k = 0; i < levels && k < 2; k++) {
      const NodeId q = *def.addNode(kGraphGroupType);
      must(def.setGroup(q, d.idOf(n.gs[i + 1])));
      must(def.link({q, interfaceKey(n.outs[i + 1]).c_str()}, {s, k ? "b" : "a"})
               .status());
    }
  }
  return n;
}

/* The drivers' call fails as a whole (over the frame budget) without the epoch moving:
 * the incremental pass fails the driven object, as a full pass does. */
void testWholeCallFailure()
{
  DriveDoc d;
  const BlockHandle a = d.object("a");
  const BlockHandle g = d.graph("g");
  const NodeId v = d.node(g, "value.vec3");
  must(d.def(g).setParam(v, "value", PropValue::ofVec3(7, 0, 0)));
  const NodeId dr = d.driver(g, "vec3", a, "location");
  const LinkId first = *d.def(g).link({v, "value"}, {dr, "value"});
  const Nest nest = doublingNest(d, 20); /* 2^20 frames, over the default 262144 */
  const NodeId bomb = d.node(g, kGraphGroupType);
  must(d.def(g).setGroup(bomb, d.idOf(nest.gs[0])));
  const NodeId comb = d.node(g, "vec.combine");
  d.link(g, bomb, interfaceKey(nest.outs[0]).c_str(), comb, "x");
  d.evaluate();
  test_assert(d.evaluatedAt(a, 7));

  must(d.def(g).unlink(first));
  const LinkId second = *d.def(g).link({comb, "vector"}, {dr, "value"});
  test_assert(!evaluateScene(*d.ctx).ok());
  test_assert(d.statusOf(a) == EvalStatus::ModelFailed);
  test_assert(!computeState(*d.lib, a, 0).ok());

  /* And back: the call works again, so the object is notified again. */
  must(d.def(g).unlink(second));
  d.link(g, v, "value", dr, "value");
  d.evaluate();
  test_assert(d.evaluatedAt(a, 7) && d.statusOf(a) == EvalStatus::Ok);
}

/* A transform type named "missing" is an ordinary type, not the one for unregistered
 * types; the drivers work. */
void testTypeNamedMissing()
{
  DriveDoc d;
  d.addFnModel("missing", [](const EvalCtx &, const TransformState &p) { return p; });
  const BlockHandle leader = d.object("leader", 1);
  d.obj(leader).setTransformType("missing");
  d.obj(leader).channels = ChannelBlock(1);
  const BlockHandle a = d.object("a"), other = d.object("other");
  const BlockHandle g = d.graph("g");
  d.follow(g, a, leader, 10);
  d.driveLocation(g, other, 3);
  d.evaluate();
  test_assert(d.evaluatedAt(other, 3) && d.evaluatedAt(a, 10));
  test_assert(d.stateAt(other)[0] == 3);
}

Status nothing(NodeExecContext &)
{
  return Status();
}

/* A transform node type that drive/ didn't register isn't used: the leader has no
 * transform node, so what reads it fails. The leader itself is fine. */
void testForeignTransformType()
{
  DriveDoc d;
  NodeTypeDef fake(std::string(kTransformPrefix) + kTrsTransform);
  fake.uiName("Not drive's").output("value", "float");
  Result<Owned<GraphKernel>> k = makeKernel<FnKernel<nothing>>(d.rt, "test.fake#kernel");
  must(k.status());
  fake.kernel(std::move(k).value());
  must(d.rt.ext<NodeTypeRegistry>().add(std::move(fake)).status());
  const BlockHandle leader = d.object("leader", 1), a = d.object("a");
  const BlockHandle g = d.graph("g");
  d.follow(g, a, leader);
  test_assert(!evaluateScene(*d.ctx).ok());
  test_assert(d.drivers->transformNode(d.idOf(leader)).isNull());
  test_assert(d.statusOf(a) == EvalStatus::ModelFailed && d.evaluatedAt(leader, 1));
}

/* A transform node placed in a listed graph is reported, and changes nothing. */
void testPlacedTransformNode()
{
  DriveDoc d;
  const BlockHandle leader = d.object("leader", 4), a = d.object("a");
  const BlockHandle g = d.graph("g");
  d.follow(g, a, leader); /* registers the transform node type of TRS */
  d.evaluate();
  (void)d.node(g, (std::string(kTransformPrefix) + kTrsTransform).c_str());
  d.evaluate();
  test_assert(d.evaluatedAt(a, 4));
  test_assert(d.hasDiagnostic("does nothing here"));
}

/* A driver's object named by an alias's id: it drives the object, and with the canonical
 * id too it is driven twice. */
void testAliasIds()
{
  DriveDoc d;
  const char *aliases[] = {"old.client"};
  must(d.rt.ext<ClientRegistry>().registerClient(kCoredccClient, aliases).status());
  const BlockHandle a = d.object("a");
  BlockId old = d.idOf(a);
  old.client = ClientId::of("old.client");
  const BlockHandle g = d.graph("g");
  const NodeId v = d.node(g, "value.vec3");
  must(d.def(g).setParam(v, "value", PropValue::ofVec3(7, 0, 0)));
  const NodeId dr = d.node(g, (std::string(kDrivePrefix) + "vec3").c_str());
  must(d.def(g).setParam(dr, "object", pointerTo(old)));
  must(d.def(g).setParam(dr, "channel", PropValue::ofString("location")));
  d.link(g, v, "value", dr, "value");
  d.evaluate();
  test_assert(d.evaluatedAt(a, 7) && d.stateAt(a)[0] == 7);
  d.driveLocation(g, a, 3);
  test_assert(!evaluateScene(*d.ctx).ok());
  test_assert(d.hasDiagnostic("driven twice"));
}

/* Before the first sync there is no generated graph; diagnostics sync first, and are a
 * copy. */
void testAccessors()
{
  DriveDoc d;
  test_assert(!d.drivers->generated() && !d.drivers->instance());
  test_assert(SceneDrivers::of(*d.lib) == d.drivers.get());
  const BlockHandle a = d.object("a");
  const BlockHandle g = d.graph("g");
  d.driveLocation(g, a, 1);
  d.driveLocation(g, a, 2);
  const Vector<DriverDiagnostic> kept = d.drivers->diagnostics();
  test_assert(kept.size() > 0);
  test_assert(d.drivers->generated() && d.drivers->instance());
  (void)computeState(*d.lib, a, 0);
  test_assert(kept.size() > 0 && kept[0].message.size() > 0);
  d.drivers.reset();
  test_assert(!SceneDrivers::of(*d.lib));
}

/* Notifications come when the graphs are evaluated: an edit alone posts none, `update`
 * does. */
void testNotifiedOnEvaluation()
{
  DriveDoc d;
  const BlockHandle a = d.object("a");
  const BlockHandle g = d.graph("g");
  const NodeId v = d.node(g, "value.vec3");
  must(d.def(g).setParam(v, "value", PropValue::ofVec3(7, 0, 0)));
  const NodeId dr = d.driver(g, "vec3", a, "location");
  d.link(g, v, "value", dr, "value");
  d.evaluate();
  int heard = 0;
  const BlockId id = d.idOf(a);
  ScopedConnection c = d.rt.ext<ChangeBus>().addChangeListener([&](const BusChange &ch) {
    if (ch.owner && ch.owner->block == id && ch.aspects == kDrivenAspect) {
      heard++;
    }
  });
  must(d.def(g).setParam(v, "value", PropValue::ofVec3(8, 0, 0)));
  test_assert(heard == 0);
  must(d.drivers->update(d.sc().time()));
  test_assert(heard == 1);
}

} // namespace

int main()
{
  testInnerNotification();
  testWholeCallFailure();
  testTypeNamedMissing();
  testForeignTransformType();
  testPlacedTransformNode();
  testAliasIds();
  testAccessors();
  testNotifiedOnEvaluation();
  return test_end();
}
