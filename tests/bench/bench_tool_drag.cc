/* One move of a transform gizmo drag, of 1, 1,000 and 100,000 selected objects: through
 * the ToolManager alone, and with the evaluate and the draw that a frame does. Not a
 * test: run it on an idle machine, with the tool, which builds it first.
 *
 *   node tools/bench.mjs tool_drag [-- [--count N]]
 *
 * One line per case, in ms per move. */
#include "coredcc/command/executor.h"
#include "coredcc/scene/builtins.h"
#include "coredcc/scene/commands.h"
#include "coredcc/scene/scene.h"
#include "coredcc/tool/builtins.h"
#include "coredcc/tool/gizmos/transform_gizmo.h"
#include "coredcc/tool/manager.h"
#include "coredcc/tool/settings.h"

#include "litestl/math/transform.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

using namespace coredcc;
using namespace coredcc::gizmo;

namespace {

void must(const Status &s)
{
  if (!s.ok()) {
    fprintf(stderr, "failed: %s\n", s.message());
    exit(1);
  }
}

double now()
{
  return std::chrono::duration<double, std::milli>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

struct Doc {
  Runtime rt;
  std::unique_ptr<Library> lib;
  std::unique_ptr<LinearHistory> history;
  std::unique_ptr<CommandExecutor> ex;
  std::unique_ptr<Context> ctx;
  std::unique_ptr<ToolManager> tm;
  View view;
  ViewMath vm;

  explicit Doc(int count)
  {
    must(registerSceneCommands(rt));
    must(registerToolBuiltins(rt));
    lib = std::make_unique<Library>(&rt);
    lib->setQuarantine(false);
    lib->setAuditRelations(false);
    history = std::make_unique<LinearHistory>();
    ex = std::make_unique<CommandExecutor>(&rt, history.get());
    ctx = std::make_unique<Context>(&rt, lib.get(), PointerRef());
    ctx->setExecutor(ex.get());
    Result<BlockHandle> s = newScene(*lib, "Scene");
    must(s.status());
    ctx->setScene(*s);
    for (int i = 0; i < count; i++) {
      Result<BlockHandle> h = newObject(*lib, "o", kTrsTransform);
      must(h.status());
      must(Scene::of(*lib, *s)->add(*lib, *h));
    }
    const CommandDef *sel = rt.ext<CommandRegistry>().find(kSelectSceneCommand);
    PropertyBag in = sel->newInputs();
    must(in.set("mode", PropValue::ofEnum(4))); /* ALL */
    must(ex->run(*ctx, *sel, std::move(in)).status());
    history->clear();
    must(evaluateScene(*ctx));
    tm = std::make_unique<ToolManager>(rt);
    ctx->setTools(tm.get());
    view.viewport[2] = 1600;
    view.viewport[3] = 1000;
    view.view = litestl::math::lookAt(double3(6, -8, 5), double3(), double3(0, 0, 1));
    view.proj = litestl::math::perspective(0.8, 1.6, 0.1, 1000.0);
    must(vm.set(view));
    must(tm->setActive(*ctx, kTransformTool));
  }
  ~Doc()
  {
    tm->shutdown(*ctx);
    ctx->setTools(nullptr);
  }
  InputEvent event(EventType type, Pixel p)
  {
    InputEvent e;
    e.type = type;
    e.x = p.x;
    e.y = p.y;
    return e;
  }
};

void run(int count)
{
  Doc d(count);
  const ToolDef *def = d.rt.ext<ToolRegistry>().find(kTransformTool);
  const TransformSettings s = TransformSettings::read(d.rt, *def);
  const Frame f = makeFrame(d.vm, computePlacement(*d.ctx, s.pivot, s.orientation), s);
  Pixel press, tip;
  toPixel(d.vm, f.pivot + f.axes[0] * (f.unit * 0.7), press);
  toPixel(d.vm, f.pivot + f.axes[0] * f.unit, tip);
  const double dx = tip.x - f.center.x, dy = tip.y - f.center.y;
  const double len = std::sqrt(dx * dx + dy * dy);
  must(d.tm->dispatch(*d.ctx, d.view, d.event(EventType::PointerDown, press)).status());
  if (!d.tm->modalOpen()) {
    fprintf(stderr, "the press missed the gizmo\n");
    exit(1);
  }
  const int moves = count >= 100000 ? 5 : count >= 1000 ? 50 : 500;
  DrawList out;
  double bare = 0.0, frame = 0.0;
  for (int i = 1; i <= moves; i++) {
    const Pixel p{press.x + dx / len * i * 0.5, press.y + dy / len * i * 0.5};
    const double t0 = now();
    must(d.tm->dispatch(*d.ctx, d.view, d.event(EventType::PointerMove, p)).status());
    const double t1 = now();
    must(evaluateScene(*d.ctx));
    must(d.tm->draw(*d.ctx, d.view, out));
    const double t2 = now();
    bare += t1 - t0;
    frame += t2 - t0;
  }
  /* ApplyDelta's own cost, the same number of times: what the manager adds is the rest.
   */
  double own = 0.0;
  for (int i = 1; i <= moves; i++) {
    TransformDelta t;
    t.vector = double3(0.001 * i, 0, 0);
    const double t0 = now();
    must(d.ex->updateInteractive(*d.ctx, [&](Context &c, Command &cmd) {
      return cmd.as<ApplyDelta>()->update(c, t);
    }));
    own += now() - t0;
  }
  must(d.tm->dispatch(*d.ctx, d.view, d.event(EventType::PointerUp, press)).status());
  printf("  %-52s %10.3f\n",
         ("move, " + std::to_string(count) + " objects").c_str(),
         bare / moves);
  printf("  %-52s %10.3f\n",
         ("ApplyDelta::update alone, " + std::to_string(count) + " objects").c_str(),
         own / moves);
  printf("  %-52s %10.3f\n",
         ("move + evaluate + draw, " + std::to_string(count) + " objects").c_str(),
         frame / moves);
  fflush(stdout);
}

} // namespace

int main(int argc, char **argv)
{
  int only = 0;
  for (int i = 1; i + 1 < argc; i++) {
    if (strcmp(argv[i], "--count") == 0) {
      only = atoi(argv[i + 1]);
    }
  }
  printf("tool drag (ms per move)\n");
  for (int count : {1, 1000, 100000}) {
    if (only == 0 || only == count) {
      run(count);
    }
  }
  return 0;
}
