/* The scene's commands on many native objects. Not a test: run it on an idle machine,
 * with the tool, which builds it first.
 *
 *   node tools/bench.mjs scene_commands [-- [--count N] [--only PART]]
 *
 * One line per case, in ms. Parts: members, history, delete, transforms, chain, types. */
#include "coredcc/api/ref_text.h"
#include "coredcc/command/executor.h"
#include "coredcc/scene/builtins.h"
#include "coredcc/scene/commands.h"
#include "coredcc/scene/scene.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace coredcc;
using litestl::util::Vector;

namespace {

void must(const Status &s)
{
  if (!s.ok()) {
    fprintf(stderr, "failed: %s\n", s.message());
    exit(1);
  }
}

template <class Fn> double timeOf(Fn &&fn)
{
  const auto from = std::chrono::steady_clock::now();
  fn();
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                   from)
      .count();
}

template <class Fn> void timed(const std::string &name, Fn &&fn)
{
  const double ms = timeOf(fn);
  printf("  %-52s %10.2f\n", name.c_str(), ms);
  fflush(stdout);
}

struct Doc {
  Runtime rt;
  std::unique_ptr<Library> lib;
  std::unique_ptr<LinearHistory> history;
  std::unique_ptr<CommandExecutor> ex;
  std::unique_ptr<Context> ctx;
  BlockHandle scene;
  std::vector<BlockHandle> objects;

  explicit Doc(int count, const char *type = kTrsTransform)
  {
    must(registerSceneCommands(rt));
    lib = std::make_unique<Library>(&rt);
    lib->setQuarantine(false);
    lib->setAuditRelations(false);
    history = std::make_unique<LinearHistory>();
    ex = std::make_unique<CommandExecutor>(&rt, history.get());
    ctx = std::make_unique<Context>(&rt, lib.get(), PointerRef());
    ctx->setExecutor(ex.get());
    Result<BlockHandle> s = newScene(*lib, "Scene");
    must(s.status());
    scene = *s;
    ctx->setScene(scene);
    for (int i = 0; i < count; i++) {
      Result<BlockHandle> h = newObject(*lib, "o", type);
      must(h.status());
      must(sc().add(*lib, *h));
      objects.push_back(*h);
    }
  }
  Scene &sc()
  {
    return *Scene::of(*lib, scene);
  }
  Object &obj(int i)
  {
    return *static_cast<Object *>(lib->data(objects[size_t(i)]));
  }
  void parent(int i, int above)
  {
    must(lib->writeRef(obj(i).parent.cell(), lib->get(objects[size_t(above)])->id()));
  }
  PropValue ids(int n, int step = 1)
  {
    Vector<BlockId> out;
    for (int i = 0; i < n; i++) {
      out.append(lib->get(objects[size_t(i * step)])->id());
    }
    return PropValue::ofString(
        formatBlockIds(rt, {out.data(), size_t(out.size())}).c_str());
  }
  using In = std::vector<std::pair<const char *, PropValue>>;
  void run(const char *command, const In &in = {})
  {
    const CommandDef *def = rt.ext<CommandRegistry>().find(command);
    PropertyBag bag = def->newInputs();
    for (const auto &[name, value] : in) {
      must(bag.set(name, value));
    }
    must(ex->run(*ctx, *def, std::move(bag)).status());
  }
  void undo()
  {
    must(ex->undo(*ctx));
  }
  void redo()
  {
    must(ex->redo(*ctx));
  }
  void evaluate()
  {
    must(evaluateScene(*ctx));
  }
};

const PropValue kStep = PropValue::ofVec3(1, 0, 0);
constexpr int64_t kAll = 4; /* modes of core.scene.select */
constexpr int64_t kNone = 5;

void cycle(Doc &d, const std::string &name, const char *command, const Doc::In &in)
{
  timed(name, [&] { d.run(command, in); });
  timed("  undo", [&] { d.undo(); });
  timed("  redo", [&] { d.redo(); });
  d.undo();
}

void members(int count)
{
  Doc d(count);
  for (int n : {100, 1000, count / 10}) {
    cycle(d, "unlink " + std::to_string(n), kUnlinkCommand, {{"objects", d.ids(n)}});
  }
  cycle(d,
        "unlink " + std::to_string(count / 10) + ", every 10th",
        kUnlinkCommand,
        {{"objects", d.ids(count / 10, 10)}});
  const int adds = 200;
  timed("core.object.add, " + std::to_string(adds) + " times", [&] {
    for (int i = 0; i < adds; i++) {
      d.run(kAddObjectCommand);
    }
  });
  timed("  undo of all", [&] {
    for (int i = 0; i < adds; i++) {
      d.undo();
    }
  });
  timed("  redo of all", [&] {
    for (int i = 0; i < adds; i++) {
      d.redo();
    }
  });
  timed("select all",
        [&] { d.run(kSelectCommand, {{"mode", PropValue::ofEnum(kAll)}}); });
}

void history(int count)
{
  for (int n : {100, 200}) {
    Doc d(count);
    for (int i = 0; i < n; i++) {
      d.run(kAddObjectCommand);
    }
    for (int i = 0; i < n; i++) {
      d.undo();
    }
    timed("a command after undoing " + std::to_string(n) + " adds",
          [&] { d.run(kSelectCommand, {{"mode", PropValue::ofEnum(kNone)}}); });
  }
}

void deleting(int count)
{
  Doc d(count);
  for (int i = 0; i < count; i++) {
    if (i % 4) {
      d.parent(i, i - 1);
    }
  }
  for (int n : {count / 100, count / 10}) {
    const std::string name = "delete " + std::to_string(n);
    cycle(d, name, kDeleteObjectsCommand, {{"objects", d.ids(n)}});
    cycle(d, name + " roots of 4", kDeleteObjectsCommand, {{"objects", d.ids(n, 4)}});
  }
}

void transforms(int count)
{
  Doc d(count);
  d.run(kSelectCommand, {{"mode", PropValue::ofEnum(kAll)}});
  d.evaluate();
  cycle(d, "applyDelta, all", kApplyDeltaCommand, {{"vector", kStep}});
  const CommandDef *def = d.rt.ext<CommandRegistry>().find(kDragDeltaCommand);
  timed("drag of all: begin",
        [&] { must(d.ex->beginInteractive(*d.ctx, *def, def->newInputs())); });
  TransformDelta t;
  for (int i = 1; i <= 2; i++) {
    t.vector = double3(i, 2, 3);
    timed("  update", [&] {
      must(d.ex->updateInteractive(*d.ctx, [&](Context &c, Command &cmd) {
        return cmd.as<ApplyDelta>()->update(c, t);
      }));
    });
  }
  timed("  evaluate", [&] { d.evaluate(); });
  timed("  cancel", [&] { must(d.ex->cancel(*d.ctx)); });
  StableRef ref;
  ref.owner = OwnerKey::ofBlock(d.lib->get(d.objects.back())->id());
  ref.type = StructId::of(kObjectType);
  cycle(d,
        "setParent, a tenth to one",
        kSetParentCommand,
        {{"parent", PropValue::ofPointer(ref)}, {"objects", d.ids(count / 10)}});
  cycle(d,
        "setTransformType, all",
        kSetTransformTypeCommand,
        {{"type", PropValue::ofString(kMatrixTransform)}});
  cycle(d,
        "setRotationMode, all",
        kSetRotationModeCommand,
        {{"mode", PropValue::ofEnum(2)}});
}

void chains(int longest)
{
  for (int n = longest / 4; n <= longest; n *= 2) {
    Doc d(n);
    for (int i = 1; i < n; i++) {
      d.parent(i, i - 1);
      d.obj(i).channels.set(trs::kLocation, 1.0);
    }
    printf(" %d\n", n);
    timed("evaluate", [&] { d.evaluate(); });
    d.run(kSelectCommand, {{"mode", PropValue::ofEnum(kAll)}});
    cycle(d, "applyDelta, all", kApplyDeltaCommand, {{"vector", kStep}});
    cycle(d, "setParent to none, all", kSetParentCommand, {});
    cycle(d,
          "setTransformType, all",
          kSetTransformTypeCommand,
          {{"type", PropValue::ofString(kMatrixTransform)}});
    cycle(d, "delete every 2nd", kDeleteObjectsCommand, {{"objects", d.ids(n / 2, 2)}});
    timed("computeState of each", [&] {
      for (BlockHandle h : d.objects) {
        must(computeState(*d.lib, h, 0.0).status());
      }
    });
  }
}

void types(int count)
{
  for (const char *type : {kTrsTransform, kFbxTransform, kMatrixTransform}) {
    Doc d(count, type);
    d.evaluate();
    std::vector<double> ms;
    for (int i = 0; i < 21; i++) {
      ms.push_back(timeOf([&] { d.evaluate(); }));
    }
    std::sort(ms.begin(), ms.end());
    printf(
        "  evaluate, unchanged, %-20s best %7.2f  median %7.2f\n", type, ms[0], ms[10]);
  }
}

} // namespace

int main(int argc, char **argv)
{
  int count = 100000;
  const char *only = "";
  for (int i = 1; i + 1 < argc; i += 2) {
    if (!strcmp(argv[i], "--count")) {
      count = std::max(atoi(argv[i + 1]), 400);
    } else if (!strcmp(argv[i], "--only")) {
      only = argv[i + 1];
    }
  }
  const auto part = [&](const char *name, void (*fn)(int), int n) {
    if (!*only || !strcmp(only, name)) {
      printf("%s, %d objects\n", name, n);
      fn(n);
    }
  };
  part("members", members, count);
  part("history", history, count);
  part("delete", deleting, count);
  part("transforms", transforms, count);
  part("chain", chains, std::min(4000, count));
  part("types", types, count);
  return 0;
}
