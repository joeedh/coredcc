/* ToolSettingsStore by path: toolSettings["<tool>"].<setting>, coercion, NoUndo, and the
 * layout check against a tool's settings. */
#include "../command/command_fixture.h"

#include "coredcc/tool/settings.h"
#include "coredcc/tool/tool.h"

#include <string>

test_init;

namespace {

struct NopTool {
  Status activate(Context &, ToolManager &)
  {
    return Status();
  }
  int32_t onEvent(Context &, ToolManager &, const ViewMath &, const InputEvent &)
  {
    return eventresult::kUnhandled;
  }
  static lbt::Struct<NopTool> *defineBindings()
  {
    auto *st = new lbt::Struct<NopTool>("test::NopTool", sizeof(NopTool));
    BIND_STRUCT_METHOD(st, activate, MARGS("ctx", "tm"));
    BIND_STRUCT_METHOD(st, onEvent, MARGS("ctx", "tm", "view", "e"));
    return st;
  }
};

} // namespace

static void test_paths()
{
  Setup s;
  Runtime &rt = s.w.rt;
  Result<const ToolDef *> tool = addNativeTool<NopTool>(
      rt,
      ToolDef("test.tool.nop")
          .setting(PropertyDef::floatProp("radius")
                       .defaultValue(PropValue::ofFloat(1))
                       .range(0.0, 10.0))
          .setting(PropertyDef::enumProp("mode").item("MOVE", 0).item("TURN", 1))
          .layout("<layout><prop path='radius'/><prop path='mode'/></layout>"));
  test_assert(tool.ok());
  struct Prefs {
    int unused = 0;
  } prefs;
  ApiStruct root("Prefs");
  rt.ext<ToolSettingsStore>().addCollectionTo(root);
  test_assert(rt.ext<ApiRegistry>().add(std::move(root)).ok());
  test_assert(rt.ext<ApiRegistry>()
                  .addRuntimeOwner("prefs",
                                   StructId::of("Prefs"),
                                   [&prefs]() { return (void *)&prefs; })
                  .ok());
  Context pc(&rt, &s.w.lib, rt.ext<ApiRegistry>().runtimeOwner("prefs"));
  pc.setExecutor(&s.ex);

  const char *radius = "toolSettings[\"test.tool.nop\"].radius";
  test_assert(*getValue(pc, radius) == PropValue::ofFloat(1));
  test_assert(setValue(pc, radius, PropValue::ofFloat(4)).ok());
  test_assert(rt.ext<ToolSettingsStore>().get(**tool, "radius")->asFloat() == 4.0);
  /* Coerced: clamped to the range. */
  test_assert(setValue(pc, radius, PropValue::ofFloat(50)).ok());
  test_assert(getValue(pc, radius)->asFloat() == 10.0);
  test_assert(
      setValue(pc, "toolSettings[\"test.tool.nop\"].mode", PropValue::ofEnum(1)).ok());
  /* NoUndo: an undoable write writes directly. */
  test_assert(setValueUndo(pc, radius, PropValue::ofFloat(2)).ok() &&
              s.history.size() == 0);
  test_assert(getValue(pc, radius)->asFloat() == 2.0);
  /* Unknown tools and settings. */
  test_assert(!getValue(pc, "toolSettings[\"test.tool.nope\"].radius").ok());
  test_assert(!getValue(pc, "toolSettings[\"test.tool.nop\"].nope").ok());
  test_assert(*getValue(pc, "toolSettings.length") == PropValue::ofInt(1));
}

static void test_layouts()
{
  Setup s;
  Runtime &rt = s.w.rt;
  auto add = [&](const char *id, const char *layout) {
    return addNativeTool<NopTool>(
               rt, ToolDef(id).setting(PropertyDef::intProp("count")).layout(layout))
        .status();
  };
  test_assert(add("test.tool.a", "<layout><prop path='count'/></layout>").ok());
  Status missing = add("test.tool.b", "<layout><prop path='cuont'/></layout>");
  test_assert(missing.code() == StatusCode::InvalidArgument);
  test_assert(std::string(missing.message()).find("cuont") != std::string::npos);
  test_assert(add("test.tool.c", "<layout><prop").code() == StatusCode::InvalidArgument);
  test_assert(!rt.ext<ToolRegistry>().find("test.tool.b"));
  /* Pointer settings are refused: they aren't kept. */
  test_assert(addNativeTool<NopTool>(
                  rt,
                  ToolDef("test.tool.d")
                      .setting(PropertyDef::pointerProp("obj", StructId::of("Object"))))
                  .status()
                  .code() == StatusCode::InvalidArgument);
}

int main()
{
  test_paths();
  test_layouts();
  return test_end();
}
