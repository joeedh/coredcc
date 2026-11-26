/* ToolRegistry, ToolSettingsStore and ToolManager: dispatch to modals, gizmos and the
 * tool, drags that are one undo step, cancels, undo during a drag, errors and Busy. */
#include "../command/command_fixture.h"

#include "coredcc/draw/drawlist.h"
#include "coredcc/tool/manager.h"
#include "coredcc/tool/settings.h"
#include "coredcc/tool/tool.h"

#include "litestl/math/transform.h"

#include <string>

test_init;

using litestl::math::lookAt;
using litestl::math::perspective;

namespace {

View testView()
{
  View v;
  v.viewport[2] = 800;
  v.viewport[3] = 600;
  v.view = lookAt(double3(1, 2, 3), double3(), double3(0, 0, 1));
  v.proj = perspective(1.0, 800.0 / 600.0, 0.1, 100.0);
  return v;
}

InputEvent event(EventType type, double x, double y, int32_t button = 0)
{
  InputEvent e;
  e.type = type;
  e.x = x;
  e.y = y;
  e.button = button;
  return e;
}

double strength(Context &ctx)
{
  return getValue(ctx, "tool.strength")->asFloat();
}

Status beginDragCommand(Context &ctx)
{
  const CommandDef *drag = ctx.runtime().ext<CommandRegistry>().find("test.drag");
  return ctx.executor()->beginInteractive(ctx, *drag, drag->newInputs());
}

/* What the test tools do, set by each test. */
struct Script {
  bool pushModal = true; /* the tool pushes a modal for the command it begins */
  bool reenter = false;  /* the tool calls the manager from its slot */
  bool stickyInModal = false;
  bool stickyInUpdate = false;
  bool handover = false;    /* 'n' in the modal commits and begins the next drag */
  bool outcome = false;     /* the modal's release sets an outcome text */
  bool popInDraw = false;   /* the modal's drawOverlay pops what it didn't push */
  ToolManager *seen = nullptr; /* ctx.tools() in the tool's onEvent */
  Status reentered;
};
Script g_script;

Status pushDragModal(Context &ctx, ToolManager &tm);

/* Sets tool.strength from x while the button is down; up finishes, Escape cancels. */
struct DragModal {
  ModalResult
  onEvent(Context &ctx, ToolManager &tm, const ViewMath &, const InputEvent &e)
  {
    g_log += "modal;";
    if (g_script.handover && e.type == EventType::KeyDown && e.key == 'n') {
      /* Ends its own drag and hands over to a new one, with a modal of its own. */
      ctx.executor()->commit(ctx).ignore();
      beginDragCommand(ctx).ignore();
      pushDragModal(ctx, tm).ignore();
      return ModalResult::Continue;
    }
    if (g_script.outcome && e.type == EventType::PointerUp) {
      tm.setStatus("dragging");
      tm.setOutcome("nothing to drag");
    }
    if (g_script.stickyInModal) {
      ctx.runtime().setStickyError(Status::error(StatusCode::Internal, "thrown"));
      return ModalResult::Continue;
    }
    if (e.type == EventType::KeyDown && e.key == keys::kEscape) {
      return ModalResult::Cancel;
    }
    if (e.type == EventType::Wheel) {
      return ModalResult::PassThrough;
    }
    if (e.type == EventType::PointerMove) {
      setValue(ctx, "tool.strength", PropValue::ofFloat(e.x / 1000.0)).ignore();
    }
    return e.type == EventType::PointerUp ? ModalResult::Finish : ModalResult::Continue;
  }
  void drawOverlay(const ViewMath &, DrawList &out) const
  {
    const double xy[4] = {0, 0, 10, 10};
    out.line2d(DrawStyle(), std::span<const double>(xy, 4)).ignore();
    if (g_script.popInDraw) {
      g_script.reentered = out.popMatrix(); /* refused: nothing of its own to pop */
      mat4d m;
      m.identity();
      out.pushMatrix(m).ignore(); /* and left: popped after it, and an error */
    }
  }
  void end(Context &, bool committed)
  {
    g_log += committed ? "end-commit;" : "end-cancel;";
  }
  static lbt::Struct<DragModal> *defineBindings()
  {
    auto *st = new lbt::Struct<DragModal>("test::DragModal", sizeof(DragModal));
    BIND_STRUCT_METHOD(st, onEvent, MARGS("ctx", "tm", "view", "e"));
    BIND_STRUCT_METHOD(st, drawOverlay, MARGS("view", "out"));
    BIND_STRUCT_METHOD(st, end, MARGS("ctx", "committed"));
    return st;
  }
};

Status pushDragModal(Context &ctx, ToolManager &tm)
{
  Result<Owned<Modal>> m = own<Modal>(
      ctx.runtime(), "test.modal", litestl::alloc::New<DragModal>("test::DragModal"));
  CDCC_TRY(m.status());
  return tm.pushModal(ctx, std::move(m).value(), true);
}

/* A gizmo with one part, a disc of 10 pixels about (100, 100). */
struct DiscGizmo {
  void update(Context &ctx)
  {
    g_log += "update;";
    if (g_script.stickyInUpdate) {
      ctx.runtime().setStickyError(Status::error(StatusCode::Internal, "update threw"));
    }
  }
  GizmoHit hitTest(const ViewMath &, double x, double y) const
  {
    GizmoHit h;
    const double d = std::sqrt((x - 100) * (x - 100) + (y - 100) * (y - 100));
    if (d < 16) {
      h.part = 3;
      h.distance = d < 10 ? 0.0 : d - 10;
    }
    return h;
  }
  void draw(const ViewMath &,
            int32_t hover,
            int32_t active,
            uint32_t idBase,
            DrawList &out) const
  {
    DrawStyle st;
    st.id = idBase + 3;
    st.color = active == 3 ? 0xff00ffffu : hover == 3 ? 0xffffffffu : 0xff0000ffu;
    out.circle2d(st, 100, 100, 10, true).ignore();
  }
  Status
  beginDrag(Context &ctx, ToolManager &tm, const ViewMath &, const InputEvent &, int32_t)
  {
    g_log += "gizmo-drag;";
    CDCC_TRY(beginDragCommand(ctx));
    return pushDragModal(ctx, tm);
  }
  static lbt::Struct<DiscGizmo> *defineBindings()
  {
    auto *st = new lbt::Struct<DiscGizmo>("test::DiscGizmo", sizeof(DiscGizmo));
    BIND_STRUCT_METHOD(st, update, MARGS("ctx"));
    BIND_STRUCT_METHOD(st, hitTest, MARGS("view", "x", "y"));
    BIND_STRUCT_METHOD(st, draw, MARGS("view", "hover", "active", "idBase", "out"));
    BIND_STRUCT_METHOD(st, beginDrag, MARGS("ctx", "tm", "view", "e", "part"));
    return st;
  }
};

/* Presses begin a drag of tool.strength; with a gizmo. */
struct DragTool {
  Status activate(Context &ctx, ToolManager &tm)
  {
    g_log += "activate;";
    Result<Owned<Gizmo>> g = own<Gizmo>(
        ctx.runtime(), "test.gizmo", litestl::alloc::New<DiscGizmo>("test::DiscGizmo"));
    CDCC_TRY(g.status());
    tm.addGizmo(std::move(g).value());
    return Status();
  }
  void deactivate(Context &, ToolManager &)
  {
    g_log += "deactivate;";
  }
  EventResult
  onEvent(Context &ctx, ToolManager &tm, const ViewMath &, const InputEvent &e)
  {
    g_script.seen = ctx.tools();
    if (g_script.reenter) {
      g_script.reentered = tm.setActive(ctx, "");
    }
    if (e.type != EventType::PointerDown || e.button != 0) {
      return EventResult::Unhandled;
    }
    g_log += "tool-press;";
    if (!beginDragCommand(ctx).ok()) {
      return EventResult::Unhandled;
    }
    if (g_script.pushModal) {
      pushDragModal(ctx, tm).ignore();
    }
    return EventResult::Handled;
  }
  void settingsChanged(Context &, ToolManager &)
  {
    g_log += "settings;";
  }
  static lbt::Struct<DragTool> *defineBindings()
  {
    auto *st = new lbt::Struct<DragTool>("test::DragTool", sizeof(DragTool));
    BIND_STRUCT_METHOD(st, activate, MARGS("ctx", "tm"));
    BIND_STRUCT_METHOD(st, deactivate, MARGS("ctx", "tm"));
    BIND_STRUCT_METHOD(st, onEvent, MARGS("ctx", "tm", "view", "e"));
    BIND_STRUCT_METHOD(st, settingsChanged, MARGS("ctx", "tm"));
    return st;
  }
};

/* A tool whose activate fails. */
struct BadTool {
  Status activate(Context &, ToolManager &)
  {
    return Status::error(StatusCode::Internal, "no");
  }
  EventResult onEvent(Context &, ToolManager &, const ViewMath &, const InputEvent &)
  {
    return EventResult::Unhandled;
  }
  static lbt::Struct<BadTool> *defineBindings()
  {
    auto *st = new lbt::Struct<BadTool>("test::BadTool", sizeof(BadTool));
    BIND_STRUCT_METHOD(st, activate, MARGS("ctx", "tm"));
    BIND_STRUCT_METHOD(st, onEvent, MARGS("ctx", "tm", "view", "e"));
    return st;
  }
};

struct ToolSetup : Setup {
  ToolManager tm{w.rt};
  View view = testView();
  const ToolDef *tool = nullptr;

  ToolSetup()
  {
    g_log.clear();
    g_script = Script();
    ctx.setTools(&tm);
    test_assert((registerImpl<Modal, DragModal>(w.rt, "test.modal")).ok());
    test_assert((registerImpl<Gizmo, DiscGizmo>(w.rt, "test.gizmo")).ok());
    Result<const ToolDef *> t = addNativeTool<DragTool>(
        w.rt,
        ToolDef("test.tool.drag")
            .setting(PropertyDef::floatProp("size").defaultValue(PropValue::ofFloat(2)))
            .setting(PropertyDef::enumProp("mode").item("MOVE", 0).item("TURN", 1)));
    test_assert(t.ok());
    tool = *t;
  }
  ~ToolSetup()
  {
    tm.shutdown(ctx);
  }
  Result<EventResult> send(const InputEvent &e)
  {
    return tm.dispatch(ctx, view, e);
  }
};

} // namespace

static void test_registry()
{
  ToolSetup s;
  ToolRegistry &reg = s.w.rt.ext<ToolRegistry>();
  test_assert(reg.size() == 1 && reg.find("test.tool.drag") == s.tool);
  test_assert(std::string(s.tool->uiNameText().c_str()) == "Drag");
  test_assert(
      addNativeTool<DragTool>(s.w.rt, ToolDef("test.tool.drag")).status().code() ==
      StatusCode::AlreadyExists);
  test_assert(addNativeTool<DragTool>(s.w.rt, ToolDef("test..x")).status().code() ==
              StatusCode::InvalidArgument);
  test_assert(addNativeTool<DragTool>(s.w.rt,
                                      ToolDef("test.tool.twice")
                                          .setting(PropertyDef::intProp("a"))
                                          .setting(PropertyDef::intProp("a")))
                  .status()
                  .code() == StatusCode::InvalidArgument);
  test_assert(reg.add(ToolDef("test.tool.noimpl")).status().code() ==
              StatusCode::InvalidArgument);
  /* Layouts: checked against the settings. */
  test_assert(addNativeTool<DragTool>(s.w.rt,
                                      ToolDef("test.tool.badlayout")
                                          .setting(PropertyDef::intProp("a"))
                                          .layout("<layout><prop path='nope'/></layout>"))
                  .status()
                  .code() == StatusCode::InvalidArgument);
  /* Checked twice (by addNativeTool, then add) against one struct, made once. */
  ApiRegistry &api = s.w.rt.ext<ApiRegistry>();
  const int structs = api.structCount();
  test_assert(addNativeTool<DragTool>(s.w.rt,
                                      ToolDef("test.tool.layout")
                                          .setting(PropertyDef::intProp("a"))
                                          .layout("<layout><prop path='a'/></layout>"))
                  .ok());
  test_assert(api.structCount() == structs + 1);
  Status unknown = reg.unknown("test.tool.drga");
  test_assert(unknown.code() == StatusCode::NotFound &&
              unknown.suggestions().size() >= 1);
  test_assert(s.tm.setActive(s.ctx, "test.tool.nope").code() == StatusCode::NotFound);
}

static void test_settings()
{
  ToolSetup s;
  ToolSettingsStore &st = s.w.rt.ext<ToolSettingsStore>();
  test_assert(st.get(*s.tool, "size")->asFloat() == 2.0 && !st.get(*s.tool, "nope"));
  const uint64_t v = st.version(*s.tool);
  test_assert(st.set(*s.tool, "size", PropValue::ofFloat(2)).ok() &&
              st.version(*s.tool) == v);
  test_assert(st.set(*s.tool, "size", PropValue::ofFloat(3)).ok() &&
              st.version(*s.tool) > v);
  test_assert(st.set(*s.tool, "nope", PropValue::ofFloat(3)).code() ==
              StatusCode::NotFound);
  test_assert(st.set(*s.tool, "mode", PropValue::ofEnum(1)).ok());
  const std::string text = st.serialize();
  test_assert(text == "test.tool.drag(size=3 mode=TURN)\n");
  st.reset(*s.tool);
  test_assert(st.get(*s.tool, "size")->asFloat() == 2.0);
  test_assert(st.load("# a comment\ngone.tool(x=1)\n(\n" + text).code() ==
              StatusCode::InvalidArgument); /* line 3; the rest loads */
  test_assert(st.get(*s.tool, "size")->asFloat() == 3.0);
  test_assert(st.get(*s.tool, "mode")->asEnum() == 1);

  /* The active tool hears of changes before its next event. */
  test_assert(s.tm.setActive(s.ctx, "test.tool.drag").ok());
  g_log.clear();
  test_assert(s.send(event(EventType::PointerMove, 500, 500)).ok());
  test_assert(g_log.find("settings;") == std::string::npos);
  test_assert(st.set(*s.tool, "size", PropValue::ofFloat(4)).ok());
  test_assert(s.send(event(EventType::PointerMove, 500, 500)).ok());
  test_assert(g_log.find("settings;") != std::string::npos);
}

static void test_drag_is_one_step()
{
  ToolSetup s;
  test_assert(s.tm.setActive(s.ctx, "test.tool.drag").ok() && g_log == "activate;");
  test_assert(s.tm.gizmoCount() == 1);
  /* A press away from the gizmo goes to the tool, which begins a drag. */
  Result<EventResult> r = s.send(event(EventType::PointerDown, 500, 500));
  test_assert(r.ok() && *r == EventResult::Handled && s.tm.modalOpen());
  test_assert(s.ex.interactiveOpen() && s.tm.activeGizmo() == -1);
  for (int i = 1; i <= 5; i++) {
    test_assert(s.send(event(EventType::PointerMove, 100.0 * i, 500)).ok());
  }
  test_assert(strength(s.ctx) == 0.5 && s.history.size() == 0);
  /* A wheel goes to the host. */
  r = s.send(event(EventType::Wheel, 500, 500));
  test_assert(r.ok() && *r == EventResult::Unhandled && s.tm.modalOpen());
  /* setActive is Busy during a gesture. */
  test_assert(s.tm.setActive(s.ctx, "").code() == StatusCode::Busy);
  g_log.clear();
  test_assert(s.send(event(EventType::PointerUp, 500, 500)).ok());
  test_assert(g_log == "modal;end-commit;" && !s.tm.modalOpen());
  test_assert(!s.ex.interactiveOpen() && s.history.size() == 1);
  test_assert(strength(s.ctx) == 0.5);
  test_assert(s.ex.undo(s.ctx).ok() && strength(s.ctx) == 0.5);
}

static void test_values()
{
  ToolSetup s;
  test_assert(s.tm.setActive(s.ctx, "test.tool.drag").ok());
  test_assert(s.send(event(EventType::PointerDown, 500, 500)).ok());
  test_assert(s.send(event(EventType::PointerMove, 800, 500)).ok());
  test_assert(strength(s.ctx) == 0.8);
  test_assert(s.send(event(EventType::PointerUp, 800, 500)).ok());
  test_assert(s.history.size() == 1 && strength(s.ctx) == 0.8);
  test_assert(s.ex.undo(s.ctx).ok() && strength(s.ctx) == 0.5);
  test_assert(s.ex.redo(s.ctx).ok() && strength(s.ctx) == 0.8);
}

static void test_cancel()
{
  ToolSetup s;
  test_assert(s.tm.setActive(s.ctx, "test.tool.drag").ok());
  /* Escape, the host's Cancel and a lost focus all cancel. */
  for (int how = 0; how < 4; how++) {
    test_assert(s.send(event(EventType::PointerDown, 500, 500)).ok());
    test_assert(s.send(event(EventType::PointerMove, 700, 500)).ok());
    test_assert(strength(s.ctx) == 0.7);
    g_log.clear();
    if (how == 0) {
      InputEvent esc = event(EventType::KeyDown, 0, 0);
      esc.key = keys::kEscape;
      test_assert(s.send(esc).ok());
    } else if (how == 1) {
      test_assert(s.send(event(EventType::Cancel, 0, 0)).ok());
    } else if (how == 2) {
      test_assert(s.send(event(EventType::FocusLost, 0, 0)).ok());
    } else {
      test_assert(s.tm.cancel(s.ctx).ok());
    }
    test_assert(g_log.find("end-cancel;") != std::string::npos);
    test_assert(!s.tm.modalOpen() && !s.ex.interactiveOpen());
    test_assert(strength(s.ctx) == 0.5 && s.history.size() == 0);
  }
  test_assert(s.tm.cancel(s.ctx).ok()); /* nothing to cancel */
}

static void test_undo_during_drag()
{
  ToolSetup s;
  test_assert(s.tm.setActive(s.ctx, "test.tool.drag").ok());
  test_assert(s.send(event(EventType::PointerDown, 500, 500)).ok());
  const uint64_t serial = s.ex.interactiveSerial();
  test_assert(s.send(event(EventType::PointerMove, 900, 500)).ok());
  /* The host's undo cancels the command behind the modal's back. */
  test_assert(s.ex.undo(s.ctx).ok() && !s.ex.interactiveOpen());
  test_assert(s.ex.interactiveSerial() != serial && strength(s.ctx) == 0.5);
  test_assert(s.tm.modalOpen()); /* until the next event or draw */
  g_log.clear();
  Result<EventResult> r = s.send(event(EventType::PointerMove, 300, 500));
  test_assert(r.ok() && g_log == "end-cancel;update;" && !s.tm.modalOpen());
  test_assert(strength(s.ctx) == 0.5 && s.history.size() == 0);
  /* A new drag that another tool's command replaced: also popped, without touching it. */
  test_assert(s.send(event(EventType::PointerDown, 500, 500)).ok());
  test_assert(s.ex.cancel(s.ctx).ok() && beginDragCommand(s.ctx).ok());
  g_log.clear();
  DrawList out;
  test_assert(s.tm.draw(s.ctx, s.view, out).ok());
  test_assert(g_log.find("end-cancel;") != std::string::npos && !s.tm.modalOpen());
  test_assert(s.ex.interactiveOpen()); /* not the manager's */
  test_assert(s.ex.cancel(s.ctx).ok());
}

static void test_gizmo()
{
  ToolSetup s;
  test_assert(s.tm.setActive(s.ctx, "test.tool.drag").ok());
  /* Hover: moves hit-test the gizmo. */
  test_assert(s.send(event(EventType::PointerMove, 104, 100)).ok());
  test_assert(s.tm.hoverGizmo() == 1 && s.tm.hoverPart() == 3);
  test_assert(s.send(event(EventType::PointerMove, 300, 100)).ok());
  test_assert(s.tm.hoverGizmo() == -1 && s.tm.hoverPart() == -1);
  /* Near enough: within hitRadius of the edge. */
  test_assert(s.send(event(EventType::PointerMove, 114, 100)).ok());
  test_assert(s.tm.hoverGizmo() == 1);
  s.tm.hitRadius = 2;
  test_assert(s.send(event(EventType::PointerMove, 114, 100)).ok());
  test_assert(s.tm.hoverGizmo() == -1);
  s.tm.hitRadius = 6;

  g_log.clear();
  test_assert(s.send(event(EventType::PointerDown, 100, 100)).ok());
  test_assert(g_log == "update;gizmo-drag;" && s.tm.modalOpen());
  test_assert(s.tm.activeGizmo() == 1 && s.tm.activePart() == 3);
  /* A right press during the drag goes to the modal, not to the gizmo or tool. */
  g_log.clear();
  test_assert(s.send(event(EventType::PointerDown, 100, 100, 2)).ok());
  test_assert(g_log == "modal;");

  DrawList out;
  test_assert(s.tm.draw(s.ctx, s.view, out).ok());
  DrawListReader rd;
  test_assert(rd.read(out.bytes()).ok());
  test_assert(rd.items().size() == 2); /* the gizmo's disc, the modal's line */
  test_assert(rd.items()[0].style.id == ToolManager::kIdStride + 3);
  test_assert(rd.items()[0].style.color == 0xff00ffffu); /* active */
  /* The camera's position, from the view matrix's inverse. */
  test_assert(std::abs(rd.origin()[0] - 1.0) < 1e-12 &&
              std::abs(rd.origin()[1] - 2.0) < 1e-12 &&
              std::abs(rd.origin()[2] - 3.0) < 1e-12);

  test_assert(s.send(event(EventType::PointerUp, 100, 100)).ok());
  test_assert(s.history.size() == 1 && s.tm.activeGizmo() == -1);
  /* Deactivating removes the gizmos. */
  test_assert(s.tm.setActive(s.ctx, "").ok() && s.tm.gizmoCount() == 0);
  test_assert(s.tm.active() == nullptr);
  Result<EventResult> r = s.send(event(EventType::PointerDown, 100, 100));
  test_assert(r.ok() && *r == EventResult::Unhandled);
}

static void test_errors_and_busy()
{
  ToolSetup s;
  test_assert(addNativeTool<BadTool>(s.w.rt, ToolDef("test.tool.bad")).ok());
  test_assert(s.tm.setActive(s.ctx, "test.tool.drag").ok());
  /* A failed activate leaves no tool. */
  test_assert(s.tm.setActive(s.ctx, "test.tool.bad").code() == StatusCode::Internal);
  test_assert(s.tm.active() == nullptr && s.tm.gizmoCount() == 0);
  test_assert(s.tm.setActive(s.ctx, "test.tool.drag").ok());

  /* A tool that begins a command and pushes no modal: the command is cancelled. */
  g_script.pushModal = false;
  Result<EventResult> r = s.send(event(EventType::PointerDown, 500, 500));
  test_assert(r.status().code() == StatusCode::FailedPrecondition);
  test_assert(!s.ex.interactiveOpen() && s.history.size() == 0);
  g_script.pushModal = true;

  /* Calls from inside a slot are Busy. */
  g_script.reenter = true;
  test_assert(s.send(event(EventType::PointerMove, 500, 500)).ok());
  test_assert(g_script.reentered.code() == StatusCode::Busy);
  test_assert(s.tm.active() == s.tool);
  g_script.reenter = false;

  /* A sticky error from elsewhere: taken, and the call fails. */
  s.w.rt.setStickyError(Status::error(StatusCode::Internal, "elsewhere"));
  r = s.send(event(EventType::PointerMove, 500, 500));
  test_assert(r.status().code() == StatusCode::FailedPrecondition);
  test_assert(!s.w.rt.hasStickyError());

  /* A modal's exception: the gesture is cancelled and the error returned. */
  test_assert(s.send(event(EventType::PointerDown, 500, 500)).ok());
  test_assert(s.send(event(EventType::PointerMove, 900, 500)).ok());
  g_script.stickyInModal = true;
  r = s.send(event(EventType::PointerMove, 800, 500));
  test_assert(r.status().code() == StatusCode::Internal);
  test_assert(!s.tm.modalOpen() && !s.ex.interactiveOpen() && strength(s.ctx) == 0.5);
  test_assert(!s.w.rt.hasStickyError());
  g_script.stickyInModal = false;

  /* A gizmo's update that throws: the tool still gets the press, and the error comes
   * back after it; the drag goes on. */
  g_script.stickyInUpdate = true;
  g_log.clear();
  r = s.send(event(EventType::PointerDown, 500, 500));
  test_assert(r.status().code() == StatusCode::Internal && g_log == "update;tool-press;");
  test_assert(s.tm.modalOpen() && s.ex.interactiveOpen() && !s.w.rt.hasStickyError());
  g_script.stickyInUpdate = false;
  test_assert(s.send(event(EventType::PointerUp, 500, 500)).ok() &&
              s.history.size() == 1);

  /* pushModal with no open command, or one another modal owns. */
  Result<Owned<Modal>> m =
      own<Modal>(s.w.rt, "test.modal", litestl::alloc::New<DragModal>("test::DragModal"));
  test_assert(s.tm.pushModal(s.ctx, std::move(m).value(), true).code() ==
              StatusCode::FailedPrecondition);
  test_assert(s.send(event(EventType::PointerDown, 500, 500)).ok());
  m = own<Modal>(s.w.rt, "test.modal", litestl::alloc::New<DragModal>("test::DragModal"));
  test_assert(s.tm.pushModal(s.ctx, std::move(m).value(), true).code() ==
              StatusCode::FailedPrecondition);
  test_assert(s.tm.cancel(s.ctx).ok() && !s.ex.interactiveOpen());
}

/* What the tools-and-overlay review found (docs/reports/feature-report-tools-overlay-
 * 2026-11-26.md), each as it was proved. */
static void test_review_fixes()
{
  ToolSetup s;
  test_assert(s.tm.setActive(s.ctx, "test.tool.drag").ok() && s.tm.gizmoCount() == 1);
  const auto gizmo = [&] {
    Result<Owned<Gizmo>> g = own<Gizmo>(
        s.w.rt, "test.gizmo", litestl::alloc::New<DiscGizmo>("test::DiscGizmo"));
    return s.tm.addGizmo(std::move(g).value());
  };

  /* clearGizmos outside a call, with three: all of them go (it skipped every other). */
  test_assert(gizmo() == 2 && gizmo() == 3 && s.tm.gizmoCount() == 3);
  s.tm.clearGizmos();
  test_assert(s.tm.gizmoCount() == 0);
  /* Ids are reused, the smallest free first, so record ids stay within 32 bits. */
  test_assert(gizmo() == 1 && gizmo() == 2 && gizmo() == 3);
  s.tm.removeGizmo(2);
  test_assert(gizmo() == 2 && s.tm.gizmoCount() == 3);
  s.tm.clearGizmos();
  test_assert(gizmo() == 1);

  /* A slot's ctx.tools() is the manager that called it, and is put back after. */
  ToolManager other(s.w.rt);
  test_assert(other.setActive(s.ctx, "test.tool.drag").ok());
  test_assert(other.dispatch(s.ctx, s.view, event(EventType::KeyDown, 0, 0)).ok());
  test_assert(g_script.seen == &other && s.ctx.tools() == &s.tm);
  other.shutdown(s.ctx);

  /* settingsChanged is an event slot: a draw doesn't call it; the next event does. */
  ToolSettingsStore &st = s.w.rt.ext<ToolSettingsStore>();
  test_assert(st.set(*s.tool, "size", PropValue::ofFloat(7)).ok());
  g_log.clear();
  DrawList out;
  test_assert(s.tm.draw(s.ctx, s.view, out).ok() && g_log.find("settings;") == std::string::npos);
  test_assert(s.send(event(EventType::PointerMove, 500, 500)).ok());
  test_assert(g_log.find("settings;") != std::string::npos);

  /* An event whose type isn't one. */
  InputEvent junk = event(EventType::PointerMove, 0, 0);
  junk.type = EventType(42);
  test_assert(s.send(junk).status().code() == StatusCode::InvalidArgument);
  junk = event(EventType::PointerMove, 0, 0);
  junk.pointer = PointerType(-1);
  test_assert(s.send(junk).status().code() == StatusCode::InvalidArgument);

  /* A modal gets its own pointer's events only. */
  test_assert(s.send(event(EventType::PointerDown, 500, 500)).ok() && s.tm.modalOpen());
  InputEvent finger = event(EventType::PointerMove, 900, 500);
  finger.pointerId = 5;
  g_log.clear();
  Result<EventResult> r = s.send(finger);
  test_assert(r.ok() && *r == EventResult::Unhandled && g_log.empty());
  test_assert(strength(s.ctx) == 0.5);

  /* A modal hands over to a new drag: the new modal stays, with its command. */
  g_script.handover = true;
  InputEvent next = event(EventType::KeyDown, 0, 0);
  next.key = 'n';
  test_assert(s.send(next).ok());
  test_assert(s.tm.modalCount() == 1 && s.ex.interactiveOpen() && s.history.size() == 1);
  g_script.handover = false;

  /* The outcome text outlives the gesture; the gesture's own doesn't. */
  g_script.outcome = true;
  test_assert(s.send(event(EventType::PointerUp, 500, 500)).ok() && !s.tm.modalOpen());
  test_assert(std::string(s.tm.status().c_str()) == "nothing to drag");
  g_script.outcome = false;

  /* A gizmo's update that throws, with `late`: the result and the error, both. */
  g_script.stickyInUpdate = true;
  Status late;
  r = s.tm.dispatch(s.ctx, s.view, event(EventType::PointerDown, 500, 500), &late);
  test_assert(r.ok() && *r == EventResult::Handled &&
              late.code() == StatusCode::Internal);
  test_assert(s.tm.modalOpen());
  g_script.stickyInUpdate = false;

  /* A slot can't pop what was pushed before it, and what it leaves is an error. */
  g_script.popInDraw = true;
  const Status drawn = s.tm.draw(s.ctx, s.view, out);
  test_assert(drawn.code() == StatusCode::InvalidArgument);
  test_assert(g_script.reentered.code() == StatusCode::InvalidArgument);
  DrawListReader rd;
  test_assert(rd.read(out.bytes()).ok());
  g_script.popInDraw = false;
  test_assert(s.tm.cancel(s.ctx).ok());

  /* The host chooses the origin. */
  s.tm.drawOrigin = double3(5, 6, 7);
  test_assert(s.tm.draw(s.ctx, s.view, out).ok() && rd.read(out.bytes()).ok());
  test_assert(rd.origin()[0] == 5.0 && rd.origin()[1] == 6.0 && rd.origin()[2] == 7.0);
}

static void test_shutdown()
{
  ToolSetup s;
  test_assert(s.tm.setActive(s.ctx, "test.tool.drag").ok());
  test_assert(s.send(event(EventType::PointerDown, 500, 500)).ok());
  test_assert(s.send(event(EventType::PointerMove, 900, 500)).ok());
  g_log.clear();
  s.tm.shutdown(s.ctx);
  test_assert(g_log == "end-cancel;deactivate;");
  test_assert(!s.ex.interactiveOpen() && strength(s.ctx) == 0.5 && !s.tm.active());
}

int main()
{
  test_registry();
  test_settings();
  test_drag_is_one_step();
  test_values();
  test_cancel();
  test_undo_during_drag();
  test_gizmo();
  test_errors_and_busy();
  test_review_fixes();
  test_shutdown();
  return test_end();
}
