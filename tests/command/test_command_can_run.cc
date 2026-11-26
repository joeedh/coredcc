/* Asking whether a command would run, without running it, and tools at a prefix. */
#include "command_fixture.h"

#include "coredcc/api/ref_text.h"
#include "coredcc/command/last_used.h"

#include <string>

test_init;

/* Runs if the Context's prefix has a radius that isn't negative, and `least` is at most
 * it. */
struct NeedsRadius {
  Status canRun(Context &ctx) const
  {
    Result<PropValue> r = getValue(ctx, "radius");
    if (!r.ok()) {
      return Status::error(StatusCode::FailedPrecondition, "no radius");
    }
    if (r->asFloat() < ctx.inputs()->get("least")->asFloat()) {
      return Status::error(StatusCode::FailedPrecondition, "too small");
    }
    if (ctx.inputs()->get("sticky")->asBool()) {
      ctx.runtime().setStickyError(Status::error(StatusCode::Internal, "from canRun"));
    }
    return Status();
  }
  Status exec(Context &ctx)
  {
    g_log += "exec;";
    return setValue(
        ctx, "radius", PropValue::ofFloat(getValue(ctx, "radius")->asFloat() + 1));
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
    g_log += "discard;";
  }
  static lbt::Struct<NeedsRadius> *defineBindings()
  {
    auto *st = new lbt::Struct<NeedsRadius>("test::NeedsRadius", sizeof(NeedsRadius));
    BIND_COMMAND_CORE(st);
    return st;
  }
};

struct CanWorld {
  Setup s;
  const CommandDef *def = nullptr;
  CanWorld()
  {
    def = *addNativeCommand<NeedsRadius>(s.w.rt,
                                         CommandDef("test.needs")
                                             .input(PropertyDef::floatProp("least"))
                                             .input(PropertyDef::boolProp("sticky")));
    g_log.clear();
  }
};

static const Path kMesh0 = *parsePath("/objects[0].data");

static void test_answers()
{
  CanWorld w;
  Context &ctx = w.s.ctx;
  const double before = w.s.w.mesh(0).radius;
  test_assert(canInvokeAt(ctx, kMesh0, "test.needs").ok() && g_log == "discard;");
  test_assert(!ctx.hasPrefix() && w.s.history.size() == 0 &&
              w.s.w.mesh(0).radius == before);
  g_log.clear();
  test_assert(hasMsg(canInvoke(ctx, "test.needs"), "no radius") && g_log == "discard;");
  test_assert(hasMsg(canInvokeAt(ctx, Path(), "test.needs"), "no radius"));
  test_assert(hasMsg(canInvokeAt(ctx, kMesh0, "test.needs(least=1e9)"), "too small"));
  /* The last-used inputs count, and asking doesn't change them. */
  w.s.w.rt.ext<LastUsedStore>().set(*w.def, "least", PropValue::ofFloat(1e9)).ignore();
  test_assert(hasMsg(canInvokeAt(ctx, kMesh0, "test.needs"), "too small"));
  test_assert(canInvokeAt(ctx, kMesh0, "test.needs(least=0)").ok());
  test_assert(hasMsg(canInvokeAt(ctx, kMesh0, "test.needs"), "too small"));
  /* What else a run refuses. */
  test_assert(canInvoke(ctx, "test.nope").code() == StatusCode::NotFound);
  test_assert(canInvoke(ctx, "test.add(amuont=1)").suggestions().size() > 0);
  test_assert(!canInvoke(ctx, "test.add(amount=x)").ok() &&
              !canInvoke(ctx, "test.").ok());
  test_assert(hasMsg(canInvoke(ctx, "test.refuse"), "nothing selected"));
  test_assert(hasMsg(canInvokeAt(ctx, *parsePath("objects[99]"), "test.add"),
                     "the path prefix: "));

  Context bare(&w.s.w.rt, &w.s.w.lib, ctx.root());
  test_assert(hasMsg(canInvoke(bare, "test.add"), "no executor"));
  test_assert(w.s.ex.canRun(bare, *w.s.add, w.s.add->newInputs()).ok() &&
              !bare.executor());
  LinearHistory history2;
  CommandExecutor ex2(&w.s.w.rt, &history2);
  test_assert(
      hasMsg(ex2.canRun(ctx, *w.s.add, w.s.add->newInputs()), "another executor"));
  /* An Interactive one can be begun. */
  test_assert(canInvoke(ctx, "test.drag").ok() && !w.s.ex.interactiveOpen());
}

static void test_states()
{
  CanWorld w;
  Context &ctx = w.s.ctx;
  /* What canRun leaves behind is its answer, and is collected. */
  test_assert(hasMsg(canInvokeAt(ctx, kMesh0, "test.needs(sticky=true)"), "from canRun"));
  test_assert(!w.s.w.rt.hasStickyError());
  /* A stray error is the next command's to report. */
  w.s.w.rt.setStickyError(Status::error(StatusCode::Internal, "stray"));
  test_assert(hasMsg(canInvoke(ctx, "test.add"), "never collected") &&
              w.s.w.rt.hasStickyError());
  test_assert(hasMsg(w.s.runAdd(1).status(), "stray") && canInvoke(ctx, "test.add").ok());
  {
    EvaluationScope scope(w.s.w.rt);
    test_assert(canInvoke(ctx, "test.add").code() == StatusCode::Busy);
  }
  test_assert(w.s.ex.interactiveDef() == nullptr);
  test_assert(w.s.ex.beginInteractive(ctx, *w.s.drag, w.s.drag->newInputs()).ok());
  test_assert(w.s.ex.interactiveDef() == w.s.drag);
  test_assert(canInvoke(ctx, "test.add").code() == StatusCode::Busy);
  auto inside = [&](Context &c, Command &) { return canInvoke(c, "test.add"); };
  test_assert(w.s.ex.updateInteractive(ctx, inside).code() ==
              StatusCode::FailedPrecondition);
  test_assert(w.s.ex.cancel(ctx).ok() && w.s.ex.interactiveDef() == nullptr);
  test_assert(canInvoke(ctx, "test.add").ok());
}

static void test_invoke_at()
{
  CanWorld w;
  Context &ctx = w.s.ctx;
  const double before = w.s.w.mesh(0).radius;
  const Path other = *parsePath("tool");
  PrefixScope scope(ctx, &other);
  test_assert(invokeAt(ctx, kMesh0, "test.needs").ok() &&
              w.s.w.mesh(0).radius == before + 1);
  test_assert(ctx.prefix() && *ctx.prefix() == other);
  test_assert(!invokeAt(ctx, Path(), "test.needs").ok() &&
              !invokeAt(ctx, Path::root(), "test.needs").ok());
  test_assert(invokeAt(ctx, Path::root(), "test.add(amount=2)").ok());
  test_assert(invokeAt(ctx, kMesh0, "nope").status().code() == StatusCode::NotFound);
}

int main()
{
  test_answers();
  test_states();
  test_invoke_at();
  return test_end();
}
