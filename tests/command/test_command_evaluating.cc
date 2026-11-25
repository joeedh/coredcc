/* The executor while a graph evaluates (docs/plans/2026-11-24-p5-graph.md): every call is
 * Busy before any other check, and nothing changes. */
#include "command_fixture.h"

#include "coredcc/command/set_property.h"

test_init;

/* Opens an EvaluationScope in its exec, as a command that pulls a graph does, and tries
 * to run a sub-command from inside it. */
struct Evaluates {
  Status canRun(Context &) const
  {
    return Status();
  }
  Status exec(Context &ctx)
  {
    const CommandDef *add = ctx.runtime().ext<CommandRegistry>().find("test.add");
    {
      EvaluationScope scope(ctx.runtime());
      test_assert(ctx.executor()->runSub(ctx, *add, add->newInputs()).status().code() ==
                  StatusCode::Busy);
      test_assert(setValueUndo(ctx, "tool.strength", PropValue::ofFloat(1)).code() ==
                  StatusCode::Busy);
    }
    return ctx.executor()->runSub(ctx, *add, add->newInputs()).status();
  }
  void undo(Context &) {}
  uint64_t memSize() const
  {
    return 0;
  }
  void discard() {}
  static lbt::Struct<Evaluates> *defineBindings()
  {
    auto *st = new lbt::Struct<Evaluates>("test::Evaluates", sizeof(Evaluates));
    BIND_COMMAND_CORE(st);
    return st;
  }
};

static void test_run_undo_redo()
{
  Setup s;
  test_assert(s.runAdd(1).ok() && s.runAdd(2).ok());
  test_assert(s.ex.undo(s.ctx).ok());
  const int64_t before = count(s.ctx);
  {
    EvaluationScope scope(s.w.rt);
    test_assert(s.runAdd(5).status().code() == StatusCode::Busy);
    test_assert(s.ex.run(s.ctx, "test.add", s.add->newInputs()).status().code() ==
                StatusCode::Busy);
    test_assert(invoke(s.ctx, "test.add()").status().code() == StatusCode::Busy);
    test_assert(s.ex.undo(s.ctx).code() == StatusCode::Busy);
    test_assert(s.ex.redo(s.ctx).code() == StatusCode::Busy);
    test_assert(s.history.undo(s.ctx).code() == StatusCode::Busy);
    test_assert(s.history.redo(s.ctx).code() == StatusCode::Busy);
    test_assert(s.ex.beginInteractive(s.ctx, *s.drag, s.drag->newInputs()).code() ==
                StatusCode::Busy);
    test_assert(!s.ex.interactiveOpen());
  }
  test_assert(count(s.ctx) == before && s.history.size() == 2 && s.history.cursor() == 1);
  test_assert(s.ex.redo(s.ctx).ok() && s.ex.undo(s.ctx).ok() && count(s.ctx) == before);
}

static void test_rerun_head()
{
  Setup s;
  test_assert(s.runAdd(3).ok());
  const int64_t before = count(s.ctx);
  {
    EvaluationScope scope(s.w.rt);
    test_assert(s.ex.rerunHead(s.ctx, s.add->newInputs()).code() == StatusCode::Busy);
  }
  test_assert(count(s.ctx) == before && s.history.head() != nullptr && s.history.size() == 1);
  test_assert(s.ex.rerunHead(s.ctx, s.add->newInputs()).ok() && count(s.ctx) == before - 2);
}

/* undo and redo cancel an open interactive command: from a kernel they must not reach
 * the cancel, whose own undo would be refused after the record was taken. */
static void test_interactive_stays_open()
{
  Setup s;
  test_assert(s.ex.beginInteractive(s.ctx, *s.drag, s.drag->newInputs()).ok());
  test_assert(setValue(s.ctx, "tool.strength", PropValue::ofFloat(0.875)).ok());
  {
    EvaluationScope scope(s.w.rt);
    test_assert(s.ex.undo(s.ctx).code() == StatusCode::Busy);
    test_assert(s.ex.redo(s.ctx).code() == StatusCode::Busy);
    test_assert(s.ex.cancel(s.ctx).code() == StatusCode::Busy);
    test_assert(s.ex.commit(s.ctx).code() == StatusCode::Busy);
    test_assert(s.ex.interactiveOpen() && s.ex.interactiveCommand());
  }
  test_assert(s.w.tool.strength == 0.875 && !s.w.rt.hasStickyError());
  test_assert(s.w.lib.collect().status().code() == StatusCode::Busy); /* still held */
  test_assert(s.ex.undo(s.ctx).ok() && !s.ex.interactiveOpen() && s.w.tool.strength == 0.5);
  test_assert(s.w.lib.collect().ok());
}

/* Busy comes before the stray-error check, which would take the error. */
static void test_stray_error_is_kept()
{
  Setup s;
  test_assert(s.runAdd(1).ok());
  s.w.rt.setStickyError(Status::error(StatusCode::Internal, "stray"));
  {
    EvaluationScope scope(s.w.rt);
    test_assert(s.runAdd(1).status().code() == StatusCode::Busy);
    test_assert(s.ex.undo(s.ctx).code() == StatusCode::Busy);
    test_assert(s.ex.redo(s.ctx).code() == StatusCode::Busy);
    test_assert(s.ex.rerunHead(s.ctx, s.add->newInputs()).code() == StatusCode::Busy);
    test_assert(s.ex.beginInteractive(s.ctx, *s.drag, s.drag->newInputs()).code() ==
                StatusCode::Busy);
  }
  test_assert(hasMsg(s.w.rt.takeStickyError(), "stray"));

  test_assert(s.ex.beginInteractive(s.ctx, *s.drag, s.drag->newInputs()).ok());
  s.w.rt.setStickyError(Status::error(StatusCode::Internal, "live"));
  {
    EvaluationScope scope(s.w.rt);
    test_assert(s.ex.commit(s.ctx).code() == StatusCode::Busy && s.ex.interactiveOpen());
  }
  test_assert(hasMsg(s.w.rt.takeStickyError(), "live"));
  test_assert(s.ex.cancel(s.ctx).ok());
}

static void test_run_sub()
{
  Setup s;
  Result<const CommandDef *> def =
      addNativeCommand<Evaluates>(s.w.rt, CommandDef("test.evaluates"));
  test_assert(def.ok());
  const int64_t before = count(s.ctx);
  test_assert(s.ex.run(s.ctx, **def, (*def)->newInputs()).ok());
  test_assert(count(s.ctx) == before + 1 && s.history.at(0).subCount() == 1);
  test_assert(s.w.tool.strength == 0.5);
}

/* cancel with a Context bound to another executor is refused, and the command stays
 * open. It used to cancel through that Context. */
static void test_cancel_checks_its_context()
{
  Setup s;
  LinearHistory history2;
  CommandExecutor ex2(&s.w.rt, &history2);
  Context ctx2 = s.w.context();
  ctx2.setExecutor(&ex2);
  test_assert(s.ex.beginInteractive(s.ctx, *s.drag, s.drag->newInputs()).ok());
  test_assert(setValue(s.ctx, "tool.strength", PropValue::ofFloat(0.75)).ok());
  test_assert(s.ex.cancel(ctx2).code() == StatusCode::FailedPrecondition);
  test_assert(s.ex.undo(ctx2).code() == StatusCode::FailedPrecondition);
  test_assert(s.ex.interactiveOpen() && s.w.tool.strength == 0.75);
  test_assert(s.ex.cancel(s.ctx).ok() && s.w.tool.strength == 0.5);
}

int main()
{
  test_run_undo_redo();
  test_rerun_head();
  test_interactive_stays_open();
  test_stray_error_is_kept();
  test_run_sub();
  test_cancel_checks_its_context();
  return test_end();
}
