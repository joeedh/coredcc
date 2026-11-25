/* Running commands with values from scripts, asking whether they would run, and the interactive
 * and last commands. */
#include "script_fixture.h"

#include "coredcc/bind/run_facades.h"
#include "coredcc/command/last_used.h"

#include <string>
#include <utility>
#include <vector>

test_init;

namespace {

using Values = std::vector<std::pair<const char *, PropValue>>;

sc::RunInputs inputs(const Values &values)
{
  sc::RunInputs out = sc::newRunInputs();
  for (const auto &[name, value] : values) {
    test_assert(sc::runInputsSet(out, name, value).ok());
  }
  return out;
}

/* `mode` takes SLOW only if `fast` isn't set. */
struct Choose {
  Status canRun(Context &) const
  {
    return Status();
  }
  Status exec(Context &ctx)
  {
    const PropertyBag &in = *ctx.inputs();
    return ctx.outputs()->set("result", PropValue::ofInt(in.get("mode")->asEnum()));
  }
  void undo(Context &) {}
  uint64_t memSize() const
  {
    return 0;
  }
  void discard() {}
  static lbt::Struct<Choose> *defineBindings()
  {
    auto *st = new lbt::Struct<Choose>("test::Choose", sizeof(Choose));
    BIND_COMMAND_CORE(st);
    return st;
  }
};

/* The radius under the prefix. */
struct DragRadius {
  double before = 0;
  Status canRun(Context &ctx) const
  {
    return getValue(ctx, "radius").status();
  }
  Status exec(Context &ctx)
  {
    before = getValue(ctx, "radius")->asFloat();
    return Status();
  }
  void undo(Context &ctx)
  {
    test_assert(setValue(ctx, "radius", PropValue::ofFloat(before)).ok());
  }
  Status redo(Context &)
  {
    return Status();
  }
  uint64_t memSize() const
  {
    return 0;
  }
  void discard() {}
  static lbt::Struct<DragRadius> *defineBindings()
  {
    auto *st = new lbt::Struct<DragRadius>("test::DragRadius", sizeof(DragRadius));
    BIND_COMMAND_CORE(st);
    BIND_STRUCT_METHOD(st, redo, MARGS("ctx"));
    return st;
  }
};

struct RunWorld {
  Setup s;
  RunWorld()
  {
    test_assert(addNativeCommand<Choose>(
                    s.w.rt, CommandDef("test.choose")
                                .input(PropertyDef::enumProp("mode").item("FAST", 0).dynamic(
                                    [](PropertyDef &def, const void *owner) {
                                      const auto &bag = *static_cast<const PropertyBag *>(owner);
                                      if (bag.get("all")->asBool()) {
                                        def.item("SLOW", 1);
                                      }
                                    }))
                                .input(PropertyDef::boolProp("all"))
                                .output(PropertyDef::intProp("result")))
                    .ok());
    test_assert(addNativeCommand<DragRadius>(
                    s.w.rt, CommandDef("test.dragRadius").flags(CommandFlags::Interactive))
                    .ok());
  }
  double radius(int i)
  {
    return getValue(s.ctx, i ? "objects[1].data.radius" : "objects[0].data.radius")->asFloat();
  }
};

} // namespace

static void test_run()
{
  RunWorld w;
  Context &ctx = w.s.ctx;
  sc::BagResult r = sc::runAt(ctx, "", "test.add", inputs({{"amount", sc::makeInt(5)}}), true);
  test_assert(r.status.ok() && *r.value.get("result") == PropValue::ofInt(count(ctx)));
  /* The last-used value, unless it isn't wanted; the last one of a name. */
  const int64_t before = count(ctx);
  test_assert(sc::runAt(ctx, "", "test.add", inputs({}), true).status.ok() && count(ctx) == before + 5);
  test_assert(sc::runAt(ctx, "", "test.add", inputs({}), false).status.ok() && count(ctx) == before + 6);
  test_assert(sc::runAt(ctx, "", "test.add",
                        inputs({{"amount", sc::makeInt(1)}, {"amount", sc::makeInt(2)}}), true)
                  .status.ok() && count(ctx) == before + 8);
  /* Converted, and refused. */
  test_assert(sc::runAt(ctx, "", "test.add", inputs({{"mode", sc::makeString("SLOW").value}}), true).status.ok());
  test_assert(sc::runAt(ctx, "", "test.add", inputs({{"mode", sc::makeString("X").value}}), true)
                  .status.code() != StatusCode::Ok);
  sc::BagResult bad = sc::runAt(ctx, "", "test.add", inputs({{"amuont", sc::makeInt(1)}}), true);
  test_assert(bad.status.code() == StatusCode::NotFound && bad.status.suggestions().size() == 1);
  test_assert(sc::runAt(ctx, "", "test.ad", inputs({}), true).status.suggestions().size() > 0);
  test_assert(hasMsg(sc::runAt(ctx, "", "test.drag", inputs({}), true).status, "beginInteractive"));
  test_assert(hasMsg(sc::runAt(ctx, "a..b", "test.add", inputs({}), true).status, "expected"));
  test_assert(hasMsg(sc::runAt(ctx, "objects[99]", "test.add", inputs({}), true).status, "past 'objects'"));
  /* Dynamic inputs after those that decide about them, in whatever order they are given. */
  test_assert(sc::runAt(ctx, "", "test.choose",
                        inputs({{"mode", sc::makeString("SLOW").value}, {"all", sc::makeBool(true)}}), false)
                  .status.ok());
  test_assert(!sc::runAt(ctx, "", "test.choose", inputs({{"mode", sc::makeString("SLOW").value}}), false)
                   .status.ok());
  /* Under a prefix, or not, whatever the Context's is. */
  const Path tool = *parsePath("tool");
  PrefixScope scope(ctx, &tool);
  test_assert(hasMsg(sc::canRunAt(ctx, "", "test.dragRadius", inputs({}), true), "no member 'radius'"));
  test_assert(sc::canRunAt(ctx, "/objects[0].data", "test.dragRadius", inputs({}), true).ok());
  test_assert(hasMsg(sc::canRunAt(ctx, "/", "test.dragRadius", inputs({}), true), "no member 'radius'"));
  test_assert(*ctx.prefix() == tool);
  Context bare(&w.s.w.rt, &w.s.w.lib, ctx.root());
  for (const Status &s : {sc::runAt(bare, "", "test.add", inputs({}), true).status,
                          sc::canRunAt(bare, "", "test.add", inputs({}), true),
                          sc::canInvokeAt(bare, "", "test.add"),
                          sc::beginInteractiveAt(bare, "", "test.drag", inputs({}), true),
                          sc::interactiveId(bare).status, sc::commitInteractive(bare),
                          sc::cancelInteractive(bare), sc::lastCommandId(bare).status,
                          sc::writeInteractive(bare, "tool.count", sc::makeInt(1)),
                          sc::lastCommandInputs(bare).status, sc::rerunLast(bare, inputs({}))})
  {
    test_assert(s.code() == StatusCode::FailedPrecondition && hasMsg(s, "no executor"));
  }
}

static void test_checks()
{
  RunWorld w;
  Context &ctx = w.s.ctx;
  test_assert(sc::canInvokeAt(ctx, "", "test.add(amount=1)").ok() && w.s.history.size() == 0);
  test_assert(hasMsg(sc::canInvokeAt(ctx, "", "test.refuse"), "nothing selected"));
  test_assert(!sc::canInvokeAt(ctx, "a..b", "test.add").ok() && !sc::canInvokeAt(ctx, "", "a(").ok());
  sc::ToolChecks tools = sc::newToolChecks();
  test_assert(sc::toolChecksAdd(tools, "", "test.add(amount=1)").value == 0);
  test_assert(sc::toolChecksAdd(tools, "objects[0].data", "test.dragRadius").value == 1);
  test_assert(sc::toolChecksAdd(tools, "/", "test.dragRadius").value == 2);
  test_assert(sc::toolChecksAdd(tools, "", "test.\"").status.code() != StatusCode::Ok);
  test_assert(sc::toolChecksAdd(tools, "a..", "test.add").status.code() != StatusCode::Ok);
  test_assert(sc::toolChecksAdd(tools, "", "test.refuse").value == 3);
  const auto run = [&] {
    sc::StringResult r = sc::toolChecksRun(ctx, tools);
    test_assert(r.status.ok());
    return std::string(r.value.c_str());
  };
  const std::string all = run();
  if (all != "[[0,null],[1,null],[2,\"no member 'radius' in Scene (at offset 0)\"],[3,\"nothing selected\"]]") {
    fprintf(stderr, "%s\n", all.c_str());
    test_assert(false);
  }
  test_assert(run() == "[]");
  test_assert(sc::toolChecksAdd(tools, "", "test.parent").value == 4 && run() == "[[4,null]]");
  test_assert(sc::beginInteractiveAt(ctx, "", "test.drag", inputs({}), true).ok());
  const std::string busy = run();
  test_assert(busy.starts_with("[[0,\"") && busy.find("[3,\"") != std::string::npos && run() == "[]");
  test_assert(sc::cancelInteractive(ctx).ok() && run().starts_with("[[0,null],[1,null],[2,\"no member"));
  test_assert(!ctx.hasPrefix());
}

static void test_interactive()
{
  RunWorld w;
  Context &ctx = w.s.ctx;
  const auto id = [&] { return std::string(sc::interactiveId(ctx).value.c_str()); };
  test_assert(id() == "" && !sc::commitInteractive(ctx).ok() && !sc::cancelInteractive(ctx).ok());
  test_assert(!sc::writeInteractive(ctx, "tool.count", sc::makeInt(1)).ok());
  test_assert(!sc::beginInteractiveAt(ctx, "", "test.add", inputs({}), true).ok());
  test_assert(!sc::beginInteractiveAt(ctx, "", "test.dragRadius", inputs({}), true).ok() && id() == "");
  test_assert(!sc::beginInteractiveAt(ctx, "", "test.nope", inputs({}), true).ok());
  const double r1 = w.radius(1);
  for (const bool commit : {false, true}) {
    test_assert(sc::beginInteractiveAt(ctx, "objects[1].data", "test.dragRadius", inputs({}), true).ok());
    test_assert(id() == "test.dragRadius" && !ctx.hasPrefix());
    test_assert(sc::writeInteractive(ctx, "radius", sc::makeFloat(7)).ok() && w.radius(1) == 7);
    test_assert(!sc::writeInteractive(ctx, "nope", sc::makeFloat(7)).ok());
    test_assert((commit ? sc::commitInteractive(ctx) : sc::cancelInteractive(ctx)).ok() && id() == "");
    test_assert(w.radius(1) == (commit ? 7 : r1) && w.s.history.size() == (commit ? 1 : 0));
  }
  test_assert(std::string(sc::lastCommandId(ctx).value.c_str()) == ""); /* not again */
  test_assert(sc::undo(ctx).ok() && w.radius(1) == r1);
}

static void test_last()
{
  RunWorld w;
  Context &ctx = w.s.ctx;
  const auto id = [&] { return std::string(sc::lastCommandId(ctx).value.c_str()); };
  test_assert(id() == "" && hasMsg(sc::lastCommandInputs(ctx).status, "no command"));
  test_assert(hasMsg(sc::rerunLast(ctx, inputs({})), "no command"));
  const int64_t before = count(ctx);
  test_assert(w.s.runAdd(3).ok() && id() == "test.add");
  test_assert(*sc::lastCommandInputs(ctx).value.get("amount") == PropValue::ofInt(3));
  test_assert(sc::rerunLast(ctx, inputs({{"amount", sc::makeInt(10)}})).ok() && count(ctx) == before + 10);
  test_assert(w.s.history.size() == 1 && sc::rerunLast(ctx, inputs({})).ok() && count(ctx) == before + 10);
  test_assert(!sc::rerunLast(ctx, inputs({{"amuont", sc::makeInt(10)}})).ok() && count(ctx) == before + 10);
  test_assert(!sc::rerunLast(ctx, inputs({{"amount", sc::makeInt(-1)}})).ok() && count(ctx) == before + 10);
  test_assert(id() == "test.add");
  /* A set isn't one, nor is there one after an undo. */
  test_assert(sc::setValue(ctx, "tool.count", sc::makeInt(1)).ok() && id() == "");
  test_assert(sc::undo(ctx).ok() && id() == "");
}

/* From a script's slots, and from a kernel. */
static void test_slots()
{
  RunWorld w;
  Context &ctx = w.s.ctx;
  Host host;
  Script script;
  std::vector<Status> got;
  const auto all = [&](Context &c) {
    got = {sc::canRunAt(c, "", "test.add", inputs({}), true),
           sc::canInvokeAt(c, "", "test.add"),
           sc::toolChecksRun(c, sc::newToolChecks()).status,
           sc::beginInteractiveAt(c, "", "test.drag", inputs({}), true),
           sc::writeInteractive(c, "tool.count", sc::makeInt(1)),
           sc::commitInteractive(c),
           sc::cancelInteractive(c),
           sc::lastCommandId(c).status,
           sc::lastCommandInputs(c).status,
           sc::rerunLast(c, inputs({}))};
  };
  Status run;
  script.onExec = [&](Context &c) {
    all(c);
    run = sc::runAt(c, "", "test.add", inputs({{"amount", sc::makeInt(1)}}), true).status;
  };
  test_assert(registerScript(w.s, host, &script, "test.script", kCore).ok());
  test_assert(sc::invoke(ctx, "test.script(amount=0)").status.ok() && run.ok());
  test_assert(w.s.history.size() == 1 && w.s.history.at(0).subCount() == 1);
  for (const Status &s : got) {
    test_assert(s.code() == StatusCode::Busy);
  }
  script.onExec = nullptr;
  script.onCanRun = [&](Context &c) { run = sc::runAt(c, "", "test.add", inputs({}), true).status; };
  test_assert(sc::invoke(ctx, "test.script(amount=0)").status.ok() && run.code() == StatusCode::Busy);
}

int main()
{
  test_run();
  test_checks();
  test_interactive();
  test_last();
  test_slots();
  return test_end();
}
