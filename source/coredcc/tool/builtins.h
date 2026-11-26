#pragma once

/** \file
 * The built-in tools: core.tool.select and core.tool.transform, and what they share:
 * selecting with a click or a box. core.tool.stroke is in tool/stroke.h.
 */

#include "coredcc/tool/gizmos/drag_math.h"
#include "coredcc/tool/tool.h"

namespace coredcc {

inline constexpr const char *kSelectTool = "core.tool.select";
inline constexpr const char *kTransformTool = "core.tool.transform";

/** Registers the built-in tools (core.tool.stroke too), the transform gizmo and the modals
 * (idempotent). Needs the scene's commands (registerSceneCommands). */
Status registerToolBuiltins(Runtime &rt);

namespace tools {

/** Logical pixels a press must move to become a box. */
inline constexpr double kBoxThreshold = 4.0;

/**
 * Starts a selection on a left press: pushes a SelectModal and returns Handled; Unhandled
 * for other events. On release, a click runs core.scene.select with what pickScene ranks
 * first: SET, or TOGGLE with Shift (a click on nothing with SET selects nothing). A drag
 * beyond kBoxThreshold is a box: pickRegion, then SET, ADD with Shift, REMOVE with Ctrl.
 * Each selection is one undo step.
 */
int32_t
selectOnPress(Context &ctx, ToolManager &tm, const ViewMath &view, const InputEvent &e);

/** The press-and-release of a selection. It owns no command. */
struct SelectModal {
  gizmo::Pixel press;
  gizmo::Pixel now;
  bool box = false;

  int32_t
  onEvent(Context &ctx, ToolManager &tm, const ViewMath &view, const InputEvent &e);
  void drawOverlay(const ViewMath &view, DrawList &out) const;
  static litestl::binding::types::Struct<SelectModal> *defineBindings();
};

struct SelectTool {
  Status activate(Context &ctx, ToolManager &tm);
  int32_t
  onEvent(Context &ctx, ToolManager &tm, const ViewMath &view, const InputEvent &e);
  static litestl::binding::types::Struct<SelectTool> *defineBindings();
};

/** Adds the transform gizmo; a press that misses it selects. */
struct TransformTool {
  Status activate(Context &ctx, ToolManager &tm);
  int32_t
  onEvent(Context &ctx, ToolManager &tm, const ViewMath &view, const InputEvent &e);
  static litestl::binding::types::Struct<TransformTool> *defineBindings();
};

inline constexpr const char *kSelectModalImpl = "core.tool.select.modal#impl";

} // namespace tools

} // namespace coredcc
