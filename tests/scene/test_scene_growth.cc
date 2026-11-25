/* What grows with the size of a scene: sixteen times the size may take no more than 64
 * times as long, where something quadratic takes 256. */
#include "command_doc.h"

#include "coredcc/api/ref_text.h"

#include <algorithm>
#include <chrono>
#include <functional>

test_init;

using namespace scene_test;

namespace {

double secondsOf(const std::function<void()> &fn)
{
  const auto start = std::chrono::steady_clock::now();
  fn();
  const std::chrono::duration<double> t = std::chrono::steady_clock::now() - start;
  return t.count();
}

/* `run(n)` gives the seconds that matter. The best of a few, and a few more if it looks
 * bad: the machine may be busy. */
bool grows(const char *name, int n, const std::function<double(int)> &run)
{
  double small = 1e9, large = 1e9;
  for (int tries = 0; tries < 8; tries++) {
    small = std::min(small, run(n));
    large = std::min(large, run(16 * n));
    if (tries >= 1 && large < 40 * small + 0.002) {
      break;
    }
  }
  const bool ok = large < 64 * small + 0.002;
  if (!ok) {
    printf(
        "%s: %g ms for %d, %g ms for %d\n", name, small * 1000, n, large * 1000, 16 * n);
  }
  return ok;
}

struct Many : CommandDoc {
  std::vector<BlockHandle> all;

  explicit Many(int n, bool chain = false)
  {
    lib->setQuarantine(false);
    lib->setAuditRelations(false);
    for (int i = 0; i < n; i++) {
      all.push_back(object("o", 1.0));
      if (chain && i) {
        setParent(all[size_t(i)], all[size_t(i) - 1]);
      }
    }
  }
  PropValue list(int step = 1)
  {
    Vector<BlockId> ids;
    for (size_t i = 0; i < all.size(); i += size_t(step)) {
      ids.append(idOf(all[i]));
    }
    return PropValue::ofString(
        formatBlockIds(rt, {ids.data(), size_t(ids.size())}).c_str());
  }
};

using Make = std::function<double(Many &)>;

bool many(const char *name, int n, const Make &fn, bool chain = false)
{
  return grows(name, n, [&](int size) {
    Many d(size, chain);
    return fn(d);
  });
}

void testMembers()
{
  test_assert(many("unlink, undo, redo of half", 250, [](Many &d) {
    const PropValue half = d.list(2);
    return secondsOf([&]() {
      d.ok(kUnlinkCommand, {{"objects", half}});
      d.undo();
      d.redo();
    });
  }));
  test_assert(many("addMany, removeMany, putBack", 250, [](Many &d) {
    Vector<BlockHandle> handles;
    std::vector<BlockId> ids;
    for (int i = 0; i < int(d.all.size()); i++) {
      handles.append(d.loose("l"));
      ids.push_back(d.idOf(handles[i]));
    }
    return secondsOf([&]() {
      must(d.sc().addMany(*d.lib, {handles.data(), size_t(handles.size())}));
      Result<Vector<Scene::Place>> places = d.sc().removeMany(*d.lib, ids);
      must(places.status());
      must(d.sc().putBack(*d.lib, {places->data(), size_t(places->size())}));
    });
  }));
  /* Each of them used to look at all members. */
  test_assert(many("as many objects added as there are", 100, [](Many &d) {
    return secondsOf([&]() {
      for (int i = int(d.all.size()); i > 0; i--) {
        d.ok(kAddObjectCommand);
      }
    });
  }));
  /* As many as there are objects: one look at the Library for all that leave the history.
   */
  test_assert(many("a command after as many undone adds", 100, [](Many &d) {
    const int n = int(d.all.size());
    for (int i = 0; i < n; i++) {
      d.ok(kAddObjectCommand);
    }
    for (int i = 0; i < n; i++) {
      d.undo();
    }
    const int blocks = d.lib->size();
    const double t =
        secondsOf([&]() { d.ok(kSelectCommand, {{"mode", PropValue::ofEnum(4)}}); });
    test_assert(d.lib->size() == blocks - n);
    return t;
  }));
  test_assert(many("a budget that drops as many deletions", 100, [](Many &d) {
    for (BlockHandle h : d.all) {
      d.select({h});
      d.ok(kDeleteObjectsCommand);
    }
    const int blocks = d.lib->size();
    const double t = secondsOf([&]() { d.history->setMemoryBudget(1); });
    test_assert(d.lib->size() == blocks - int(d.all.size()) + 1 &&
                d.history->size() == 1);
    return t;
  }));
}

void testChains()
{
  test_assert(many(
      "applyDelta, a chain",
      60,
      [](Many &d) {
        const PropValue all = d.list();
        return secondsOf([&]() {
          d.ok(kApplyDeltaCommand,
               {{"objects", all}, {"vector", PropValue::ofVec3(1, 0, 0)}});
        });
      },
      true));
  test_assert(many(
      "setParent, a chain",
      60,
      [](Many &d) {
        const PropValue all = d.list();
        return secondsOf([&]() { d.ok(kSetParentCommand, {{"objects", all}}); });
      },
      true));
  test_assert(many(
      "setTransformType, a chain",
      60,
      [](Many &d) {
        const PropValue all = d.list();
        return secondsOf([&]() {
          d.ok(kSetTransformTypeCommand,
               {{"objects", all}, {"type", PropValue::ofString(kMatrixTransform)}});
        });
      },
      true));
  test_assert(many(
      "delete, every second of a chain",
      60,
      [](Many &d) {
        const PropValue half = d.list(2);
        return secondsOf([&]() { d.ok(kDeleteObjectsCommand, {{"objects", half}}); });
      },
      true));
  test_assert(many("delete, half of them", 250, [](Many &d) {
    const PropValue half = d.list(2);
    return secondsOf([&]() {
      d.ok(kDeleteObjectsCommand, {{"objects", half}});
      d.undo();
      d.redo();
    });
  }));
}

void testEvaluate()
{
  test_assert(many("evaluate, and what changed", 250, [](Many &d) {
    d.evaluate();
    size_t found = 0;
    const double t = secondsOf([&]() {
      for (int i = 0; i < 10; i++) {
        d.obj(d.all[size_t(i)]).channels.set(trs::kLocation, 5.0 + i);
        d.evaluate();
        found += size_t(d.sc().lastChanges().places.size());
      }
    });
    test_assert(found == 10);
    return t;
  }));
}

} // namespace

int main()
{
  testMembers();
  testChains();
  testEvaluate();
  return test_end();
}
