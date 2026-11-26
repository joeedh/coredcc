#pragma once

/** \file
 * ToolManager: one viewport's tools. It dispatches events to the modal, the gizmos and
 * the active tool, and draws their overlays (architecture §11, "ToolManager dispatch").
 */

#include "coredcc/api/context.h"
#include "coredcc/base/iface/owned.h"
#include "coredcc/draw/drawlist.h"
#include "coredcc/draw/view.h"
#include "coredcc/tool/input.h"

#include "litestl/util/string.h"
#include "litestl/util/vector.h"

#include <optional>
#include <string_view>

namespace coredcc {

enum class EventResult : int32_t; /* tool.h */
struct Tool;
struct Modal;
struct Gizmo;
class ToolDef;
class CommandExecutor;

/**
 * Dispatches an event to, in this order:
 * 1. the top modal;
 * 2. on a left press, the gizmo part under the cursor (the best `hitTest` of all gizmos
 *    within `hitRadius`, by distance, then view distance), whose `beginDrag` should push
 * a modal;
 * 3. the active tool;
 * 4. nobody: the event is Unhandled, for the host.
 *
 * - **Modals that own the interactive command** (pushModal) are checked before each
 *   event and draw, and after every slot call: one whose command isn't the executor's
 *   open one any more (an undo cancelled it, the host did, a slot committed it) is popped
 *   with the modals above it, and `end(ctx, false)` is called. The executor isn't called
 *   for it.
 * - **Errors:** a script's exception is the Runtime's sticky error, which the manager
 *   takes after every slot call. In an event slot it cancels the modals and is returned;
 *   in a draw or hit-test slot it is returned after the frame is drawn, and the gesture
 *   goes on. A sticky error that is pending before a call came from elsewhere: the call
 *   takes it and fails with FailedPrecondition.
 * - **Busy:** `setActive` while a modal is open, and every call of the manager from
 * inside one of its own slots; everything while a graph evaluates.
 * - **The Context's ToolManager** is this one during each of its calls (and restored
 *   after), so that what a slot does through `ctx.tools()`, as scripts do, acts on the
 *   manager that called it.
 * - **Slots' arguments** (the manager, the view, the event, the DrawList) are lent for
 *   the call only: a slot mustn't keep them.
 * - **Pointers:** a modal pushed during a pointer event gets the pointer events of that
 *   `pointerId` only; another pointer's are Unhandled, for the host.
 * - **Lifetime:** destroy it before the Runtime, and call `shutdown` before, with a
 *   Context, so that an open gesture is cancelled and the tool deactivated.
 */
class ToolManager {
public:
  explicit ToolManager(Runtime &rt);
  ~ToolManager();
  ToolManager(const ToolManager &) = delete;
  ToolManager &operator=(const ToolManager &) = delete;

  /** Deactivates the active tool and activates `toolId`; "" for none. NotFound for an id
   * that isn't registered. */
  Status setActive(Context &ctx, std::string_view toolId);
  /** The active tool's def, or null. */
  const ToolDef *active() const
  {
    return def_;
  }
  /**
   * An EventResult. InvalidArgument for an event whose type or pointer isn't one
   * (`InputEvent::valid`). An error of a gizmo's `update` or `hitTest` comes after the
   * event went on to the tool: with `late`, it goes there and the result is still
   * returned; without, it is returned instead of the result. The active tool's
   * `settingsChanged` is called first when its settings changed since the last event.
   */
  Result<EventResult>
  dispatch(Context &ctx, const View &view, const InputEvent &e, Status *late = nullptr);
  /** Begins `out` at `drawOrigin`, draws the scene's, the tool's, the gizmos' and the
   * modals' overlays, and ends it. Each slot draws above what was pushed before it: it
   * can't pop that, and what it leaves pushed is popped after it. */
  Status draw(Context &ctx, const View &view, DrawList &out);
  /** The DrawLists' origin: unset (the default), the camera's position. A host that
   * renders relative to another point (a floating origin, a tile's centre) sets it. */
  std::optional<double3> drawOrigin;
  /** Ends the gesture in progress, cancelling its command. Nothing without one. */
  Status cancel(Context &ctx);
  /** cancel, then deactivates the tool. */
  void shutdown(Context &ctx);

  /* ---- for tools and gizmos ---- */

  /**
   * Pushes a modal. With `ownsInteractive`, the executor's open interactive command is
   * its: the manager commits or cancels it when the modal finishes. FailedPrecondition if
   * none is open, or another modal owns it; the modal is destroyed then.
   */
  Status pushModal(Context &ctx, Owned<Modal> modal, bool ownsInteractive);
  bool modalOpen() const
  {
    return modals_.size() > 0;
  }
  int32_t modalCount() const
  {
    return int32_t(modals_.size());
  }
  /** Its id, 1 to kMaxGizmos: the smallest that no gizmo has, so ids are reused. -1 when
   * there are kMaxGizmos already. The manager owns it until it is removed, or the tool
   * changes. */
  int32_t addGizmo(Owned<Gizmo> gizmo);
  /** During a dispatch or a draw, the gizmo goes when that ends. */
  void removeGizmo(int32_t id);
  void clearGizmos();
  int32_t gizmoCount() const;
  /** The gizmo and part under the cursor at the last move, or -1. */
  int32_t hoverGizmo() const
  {
    return hoverGizmo_;
  }
  int32_t hoverPart() const
  {
    return hoverPart_;
  }
  /** Those of the drag in progress, or -1. */
  int32_t activeGizmo() const
  {
    return activeGizmo_;
  }
  int32_t activePart() const
  {
    return activePart_;
  }
  /** For the host's status bar; tools set it. When a gesture ends it becomes the
   * outcome's text, empty unless a slot set one. */
  const litestl::util::string &status() const
  {
    return status_;
  }
  void setStatus(std::string_view text);
  /** The text to show once the gesture in progress is over (why a click did nothing,
   * say), instead of an empty status. */
  void setOutcome(std::string_view text);

  /** Logical pixels. */
  double hitRadius = 6.0;
  /** Each gizmo's records' ids are its id times this, plus the part. */
  static constexpr uint32_t kIdStride = 65536;
  /** So that every record id fits in 32 bits. */
  static constexpr int32_t kMaxGizmos = 65535;

  Runtime &runtime() const
  {
    return rt_;
  }
  /** Bound opaque, so slots can take a ToolManager&. */
  static const litestl::binding::types::Struct<ToolManager> *defineBindings();

private:
  /* Defined in manager.cc: tool.h, which defines the interfaces, includes this. */
  struct ModalEntry;
  struct GizmoEntry;
  struct ToolSlot;
  class Call;

  Status enter(Context &ctx, const char *what);
  /* The sticky error a slot left, if any. */
  Status slotError();
  bool valid(Context &ctx, const ModalEntry &m) const;
  bool owned(const CommandExecutor *ex, uint64_t serial) const;
  /* Pops modal i, committing or cancelling its command; the last one to go puts the
   * outcome in the status. */
  Status finish(Context &ctx, int i, bool commit);
  /* Pops every modal from the top down to i, cancelling. */
  Status cancelFrom(Context &ctx, int i);
  /* Pops the modals whose command isn't open any more. */
  Status prune(Context &ctx);
  /* A slot began an interactive command and pushed no modal for it. */
  Status orphan(Context &ctx, uint64_t before);
  /* Only from dispatch, as settingsChanged is an event slot. */
  Status checkSettings(Context &ctx);
  Status updateGizmos(Context &ctx);
  /* The best part under a pixel: its gizmo's index, or -1. A gizmo's error goes in
   * `err`, and that gizmo is missed. */
  int bestHit(const ViewMath &view, double x, double y, int32_t &part, Status &err);
  void flushRemoved();
  Result<EventResult> toModal(Context &ctx, const ViewMath &view, const InputEvent &e);
  void deactivate(Context &ctx);

  Runtime &rt_;
  const ToolDef *def_ = nullptr;
  ToolSlot *tool_ = nullptr;
  uint64_t settingsVersion_ = 0;
  litestl::util::Vector<ModalEntry *> modals_;
  litestl::util::Vector<GizmoEntry *> gizmos_;
  int32_t hoverGizmo_ = -1, hoverPart_ = -1;
  int32_t activeGizmo_ = -1, activePart_ = -1;
  litestl::util::string status_;
  litestl::util::string outcome_;
  const InputEvent *event_ = nullptr; /* the event being dispatched */
  bool inCall_ = false;
};

} // namespace coredcc
