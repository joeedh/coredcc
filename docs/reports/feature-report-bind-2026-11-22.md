# Feature report: script bindings (`bind/`), 2026-11-22

- **Reviewed:** the API surface of `source/coredcc/bind/` and the bindings it added to
  `props/`, `command/` and `base/`, at `1d0f667`. It was checked against the P1 plan's
  "P1f step 4: `bind/` design" through "P1f step 4 implementation", and against the C2
  foreign-dispatch contract (`extern/litestl/documentation/binding.md`).
- **Reviewer:** a fresh agent given the code and the design, not the author's reasoning.
- **Method:**
  - A throwaway native test (since deleted) drove the facades and script commands
    through `tests/bind/fake_host.h`.
  - `coredcc_gen_ts` output was type-checked with `tsc --strict`, together with a small
    sample of how step 5 would use it.
  - Each finding says whether it was confirmed by **experiment** or by **code reading**.
- Findings are ranked by severity within each section. Each ends with its resolution;
  the regression tests are in `tests/bind/test_bind_review.cc`.

## Summary
- **What held up:**
  - **No abort from script input.** Tried: the wrong value types, `PropValue()` (None)
    passed to `writeValue`, `setValue` and `defInput`, `statusError(Ok)` and
    `statusError(99)`, out-of-range indices, unknown flag bits, bad input names, a
    re-registered (moved-from) def, unknown slot names, and squatting a core command id.
    Every one came back as an error result.
  - **Handle counts.** Every instance and factory handle was released exactly once on
    these paths:
    - a throwing canRun, and a refusing one;
    - a throwing memSize, undo and discard;
    - redo truncation and budget eviction;
    - rerunHead on a script head;
    - an interactive script command cancelled by executor teardown;
    - a registration made from inside exec;
    - every refused registration.
  - **Reentrancy.** No route from a script slot reached `HistoryCall`'s abort. That
    includes setValue from canRun, invoke from exec, every facade from discard during
    `clear()`, and setValue from undo.
  - The generated TS type-checks under `--strict`. The pressure test had left that
    unverified.
- **What didn't:**
  - Teardown order is a convention that nothing checks, and step 5's host crosses a GC
    boundary.
  - A script can take a core command id and break `setValue` for the whole Runtime.
  - Script failures in `undo` and `memSize` have outcomes a script can't recover from
    sensibly.
  - The surface is inconsistent: `setValue` from exec is a sub-command, but `invoke`
    from exec is Busy. Enum and Flags values can't be read at all.

## Bugs and hazards
1. **H1 (high, latent): Runtime-before-history teardown is a use-after-free, and nothing
   detects it.** *Code reading; not run, because it crashes.*
   - **Scenario:** the host destroys the Runtime before the LinearHistory.
     - `~Runtime` deletes ForeignBindings and its Dispatcher, then the IfaceRegistry and
       its tables. CommandRegistry goes too, with the CommandDefs that `CommandRecord::def_`
       points at.
     - `~LinearHistory` then calls `clear()`, which calls `discard` on each record,
       which calls `FwdCall` through a freed `IfaceTable` and `Dispatcher`.
     - An executor destroyed after the Runtime with an interactive script command open
       does the same through `open_->undo`.
   - **Why it matters now:** step 5's host setup (Runtime, Library, executor, history)
     sits behind the JS boundary. If JS owns these as separate objects, their
     FinalizationRegistry order is undefined.
   - **Fix:**
     - The spike binds one C++ host object that owns all of them, in the right
       destruction order, rather than binding each one.
     - ForeignBindings counts live instances. `makeInstance` increments the count, and
       the `Owned` deleter decrements it (the deleter's unused `ctx` can carry the
       ForeignBindings). `~ForeignBindings` then makes a `CDCC_CHECK` naming the count,
       so a wrong order becomes a clear abort instead of a silent UAF.
   - *Fixed (the check):* ForeignBindings counts live instances (`makeInstance` and the
     `Owned` deleter, through its `ctx`), and `~ForeignBindings` aborts with "script
     command instances outlive the Runtime" when any is alive.
   - *Step 5:* the spike binds one host object that owns the Runtime, Library,
     executor and history in order.
2. **B1 (medium): a script can register `core.setProperty` and break `setValue` for the
   whole Runtime.** *Experiment.*
   - **Scenario:** a script registers `core.setProperty` before the first `setValue`.
     - Registration succeeds.
     - `setPropertyDef` finds the script's def. `own<Command>` then finds the foreign
       table under `core.setProperty#impl`.
     - Every later `setValue` and `setValueUndo` fails: "TypeMismatch:
       core.setProperty#impl is not registered for this C++ type". This lasts until
       restart.
   - Any natively registered command that is registered lazily, or after scripts load,
     can be taken the same way. A later `addNativeCommand` then gets AlreadyExists.
   - **Fix:**
     - Register `core.setProperty` eagerly, with the registry's built-ins.
     - Make `setPropertyDef` check that the def it found has its own `implType`.
     - Refuse script ids in the `core.` namespace (or a reserved-prefix list) in
       `registerForeignCommand`.
   - *Fixed:*
     - `registerForeignCommand` refuses ids in the `core.` namespace.
     - `setPropertyDef` refuses a `core.setProperty` def with another implementation
       (AlreadyExists, instead of a TypeMismatch on every set).
   - *Not done:* eager registration. CommandRegistry's constructor can't register a
     command (the extension would ask for itself), and the two checks close the hole.
3. **B2 (medium): a failed script `undo` leaves the record redoable, so redo applies the
   command again over state that was never undone.** *Experiment.*
   - **Scenario:** a TS undo throws.
     - `sc::undo` returns Internal ("undo: Error: undo failed"). The cursor has still
       moved.
     - `sc::redo` returns ok. With no redo slot it re-runs exec on top of the state
       that was never undone.
   - This is the executor's existing rule that undo can't be rolled back (command report
     B8). Script slots throw far more often than native ones, though, and a scripting
     user can't see the corruption.
   - **Fix:** when undo leaves an error, LinearHistory drops that record and the redo
     side, as it does for a failed redo, and the returned error says so.
   - *Fixed:* `LinearHistory::undo` checks for a sticky error the undo left. If there is
     one, it drops that record and the redo side, takes the error, and returns it with
     "it and the commands after it were dropped". This applies to native commands
     too, and is documented on LinearHistory.
4. **B3 (medium): a throwing `memSize` reports the run as failed, although the command
   was applied and recorded.** *Experiment.*
   - **Scenario:** a TS class whose `memSize` returns `undefined` or throws.
     `foreign.ts`'s `writeScalar` throws "expected a number or bigint".
     - `invoke` returns "'js.k' ran and was recorded, but the history's upkeep failed".
     - The history grew by one.
     - A script that retries on failure applies the command twice.
   - **Fix:**
     - The TS adapter always supplies `memSize`, catching to 0.
     - Optionally, `registerForeignCommand`'s tables could treat a failed `memSize` as
       0 with a warning instead of a sticky error.
   - *Deferred to step 5:* the TS adapter always supplies `memSize`, catching to 0. A
     test pins today's behaviour (reported, recorded, and the history consistent).
5. **B4 (medium): scripts can't compose commands, and the surface is inconsistent about
   it.** *Experiment.*
   - **Scenario:** a TS command's exec calls `invoke(ctx, "test.add(amount=1)")` and gets
     Busy. The same exec may call `setValue`, which becomes a sub-command.
   - No facade reaches `runSub`, so a script "macro" command is impossible. Native
     commands can write one.
   - **Fix:** apply `setValue`'s rule to `invoke`.
     - Inside a script exec (`inForeignCall && ex->running()`), it runs as `runSub` via
       `inputsFromInvocation`.
     - Elsewhere in a foreign call it stays Busy.
   - *Fixed:* from a script command's exec, `invoke` runs as `runSub`. Elsewhere in a
     script slot it is Busy.
6. **B5 (medium-low): the guard approximates "inside exec" with `ex->running()`, and
   `writeValue` is not guarded at all.** *Code reading and experiment.*
   - **Scenario A:** a native parent's exec runs a script sub-command. The sub's
     `canRun` (or its factory's `make`) calls `setValue`.
     - `running()` is true, so the guard lets it through.
     - The executor's `slotDepth_` check then refuses it as FailedPrecondition.
     - This is safe, but it shows the guard does not express the rule it documents.
   - **Scenario B:** a script's `canRun`, `memSize`, `discard` (via a captured host
     ctx) or `make` calls `writeValue`. The document changes, and no record owns the
     change. *Code reading: `writeValue` has no guard.*
   - **Fix:** the Dispatcher already receives the slot index. Push it on a slot stack
     in ForeignBindings, and gate each facade per slot:
     - `setValue` and `invoke` only in exec;
     - `writeValue` only in exec, undo and redo;
     - reads anywhere.
   - *Fixed:* ForeignBindings keeps a stack of the foreign calls' method names, and the
     facades are gated on the innermost one: `setValue` and `invoke` only in exec,
     `writeValue` in exec, undo and redo, and `undo`/`redo` never. Outside any script
     slot everything is allowed. The rules are in `facades.h`.
7. **B6 (low-medium): Enum and Flags values can't be read.** *Experiment.*
   - `getValue(ctx, "objects[0].mode")` returns an Enum value. `readInt` and
     `readString` are both InvalidArgument. Flags are the same.
   - Writing works: `writeValue(..., makeString("EDIT"))` is ok. A Pointer read
     round-trips opaquely: `setValue(tool.activeBrush, <read value>)` is ok.
   - **Fix:**
     - Add `readEnum` (an IntResult) and `readFlags`.
     - Add `valueToString` over `PropValue::toString`, for display and debugging.
     - `makeQuat` and `makeMat4` can wait.
   - *Fixed:* `readEnum`, `readFlags` (the bits as an int64) and `valueToString`.
8. **B7 (low): reusing a registered def is silent, then fails with a misleading
   message.** *Experiment.*
   - **Scenario:** after `registerForeignCommand` the TS-owned def has been moved from.
     - `defInput` on it returns ok.
     - Registering it again returns "command '': an empty id".
   - **Fix:** either:
     - copy the def at registration, leaving the TS builder intact and reusable; or
     - mark it registered, and have the `def*` facades and registration return
       FailedPrecondition, "this def was already registered as 'js.x'".
   - *Fixed:* registration copies the def. The builder stays usable, and registering it
     again is AlreadyExists.
9. **B8 (low): `checkDef` accepts ids that the invocation parser can't call.**
   *Experiment.*
   - `"Not An Id!"` registers fine. `invoke("Not An Id!()")` then fails to parse.
   - This is shared with native defs, but script ids are the ones most likely to be
     wrong.
   - **Fix:** check the dotted-identifier syntax in `checkDef`.
   - *Fixed:* `checkDef` refuses an id that isn't dotted identifiers (native defs
     too).
10. **B9 (low): `defInput` and `defOutput` don't check names.** *Experiment.*
    - An empty name and a duplicate are both accepted, and fail only at registration.
      By then the bad entry can't be removed, so the def has to be rebuilt.
    - **Fix:** check the identifier and uniqueness in `scriptProp`.
    - *Fixed:* `defInput` and `defOutput` refuse a non-identifier name
      (InvalidArgument) and a repeated one (AlreadyExists).
11. **B10 (low): the factory count is per registration, which is easy to get wrong.**
    *Experiment.*
    - **Scenario:** one factory handle passed to two `registerForeignCommand` calls is
      released twice.
    - This is correct only if TS called `foreignHandle(obj)` once per call. The TS
      table adds a count each time, so it works, but nothing says so.
    - **Fix:** document "each call takes one count of `factory`" on
      `registerForeignCommand`, and in the TS layer.
    - *Fixed:* documented on `registerForeignCommand`.
12. **B11 (low): `statusError` accepts any code number.** *Experiment.*
    - `statusError(99, "x")` makes an error whose code prints as "Unknown".
    - **Fix:** map out-of-range codes to InvalidArgument, as `Ok` already is.
    - *Fixed.*

## For the TS layer (step 5)
- *Resolution:* all of these are recorded in the plan for step 5.
1. **Result-struct members are borrowed views that don't keep the result alive.** *Code
   reading of `typescriptRuntime/bind.ts`: a member getter calls `getBoundPointer` with
   no reference to the parent.*
   - Keeping `invoke(...).status` or `.value` past the current turn reads freed memory
     once the result is finalized.
   - Unwrapping in the same synchronous code is safe: finalizers don't run
     mid-turn.
   - **Fix:** unwrap every facade result at once, behind one helper:
     `using r = f(...)`, throw on `!r.status.ok()`, and return a JS value or a copy.
     Never hand result structs to user code.
2. **`CommandImpl` requires `canRun`, `memSize` and `discard`, and `addForeign` does too.**
   *tsc and experiment.*
   - **Scenario:** `class TsCommand { exec; undo }` fails `tsc` against `CommandImpl`,
     and registration refuses it (TypeMismatch, a missing required slot).
   - **Fix:** the hand-written `ScriptCommand` makes these optional. `make` returns
     the handle of an adapter that supplies the defaults (an ok Status, 0, a no-op).
     - The adapter always lists the required slots in `slots`.
     - It lists `redo` only if the user's class has one.
     - It must not detect methods by walking a base class that provides `redo`. That
       would silently turn redo-by-exec off.
3. **Thrown errors are always Internal.**
   - A canRun refusal written as `throw` reads as Internal, not FailedPrecondition. The
     `ret` converter never sees a throw.
   - **Fix:** the adapter catches inside `canRun` and returns
     `statusError(StatusCode.FailedPrecondition, msg)`. For that it needs the runtime
     enum, so see item 5.
4. **int64 is typed `bigint` only.** *tsc.*
   - `makeInt(3)` doesn't type-check, and `readInt(...).value` is a `bigint`. The
     runtime accepts numbers as arguments.
   - **Fix:** wrap these as number helpers in the TS layer.
5. **Generated module shape.**
   - `index.ts` exports neither `functions.ts` nor the `StatusCode` and `PropValueType`
     enums. Import them from their own files.
   - `newCommandDef` is typed `CommandDef | undefined`.
   - Each `undefined` → ok `Status` conversion allocates a bound Status. Cache one.

## Gaps against the design and earlier deferrals
- *Resolutions:*
  - `canUndo`, `canRedo`, `defDescription` and `defIcon` were added.
  - Registry removal (TS hot reload), the `runSub` argument order, and Enum inputs and
    numeric ranges in the def builders are deferred past step 5, and recorded in the
    plan.
- **Registry removal (TS hot reload)** was deferred to P1f by the command review. Step 4
  neither does it nor records a new deferral.
  - Reloading a script re-registers its ids and gets AlreadyExists.
  - Removal also has to deal with history records that point at the def and its
    foreign table.
- **"A consistent `runSub` argument order"** was also deferred to P1f, and is
  untouched. B4 would make it relevant.
- **The def builders are missing several things native defs have:**
  - description and icon;
  - Enum and Flags inputs;
  - numeric ranges.
  - Script commands therefore can't take a mode argument.
- **No `canUndo` or `canRedo` facade.** A script UI can't grey out its buttons.

## Test gaps (`tests/bind/`)
- *Resolution:* `test_bind_review.cc` covers all of these except B10 (the fake host
  holds one count per handle) and None values (covered by the reviewer's experiment).
- **Slot failures:**
  - canRun throwing, and canRun refusing;
  - memSize throwing (B3);
  - undo throwing, then redo (B2).
- **Handle counts:** release-once on redo truncation, budget eviction, rerunHead, an
  interactive script command, and executor teardown with that command open.
- **The guard:**
  - Busy from canRun, memSize and `make`;
  - setValue from a script sub's canRun inside a native parent's exec (B5A).
- **Registration:**
  - B1 (a squatted core id);
  - B7 (a reused def);
  - B8 (an unparseable id);
  - B10 (a shared factory).
- **Facades:**
  - Enum, Flags and Pointer reads (B6);
  - None values passed to `writeValue`, `setValue` and `defInput`.

## Spec deviations
- **The reviewer judged the recorded deviations justified:**
  - `::` interface names;
  - `defOutput`;
  - the top-level `PropValueType`;
  - ForeignFactory as a plain `isInterface` struct;
  - `make` raising the depth;
  - building `coredcc_gen_ts` now.
- **Unrecorded, and cosmetic:** the plan's `ScriptDispatcher` is the private
  `ForeignBindings::Dispatcher`.
