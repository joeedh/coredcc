/* A Context's path prefix goes with a run: its undo, its redo and its sub-commands see it. */
#include "command_fixture.h"

test_init;

namespace {

/* Adds 1 to `radius`, which must be under the prefix, in every slot. */
struct Grow {
  std::string seen;
  double before = 0;
  Status canRun(Context &ctx) const
  {
    return getValue(ctx, "radius").status();
  }
  Status exec(Context &ctx)
  {
    Result<PropValue> v = getValue(ctx, "radius");
    if (!v.ok()) {
      return v.status();
    }
    before = v->asFloat();
    return setValue(ctx, "radius", PropValue::ofFloat(before + 1));
  }
  void undo(Context &ctx)
  {
    test_assert(setValue(ctx, "radius", PropValue::ofFloat(before)).ok());
  }
  uint64_t memSize() const
  {
    return 0;
  }
  void discard() {}
  static lbt::Struct<Grow> *defineBindings()
  {
    auto *st = new lbt::Struct<Grow>("test::Grow", sizeof(Grow));
    BIND_COMMAND_CORE(st);
    return st;
  }
};

/* Grow as a sub-command, and a property set, both under the prefix. */
struct GrowTwice {
  Status canRun(Context &) const
  {
    return Status();
  }
  Status exec(Context &ctx)
  {
    const CommandDef *grow = ctx.runtime().ext<CommandRegistry>().find("test.grow");
    Result<PropertyBag> out = ctx.executor()->runSub(ctx, *grow, grow->newInputs());
    CDCC_TRY(out.status());
    PrefixScope root(ctx, nullptr); /* a slot can set its own, for as long as it runs */
    return setValueUndo(ctx, "tool.strength", PropValue::ofFloat(0.125));
  }
  void undo(Context &) {}
  uint64_t memSize() const
  {
    return 0;
  }
  void discard() {}
  static lbt::Struct<GrowTwice> *defineBindings()
  {
    auto *st = new lbt::Struct<GrowTwice>("test::GrowTwice", sizeof(GrowTwice));
    BIND_COMMAND_CORE(st);
    return st;
  }
};

/* Folds with any other run of itself. Leaves the prefix changed. */
struct Folder {
  int runs = 1;
  Status canRun(Context &) const
  {
    return Status();
  }
  Status exec(Context &ctx)
  {
    ctx.setPrefix(*parsePath("tool"));
    return Status();
  }
  void undo(Context &) {}
  uint64_t memSize() const
  {
    return 0;
  }
  void discard() {}
  uint64_t foldKey() const
  {
    return 7;
  }
  bool foldFrom(Command)
  {
    runs++;
    return true;
  }
  static lbt::Struct<Folder> *defineBindings()
  {
    auto *st = new lbt::Struct<Folder>("test::Folder", sizeof(Folder));
    BIND_COMMAND_CORE(st);
    BIND_STRUCT_METHOD(st, foldKey, MARGS());
    BIND_STRUCT_METHOD(st, foldFrom, MARGS("next"));
    return st;
  }
};

struct PrefixWorld {
  Setup s;
  const Path mesh0 = *parsePath("objects[0].data");
  const Path mesh1 = *parsePath("objects[1].data");

  PrefixWorld()
  {
    test_assert(addNativeCommand<Grow>(s.w.rt, CommandDef("test.grow")
                                                   .input(PropertyDef::intProp("times")))
                    .ok());
    test_assert(addNativeCommand<GrowTwice>(s.w.rt, CommandDef("test.growTwice")).ok());
    test_assert(addNativeCommand<Folder>(s.w.rt, CommandDef("test.folder")).ok());
  }
  double radius(int i)
  {
    return getValue(s.ctx, i ? "objects[1].data.radius" : "objects[0].data.radius")->asFloat();
  }
  Status at(const Path &prefix, const char *invocation)
  {
    PrefixScope scope(s.ctx, &prefix);
    return invoke(s.ctx, invocation).status();
  }
};

} // namespace

static void test_scope()
{
  PrefixWorld w;
  Context &ctx = w.s.ctx;
  {
    PrefixScope a(ctx, &w.mesh0);
    test_assert(ctx.prefix() && *ctx.prefix() == w.mesh0);
    {
      PrefixScope b(ctx, nullptr);
      test_assert(!ctx.prefix());
      PrefixScope c(ctx, &w.mesh1);
      test_assert(*ctx.prefix() == w.mesh1);
    }
    test_assert(*ctx.prefix() == w.mesh0);
    PrefixScope same(ctx, ctx.prefix()); /* its own */
    test_assert(*ctx.prefix() == w.mesh0);
  }
  test_assert(!ctx.prefix());
}

static void test_undo_redo()
{
  PrefixWorld w;
  Context &ctx = w.s.ctx;
  const double r0 = w.radius(0), r1 = w.radius(1);
  test_assert(!invoke(ctx, "test.grow()").ok()); /* canRun at the root */
  test_assert(w.at(w.mesh1, "test.grow()").ok() && !ctx.prefix());
  test_assert(w.radius(1) == r1 + 1 && w.radius(0) == r0);
  test_assert(w.s.ex.undo(ctx).ok() && w.radius(1) == r1);
  test_assert(w.s.ex.redo(ctx).ok() && w.radius(1) == r1 + 1 && !ctx.prefix());
  /* Whatever the prefix is by then. */
  ctx.setPrefix(w.mesh0);
  test_assert(w.s.ex.undo(ctx).ok() && w.s.ex.redo(ctx).ok());
  test_assert(*ctx.prefix() == w.mesh0);
  ctx.clearPrefix();
  test_assert(w.radius(1) == r1 + 1 && w.radius(0) == r0);
  /* A run without one is undone without one. */
  test_assert(invoke(ctx, "test.add(amount=2)").ok());
  const int64_t c = count(ctx);
  ctx.setPrefix(w.mesh0);
  test_assert(w.s.ex.undo(ctx).ok());
  ctx.clearPrefix();
  test_assert(count(ctx) == c - 2);
}

static void test_subs()
{
  PrefixWorld w;
  Context &ctx = w.s.ctx;
  const double r1 = w.radius(1);
  test_assert(w.at(w.mesh1, "test.growTwice()").ok());
  for (int i = 0; i < 2; i++) {
    test_assert(w.radius(1) == r1 + 1 && w.s.w.tool.strength == 0.125);
    test_assert(w.s.ex.undo(ctx).ok());
    test_assert(w.radius(1) == r1 && w.s.w.tool.strength == 0.5);
    test_assert(w.s.ex.redo(ctx).ok());
  }
}

static void test_rerun()
{
  PrefixWorld w;
  Context &ctx = w.s.ctx;
  const double r1 = w.radius(1);
  test_assert(w.at(w.mesh1, "test.grow()").ok());
  PropertyBag in = w.s.history.head()->inputs();
  test_assert(w.s.ex.rerunHead(ctx, in).ok() && w.radius(1) == r1 + 1 && !ctx.prefix());
  test_assert(w.s.ex.undo(ctx).ok() && w.radius(1) == r1);
}

static void test_fold()
{
  PrefixWorld w;
  Context &ctx = w.s.ctx;
  test_assert(w.at(w.mesh0, "test.folder()").ok() && !ctx.prefix());
  test_assert(w.at(w.mesh0, "test.folder()").ok() && w.s.history.size() == 1);
  /* Not with a run under another prefix, or none. */
  test_assert(w.at(w.mesh1, "test.folder()").ok() && w.s.history.size() == 2);
  test_assert(invoke(ctx, "test.folder()").ok() && w.s.history.size() == 3);
  test_assert(!ctx.prefix()); /* what exec set is gone */
  test_assert(invoke(ctx, "test.folder()").ok() && w.s.history.size() == 3);
  test_assert(w.at(w.mesh1, "test.folder()").ok() && w.s.history.size() == 4);
}

int main()
{
  test_scope();
  test_undo_redo();
  test_subs();
  test_rerun();
  test_fold();
  return test_end();
}
