/* core.tool.transform: the transform gizmo of its mode; presses that miss it select. */
#include "coredcc/tool/builtins.h"
#include "coredcc/tool/gizmos/transform_gizmo.h"
#include "coredcc/tool/manager.h"

#include "litestl/binding/binding.h"

namespace coredcc::tools {

namespace lbt = litestl::binding::types;

Status TransformTool::activate(Context &ctx, ToolManager &tm)
{
  const ToolDef *def = ctx.runtime().ext<ToolRegistry>().find(kTransformTool);
  if (!def) {
    return Status::error(StatusCode::NotFound, "core.tool.transform isn't registered");
  }
  auto *g = litestl::alloc::New<gizmo::TransformGizmo>("coredcc::gizmo::TransformGizmo");
  g->tool = def;
  Result<Owned<Gizmo>> owned = own<Gizmo>(ctx.runtime(), gizmo::kTransformGizmoImpl, g);
  CDCC_TRY(owned.status());
  tm.addGizmo(std::move(owned).value());
  return Status();
}

int32_t TransformTool::onEvent(Context &ctx,
                               ToolManager &tm,
                               const ViewMath &view,
                               const InputEvent &e)
{
  return selectOnPress(ctx, tm, view, e);
}

lbt::Struct<TransformTool> *TransformTool::defineBindings()
{
  static lbt::Struct<TransformTool> *st = nullptr;
  if (!st) {
    litestl::alloc::PermanentGuard guard;
    st = new lbt::Struct<TransformTool>("coredcc::tools::TransformTool",
                                        sizeof(TransformTool));
    BIND_STRUCT_METHOD(st, activate, MARGS("ctx", "tm"));
    BIND_STRUCT_METHOD(st, onEvent, MARGS("ctx", "tm", "view", "e"));
  }
  return st;
}

} // namespace coredcc::tools
