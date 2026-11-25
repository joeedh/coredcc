# Feature report: TA1, the test app's first coredcc stage, 2026-11-23

- **Reviewed:** the API surface of P1f step 6 at `8289196`:
  - the page backend `apps/testapp/src/backend/wasm.ts` and its testbus methods, the
    startup in `src/main.ts`, and `info()` in `src/testbus.ts`;
  - the wire changes in `apps/testapp/wire.mjs` (`__proto__` keys, error `code`);
  - the build and test entry points: `build.mjs`, `ctest.mjs`, `nwjs/launcher.mjs`,
    `package.json`, `tsconfig.json`;
  - the C++ host `source/coredcc/testapp/host.cc`, its CMake, `tests/wasm/CMakeLists.txt`
    and `source/coredcc/bind/ts/gen.ts`;
  - the tests `apps/testapp/tests/ta1.test.ts` and `tests/wasm/spike.test.ts`.

  It was checked against the P1 plan's "P1f step 6: TA1 design" and the test-app plan
  (`docs/plans/2026-11-19-test-app.md`), with an eye on what TA2–TA4 will need.
- **Reviewer:** a fresh agent given the code and the design, not the author's reasoning.
- **Method:**
  - Built wasm-st Debug and RelWithDebInfo, and ran the `testapp` and `spike_ts` ctests
    on both.
  - A probe script (kept outside the repo) launched the app headless on a throwaway
    profile, through the app's own launcher and CDP client, and called testbus methods
    in scripted scenarios: values of each kind, errors, script commands defined over
    the wire, startup with different `--apptest-module` values.
  - A copy of `ta1.test.ts` with one extra test ran against the harness, and the real
    `ta1.test.ts` ran with a wrong module and during concurrent rebuilds.
  - Each finding says whether it was confirmed by **experiment** or by **code reading**.
- Findings are ranked by severity within each section. Each ends with its resolution;
  the regression tests are in `apps/testapp/tests/ta1.test.ts`, named by finding.

## Summary
- **What held up:**
  - **The four exit-criteria steps work, on both configurations.** The ctest passes on
    wasm-st Debug (28 s) and on RelWithDebInfo (7 s, built for this review). The
    release module also loads from a manual `--apptest-module`.
  - **Hiding `process` is clean.** After startup, `globalThis.process` has the same
    descriptor (writable, enumerable, configurable) whether or not the factory ran, and
    `process === globalThis.process`.
  - **Error codes cross the wire.** Each of these arrives as `ScriptError` with its
    code name and message:
    - NotFound, with "did you mean";
    - InvalidArgument, for a parse error, an empty path, or a member of an Int;
    - TypeMismatch (a Vec2 written to a Vec3);
    - OutOfRange (a 1.5 written to an Int);
    - FailedPrecondition ("nothing to undo", and a canRun that refuses);
    - AlreadyExists (a duplicate `defineCommand`);
    - Internal (a plain `Error` thrown in exec).

    A `TypeError` thrown by the TS layer arrives as a `TypeError` with no code.
  - **The step 5 protections hold over the wire:**
    - an `async exec` fails ("exec returned a Promise");
    - a `reset` called through the testbus from inside a command's exec is refused
      ("can't close the document from inside a command");
    - a duplicate registration doesn't leak its factory handle (`foreignCount` stays at
      1).
  - **Values:**
    - Vec3 values cross as arrays, and a `Float32Array` can be written;
    - `offset[1]` reads and writes a single component;
    - enums and flags can be written by name;
    - a bigint output (2^60) arrives as a bigint;
    - an output named `__proto__` survives in both directions.
  - **The build:**
    - the OBJECT library compiles `host.cc` once per configuration and links it twice;
    - `tsc -p .` and `tsc -p tests` are clean;
    - `spike_ts` passes on the moved host.
- **What didn't:**
  - A runaway script command (unbounded recursion) wedges the executor without
    poisoning the module. `check()` still reports the document valid, and the next
    `reset()` aborts the module. From then on every call fails with a misleading
    message, and `check()` reads the destroyed host and keeps reporting it valid.
  - Outside ctest, TA1 skips whenever the backend didn't load, for any reason, and
    still exits 0. A wrong module, or a rebuild running at the same time, turned all 7
    tests into skips.
  - `build.mjs` deletes `dist/backend-wasm.js` at the start of every build, so a page
    launched during a build has no backend.
  - When a test fails in `beforeEach`, the harness doesn't keep its artifacts, so the C++
    assertion text, which only reaches the console log, is lost.
  - The backend's shape (one wasm bundle, `install(register, modulePath)`, error mapping
    inside `wasm.ts`, and tests that expect `'wasm-st'`) will need reworking for TA4's
    native backend.

## Bugs and hazards
1. **A1 (high): unbounded recursion in a script command wedges the executor, and the
   next `reset()` aborts the module.** *Experiment.*
   - **Scenario:**
     - `defineCommand('r.inf', {}, 'class { exec(c) { cdcc.invoke(c, "r.inf") } undo() {} }')`,
       then `exec('r.inf')`.
     - The reply is a clean `ScriptError` "Internal: Maximum call stack size exceeded".
     - But the executor still believes a command is running. A later `undo()` gives
       "Busy: undo: not from a script command's exec". `liveInstances` is 9, with an
       empty history.
     - The next `reset()` replies "RuntimeError: Aborted(native code called abort())".
       The console shows `executor.cc:444: ... CommandExecutor destroyed while a command
       runs`. In a second run, where other commands were in the history, it was
       `foreign_command.cc:85: ... live_ == 0 (script command instances outlive the
       Runtime)`.
   - **Likely mechanism** (code reading): the JS `RangeError` escapes litestl's
     `dispatch` into wasm, and so unwinds the C++ frames of the outer `invoke` calls
     without running their destructors.
     - The adapter turns the first `RangeError` into a Status, and `dispatch` catches
       errors in its `try`. But both make wasm calls (`statusError`, `writeError`) to
       report the error. Near the stack limit those calls throw again, from inside the
       `catch`.
     - `isFatal` covers only `RuntimeError`, `ExitStatus` and `WasiExit`, so a
       `RangeError` doesn't poison the module.
   - Bounded recursion is fine: 40 nested `invoke`s ran and recorded one command.
   - **Why it matters:** a script command that recurses by mistake is an ordinary bug.
     Here it silently corrupts the document instead of failing cleanly. It is also a
     test that someone will write in TA2–TA4.
   - **Fix:**
     - In `CommandExecutor`, cap nesting (for example 64 levels) with a
       `ResourceExhausted` status. Native builds need this as well, because unbounded
       recursion there overflows the C stack.
     - In litestl's `dispatch`, poison the module when any exception would escape into
       wasm, including one thrown while writing the error result. Wrap the whole body,
       not just the method call.
   - *Fixed:* `CommandExecutor::kMaxNesting` (32) caps runSub nesting with OutOfRange
     ("commands nested deeper than 32"). 64 was too many: native Debug builds overflowed
     Windows' 1 MB stack at 52 levels on both compilers, with no JS involved
     (`test_command_review`, `test_nesting_cap`). In litestl, `dispatch` now poisons the
     module on anything that escapes it, and reports an unprintable thrown value instead
     of escaping: joeedh/litestl#11, merged 2026-11-24 as `1c5b7df`.
2. **A2 (medium-high): `check()` can't see the A1 state, and after an abort it reads the
   destroyed host.** *Experiment.*
   - `validate()` runs `Runtime::validateAll()` and `LinearHistory::validate()`. Neither
     checks that the executor is idle, or that live script instances match the history.
   - **Scenario A:** in a copy of `ta1.test.ts`, a first test that runs `r.inf` *passes*.
     Its `afterEach` `check()` reported `valid: true`.
   - **Scenario B:**
     - After `reset()` aborts, `doc` still points at the old host. `Session` zeroed the
       ctx, but not the `Host` wrapper, because the host's dispose aborted partway
       through.
     - `check()` then keeps returning `{valid: true, liveInstances: 9, foreignCount: 11}`,
       read from the destroyed host.
     - The other methods fail with "bound object was disposed".
   - **Fix:**
     - Have `testapp::validate` also fail when `ex_.running()` is true or the slot depth
       is not zero, and when `ForeignBindings::liveInstances()` is more than the number
       of script records in the history.
     - Track a fatal state in the backend (see A3), and make `check()` throw once it is
       set.
   - *Fixed:* `testapp::validate` fails while the executor is still running a command. A
     dead module (A3) makes `check()` refuse too. *Not done:* comparing live instances
     with the history: sub-commands hold instances too, so the counts legitimately
     differ.
3. **A3 (medium): a dead module isn't reported, and its errors are misleading.**
   *Experiment.*
   - After the abort in A1:
     - `info()` still reports `backend: 'wasm-st'` with no `backendError`;
     - `get` and `history` fail with "getValue argument 'ctx': bound object was
       disposed", and the testbus never says "the module aborted".
   - **Scenario:** in the `ta1.test.ts` copy, all 7 tests after the overflow failed in
     `beforeEach`, each with "RuntimeError: Aborted(native code called abort())". The
     assertion that explains it was only in the console log, which A5 then discards.
   - The TS layer's poisoning doesn't cover this. The abort happened in `host[dispose]`,
     not inside a foreign call (the same gap the step 5 review noted for T4).
   - **Fix:**
     - Wrap every backend method. On a fatal error (`WebAssembly.RuntimeError`,
       `ExitStatus`, or litestl's `isPoisoned`), record it with the last console error.
     - From then on, every method throws "the wasm module aborted: <first error>", and
       `info()` reports it in `backendError`.
     - Tests can then fail with the real cause.
   - *Fixed:* every backend method (`src/backend/methods.ts`) treats a trap, an exit or
     a poisoned module as fatal. It records the error with the module's last stderr
     lines (the C++ check's text), reports it as `info().backendError`, and every later
     call throws "the wasm-st module aborted: ...". Tested with a new
     `testapp::abortForTest`.
4. **A4 (medium-high): outside ctest, TA1 skips whenever the backend didn't load, for
   any reason.** *Experiment.*
   - `step()` skips when `info().backend === 'none'`. Only
     `COREDCC_TESTAPP_REQUIRE_BACKEND` (set by ctest) turns that into a failure.
   - The skip message is always "no wasm-st build", and `backendError` isn't shown.
   - **Scenario:**
     - `COREDCC_TESTAPP_MODULE=<...>/coredcc_testapp_node.mjs node --test tests/ta1.test.ts`
       gave pass 0, fail 0, skipped 7, and exit code 0.
     - The real error was "This page was compiled without support for Chrome browser".
   - `pnpm test`, which CLAUDE.md documents as the way to run the app's tests, behaves
     the same way. A regression in `wasm.ts`'s `install`, a renamed export, or a broken
     bundle would show up only as skips there.
   - **Fix:**
     - Skip only when the build has no backend. For example, have `install` not run at
       all when `dist/backend-wasm.js` is missing or the module file doesn't exist, and
       report `backend: 'none'` with `backendMissing: true`.
     - Treat every other load failure as a test failure.
     - Put `backendError` in the skip or failure message.
   - *Fixed:* the launcher decides: `--apptest-no-backend=<why>` when the build has no
     `dist/backend-wasm.js`, or no module at the default path with none named. Only then
     does the page report `backendMissing`, and only then do the tests skip, with the
     reason. Any other load failure fails the suite. Checked: no bundle gives 13 skips;
     the node variant fails with "is the node variant".
5. **A5 (medium): a rebuild running at the same time turns TA1 into skips.**
   *Experiment.*
   - `build.mjs` removes `dist/backend-wasm.js` first, then spends several seconds
     generating `gen/`, and writes the bundle last. A page launched in that window has
     no backend.
   - **Scenario:**
     - 12 `node build.mjs` runs looped in the background while
       `node --test tests/ta1.test.ts` ran 3 times.
     - Run 1 skipped all 7 tests and exited 0. Runs 2 and 3 passed.
     - Under ctest the same race fails instead (REQUIRE_BACKEND). That happens, for
       example, when the debug and release presets' ctests run at once, since both
       write `apps/testapp/dist` and `gen/`.
   - The plan only says "don't" run two builds at once. Nothing enforces it.
   - **Fix:**
     - Build into a temporary directory, then rename `backend-wasm.js` (and its map)
       into place. Delete it only when the build has no module.
     - Generate into a per-run temporary `gen/`, or take a lockfile in `apps/testapp`
       for the duration of the build.
   - *Fixed:* `build.mjs` writes every output to a temporary name and renames it into
     place (retrying while Windows holds the target), and generates into a private `gen`
     directory it swaps in last. It deletes the backend bundle only when it has no
     module. Checked: two test runs during six back-to-back builds passed 13/13 each.
6. **A6 (medium-low): when a test fails in `beforeEach`, its artifacts are deleted.**
   *Experiment.*
   - `failed` is set only inside `step()` and when `check()` returns `!valid`. It is not
     set when the `reset` in `beforeEach` throws, or when `check()` itself throws.
   - **Scenario:** in the A1 run, 7 tests failed and `test-artifacts/` gained no
     directory. `cdp-log.json` held the only copy of the C++ assertion text, and it was
     discarded.
   - **Fix:** set `failed = true` in a `try`/`catch` around the bodies of `beforeEach`
     and `afterEach`. Or, simpler, in `after`, check whether any test in the suite
     failed.
   - *Fixed:* the hooks run through `tracked()`, which marks the run failed on any
     throw.
7. **A7 (low-medium): a relative `--apptest-module` resolves against `apps/testapp`,
   not the caller's directory.** *Experiment.*
   - The launcher resolves `opts.module` and `COREDCC_TESTAPP_MODULE`, but passes an
     `--apptest-module=` given in `appArgs` through unchanged. The page resolves it
     against NW.js's working directory, which is `apps/testapp`.
   - **Scenario:** from the repo root,
     `node apps/testapp/nwjs/launch.mjs --ephemeral --headless --apptest-module=build/wasm-st/.../coredcc_testapp.mjs`
     gives `backend: 'none'`, "Failed to fetch dynamically imported module:
     file:///C:/dev/coredcc/apps/testapp/build/...".
   - `info().module` also reports the path as it was given, not resolved.
   - **Fix:** in `launch()`, resolve an `--apptest-module=` app argument against
     `process.cwd()`. The page should report the resolved path.
   - *Fixed:* the launcher resolves an `--apptest-module` app argument against its own
     working directory, and `info().module` reports the resolved path.
8. **A8 (low): error replies lose information.** *Experiment and code reading.*
   - The backend copies `ScriptError.detail` onto the error it rethrows, but `errorInfo`
     copies only `name`, `message`, `stack` and a string `code`, so `detail` never
     crosses.
   - The rethrown `Error` has its own stack (the wrapper in `wasm.ts`), not the stack of
     the `ScriptError`.
   - Loading the node variant by mistake reports "This page was compiled without
     support for Chrome browser. Pass -sMIN_CHROME_VERSION=149 or lower". That points
     at the compiler flags, not at the wrong file.
   - `errorInfo` now also copies Node's string codes (`ENOENT` from `shot({outPath})`).
     That is harmless, but `code` no longer means "a StatusCode name".
   - **Fix:**
     - Copy `detail` in `errorInfo`, and set `out.stack = e.stack`.
     - In `instantiate`, if the file name ends in `_node.mjs`, say "this is the node
       variant; the page needs coredcc_testapp.mjs".
     - Either document `code` as "StatusCode name or Node error code", or put the
       StatusCode in its own field (`status`).
   - *Fixed:* `errorInfo` copies a string `detail`, and the rethrown error keeps the
     `ScriptError`'s stack. Loading a `_node.mjs` module says it is the node variant.
     `code` is documented as a StatusCode name or a Node error code.
9. **A9 (low): `reset()` isn't exception-safe.** *Code reading.*
   - `reset` disposes of the old session and then calls `open()`. If `open()` throws,
     `doc` still holds the disposed session, and every later call fails with "bound
     object was disposed".
   - If `registerCommand` throws inside `open()`, the new host and session are never
     disposed. They wait for GC.
   - It can't happen today: the page's command spec is fixed. It will matter once
     `reset(sceneName)` builds real scenes.
   - **Fix:** in `open()`, dispose of the session if anything after `create()` throws.
     In `reset`, assign `doc` only on success, and record a "no document" state that
     methods report clearly.
   - *Fixed:* `openDoc` disposes of its session if registering the page's command
     throws, and `reset` leaves "no document: reset failed: ..." for later calls instead
     of a disposed one.

## API shape for TA2–TA4
1. **S1 (medium): the backend is wasm-only in its shape, not just its loading.** *Code
   reading.*
   - The test-app plan's layout has `backend/wasm.ts, native.ts: same surface over the
     generated bindings`, and TA4 runs every test against both backends. Today:
     - `main.ts` hardcodes `./backend-wasm.js` and the `--apptest-module` argument;
     - `install(register, modulePath)` both loads the module and defines every testbus
       method;
     - the `ScriptError` mapping and `wireValue` live inside `wasm.ts`;
     - `ta1.test.ts` asserts `info.backend === 'wasm-st'` and `module` matching
       `/coredcc_testapp\.mjs$/`.
   - A native backend written the same way would copy all of the method code, and the
     copies would drift apart.
   - **Fix:**
     - Split `wasm.ts` into a loader that returns a small `Backend` object (`ctx()`,
       `host`, `foreignCount()`, `open()`, plus `kind` and `module` for `info()`), and a
       shared `methods.ts` that registers the testbus methods and maps errors over any
       `Backend`.
     - Choose the backend with `--apptest-backend=wasm-st|native`.
     - Have tests read the expected backend from an environment variable rather than
       hardcoding `'wasm-st'`.
   - *Fixed, in part:* the loader (`wasm.ts`) hands a `LoadedBackend` (kind, module, the
     binding backend, `isFatal`, recent stderr) to shared methods (`methods.ts`). Tests
     read the expected backend from `COREDCC_TESTAPP_BACKEND`. *Left for TA4:* choosing
     the backend (`--apptest-backend`) and its bundle in `main.ts`.
2. **S2 (medium-low): pointer values cross as `{text}`, which can't be written back, and
   that branch is never run.** *Experiment and code reading.*
   - `wireValue` turns a PropValue into `{text: valueToString(v)}`. `set(path, {text})`
     fails with "TypeError: fromJs: can't make a value of [object Object]".
   - The test-app plan says "64-bit ids and StableRefs are bigints". TA2–TA3 tests that
     set an active object or a reference through the testbus will need a value they can
     write back.
   - `testapp::Data` has no pointer member, and no facade makes a pointer value, so the
     PropValue branch of `wireValue` never runs in any test.
   - **Fix:** before TA2, decide the wire form of a pointer (for example
     `{$ref: <bigint StableRef>, text}`), accept that form in `set`, and add a pointer
     member to the host so the round trip is tested.
   - *Deferred to TA2:* the writable wire form of a pointer, and a pointer member to
     test it. Noted in `methods.ts`.
3. **S3 (low-medium): enums and flags are written by name but read back as numbers.**
   *Experiment.*
   - `set('mode', 'EDIT')` and then `get('mode')` gives `1`. `set('flags', 'SELECT')`
     works, and `get('flags')` gives `3` after `set('flags', 3)`.
   - `set('flags', ['SELECT'])` fails with a vector-length TypeError, so the only way to
     write several flags is a number.
   - The property panel (TA2) and tests that assert on it need item names and ranges.
     The plan defers that to `meta(path)`.
   - **Fix:** with `meta(path)`, return the enum and flags items. Consider a
     `get(path, {names: true})` option that returns identifiers, so tests can assert on
     names.
   - *Deferred to TA2:* item names come with `meta(path)`.
4. **S4 (low-medium): `defineCommand` can't declare a Float input whose default is a
   whole number.** *Experiment.*
   - `float()` can't cross the wire. `inputs: {factor: 1}` declares an Int, so
     `exec('r.floatIn2(factor=0.5)')` fails with "OutOfRange: factor: 0.5 is not an
     integer". `inputs: {factor: {value: 1}}` fails with a TypeError.
   - The plan says TA1 doesn't need it. Tool commands in TA3 (scale, strength, opacity)
     will.
   - **Fix:** accept a wire marker in `defineCommand`'s spec, such as `{$float: 1}`, and
     map it to `cdcc.float(1)`.
   - *Fixed:* `defineCommand` takes `{$float: n}` in inputs and outputs.
5. **S5 (low): `resolve` returns only `true`.** *Code reading.*
   - It can't report the resolved type or the canonical path. TA2's `meta` will cover
     this; until then, `resolve` adds nothing that `get` doesn't.
   - **Fix:** when `meta` lands, have `resolve` return the resolved kind, or merge it
     into `meta`.
   - *Deferred to TA2:* `meta(path)`.

## Harness and CI
1. **H1 (low): the node version check lets 23.0–23.5 through.** *Code reading.*
   - `ctest.mjs` skips below 22.18. Type stripping is on by default only from 22.18 and
     from 23.6, so node 23.0–23.5 passes the check and then fails on the `.ts` files.
   - **Fix:** also skip when `major === 23 && minor < 6`. Or feature-test by running
     `node -e` on a one-line `.ts` file.
   - *Fixed:* `ctest.mjs` also skips node 23.0-23.5.
2. **H2 (low): `build.mjs` builds without the backend, exits 0, and leaves `gen/`
   stale.** *Code reading.*
   - `node build.mjs --node-module <typo>` warns and exits 0 with no backend.
   - Without the module, `gen/` from an earlier build is left in place, so
     `pnpm run typecheck` checks against old bindings. With no earlier build, it fails
     with unresolved `coredcc-gen/...` imports.
   - **Fix:** an explicit `--node-module` that doesn't exist is an error (exit 1). When
     the backend is skipped, remove `gen/`, and have `typecheck` say why it can't run.
   - *Fixed:* a `--node-module` that doesn't exist exits 1, and a build without the
     module removes `gen/`.
3. **Held up:**
   - ctest registration: `testapp` is under `tests/wasm`, has `SKIP_RETURN_CODE 77` and
     `TIMEOUT 600`, and uses `$<TARGET_FILE:...>`, so each configuration tests its own
     modules.
   - `ctest.mjs` checks that the build produced `backend-wasm.js` before running tests.
   - No NW.js processes outlived any run in this review.
   - Cost: TA0's tests now load the 15 MB Debug module on every launch. The whole
     Debug `testapp` ctest still takes 28 s.

## Test gaps (`apps/testapp/tests/ta1.test.ts`)
- *Resolution:* covered now: FailedPrecondition from undo and redo, a refusing canRun,
  a TypeError crossing with its name and no code, `detail`, vector, bool and string
  outputs, flags, a Float input, runaway recursion, a wrong module, and an aborted
  module. Still open: the pointer branch (S2), and an Int past 2^53 through `get`
  (the host has no int64 member).
- **Assertions that can't fail:** `afterEach`'s `check().valid` misses a wedged executor
  and leaked script instances (A2). `foreignCount` and `liveInstances` from `check()`
  are asserted only once, in step 4.
- **Branches never exercised:**
  - `wireValue`'s PropValue (pointer) branch (S2);
  - a non-`ScriptError` error crossing the wire (the TS layer's `TypeError`, or
    `SyntaxError` from `defineCommand`);
  - FailedPrecondition from `undo` or `redo` with nothing to undo or redo;
  - canRun refusing, and an undo that throws (reported as Internal), over the wire;
  - outputs other than numbers: vectors, bools, strings, bigints. All of these worked in
    the probe.
- **Values:**
  - no Int member can hold more than 2^53 (`count` is `int`), so the bigint read path
    through `get` is untested;
  - flags are never read or written by the suite.
- **Startup:** nothing checks `info().backendError` on a failed load, or that a wrong
  module fails rather than skips (A4).
- **Fatal paths:** nothing covers a module abort or poisoning, and what the testbus
  reports afterwards (A1, A3).

## Design doc and code
- *Resolution:* the plan now says one launch for the main suite. A real build id is
  deferred: the module path in `info()` and the launcher's explicit module cover the
  mix-ups seen so far.
- The plan says `ta1.test.ts` runs "over one launch per test". The file uses one launch
  for the whole file, with `reset()` between tests, which is what the file's own doc
  comment says. The plan's wording should match.
- The test-app plan's `info()` has a "build id". TA1 reports `build: 'ta1'`, a stage
  label that is set even when there is no backend. A real id would catch a page bundle
  generated from one module and run against another; the module's mtime, or a hash
  baked into the bundle, would do.
- The plan says TA1's tests skip "without a wasm-st build". The code skips on any load
  failure (A4).
- Everything else in the design is implemented as written:
  - the startup order;
  - hiding `process`;
  - two bundles, with the module chosen at launch;
  - the gating on `COREDCC_BUILD_TESTS` and not wasm-mt;
  - the tests registered under `tests/`;
  - `__proto__` keys over the wire;
  - string codes only;
  - pointer copies disposed of in `get`.

  The plan's one unverified item, the OBJECT library linked into two executables,
  now works on both configurations.

## Small notes
- *Resolution:* `registerCommand` refuses a non-class with a TypeError, `registerMethod`
  refuses a duplicate name, and `check()` documents the page factory in `foreignCount`.
  The clamping of a ranged Float is the property layer's choice, unchanged.
- `defineCommand(id, spec, '42')` fails with "Cannot read properties of undefined
  (reading 'redo')". `registerCommand` could check that `Cls` is a constructor and say
  so. No handle leaks in that case.
- Backend methods are registered before `registerCoreMethods`, so a backend method named
  `info` or `echo` would be silently replaced. A `registerMethod` that throws on a
  duplicate name costs nothing.
- `set('strength', 2)` on a 0–1 Float member succeeds and clamps to 1. That is the
  property layer's choice, not TA1's, but tests that expect OutOfRange on a ranged Float
  will be surprised.
- `reset()` returns `{foreignCountBetween}`, and `check().foreignCount` includes the
  page's own factory (a baseline of 1). Document both, or have `check()` subtract the
  baseline.
