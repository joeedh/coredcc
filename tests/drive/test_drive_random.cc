/* Random documents, driver graphs and edits, with undo and redo. After each edit, the
 * scene's states must be those of the same document saved, loaded and given drivers of
 * its own (a sync from scratch), bitwise, with the same statuses; and those of G must be
 * the direct pass's. */
#include "drive_doc.h"

#include "coredcc/drive/commands.h"

#include <random>
#include <string>
#include <vector>

test_init;

using namespace drive_test;

namespace {

struct Random {
  DriveDoc d;
  std::mt19937 rng;
  std::vector<BlockHandle> objects;
  std::vector<std::string> names;
  std::vector<BlockHandle> graphs;

  explicit Random(int seed) : rng(uint32_t(seed))
  {
    const int n = 4 + pick(8);
    for (int i = 0; i < n; i++) {
      names.push_back("o" + std::to_string(i));
      objects.push_back(d.object(names.back().c_str(), pick(10)));
    }
    for (int i = 1; i < n; i++) {
      if (pick(2)) {
        d.setParent(objects[i], objects[pick(i)]);
      }
    }
    for (int g = 0; g < 1 + pick(3); g++) {
      graphs.push_back(d.graph(("g" + std::to_string(g)).c_str(), pick(4) != 0));
      for (int k = 0; k < 1 + pick(3); k++) {
        addAdapter(graphs.back());
      }
    }
  }
  int pick(int n)
  {
    return int(rng() % uint32_t(n));
  }
  BlockHandle any()
  {
    return objects[pick(int(objects.size()))];
  }
  void addAdapter(BlockHandle g)
  {
    if (pick(3) == 0) {
      d.driveLocation(g, any(), pick(20));
    } else {
      d.follow(g, any(), any(), pick(5));
    }
  }
  PropValue named(BlockHandle h)
  {
    return PropValue::ofString(formatBlockId(d.rt, d.idOf(h)));
  }

  void edit()
  {
    switch (pick(9)) {
    case 0:
      d.moveTo(any(), pick(30));
      break;
    case 1:
      (void)d.run(kSetParentCommand,
                  {{"parent", pick(3) ? d.ptr(any()) : d.ptr(BlockHandle())},
                   {"objects", named(any())},
                   {"keepWorld", PropValue::ofBool(pick(2) != 0)}});
      break;
    case 2: {
      const BlockHandle g = graphs[pick(int(graphs.size()))];
      (void)d.run(pick(2) ? kAddGraphCommand : kRemoveGraphCommand,
                  {{"graph", d.ptr(g)}});
      break;
    }
    case 3:
      addAdapter(graphs[pick(int(graphs.size()))]);
      break;
    case 4: {
      /* Retarget a reader or a driver. */
      GraphDef &def = d.def(graphs[pick(int(graphs.size()))]);
      std::vector<NodeId> adapters;
      def.forEachNode([&](const GraphNode &n) {
        if (n.type() && strncmp(n.typeName().c_str(), "scene.", 6) == 0 &&
            strcmp(n.typeName().c_str(), kTimeNode) != 0)
        {
          adapters.push_back(n.id());
        }
      });
      if (!adapters.empty()) {
        must(def.setParam(
            adapters[pick(int(adapters.size()))], "object", pointerTo(d.idOf(any()))));
      }
      break;
    }
    case 5:
      (void)d.ex->undo(*d.ctx);
      break;
    case 6:
      (void)d.ex->redo(*d.ctx);
      break;
    case 7:
      d.sc().frame = pick(48);
      break;
    default:
      d.select({any()});
      (void)d.run(kApplyDeltaCommand, {{"vector", PropValue::ofVec3(pick(3), 0, 0)}});
      break;
    }
    if (d.rt.hasStickyError()) {
      (void)d.rt.takeStickyError(); /* an undo that couldn't be done */
    }
  }

  void check(int seed, int step)
  {
    (void)evaluateScene(*d.ctx);
    Reloaded r(d);
    (void)r.evaluate();
    for (int i = 0; i < int(objects.size()); i++) {
      const ObjectEval mine = d.result(objects[i]);
      ObjectEval theirs;
      test_assert(r.evaluated(names[i].c_str(), theirs));
      if (!sameState(mine.state, theirs.state) || mine.status != theirs.status) {
        fprintf(stderr,
                "seed %d step %d: %s differs (%g, %d / %g, %d)\n",
                seed,
                step,
                names[i].c_str(),
                at(mine.state)[0],
                int(mine.status),
                at(theirs.state)[0],
                int(theirs.status));
        test_assert(false);
      }
      const TransformState *g = d.drivers->generatedState(d.idOf(objects[i]));
      if (g && mine.status != EvalStatus::Cycle) {
        test_assert(sameState(*g, mine.state));
      }
    }
    test_assert(d.drivers->diagnostics().size() == r.drivers->diagnostics().size());
  }
};

} // namespace

int main()
{
  const int seeds = 40 * coredcc_test::testScale();
  for (int seed = 0; seed < seeds; seed++) {
    Random r(seed);
    r.check(seed, -1);
    for (int step = 0; step < 12; step++) {
      r.edit();
      r.check(seed, step);
    }
  }
  return test_end();
}
