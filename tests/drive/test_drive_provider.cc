/* What commands and computeState see of driven channels, outside a pass and inside one;
 * an inner provider. */
#include "drive_doc.h"

test_init;

using namespace drive_test;

namespace {

/* Drives one channel of one object with `base + time`. */
struct Inner : OverrideProvider {
  BlockHandle object;
  int32_t channel = 0;
  double base = 0;
  Status error;

  Status overridesOf(const Library &,
                     BlockHandle h,
                     const TransformType &,
                     double time,
                     ChannelOverrides &out) override
  {
    if (h == object) {
      CDCC_TRY(error);
      out.set(channel, base + time);
    }
    return Status();
  }
};

void testCommands()
{
  DriveDoc d;
  const BlockHandle a = d.object("a", 1, 2, 3);
  const BlockHandle g = d.graph("g");
  const NodeId v = d.node(g, "value.float");
  must(d.def(g).setParam(v, "value", PropValue::ofFloat(0.5)));
  /* A float can't drive a vec3: the object fails for commands too. */
  const NodeId dr = d.driver(g, "float", a, "rotationEuler");
  d.link(g, v, "value", dr, "value");
  test_assert(!computeState(*d.lib, a, 0).ok());
  must(d.def(g).removeNode(dr));
  d.driveLocation(g, a, 7, 8, 9);
  test_assert(d.stateAt(a)[0] == 7);

  /* Moving it: the driven channels are counted, the stored ones change. */
  d.select({a});
  PropertyBag out = d.ok(kApplyDeltaCommand, {{"vector", PropValue::ofVec3(1, 1, 1)}});
  test_assert(out.get("driven")->asInt() == 1);
  test_assert(lengthOf(get(d.obj(a).channels, trs::kLocation) - double3(1, 2, 3)) == 0.0);
  d.evaluate();
  test_assert(d.evaluatedAt(a, 7, 8, 9));
}

/* A command writes an object a graph reads; computeState of the follower sees it before
 * any evaluateScene. */
void testReadAfterWrite()
{
  DriveDoc d;
  const BlockHandle leader = d.object("leader", 1);
  const BlockHandle a = d.object("a");
  const BlockHandle g = d.graph("g");
  d.follow(g, a, leader, 10);
  test_assert(d.stateAt(a)[0] == 11);
  d.select({leader});
  d.ok(kApplyDeltaCommand, {{"vector", PropValue::ofVec3(4, 0, 0)}});
  test_assert(d.stateAt(a)[0] == 15);
  must(d.ex->undo(*d.ctx));
  test_assert(d.stateAt(a)[0] == 11);

  /* Inside an evaluation: answers for the time evaluated, FailedPrecondition otherwise.
   */
  d.evaluate();
  {
    EvaluationScope scope(d.rt);
    ChannelOverrides o;
    o.reset(d.trs->channelCount());
    test_assert(d.drivers->overridesOf(*d.lib, a, *d.trs, d.sc().time(), o).ok());
    test_assert(o.any() && o.values.get(trs::kLocation) == 11);
    o.reset(d.trs->channelCount());
    const Status s = d.drivers->overridesOf(*d.lib, a, *d.trs, d.sc().time() + 1, o);
    test_assert(s.code() == StatusCode::FailedPrecondition);
    test_assert(d.drivers->update(0).code() == StatusCode::Busy);
  }
}

/* An inner provider is asked first; the graphs win where both drive a channel. Objects of
 * G see its values too. */
void testInner()
{
  DriveDoc d;
  const BlockHandle a = d.object("a"), b = d.object("b"), c = d.object("c");
  Inner inner;
  inner.object = b;
  inner.channel = trs::kLocation + 1;
  inner.base = 5;
  d.drivers->setInner(&inner);
  const BlockHandle g = d.graph("g");
  d.follow(g, a, b);
  d.evaluate();
  test_assert(d.evaluatedAt(b, 0, 5) && d.evaluatedAt(a, 0, 5));
  const TransformState *gb = d.drivers->generatedState(d.idOf(b));
  test_assert(gb && sameState(*gb, d.result(b).state));

  inner.object = c;
  inner.channel = trs::kLocation;
  d.driveLocation(g, c, 3);
  d.rt.ext<ChangeBus>().notifyAll(); /* its answers changed */
  d.evaluate();
  test_assert(d.evaluatedAt(c, 3) && d.evaluatedAt(b, 0) && d.evaluatedAt(a, 0));

  inner.object = b;
  inner.error = Status::error(StatusCode::Internal, "inner");
  d.rt.ext<ChangeBus>().notifyAll();
  test_assert(!evaluateScene(*d.ctx).ok());
  test_assert(d.statusOf(b) == EvalStatus::ModelFailed);
  test_assert(d.statusOf(a) == EvalStatus::ModelFailed);
  d.drivers->setInner(nullptr);
  d.evaluate();
  test_assert(d.statusOf(a) == EvalStatus::Ok);
}

/* Driven values that change are notified with the aspect kDrivenAspect. */
void testNotifications()
{
  DriveDoc d;
  const BlockHandle a = d.object("a");
  const BlockHandle g = d.graph("g");
  const NodeId t = d.node(g, kTimeNode);
  const NodeId comb = d.node(g, "vec.combine");
  d.link(g, t, "seconds", comb, "x");
  const NodeId dr = d.driver(g, "vec3", a, "location");
  d.link(g, comb, "vector", dr, "value");
  int heard = 0;
  const BlockId id = d.idOf(a);
  ScopedConnection c =
      d.rt.ext<ChangeBus>().addChangeListener([&](const BusChange &change) {
        if (change.owner && change.owner->block == id &&
            change.aspects == kDrivenAspect) {
          heard++;
        }
      });
  must(d.drivers->update(1));
  const int first = heard;
  test_assert(first == 1);
  must(d.drivers->update(1));
  test_assert(heard == first);
  must(d.drivers->update(2));
  test_assert(heard == first + 1);
}

} // namespace

int main()
{
  testCommands();
  testReadAfterWrite();
  testInner();
  testNotifications();
  return test_end();
}
