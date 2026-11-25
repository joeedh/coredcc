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
  test_invoke_and_last_used();
  test_command_defaults_path();
  test_literals();
  test_registration();
  return test_end();
}
