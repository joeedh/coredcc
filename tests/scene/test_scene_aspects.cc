/* Aspects of a change: what invalidateData says changed, and graphs that use only some of it. */
#include "scene_doc.h"

#include "coredcc/graph/instance.h"

#include "../graph/graph_fixture.h"

test_init;

using namespace scene_test;

namespace {

static_assert(uint32_t(InvalidateKinds::Materials) < (1u << 16), "bits 16 to 31 are the client's");

BlockId g_block;
ChangeAspects g_aspects = 0;

/* The size of a shape, of which it uses `g_aspects`. */
Status sizeKernel(NodeExecContext &ctx)
{
  const Spatial s = ctx.queryBlock<Spatial>(ctx.useBlock(g_block, g_aspects).handle);
  return ctx.setOutput(0, GraphValue::ofFloat(s ? s.localBounds().max[0] : -1.0));
}

constexpr InvalidateKinds kGeometry =
    InvalidateKinds::Topology | InvalidateKinds::Positions;

void testKernels(bool incremental)
{
  Doc d;
  must(registerGraphBlocks(d.rt));
  NodeTypeDef t("t.size");
  t.output("value", "float");
  fixture::addType<sizeKernel>(d.rt, t, "t.size");
  const BlockHandle shape = d.shapeBlock("shape", 1.0), other = d.shapeBlock("other", 1.0);
  g_block = d.idOf(shape);
  GraphDef def(&d.rt, d.lib.get());
  GraphInstance inst(&d.rt, d.lib.get(), &def);
  inst.setIncremental(incremental);
  const SocketPath path = SocketPath::of(fixture::add(def, "t.size"), "value");
  const auto runs = [&] {
    const uint64_t before = inst.totalRuns();
    must(inst.evaluate(path));
    return int(inst.totalRuns() - before);
  };
  const auto value = [&] { return inst.value(path)->asFloat(); };

  g_aspects = ChangeAspects(kGeometry);
  test_assert(runs() == 1 && value() == 1.0);
  test_assert(runs() == 0);
  d.shape(shape).box.max[0] = 2.0;
  /* Selecting elements, and other blocks, aren't the kernel's business. */
  must(invalidateData(*d.ctx, shape, InvalidateKinds::Selection));
  must(invalidateData(*d.ctx, shape, InvalidateKinds::Attributes | InvalidateKinds::Materials));
  must(invalidateData(*d.ctx, shape, InvalidateKinds::None));
  must(invalidateData(*d.ctx, other, kGeometry));
  test_assert(runs() == 0 && value() == 1.0);
  must(invalidateData(*d.ctx, shape, InvalidateKinds::Positions));
  test_assert(runs() == 1 && value() == 2.0);
  d.shape(shape).box.max[0] = 3.0;
  must(invalidateData(*d.ctx, shape, InvalidateKinds::Selection | InvalidateKinds::Topology));
  test_assert(runs() == 1 && value() == 3.0);
  /* A write by path is all of it. */
  d.rt.ext<ChangeBus>().notify(OwnerKey::ofBlock(g_block));
  test_assert(runs() == 1);

  /* A kernel of the selection. */
  g_aspects = ChangeAspects(InvalidateKinds::Selection);
  must(invalidateData(*d.ctx, shape, InvalidateKinds::Positions));
  test_assert(runs() == 1); /* it ran for the positions once more */
  must(invalidateData(*d.ctx, shape, InvalidateKinds::Positions));
  test_assert(runs() == 0);
  must(invalidateData(*d.ctx, shape, InvalidateKinds::Selection));
  test_assert(runs() == 1);
}

void testBus()
{
  Doc d;
  const BlockHandle a = d.object("a", 1.0), shape = d.shapeBlock("shape", 1.0);
  ChangeAspects seen = 0;
  int n = 0;
  const ScopedConnection c = d.rt.ext<ChangeBus>().addChangeListener([&](const BusChange &change) {
    n++;
    seen = change.aspects;
    test_assert(change.member.empty() && *change.owner == OwnerKey::ofBlock(d.idOf(shape)));
  });
  for (InvalidateKinds k : {InvalidateKinds::Topology, InvalidateKinds::Positions,
                            InvalidateKinds::Attributes, InvalidateKinds::Selection,
                            InvalidateKinds::Materials, kGeometry, InvalidateKinds::None})
  {
    const int before = n;
    must(invalidateData(*d.ctx, shape, k));
    test_assert(n == before + 1 && seen == ChangeAspects(k));
  }
  (void)a;
}

} // namespace

int main()
{
  testKernels(true);
  testKernels(false);
  testBus();
  return test_end();
}
