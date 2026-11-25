/* setValueUndo, invocation, LastUsed, value literals and registration. */
#include "command_fixture.h"

#include <cmath>
#include <string>

test_init;

static void test_set_value_undo()
{
  Setup s;
  /* Through `active`; undo writes the same object after `active` moved. */
  test_assert(setValueUndo(s.ctx, "objects.active.location[1]", PropValue::ofFloat(3)).ok());
  test_assert(s.w.object(0).location[1] == 3 && s.history.size() == 1);
  s.w.scene.active = 1;
  test_assert(s.ex.undo(s.ctx).ok());
  test_assert(s.w.object(0).location[1] == 0 && s.w.object(1).location[1] == 0);
  test_assert(s.ex.redo(s.ctx).ok() && s.w.object(0).location[1] == 3);

  /* A gesture folds into one step; another gesture or member doesn't. */
  for (double v : {0.1, 0.2, 0.3}) {
    test_assert(setValueUndo(s.ctx, "tool.strength", PropValue::ofFloat(v), 42).ok());
  }
  test_assert(s.history.size() == 2 && s.w.tool.strength == 0.3);
  test_assert(setValueUndo(s.ctx, "tool.strength", PropValue::ofFloat(0.4), 43).ok());
  test_assert(setValueUndo(s.ctx, "tool.free", PropValue::ofFloat(1), 43).ok());
  test_assert(s.history.size() == 4);
  test_assert(s.ex.undo(s.ctx).ok() && s.ex.undo(s.ctx).ok() && s.w.tool.strength == 0.3);
  test_assert(s.ex.undo(s.ctx).ok() && s.w.tool.strength == 0.5);

  /* Errors come back without a history entry. */
  test_assert(setValueUndo(s.ctx, "tool.version", PropValue::ofInt(1)).code() ==
              StatusCode::FailedPrecondition);
  test_assert(setValueUndo(s.ctx, "tool.nope", PropValue::ofInt(1)).code() == StatusCode::NotFound);
  test_assert(s.history.size() == 4);

  /* Undo of a set whose target is gone leaves a sticky error. */
  Setup t;
  test_assert(setValueUndo(t.ctx, "objects[0].data.radius", PropValue::ofFloat(2)).ok());
  test_assert(t.w.lib.writeRef(t.w.object(0).mesh, t.w.meshId(1)).ok());
  test_assert(t.w.lib.markDeleted(t.w.meshes[0]).ok());
  test_assert(t.ex.undo(t.ctx).code() == StatusCode::NotFound);
}

/* A member that says itself what to put back. */
static void test_save()
{
  Setup s;
  struct Data {
    bool set = false;
    double value = 0;
    int updates = 0;
  };
  static Data data;
  data = Data();
  ApiStruct st("test.Saving");
  ValueAccessor a;
  a.get = [](const PointerRef &, Context &) {
    return PropValue::ofFloat(data.set ? data.value : 7);
  };
  a.set = [](const PointerRef &, Context &, const PropValue &v) {
    data.set = true;
    data.value = v.asFloat();
    return Status();
  };
  a.save = [](const PointerRef &, Context &) {
    ValueAccessor::Stored out;
    out.bytes = data.set ? 20000 : 1000;
    out.restore = [was = data](const PointerRef &, Context &) {
      if (was.set && was.value < 0) {
        return Status::error(StatusCode::Unsupported, "restore fails");
      }
      data.set = was.set;
      data.value = was.value;
      return Status();
    };
    return out;
  };
  st.add(PropertyDef::floatProp("value"), std::move(a)).update([](const PointerRef &, Context &) {
    data.updates++;
  });
  ApiRegistry &reg = s.w.rt.ext<ApiRegistry>();
  test_assert(reg.add(std::move(st)).ok());
  test_assert(reg.addRuntimeOwner("saving", StructId::of("test.Saving"), []() {
                   return static_cast<void *>(&data);
                 }).ok());
  Context ctx(&s.w.rt, &s.w.lib, reg.runtimeOwner("saving"));
  ctx.setExecutor(&s.ex);
  int fired = 0;
  auto c = s.w.rt.ext<ChangeBus>().watch(
      ctx, "value", [&](const PropValue &, const Status &) { fired++; });
  auto flushed = [&](int want) {
    s.w.rt.ext<ChangeBus>().flush();
    return fired == want && data.updates == want;
  };
  const uint64_t empty = s.history.memoryUsed();
  test_assert(setValueUndo(ctx, "value", PropValue::ofFloat(3)).ok() && data.set && flushed(1));
  const uint64_t held = s.history.memoryUsed() - empty; /* what both hold, and little more */
  test_assert(held >= 21000 && held < 23000);
  test_assert(s.ex.undo(ctx).ok() && !data.set && flushed(2));
  test_assert(s.ex.redo(ctx).ok() && data.set && data.value == 3 && flushed(3));
  /* It may fail. */
  test_assert(setValueUndo(ctx, "value", PropValue::ofFloat(-1)).ok());
  test_assert(setValueUndo(ctx, "value", PropValue::ofFloat(5)).ok());
  test_assert(s.ex.undo(ctx).code() == StatusCode::Unsupported && data.value == 5);
  s.history.clear();
}

static void test_cancel_gesture()
{
  test_assert(strcmp(CommandDef("test.doIt").uiNameText().c_str(), "Do It") == 0);
  Setup s;
  Context &ctx = s.ctx;
  Runtime &rt = s.w.rt;
  test_assert(!isGesture(rt, 0) && !isGesture(rt, 1));
  test_assert(cancelGesture(ctx, 1).code() == StatusCode::InvalidArgument);
  const uint64_t a = beginGesture(rt), b = beginGesture(rt);
  test_assert(a != 0 && a != b && isGesture(rt, a) && isGesture(rt, b) && !isGesture(rt, b + 1));
  const double free = getValue(ctx, "tool.free")->asFloat();
  const auto set = [&](const char *path, double v, uint64_t g) {
    test_assert(setValueUndo(ctx, path, PropValue::ofFloat(v), g).ok());
  };
  /* Several members, and not those of another gesture before it. */
  set("tool.strength", 0.125, b);
  for (int i = 1; i <= 3; i++) {
    set("tool.strength", 0.25 * i, a);
    set("tool.free", i, a);
  }
  test_assert(s.history.size() == 7);
  test_assert(cancelGesture(ctx, a).ok());
  test_assert(s.w.tool.strength == 0.125 && getValue(ctx, "tool.free")->asFloat() == free);
  test_assert(s.history.size() == 1 && !s.history.canRedo() && s.history.validate().ok());
  test_assert(cancelGesture(ctx, a).ok() && s.history.size() == 1); /* there is none now */
  /* What comes after doesn't fold into what was before. */
  set("tool.strength", 0.375, b);
  test_assert(s.history.size() == 2 && s.history.head());
  test_assert(cancelGesture(ctx, b).ok() && s.history.size() == 0 && s.w.tool.strength == 0.5);

  /* Not if a command was run since, or one undone. */
  set("tool.strength", 0.25, a);
  test_assert(s.runAdd(1).ok() && cancelGesture(ctx, a).ok() && s.history.size() == 2);
  test_assert(s.ex.undo(ctx).ok() && cancelGesture(ctx, a).ok() && s.w.tool.strength == 0.25);
  test_assert(s.history.size() == 2 && s.history.canRedo());
  /* Nor past a barrier. */
  set("tool.strength", 0.75, b);
  s.history.pushBarrier();
  test_assert(cancelGesture(ctx, b).ok() && s.w.tool.strength == 0.75);

  /* An undo that fails is reported, and dropped. */
  set("tool.free", 5, a);
  rt.setStickyError(Status::error(StatusCode::Internal, "stray"));
  test_assert(cancelGesture(ctx, a).code() == StatusCode::FailedPrecondition);
  test_assert(getValue(ctx, "tool.free")->asFloat() == 5 && cancelGesture(ctx, a).ok());
  test_assert(getValue(ctx, "tool.free")->asFloat() == free);
  const int size = s.history.size();
  test_assert(setValueUndo(ctx, "objects[0].location[0]", PropValue::ofFloat(3), a).ok());
  Library &lib = s.w.lib;
  const BlockHandle h = s.w.scene.objects[0];
  test_assert(lib.markDeleted(h).ok());
  test_assert(!cancelGesture(ctx, a).ok() && s.history.size() == size && !rt.hasStickyError());

  Context bare(&rt, &lib, ctx.root());
  test_assert(cancelGesture(bare, a).code() == StatusCode::FailedPrecondition);
  test_assert(s.ex.beginInteractive(ctx, *s.drag, s.drag->newInputs()).ok());
  test_assert(cancelGesture(ctx, a).code() == StatusCode::Busy && s.ex.cancel(ctx).ok());
}

static void test_invoke_and_last_used()
{
  Setup s;
  Result<PropertyBag> r = invoke(s.ctx, "test.add(amount=5 mode=SLOW)");
  test_assert(r.ok() && s.w.tool.count == 9);
  /* Inputs not given take their LastUsed values. */
  test_assert(invoke(s.ctx, "test.add").ok() && s.w.tool.count == 14);
  LastUsedStore &lu = s.w.rt.ext<LastUsedStore>();
  test_assert(*lu.get(*s.add, "mode") == PropValue::ofEnum(1));
  test_assert(lu.get(*s.add, "mem") == nullptr); /* never set */

  Result<PropertyBag> bad = invoke(s.ctx, "test.add(amonut=1)");
  test_assert(bad.status().code() == StatusCode::NotFound);
  test_assert(hasMsg(bad.status(), "no input 'amonut' (at offset 9)"));
  test_assert(bad.status().suggestions().size() == 1);
  test_assert(invoke(s.ctx, "test.add(amount=\"x\")").status().code() == StatusCode::InvalidArgument);
  test_assert(invoke(s.ctx, "test.nope").status().code() == StatusCode::NotFound);
  test_assert(!invoke(s.ctx, "test.add(").ok());

  /* serialize/load round trip, skipping what no longer exists. */
  const std::string text = lu.serialize();
  test_assert(text == "test.add(amount=5 mode=SLOW)\n");
  lu.clear();
  test_assert(lu.load("# comment\n\ntest.gone(x=1)\ntest.add(amount=7 nope=1 mode=FAST)\r\n").ok());
  test_assert(*lu.get(*s.add, "amount") == PropValue::ofInt(7));
  test_assert(*lu.get(*s.add, "mode") == PropValue::ofEnum(0));
  test_assert(hasMsg(lu.load("test.add(amount=1)\ntest.add(\n"), "line 2"));
  test_assert(*lu.get(*s.add, "amount") == PropValue::ofInt(1));
  lu.clear();
  test_assert(lu.load(text).ok() && lu.serialize() == text);
}

static void test_command_defaults_path()
{
  Setup s;
  Runtime &rt = s.w.rt;
  struct Prefs {
    int unused = 0;
  } prefs;
  ApiStruct root("Prefs");
  rt.ext<LastUsedStore>().addCollectionTo(root);
  test_assert(rt.ext<ApiRegistry>().add(std::move(root)).ok());
  test_assert(rt.ext<ApiRegistry>()
                  .addRuntimeOwner("prefs", StructId::of("Prefs"), [&prefs]() { return (void *)&prefs; })
                  .ok());
  Context pc(&rt, &s.w.lib, rt.ext<ApiRegistry>().runtimeOwner("prefs"));
  pc.setExecutor(&s.ex);

  test_assert(*getValue(pc, "commandDefaults[\"test.add\"].amount") == PropValue::ofInt(1));
  test_assert(setValue(pc, "commandDefaults[\"test.add\"].amount", PropValue::ofInt(3)).ok());
  test_assert(*rt.ext<LastUsedStore>().get(*s.add, "amount") == PropValue::ofInt(3));
  test_assert(invoke(s.ctx, "test.add").ok() && s.w.tool.count == 7);
  /* NoUndo: setValueUndo writes directly. */
  test_assert(setValueUndo(pc, "commandDefaults[\"test.add\"].amount", PropValue::ofInt(4)).ok());
  test_assert(s.history.size() == 1);
  /* Watchers see a run, a write, and what is forgotten. */
  ChangeBus &bus = rt.ext<ChangeBus>();
  std::string seen;
  Result<ScopedConnection> watch =
      bus.watch(pc, "commandDefaults[\"test.add\"].amount",
                [&](const PropValue &v, const Status &) { seen += v.toString() + ";"; });
  test_assert(watch.ok() && bus.flush() == 0);
  const CommandDef *add = rt.ext<CommandRegistry>().find("test.add");
  LastUsedStore &store = rt.ext<LastUsedStore>();
  test_assert(store.set(*add, "amount", PropValue::ofInt(6)).ok() && bus.flush() == 1);
  const uint64_t epoch = bus.epoch();
  test_assert(store.set(*add, "amount", PropValue::ofInt(6)).ok() && bus.epoch() == epoch);
  test_assert(invoke(s.ctx, "test.add(amount=7)").ok() && bus.flush() == 1);
  store.forget(*add);
  test_assert(bus.flush() == 1);
  test_assert(store.set(*add, "amount", PropValue::ofInt(8)).ok() && bus.flush() == 1);
  store.clear();
  test_assert(bus.flush() == 1);
  test_assert(seen == "6;7;1;8;1;");
  test_assert(setValue(pc, "commandDefaults[\"test.add\"].amount", PropValue::ofInt(4)).ok());
  watch = Status::error(StatusCode::NotFound, "");
  test_assert(*getValue(pc, "commandDefaults.length") == PropValue::ofInt(rt.ext<CommandRegistry>().size()));
  Result<StableRef> c = canonicalize(pc, *parsePath("commandDefaults[\"test.add\"].mode"));
  test_assert(c.ok() && c->key.size() == 3);
  Result<PropValue> miss = getValue(pc, "commandDefaults[\"test.add\"].amonut");
  test_assert(miss.status().suggestions().size() == 1);
  test_assert(rt.validateAll().ok());
}

static void test_literals()
{
  Runtime rt;
  struct Case {
    PropertyDef def;
    PropValue value;
    const char *text;
  };
  const double m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  Case cases[] = {
      {PropertyDef::boolProp("b"), PropValue::ofBool(true), "true"},
      {PropertyDef::intProp("i"), PropValue::ofInt(-12), "-12"},
      {PropertyDef::floatProp("f"), PropValue::ofFloat(0.1), "0.1"},
      {PropertyDef::floatProp("f"), PropValue::ofFloat(INFINITY), "inf"},
      {PropertyDef::stringProp("s"), PropValue::ofString("a\"b\n"), "\"a\\\"b\\n\""},
      {PropertyDef::enumProp("e").item("A", 0).item("B", 3), PropValue::ofEnum(3), "B"},
      {PropertyDef::flagsProp("g").item("X", 1).item("Y", 4), PropValue::ofFlags(5), "\"X|Y\""},
      {PropertyDef::flagsProp("g").item("X", 1), PropValue::ofFlags(0), "0"},
      {PropertyDef::vecProp("v", 3), PropValue::ofVec3(1, -0.5, 2), "(1, -0.5, 2)"},
      {PropertyDef::mat4Prop("m"), PropValue::ofComponents(PropValue::Type::Mat4, m), nullptr},
  };
  for (Case &c : cases) {
    Result<std::string> lit = formatLiteral(rt, c.def, c.value);
    test_assert(lit.ok());
    if (c.text && *lit != c.text) {
      fprintf(stderr, "literal was %s, expected %s\n", lit->c_str(), c.text);
      test_assert(false);
    }
    Result<Invocation> inv = parseInvocation("x(a=" + *lit + ")");
    test_assert(inv.ok());
    Result<PropValue> back = parseLiteral(rt, c.def, inv->args[0].value);
    test_assert(back.ok() && *back == c.value);
  }
  test_assert(formatLiteral(rt, PropertyDef::enumProp("e").item("A", 0), PropValue::ofEnum(9))
                  .status()
                  .code() == StatusCode::InvalidArgument);
  /* Clamped on the way in. */
  Result<Invocation> inv = parseInvocation("x(a=5)");
  test_assert(*parseLiteral(rt, PropertyDef::floatProp("f").range(0, 1), inv->args[0].value) ==
              PropValue::ofFloat(1));
  test_assert(!parseLiteral(rt, PropertyDef::vecProp("v", 2), inv->args[0].value).ok());
}

static void test_registration()
{
  Setup s;
  Runtime &rt = s.w.rt;
  test_assert(addNativeCommand<AddCount>(rt, CommandDef("test.add")).status().code() ==
              StatusCode::AlreadyExists);
  /* Interactive needs a redo slot. */
  test_assert(hasMsg(addNativeCommand<AddCount>(
                         rt, CommandDef("test.dragNoRedo").flags(CommandFlags::Interactive))
                         .status(),
                     "redo slot"));
  /* No implementation, an unregistered one, duplicate inputs. */
  test_assert(hasMsg(rt.ext<CommandRegistry>().add(CommandDef("test.bare")).status(),
                     "no implementation"));
  CommandDef ghost("test.ghost");
  ghost.impl("test.ghost#impl", [](Runtime &) { return Result<Owned<Command>>(Owned<Command>()); });
  test_assert(rt.ext<CommandRegistry>().add(std::move(ghost)).status().code() == StatusCode::NotFound);
  test_assert(hasMsg(addNativeCommand<AddCount>(rt, CommandDef("test.dup")
                                                        .input(PropertyDef::intProp("a"))
                                                        .input(PropertyDef::intProp("a")))
                         .status(),
                     "input"));
  test_assert(rt.validateAll().ok());
}

int main()
{
  test_set_value_undo();
  test_save();
  test_invoke_and_last_used();
  test_cancel_gesture();
  test_command_defaults_path();
  test_literals();
  test_registration();
  return test_end();
}
