/* Selecting with a click or a box: core.tool.select, and the transform tool's presses
 * that miss its gizmo. */
#include "coredcc/api/ref_text.h"
#include "coredcc/command/executor.h"
#include "coredcc/scene/commands.h"
#include "coredcc/scene/pick_scene.h"
#include "coredcc/tool/builtins.h"
#include "coredcc/tool/manager.h"

#include "litestl/binding/binding.h"

#include <cmath>

namespace coredcc::tools {

namespace lbt = litestl::binding::types;
using litestl::util::Vector;

namespace {

Status
runSelect(Context &ctx, const char *mode, BlockHandle object, const std::string *objects)
{
  const CommandDef *def = ctx.runtime().ext<CommandRegistry>().find(kSelectCommand);
  if (!def || !ctx.executor() || !ctx.library()) {
    return Status::error(
        StatusCode::FailedPrecondition,
        "selecting needs an executor, a library and the scene's commands");
  }
  PropertyBag in = def->newInputs();
  const EnumMeta *items = def->findInput("mode")->enumMeta();
  const EnumItem *item = items ? items->findKey(mode) : nullptr;
  if (!item) {
    return Status::error(StatusCode::Internal, "core.scene.select has no such mode");
  }
  CDCC_TRY(in.set("mode", PropValue::ofEnum(item->value)));
  if (object) {
    CDCC_TRY(in.set("object", detail::pointerTo(*ctx.library(), object)));
  }
  if (objects) {
    CDCC_TRY(in.set("objects", PropValue::ofString(*objects)));
  }
  return ctx.executor()->run(ctx, *def, std::move(in)).status();
}

Status
clickSelect(Context &ctx, ToolManager &tm, const ViewMath &view, const InputEvent &e)
{
  PickOptions options;
  options.radius = tm.hitRadius;
  options.maxHits = 1;
  Vector<PickHit> hits;
  Status picked =
      pickScene(ctx, ctx.scene(), view, e.x, e.y, options, PickFilter(), hits);
  if (!picked.ok() && picked.code() != StatusCode::FailedPrecondition) {
    return picked; /* FailedPrecondition: nothing was evaluated yet, so nothing is hit */
  }
  const BlockHandle hit = hits.size() > 0 ? hits[0].object : BlockHandle();
  if (e.has(modifiers::kShift)) {
    return hit ? runSelect(ctx, "TOGGLE", hit, nullptr) : Status();
  }
  return runSelect(ctx, "SET", hit, nullptr);
}

Status boxSelect(
    Context &ctx, const ViewMath &view, gizmo::Pixel a, gizmo::Pixel b, uint32_t mods)
{
  const ViewRegion region = view.regionFromRect(a.x, a.y, b.x, b.y);
  Vector<RegionHit> hits;
  CDCC_TRY(pickRegion(ctx, ctx.scene(), region, -1, PickFilter(), hits));
  Vector<BlockId> ids;
  for (const RegionHit &h : hits) {
    ids.append(ctx.library()->get(h.object)->id());
  }
  const std::string text = formatBlockIds(
      ctx.runtime(), std::span<const BlockId>(ids.data(), size_t(ids.size())));
  const char *mode = (mods & modifiers::kCtrl)    ? "REMOVE"
                     : (mods & modifiers::kShift) ? "ADD"
                                                  : "SET";
  if (ids.size() == 0) {
    return std::string(mode) == "SET" ? runSelect(ctx, "SET", BlockHandle(), nullptr)
                                      : Status();
  }
  return runSelect(ctx, mode, BlockHandle(), &text);
}

} // namespace

int32_t
selectOnPress(Context &ctx, ToolManager &tm, const ViewMath &, const InputEvent &e)
{
  if (e.type != EventType::PointerDown || e.button != 0) {
    return eventresult::kUnhandled;
  }
  SelectModal *m = litestl::alloc::New<SelectModal>("coredcc::tools::SelectModal");
  m->press = m->now = gizmo::Pixel{e.x, e.y};
  Result<Owned<Modal>> modal = own<Modal>(ctx.runtime(), kSelectModalImpl, m);
  if (!modal.ok() || !tm.pushModal(ctx, std::move(modal).value(), false).ok()) {
    return eventresult::kUnhandled;
  }
  return eventresult::kHandled;
}

int32_t SelectModal::onEvent(Context &ctx,
                             ToolManager &tm,
                             const ViewMath &view,
                             const InputEvent &e)
{
  if ((e.type == EventType::KeyDown && e.key == keys::kEscape) ||
      (e.type == EventType::PointerDown && e.button == 2))
  {
    return modalresult::kCancel;
  }
  if (e.type == EventType::Wheel) {
    return modalresult::kPassThrough;
  }
  if (e.type == EventType::PointerMove || e.type == EventType::PointerUp) {
    now = gizmo::Pixel{e.x, e.y};
    const double dpi = view.view().dpiScale > 0.0 ? view.view().dpiScale : 1.0;
    box = box || std::hypot(now.x - press.x, now.y - press.y) / dpi > kBoxThreshold;
  }
  if (e.type != EventType::PointerUp || e.button != 0) {
    return modalresult::kContinue;
  }
  const Status s =
      box ? boxSelect(ctx, view, press, now, e.modifiers) : clickSelect(ctx, tm, view, e);
  if (!s.ok()) {
    tm.setStatus(s.message());
  }
  return modalresult::kFinish;
}

void SelectModal::drawOverlay(const ViewMath &, DrawList &out) const
{
  if (!box) {
    return;
  }
  DrawStyle st;
  st.color = 0xccffffffu;
  st.width = 1.0f;
  st.depth = int32_t(DepthMode::Always);
  const double xy[10] = {
      press.x, press.y, now.x, press.y, now.x, now.y, press.x, now.y, press.x, press.y};
  out.line2d(st, std::span<const double>(xy, 10)).ignore();
}

lbt::Struct<SelectModal> *SelectModal::defineBindings()
{
  static lbt::Struct<SelectModal> *st = nullptr;
  if (!st) {
    litestl::alloc::PermanentGuard guard;
    st = new lbt::Struct<SelectModal>("coredcc::tools::SelectModal", sizeof(SelectModal));
    BIND_STRUCT_METHOD(st, onEvent, MARGS("ctx", "tm", "view", "e"));
    BIND_STRUCT_METHOD(st, drawOverlay, MARGS("view", "out"));
  }
  return st;
}

/* ---- core.tool.select ---- */

Status SelectTool::activate(Context &, ToolManager &)
{
  return Status();
}

int32_t SelectTool::onEvent(Context &ctx,
                            ToolManager &tm,
                            const ViewMath &view,
                            const InputEvent &e)
{
  return selectOnPress(ctx, tm, view, e);
}

lbt::Struct<SelectTool> *SelectTool::defineBindings()
{
  static lbt::Struct<SelectTool> *st = nullptr;
  if (!st) {
    litestl::alloc::PermanentGuard guard;
    st = new lbt::Struct<SelectTool>("coredcc::tools::SelectTool", sizeof(SelectTool));
    BIND_STRUCT_METHOD(st, activate, MARGS("ctx", "tm"));
    BIND_STRUCT_METHOD(st, onEvent, MARGS("ctx", "tm", "view", "e"));
  }
  return st;
}

} // namespace coredcc::tools
