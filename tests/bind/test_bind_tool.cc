/* The tool facades (bind/tool_facades.h) with script tools, modals and gizmos played by
 * fakehost::Host: registration and handle counts, a script drag through updateDrag, the
 * gate table of tool slots, and the reads allowed in read-only slots. */

#include "../tool/tool_fixture.h"
#include "fake_host.h"

#include "coredcc/bind/facades.h"
#include "coredcc/bind/foreign_command.h"
#include "coredcc/bind/run_facades.h"
#include "coredcc/bind/scene_facades.h"
#include "coredcc/bind/tool_facades.h"
#include "coredcc/scene/commands.h"
#include "coredcc/tool/stroke.h"

#include <cmath>
#include <cstring>
#include <functional>
#include <new>
#include <string>
#include <vector>

using namespace tool_test;
namespace sc = coredcc::script;
using FakeHost = fakehost::Host;
using fakehost::Obj;
using litestl::util::string;

namespace {

void must(const Status &s)
{
  if (!s.ok()) {
    fprintf(stderr, "%s\n", s.message());
  }
  test_assert(s.ok());
}

bool isCode(const Status &s, StatusCode code, const char *part = nullptr)
{
  if (s.code() != code || (part && !strstr(s.message(), part))) {
    fprintf(stderr, "unexpected: %s\n", s.toString().c_str());
    return false;
  }
  return true;
}

std::string nameOf(ToolDoc &d, BlockHandle h)
{
  const sc::StringResult r = sc::pointerText(*d.ctx, d.ptr(h));
  test_assert(r.status.ok());
  return r.value.c_str();
}

/* What the script tool, modal and gizmo do and saw. */
struct Script {
  std::function<EventResult(Context &, ToolManager &, const InputEvent &)> onEvent;
  std::function<ModalResult(Context &, const InputEvent &)> modalEvent;
  std::function<void(Context &)> gizmoUpdate;
  int activates = 0, deactivates = 0, ends = 0, modalEvents = 0;
  bool committed = false;
};

Obj modalObj(Script *s)
{
  Obj m;
  m.methods["onEvent"] = [s](void **args, void *ret, string *) {
    s->modalEvents++;
    *static_cast<ModalResult *>(ret) = s->modalEvent(*static_cast<Context *>(args[0]),
                                                     *static_cast<InputEvent *>(args[3]));
    return true;
  };
  m.methods["end"] = [s](void **args, void *, string *) {
    s->ends++;
    s->committed = *static_cast<bool *>(args[1]);
    return true;
  };
  return m;
}

Obj toolObj(Script *s)
{
  Obj t;
  t.methods["activate"] = [s](void **, void *ret, string *) {
    s->activates++;
    new (ret) Status();
    return true;
  };
  t.methods["deactivate"] = [s](void **, void *, string *) {
    s->deactivates++;
    return true;
  };
  t.methods["onEvent"] = [s](void **args, void *ret, string *) {
    *static_cast<EventResult *>(ret) =
        s->onEvent ? s->onEvent(*static_cast<Context *>(args[0]),
                                *static_cast<ToolManager *>(args[1]),
                                *static_cast<InputEvent *>(args[3]))
                   : EventResult::Unhandled;
    return true;
  };
  return t;
}

Obj gizmoObj(Script *s)
{
  Obj g;
  g.methods["update"] = [s](void **args, void *, string *) {
    if (s->gizmoUpdate) {
      s->gizmoUpdate(*static_cast<Context *>(args[0]));
    }
    return true;
  };
  g.methods["hitTest"] = [](void **, void *ret, string *) {
    new (ret) GizmoHit();
    return true;
  };
  g.methods["draw"] = [](void **, void *, string *) { return true; };
  g.methods["beginDrag"] = [](void **, void *ret, string *) {
    new (ret) Status();
    return true;
  };
  return g;
}

Status registerTool(ToolDoc &d, FakeHost &host, Script *s, const char *id)
{
  ToolDef *def = sc::newToolDef(id);
  must(sc::toolDefUiName(*def, "Nudge"));
  must(sc::toolDefSetting(*def, "step", sc::makeFloat(1.0)));
  must(sc::toolDefEnumSetting(*def, "mode", "A B"));
  Obj factory;
  factory.methods["make"] = [&host, s](void **, void *ret, string *) {
    *static_cast<uint32_t *>(ret) = host.add(toolObj(s));
    return true;
  };
  Status st = sc::registerForeignTool(
      *d.ctx, *def, host.add(factory), "activate deactivate onEvent");
  litestl::alloc::Delete(def);
  return st;
}

TransformDelta moveBy(double x)
{
  TransformDelta t;
  t.kind = DeltaKind::Translate;
  t.vector = double3(x, 0.0, 0.0);
  return t;
}

/* Registration: ids, the reserved namespace, settings, and the factory's count. */
void testRegister()
{
  FakeHost host;
  {
    ToolDoc d;
    Script s;
    must(registerTool(d, host, &s, "js.nudge"));
    test_assert(isCode(registerTool(d, host, &s, "js.nudge"), StatusCode::AlreadyExists));
    test_assert(isCode(
        registerTool(d, host, &s, "core.mine"), StatusCode::InvalidArgument, "core."));
    test_assert(isCode(sc::registerForeignTool(*d.ctx, ToolDef("js.x"), 0, "onEvent"),
                       StatusCode::InvalidArgument,
                       "factory"));
    const ToolDef *def = d.rt.ext<ToolRegistry>().find("js.nudge");
    test_assert(def && def->findSetting("step") && def->findSetting("mode"));

    must(sc::setActiveTool(*d.ctx, "js.nudge"));
    test_assert(s.activates == 1 &&
                std::string(sc::activeTool(*d.ctx).value.c_str()) == "js.nudge");
    must(sc::setActiveTool(*d.ctx, kSelectTool));
    test_assert(s.deactivates == 1);
  }
  test_assert(host.allReleasedOnce());
}

/* A script drag: a press begins core.object.dragDelta and pushes a script modal, moves
 * call updateDrag, the release finishes. One undo step. */
void testScriptDrag()
{
  FakeHost host;
  {
    ToolDoc d;
    const BlockHandle a = d.box("a"), b = d.box("b", 3.0);
    d.select({a, b});
    Script s;
    must(registerTool(d, host, &s, "js.nudge"));
    s.onEvent = [&](Context &ctx, ToolManager &, const InputEvent &e) {
      if (e.type != EventType::PointerDown) {
        return EventResult::Unhandled;
      }
      must(sc::beginInteractiveAt(ctx, "", kDragDeltaCommand, sc::newRunInputs(), false));
      must(sc::pushModal(ctx, host.add(modalObj(&s)), "onEvent end", true));
      return EventResult::Handled;
    };
    s.modalEvent = [&](Context &ctx, const InputEvent &e) {
      if (e.type == EventType::PointerUp) {
        return ModalResult::Finish;
      }
      test_assert(sc::dragCount(ctx).value == 2);
      must(sc::updateDrag(ctx, moveBy(e.x / 100.0)));
      return ModalResult::Continue;
    };
    d.useTool("js.nudge");
    const std::string before = CommandDoc::liveOnly(d.describe());
    must(d.send(EventType::PointerDown, 0, 0).status());
    must(d.send(EventType::PointerMove, 100, 0).status());
    must(d.send(EventType::PointerMove, 250, 0).status());
    test_assert(std::abs(d.position(a)[0] - 2.5) < 1e-9 &&
                std::abs(d.position(b)[0] - 5.5) < 1e-9);
    must(d.send(EventType::PointerUp, 250, 0).status());
    test_assert(s.ends == 1 && s.committed && !d.ctx->executor()->interactiveOpen());

    /* updateDrag without a drag, and updateDragEach with the wrong count. */
    test_assert(
        isCode(sc::updateDrag(*d.ctx, moveBy(1)), StatusCode::FailedPrecondition));
    must(
        sc::beginInteractiveAt(*d.ctx, "", kDragDeltaCommand, sc::newRunInputs(), false));
    litestl::util::Vector<double> numbers;
    for (int i = 0; i < 16; i++) {
      numbers.append(0.0);
    }
    test_assert(
        isCode(sc::updateDragEach(*d.ctx, numbers), StatusCode::InvalidArgument, "16"));
    for (int i = 0; i < 16; i++) {
      numbers.append(0.0);
    }
    numbers[4] = numbers[16 + 4] = 1.0; /* identity bases */
    numbers[8] = 1.0;                   /* a moves 1 in x, b stays */
    /* A kind that isn't 0, 1 or 2 (casting NaN or 7 to DeltaKind was undefined). */
    for (double kind : {7.0, 0.5, -1.0, std::nan("")}) {
      numbers[16] = kind;
      test_assert(isCode(
          sc::updateDragEach(*d.ctx, numbers), StatusCode::InvalidArgument, "kind"));
    }
    numbers[16] = 0.0;
    must(sc::updateDragEach(*d.ctx, numbers));
    d.evaluate();
    test_assert(std::abs(d.position(a)[0] - 3.5) < 1e-9 &&
                std::abs(d.position(b)[0] - 5.5) < 1e-9);
    test_assert(isCode(sc::updateElementsDrag(*d.ctx, moveBy(1)),
                       StatusCode::FailedPrecondition,
                       "another"));
    must(sc::cancelInteractive(*d.ctx));

    must(sc::undo(*d.ctx));
    test_assert(CommandDoc::liveOnly(d.describe()) == before);
  }
  test_assert(host.allReleasedOnce());
}

/* The gate: a tool's onEvent may do what the top level may (invoke runs a top-level
 * command); a gizmo's update may only read. */
void testGates()
{
  FakeHost host;
  {
    ToolDoc d;
    const BlockHandle a = d.box("a");
    const std::string name = nameOf(d, a);
    Script s;
    must(registerTool(d, host, &s, "js.nudge"));
    std::vector<Status> inEvent, inUpdate;
    s.onEvent = [&](Context &ctx, ToolManager &, const InputEvent &e) {
      if (e.type == EventType::PointerDown) {
        inEvent.push_back(sc::invoke(ctx, "core.scene.new").status);
        inEvent.push_back(sc::worldMatrix(ctx, string(name.c_str())).status);
        const sc::IntResult g = sc::addForeignGizmo(
            ctx, host.add(gizmoObj(&s)), "update hitTest draw beginDrag");
        inEvent.push_back(g.status);
      }
      return EventResult::Handled;
    };
    s.gizmoUpdate = [&](Context &ctx) {
      inUpdate.push_back(sc::worldMatrix(ctx, string(name.c_str())).status);
      inUpdate.push_back(sc::invoke(ctx, "core.scene.new").status);
      inUpdate.push_back(sc::setActiveTool(ctx, kSelectTool));
    };
    d.useTool("js.nudge");
    const int steps = d.history->size();
    must(d.send(EventType::PointerDown, 10, 10).status());
    test_assert(inEvent.size() == 3);
    for (const Status &st : inEvent) {
      must(st);
    }
    test_assert(d.history->size() ==
                steps + 1); /* a top-level command, not a sub-command */
    must(d.send(EventType::PointerMove, 20, 20).status());
    test_assert(inUpdate.size() >= 3);
    must(inUpdate[0]);
    test_assert(isCode(inUpdate[1], StatusCode::Busy, "Gizmo.update"));
    test_assert(isCode(inUpdate[2], StatusCode::Busy));
    test_assert(std::string(sc::activeTool(*d.ctx).value.c_str()) == "js.nudge");

    /* Not from outside a tool's ToolManager. */
    d.ctx->setTools(nullptr);
    test_assert(
        isCode(sc::setActiveTool(*d.ctx, kSelectTool), StatusCode::FailedPrecondition));
    const fakehost::lf::Handle m = host.add(modalObj(&s));
    test_assert(isCode(sc::pushModal(*d.ctx, m, "onEvent", false),
                       StatusCode::FailedPrecondition));
    test_assert(host.releasesOf(m) == 1);
    d.ctx->setTools(&d.tm);
  }
  test_assert(host.allReleasedOnce());
}

/* The built-in transform gizmo, added by a script tool. */
void testBuiltinGizmo()
{
  FakeHost host;
  {
    ToolDoc d;
    const BlockHandle a = d.box("a");
    d.select({a});
    Script s;
    must(registerTool(d, host, &s, "js.nudge"));
    int32_t id = -1;
    s.onEvent = [&](Context &ctx, ToolManager &, const InputEvent &e) {
      if (e.type == EventType::KeyDown) {
        test_assert(isCode(sc::addGizmo(ctx, "lasso", "").status, StatusCode::NotFound));
        /* Another tool's settings: one that isn't registered. */
        test_assert(isCode(sc::addGizmo(ctx, "transform", "js.gone").status,
                           StatusCode::NotFound));
        const sc::IntResult g = sc::addGizmo(ctx, "transform", "js.nudge");
        must(g.status);
        id = int32_t(g.value);
      }
      return EventResult::Handled;
    };
    d.useTool("js.nudge");
    InputEvent key;
    key.type = EventType::KeyDown;
    key.key = 'g';
    must(d.send(key).status());
    test_assert(id >= 0);
    DrawList list;
    must(sc::drawTools(*d.ctx, d.view, list));
    test_assert(list.recordCount() > 0);
    must(sc::removeGizmo(*d.ctx, id));
  }
  test_assert(host.allReleasedOnce());
}

/* A script brush through core.tool.stroke: one instance per stroke, kept by its record for
 * undo and redo. Its begin, apply and undo may write values but not run commands. */
void testScriptBrush()
{
  FakeHost host;
  {
    ToolDoc d;
    const BlockHandle a = d.box("a", 0, 0, 0, 1.0);
    d.select({a});
    const std::string aName = nameOf(d, a);
    int made = 0, begins = 0, applies = 0, undos = 0;
    std::vector<Status> inApply;
    Obj factory;
    factory.methods["make"] = [&](void **, void *ret, string *) {
      made++;
      Obj b;
      b.methods["begin"] = [&](void **, void *r, string *) {
        begins++;
        new (r) Status();
        return true;
      };
      b.methods["apply"] = [&](void **args, void *r, string *) {
        if (applies++ == 0) {
          Context &ctx = *static_cast<Context *>(args[0]);
          inApply.push_back(sc::writeValue(ctx, "nothing.here", sc::makeFloat(1.0)));
          inApply.push_back(sc::invoke(ctx, "core.scene.new").status);
          inApply.push_back(sc::notifyChanged(ctx, string(aName.c_str())));
        }
        new (r) Status();
        return true;
      };
      b.methods["undo"] = [&](void **, void *, string *) {
        undos++;
        return true;
      };
      b.methods["memSize"] = [](void **, void *r, string *) {
        *static_cast<uint64_t *>(r) = 0;
        return true;
      };
      *static_cast<uint32_t *>(ret) = host.add(std::move(b));
      return true;
    };
    must(sc::registerForeignStrokeBrush(
        *d.ctx, "js.brush", host.add(factory), "begin apply undo memSize"));
    test_assert(isCode(sc::registerForeignStrokeBrush(*d.ctx, "core.b", host.add(Obj()), "begin"),
                       StatusCode::InvalidArgument,
                       "reserved"));
    test_assert(isCode(sc::registerForeignStrokeBrush(*d.ctx, "js.brush", host.add(Obj()), "begin"),
                       StatusCode::AlreadyExists));
    test_assert(isCode(sc::registerForeignStrokeBrush(*d.ctx, "js.none", 0, "begin"),
                       StatusCode::InvalidArgument));
    const ToolDef *def = d.rt.ext<ToolRegistry>().find(kStrokeTool);
    must(d.rt.ext<ToolSettingsStore>().set(*def, "brush", PropValue::ofString("js.brush")));
    d.useTool(kStrokeTool);
    const Pixel c = d.pixelOf(double3(0, 0, 0));
    must(d.send(EventType::PointerDown, c.x - 20, c.y).status());
    must(d.send(EventType::PointerMove, c.x, c.y).status());
    must(d.send(EventType::PointerMove, c.x + 20, c.y).status());
    must(d.send(EventType::PointerUp, c.x + 20, c.y).status());
    test_assert(made == 1 && begins == 1 && applies > 2 && d.history->size() == 1);
    test_assert(inApply.size() == 3);
    must(inApply[2]); /* notifyChanged is allowed where values may be written */
    test_assert(isCode(inApply[0], StatusCode::NotFound)); /* past the gate: no such path */
    test_assert(isCode(inApply[1], StatusCode::Busy));
    const int first = applies;
    d.undo();
    test_assert(undos == 1);
    d.redo();
    test_assert(made == 1 && begins == 2 && applies == 2 * first);
  }
  test_assert(host.allReleasedOnce());
}

} // namespace

test_init;

int main()
{
  testRegister();
  testScriptDrag();
  testGates();
  testBuiltinGizmo();
  testScriptBrush();
  return test_end();
}
