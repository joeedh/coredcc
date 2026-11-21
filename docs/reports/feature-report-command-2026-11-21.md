# Feature report: commands and undo (`command/`), 2026-11-21

- **Reviewed:** the API surface of `command/` (P1e) at `8e91f8a`, against architecture
  §5, §6 and §8a, and the P1 plan's P1e section and implementation notes.
- **Reviewer:** a fresh agent given the code and the spec, not the author's reasoning.
  It confirmed each bug with a scratch program, built from an exported copy of HEAD.
- **This file:** a rewrite of its review. Each finding ends with its resolution. The
  regression tests are in `tests/command/test_command_review.cc`.

## Summary
- **What held up:**
  - sub-command grouping and rollback on a failed exec;
  - the fold rules;
  - barriers and the budget's "newest always stays";
  - the interactive protocol's Busy and cancel rules;
  - LastUsed round trips and the `commandDefaults` datapath.
- **What didn't:**
  - The executor guarded only `exec` against reentrancy.
  - It trusted every input bag.
  - It collected sticky errors in one place only.
  - Three paths (rerunHead, failed redo, folding) could leave the document holding
    edits that no history entry owned.
  - The history pins of §8a didn't exist, so a purge could free what undo needed.

## Bugs
1. **B1, a slot that called `run()` from undo or redo corrupted the history.** The undo
   case freed the record whose undo was on the stack. The redo case left
   `cursor > size`.
   - *Fixed:*
     - The executor counts slot calls other than exec (`slotDepth_`). An exec frame
       resets the count, so redo by re-exec can still call `runSub`.
     - run, undo, redo, rerunHead, the interactive calls and runSub-outside-exec are
       refused while a slot runs.
     - LinearHistory refuses nested undo and redo (Busy), and aborts on a nested push
       or clear.
   - The P1e note "`run` from inside a slot is refused" now holds for every slot.
2. **B2, `run()` never checked the bag against the def.** A missing input crashed
   `core.block.delete`.
   - *Fixed:* `start` builds `def.newInputs()` and copies in the caller's set values.
     Unknown names and values that don't coerce are InvalidArgument. An empty bag
     therefore gets the defaults: the delete's null block is FailedPrecondition, not a
     crash.
3. **B3, `rerunHead` on a setValueUndo or interactive head undid it and threw it away.**
   - *Fixed, part 1:* rerunHead refuses before undoing anything when the def is
     Interactive, or when the record came from `runInstance`
     (`CommandRecord::fromFactory`).
   - *Fixed, part 2:* when the new run fails, the old record is redone and pushed back.
     It used to re-run the old inputs.
   - *Kept:* `core.setProperty` holds its path and value in the impl, not in def inputs
     (§5 says inputs). A PropValue of any kind would need an "any" PropKind. The
     refusal makes the deviation harmless, and it is now recorded in the plan.
4. **B4, a failed redo left partial state applied.**
   - *Fixed:* redo by re-exec rolls back the sub-commands it made. The redo-slot path
     undoes the sub-commands it already redid, in reverse. LinearHistory then drops a
     record that is fully undone.
5. **B5, no history pins.** A purge freed the old target of a pointer set, and a
   created block.
   - *Fixed:*
     - SetProperty pins the old and new Pointer targets.
     - `CreatedBlocks::add` now takes the Library, and pins each block.
     - Both unpin in `discard`; a fold moves the folded record's pins into the head.
6. **B6, `deletePreview` counted users that weren't Live, and compared ids without
   canonicalizing them.**
   - *Fixed:* InHistory, Detached and Placeholder users are skipped, and targets are
     compared through `ClientRegistry::canonical`.
   - Detached users are guarded by `destroyFromHistory`'s refusal instead.
7. **B7, redoing a delete reused the old cascade.** A Strong user added since was left
   dangling. `discard` also ignored `destroyFromHistory` failures.
   - *Fixed:* redo recomputes the preview.
     - With `cascade`, it deletes the new users too, and records them.
     - Without it, the redo fails and is dropped.
     - A failure part-way restores what it deleted.
   - Discard failures other than NotFound become the sticky error, for `CreatedBlocks`
     too.
8. **B8, sticky errors leaked into the next command.**
   - *Fixed:*
     - The executor collects them after every slot call it makes: canRun, the factory,
       exec, rollback, push, undo, redo and cancel. Each is returned from that call,
       combined with the slot's own error.
     - `commit` with a pending error cancels the command and returns the error.
     - An upkeep failure during push ("ran and was recorded, but the history's upkeep
       failed") belongs to the pushing command.
     - One pending before a top-level call came from outside the executor. The call
       takes it and fails with FailedPrecondition, naming it, so nothing is blamed
       silently and nothing is lost.
9. **B9, folding dropped the folded record's sub-commands.** *Fixed:* records with
   sub-commands never fold.
10. **B10, destroying the executor with an interactive command open blocked collection
    for good.** A Context also kept a dangling executor pointer.
    - *Fixed:*
      - The destructor cancels the command through the Context and Library saved at
        `beginInteractive`.
      - A Context with no executor is bound only for the length of each call.
      - A Context bound to another executor is FailedPrecondition (it was an abort).
      - `invoke` and `setValueUndo` need an explicit `ctx.setExecutor`.
11. **B11, `-inf` was written but couldn't be read, and one bad line stopped
    `LastUsedStore::load`.**
    - *Fixed:*
      - The invocation lexer reads a signed identifier (`-inf`), and literals accept
        it, in tuples too.
      - `load` skips a bad line, keeps loading, and returns the first error.
      - Integer and float literals out of range are errors, not clamps.
12. **B12, a Flags round trip could add bits.** *Fixed:* an item is emitted only when all
    of its bits are set.
13. **B13, `foldKey` without `foldFrom` was accepted.** *Fixed:* registration requires
    both or neither.
14. **B14, a failed registration left the impl name taken.**
    - *Fixed:* `addNativeCommand` runs `CommandRegistry::checkDef` (the id and bag
      checks) before `registerImpl`.
    - Checks on the implementation's slots still run after it. They are code errors,
      not retries.
15. **B15, a folded head kept the first run's inputs.** *Fixed:* `CommandRecord::adoptBags`
    swaps the folded run's inputs and outputs into the head.
16. **B16, a canRun refusal skipped `discard`.** *Fixed:* every instance the executor makes
    or takes is discarded exactly once, including after a refused canRun or a rejected
    bag.

## Design
1. **Interactive commands can't be scripted, re-run, or record their final values.**
   *Deferred to P3*, with the tools. `runSub` of an Interactive def is in the same
   bucket.
2. **UndoHistory is awkward for host adapters.** These are `undoRecord`/`redoRecord`
   without the executor, labels, change notification, and interactive hooks.
   *Deferred to P6*, with the first host adapter.
3. **The P1f binding surface.** These are `std::string_view` and `std::string`,
   `RecordPtr`, `util::function` factories, name-sharing overloads, `PropertyBag`, and a
   foreign `self` for `foldFrom`'s `next.as<T>()`. *Deferred to P1f:* the `bind/`
   facades.
4. **`memSize` was quadratic.** *Fixed:* each record's size is read once at push (and
   again after a fold), and the history keeps a running total that `validate` checks.
   1000 pushes now make 1000 calls.
5. **Sub-commands undo before the parent's own work.** *Documented* on
   `CommandRecord`: put dependent work in a sub-command too.
6. **`discard` has no Context.** *Documented* on `UndoHistory`: destroy or clear the
   history before the Library.
7. **`setValueUndo` couldn't be used inside a command.** *Fixed:* inside exec it runs
   as a sub-command.
8. **Snapshot undo has nothing behind it.** *Deferred to P2*, with library IO, which
   decides between before and after blobs.
9. **Unsafe defaults:**
   - *Fixed:* the other-executor abort; SetProperty's `memSize` now counts strings.
   - *Deferred to P1f:* `CommandRegistry` removal (TS hot reload) and a consistent
     `runSub` argument order.
   - *Deferred to P2:* CreatedBlocks recording `BlockId`s for reloads.

## Test gaps
- *Added:* every bug above, plus:
  - bags that don't match the def;
  - rerunHead on set-property and interactive heads, and a failed re-run restoring the
    old record;
  - fold with sub-commands and fold inputs;
  - sticky errors in exec, commit, push, and stray ones;
  - destroying the executor while interactive, and another executor's Context;
  - delete with an InHistory user, and redo after the graph changed (both cascade
    settings);
  - `-inf`, multi-bit flags, and overflowing literals;
  - a bad LastUsed line;
  - registration retry, memSize call counts, and setValueUndo inside exec.
- *Added after the fixes:* nested `runSub` (undo order, redo by re-exec, and rollback at
  depth), budget eviction across a barrier, and "discarded exactly once" on every path
  (refusal, failed exec, fold, truncation, rerunHead both ways, budget, destruction).
- *Not added (tracked for the P1f test pass):*
  - slot (element-level) refs in the cascade;
  - Detached and Placeholder users in the preview.
- `test_command.cc` was split into `test_command_history`, `_values` and `_blocks`,
  sharing `command_fixture.h`.

## Spec deviations
- **Now recorded in the plan:**
  - SetProperty's state lives in the impl, and rerunHead refuses it;
  - rerunHead is on the executor;
  - `tryFold` is merged into `push`;
  - CreatedBlocks holds handles until P2.
- **The reviewer judged these recorded deviations justified:**
  - canRun returning Status;
  - the `uint64` foldKey;
  - `restoreFromHistory` for create redo;
  - attach and detach moving to P2.
- **The reviewer judged these justified but incomplete, now completed:**
  - the interactive protocol in the executor (B10);
  - a failed redo (B4);
  - undo reporting through the sticky error (B8);
  - value literals (B11, B12).
