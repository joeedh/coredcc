#pragma once

/** \file
 * The transform gizmo (translate, rotate or scale, by the transform tool's `mode`) and
 * the modal of its drag, which runs core.object.dragDelta.
 */

#include "coredcc/scene/transform.h"
#include "coredcc/tool/gizmos/gizmo_common.h"
#include "coredcc/tool/tool.h"

namespace coredcc {
class ApplyDelta;
}

namespace coredcc::gizmo {

/** The transform tool's settings, read from ToolSettingsStore. */
struct TransformSettings {
  Mode mode = Mode::Translate;
  Orientation orientation = Orientation::Global;
  PivotMode pivot = PivotMode::Median;
  double moveStep = 1.0;
  /** Radians. */
  double angleStep = 0.0872664625997164788; /* 5° */
  double scaleStep = 0.1;
  /** Logical pixels. */
  double size = look::kSize;
  /** Elements instead of objects: of `domain` (an ElementDomain), mirrored with
   * `symmetry`. */
  bool elements = false;
  int32_t domain = 0;
  bool symmetry = false;

  static TransformSettings read(Runtime &rt, const ToolDef &def);
};

/** One frame's geometry of the gizmo: what both its hit test and its drawing use. */
struct Frame {
  Mode mode = Mode::Translate;
  double3 pivot;
  /** The gizmo's axes in the world, unit length. */
  double3 axes[3];
  quatd basis;
  /** The world length of the gizmo's size. */
  double unit = 0.0;
  double3 eye;
  /** How much of each axis, and each plane, is shown (0 hidden). */
  double axisShown[3] = {0, 0, 0};
  double planeShown[3] = {0, 0, 0};
  Pixel centre;
  bool onScreen = false;
  /** Logical pixels: the gizmo's size, and device pixels per logical pixel. */
  double size = look::kSize;
  double dpi = 1.0;

  /** The parts that exist in this mode. */
  bool hasPart(int32_t part) const;
  /** Logical pixels from a device pixel to a part; infinity for a hidden part. */
  double distance(const ViewMath &view, int32_t part, Pixel p) const;
  /** Draws a part; `hover` and `active` brighten it. */
  void
  draw(const ViewMath &view, int32_t part, bool hi, uint32_t id, DrawList &out) const;
};

Frame makeFrame(const ViewMath &view,
                const Placement &placement,
                const TransformSettings &settings);

/** The gizmo of core.tool.transform. */
struct TransformGizmo {
  const ToolDef *tool = nullptr;
  Placement placement;
  TransformSettings settings;

  void update(Context &ctx);
  GizmoHit hitTest(const ViewMath &view, double x, double y) const;
  void draw(const ViewMath &view,
            int32_t hover,
            int32_t active,
            uint32_t idBase,
            DrawList &out) const;
  Status beginDrag(Context &ctx,
                   ToolManager &tm,
                   const ViewMath &view,
                   const InputEvent &e,
                   int32_t part);
  static litestl::binding::types::Struct<TransformGizmo> *defineBindings();
};

/**
 * A drag of one part. It owns the open core.object.dragDelta: each move updates it with
 * the total delta since the press; a release commits it, unless nothing changed (a click
 * makes no step); Escape and a right press cancel it.
 */
struct TransformModal {
  /** Where the gizmo was drawn at the press, and its frame then: the drag's screen math
   * uses it, as the press was on what was drawn. */
  Placement placement;
  Frame frame;
  /** What the deltas turn and scale about: the objects' pivot when the drag began, from
   * its command, which is newer than the last evaluation `placement` is from. */
  double3 pivot;
  TransformSettings settings;
  int32_t part = -1;
  uint32_t id = 0;
  Pixel press;
  /* The press's point on the axis or the plane, as a parameter or a point. */
  double pressT = 0.0;
  double3 pressPoint;
  bool pressOk = false;
  AngleTracker angle;
  double handedness = 1.0;
  /** What the last move gave, for the overlay and the status text. */
  TransformDelta last;
  double amount = 0.0;

  ModalResult
  onEvent(Context &ctx, ToolManager &tm, const ViewMath &view, const InputEvent &e);
  void drawOverlay(const ViewMath &view, DrawList &out) const;
  void end(Context &ctx, bool committed);
  static litestl::binding::types::Struct<TransformModal> *defineBindings();

  /** Takes the press's geometry from `frame`, `part` and `press`. */
  void begin(const ViewMath &view);
  /** The delta for the cursor at `p`; false when the geometry gives none (keep the last).
   * `snapping` rounds it to the settings' steps. */
  bool deltaAt(const ViewMath &view, Pixel p, bool snapping, TransformDelta &out);
  /** The delta for each object, for INDIVIDUAL and LOCAL. */
  Status perObject(Context &ctx,
                   ApplyDelta &cmd,
                   const TransformDelta &d,
                   litestl::util::Vector<TransformDelta> &out) const;
};

/** The implementation names, registered by registerToolBuiltins. */
inline constexpr const char *kTransformGizmoImpl = "core.gizmo.transform#impl";
inline constexpr const char *kTransformModalImpl = "core.gizmo.transform.modal#impl";

} // namespace coredcc::gizmo
