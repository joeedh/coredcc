/* Registration of the built-in tools, gizmos and modals. */
#include "coredcc/tool/builtins.h"

#include "coredcc/base/iface/registry.h"
#include "coredcc/tool/gizmos/transform_gizmo.h"

#include <numbers>

namespace coredcc {

namespace {

constexpr const char *kTransformLayout = "<layout>"
                                         "<prop path='mode'/>"
                                         "<prop path='orientation'/>"
                                         "<prop path='pivot'/>"
                                         "<prop path='moveStep'/>"
                                         "<prop path='angleStep'/>"
                                         "<prop path='scaleStep'/>"
                                         "<prop path='size'/>"
                                         "</layout>";

ToolDef transformDef()
{
  ToolDef def(kTransformTool);
  def.uiName("Transform")
      .description("Move, rotate and scale the selected objects with a gizmo")
      .icon("transform")
      .setting(PropertyDef::enumProp("mode")
                   .item("TRANSLATE", int64_t(gizmo::Mode::Translate))
                   .item("ROTATE", int64_t(gizmo::Mode::Rotate))
                   .item("SCALE", int64_t(gizmo::Mode::Scale)))
      .setting(PropertyDef::enumProp("orientation")
                   .item("GLOBAL", int64_t(gizmo::Orientation::Global))
                   .item("LOCAL", int64_t(gizmo::Orientation::Local))
                   .item("VIEW", int64_t(gizmo::Orientation::View)))
      .setting(PropertyDef::enumProp("pivot")
                   .item("MEDIAN", int64_t(gizmo::PivotMode::Median))
                   .item("ACTIVE", int64_t(gizmo::PivotMode::Active))
                   .item("INDIVIDUAL", int64_t(gizmo::PivotMode::Individual)))
      .setting(PropertyDef::floatProp("moveStep")
                   .defaultValue(PropValue::ofFloat(1.0))
                   .range(0.0, 1e6))
      .setting(PropertyDef::floatProp("angleStep")
                   .defaultValue(PropValue::ofFloat(5.0 * std::numbers::pi / 180.0))
                   .range(0.0, 2.0 * std::numbers::pi))
      .setting(PropertyDef::floatProp("scaleStep")
                   .defaultValue(PropValue::ofFloat(0.1))
                   .range(0.0, 100.0))
      .setting(PropertyDef::floatProp("size")
                   .defaultValue(PropValue::ofFloat(gizmo::look::kSize))
                   .range(10.0, 1000.0))
      .layout(kTransformLayout);
  return def;
}

} // namespace

Status registerToolBuiltins(Runtime &rt)
{
  ToolRegistry &reg = rt.ext<ToolRegistry>();
  if (reg.find(kSelectTool) && reg.find(kTransformTool)) {
    return Status(); /* registered already */
  }
  CDCC_TRY((registerImpl<Gizmo, gizmo::TransformGizmo>(rt, gizmo::kTransformGizmoImpl)));
  CDCC_TRY((registerImpl<Modal, gizmo::TransformModal>(rt, gizmo::kTransformModalImpl)));
  CDCC_TRY((registerImpl<Modal, tools::SelectModal>(rt, tools::kSelectModalImpl)));
  if (!reg.find(kSelectTool)) {
    ToolDef select(kSelectTool);
    select.uiName("Select")
        .description("Select objects with a click or a box")
        .icon("select");
    CDCC_TRY(addNativeTool<tools::SelectTool>(rt, std::move(select)).status());
  }
  if (!reg.find(kTransformTool)) {
    CDCC_TRY(addNativeTool<tools::TransformTool>(rt, transformDef()).status());
  }
  return Status();
}

} // namespace coredcc
