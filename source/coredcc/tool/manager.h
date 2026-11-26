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

#include <string_view>

namespace coredcc {

struct Tool;
struct Modal;
struct Gizmo;
class ToolDef;

/**
 * Dispatches an event to, in this order:
 * 1. the top modal;
 * 2. on a left press, the gizmo part under the cursor (the best `hitTest` of all gizmos
 *    within `hitRadius`, by distance, then depth), whose `beginDrag` should push a modal;
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
  /** An eventresult. */
  Result<int32_t> dispatch(Context &ctx, const View &view, const InputEvent &e);
  /** Begins `out` at the camera's position, draws the tool's, the gizmos' and the modals'
   * overlays, and ends it. */
  Status draw(Context &ctx, const View &view, DrawList &out);
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
  /** Its id, 1 or more. The manager owns it until it is removed, or the tool changes. */
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
  /** For the host's status bar; tools set it. */
  const litestl::util::string &status() const
  {
    return status_;
  }
  void setStatus(std::string_view text);

  /** Logical pixels. */
  double hitRadius = 6.0;
  /** Each gizmo's records' ids are its id times this, plus the part. */
  static constexpr uint32_t kIdStride = 65536;

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
  bool owned(uint64_t serial) const;
  /* Pops modal i, which must be the top one, committing or cancelling its command. */
  Status finish(Context &ctx, int i, bool commit);
  /* Pops every modal from the top down to i, cancelling. */
  Status cancelFrom(Context &ctx, int i);
  Status prune(Context &ctx);
  /* A slot began an interactive command and pushed no modal for it. */
  Status orphan(Context &ctx, uint64_t before);
  Status checkSettings(Context &ctx);
  Status updateGizmos(Context &ctx);
  /* The best part under a pixel: its gizmo's index, or -1. A gizmo's error goes in
   * `err`, and that gizmo is missed. */
  int bestHit(const ViewMath &view, double x, double y, int32_t &part, Status &err);
  void flushRemoved();
  Result<int32_t> toModal(Context &ctx, const ViewMath &view, const InputEvent &e);
  void deactivate(Context &ctx);

  Runtime &rt_;
  const ToolDef *def_ = nullptr;
  ToolSlot *tool_ = nullptr;
  uint64_t settingsVersion_ = 0;
  litestl::util::Vector<ModalEntry *> modals_;
  litestl::util::Vector<GizmoEntry *> gizmos_;
  int32_t nextGizmo_ = 1;
  int32_t hoverGizmo_ = -1, hoverPart_ = -1;
  int32_t activeGizmo_ = -1, activePart_ = -1;
  litestl::util::string status_;
  bool inCall_ = false;
};

} // namespace coredcc
