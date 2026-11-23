# Feature report: the TS spike and coredcc's TS layer, 2026-11-22

- **Reviewed:** the API surface of P1f step 5 at `e7bf769`:
  - `source/coredcc/bind/ts/index.ts` (`coredcc-script`);
  - `tests/wasm/` (`spike_host.cc`, `run.mjs`, `gen.ts`, `spike.test.ts`, `CMakeLists.txt`);
  - `addBindings()` and `statusText()` in `bind/`.

  It was checked against the P1 plan's "P1f step 4 review corrections" (the step 5
  obligations) and "P1f step 5: the spike", the previous review's "For the TS layer
  (step 5)", and litestl's TS runtime (`foreign.ts`, `backend.ts`, `bind.ts`,
  `manager.ts`).
- **Reviewer:** a fresh agent given the code and the design, not the author's reasoning.
- **Method:**
  - Extra `*.test.ts` files, kept outside the repo, ran against the wasm-st Debug
    `coredcc_spike.mjs`. They went through a copy of `run.mjs` that takes the test file
    from an environment variable, so each one was type-checked with `tsc --strict` like
    the suite.
  - Destructive cases (a trap, a host freed while in use, two modules) each ran in a
    fresh process.
  - Each finding says whether it was confirmed by **experiment** or by **code reading**.
- Findings are ranked by severity within each section. Each ends with its resolution;
  the regression tests are in `tests/wasm/spike.test.ts`, named by finding.

## Summary
- **What held up:**
  - **Results are never handed out.** `check` and `take` unwrap and dispose of every
    result in the same call, and `toJs` copies a Pointer value into an owned PropValue.
    Pointer values hold a `StableRef`, which is safe to keep across turns.
  - **Fatal errors.** A `WebAssembly.RuntimeError` thrown from exec is rethrown and
    poisons the module. Later calls throw "the wasm module was poisoned".
  - **Stale slot contexts throw rather than crash.** Some code keeps the exec `ctx` and
    uses it later. At the top level that is an Error ("bound object was disposed"). In
    undo, the undo fails and its record is dropped cleanly.
  - **Error codes survive nesting.** A NotFound from a sub-command run through `invoke`
    in exec reaches the top level as NotFound. A plain Error becomes Internal with its
    message and no slot-name prefix.
  - **Every step 5 obligation is met:**
    - one host object, in destruction order;
    - results unwrapped at once;
    - the adapter supplies canRun, memSize and discard;
    - redo is listed only when the class has one;
    - a throwing or `false` canRun is FailedPrecondition;
    - number helpers;
    - imports from the enums' own files.

    Experiments confirmed the parts the suite doesn't test:
    - a memSize that throws gives 0, and the run is ok and recorded;
    - an inherited `redo` is detected and runs;
    - a ScriptError thrown from redo keeps its code and drops the redo side.
  - **Other cases that worked:**
    - registering from inside exec and from inside canRun;
    - calling `load` twice on the same module;
    - a misnamed `getManager` on a real module, which gives a clear error;
    - an `--out` path containing spaces.
  - **NaN and Infinity on an Int member are OutOfRange errors.** NaN on a Float member
    is an error too, and Infinity clamps to the member's range.
- **What didn't:**
  - Lifetimes the TS layer can't see. A `Context` view outlives its Host, and a
    collected Host's context silently addresses a different document.
  - The adapter swallows its slots' return values, and with them litestl's "returned a
    Promise" check. `async exec` type-checks, is recorded as a success, and then runs
    after its slot has closed.
  - A throwing `discard` repeats B3 from the previous review: an unrelated applied
    command is reported as failed.
  - "Integers become Int" makes a Float input with a whole default (`1.0`) impossible,
    and integers past int64 wrap silently.
  - Loading a second module redirects every call to it and accepts the first module's
    wrappers, so calls read the wrong memory.

## Bugs and hazards
1. **T1 (high): a `Context` view outlives its Host. After a GC it silently addresses
   another document.** *Experiment.*
   - `spike.context(host)` returns a non-owning view. Neither the view nor the TS layer
     keeps the Host wrapper alive, or notices when it goes.
   - **Scenario A (dispose):** `host[Symbol.dispose]()`, then `cdcc.get(ctx, 'count')`.
     The ctx still has its pointer, so the call reads the freed Runtime. Here it returned
     "NotFound: no member 'count' in an unregistered struct" (freed memory). It could as
     well trap or write.
   - **Scenario B (GC):**
     - `const ctx = spike.context(spike.create())` leaves the Host wrapper unreachable.
     - After `gc()` its finalizer frees the Host.
     - A second `spike.create()` reuses the block.
     - `cdcc.get(ctx, 'count')` then returned **7, the second host's value**, and
       `cdcc.set(ctx, ...)` wrote into the second document.
     - Run under `--expose-gc`.
   - **Why it matters past the spike:** P2's real host will hand scripts a Context the
     same way. The previous review's H1 moved the *C++* order into one object. This is
     the same problem one level up, between the Host and the views it hands out.
   - **Fix:**
     - Don't give user code a raw Context. The TS layer wraps the host in a `Session`
       (or similar) that holds the Host wrapper, so it can't be collected, and hands out
       its `ctx`. On dispose it sets the ctx view's `ptr` to 0, so later calls throw
       "disposed" like a stale slot ctx.
     - Longer term, litestl could add a "borrowed from argument N" return policy. The
       view would hold its parent and check `parent.ptr` on each access.
   - *Fixed:* the TS layer's `Session(host, ctx)` hides a reference to the host on the
     ctx, so GC can't free a document whose context is reachable. Disposing of the
     session zeroes the ctx, so later calls throw "disposed". A raw context from
     `spike.context` still has the hazard; hosts hand out Sessions.
2. **T2 (high): async slots pass silently: `async exec` is recorded as a success, and
   `async canRun` never refuses.** *Experiment and tsc.*
   - `foreign.ts` rejects a slot that returns a Promise. `Adapter` calls the user's
     method, ignores what it returns, and returns `undefined` (ok). The thenable check
     never sees the Promise.
   - **Scenario:**
     - `class C implements ScriptCommand { async exec(c) { await null; cdcc.write(c, 'count', 7) } undo() {} }`
       type-checks: a `void` method signature accepts `Promise<void>`.
     - `cdcc.invoke(ctx, 'ts.async')` returns `{}`, and the history grows by one.
     - The body runs on the next microtask against the closed slot's ctx. It fails as
       an **unhandled rejection** ("writeValue argument 'ctx': bound object was
       disposed"), and `count` stays 0. The user sees a successful, recorded command
       that did nothing, and an error that names no command.
     - `async canRun() { return 'nope' }` doesn't type-check under `implements`. Without
       `implements` it is allowed to run, although it refused.
   - Async code is ordinary JS, so scripting users will write it.
   - **Fix:** in every adapter slot, check the user method's return value. If it is a
     thenable, throw (or return) a TypeError status: "exec returned a Promise: command
     slots are synchronous". Attach a no-op `.catch` to the Promise so it doesn't also
     surface as an unhandled rejection.
   - A compile-time check is harder than it looks. Typing the slots as returning
     `undefined` would reject `async`, but it would also reject every plain method,
     whose inferred return type is `void`. *Unverified* whether a cleaner type exists;
     the runtime check is the one that matters.
   - *Fixed:* every adapter slot rejects a returned thenable ("exec returned a Promise:
     command slots are synchronous"), and attaches a no-op `catch`.
3. **T3 (medium): a throwing `discard` reports a later, unrelated command as failed,
   although it was applied and recorded.** *Experiment.* This is B3 again, for discard.
   - **Scenario:**
     - Run a command whose `discard` throws, then undo it.
     - `cdcc.set(ctx, 'strength', 0.1)` truncates the redo side, which calls `discard`.
     - The set throws "Internal: 'core.setProperty' ran and was recorded, but the
       history's upkeep failed: discard: discard failed".
     - `strength` is 0.1 and the set is in the history. A script that retries on error
       applies it twice.
     - Budget eviction behaves the same (code reading: it runs the same discard).
   - **Fix:** `Adapter.discard` catches non-fatal errors, as `memSize` does, and reports
     them as a warning (`console.warn` with the command id). Nothing can act on a failed
     discard anyway.
   - *Fixed:* `Adapter.discard` catches non-fatal errors and warns, as memSize does.
4. **T4 (medium): a second module silently takes over every call and accepts the first
   module's wrappers.** *Experiment.*
   - `setBindingBackend` is process-global, and `bindFunction` re-resolves against the
     newest backend. Wrappers are matched by type name only.
   - **Scenario:** load module A and make a host. Then `load(moduleB)`, and call
     `cdcc.get(ctxA, 'count')`.
     - That ran B's `getValue` on A's pointer: "RuntimeError: memory access out of
       bounds", with a stream of "litestl::alloc::check_mem: error: invalid memory
       block".
     - `cdcc.set(ctxA, ...)` threw **`ScriptError(0)` with an empty message**, an "Ok"
       error read from garbage.
     - The RuntimeError happened outside a foreign call, so module B was not poisoned
       and stays in use after the trap.
   - `load`'s doc says "Call once per module", which reads as though several modules
     are supported.
   - **Fix:**
     - Document one coredcc module per JS realm.
     - Make a second `load` of a *different* module throw, unless the caller passes an
       explicit `replace: true`.
     - In litestl, tag each wrapper with its manager (or wasm). Have argument packing
       reject a wrapper from another module, with a TypeError naming both.
   - *Fixed:* `load` returns the backend for the same module, and throws for another one
     unless `{replace: true}`. Documented as one module per realm. Tagging wrappers by
     module in litestl is left for later.
5. **T5 (medium): an input declared with a whole-number Float default becomes Int.**
   *Experiment.*
   - `fromJs` maps every `Number.isInteger` value to Int, and `defInput` takes its kind
     from the default's type. In JS `1.0 === 1`.
   - **Scenario:** `registerCommand(ctx, {id: 'ts.scale', inputs: {factor: 1.0}}, ...)`,
     then `invoke(ctx, 'ts.scale(factor=0.5)')` gives "OutOfRange: factor: 0.5 is not an
     integer in the int64 range". A scale, opacity or weight input defaulting to 1 can't
     be declared at all.
   - Outputs are affected the same way.
   - `set` and `output` on existing Float members are unaffected, because C++ coerces
     Int to Float.
   - **Fix:** let a spec entry say its kind: `inputs: {factor: cdcc.float(1)}`, or
     `{factor: {float: 1}}`, alongside the plain-value shorthand. Document that a bare
     integer declares an Int.
   - *Fixed:* `float(1)` declares a Float; documented that a bare whole number declares
     an Int.
6. **T6 (medium-low): integers past int64 wrap silently, and past 2^53 they round
   silently.** *Experiment.*
   - `BigInt(x)` is written into an int64 slot, which wraps modulo 2^64:
     - `fromJs(2 ** 63)` reads back as -9223372036854775808;
     - `fromJs(2n ** 64n)` reads back as 0.
   - **Scenario:** `cdcc.set(ctx, 'strength', 1e19)` (a Float member, range 0-1) sets
     **0**. 1e19 wraps to a negative int64, which then clamps to the minimum. 1e20 wraps
     positive and gives 1.
   - Other slots wrap the same way:
     - `flags: 2 ** 40` truncates to 0 in `defFlags`' uint32, and registers with no
       flags;
     - `memSize` returning `2 ** 70` writes 0.
   - Reading: `toJs` of an Int `2^53 + 1` returns 2^53. Flags bits above 52 are lost the
     same way.
   - **Fix:**
     - `fromJs`: an integer outside ±2^53 becomes a Float (C++ then range-checks it, or
       coerces it to Int with "not an integer in the int64 range"). A bigint outside
       int64 is a RangeError.
     - `toJs`: return a `bigint` when the Int isn't a safe integer. Alternatively throw,
       or offer a `getBig` variant.
     - `defFlags`: check `flags` is a uint32 in TS.
     - `memSize`: clamp to `Number.MAX_SAFE_INTEGER`.
   - *Fixed:* a number past 2^53 becomes a Float (C++ range-checks it), a bigint outside
     int64 is a RangeError, an Int read past 2^53 returns a bigint, `registerCommand`
     checks `flags` is a uint32, and memSize clamps.
7. **T7 (medium-low): a `redo` written as an arrow-function property is ignored, and
   redo silently re-runs exec.** *Experiment.*
   - Detection reads `Cls.prototype.redo`, and class fields live on the instance. The
     adapter calls `this.cmd.exec` and the like through the instance, so arrow `exec`,
     `undo` and `canRun` all work. Only `redo` silently changes meaning.
   - **Scenario:** `class { exec = () => {...}; undo = () => {}; redo = () => {...} }`,
     run, undo, then redo: exec ran twice, and redo never.
   - **Fix:** in the `Adapter` constructor, if the instance has a `redo` function but the
     registration didn't list `redo`, throw a TypeError ("redo must be a method, not a
     field"). `make` then fails on the first run with a clear message. Or document that
     slots must be methods.
   - *Fixed:* the adapter refuses an instance whose `redo` is a field when the class has
     no `redo` method ("redo must be a method"), so the first run fails clearly.
8. **T8 (low-medium): a slot can dispose of the Host and abort the module.**
   *Experiment.*
   - **Scenario:** a script command's exec calls `host[Symbol.dispose]()` (or drops the
     last reference to a Host that is mid-command, and a GC runs).
     `~CommandExecutor` asserts "CommandExecutor destroyed while a command runs". The
     abort poisons the module.
   - This crashes the module instead of throwing, and nothing in the TS layer stops it.
   - **Fix:** the T1 `Session` refuses to dispose while one of its slots is running. The
     adapter keeps a depth count, and dispose throws "can't close the document from
     inside a command".
   - *Fixed:* the adapter counts slots in progress, and `Session` refuses to close
     inside one ("can't close the document from inside a command").
9. **T9 (low-medium): codes are lost in undo and in the constructor.** *Experiment.*
   - The undo slot returns `void`, so its errors go through `foreign.ts`'s error string.
     The adapter doesn't map undo errors.
   - **Scenario A (undo):** an undo whose `write` gets NotFound reports "Internal: undo:
     NotFound: no member 'nope' ...; undo of 'ts.undoThrows' failed, ...". The code
     appears inside the message, and the reported code is Internal.
   - **Scenario B (constructor):** a constructor that throws
     `ScriptError(NotFound, 'no brush')` gives "Internal: the command factory failed:
     make: no brush".
   - exec, redo and canRun keep codes. Undo and `make` don't, and the asymmetry isn't
     documented.
   - **Fix:** document it on `ScriptCommand`, or carry the code:
     - Undo has no Status to return, so a C++ change would have to let a foreign error
       string carry a code. One option is a `"Code: "` prefix that `ForeignBindings::call`
       parses.
     - For `make`, the adapter could catch the constructor's error in `make`, keep it,
       and return 0. The C++ make failure would then need to look it up.
   - *Documented* on `ScriptCommand`: undo and constructor errors report Internal, with
     the error in the message. Carrying the code needs a C++ change to foreign error
     strings, left for later.
10. **T10 (low): `undefined` is a `Value`, but nothing accepts it, and the errors are a
    mix of types.** *Experiment.*
    - `set(ctx, p, undefined)`, `output(ctx, n, undefined)` and
      `inputs: {a: undefined}` type-check. Each throws "TypeError: fromJs: can't make a
      value of undefined". `toJs` returns `undefined` for None, so a None read can't be
      written back.
    - What callers catch is a mix of types:
      - `ScriptError` from C++;
      - `TypeError` from `fromJs`, for a vector length or a non-number component;
      - a plain `Error` for a stale ctx or a disposed PropValue, and from `load`.
    - A `Float32Array` gives "can't make a value of 1,2".
    - Mat4 is no better: `toJs` returns 16 numbers, but `fromJs` refuses 16 (no
      `makeMat4`), so `set(ctx, m, get(ctx, m))` fails for Mat4 members. *Code reading.*
      Quat works, because C++ coerces a Vec4 to a Quat (`props/def.cc`, coerce).
    - **Fix:**
      - Split the types: `Value` for reads, and `Writable` (without `undefined`) for
        writes.
      - Accept `ArrayLike<number>` of length 2-4 and 16, which needs a `makeMat4` facade.
      - Say on `ScriptError` that misuse of the TS layer itself throws `TypeError`.
    - *Fixed:* `Value` (reads) and `Writable` (writes, no `undefined`) are separate
      types; writes accept `ArrayLike<number>`; the module doc says misuse of the layer
      throws TypeError or Error. *Not done:* Mat4 writes (no `makeMat4` facade yet),
      documented on `Writable`.
11. **T11 (low): a stale ctx passed to `registerCommand` leaks a factory handle.**
    *Experiment.*
    - `backend.foreignHandle(factory)` is evaluated as an argument *before* argument
      packing rejects the stale ctx. C++ never takes the count.
    - **Scenario:** `registerCommand(keptSlotCtx, ...)` throws "bound object was
      disposed", and `backend.foreignCount()` goes from 1 to 2 for good. The spike's
      `afterEach` would catch it.
    - **Fix:** check `ctx.ptr` (or call a cheap facade such as `canUndo`) before taking
      the handle. Or release it on a JS-side throw, which needs litestl to export a
      release for `foreignHandle`.
    - *Fixed:* `registerCommand` checks the ctx before taking the factory handle.
12. **T12 (low): outputs named `__proto__` vanish.** *Experiment.*
    - `bagToJs` assigns `out[name] = ...` on a plain object, and `__proto__` is a valid
      identifier to `defOutput`.
    - **Scenario A:** `outputs: {__proto__: 1}` as an object literal sets the
      prototype, and nothing is registered.
    - **Scenario B:** an output registered through `defineProperty` runs, but its value
      goes to the `__proto__` setter. `invoke` returns `{}`.
    - **Fix:** build the result with `Object.create(null)` or `defineProperty`. Or have
      `defInput` and `defOutput` refuse `__proto__`.
    - *Fixed:* `bagToJs` defines each output with `defineProperty`.
13. **T13 (low): failed slots' Status objects wait for GC.** *Code reading.*
    - `statusOf` and the string or `false` canRun branches return an owning Status from
      `statusError`. `dispatch` copies it into C++'s return slot, and nothing disposes
      of the JS one. It stays on the wasm heap until the FinalizationRegistry runs.
    - This is harmless unless a loop of failing commands races the GC, and it shows up
      in `getMemSize`.
    - **Fix:** after making the error Status, `queueMicrotask(() => s[Symbol.dispose]())`.
      The copy happens synchronously in `dispatch`, before the microtask.
    - *Fixed:* an error Status a slot returns is disposed of in a microtask, after C++
      copies it.
14. **T14 (low): `load` misuse gives runtime internals.** *Experiment.*
    - `load({})` throws "TypeError: Cannot read properties of undefined (reading
      'buffer')". `createWasmHelpers` runs before the export check.
    - The default `getManager = 'CDCC_GetManager'` names an export that only
      `bind/wasm_entry.cc` hosts have. The spike has to pass `SPIKE_GetManager`, and
      `gen.ts` hardcodes it.
    - **Fix:** check that `module` has `wasmMemory` or `HEAPU8` and the export first,
      then name the expected shape in the error.
    - *Fixed:* `load` checks the module's shape and the export before
      `createWasmHelpers`.
15. **T15 (low): `flags` is a bare number.** *Experiment.*
    - No `CommandFlags` enum is bound, so scripts write magic numbers from `command.h`.
      Out-of-range values fail with the wrapped value ("unknown flag bits 4294967280"
      for -1).
    - **Fix:** bind `CommandFlags` as an enum and re-export it, as `StatusCode` is.
    - *Fixed:* `CommandFlags` is bound as an enum and re-exported; `flags` takes it.

## Harness (`run.mjs`, CMake)
1. **R1 (low-medium): `spike_ts` exists only if `node_modules` existed at configure
   time.** *Code reading* (`tests/wasm/CMakeLists.txt`: `EXISTS .../node_modules`).
   - **Scenario:** on a fresh clone, configure and then `npm ci`. Until a reconfigure,
     ctest runs 36 tests and passes. The only trace is a STATUS line in the configure
     log. The gate in the commit message ("wasm-st 37/37 (with spike_ts)") can't tell
     the difference.
   - Nothing checks that `node_modules` matches `package-lock.json`.
   - **Fix:** always `add_test` when node is found. When the deps are missing, `run.mjs`
     exits with a dedicated code, which `SKIP_RETURN_CODE` turns into a visible ctest
     *skip*.
   - *Fixed:* `spike_ts` is registered whenever node is found. Without `npm ci`, run.mjs
     exits 77, which ctest reports as a skip (checked).
2. **R2 (low): a `%` in a path breaks the `.cmd` route.** *Experiment.*
   - The quoting handles spaces (verified with `--out "...\e3 with space"`), but
     `cmd.exe` still expands `%VAR%` inside quotes.
   - **Scenario:** `--out ...\p%OS%q`. esbuild wrote `...\pWindows_NTq\gen.mjs`, and
     node then failed with "Cannot find module ...\p%OS%q\gen.mjs".
   - **Fix:** skip the shell. Run `process.execPath` on the packages' JS entry points
     (`node_modules/typescript/bin/tsc`, `node_modules/esbuild-wasm/bin/esbuild`).
   - *Fixed:* tsc and esbuild run as `node <entry point>`, with no shell.
3. **R3 (low): the default `--out` is relative to the working directory.** *Code
   reading.*
   - `node tests/wasm/run.mjs --module ...` from the repo root writes `spike-ts/` into
     the checkout. That directory isn't ignored (`.gitignore` has only `build/`, and
     `tests/wasm/.gitignore` only `node_modules/`).
   - **Fix:** default to a directory under the module's folder, or to `os.tmpdir()`.
   - *Fixed:* `--out` defaults to `<tmpdir>/coredcc-spike`.
- **Held up:**
  - The generated directory is wiped on every run, so no stale bindings survive.
  - The bindings are generated from the module under test, so they can't drift from it.
  - The missing-`tsc` check gives a clear "run npm ci" message.

## Test gaps (`tests/wasm/spike.test.ts`)
- *Resolution:* the suite now covers every branch listed here, plus Bool, String,
  Vec3, Enum and Flags members (added to `spike::Data`) and each finding above. Pointer,
  Quat and Mat4 values through the TS layer are still untested.
- **The four exit-criteria steps are all covered:**
  - step 4 checks undo, redo-by-exec, LastUsed inputs and outputs;
  - `afterEach` checks the host's invariants and the foreign handle count.
- **Branches the suite never exercises:**
  - a class with its own `redo`: the `slots.push('redo')` branch never runs;
  - `canRun` throwing and returning `false`;
  - `memSize` throwing, which was the B3 obligation (verified only by this review);
  - `discard` throwing (T3).
- **Values:** the root struct has only an int and a float, so nothing tests the
  following through the TS layer:
  - Bool, String, vectors, Quat and Mat4 (T10);
  - Enum and Flags reads;
  - Pointer values.

  Adding a `Vec3`, an enum and a pointer member to `spike::Data` would cover these.
- **Error fidelity:** nothing checks that a `ScriptError` code survives a sub-command,
  or what happens in undo and `make` (T9).
- **Misuse:**
  - an async slot (T2);
  - a stale ctx;
  - a ctx used after its host is disposed (T1);
  - a whole-number Float default (T5).

## Small notes
- *Resolution:* `ScriptError.code` is readonly, and `registerCommand` checks the def.
  `isFatal` stays local (the layer is Emscripten-only).
- `index.ts` has its own `isFatal`, without `WasiExit`. That matches `load` being
  Emscripten-only. If the layer ever runs on WASI, import the runtime's check instead
  of keeping a copy.
- `ScriptError.code` is a public mutable field, while `detail` is `readonly`. Make
  both `readonly`.
- `registerCommand`'s cast `cd.newCommandDef(spec.id) as CommandDef` hides the generated
  `| undefined`. If it were ever undefined, `def[Symbol.dispose]()` in `finally` would
  replace the real error with a TypeError. A one-line check costs nothing.
