#pragma once

/* The headless host of the tool tests: a scene document with the scene's commands, an
 * executor and a LinearHistory (scene_test::CommandDoc), the built-in tools, a
 * ToolManager and a View. Like a host, it evaluates the scene after every event, so that
 * gizmos and picks see what would be drawn. */

#include "../scene/command_doc.h"

#include "coredcc/scene/pick_scene.h"
#include "coredcc/tool/builtins.h"
#include "coredcc/tool/event_script.h"
#include "coredcc/tool/gizmos/transform_gizmo.h"
#include "coredcc/tool/manager.h"
#include "coredcc/tool/settings.h"

#include "litestl/math/transform.h"

#include <string>

namespace tool_test {

using namespace scene_test;
using namespace coredcc::gizmo;

struct ToolDoc : CommandDoc {
  ToolManager tm{rt};
  View view;
  ViewMath vm;

  ToolDoc()
  {
    must(registerToolBuiltins(rt));
    must(registerToolBuiltins(rt)); /* idempotent */
    ctx->setTools(&tm);
    lookFrom(double3(6.0, -8.0, 5.0));
  }
  ~ToolDoc()
  {
    tm.shutdown(*ctx);
    ctx->setTools(nullptr);
  }

  /** A camera at `eye` looking at `target`, z up. */
  void lookFrom(const double3 &eye, const double3 &target = double3(0.0, 0.0, 0.0))
  {
    view = View();
    view.viewport[2] = 800;
    view.viewport[3] = 600;
    view.view = litestl::math::lookAt(eye, target, double3(0.0, 0.0, 1.0));
    view.proj = litestl::math::perspective(0.8, 800.0 / 600.0, 0.1, 1000.0);
    must(vm.set(view));
  }
  /** A box of half-size `size` at a place, in the scene. */
  BlockHandle
  box(const char *name, double x = 0, double y = 0, double z = 0, double size = 0.5)
  {
    const BlockHandle h = object(name, x, y, z);
    setData(h, shapeBlock((std::string(name) + ".shape").c_str(), size));
    return h;
  }
  void useTool(const char *id)
  {
    evaluate();
    must(tm.setActive(*ctx, id));
  }
  void setting(const char *name, const PropValue &v)
  {
    const ToolDef *def = rt.ext<ToolRegistry>().find(kTransformTool);
    must(rt.ext<ToolSettingsStore>().set(*def, name, v));
  }
  void settingKey(const char *name, const char *key)
  {
    const ToolDef *def = rt.ext<ToolRegistry>().find(kTransformTool);
    const EnumItem *item = def->findSetting(name)->enumMeta()->findKey(key);
    test_assert(item != nullptr);
    setting(name, PropValue::ofEnum(item->value));
  }
  /** Dispatches, then evaluates as a frame would. */
  Result<int32_t> send(const InputEvent &e)
  {
    Result<int32_t> r = tm.dispatch(*ctx, view, e);
    must(evaluateScene(*ctx));
    return r;
  }
  Result<int32_t>
  send(EventType type, double x, double y, uint32_t mods = 0, int32_t button = 0)
  {
    InputEvent e;
    e.type = type;
    e.x = x;
    e.y = y;
    e.button = button;
    e.modifiers = mods;
    e.buttons = type == EventType::PointerUp ? 0u : 1u;
    return send(e);
  }
  Status play(const char *script)
  {
    Result<Vector<InputEvent>> events = parseEventScript(script);
    CDCC_TRY(events.status());
    for (const InputEvent &e : *events) {
      Result<int32_t> r = send(e);
      CDCC_TRY(r.status());
    }
    return Status();
  }
  /** The transform gizmo's frame now, as it would hit-test and draw. */
  Frame gizmoFrame()
  {
    const ToolDef *def = rt.ext<ToolRegistry>().find(kTransformTool);
    const TransformSettings s = TransformSettings::read(rt, *def);
    return makeFrame(vm, computePlacement(*ctx, s.pivot, s.orientation), s);
  }
  Pixel pixelOf(const double3 &p)
  {
    Pixel px;
    test_assert(toPixel(vm, p, px));
    return px;
  }
  double3 position(BlockHandle h)
  {
    const double *d = result(h).state.world;
    return double3(d[12], d[13], d[14]);
  }
};

} // namespace tool_test
