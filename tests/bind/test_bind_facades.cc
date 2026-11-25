/* The script facades on the api fixture: values, bags, paths and commands (P1f step 4). */
#include "../command/command_fixture.h"

#include "coredcc/bind/checked.h"
#include "coredcc/bind/facades.h"
#include "coredcc/bind/foreign_command.h"
#include "coredcc/bind/layout_facades.h"
#include "coredcc/bind/manager.h"

#include "litestl/binding/manager.h"

#include <cstring>

test_init;

namespace sc = coredcc::script;

static void test_status()
{
  Status e = sc::statusError(StatusCode::NotFound, "gone");
  test_assert(e.code() == StatusCode::NotFound && strcmp(e.message(), "gone") == 0);
  /* Ok isn't an error code: still an error, never an abort. */
  test_assert(sc::statusError(StatusCode::Ok, "x").code() == StatusCode::InvalidArgument);
}

static void test_values()
{
  test_assert(sc::readBool(sc::makeBool(true)).value);
  test_assert(sc::readInt(sc::makeInt(-3)).value == -3);
  test_assert(sc::readFloat(sc::makeFloat(0.25)).value == 0.25);
  test_assert(strcmp(sc::readString(sc::makeString("hi").value).value.c_str(), "hi") == 0);
  test_assert(sc::makeVec2(1, 2) == PropValue::ofVec2(1, 2));
  test_assert(sc::makeVec4(1, 2, 3, 4) == PropValue::ofVec4(1, 2, 3, 4));

  /* The wrong type is an error result. */
  sc::IntResult wrong = sc::readInt(sc::makeString("3").value);
  test_assert(wrong.status.code() == StatusCode::InvalidArgument && wrong.value == 0);
  test_assert(strstr(wrong.status.message(), "String") != nullptr);
  test_assert(sc::readBool(sc::makeInt(1)).status.code() == StatusCode::InvalidArgument);
  test_assert(sc::readFloat(PropValue()).status.code() == StatusCode::InvalidArgument);
  test_assert(sc::readString(sc::makeFloat(1)).status.code() == StatusCode::InvalidArgument);

  /* Components: in range, out of range, and of a value without any. */
  const PropValue v = sc::makeVec3(1, 2, 3);
  test_assert(sc::readComponent(v, 2).value == 3 && sc::readComponent(v, 2).status.ok());
  test_assert(sc::readComponent(v, 3).status.code() == StatusCode::OutOfRange);
  test_assert(sc::readComponent(v, -1).status.code() == StatusCode::OutOfRange);
  test_assert(sc::readComponent(sc::makeFloat(5), 0).value == 5);
  test_assert(sc::readComponent(sc::makeInt(5), 0).status.code() == StatusCode::OutOfRange);
}

static void test_bags()
{
  PropertyBag bag;
  test_assert(bag.add(PropertyDef::intProp("amount").defaultValue(PropValue::ofInt(2))).ok());
  test_assert(bag.add(PropertyDef::stringProp("label")).ok());
  test_assert(sc::bagSize(bag) == 2);
  test_assert(strcmp(sc::bagName(bag, 1).value.c_str(), "label") == 0);
  test_assert(sc::bagName(bag, 2).status.code() == StatusCode::OutOfRange);
  test_assert(sc::bagName(bag, -1).status.code() == StatusCode::OutOfRange);
  test_assert(sc::bagGet(bag, "amount").value == PropValue::ofInt(2));
  sc::ValueResult miss = sc::bagGet(bag, "amonut");
  test_assert(miss.status.code() == StatusCode::NotFound);
  test_assert(miss.status.suggestions().size() == 1 &&
              strcmp(miss.status.suggestions()[0].c_str(), "amount") == 0);
}

/* Reads its input and writes its output through the facades. */
struct Doubler {
  Status canRun(Context &) const
  {
    return Status();
  }
  Status exec(Context &ctx)
  {
    sc::ValueResult in = sc::inputGet(ctx, "amount");
    test_assert(in.status.ok());
    test_assert(sc::inputGet(ctx, "nope").status.code() == StatusCode::NotFound);
    test_assert(sc::outputSet(ctx, "nope", in.value).code() == StatusCode::NotFound);
    CDCC_TRY(sc::outputSet(ctx, "result", sc::makeInt(2 * *in.value.tryInt())));
    test_assert(sc::outputGet(ctx, "result").value == sc::makeInt(2 * *in.value.tryInt()));
    return Status();
  }
  void undo(Context &) {}
  uint64_t memSize() const
  {
    return 0;
  }
  void discard() {}
  static lbt::Struct<Doubler> *defineBindings()
  {
    auto *st = new lbt::Struct<Doubler>("test::Doubler", sizeof(Doubler));
    BIND_COMMAND_CORE(st);
    return st;
  }
};

static void test_command_bags()
{
  Setup s;
  test_assert(addNativeCommand<Doubler>(
                  s.w.rt, CommandDef("test.double")
                              .input(PropertyDef::intProp("amount"))
                              .output(PropertyDef::intProp("result")))
                  .ok());
  /* Outside a slot there are no bags. */
  test_assert(sc::inputGet(s.ctx, "amount").status.code() == StatusCode::FailedPrecondition);
  test_assert(sc::outputSet(s.ctx, "result", sc::makeInt(1)).code() ==
              StatusCode::FailedPrecondition);
  test_assert(sc::outputGet(s.ctx, "result").status.code() == StatusCode::FailedPrecondition);
  sc::BagResult out = sc::invoke(s.ctx, "test.double(amount=21)");
  test_assert(out.status.ok() && sc::bagGet(out.value, "result").value == sc::makeInt(42));
}

static void test_paths()
{
  Setup s;
  Context &ctx = s.ctx;
  test_assert(strcmp(sc::resolvePath(ctx, "tool.strength").value.c_str(), "Float") == 0);
  Status bad = sc::resolvePath(ctx, "tool.strenght").status;
  test_assert(bad.code() == StatusCode::NotFound && bad.suggestions().size() >= 1);
  test_assert(sc::getValue(ctx, "tool.strength").value == PropValue::ofFloat(0.5));
  test_assert(sc::getValue(ctx, "tool.nope").status.code() == StatusCode::NotFound);

  /* setValue is undoable; writeValue isn't recorded. */
  test_assert(sc::setValue(ctx, "tool.strength", sc::makeFloat(0.75)).ok());
  test_assert(s.w.tool.strength == 0.75 && s.history.size() == 1);
  test_assert(sc::undo(ctx).ok() && s.w.tool.strength == 0.5);
  test_assert(sc::redo(ctx).ok() && s.w.tool.strength == 0.75);
  test_assert(sc::writeValue(ctx, "tool.strength", sc::makeFloat(0.25)).ok());
  test_assert(s.w.tool.strength == 0.25 && s.history.size() == 1);
  test_assert(sc::writeValue(ctx, "tool.version", sc::makeInt(9)).code() != StatusCode::Ok);

  /* invoke: outputs, and the parser's errors. */
  const int64_t c0 = s.w.tool.count;
  sc::BagResult r = sc::invoke(ctx, "test.add(amount=3)");
  test_assert(r.status.ok() && sc::bagGet(r.value, "result").value == sc::makeInt(c0 + 3));
  test_assert(!sc::invoke(ctx, "test.add(amount=").status.ok());
  test_assert(sc::invoke(ctx, "test.nope()").status.code() == StatusCode::NotFound);
  test_assert(s.history.validate().ok() && !s.w.rt.hasStickyError());
}

static void test_no_executor()
{
  World w;
  Context ctx = w.context();
  test_assert(sc::setValue(ctx, "tool.strength", sc::makeFloat(1)).code() ==
              StatusCode::FailedPrecondition);
  test_assert(sc::undo(ctx).code() == StatusCode::FailedPrecondition);
  test_assert(sc::redo(ctx).code() == StatusCode::FailedPrecondition);
  test_assert(sc::writeValue(ctx, "tool.strength", sc::makeFloat(1)).ok());
}

static void test_manager()
{
  litestl::binding::BindingManager *mgr = sc::bindings();
  test_assert(mgr == sc::bindings());
  int found = 0;
  for (const litestl::binding::types::Method *m : mgr->getFunctions()) {
    for (const char *name : {"coredcc::registerForeignCommand", "coredcc::invoke",
                             "coredcc::newCommandDef", "coredcc::readComponent"}) {
      found += strcmp(m->name.c_str(), name) == 0;
    }
  }
  test_assert(found == 4);
}

/* What is bound refuses a NUL in any string. */
static void test_nul()
{
  using sc::detail::Checked;
  Setup s;
  sc::string nul("a");
  nul += char(0);
  nul += "b";
  test_assert(nul.size() == 3);
  const sc::string fine("label");
  test_assert(Checked<&sc::makeString>::call(nul).status.code() == StatusCode::InvalidArgument);
  test_assert(Checked<&sc::makeString>::call(fine).value.asString() == fine);
  const Status second = Checked<&sc::setValue>::call(s.ctx, nul, sc::makeInt(1));
  test_assert(second.code() == StatusCode::InvalidArgument &&
              strstr(second.message(), "argument 2: a string can't hold a NUL"));
  test_assert(strstr(Checked<&sc::invokeAt>::call(s.ctx, fine, nul).status.message(), "argument 3"));
  test_assert(Checked<&sc::setValue>::call(s.ctx, "tool.count", sc::makeInt(1)).ok());
  test_assert(Checked<&sc::canUndo>::call(s.ctx).value);
  test_assert(sc::detail::nulArgument(1, fine, nul) == 3);
  /* The ids of defs: refused later. */
  CommandDef *def = sc::newCommandDef(nul);
  test_assert(!s.w.rt.ext<CommandRegistry>().checkDef(*def).ok());
  litestl::alloc::Delete(def);
}

int main()
{
  test_status();
  test_values();
  test_bags();
  test_command_bags();
  test_paths();
  test_no_executor();
  test_manager();
  test_nul();
  return test_end();
}
