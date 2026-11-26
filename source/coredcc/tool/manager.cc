#include "coredcc/tool/manager.h"

#include "coredcc/tool/tool.h"

#include "coredcc/command/executor.h"
#include "coredcc/tool/scene_overlay.h"
#include "coredcc/tool/settings.h"

#include "litestl/binding/binding.h"
#include "litestl/math/transform.h"
#include "litestl/util/alloc.h"

#include <string>

namespace coredcc {

namespace lbt = litestl::binding::types;

namespace {

/* The first error of two. */
Status first(const Status &a, const Status &b)
{
  return a.ok() ? b : a;
}

/* The camera's position: the view matrix's inverse, applied to the origin. */
double3 eyeOf(const View &view)
{
  mat4d inv;
  if (!litestl::math::tryInverse(view.view, inv)) {
    return double3(0.0, 0.0, 0.0);
  }
  const double *d = inv;
  return double3(d[12], d[13], d[14]);
}

} // namespace

struct ToolManager::ModalEntry {
  Owned<Modal> modal;
  bool owns = false;
  /* The command it owns: the executor's open one with this serial. */
  const CommandExecutor *executor = nullptr;
  uint64_t serial = 0;
  /* Pushed during a pointer event: that pointer's events only. */
  bool boundPointer = false;
  int32_t pointerId = 0;
};

struct ToolManager::GizmoEntry {
  int32_t id = 0;
  Owned<Gizmo> gizmo;
  bool removed = false;
};

struct ToolManager::ToolSlot {
  Owned<Tool> tool;
};

/* Marks the manager as inside a call, so that its slots can't call it again, and makes
 * it the Context's ToolManager for the call. */
class ToolManager::Call {
public:
  Call(ToolManager &tm, Context &ctx) : tm_(tm), ctx_(ctx), was_(ctx.tools())
  {
    tm_.inCall_ = true;
    ctx_.setTools(&tm_);
  }
  ~Call()
  {
    ctx_.setTools(was_);
    tm_.inCall_ = false;
    tm_.event_ = nullptr;
    tm_.flushRemoved();
  }
  Call(const Call &) = delete;
  Call &operator=(const Call &) = delete;

private:
  ToolManager &tm_;
  Context &ctx_;
  ToolManager *was_;
};

ToolManager::ToolManager(Runtime &rt) : rt_(rt)
{
}

ToolManager::~ToolManager()
{
  if (tool_) {
    litestl::alloc::Delete(tool_);
  }
  for (ModalEntry *m : modals_) {
    litestl::alloc::Delete(m);
  }
  for (GizmoEntry *g : gizmos_) {
    litestl::alloc::Delete(g);
  }
}

const lbt::Struct<ToolManager> *ToolManager::defineBindings()
{
  static lbt::Struct<ToolManager> *st = nullptr;
  if (!st) {
    litestl::alloc::PermanentGuard guard;
    st = new lbt::Struct<ToolManager>("coredcc::ToolManager", sizeof(ToolManager));
  }
  return st;
}

void ToolManager::setStatus(std::string_view text)
{
  status_ = litestl::util::string(std::string(text).c_str());
}

void ToolManager::setOutcome(std::string_view text)
{
  outcome_ = litestl::util::string(std::string(text).c_str());
}

Status ToolManager::enter(Context &ctx, const char *what)
{
  CDCC_TRY(rt_.busyIfEvaluating(what));
  if (inCall_) {
    return Status::error(
        StatusCode::Busy,
        (std::string(what) + ": not from one of the ToolManager's own slots").c_str());
  }
  if (&ctx.runtime() != &rt_) {
    return Status::error(StatusCode::FailedPrecondition,
                         (std::string(what) + ": a Context of another Runtime").c_str());
  }
  if (rt_.hasStickyError()) {
    const Status pending = rt_.takeStickyError();
    return Status::error(StatusCode::FailedPrecondition,
                         (std::string(what) +
                          ": an error was pending from elsewhere: " + pending.message())
                             .c_str());
  }
  return Status();
}

Status ToolManager::slotError()
{
  return rt_.hasStickyError() ? rt_.takeStickyError() : Status();
}

bool ToolManager::valid(Context &ctx, const ModalEntry &m) const
{
  const CommandExecutor *ex = ctx.executor();
  return !m.owns || (ex && ex == m.executor && ex->interactiveOpen() &&
                     ex->interactiveSerial() == m.serial);
}

bool ToolManager::owned(const CommandExecutor *ex, uint64_t serial) const
{
  for (const ModalEntry *m : modals_) {
    if (m->owns && m->executor == ex && m->serial == serial) {
      return true;
    }
  }
  return false;
}

Status ToolManager::finish(Context &ctx, int i, bool commit)
{
  ModalEntry *m = modals_[i];
  modals_.remove_at(i);
  Status s;
  bool committed = false;
  if (m->owns && valid(ctx, *m)) {
    CommandExecutor *ex = ctx.executor();
    s = commit ? ex->commit(ctx) : ex->cancel(ctx);
    committed = commit && s.ok();
  }
  if (m->modal->hasEnd()) {
    m->modal->end(ctx, committed);
    s = first(s, slotError());
  }
  litestl::alloc::Delete(m);
  if (modals_.size() == 0) {
    activeGizmo_ = activePart_ = -1;
    status_ = outcome_; /* the gesture's text goes */
    outcome_ = litestl::util::string();
  }
  return s;
}

Status ToolManager::cancelFrom(Context &ctx, int i)
{
  Status s;
  while (int(modals_.size()) > i) {
    s = first(s, finish(ctx, int(modals_.size()) - 1, false));
  }
  return s;
}

/* Only the stale ones go: a modal that ended its command and pushed another for the
 * next one stays. */
Status ToolManager::prune(Context &ctx)
{
  Status s;
  for (int i = int(modals_.size()) - 1; i >= 0; i--) {
    if (i < int(modals_.size()) && !valid(ctx, *modals_[i])) {
      s = first(s, finish(ctx, i, false));
    }
  }
  return s;
}

Status ToolManager::orphan(Context &ctx, uint64_t before)
{
  CommandExecutor *ex = ctx.executor();
  if (!ex || !ex->interactiveOpen() || ex->interactiveSerial() == before ||
      owned(ex, ex->interactiveSerial()))
  {
    return Status();
  }
  const Status c = ex->cancel(ctx);
  return first(Status::error(StatusCode::FailedPrecondition,
                             "a tool's slot began an interactive command and pushed no "
                             "modal that owns it, so it was cancelled"),
               c);
}

Status ToolManager::checkSettings(Context &ctx)
{
  if (!def_ || !tool_) {
    return Status();
  }
  const uint64_t v = rt_.ext<ToolSettingsStore>().version(*def_);
  if (v == settingsVersion_) {
    return Status();
  }
  settingsVersion_ = v;
  if (!tool_->tool->hasSettingsChanged()) {
    return Status();
  }
  tool_->tool->settingsChanged(ctx, *this);
  return slotError();
}

Status ToolManager::updateGizmos(Context &ctx)
{
  Status s;
  for (int i = 0; i < int(gizmos_.size()); i++) {
    if (!gizmos_[i]->removed) {
      gizmos_[i]->gizmo->update(ctx);
      s = first(s, slotError());
    }
  }
  return s;
}

int ToolManager::bestHit(
    const ViewMath &view, double x, double y, int32_t &part, Status &err)
{
  int best = -1;
  GizmoHit found;
  for (int i = 0; i < int(gizmos_.size()); i++) {
    if (gizmos_[i]->removed) {
      continue;
    }
    const GizmoHit h = gizmos_[i]->gizmo->hitTest(view, x, y);
    if (rt_.hasStickyError()) {
      err = first(err, rt_.takeStickyError()); /* a gizmo that throws can't be hit */
      continue;
    }
    if (h.part < 0 || !(h.distance <= hitRadius)) {
      continue;
    }
    if (best < 0 || h.distance < found.distance ||
        (h.distance == found.distance && h.depth < found.depth))
    {
      best = i;
      found = h;
    }
  }
  part = best >= 0 ? found.part : -1;
  return best;
}

void ToolManager::flushRemoved()
{
  for (int i = int(gizmos_.size()) - 1; i >= 0; i--) {
    if (gizmos_[i]->removed) {
      litestl::alloc::Delete(gizmos_[i]);
      gizmos_.remove_at(i);
    }
  }
}

/* ---- tools ---- */

void ToolManager::deactivate(Context &ctx)
{
  if (tool_ && tool_->tool->hasDeactivate()) {
    tool_->tool->deactivate(ctx, *this);
    slotError().ignore(); /* the tool goes anyway */
  }
  clearGizmos();
  if (tool_) {
    litestl::alloc::Delete(tool_);
  }
  tool_ = nullptr;
  def_ = nullptr;
  hoverGizmo_ = hoverPart_ = -1;
}

Status ToolManager::setActive(Context &ctx, std::string_view toolId)
{
  CDCC_TRY(enter(ctx, "setActive"));
  if (modalOpen()) {
    return Status::error(StatusCode::Busy, "setActive: a gesture is in progress");
  }
  const ToolDef *def = nullptr;
  if (!toolId.empty()) {
    def = rt_.ext<ToolRegistry>().find(toolId);
    if (!def) {
      return rt_.ext<ToolRegistry>().unknown(toolId);
    }
  }
  Call call(*this, ctx);
  deactivate(ctx);
  if (!def) {
    return Status();
  }
  Result<Owned<Tool>> made = def->instantiate(rt_);
  CDCC_TRY(first(made.status(), slotError()));
  tool_ = litestl::alloc::New<ToolSlot>("coredcc::ToolManager::ToolSlot");
  tool_->tool = std::move(made).value();
  def_ = def;
  settingsVersion_ = rt_.ext<ToolSettingsStore>().version(*def);
  const uint64_t before = ctx.executor() ? ctx.executor()->interactiveSerial() : 0;
  Status s = tool_->tool->activate(ctx, *this);
  s = first(s, slotError());
  s = first(s, orphan(ctx, before));
  if (!s.ok()) {
    cancelFrom(ctx, 0).ignore();
    deactivate(ctx);
  }
  return s;
}

Status ToolManager::pushModal(Context &ctx, Owned<Modal> modal, bool ownsInteractive)
{
  if (!modal) {
    return Status::error(StatusCode::InvalidArgument, "pushModal: no modal");
  }
  uint64_t serial = 0;
  const CommandExecutor *ex = ctx.executor();
  if (ownsInteractive) {
    if (!ex || !ex->interactiveOpen()) {
      return Status::error(StatusCode::FailedPrecondition,
                           "pushModal: it owns no command: none is open");
    }
    serial = ex->interactiveSerial();
    if (owned(ex, serial)) {
      return Status::error(StatusCode::FailedPrecondition,
                           "pushModal: another modal owns the open command");
    }
  }
  ModalEntry *m = litestl::alloc::New<ModalEntry>("coredcc::ToolManager::ModalEntry");
  m->modal = std::move(modal);
  m->owns = ownsInteractive;
  m->executor = ownsInteractive ? ex : nullptr;
  m->serial = serial;
  if (event_ && event_->isPointer()) {
    m->boundPointer = true;
    m->pointerId = event_->pointerId;
  }
  modals_.append(m);
  return Status();
}

int32_t ToolManager::addGizmo(Owned<Gizmo> gizmo)
{
  if (!gizmo) {
    return -1;
  }
  if (int32_t(gizmos_.size()) >= kMaxGizmos) {
    return -1;
  }
  /* The smallest free id: gizmos_ is kept sorted by id. */
  int at = 0;
  int32_t id = 1;
  while (at < int(gizmos_.size()) && gizmos_[at]->id == id) {
    at++;
    id++;
  }
  GizmoEntry *g = litestl::alloc::New<GizmoEntry>("coredcc::ToolManager::GizmoEntry");
  g->id = id;
  g->gizmo = std::move(gizmo);
  gizmos_.append(g);
  for (int i = int(gizmos_.size()) - 1; i > at; i--) {
    gizmos_[i] = gizmos_[i - 1];
  }
  gizmos_[at] = g;
  return g->id;
}

void ToolManager::removeGizmo(int32_t id)
{
  for (GizmoEntry *g : gizmos_) {
    if (g->id == id) {
      g->removed = true;
    }
  }
  if (activeGizmo_ == id) {
    activeGizmo_ = activePart_ = -1;
  }
  if (hoverGizmo_ == id) {
    hoverGizmo_ = hoverPart_ = -1;
  }
  if (!inCall_) {
    flushRemoved();
  }
}

void ToolManager::clearGizmos()
{
  /* Not by removeGizmo: outside a call, it erases from the vector being walked. */
  for (GizmoEntry *g : gizmos_) {
    g->removed = true;
  }
  activeGizmo_ = activePart_ = hoverGizmo_ = hoverPart_ = -1;
  if (!inCall_) {
    flushRemoved();
  }
}

int32_t ToolManager::gizmoCount() const
{
  int32_t n = 0;
  for (const GizmoEntry *g : gizmos_) {
    n += g->removed ? 0 : 1;
  }
  return n;
}

/* ---- events ---- */

Result<EventResult>
ToolManager::toModal(Context &ctx, const ViewMath &view, const InputEvent &e)
{
  const int top = int(modals_.size()) - 1;
  ModalEntry *m = modals_[top];
  if (e.isPointer() && m->boundPointer && e.pointerId != m->pointerId) {
    return EventResult::Unhandled; /* another finger or pen: the host's */
  }
  const CommandExecutor *ex = ctx.executor();
  const uint64_t before = ex ? ex->interactiveSerial() : 0;
  ModalResult r = m->modal->onEvent(ctx, *this, view, e);
  if (Status err = slotError(); !err.ok()) {
    return first(err, cancelFrom(ctx, top));
  }
  /* The slot may have ended its own command, or begun another. */
  bool stillThere = false;
  CDCC_TRY(prune(ctx));
  for (const ModalEntry *x : modals_) {
    stillThere = stillThere || x == m;
  }
  if (!stillThere) {
    CDCC_TRY(orphan(ctx, before));
    return EventResult::Handled;
  }
  if (e.type == EventType::Cancel || e.type == EventType::FocusLost) {
    r = ModalResult::Cancel;
  }
  if (r == ModalResult::Finish || r == ModalResult::Cancel) {
    if (int(modals_.size()) - 1 > top) {
      const Status c = cancelFrom(ctx, top);
      return first(Status::error(StatusCode::InvalidArgument,
                                 "a modal finished with modals above it, so all of them "
                                 "were cancelled"),
                   c);
    }
    CDCC_TRY(finish(ctx, top, r == ModalResult::Finish));
    return EventResult::Handled;
  }
  CDCC_TRY(orphan(ctx, before));
  return r == ModalResult::PassThrough ? EventResult::Unhandled : EventResult::Handled;
}

Result<EventResult> ToolManager::dispatch(Context &ctx,
                                          const View &view,
                                          const InputEvent &e,
                                          Status *lateOut)
{
  if (lateOut) {
    *lateOut = Status();
  }
  CDCC_TRY(enter(ctx, "dispatch"));
  if (!e.valid()) {
    return Status::error(StatusCode::InvalidArgument,
                         "dispatch: the event's type or pointer isn't one");
  }
  ViewMath vm;
  CDCC_TRY(vm.set(view));
  Call call(*this, ctx);
  event_ = &e;
  CDCC_TRY(prune(ctx));
  const CommandExecutor *ex = ctx.executor();
  uint64_t before = ex ? ex->interactiveSerial() : 0;
  if (Status s = checkSettings(ctx); !s.ok()) {
    return first(s, first(cancelFrom(ctx, 0), orphan(ctx, before)));
  }
  CDCC_TRY(orphan(ctx, before));
  CDCC_TRY(prune(ctx));
  before = ex ? ex->interactiveSerial() : 0;
  if (modalOpen()) {
    return toModal(ctx, vm, e);
  }
  const bool press = e.type == EventType::PointerDown && e.button == 0;
  /* Errors of update and hitTest are given once the event is dispatched. */
  Status late;
  const auto deliver = [&](EventResult r) -> Result<EventResult> {
    if (late.ok()) {
      return r;
    }
    if (!lateOut) {
      return late;
    }
    *lateOut = late;
    return r;
  };
  if (press || e.type == EventType::PointerMove) {
    late = updateGizmos(ctx);
    int32_t part = -1;
    const int g = bestHit(vm, e.x, e.y, part, late);
    hoverGizmo_ = g >= 0 ? gizmos_[g]->id : -1;
    hoverPart_ = part;
    if (press && g >= 0) {
      GizmoEntry *entry = gizmos_[g];
      Status s = entry->gizmo->beginDrag(ctx, *this, vm, e, part);
      s = first(s, slotError());
      if (!s.ok()) {
        return first(s, first(cancelFrom(ctx, 0), orphan(ctx, before)));
      }
      CDCC_TRY(orphan(ctx, before));
      if (modalOpen()) {
        activeGizmo_ = entry->id;
        activePart_ = part;
      }
      return deliver(EventResult::Handled);
    }
  }
  if (!tool_) {
    return deliver(EventResult::Unhandled);
  }
  const EventResult r = tool_->tool->onEvent(ctx, *this, vm, e);
  if (Status err = slotError(); !err.ok()) {
    return first(err, first(cancelFrom(ctx, 0), orphan(ctx, before)));
  }
  CDCC_TRY(orphan(ctx, before));
  return deliver(r == EventResult::Handled ? EventResult::Handled
                                           : EventResult::Unhandled);
}

Status ToolManager::draw(Context &ctx, const View &view, DrawList &out)
{
  CDCC_TRY(enter(ctx, "draw"));
  ViewMath vm;
  CDCC_TRY(vm.set(view));
  Call call(*this, ctx);
  Status s = out.begin(drawOrigin ? *drawOrigin : eyeOf(view));
  s = first(s, drawSceneOverlays(ctx, ctx.scene(), vm, out));
  s = first(s, prune(ctx));
  if (!modalOpen()) {
    /* During a gesture the modal draws what moves: placing a gizmo can be O(selection).
     */
    s = first(s, updateGizmos(ctx));
  }
  if (tool_ && tool_->tool->hasDrawOverlay()) {
    DrawSlotScope slot(out);
    tool_->tool->drawOverlay(vm, out);
    s = first(s, first(slotError(), slot.close("the tool's drawOverlay")));
  }
  for (const GizmoEntry *g : gizmos_) {
    if (g->removed) {
      continue;
    }
    const bool active = g->id == activeGizmo_;
    DrawSlotScope slot(out);
    g->gizmo->draw(vm,
                   g->id == hoverGizmo_ && !modalOpen() ? hoverPart_ : -1,
                   active ? activePart_ : -1,
                   uint32_t(g->id) * kIdStride,
                   out);
    s = first(s, first(slotError(), slot.close("a gizmo's draw")));
  }
  for (const ModalEntry *m : modals_) {
    if (m->modal->hasDrawOverlay()) {
      DrawSlotScope slot(out);
      m->modal->drawOverlay(vm, out);
      s = first(s, first(slotError(), slot.close("a modal's drawOverlay")));
    }
  }
  return first(s, out.end());
}

Status ToolManager::cancel(Context &ctx)
{
  CDCC_TRY(enter(ctx, "cancel"));
  Call call(*this, ctx);
  return cancelFrom(ctx, 0);
}

void ToolManager::shutdown(Context &ctx)
{
  if (inCall_) {
    return;
  }
  Call call(*this, ctx);
  cancelFrom(ctx, 0).ignore();
  deactivate(ctx);
  slotError().ignore();
}

} // namespace coredcc
