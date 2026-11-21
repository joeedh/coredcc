/* The executor and LinearHistory: undo/redo, sub-commands, folding, the budget,
 * rerunHead and interactive commands. */
#include "command_fixture.h"

#include <cmath>
#include <string>

test_init;

static void test_undo_redo()
{
  Setup s;
  test_assert(s.w.tool.count == 4);
  Result<PropertyBag> out = s.runAdd(2);
  test_assert(out.ok() && *out->get("result") == PropValue::ofInt(6));
  test_assert(s.runAdd(3).ok() && s.w.tool.count == 9);
  test_assert(s.history.size() == 2 && s.history.cursor() == 2);

  test_assert(s.ex.undo(s.ctx).ok() && s.w.tool.count == 6);
  test_assert(s.ex.undo(s.ctx).ok() && s.w.tool.count == 4);
  test_assert(s.ex.undo(s.ctx).code() == StatusCode::FailedPrecondition);
  test_assert(s.ex.redo(s.ctx).ok() && s.w.tool.count == 6); /* re-exec: no redo slot */
  /* A new command truncates the redo side, discarding it. */
  g_log.clear();
  test_assert(s.runAdd(1).ok() && s.w.tool.count == 7);
  test_assert(g_log == "exec1;discard;"); /* truncated when the new one is pushed */
  test_assert(s.history.size() == 2 && !s.history.canRedo());
  test_assert(s.ex.redo(s.ctx).code() == StatusCode::FailedPrecondition);

  /* canRun refuses; nothing runs or reaches the history. */
  Result<PropertyBag> r = s.ex.run(s.ctx, *s.refuse, s.refuse->newInputs());
  test_assert(r.status().code() == StatusCode::FailedPrecondition);
  test_assert(g_log.find("refuse-exec") == std::string::npos && s.history.size() == 2);
  test_assert(s.ex.run(s.ctx, "test.ad", PropertyBag()).status().suggestions().size() == 1);
  test_assert(s.history.validate().ok() && s.w.rt.validateAll().ok());
}

static void test_sub_commands()
{
  Setup s;
  /* Sub-commands are undone in reverse before the parent's own undo. */
  test_assert(s.ex.run(s.ctx, *s.parent, s.parent->newInputs()).ok());
  test_assert(s.w.tool.count == 15 && s.history.size() == 1 && s.history.at(0).subCount() == 2);
  g_log.clear();
  test_assert(s.ex.undo(s.ctx).ok() && s.w.tool.count == 4);
  test_assert(g_log == "undo;undo;parent-undo;");
  /* Redo without a redo slot drops the old subs and runs exec, which makes new ones. */
  g_log.clear();
  test_assert(s.ex.redo(s.ctx).ok() && s.w.tool.count == 15);
  test_assert(g_log == "discard;discard;exec1;exec10;" && s.history.at(0).subCount() == 2);

  /* A failed exec undoes its sub-commands, and nothing reaches the history. */
  for (int64_t mode : {1, 2}) {
    PropertyBag in = s.parent->newInputs();
    test_assert(in.set("fail", PropValue::ofInt(mode)).ok());
    g_log.clear();
    Result<PropertyBag> r = s.ex.run(s.ctx, *s.parent, std::move(in));
    test_assert(!r.ok() && s.w.tool.count == 15 && s.history.size() == 1);
    test_assert(g_log == "exec1;exec10;undo;discard;undo;discard;");
    test_assert(!s.w.rt.hasStickyError());
  }
  /* runSub outside an exec, and run inside one, are refused. */
  test_assert(s.ex.runSub(s.ctx, *s.add, s.add->newInputs()).status().code() ==
              StatusCode::FailedPrecondition);
}

static void test_folding_and_barriers()
{
  Setup s;
  test_assert(s.runFold(7).ok() && s.runFold(7).ok() && s.runFold(7).ok());
  test_assert(s.history.size() == 1 && s.w.tool.count == 7);
  test_assert(s.runFold(8).ok() && s.history.size() == 2); /* another key */
  test_assert(s.runFold(0).ok() && s.runFold(0).ok() && s.history.size() == 4); /* 0 never folds */
  /* Nothing folds into a head that was undone and redone. */
  test_assert(s.runFold(9).ok() && s.history.size() == 5);
  test_assert(s.ex.undo(s.ctx).ok() && s.ex.redo(s.ctx).ok());
  test_assert(s.runFold(9).ok() && s.history.size() == 6);
  /* One undo reverses the folded run. */
  while (s.history.canUndo()) {
    test_assert(s.ex.undo(s.ctx).ok());
  }
  test_assert(s.w.tool.count == 4);

  /* A barrier stops undo. */
  test_assert(s.ex.redo(s.ctx).ok());
  test_assert(s.runAdd(1).ok());
  s.history.pushBarrier();
  test_assert(!s.history.canUndo() && s.ex.undo(s.ctx).code() == StatusCode::FailedPrecondition);
  test_assert(s.runAdd(1).ok() && s.ex.undo(s.ctx).ok() && !s.history.canUndo());
  test_assert(s.history.validate().ok());
}

static void test_memory_budget()
{
  Setup s;
  s.history.setMemoryBudget(250);
  test_assert(s.runAdd(1, 100).ok() && s.runAdd(1, 100).ok());
  g_log.clear();
  test_assert(s.runAdd(1, 100).ok());
  test_assert(s.history.size() == 2 && s.history.memoryUsed() == 200);
  test_assert(g_log == "exec1;discard;"); /* the oldest went */
  /* The newest always stays, even over budget. */
  test_assert(s.runAdd(1, 1000).ok() && s.history.size() == 1 && s.history.canUndo());
  test_assert(s.history.validate().ok());
}

static void test_rerun_head()
{
  Setup s;
  test_assert(s.runAdd(2).ok() && s.w.tool.count == 6);
  PropertyBag in = s.add->newInputs();
  test_assert(in.set("amount", PropValue::ofInt(5)).ok());
  test_assert(s.ex.rerunHead(s.ctx, std::move(in)).ok());
  test_assert(s.w.tool.count == 9 && s.history.size() == 1);
  /* New inputs that fail: the old ones run again and the error comes back. */
  in = s.add->newInputs();
  test_assert(in.set("amount", PropValue::ofInt(-1)).ok());
  Status st = s.ex.rerunHead(s.ctx, std::move(in));
  test_assert(hasMsg(st, "negative amount") && s.w.tool.count == 9 && s.history.size() == 1);
  test_assert(s.ex.undo(s.ctx).ok() && s.w.tool.count == 4);
  /* Nothing to re-run once undone. */
  test_assert(s.ex.rerunHead(s.ctx, s.add->newInputs()).code() == StatusCode::FailedPrecondition);
}

static void test_interactive()
{
  Setup s;
  test_assert(s.ex.beginInteractive(s.ctx, *s.drag, s.drag->newInputs()).ok());
  test_assert(s.ex.interactiveOpen() && s.ex.interactiveCommand());
  test_assert(setValue(s.ctx, "tool.strength", PropValue::ofFloat(0.9)).ok()); /* the tool */

  /* Other commands are Busy, and collection too. */
  test_assert(s.runAdd(1).status().code() == StatusCode::Busy);
  test_assert(setValueUndo(s.ctx, "tool.count", PropValue::ofInt(1)).code() == StatusCode::Busy);
  test_assert(s.w.lib.collect().status().code() == StatusCode::Busy);
  test_assert(s.ex.beginInteractive(s.ctx, *s.drag, s.drag->newInputs()).code() ==
              StatusCode::Busy);
  test_assert(s.ex.run(s.ctx, *s.drag, s.drag->newInputs()).status().code() == StatusCode::Busy);

  /* Undo while one is open only cancels it. */
  test_assert(s.runAdd(0).status().code() == StatusCode::Busy);
  test_assert(s.ex.undo(s.ctx).ok() && !s.ex.interactiveOpen());
  test_assert(s.w.tool.strength == 0.5 && s.history.size() == 0);
  test_assert(s.w.lib.collect().ok());

  /* Commit, then undo and redo. */
  test_assert(s.runAdd(1).ok());
  test_assert(s.ex.beginInteractive(s.ctx, *s.drag, s.drag->newInputs()).ok());
  test_assert(setValue(s.ctx, "tool.strength", PropValue::ofFloat(0.8)).ok());
  test_assert(s.ex.commit(s.ctx).ok() && s.history.size() == 2);
  test_assert(s.ex.undo(s.ctx).ok() && s.w.tool.strength == 0.5 && s.w.tool.count == 5);
  test_assert(s.ex.redo(s.ctx).ok() && s.w.tool.strength == 0.8);
  test_assert(s.ex.commit(s.ctx).code() == StatusCode::FailedPrecondition);
  /* Interactive defs need beginInteractive, and plain ones can't use it. */
  test_assert(s.ex.run(s.ctx, *s.drag, s.drag->newInputs()).status().code() ==
              StatusCode::FailedPrecondition);
  test_assert(s.ex.beginInteractive(s.ctx, *s.add, s.add->newInputs()).code() ==
              StatusCode::FailedPrecondition);
}

int main()
{
  test_undo_redo();
  test_sub_commands();
  test_folding_and_barriers();
  test_memory_budget();
  test_rerun_head();
  test_interactive();
  return test_end();
}
