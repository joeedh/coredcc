#include "coredcc/bind/tool_facades.h"

#include "coredcc/bind/checked.h"
#include "coredcc/bind/detail.h"
#include "coredcc/bind/foreign_command.h"
#include "coredcc/command/executor.h"
#include "coredcc/draw/overlay.h"
#include "coredcc/scene/commands.h"
#include "coredcc/tool/builtins.h"
#include "coredcc/tool/elements.h"
#include "coredcc/tool/gizmos/transform_gizmo.h"
#include "coredcc/tool/manager.h"
#include "coredcc/tool/stroke.h"

#include "litestl/binding/binding.h"
#include "litestl/binding/foreign.h"
#include "litestl/binding/manager.h"
#include "litestl/util/alloc.h"

#include <algorithm>
#include <span>
#include <string>
#include <vector>

namespace coredcc::script {

namespace lf = litestl::binding::foreign;
using detail::error;
using detail::failed;
using detail::Gate;
using detail::ok;
using detail::slotGate;
using detail::splitSlots;
using detail::view;
using litestl::util::Vector;

namespace {

void *selfOf(lf::Handle h)
{
  return reinterpret_cast<void *>(uintptr_t(h));
}

lf::Handle handleOf(void *self)
{
  return lf::Handle(reinterpret_cast<uintptr_t>(self));
}

Result<ToolManager *> managerOf(Context &ctx, const char *what)
{
  if (!ctx.tools()) {
    return error(StatusCode::FailedPrecondition,
                 std::string(what) + ": the Context has no ToolManager");
  }
  return ctx.tools();
}

/* Owns a script object as interface I: releases its handle and counts it. */
template <class I> Owned<I> ownForeign(Runtime &rt, I ref, lf::Handle h)
{
  ForeignBindings &fb = rt.ext<ForeignBindings>();
  fb.instanceMade();
  (void)h;
  return Owned<I>(
      ref,
      [](void *self, void *ctx) {
        static_cast<ForeignBindings *>(ctx)->instanceGone();
        lf::release(handleOf(self));
      },
      &fb);
}

/* The table of script objects of I with `slots`, registered on first use. */
template <class I>
Result<TypeId> foreignTable(Runtime &rt, const char *kind, const string &slots)
{
  std::vector<std::string> names = splitSlots(slots);
  std::sort(names.begin(), names.end());
  std::string impl = std::string("script.") + kind + "(";
  for (size_t i = 0; i < names.size(); i++) {
    impl += (i ? " " : "") + names[i];
  }
  impl += ")";
  const TypeId type = TypeId::of(impl);
  if (!rt.ifaces().find(type, InterfaceId::of(I::ifaceName))) {
    CDCC_TRY(foreignImplementable(coredcc::detail::ifaceDesc<I>()));
    std::vector<const char *> present;
    for (const std::string &n : names) {
      present.push_back(n.c_str());
    }
    CDCC_TRY(
        registerForeign<I>(rt,
                           impl,
                           rt.ext<ForeignBindings>().dispatcher(),
                           nullptr,
                           std::span<const char *const>(present.data(), present.size())));
  }
  return type;
}

/* A script object of I, from a handle that carries a count. */
template <class I>
Result<Owned<I>> adopt(Context &ctx, const char *kind, lf::Handle h, const string &slots)
{
  if (h == 0) {
    return error(StatusCode::InvalidArgument, std::string("no ") + kind + " handle");
  }
  Runtime &rt = ctx.runtime();
  Result<TypeId> type = foreignTable<I>(rt, kind, slots);
  if (!type.ok()) {
    lf::release(h);
    return type.status();
  }
  I ref = query<I>(rt, *type, selfOf(h));
  if (!ref) {
    lf::release(h);
    return error(StatusCode::Internal,
                 std::string("the script ") + kind + "'s table is gone");
  }
  return ownForeign<I>(rt, ref, h);
}

/* A script factory's instance of `type`, owned. `what` is "tool" or "stroke brush". */
template <class I>
Result<Owned<I>> makeInstance(Runtime &rt, TypeId type, lf::Handle factory, const char *what)
{
  const lbt::Method *make = ForeignFactory::defineBindings()->findMethod("make");
  lf::Handle h = 0;
  Status s = rt.ext<ForeignBindings>().call(
      factory, ForeignFactory::defineBindings(), make, nullptr, &h);
  if (!s.ok()) {
    return error(s.code(), std::string("the ") + what + " factory failed: " + s.message());
  }
  if (h == 0) {
    return error(StatusCode::Internal,
                 std::string("the ") + what + " factory's make returned no instance");
  }
  I t = query<I>(rt, type, selfOf(h));
  if (!t) {
    lf::release(h);
    return error(StatusCode::NotFound, std::string("the script ") + what + "'s table is gone");
  }
  return ownForeign<I>(rt, t, h);
}

Result<Owned<Tool>> makeToolInstance(Runtime &rt, TypeId type, lf::Handle factory)
{
  return makeInstance<Tool>(rt, type, factory, "tool");
}

} // namespace

/* ---- declaring tools ---- */

ToolDef *newToolDef(const string &id)
{
  return litestl::alloc::New<ToolDef>("coredcc::ToolDef", view(id));
}

Status toolDefUiName(ToolDef &def, const string &s)
{
  def.uiName(view(s));
  return Status();
}

Status toolDefDescription(ToolDef &def, const string &s)
{
  def.description(view(s));
  return Status();
}

Status toolDefSetting(ToolDef &def, const string &name, const PropValue &defaultValue)
{
  Result<PropertyDef> pd = detail::scriptProp(
      "toolDefSetting", name, defaultValue, def.findSetting(view(name)));
  CDCC_TRY(pd.status());
  def.setting(std::move(pd).value());
  return Status();
}

Status toolDefEnumSetting(ToolDef &def, const string &name, const string &keys)
{
  if (def.findSetting(view(name))) {
    return error(StatusCode::AlreadyExists,
                 "toolDefEnumSetting: '" + std::string(view(name)) + "' is taken");
  }
  PropertyDef d = PropertyDef::enumProp(view(name));
  int64_t v = 0;
  for (const std::string &k : splitSlots(keys)) {
    d.item(k, v++);
  }
  if (v == 0) {
    return error(StatusCode::InvalidArgument, "toolDefEnumSetting: no keys");
  }
  def.setting(std::move(d));
  return Status();
}

Status toolDefLayout(ToolDef &def, const string &xml)
{
  def.layout(view(xml));
  return Status();
}

Status registerForeignTool(Context &ctx,
                           const ToolDef &def,
                           uint32_t factory,
                           const string &slots)
{
  if (factory == 0) {
    return error(StatusCode::InvalidArgument, "registerForeignTool: no factory handle");
  }
  Runtime &rt = ctx.runtime();
  auto attempt = [&]() -> Status {
    CDCC_TRY(slotGate(ctx, "registerForeignTool", {}));
    const std::string id = def.name().c_str();
    if (id.rfind("core.", 0) == 0) {
      return error(StatusCode::InvalidArgument,
                   "tool '" + id + "': the 'core.' namespace is reserved for coredcc");
    }
    CDCC_TRY(foreignImplementable(coredcc::detail::ifaceDesc<Tool>()));
    std::vector<const char *> present;
    const std::vector<std::string> names = splitSlots(slots);
    for (const std::string &n : names) {
      present.push_back(n.c_str());
    }
    const std::string impl = id + "#impl";
    ToolDef copy = def;
    copy.impl(impl, [](Runtime &) -> Result<Owned<Tool>> {
      return Status::error(StatusCode::Internal, "unset");
    });
    CDCC_TRY(rt.ext<ToolRegistry>().validateDef(copy));
    CDCC_TRY(registerForeign<Tool>(
        rt,
        impl,
        rt.ext<ForeignBindings>().dispatcher(),
        nullptr,
        std::span<const char *const>(present.data(), present.size())));
    const TypeId type = TypeId::of(impl);
    copy.impl(impl,
              [type, factory](Runtime &r) { return makeToolInstance(r, type, factory); });
    return rt.ext<ToolRegistry>().add(std::move(copy)).status();
  };
  Status s = attempt();
  if (s.ok()) {
    rt.ext<ForeignBindings>().keepHandle(factory);
  } else {
    lf::release(factory);
  }
  return s;
}

Status registerForeignStrokeBrush(Context &ctx,
                                  const string &name,
                                  uint32_t factory,
                                  const string &slots)
{
  if (factory == 0) {
    return error(StatusCode::InvalidArgument,
                 "registerForeignStrokeBrush: no factory handle");
  }
  Runtime &rt = ctx.runtime();
  auto attempt = [&]() -> Status {
    CDCC_TRY(slotGate(ctx, "registerForeignStrokeBrush", {}));
    const std::string id = name.c_str();
    if (id.rfind("core.", 0) == 0) {
      return error(StatusCode::InvalidArgument,
                   "stroke brush '" + id + "': the 'core.' namespace is reserved for coredcc");
    }
    if (rt.ext<StrokeBrushRegistry>().has(id)) {
      return error(StatusCode::AlreadyExists, "stroke brush '" + id + "' exists");
    }
    CDCC_TRY(foreignImplementable(coredcc::detail::ifaceDesc<StrokeBrush>()));
    std::vector<const char *> present;
    const std::vector<std::string> names = splitSlots(slots);
    for (const std::string &n : names) {
      present.push_back(n.c_str());
    }
    const std::string impl = id + "#brush";
    CDCC_TRY(registerForeign<StrokeBrush>(
        rt,
        impl,
        rt.ext<ForeignBindings>().dispatcher(),
        nullptr,
        std::span<const char *const>(present.data(), present.size())));
    const TypeId type = TypeId::of(impl);
    return rt.ext<StrokeBrushRegistry>().add(id, [type, factory](Runtime &r) {
      return makeInstance<StrokeBrush>(r, type, factory, "stroke brush");
    });
  };
  Status s = attempt();
  if (s.ok()) {
    rt.ext<ForeignBindings>().keepHandle(factory);
  } else {
    lf::release(factory);
  }
  return s;
}

/* ---- the Context's ToolManager ---- */

Status toolSetActive(Context &ctx, const string &id)
{
  CDCC_TRY(slotGate(ctx, "toolSetActive", {}));
  Result<ToolManager *> tm = managerOf(ctx, "toolSetActive");
  CDCC_TRY(tm.status());
  return (*tm)->setActive(ctx, view(id));
}

IntResult toolEvent(Context &ctx, const View &v, const InputEvent &e)
{
  if (Status s = slotGate(ctx, "toolEvent", {}); !s.ok()) {
    return failed<IntResult>(std::move(s));
  }
  Result<ToolManager *> tm = managerOf(ctx, "toolEvent");
  if (!tm.ok()) {
    return failed<IntResult>(tm.status());
  }
  Status late;
  Result<EventResult> r = (*tm)->dispatch(ctx, v, e, &late);
  if (!r.ok()) {
    return failed<IntResult>(r.status());
  }
  IntResult out = ok<IntResult>(int64_t(*r));
  out.status = late;
  return out;
}

Status toolDraw(Context &ctx, const View &v, DrawList &out)
{
  CDCC_TRY(slotGate(ctx, "toolDraw", {}));
  Result<ToolManager *> tm = managerOf(ctx, "toolDraw");
  CDCC_TRY(tm.status());
  return (*tm)->draw(ctx, v, out);
}

Status toolCancel(Context &ctx)
{
  CDCC_TRY(slotGate(ctx, "toolCancel", {}));
  Result<ToolManager *> tm = managerOf(ctx, "toolCancel");
  CDCC_TRY(tm.status());
  return (*tm)->cancel(ctx);
}

StringResult toolStatus(Context &ctx)
{
  if (Status s = slotGate(ctx, "toolStatus", {}, Gate::Read); !s.ok()) {
    return failed<StringResult>(std::move(s));
  }
  Result<ToolManager *> tm = managerOf(ctx, "toolStatus");
  return tm.ok() ? ok<StringResult>((*tm)->status()) : failed<StringResult>(tm.status());
}

Status toolSetStatus(Context &ctx, const string &text)
{
  CDCC_TRY(slotGate(ctx, "toolSetStatus", {}));
  Result<ToolManager *> tm = managerOf(ctx, "toolSetStatus");
  CDCC_TRY(tm.status());
  (*tm)->setStatus(view(text));
  return Status();
}

Status toolSetOutcome(Context &ctx, const string &text)
{
  CDCC_TRY(slotGate(ctx, "toolSetOutcome", {}));
  Result<ToolManager *> tm = managerOf(ctx, "toolSetOutcome");
  CDCC_TRY(tm.status());
  (*tm)->setOutcome(view(text));
  return Status();
}

StringResult toolActive(Context &ctx)
{
  if (Status s = slotGate(ctx, "toolActive", {}, Gate::Read); !s.ok()) {
    return failed<StringResult>(std::move(s));
  }
  Result<ToolManager *> tm = managerOf(ctx, "toolActive");
  if (!tm.ok()) {
    return failed<StringResult>(tm.status());
  }
  const ToolDef *def = (*tm)->active();
  return ok<StringResult>(def ? def->name() : string(""));
}

/* ---- from a tool's slots ---- */

Status
toolPushModal(Context &ctx, uint32_t modal, const string &slots, bool ownsInteractive)
{
  Status gate = slotGate(ctx, "toolPushModal", {});
  Result<ToolManager *> tm = managerOf(ctx, "toolPushModal");
  if (!gate.ok() || !tm.ok()) {
    if (modal != 0) {
      lf::release(modal);
    }
    return gate.ok() ? tm.status() : gate;
  }
  Result<Owned<Modal>> m = adopt<Modal>(ctx, "modal", modal, slots);
  CDCC_TRY(m.status());
  return (*tm)->pushModal(ctx, std::move(m).value(), ownsInteractive);
}

IntResult toolAddGizmo(Context &ctx, const string &kind, const string &settingsOf)
{
  if (Status s = slotGate(ctx, "toolAddGizmo", {}); !s.ok()) {
    return failed<IntResult>(std::move(s));
  }
  Result<ToolManager *> tm = managerOf(ctx, "toolAddGizmo");
  if (!tm.ok()) {
    return failed<IntResult>(tm.status());
  }
  if (view(kind) != "transform") {
    return failed<IntResult>(error(StatusCode::NotFound,
                                   "toolAddGizmo: no built-in gizmo '" +
                                       std::string(view(kind)) +
                                       "' (there is 'transform')"));
  }
  ToolRegistry &reg = ctx.runtime().ext<ToolRegistry>();
  const std::string_view of = view(settingsOf).empty() ? std::string_view(kTransformTool)
                                                       : view(settingsOf);
  const ToolDef *def = reg.find(of);
  if (!def) {
    Status s = view(settingsOf).empty()
                   ? error(StatusCode::FailedPrecondition,
                           "toolAddGizmo: the built-in tools aren't registered")
                   : reg.unknown(of);
    return failed<IntResult>(std::move(s));
  }
  auto *g = litestl::alloc::New<gizmo::TransformGizmo>("coredcc::gizmo::TransformGizmo");
  g->tool = def;
  Result<Owned<Gizmo>> owned = own<Gizmo>(ctx.runtime(), gizmo::kTransformGizmoImpl, g);
  if (!owned.ok()) {
    return failed<IntResult>(owned.status());
  }
  return ok<IntResult>(int64_t((*tm)->addGizmo(std::move(owned).value())));
}

IntResult toolAddForeignGizmo(Context &ctx, uint32_t gizmo, const string &slots)
{
  Status gate = slotGate(ctx, "toolAddForeignGizmo", {});
  Result<ToolManager *> tm = managerOf(ctx, "toolAddForeignGizmo");
  if (!gate.ok() || !tm.ok()) {
    if (gizmo != 0) {
      lf::release(gizmo);
    }
    return failed<IntResult>(gate.ok() ? tm.status() : gate);
  }
  Result<Owned<Gizmo>> g = adopt<Gizmo>(ctx, "gizmo", gizmo, slots);
  if (!g.ok()) {
    return failed<IntResult>(g.status());
  }
  return ok<IntResult>(int64_t((*tm)->addGizmo(std::move(g).value())));
}

Status toolRemoveGizmo(Context &ctx, int32_t id)
{
  /* A read: deactivate and end may remove gizmos. */
  CDCC_TRY(slotGate(ctx, "toolRemoveGizmo", {}, Gate::Read));
  Result<ToolManager *> tm = managerOf(ctx, "toolRemoveGizmo");
  CDCC_TRY(tm.status());
  (*tm)->removeGizmo(id);
  return Status();
}

/* ---- drags ---- */

namespace {

template <class Cmd, class Fn> Status updateOpen(Context &ctx, const char *what, Fn &&fn)
{
  CDCC_TRY(slotGate(ctx, what, {}));
  CommandExecutor *ex = ctx.executor();
  if (!ex || !ex->interactiveOpen()) {
    return error(StatusCode::FailedPrecondition, std::string(what) + ": no open drag");
  }
  return ex->updateInteractive(ctx, [&](Context &c, Command &cmd) -> Status {
    Cmd *d = cmd.as<Cmd>();
    if (!d) {
      return error(StatusCode::FailedPrecondition,
                   std::string(what) + ": the open command is another (" +
                       ex->interactiveDef()->name().c_str() + ")");
    }
    return fn(c, *d);
  });
}

} // namespace

Status dragUpdate(Context &ctx, const TransformDelta &delta)
{
  return updateOpen<ApplyDelta>(
      ctx, "dragUpdate", [&](Context &c, ApplyDelta &d) { return d.update(c, delta); });
}

Status dragUpdateEach(Context &ctx, const Vector<double> &numbers)
{
  return updateOpen<ApplyDelta>(ctx, "dragUpdateEach", [&](Context &c, ApplyDelta &d) {
    constexpr int kEach = 16;
    if (int(numbers.size()) != kEach * d.count()) {
      return error(StatusCode::InvalidArgument,
                   "dragUpdateEach: " + std::to_string(kEach) + " numbers for each of " +
                       std::to_string(d.count()) + " objects, not " +
                       std::to_string(numbers.size()));
    }
    Vector<TransformDelta> deltas;
    for (int i = 0; i < d.count(); i++) {
      const double *n = numbers.data() + kEach * i;
      if (!(n[0] == 0.0 || n[0] == 1.0 || n[0] == 2.0)) {
        return error(StatusCode::InvalidArgument,
                     "dragUpdateEach: delta " + std::to_string(i) +
                         ": a kind of 0 (translate), 1 (rotate) or 2 (scale), not " +
                         std::to_string(n[0]));
      }
      TransformDelta t;
      t.kind = DeltaKind(int32_t(n[0]));
      t.basis = quatd(n[1], n[2], n[3], n[4]);
      t.pivot = double3(n[5], n[6], n[7]);
      t.vector = double3(n[8], n[9], n[10]);
      t.axis = double3(n[11], n[12], n[13]);
      t.angle = n[14];
      deltas.append(t);
    }
    return d.update(
        c, std::span<const TransformDelta>(deltas.data(), size_t(deltas.size())));
  });
}

Status elementsDragUpdate(Context &ctx, const TransformDelta &delta)
{
  return updateOpen<ElementsDelta>(
      ctx, "elementsDragUpdate", [&](Context &c, ElementsDelta &d) {
        return d.update(c, delta);
      });
}

IntResult dragCount(Context &ctx)
{
  if (Status s = slotGate(ctx, "dragCount", {}, Gate::Read); !s.ok()) {
    return failed<IntResult>(std::move(s));
  }
  CommandExecutor *ex = ctx.executor();
  Command *cmd = ex ? ex->interactiveCommand() : nullptr;
  const ApplyDelta *a = cmd ? cmd->as<ApplyDelta>() : nullptr;
  return ok<IntResult>(int64_t(a ? a->count() : 0));
}

Status drawListRead(const DrawList &list, int64_t offset, std::span<uint8_t> out)
{
  return list.read(offset, out);
}

/* ---- bindings ---- */

void addToolBindings(litestl::binding::BindingManager *mgr)
{
  using litestl::binding::Bind;
  litestl::alloc::PermanentGuard guard;
  mgr->add(Bind<PointerSample>());
  mgr->add(Bind<EventType>());
  mgr->add(Bind<PointerType>());
  mgr->add(Bind<EventResult>());
  mgr->add(Bind<ModalResult>());
  mgr->add(Bind<InputEvent>());
  mgr->add(Bind<DepthMode>());
  mgr->add(Bind<ShapeKind>());
  mgr->add(Bind<DrawStyle>());
  mgr->add(Bind<DrawList>());
  mgr->add(Bind<GizmoHit>());
  mgr->add(Bind<ElementSet>());
  mgr->add(Bind<ToolManager>());
  mgr->add(Bind<ToolDef>());
  mgr->add(Bind<Tool>());
  mgr->add(Bind<Modal>());
  mgr->add(Bind<Gizmo>());
  mgr->add(Bind<OverlayDrawable>());
  mgr->add(Bind<TransformableElements>());
  mgr->add(Bind<Symmetry>());
  mgr->add(Bind<StrokeSpace>());
  mgr->add(Bind<StrokeMethod>());
  mgr->add(Bind<StrokeSettings>());
  mgr->add(Bind<StrokeSample>());
  mgr->add(Bind<StrokeTarget>());
  mgr->add(Bind<StrokeBrush>());

  BIND_FUNCTION(mgr, "coredcc::newToolDef", script::newToolDef, MARGS("id"))
      .returnsOwned();
  BIND_FACADE(mgr, "coredcc::toolDefUiName", script::toolDefUiName, MARGS("def", "s"));
  BIND_FACADE(
      mgr, "coredcc::toolDefDescription", script::toolDefDescription, MARGS("def", "s"));
  BIND_FACADE(mgr,
              "coredcc::toolDefSetting",
              script::toolDefSetting,
              MARGS("def", "name", "defaultValue"));
  BIND_FACADE(mgr,
              "coredcc::toolDefEnumSetting",
              script::toolDefEnumSetting,
              MARGS("def", "name", "keys"));
  BIND_FACADE(mgr, "coredcc::toolDefLayout", script::toolDefLayout, MARGS("def", "xml"));
  BIND_OWNING_FACADE(mgr,
                     "coredcc::registerForeignTool",
                     script::registerForeignTool,
                     3,
                     MARGS("ctx", "def", "factory", "slots"));

  BIND_OWNING_FACADE(mgr,
                     "coredcc::registerForeignStrokeBrush",
                     script::registerForeignStrokeBrush,
                     3,
                     MARGS("ctx", "name", "factory", "slots"));

  BIND_FACADE(mgr, "coredcc::toolSetActive", script::toolSetActive, MARGS("ctx", "id"));
  BIND_FACADE(mgr, "coredcc::toolEvent", script::toolEvent, MARGS("ctx", "view", "e"));
  BIND_FACADE(mgr, "coredcc::toolDraw", script::toolDraw, MARGS("ctx", "view", "out"));
  BIND_FACADE(mgr, "coredcc::toolCancel", script::toolCancel, MARGS("ctx"));
  BIND_FACADE(mgr, "coredcc::toolStatus", script::toolStatus, MARGS("ctx"));
  BIND_FACADE(mgr, "coredcc::toolSetStatus", script::toolSetStatus, MARGS("ctx", "text"));
  BIND_FACADE(mgr, "coredcc::toolSetOutcome", script::toolSetOutcome, MARGS("ctx", "text"));
  BIND_FACADE(mgr, "coredcc::toolActive", script::toolActive, MARGS("ctx"));
  BIND_OWNING_FACADE(mgr,
                     "coredcc::toolPushModal",
                     script::toolPushModal,
                     2,
                     MARGS("ctx", "modal", "slots", "ownsInteractive"));
  BIND_FACADE(
      mgr, "coredcc::toolAddGizmo", script::toolAddGizmo, MARGS("ctx", "kind", "settingsOf"));
  BIND_OWNING_FACADE(mgr,
                     "coredcc::toolAddForeignGizmo",
                     script::toolAddForeignGizmo,
                     2,
                     MARGS("ctx", "gizmo", "slots"));
  BIND_FACADE(
      mgr, "coredcc::toolRemoveGizmo", script::toolRemoveGizmo, MARGS("ctx", "id"));

  BIND_FACADE(mgr, "coredcc::dragUpdate", script::dragUpdate, MARGS("ctx", "delta"));
  BIND_FACADE(
      mgr, "coredcc::dragUpdateEach", script::dragUpdateEach, MARGS("ctx", "numbers"));
  BIND_FACADE(mgr,
              "coredcc::elementsDragUpdate",
              script::elementsDragUpdate,
              MARGS("ctx", "delta"));
  BIND_FACADE(mgr, "coredcc::dragCount", script::dragCount, MARGS("ctx"));
  BIND_FACADE(
      mgr, "coredcc::drawListRead", script::drawListRead, MARGS("list", "offset", "out"));
}

} // namespace coredcc::script
