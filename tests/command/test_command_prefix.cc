/* A Context's path prefix goes with a run: its undo, its redo and its sub-commands see
 * it. */
#include "command_fixture.h"

#include "coredcc/api/change_bus.h"

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
  uint64_t byteSize() const
  {
    return 0;
  }
  void discard()
  {
  }
  static lbt::Struct<Grow> *defineBindings()
  {
    auto *st = new lbt::Struct<Grow>("test::Grow", sizeof(Grow));
    BIND_COMMAND_CORE(st);
    return st;
  }
};

/* Grow with a redo slot, for interactive runs. */
struct GrowLive : Grow {
  double after = 0;
  Status redo(Context &ctx)
  {
    return setValue(ctx, "radius", PropValue::ofFloat(after));
  }
  static lbt::Struct<GrowLive> *defineBindings()
  {
    auto *st = new lbt::Struct<GrowLive>("test::GrowLive", sizeof(GrowLive));
    BIND_COMMAND_CORE(st);
    BIND_STRUCT_METHOD(st, redo, MARGS("ctx"));
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
  void undo(Context &)
  {
  }
  uint64_t byteSize() const
  {
    return 0;
  }
  void discard()
  {
  }
  static lbt::Struct<GrowTwice> *defineBindings()
  {
    auto *st = new lbt::Struct<GrowTwice>("test::GrowTwice", sizeof(GrowTwice));
    BIND_COMMAND_CORE(st);
    return st;
  }
};

/* Needs its prefix to name a struct in every slot. */
struct Named {
  Status canRun(Context &ctx) const
  {
    return resolve(ctx, Path()).status();
  }
  Status exec(Context &ctx)
  {
    return resolve(ctx, Path()).status();
  }
  void undo(Context &ctx)
  {
    test_assert(resolve(ctx, Path()).ok());
  }
  uint64_t byteSize() const
  {
    return 0;
  }
  void discard()
  {
  }
  static lbt::Struct<Named> *defineBindings()
  {
    auto *st = new lbt::Struct<Named>("test::Named", sizeof(Named));
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
  void undo(Context &)
  {
  }
  uint64_t byteSize() const
  {
    return 0;
  }
  void discard()
  {
  }
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
    test_assert(addNativeCommand<Grow>(
                    s.w.rt, CommandDef("test.grow").input(PropertyDef::intProp("times")))
                    .ok());
    test_assert(addNativeCommand<GrowLive>(
                    s.w.rt, CommandDef("test.growLive").flags(CommandFlags::Interactive))
                    .ok());
    test_assert(addNativeCommand<GrowTwice>(s.w.rt, CommandDef("test.growTwice")).ok());
    test_assert(addNativeCommand<Named>(s.w.rt, CommandDef("test.name")).ok());
    test_assert(addNativeCommand<Folder>(s.w.rt, CommandDef("test.folder")).ok());
  }
  double radius(int i)
  {
    return getValue(s.ctx, i ? "objects[1].data.radius" : "objects[0].data.radius")
        ->asFloat();
  }
  double radiusOf(const StableRef &mesh)
  {
    StableRef r = mesh;
    r.key.append(KeySegment::member("radius"));
    r.type = StructId();
    return getValue(s.ctx, *resolve(s.ctx, r))->asFloat();
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
    const StableRef mesh = *canonicalize(ctx, Path());
    const StableRef none;
    PrefixScope address(ctx, &mesh);
    test_assert(ctx.hasPrefix() && !ctx.prefix() && *ctx.prefixAddress() == mesh);
    test_assert(getValue(ctx, "radius")->asFloat() == w.radiusOf(mesh));
    test_assert(*canonicalize(ctx, Path()) == mesh);
    PrefixScope nothing(ctx, &none);
    test_assert(!getValue(ctx, "radius").ok());
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

/* A path that starts with `/` doesn't use the prefix. */
static void test_from_root()
{
  PrefixWorld w;
  Context &ctx = w.s.ctx;
  PrefixScope scope(ctx, &w.mesh1);
  test_assert(getValue(ctx, "/objects[1].data.radius")->asFloat() ==
              getValue(ctx, "radius")->asFloat());
  test_assert(!getValue(ctx, "objects[1].data.radius").ok() &&
              !getValue(ctx, "/radius").ok());
  test_assert(resolve(ctx, "/")->ptr.data == ctx.root().data);
  test_assert(setValueUndo(ctx, "/tool.strength", PropValue::ofFloat(0.25)).ok());
  test_assert(w.s.w.tool.strength == 0.25 && w.s.ex.undo(ctx).ok() &&
              w.s.w.tool.strength == 0.5);
  test_assert(w.s.ex.redo(ctx).ok() && w.s.w.tool.strength == 0.25);
  /* A prefix can be one, too. */
  const Path path = *parsePath("/objects[0].data");
  PrefixScope other(ctx, &path);
  test_assert(getValue(ctx, "radius").ok());
}

/* A watcher keeps the prefix it was made with. */
static void test_watcher_prefix()
{
  PrefixWorld w;
  Context &ctx = w.s.ctx;
  ChangeBus &bus = w.s.w.rt.ext<ChangeBus>();
  int under = 0, root = 0;
  ScopedConnection a, b;
  {
    PrefixScope scope(ctx, &w.mesh1);
    a = bus.watch(ctx,
                  "radius",
                  [&](const PropValue &, const Status &s) { under += s.ok(); })
            .value();
  }
  b = bus.watch(ctx,
                "tool.count",
                [&](const PropValue &, const Status &s) { root += s.ok(); })
          .value();
  test_assert(setValue(ctx, "objects[1].data.radius", PropValue::ofFloat(7)).ok() &&
              bus.flush() == 1);
  test_assert(setValue(ctx, "tool.count", PropValue::ofInt(77)).ok());
  PrefixScope scope(ctx, &w.mesh0);
  test_assert(bus.flush() == 1 && under == 1 && root == 1 && *ctx.prefix() == w.mesh0);
}

/* What a prefix names may change outside the history: undo, redo and rerun are where the
 * run was. */
static void test_repointed()
{
  PrefixWorld w;
  Context &ctx = w.s.ctx;
  const Path active = *parsePath("objects.active.data");
  w.s.w.scene.active = 0;
  const double r0 = w.radius(0), r1 = w.radius(1);
  test_assert(w.at(active, "test.grow()").ok() && w.radius(0) == r0 + 1);
  w.s.w.scene.active = 1;
  test_assert(w.s.ex.undo(ctx).ok() && w.radius(0) == r0 && w.radius(1) == r1);
  test_assert(w.s.ex.redo(ctx).ok() && w.radius(0) == r0 + 1 && w.radius(1) == r1);
  PropertyBag in = w.s.history.head() ? w.s.history.head()->inputs() : PropertyBag();
  test_assert(w.at(active, "test.growTwice()").ok() && w.radius(1) == r1 + 1);
  w.s.w.scene.active = 0;
  in = w.s.history.head()->inputs();
  test_assert(w.s.ex.rerunHead(ctx, in).ok() && w.radius(1) == r1 + 1 &&
              w.radius(0) == r0 + 1);
  test_assert(w.s.ex.undo(ctx).ok() && w.radius(1) == r1 && w.radius(0) == r0 + 1);
  w.s.w.scene.active = -1; /* nor need it name anything */
  test_assert(w.s.ex.redo(ctx).ok() && w.radius(1) == r1 + 1);
  test_assert(w.s.ex.undo(ctx).ok() && w.s.ex.undo(ctx).ok() && w.radius(0) == r0);

  /* Nor be named as it was. */
  w.s.w.scene.active = 0;
  Library &lib = w.s.w.lib;
  const std::string name = lib.get(w.s.w.scene.objects[0])->name().c_str();
  test_assert(w.at(*parsePath("objects[\"" + name + "\"]"), "test.name()").ok());
  test_assert(lib.rename(w.s.w.scene.objects[0], "another").ok());
  test_assert(w.s.ex.undo(ctx).ok() && w.s.ex.redo(ctx).ok());

  /* A prefix inside its owner, by its key. */
  test_assert(w.at(*parsePath("tool.activeBrush"), "test.name()").ok());
  const StableRef *at = w.s.history.head()->prefix();
  test_assert(at && at->key.size() > 0 &&
              *at == *canonicalize(ctx, *parsePath("tool.brushes[0]")));

  /* One that names nothing. */
  w.s.w.scene.active = -1;
  const int size = w.s.history.size();
  const Status none = w.at(active, "test.name()");
  test_assert(none.code() == StatusCode::NotFound &&
              strstr(none.message(), "the path prefix: "));
  test_assert(w.s.history.size() == size && !ctx.hasPrefix());
  /* setValueUndo's own command too, once. */
  const CommandDef &add = *w.s.add;
  Result<Owned<Command>> cmd = add.instantiate(w.s.w.rt);
  PrefixScope scope(ctx, &active);
  g_log.clear();
  test_assert(
      !w.s.ex.runInstance(ctx, add, std::move(cmd).value(), add.newInputs()).ok());
  test_assert(g_log == "discard;");
}

static void test_fold_repointed()
{
  PrefixWorld w;
  const Path active = *parsePath("objects.active.data");
  w.s.w.scene.active = 0;
  test_assert(w.at(active, "test.folder()").ok());
  w.s.w.scene.active = 1; /* the same path, another object */
  test_assert(w.at(active, "test.folder()").ok() && w.s.history.size() == 2);
  test_assert(w.at(w.mesh1, "test.folder()").ok() &&
              w.s.history.size() == 2); /* another, the same */
}

/* The live phase of an interactive command is under its prefix. */
static void test_interactive()
{
  PrefixWorld w;
  Context &ctx = w.s.ctx;
  const CommandDef *def = w.s.w.rt.ext<CommandRegistry>().find("test.growLive");
  auto update = [](Context &c, Command &cmd) {
    test_assert(c.inputs() && c.outputs());
    cmd.as<GrowLive>()->after = 9;
    return setValue(c, "radius", PropValue::ofFloat(9));
  };
  test_assert(w.s.ex.updateInteractive(ctx, update).code() ==
              StatusCode::FailedPrecondition);
  const double r1 = w.radius(1);
  for (bool commit : {false, true}) {
    {
      PrefixScope scope(ctx, &w.mesh1);
      test_assert(w.s.ex.beginInteractive(ctx, *def, def->newInputs()).ok());
    }
    test_assert(!getValue(ctx, "radius").ok());
    test_assert(w.s.ex.updateInteractive(ctx, update).ok() && w.radius(1) == 9);
    test_assert(!ctx.hasPrefix() && !ctx.inputs());
    /* Commands, and the open one's end, aren't for it. */
    const Status nested = w.s.ex.updateInteractive(ctx, [&](Context &c, Command &) {
      test_assert(w.s.ex.updateInteractive(c, update).code() ==
                  StatusCode::FailedPrecondition);
      test_assert(!w.s.ex.commit(c).ok() && !w.s.ex.undo(c).ok());
      return w.s.ex.cancel(c);
    });
    test_assert(nested.code() == StatusCode::FailedPrecondition &&
                w.s.ex.interactiveOpen());
    test_assert((commit ? w.s.ex.commit(ctx) : w.s.ex.cancel(ctx)).ok());
    test_assert(w.radius(1) == (commit ? 9 : r1));
  }
  test_assert(w.s.ex.undo(ctx).ok() && w.radius(1) == r1);
  test_assert(w.s.ex.redo(ctx).ok() && w.radius(1) == 9);
}

int main()
{
  test_scope();
  test_undo_redo();
  test_subs();
  test_rerun();
  test_fold();
  test_from_root();
  test_watcher_prefix();
  test_repointed();
  test_fold_repointed();
  test_interactive();
  return test_end();
}
