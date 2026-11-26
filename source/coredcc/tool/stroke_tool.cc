/* core.tool.stroke: a press begins core.stroke on the active object, moves feed it, a release
 * commits it. */
#include "coredcc/command/executor.h"
#include "coredcc/command/command.h"
#include "coredcc/scene/commands.h"
#include "coredcc/scene/scene.h"
#include "coredcc/tool/manager.h"
#include "coredcc/tool/settings.h"
#include "coredcc/tool/stroke.h"
#include "coredcc/tool/tool.h"

#include "litestl/binding/binding.h"

namespace coredcc {

namespace lbt = litestl::binding::types;

namespace detail {
PropertyDef strokeSpaceProp();
PropertyDef strokeMethodProp();
} // namespace detail

namespace {

constexpr const char *kStrokeModalImpl = "core.tool.stroke.modal#impl";
constexpr const char *kSettingNames[] = {
    "brush", "space", "method", "spacing", "radius", "radiusIsWorld", "symmetry"};

/* The cursor: the brush's circle. */
void drawCursor(const ViewMath &view, double x, double y, double radius, DrawList &out)
{
  DrawStyle style;
  style.color = 0xe0ffffffu;
  style.depth = int32_t(DepthMode::Always);
  out.circle2d(style, x, y, radius, false).ignore();
  (void)view;
}

double radiusOf(Runtime &rt, const ToolDef &def)
{
  ToolSettingsStore &store = rt.ext<ToolSettingsStore>();
  const PropValue *r = store.get(def, "radius");
  const PropValue *world = store.get(def, "radiusIsWorld");
  /* A world radius has no fixed size on the screen: the cursor shows a nominal one. */
  return world && world->asBool() ? 25.0 : (r ? r->asFloat() : 25.0);
}

} // namespace

namespace tools {

/** The stroke's grab: it owns core.stroke. */
struct StrokeModal {
  double x = 0.0, y = 0.0, radius = 25.0;

  int32_t onEvent(Context &ctx, ToolManager &, const ViewMath &view, const InputEvent &e)
  {
    if ((e.type == EventType::KeyDown && e.key == keys::kEscape) ||
        (e.type == EventType::PointerDown && e.button == 2))
    {
      return modalresult::kCancel;
    }
    if (e.type == EventType::Wheel) {
      return modalresult::kPassThrough;
    }
    CommandExecutor *ex = ctx.executor();
    const bool release = e.type == EventType::PointerUp && e.button == 0;
    if (e.type != EventType::PointerMove && !release) {
      return modalresult::kContinue;
    }
    x = e.x;
    y = e.y;
    int32_t samples = 0;
    Status s = ex->updateInteractive(ctx, [&](Context &c, Command &cmd) {
      StrokeCommand *stroke = cmd.as<StrokeCommand>();
      if (!stroke) {
        return Status::error(StatusCode::Internal, "the open command isn't core.stroke");
      }
      Status u = release ? stroke->finish(c, view) : stroke->update(c, view, e);
      samples = stroke->sampleCount();
      return u;
    });
    if (!s.ok()) {
      ctx.runtime().setStickyError(s); /* the manager cancels the stroke and returns it */
      return modalresult::kCancel;
    }
    if (!release) {
      return modalresult::kContinue;
    }
    return samples > 0 ? modalresult::kFinish : modalresult::kCancel;
  }
  void drawOverlay(const ViewMath &view, DrawList &out) const
  {
    drawCursor(view, x, y, radius, out);
  }
  void end(Context &, bool)
  {
  }

  static lbt::Struct<StrokeModal> *defineBindings()
  {
    static lbt::Struct<StrokeModal> *st = nullptr;
    if (!st) {
      litestl::alloc::PermanentGuard guard;
      st = new lbt::Struct<StrokeModal>("coredcc::tools::StrokeModal", sizeof(StrokeModal));
      BIND_STRUCT_METHOD(st, onEvent, MARGS("ctx", "tm", "view", "e"));
      BIND_STRUCT_METHOD(st, drawOverlay, MARGS("view", "out"));
      BIND_STRUCT_METHOD(st, end, MARGS("ctx", "committed"));
    }
    return st;
  }
};

/** Strokes on the active object (or the first selected one) with the brush its settings name. */
struct StrokeTool {
  const ToolDef *def = nullptr;
  double x = 0.0, y = 0.0, radius = 25.0;
  bool hover = false;

  Status activate(Context &ctx, ToolManager &)
  {
    def = ctx.runtime().ext<ToolRegistry>().find(kStrokeTool);
    return def ? Status()
               : Status::error(StatusCode::NotFound, "core.tool.stroke isn't registered");
  }

  int32_t onEvent(Context &ctx, ToolManager &tm, const ViewMath &view, const InputEvent &e)
  {
    if (e.isPointer()) {
      x = e.x;
      y = e.y;
      hover = true;
      radius = radiusOf(ctx.runtime(), *def);
    }
    if (e.type != EventType::PointerDown || e.button != 0) {
      return eventresult::kUnhandled;
    }
    Status s = begin(ctx, tm, view, e);
    if (!s.ok()) {
      tm.setStatus(std::string("Stroke: ") + s.message());
      if (ctx.executor() && ctx.executor()->interactiveOpen()) {
        ctx.executor()->cancel(ctx).ignore();
      }
    }
    return eventresult::kHandled;
  }

  void drawOverlay(const ViewMath &view, DrawList &out) const
  {
    if (hover) {
      drawCursor(view, x, y, radius, out);
    }
  }

  Status begin(Context &ctx, ToolManager &tm, const ViewMath &view, const InputEvent &e)
  {
    Result<detail::SceneOf> s = detail::sceneOf(ctx);
    CDCC_TRY(s.status());
    BlockHandle target = s->scene->active(*s->lib);
    if (!target && s->scene->selected(*s->lib).size() > 0) {
      target = s->scene->selected(*s->lib)[0];
    }
    if (!target) {
      return Status::error(StatusCode::FailedPrecondition, "no active object");
    }
    CommandExecutor *ex = ctx.executor();
    const CommandDef *cdef = ctx.runtime().ext<CommandRegistry>().find(kStrokeCommand);
    if (!ex || !cdef) {
      return Status::error(StatusCode::FailedPrecondition,
                           "the stroke tool needs an executor and core.stroke");
    }
    PropertyBag inputs = cdef->newInputs();
    ToolSettingsStore &store = ctx.runtime().ext<ToolSettingsStore>();
    for (const char *name : kSettingNames) {
      if (const PropValue *v = store.get(*def, name)) {
        CDCC_TRY(inputs.set(name, *v));
      }
    }
    CDCC_TRY(inputs.set("object", detail::pointerTo(*s->lib, target)));
    CDCC_TRY(ex->beginInteractive(ctx, *cdef, std::move(inputs)));
    CDCC_TRY(ex->updateInteractive(ctx, [&](Context &c, Command &cmd) {
      return cmd.as<StrokeCommand>()->update(c, view, e);
    }));
    StrokeModal *m = litestl::alloc::New<StrokeModal>("coredcc::tools::StrokeModal");
    m->x = e.x;
    m->y = e.y;
    m->radius = radius;
    Result<Owned<Modal>> modal = own<Modal>(ctx.runtime(), kStrokeModalImpl, m);
    CDCC_TRY(modal.status());
    return tm.pushModal(ctx, std::move(modal).value(), true);
  }

  static lbt::Struct<StrokeTool> *defineBindings()
  {
    static lbt::Struct<StrokeTool> *st = nullptr;
    if (!st) {
      litestl::alloc::PermanentGuard guard;
      st = new lbt::Struct<StrokeTool>("coredcc::tools::StrokeTool", sizeof(StrokeTool));
      BIND_STRUCT_METHOD(st, activate, MARGS("ctx", "tm"));
      BIND_STRUCT_METHOD(st, onEvent, MARGS("ctx", "tm", "view", "e"));
      BIND_STRUCT_METHOD(st, drawOverlay, MARGS("view", "out"));
    }
    return st;
  }
};

} // namespace tools

Status registerStrokeBuiltins(Runtime &rt)
{
  CDCC_TRY(registerStrokeCommand(rt));
  ToolRegistry &reg = rt.ext<ToolRegistry>();
  if (reg.find(kStrokeTool)) {
    return Status();
  }
  CDCC_TRY((registerImpl<Modal, tools::StrokeModal>(rt, kStrokeModalImpl)));
  const StrokeSettings d;
  ToolDef def(kStrokeTool);
  def.uiName("Stroke")
      .description("Paint strokes on the active object with a brush")
      .icon("brush")
      .setting(PropertyDef::stringProp("brush"))
      .setting(detail::strokeSpaceProp())
      .setting(detail::strokeMethodProp())
      .setting(PropertyDef::floatProp("spacing")
                   .defaultValue(PropValue::ofFloat(d.spacing))
                   .range(1e-4, 100.0))
      .setting(PropertyDef::floatProp("radius")
                   .defaultValue(PropValue::ofFloat(d.radius))
                   .range(1e-9, 1e9))
      .setting(PropertyDef::boolProp("radiusIsWorld"))
      .setting(PropertyDef::boolProp("symmetry"))
      .layout("<layout><prop path='brush'/><prop path='space'/><prop path='method'/>"
              "<prop path='spacing'/><prop path='radius'/><prop path='radiusIsWorld'/>"
              "<prop path='symmetry'/></layout>");
  return addNativeTool<tools::StrokeTool>(rt, std::move(def)).status();
}

} // namespace coredcc
