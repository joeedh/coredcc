#pragma once

/** \file
 * Tool, Modal and Gizmo, the interfaces of interactive tools, and ToolDef and
 * ToolRegistry, where tools are declared (architecture §11). ToolManager
 * (tool/manager.h) dispatches events to them.
 */

#include "coredcc/api/context.h"
#include "coredcc/base/iface/iface.h"
#include "coredcc/base/iface/owned.h"
#include "coredcc/draw/drawlist.h"
#include "coredcc/draw/view.h"
#include "coredcc/props/def.h"
#include "coredcc/tool/input.h"
#include "coredcc/tool/manager.h"

#include "litestl/util/function.h"
#include "litestl/util/map.h"
#include "litestl/util/string.h"
#include "litestl/util/vector.h"

#include <string_view>

namespace coredcc {

/** What Tool::onEvent returns, as a number. */
namespace eventresult {
inline constexpr int32_t kUnhandled = 0;
inline constexpr int32_t kHandled = 1;
} // namespace eventresult

/** What Modal::onEvent returns, as a number. */
namespace modalresult {
/** Keep the grab; the event is used. */
inline constexpr int32_t kContinue = 0;
/** Keep the grab; the host gets the event (a wheel zoom during a drag). */
inline constexpr int32_t kPassThrough = 1;
/** Done: commit the command it owns. */
inline constexpr int32_t kFinish = 2;
/** Done: cancel the command it owns. */
inline constexpr int32_t kCancel = 3;
} // namespace modalresult

/**
 * The persistent tool that the user picked. An instance lives from `activate` to
 * `deactivate`.
 *
 * - `activate` adds its gizmos (ToolManager::addGizmo). An error leaves no tool active.
 * - `deactivate` (optional): the manager removes the gizmos itself.
 * - `onEvent`: an event that no modal and no gizmo took, moves too (there is no separate
 *   hover). eventresult::kHandled or kUnhandled. It may run commands, begin an
 *   interactive one and push a modal for it.
 * - `drawOverlay` (optional): what it shows besides its gizmos. It may not change
 *   anything.
 * - `settingsChanged` (optional): its settings (ToolSettingsStore) changed since the last
 *   event or draw.
 */
struct Tool : IfaceRef {
  Status activate(Context &ctx, ToolManager &tm)
  {
    return CDCC_CALL(activate, ctx, tm);
  }
  bool hasDeactivate() const
  {
    return CDCC_HAS(deactivate);
  }
  void deactivate(Context &ctx, ToolManager &tm)
  {
    CDCC_CALL(deactivate, ctx, tm);
  }
  int32_t
  onEvent(Context &ctx, ToolManager &tm, const ViewMath &view, const InputEvent &e)
  {
    return CDCC_CALL(onEvent, ctx, tm, view, e);
  }
  bool hasDrawOverlay() const
  {
    return CDCC_HAS(drawOverlay);
  }
  void drawOverlay(const ViewMath &view, DrawList &out) const
  {
    CDCC_CALL(drawOverlay, view, out);
  }
  bool hasSettingsChanged() const
  {
    return CDCC_HAS(settingsChanged);
  }
  void settingsChanged(Context &ctx, ToolManager &tm)
  {
    CDCC_CALL(settingsChanged, ctx, tm);
  }

  CDCC_INTERFACE(Tool, "coredcc::Tool")
  {
    BIND_STRUCT_METHOD(st, activate, MARGS("ctx", "tm"));
    BIND_OPTIONAL_METHOD(st, deactivate, MARGS("ctx", "tm"));
    BIND_STRUCT_METHOD(st, onEvent, MARGS("ctx", "tm", "view", "e"));
    BIND_OPTIONAL_METHOD(st, drawOverlay, MARGS("view", "out"));
    BIND_OPTIONAL_METHOD(st, settingsChanged, MARGS("ctx", "tm"));
  }
};

/**
 * A grab of every event for one gesture. A tool or a gizmo pushes it, usually after
 * beginning an interactive command that the modal then owns (ToolManager::pushModal).
 *
 * - `onEvent`: a modalresult. On kFinish the manager commits the command it owns, on
 *   kCancel it cancels it; after either it pops the modal and calls `end`. Cancel and
 *   FocusLost events always end it with a cancel.
 * - `drawOverlay` (optional), as Tool's.
 * - `end` (optional): the modal is gone; `committed` says whether its command was
 *   committed. Also called when the command ended behind its back (an undo cancelled it).
 */
struct Modal : IfaceRef {
  int32_t
  onEvent(Context &ctx, ToolManager &tm, const ViewMath &view, const InputEvent &e)
  {
    return CDCC_CALL(onEvent, ctx, tm, view, e);
  }
  bool hasDrawOverlay() const
  {
    return CDCC_HAS(drawOverlay);
  }
  void drawOverlay(const ViewMath &view, DrawList &out) const
  {
    CDCC_CALL(drawOverlay, view, out);
  }
  bool hasEnd() const
  {
    return CDCC_HAS(end);
  }
  void end(Context &ctx, bool committed)
  {
    CDCC_CALL(end, ctx, committed);
  }

  CDCC_INTERFACE(Modal, "coredcc::Modal")
  {
    BIND_STRUCT_METHOD(st, onEvent, MARGS("ctx", "tm", "view", "e"));
    BIND_OPTIONAL_METHOD(st, drawOverlay, MARGS("view", "out"));
    BIND_OPTIONAL_METHOD(st, end, MARGS("ctx", "committed"));
  }
};

/** What Gizmo::hitTest found: a part, how far the cursor is from it, and its depth. */
struct GizmoHit {
  /** -1 for none. */
  int32_t part = -1;
  /** Logical pixels from the cursor to the part. */
  double distance = 0.0;
  /** ViewMath::viewDistance of the nearest point of the part. */
  double depth = 0.0;

  static litestl::binding::types::Struct<GizmoHit> *defineBindings();
};

/**
 * A handle on the screen that starts a drag: one gizmo with numbered parts (the arrows,
 * planes and centre of a translate gizmo, say).
 *
 * - `update` places it from the document, before each hit test and draw, but not during
 *   a gesture: then the gesture's modal draws what moves. It may only read.
 * - `hitTest`: the part nearest to a pixel. The manager takes the best of all gizmos'
 *   within ToolManager::hitRadius, by distance, then depth.
 * - `draw`: `hover` and `active` are parts or -1. Records' ids are `idBase + part`.
 * - `beginDrag`: a press on `part`. It begins the command and pushes a modal.
 */
struct Gizmo : IfaceRef {
  void update(Context &ctx)
  {
    CDCC_CALL(update, ctx);
  }
  GizmoHit hitTest(const ViewMath &view, double x, double y) const
  {
    return CDCC_CALL(hitTest, view, x, y);
  }
  void draw(const ViewMath &view,
            int32_t hover,
            int32_t active,
            uint32_t idBase,
            DrawList &out) const
  {
    CDCC_CALL(draw, view, hover, active, idBase, out);
  }
  Status beginDrag(Context &ctx,
                   ToolManager &tm,
                   const ViewMath &view,
                   const InputEvent &e,
                   int32_t part)
  {
    return CDCC_CALL(beginDrag, ctx, tm, view, e, part);
  }

  CDCC_INTERFACE(Gizmo, "coredcc::Gizmo")
  {
    BIND_STRUCT_METHOD(st, update, MARGS("ctx"));
    BIND_STRUCT_METHOD(st, hitTest, MARGS("view", "x", "y"));
    BIND_STRUCT_METHOD(st, draw, MARGS("view", "hover", "active", "idBase", "out"));
    BIND_STRUCT_METHOD(st, beginDrag, MARGS("ctx", "tm", "view", "e", "part"));
  }
};

/** Makes a tool's instance when it is activated. */
using ToolFactory = litestl::util::function<Result<Owned<Tool>>(Runtime &rt)>;

/** A declared tool. Register it with ToolRegistry, which makes it immutable. */
class ToolDef {
public:
  /** `id` is dotted, e.g. "core.tool.transform". */
  explicit ToolDef(std::string_view id);

  ToolDef &impl(std::string_view implName, ToolFactory factory);
  ToolDef &uiName(std::string_view s);
  ToolDef &description(std::string_view s);
  ToolDef &icon(std::string_view s);
  /** A setting, kept in ToolSettingsStore. */
  ToolDef &setting(PropertyDef def);
  /** Its options panel: a layout whose paths are relative to its settings. */
  ToolDef &layout(std::string_view xml);

  const litestl::util::string &name() const
  {
    return name_;
  }
  uint64_t key() const
  {
    return key_;
  }
  const litestl::util::string &uiNameText() const
  {
    return uiName_;
  }
  const litestl::util::string &descriptionText() const
  {
    return description_;
  }
  const litestl::util::string &iconText() const
  {
    return icon_;
  }
  const litestl::util::string &layoutXml() const
  {
    return layout_;
  }
  int settingCount() const
  {
    return int(settings_.size());
  }
  const PropertyDef &settingDef(int i) const
  {
    return settings_[i];
  }
  const PropertyDef *findSetting(std::string_view name) const;
  const litestl::util::string &implName() const
  {
    return implName_;
  }
  bool hasImpl() const
  {
    return bool(factory_);
  }
  Result<Owned<Tool>> instantiate(Runtime &rt) const
  {
    return factory_(rt);
  }

  static const litestl::binding::types::Struct<ToolDef> *defineBindings();

private:
  litestl::util::string name_;
  uint64_t key_ = 0;
  litestl::util::string uiName_;
  litestl::util::string description_;
  litestl::util::string icon_;
  litestl::util::string layout_;
  litestl::util::string implName_;
  litestl::util::Vector<PropertyDef, 1> settings_;
  ToolFactory factory_;
};

/** Registered tools. A Runtime extension. */
class ToolRegistry {
public:
  static constexpr const char *kExtName = "coredcc.ToolRegistry";

  explicit ToolRegistry(Runtime *rt) : rt_(*rt)
  {
  }
  ~ToolRegistry();
  ToolRegistry(const ToolRegistry &) = delete;
  ToolRegistry &operator=(const ToolRegistry &) = delete;

  /**
   * Checks and registers `def`: a dotted id that is free (AlreadyExists), a factory,
   * settings that are valid and uniquely named, and a layout that parses and is valid
   * against its settings' struct. Busy while a graph evaluates.
   */
  Result<const ToolDef *> add(ToolDef def);
  /** add's checks without registering. */
  Status validateDef(const ToolDef &def) const;
  const ToolDef *find(std::string_view id) const;
  /** NotFound for `id`, with suggestions. */
  Status unknown(std::string_view id) const;
  int size() const
  {
    return int(defs_.size());
  }
  const ToolDef &at(int i) const
  {
    return *defs_[i];
  }

private:
  Runtime &rt_;
  litestl::util::Vector<ToolDef *> defs_;
  litestl::util::Map<uint64_t, ToolDef *> byId_;
  /* Names the structs that layouts are checked against. */
  mutable uint64_t checks_ = 0;
};

/** Registers a bound C++ class as a tool's implementation, and the def. */
template <class Impl> Result<const ToolDef *> addNativeTool(Runtime &rt, ToolDef def);

} // namespace coredcc

#include "coredcc/base/iface/registry.h"

namespace coredcc {

template <class Impl> Result<const ToolDef *> addNativeTool(Runtime &rt, ToolDef def)
{
  const std::string impl = std::string(def.name().c_str()) + "#impl";
  ToolRegistry &reg = rt.ext<ToolRegistry>();
  if (reg.find(def.name().c_str())) {
    return Status::error(
        StatusCode::AlreadyExists,
        (std::string("tool '") + def.name().c_str() + "' exists").c_str());
  }
  def.impl(impl, [impl](Runtime &r) {
    return own<Tool>(r, impl, litestl::alloc::New<Impl>("coredcc::Tool"));
  });
  CDCC_TRY(reg.validateDef(def));
  CDCC_TRY((registerImpl<Tool, Impl>(rt, impl)));
  return reg.add(std::move(def));
}

} // namespace coredcc
