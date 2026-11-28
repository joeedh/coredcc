# P6a: the native N-API addon

- Status: pressure-tested 2026-11-28 and revised; the record is the last section.
- Phase: the first part of P6 (architecture §14, "P6 — Integration"; task list row 11).
- Depends on: litestl's N-API runtime, which is on litestl master and in coredcc's pin
  (`extern/litestl/binding/napi/`); the naming pass (row 10b).
- TA4 of the [test app plan](2026-11-19-test-app.md) is part of this plan.

## Goals
- Build coredcc as a Node-API addon (`.node`) on Windows, against litestl's N-API runtime, with the same
  generated TS and the same TS layer (`source/coredcc/bind/ts/`) as the WASM backend.
- Load it in NW.js 0.112 (Node 26.1) and in plain Node 24, from one binary.
- Run the Node TS suites (`tests/wasm/*.test.ts`) and the test app's TA1–TA3 against both backends.
- Settle the architecture's unverified claims about N-API early: reverse marshalling, off-thread foreign calls,
  external ArrayBuffers, and the NW.js link.
- Add the addon to the gates and CI without making CI's wall-clock time longer.
- Decide how registered facade names map to N-API exports (deferred by the naming pass).

## Non-goals
- The faberforge spike, the ubertopo spike and `UnrealHistory`. They are later parts of P6 (P6b–P6d) and get their
  own plans. P6a gives them an addon to build on.
- Electron. The design keeps it possible (see "Buffers"), but nothing here is tested in Electron.
- Linux and macOS. Only Windows is built (`docs/building.md`).
- Running coredcc from Node worker threads, or from more than one `napi_env` at a time.
- A zero-copy DrawList on N-API, unless the measurement in step 5 calls for it.
- Async or Promise-returning facades. Calls stay synchronous, as on WASM.
- Python.


- The research (what the code says, what was measured) and the full risk table are in
  [the appendix](2026-11-28-p6a-napi-addon-risks.md). The plan below cites it as "appendix".

## Key decisions
1. **One binary for Node and NW.js,** through a delay-load hook, not two builds. N-API is ABI-stable, and
   `NAPI_VERSION=8` is below what both runtimes have (10). Measured twice (appendix, R4).
2. **The addon is built in the existing native presets,** not in a new preset. It links objects those presets
   already compile, so it costs a few translation units and two links, not a second build of coredcc.
3. **The addon's exports are litestl's backend surface only.** Facades are reached by their registered names
   through `getFunction`, as on WASM.
4. **The TS layer gets a backend-neutral `load`,** and invalidates contexts through the backend, which needs a small
   litestl PR (`detach`).
5. **One `napi_env` per process.** The addon refuses a second one, on any thread.
6. **An orderly teardown at Node exit:** open sessions are disposed while litestl's foreign host still exists.
7. **DrawList bytes are copied once per read on N-API,** through the existing chunked path.
8. **Script tests carry ctest labels,** and each gate step says which labels it leaves out, because the gate runs
   every registered test (`gate.mjs` runs `ctest --preset` with no filter).

## Design

### Targets and files
- `cmake/CoredccNapi.cmake` (new): finds the headers and defines
  `coredcc_napi_addon(<target> OUTPUT_NAME <name> SOURCES ... DEPS ...)`.
  - It calls litestl's `lt_add_napi_addon`, then adds `LINKER:/DELAYLOAD:node.exe`, `delayimp.lib` and
    `delay_load.cc`, and applies `coredcc_target_flags`.
  - The output is `<name>.node` in the target's config folder, with its PDB.
- `source/coredcc/bind/napi/` (new, compiled only when `COREDCC_NAPI` is on):
  - `addon.h`, `addon.cc`: `coredcc::script::napi::install(napi_env, napi_value exports, BindingManager *)`. It
    builds litestl's `Runtime` under `PermanentGuard`, installs the exports, and refuses a second environment (see
    "Threads and environments"). Every addon's init is one call to it.
  - `delay_load.cc`: the hook, about 15 lines, with the reason in a comment.
  - `coredcc_addon.cc`: `NAPI_MODULE` over `script::bindings()`. Target `coredcc_napi`, output `coredcc.node`.
- `source/coredcc/testapp/`:
  - The manager export moves out of `host.cc` into `wasm_entry.cc` (Emscripten) and `napi_entry.cc` (N-API), and
    `host.cc` drops `emscripten.h`.
  - `CMakeLists.txt` builds the host natively too, as `coredcc_testapp_napi` with output `coredcc_testapp.node`, when
    `COREDCC_NAPI` and `COREDCC_BUILD_TESTS` are on.
  - `napi_entry.cc` turns off the CRT's abort and assert dialogs, as `tests/support/no_dialogs.cc` does. When the
    page passes it a log path, it also sends the addon's `stderr` there (see "Errors"). Only the test addon does
    this: a library has no business changing a host's CRT behaviour.
- `tools/install-node-api.mjs` and `tools/node-api.json` (new): download the pinned node-api-headers (1.5.0, the
  version litestl pins) from npm into `build/deps/node-api-headers`, checking the tarball's `integrity` hash. A
  worktree uses the main checkout's copy, as the emsdk does (`cmake/Emscripten.cmake:5-16`).
- `tests/napi/CMakeLists.txt` (new): the ctests and their labels (see "Tests"). `tests/napi/nw/`: a minimal NW.js
  app for `napi_load_nw`.
- `apps/testapp/src/backend/native.ts` (new), bundled as `dist/backend-native.js`.
- `apps/testapp/nwjs/launcher.mjs`: an `appDir` option (default the test app), so `napi_load_nw` can launch its own
  app; and an `addon` option beside `module`.

### Options and presets
- `COREDCC_NAPI`: on by default when coredcc is the top-level project and not Emscripten; off for host projects.
  - On, it sets `LITESTL_NODE_API_HEADERS` with `CACHE ... FORCE` before `add_subdirectory(extern/litestl)`, to
    `COREDCC_NODE_API_HEADERS` (default `build/deps/node-api-headers`, then the main checkout's). `FORCE` because
    litestl's variable is a `CACHE PATH` (`extern/litestl/CMakeLists.txt:37`), and existing build trees hold
    litestl's own copy in it (`build/native-clang/CMakeCache.txt:272`).
  - On and without headers, configuring fails and names `node tools/install-node-api.mjs`. Missing dependencies
    are a failure, as they are for the emsdk. The option, the tool and CI's install step land in one commit
    (step 1), so no checkout or CI run meets the option before it can get the headers. Other checkouts on this
    machine find the main checkout's copy once it has run the tool.
- `binding_napi` joins coredcc's `SYSTEM` loop over litestl targets (`CMakeLists.txt:62-66`), so a host that
  supplies litestl without `SYSTEM` doesn't expose `node_api.h` to `/W4 /WX`.
- No new presets. `native-clang-*` and `native-msvc-*` build `coredcc.node` and `coredcc_testapp.node`.

### Loading
- **Node:** `require(path)` (or `createRequire(import.meta.url)(path)` from ESM) returns the backend.
- **NW.js:** `require(path)` from the page, which runs in NW.js's mixed Node and browser context. The test app passes
  the path as `--apptest-addon=<path>`, as it passes `--apptest-module` today.
- **The hook:** the import library names `NODE.EXE`. The hook answers that name with `GetModuleHandle("node.dll")`
  (NW.js) or, when that is null, `GetModuleHandle(nullptr)` (node.exe, and Electron, whose executable exports
  N-API). Without the hook the NW.js page dies at the `require`, before a `catch` runs (pressure test).
- **TS:** `cdcc.load(backendOrModule, getManager?, opts?)`:
  - an object whose `kind` is `'napi'` and that has `getFunction` is used as the backend as it is;
  - anything else goes the Emscripten path, as today.
  - Both then install the `coredcc::Status` return converter (`core.ts:160-162`), and the one-module-per-realm rule
    stays: a second addon object throws unless `replace`.
- `gen.ts` and `tests/wasm/gen_paths.ts` take either a WASM module or an addon. `run.mjs --addon` passes the addon
  to both. The test app's `build.mjs` generates from whichever it is given (`--node-module` or `--addon`).

### Dispatcher
- Nothing changes in coredcc's dispatcher. `install` makes litestl's N-API host current for the thread, and
  `ForeignBindings::call` reaches it through `foreign::call`.
- The facades' Busy rules, sticky errors and per-slot limits are the same on both backends, because they live in
  `ForeignBindings` and the facades, not in the host.

### Errors and exceptions

| What happens | WASM | N-API |
|---|---|---|
| A coredcc call fails | a `Status` or `Result`; the TS layer throws `ScriptError` | the same |
| Wrong argument count or type, a disposed wrapper | the runtime throws `TypeError` or `Error` | the same, with litestl's wording (the conformance suite holds them to the same tests) |
| A JS slot throws, returns the wrong type, or returns a Promise | the slot's `Status` is Internal with `<method>: <message>`, or the sticky error | the same |
| A JS exception is already pending when C++ calls a slot | — | the call is refused with an error; the exception stays for JS |
| `CDCC_CHECK` fails, or `abort()` | a trap; the module is poisoned; `isFatal` is true | the process ends with code 3 and no dump. In Node the message is on stderr. In NW.js the renderer is gone, CDP closes, and nw.exe still exits 0 |
| C++ exceptions | off | off |

- `isFatal` stays false for every N-API error: nothing survives a fatal one to be asked.
- The test app's `LoadedBackend.isFatal` for native is `() => false`. A crash is the harness's to report:
  `crashed()` is true when the CDP socket closes (`harness.ts:57-59`). An exit code can't tell a crash from a quit.
- **The message in NW.js:** renderer stderr doesn't reach nw.exe's. `CDCC_CHECK` prints with `fprintf(stderr)` and
  then calls `abort()` (`base/assert.cc:32-42`), so a CRT report hook never sees the text. The test addon instead
  reopens its `stderr` onto `<--apptest-log>.stderr` at init, and the harness reads that file after a crash.

### Threads and environments
- All calls into the addon come from the thread that loaded it. coredcc's rule (`docs/embedding.md:155-159`) is
  unchanged.
- **Workers are safe for what coredcc runs on them today.** Natively `poolUsable()` is true, so BVH builds and
  refits of large inputs use litestl's task pool while the JS thread blocks. They read gathered data only.
- **A foreign slot called off the main thread fails with an error; it does not deadlock.** litestl's N-API host is
  per thread, so the call returns "no foreign host" and the forwarder records it. N-API itself doesn't check
  threads (a stray `napi_create_int32` from a worker returned `napi_ok`), so litestl's host is the only guard.
  - A `release` off the main thread is a no-op, so that handle leaks. Nothing releases off the main thread today.
- **`ThreadAffine` stays unread.** No parallel site calls a slot. Reading it is deferred until one needs to;
  `embedding.md` says so.
- **One `napi_env`.** `install` records the first env. Any later init with another env throws "coredcc's addon is
  already in use by another environment", whatever its thread.
  - Why not only other threads: `process.dlopen` of the loaded file re-runs init on the same thread with a new env,
    and litestl's `installForeign` would call `setHost` with a second host, so every handle of the first env would
    become unknown.
  - `install` registers its env cleanup hook once, with the env-specific state as its argument: a second
    `napi_add_env_cleanup_hook` with the same `(fn, arg)` aborts Node.
  - A second `require` in the same env returns Node's cached module and doesn't call init.

### Teardown
- **Node at exit** runs the env cleanup hooks first, newest first, which removes litestl's foreign host. Then it runs
  every wrapper's finalizer, newest first, with JS calls failing. So every owned C++ object still in JS is destroyed
  in reverse creation order, with no foreign host. WASM never runs this code.
- **The order coredcc arranges:** when `load()` takes an addon, the TS layer adds one `process.once('exit')`
  listener. Node emits `exit` before it tears the env down. The listener disposes every open `Session` (the TS
  layer keeps them in a `Set`), newest first, while the foreign host exists. Each dispose runs the normal sequence,
  `disableDrivers` before the Library (`core.ts:207-210`). A session whose dispose is refused (a slot in
  progress, when `process.exit` is called from inside one) is left to its finalizer.
- **What remains for the finalizers:** hosts that no `Session` owns, and plain owned values (results, defs, views).
  - A host's destructor already destroys the executor and history before its Runtime, so `ForeignBindings`'
    `CDCC_CHECK(live_ == 0)` holds.
  - `foreign::release` without a host is a no-op, so dropping handles is safe.
  - No owned value other than a host holds script instances. Step 3 checks that claim with a test rather than
    trusting it.
- **NW.js** runs no teardown: the renderer is ended. Nothing to order, and nothing to test there beyond `crashed()`
  being false after a quit.

### Buffers
- **DrawList:** `readDrawList(list)` copies the bytes into a JS `Uint8Array` with `drawListRead` in 64 KB chunks
  (`drawlist.ts:166-170`). On N-API each chunk's `Span<uint8>` is the typed array's own memory
  (`napi_runtime.cc:880-888`), so the bytes are copied once, by C++, into JS memory.
  - `{view: true}` has no heap to view on N-API and returns that copy. Its doc comment already says "where there is
    one". `DrawList::address()` is meaningless on N-API; its comment says so.
- **Zero-copy later:** NW.js 0.112 allows external ArrayBuffers (measured). Electron refuses them. A view would need a
  litestl return mode that makes an external buffer and falls back to a copy on `napi_no_external_buffers_allowed`,
  and a lifetime rule (valid until the next `begin()`). That waits for step 5's measurement.
- **Other bytes** (`ScriptBytes`, saved files) are `Vector<uint8>` returns, copied into a `Uint8Array`
  (`napi_runtime.cc:1041-1051`), as on WASM.

### Export names of facades (the naming pass's deferral)
- The addon exports only the backend surface. There are no per-facade exports.
- A facade's registered name (`coredcc::getValue`) is its key on both backends, and it decides the generated TS file
  (`coredcc-gen/coredcc/functions.ts`). Nothing maps names for N-API, so nothing can differ between backends. The
  pressure test found the generated TS byte-identical between `coredcc_gen_ts` and the wasm-st module.
- So the `coredcc::` prefix stays, although the C++ functions are in `coredcc::script`. `naming.md` gets a line
  saying that the registered namespace is the script namespace, not the C++ one. This closes finding 24 of the
  naming report.
- The naming pass's other "Deferred to P6" items are not about N-API. See open question 7.

## Risks
The appendix has the table (R1–R12), with what the pressure test changed. In short:
- R5, the use after free of a disposed `session.ctx`, is the one real bug found; step 2 fixes it.
- R7 (teardown at Node exit) and R12 (a second env through `dlopen`) are new failure modes, designed for above.
- R1 (coredcc's interfaces over N-API) is the largest unknown left; step 3 runs everything the suites have.
- R8 is settled by a type scan, not by loading: `checkFunctions` checks no signatures.

## Steps
Each step is one or more commits that build and pass the pre-commit gate and CI on their own. A step that needs a
litestl PR waits on a local branch until it merges (`docs/implementation-tasklist.md`, "Where work happens"). Each
step that adds a test also adds what the test needs to the jobs that run it.

1. **The addon loads, everywhere the gates build it (R4, R8, R9, R12).**
   - `tools/install-node-api.mjs`, `tools/node-api.json`, `cmake/CoredccNapi.cmake`, `COREDCC_NAPI` (with `FORCE`),
     `binding_napi` in the `SYSTEM` loop.
   - `source/coredcc/bind/napi/`: `install` (with the one-env rule and a single cleanup hook), the hook, `coredcc.node`.
   - The gate's labels (see "Gates and CI"), the `napi` matrix key, and the workflow's header step, for every native
     job. `native-clang-debug` gets `scripts: true` and its installs.
   - The ctest `napi_load` (label `napi`), a Node script that requires `coredcc.node` and checks:
     - `kind === 'napi'`, and `bindingKeys()` includes `coredcc::Status` and `coredcc::Context`;
     - the type scan: every parameter and return of every function, method, constructor and foreign slot is a kind
       the N-API runtime marshals (the pressure test's `scan.ts`, ported to the backend's descriptors);
     - `generateTypeScript()` equals what `coredcc_gen_ts` writes, file for file;
     - a call: `getFunction('coredcc::statusText')` on a constructed `Status`;
     - `process.dlopen` of the same file again throws the one-env error.
   - The ctest `napi_load_nw` (label `napi-nw`): the same load and call in NW.js, through `tests/napi/nw/` and the
     launcher's `appDir`.
   - Testable: both ctests in `native-clang-debug`; `napi_load` in the other three native presets.
2. **The TS layer is backend-neutral (R5).**
   - litestl PR: `IBindingBackend.detach(obj)`. On a non-owning wrapper, later member access, method calls and use as
     an argument throw "bound object was disposed". WASM sets `ptr = 0`; N-API clears `Wrapped::ptr`. Owning wrappers
     are refused (dispose them instead). With a conformance test, and a line in `documentation/binding.md`.
   - coredcc, after it merges: `Session[Symbol.dispose]` calls `backend.detach(ctx)`; `checkCtx` keeps a `WeakSet` of
     detached contexts for its own message; `load()`, `gen.ts` and `gen_paths.ts` take an addon; the session `Set`
     and the exit listener of "Teardown".
   - A new test in `spike.test.ts`: `get(session.ctx, 'count')` after `session[Symbol.dispose]()` throws
     /disposed/. It runs on wasm-st now and on N-API from step 3.
   - Testable: `spike_ts` on wasm-st.
3. **The Node suites on the addon (R1, R2, R6, R7, R11, R12).**
   - The test app's host builds natively as `coredcc_testapp.node` (the `host.cc` split).
   - `tests/wasm/run.mjs --addon <path>`: generates the TS and the typed paths from the addon, type-checks, bundles,
     and runs the suites with `SPIKE_BACKEND=napi`. `fixture.ts` loads whichever it is given.
   - Backend-specific tests are registered only for their backend, as two named tests rather than a skip. The
     runner fails if any test reports `skip`, so a skip can't hide inside a passing ctest.
     - T4/T14's native twin `dlopen`s `coredcc_testapp.node` again and expects the one-env refusal.
   - New tests: the off-thread call (R2), GC release (R6), and the teardown test (R7). See "Tests".
   - The ctest `spike_napi` (label `napi-node`). Testable: `ctest -R spike_napi`.
4. **TA4: the test app on the native backend (R10).**
   - `src/backend/native.ts`; `main.ts` picks the bundle from `--apptest-addon` or `--apptest-module`; the launcher
     takes `addon`; `build.mjs --addon` bundles `backend-native.js`.
   - TA1–TA3 read the expected backend from `COREDCC_TESTAPP_BACKEND` (TA3 too). The WASM-only tests get native
     twins:
     - A3: `abortForTest` ends the renderer. The test asserts `crashed()`, and finds the `CDCC_CHECK` text in
       `<--apptest-log>.stderr`. It expects no dump.
     - A4: a wrong file (the WASM module given as `--apptest-addon`) fails to load and is reported.
     - A clean quit with an open document and script commands: `crashed()` stays false.
   - `ctest.mjs --addon <path>` runs `tests/**/*.test.ts` with `COREDCC_TESTAPP_BACKEND=native`. The ctest
     `testapp_napi` (label `napi-nw`), `RUN_SERIAL`, as `testapp` is.
   - Testable: `ctest -R testapp_napi` in `native-clang-debug`.
5. **DrawList on N-API, measured.** Time `readDrawList` and `drawTools` per frame for TA3's scene and for a 10,000
   segment list, on both backends. If the copy is under 5% of the frame's coredcc time, close R3 as "not needed" and
   record the numbers here. Otherwise plan the litestl external-buffer return as its own PR.
6. **Documents.** See "Documentation". Then the feature review (task list, "Feature reviews"). Record the CI job
   times here.

## Tests
- **Native (C++):** nothing new in `tests/` for the library. `install`'s one-env refusal needs a real env, so it is
  tested from Node.
- **`napi_load` (Node) and `napi_load_nw` (NW.js):** as step 1.
- **`spike_napi`:** the five suites of `tests/wasm/` (`spike`, `ui`, `scene`, `drive`, `tool`; 80 tests) on the
  addon, plus:
  - **Off-thread (R2):** a test-only facade, `testapp::callSlotOffThread(ctx, ...)`, calls a script slot from one
    `std::thread` that it starts and joins (not the task pool: `parallel_for` runs a band on the calling thread,
    which would race on `ForeignBindings::frames_`). The call returns Internal with "no foreign host", within a
    second. On wasm-st the facade can't start a thread: it calls the slot on the caller's thread and returns a flag
    that says so, so the shared suite type-checks and runs on both, and asserts per backend.
  - **GC (R6):** drop a host with script commands; `gc()`, `await new Promise(setImmediate)`, `gc()`, and once more;
    then the host has been finalized (a `FinalizationRegistry` callback has run) and `foreignCount()` is back where
    it started.
  - **Teardown (R7):** a child `node` process opens two sessions with script commands, undo records and a graph
    kernel, plus a host that no `Session` owns, and exits without disposing anything. Its exit code is 0 and its
    stderr has no assertion. A second child does the same with the exit listener removed, which tests the
    finalizer-only path that a host without sessions gets.
  - **The disposed context (R5):** as step 2, on N-API.
- **Test app (`testapp` and `testapp_napi`):** every TA0–TA3 test on both backends, with the native twins of A3 and
  A4, and the clean-quit test.
- **Not tested:** Electron, worker threads beyond the refusal, release-CRT NW.js builds of the test addon, and a task
  pool that is busy at Node exit.

## Gates and CI
- **Labels:** `napi` (`napi_load`: needs Node only), `napi-node` (`spike_napi`: needs `npm ci` in `tests/wasm`),
  `napi-nw` (`napi_load_nw`, `testapp_napi`: need `pnpm install` in `apps/testapp`). A gate step gains `exclude`, a
  label regex passed to ctest as `-LE`. The labels are set in `tests/napi/CMakeLists.txt`, not in the test presets,
  because pre-commit and CI share the `native-clang-release` preset but not its tests.
- **Pre-commit (`native-clang-release`):** builds both addons and runs `napi_load` and `spike_napi`
  (`exclude: 'napi-nw'`, `tests: ['napi_load', 'spike_napi']`).
  - Cost locally: about 30 s of compiling and linking on a cold cache, plus about 10 s of tests.
  - It then needs `npm ci` in `tests/wasm`, which `new-worktree.mjs --deps` already does.
- **CI:**
  - `native-clang-debug` runs everything: `tests: ['napi_load', 'napi_load_nw', 'spike_napi', 'testapp_napi']`,
    `scripts: true`.
  - `native-clang-release`, `native-msvc-debug` and `native-msvc-release` build both addons and run `napi_load` only
    (`exclude: 'napi-node|napi-nw'`). MSVC builds catch what clang accepts.
  - `matrix()` gains `napi: true` for the native presets, and the workflow a step "Install the node-api headers"
    (`node tools/install-node-api.mjs`, a 300 KB download) for them. `scripts` already drives the npm and pnpm steps.
- **Time, on GitHub's runners** (run 45925984288 as the base; runners are 2.3 times slower than local):

  | Job | Today | Added | Estimate |
  |---|---|---|---|
  | native-clang-debug | 10m10s | addons ~1.5 min; installs ~0.6 min (34 s in wasm-st-debug); headers; four tests ~1.5 min | ~14 min |
  | native-clang-release | 11m10s | addons ~1.5 min; headers; `napi_load` | ~13 min |
  | native-msvc-debug | 12m09s | the same | ~14 min |
  | native-msvc-release | 14m58s | the same | ~16.5 min |
  | wasm-mt-debug | 12m07s | — | 12 min |
  | wasm-st-debug | 17m15s | — | 17 min |

  - Wall-clock time stays at wasm-st-debug's 17 minutes, but `native-msvc-release` comes within a minute of it. If
    it overtakes, the run gets longer by the difference; step 6 records the real times.
  - Minutes billed rise by about 8 per run (78 to 86).
- `tools/gate.test.mjs` gets cases for the labels, the new required tests and the `napi` matrix key.

## Documentation
- `docs/building.md`: the node-api headers in "What you need", `install-node-api.mjs`, `COREDCC_NAPI`, the addon
  targets and their outputs, the new ctests, their labels and what they need, the gates' required tests.
- `docs/embedding.md`: a "Node and NW.js" section: build your own addon with `coredcc_napi_addon` so that your
  types share coredcc's image ("Interfaces across binary images"); loading; the delay-load hook; one thread and one
  env; teardown at Node exit and the exit listener; DrawList bytes are copied; `abort()` ends the process.
  Update "Threading" for off-thread foreign calls and `ThreadAffine`.
- `docs/modules.md`: `bind/napi/` in the `bind/` entry.
- `docs/debugging.md`:
  - a missing delay-load hook: in NW.js the page dies at the `require`, with no error and no log line, and nw.exe
    exits 0;
  - a `.node` that a running NW.js or Node keeps locked (the link fails with LNK1104);
  - `dlopen` or a cleared require cache that loads the addon into a second env;
  - a native crash in the test app leaves no dump: read `<--apptest-log>.stderr`.
- `docs/naming.md`: the registered namespace is the script namespace.
- `docs/drawlist-format.md`: the host note on reading the bytes under N-API.
- The architecture plan: §13's dispatcher and threads bullets, §10's Electron bullet, and its "Still unverified"
  lists, each marked with what P6a found and linked here.
- The test app plan: TA4 as built; the runtime facts corrected (one binary, external buffers, NW.js exits 0 after a
  crash).
- `docs/implementation-tasklist.md`: row 11 split into P6a (this plan) and the later parts; `docs/index.md`: this
  plan and its appendix.
- litestl's `documentation/binding.md`: `detach`, in its PR.

## litestl
- **PR 1 (step 2):** `IBindingBackend.detach`, both backends, a conformance test, documentation.
- **Maybe later:** the delay-load hook in `lt_add_napi_addon`, so that litestl's own fixture loads in NW.js
  (open question 3).
- **Maybe later:** an external-buffer return mode (step 5).
- **Not proposed:** making litestl's `installForeign` refuse a second env. coredcc's `install` refuses first, and
  litestl's per-thread host is right for a host that wants one env per worker.

## Exit criteria
- `coredcc.node` and `coredcc_testapp.node` build in all four native presets and load in Node 24 and NW.js 0.112.
- `napi_load`, `napi_load_nw`, `spike_napi` and `testapp_napi` pass, with no skipped tests, and the WASM suites still
  pass.
- R1, R2 and R4–R12 are settled, each with a test; R3 is closed by step 5.
- CI passes, with the jobs' times recorded here.
- The documents above are updated, and the feature review is done.

## Open questions for the owner
1. **litestl `detach`: a litestl PR first, which blocks step 2's coredcc half until it merges.** Recommended. A
   `WeakSet` in the TS layer only would leave a disposed `session.ctx` pointing at freed memory for every function
   that doesn't call `checkCtx`, such as `get`.
2. **Install the headers with a coredcc tool pinned like the emsdk (`tools/install-node-api.mjs`).** Recommended.
   Alternatives: litestl's `configureEnv.mjs --fetch-deps`, which also downloads the WASI sysroot and builtins and
   writes into the submodule's `build/`; or an npm devDependency in `tests/wasm`, which would make every native
   configure depend on `npm ci`.
3. **Keep the delay-load hook in coredcc now, and offer it to litestl later.** Recommended: coredcc is the only
   consumer that loads in NW.js, and the hook needs no litestl change.
4. **Pre-commit runs `napi_load` and `spike_napi`, and leaves out the `napi-nw` label.** Recommended: the Node suite
   catches marshalling and dispatch faults in about 10 s; NW.js tests stay in CI, as the WASM test app's are. The
   alternative adds about 15 s and two NW.js launches to every commit.
5. **The NW.js and Node suites run in `native-clang-debug` only; the other native jobs run `napi_load`.**
   Recommended: no second build of coredcc, and only one job pays for the installs. A separate job would compile
   everything again (about 10 minutes billed).
6. **`COREDCC_NAPI` is on by default for top-level native builds, so configuring needs the headers.** Recommended,
   landing in one commit with the tool and CI's install step. The alternative, off by default, lets a native build
   skip the addon silently.
7. **The naming pass's other "Deferred to P6" items move to P6b (the faberforge spike).** Recommended: they are not
   about N-API, both backends share them, and P6b has a consumer that shows which ones matter.
8. **DrawList stays a copy on N-API unless step 5 measures a need.** Recommended. Zero-copy needs a litestl PR, a new
   lifetime rule for hosts, and a fallback for Electron.
9. **One `napi_env` per process; a second is refused, on any thread.** Recommended, until litestl's string interning
   is thread-safe and coredcc keeps a foreign host and a Runtime set per env.
10. **coredcc ships `coredcc.node` as well as the CMake function.** Recommended: it is what `napi_load` tests and what
    a host with no native types of its own loads. faberforge, which binds its own types, builds its own addon.
11. **Teardown at Node exit: the TS layer disposes open sessions from a `process.once('exit')` listener, and every
    other owned type must be safe to destroy without a foreign host, in any order.** Recommended: it uses the
    dispose sequence that already exists, and the teardown test checks the remainder. Alternatives: the addon
    tracks every owned C++ object and destroys them in dependency order from its own cleanup hook (more code, and
    it would run after litestl's foreign host is gone anyway); or the addon leaks everything at exit by skipping
    finalizers, which hides destructor bugs that a host's normal close would then hit.

## Pressure test
Done 2026-11-28 by a fresh agent that saw the plan and the code but not the author's reasoning. Probes are in the
session scratchpad, `p6a-pt\`: `probe.cc` (a delay-loaded addon with finalizers, a static thread pool, abort and
init counting), `drive.mjs` (launches NW.js and watches CDP), `scan.ts` (walks every binding of the wasm-st test app
module and checks each parameter and return against the N-API runtime's rules).

### Claims that survived
- **The delay-load hook works (R4).** Rebuilt independently with clang-cl and lld-link (`/MDd`, no exceptions):
  the addon loaded in Node 24.22.1 (`node.dll` absent, the exe answered) and in NW.js 0.112 (`node.dll` present).
- **Without the hook, NW.js can't use the addon.** The same probe linked without `/DELAYLOAD` killed the NW.js
  page before its `catch` ran (no log line, and nw.exe exited 0 after 0.7 s). With `C:\Program Files\nodejs` on the
  PATH, the symptom is not the "module could not be found" message that the planned `debugging.md` entry names.
- **The facades' and slots' types are all supported (R8, second half).** `scan.ts` covered 223 free functions,
  160 methods, 105 constructors and 37 foreign slots. No unions, at most 6 arguments, and no reference to a
  scalar, pointer to a scalar or fixed array among facade parameters. No fixed-array returns, which would matter:
  `returnToJS` makes a live view over `ret` and then frees `ret`. No foreign slot returns a reference, an owned
  pointer or a struct without a copy constructor. The only gaps are litestl's `Vector<T>::ptrWithCount`
  constructors (pointer to a scalar; unused) and assigning to embedded struct members such as `View.proj` or
  `TransformDelta.vector` (`napi_runtime.cc:604`). Nothing in `bind/ts`, the test app or the suites assigns to them.
- **The generated TS is the same on both backends.** `coredcc_gen_ts` (native-clang, RelWithDebInfo) wrote
  `coredcc/` and `litestl/` byte for byte as the wasm-st module's generator did (`build/wasm-st/tests/wasm/spike/gen`).
  So `size_t`-like widths don't make `number` on WASM and `bigint` natively.
- **Facades are keyed by `qualifiedJsName` on both backends** (`napi_runtime.cc` `Runtime::Runtime`,
  `backend.ts` `WasmBindingBackend`). `getFunction('coredcc::statusText')` exists (`functions.ts:230`).
- **An off-thread foreign call fails with "no foreign host" and does not wait (R2).** Natively `LSTL_HAS_JS_HOST`
  is undefined, so `foreign::call` returns `fail(error, "no foreign host", m)` (`binding/foreign.cc:77-97`).
- **No current parallel site calls a slot.** `bvh::forChunks` callers read gathered or native data, and
  `AnyBvh` gathers on the calling thread (`any_source.cc:40-74`). `poolUsable()` is true natively (`bvh/build.cc:20-30`).
- **`checkCtx` throws for every live context on N-API (R5, first half).** Wrappers have no `ptr`; setting
  `ctx.ptr = 0` only adds an own property that the runtime ignores, so a disposed `session.ctx` still reaches freed
  memory through every function that doesn't call `checkCtx` (`get` doesn't: `index.ts:128`).
- **Slot arguments are call-scoped on both backends** (`napi_foreign.cc` `ScopeGuard`, `foreign.ts:102-127`), so
  the TS layer can't depend on keeping a slot's `ctx` on one backend only.
- **litestl's runtime compiles under MSVC (R9, compile half).** `napi_runtime.cc` and `napi_foreign.cc` built with
  `cl` and coredcc's flags (`/GR- /EHs-c- -D_HAS_EXCEPTIONS=0 /std:c++20 /MDd`), without warnings at the default
  level. Linking and `/W4` on coredcc's own N-API files are still for step 1.
- **CI times are as stated.** Run 45925984288's jobs match the plan. The three script-dependency steps took 34 s
  in all in `wasm-st-debug` (npm ci 4 s, installing pnpm 14 s, `pnpm install` 16 s), well under the "about 2 minutes"
  the plan allows.

### Claims that broke
- **`checkFunctions` does not check signatures (R8, first half).** It only refuses two functions that share a JS
  name (`binding/manager.h:150-171`). Marshalling faults show up only when a function is called. The test app's
  manager calls `validate()`, which aborts rather than throws (`host.cc:306`). So `napi_load` loading the addon
  proves nothing about types. The scan above does, and could become part of `napi_load`.
- **The R5 example is wrong.** `spike.test.ts:584-587` doesn't crash on N-API: `kept` is a slot argument, and its
  wrapper is cleared when the call ends, so the call throws "the bound object was disposed or outlived its call",
  which matches `/disposed/`. The real use-after-free is `session.ctx` after dispose, passed to a function that
  doesn't call `checkCtx`. Step 2 needs a test of that case, on both backends.
- **"A second `install` on another thread" misses the same thread.** `process.dlopen` of the already-loaded addon
  in the same environment calls init again, with a new `napi_env`, on the same thread. Measured: `init 2
  sameEnv=0 sameThread=1`. litestl's `installForeign` then calls `foreign::setHost` with the second host, so every
  foreign handle from the first environment becomes "unknown foreign handle". T4/T14's planned native twin, "loads
  the addon twice", is exactly this case if it uses `dlopen`; with `require` it gets the cached module and tests
  nothing. A loader that clears the require cache does the same thing.
  - Also measured: a second `napi_add_env_cleanup_hook` with the same `(fn, arg)` pair aborts Node
    (`AddEnvironmentCleanupHook ... Assertion failed: result.second`). litestl passes a fresh `ForeignHost *`
    each time, so it is safe today, but `install`'s own hook must not use a constant argument.
- **Synchronous `gc()` calls never run N-API finalizers (R6).** For an NAPI_VERSION 8 module they run deferred.
  Measured: after `gc(); gc()` nothing was finalized, and after one `setImmediate` the object was. The planned test,
  "after two `gc()` calls, `foreignCount()` is back where it started", passes without anything having been
  finalized. It needs `await setImmediate` between the calls, as T1 does (`spike.test.ts:590-594`). Since
  finalizers run in their own task, the plan's specific worry (a finalizer calling `WeakMap.delete`) is legal.
- **Node destroys every owned C++ object at exit; WASM never does (a new risk for R7).** Measured: at Node exit the
  env cleanup hooks ran first (newest first, so litestl's foreign host is removed before anything else), then
  every wrapper's finalizer ran, newest first. In those finalizers `napi_call_function` returned 10
  (`napi_pending_exception`). So every `spike_napi` process ends by running the destructors of everything still
  owned by a wrapper, in reverse creation order, with no foreign host. That is where
  `ForeignBindings::~ForeignBindings`'s `CDCC_CHECK(live_ == 0)` (`foreign_command.cc:82-90`) would fire, if an
  object holding script instances outlived its Runtime's owner. WASM never ran that code at exit.
  - A GC'd `Session` also skips its dispose sequence (`disableDrivers` before the Library goes, `core.ts:207-210`).
    The host's C++ destructor then runs without it. That is true on WASM too, but WASM has never run it at exit.
- **NW.js runs no teardown at all (R7 as planned is vacuous).** Measured: after `nw.App.quit()` no finalizer and no
  cleanup hook ran. The renderer is simply ended. After an `abort()` in the page, CDP sent
  `Inspector.targetCrashed`, the socket closed, and nw.exe exited with code **0**. So TA4's "quit with an open
  document, exit code 0, no crash dump" passes whether or not teardown is safe, and an exit code can't tell a crash
  from a quit. The teardown test belongs in Node (`spike_napi`). In NW.js, check `crashed()`.
- **Native A3 keeps no crash dump and no message.** With `_set_abort_behavior(0, _CALL_REPORTFAULT)`, `abort()`
  calls `_exit(3)` and raises no exception, so there is nothing for crashpad to write. The renderer's stderr also
  didn't reach nw.exe's stderr in either probe, so the CDCC_CHECK text that WASM's A3 asserts
  (`ta1.test.ts:325`) isn't available. The harness does see the crash: the CDP socket closes, so `crashed()` is
  true (`harness.ts:57-59`). For the message, the test addon should write it to `--apptest-log` before aborting
  (for example, from a CRT report hook).
- **The steps don't each pass CI on their own.**
  - Step 1 turns `COREDCC_NAPI` on and makes configuring fail without the headers. GitHub's runners don't have
    them until step 5's workflow step. Other checkouts on this machine fail too: coredcc's default path,
    `build/deps/node-api-headers`, doesn't exist yet. Only litestl's copy under `extern/litestl/build/deps` does.
  - Step 1's `napi_load` has an NW.js half that exits 77 without `pnpm install`, and the gate counts that as a
    failure. Every native CI job runs it, and none of them installs the test app's dependencies today
    (`matrix()` sets `scripts` only where `tests` is set, `gate.mjs:46-48`).
  - Step 3's `spike_napi` needs `npm ci` in those same jobs.
  - So the header install, the `napi`/`scripts` matrix keys and the workflow steps have to land in step 1, with
    each test's dependency added by the step that adds the test.
- **The gate runs every registered test, not just the ones named in `tests`.** `gate.mjs` runs
  `ctest --preset <preset>` with no filter. `tests` only lists the tests that must be present. If `spike_napi` and
  `testapp_napi` are registered wherever the addons are built, as "Targets and files" says, then:
  - pre-commit (`native-clang-release`) runs `testapp_napi`, which contradicts open question 4;
  - all four native CI jobs run both suites, not only `native-clang-debug`.
  Registering a test per preset needs a ctest label with a filter in the test presets (or in `gate.mjs`), or an
  option that the presets set.
- **The CI cost is underestimated.** With the NW.js half of `napi_load` in every native job, all four jobs need
  `pnpm install`, which downloads the NW.js SDK. If the suites aren't filtered, `native-msvc-release` (14m58s today)
  also gets two addon links and both suites, which puts it close to `wasm-st-debug`'s 17m15s. "Minutes billed rise
  by about 8" counts the installs in one job only.
- **Smaller points:**
  - `tests/wasm/run.mjs` also runs `gen_paths.ts` against the module (`tests/wasm/gen_paths.ts:13`, `cdcc.load` of
    an Emscripten module). It needs the addon path as well as `gen.ts` does.
  - The launcher hard-codes the test app as the NW.js app (`launcher.mjs:116`, `APP_DIR`). Step 1's minimal app in
    `tests/napi/nw/` needs an `appDir` option.
  - `LITESTL_NODE_API_HEADERS` is a `CACHE PATH` in litestl (`extern/litestl/CMakeLists.txt:37`). An existing
    build tree keeps its cached value, which is litestl's own copy here (`build/native-clang/CMakeCache.txt:272`),
    unless coredcc sets a normal variable or uses `FORCE`.
  - coredcc's `SYSTEM` loop over litestl targets (`CMakeLists.txt:62-66`) doesn't list `binding_napi`. A host
    project that supplies litestl without `SYSTEM` would expose `node_api.h` to `/W4 /WX`.

### Unclear
- **The R2 test's design.** `parallel_for` runs its last band on the calling thread (`util/task.h:373`). If the
  test-only facade uses the pool, the main thread calls the real JS slot while workers fail. Both paths then write
  `ForeignBindings::frames_` (`foreign_command.cc:100-104`) and the sticky error at once, which is a data race.
  Use one `std::thread` that the facade joins. On wasm-st the facade can't start a thread, but it has to be bound
  there too, or the shared suites don't type-check against the wasm-st TS. So it needs a non-thread fallback.
- **N-API doesn't check threads.** `napi_create_int32` called from another thread returned `napi_ok`. Nothing in
  N-API itself catches a stray off-thread call. Only litestl's per-thread host and coredcc's refusal do.
- **The global task pool's destructor joins its workers at DLL detach.** In Node, a probe with eight waiting
  workers exited in 0.1 s with code 0 after a join at detach. NW.js never reaches detach. Not tested with a pool
  that is busy at exit.
- **Electron and the zero-copy path (R3).** Not checked, as the plan says.

### Changes recommended
- `install` refuses any second `napi_env`, on any thread. It records the first env, not the thread. T4's native
  twin calls `process.dlopen` on the same file and expects the refusal.
- Replace R8's test with a type scan inside `napi_load`: walk `bindingKeys()` and check each parameter and return
  against what the runtime supports, as `scan.ts` does.
- Move the R7 teardown test to Node. A `spike_napi` case ends its process with documents, script commands and undo
  records still owned by wrappers, and checks for exit code 0. Keep the NW.js clean-quit test, but assert that
  `crashed()` is false rather than checking the exit code.
- R6: `await new Promise(setImmediate)` between the `gc()` calls.
- Native A3: assert `crashed()`. Get the CDCC_CHECK message from `--apptest-log`, written by a CRT report hook in
  `napi_entry.cc`. Don't expect a dump.
- Fix R5's example. Add a test that uses the disposed `session.ctx` with `get` on both backends.
- Re-order the steps. Step 1 carries the header tool, the `napi`/`scripts` matrix keys, the workflow step and the
  dependencies `napi_load` needs. Either drop `napi_load`'s NW.js half from three of the four native jobs, or
  accept a `pnpm install` in each.
- Filter the script tests by label per preset, so that pre-commit doesn't run `testapp_napi` and only
  `native-clang-debug` runs the suites. Recompute the CI estimate with `native-msvc-release` in it.
- `run.mjs --addon` also drives `gen_paths`. Give the launcher an `appDir`. Make `LITESTL_NODE_API_HEADERS` a
  normal variable or `FORCE` it, and add `binding_napi` to the `SYSTEM` loop.
- R2's facade uses a single joined `std::thread`, with a fallback on wasm-st.
- Correct the planned `debugging.md` entry for a missing hook: in NW.js the page dies, and there is no "module not
  found" error.

### Open questions
- Add one: what Node's exit-time finalizers may do. Either the TS layer or the addon arranges an orderly teardown
  (for example, an exit hook that disposes open sessions while the foreign host still exists), or every owned
  type's destructor must be safe in any order without a foreign host.
- Question 4 is moot until tests are filtered per preset: the gate runs every registered test.
- Question 6 is fine, but only if it lands together with the CI header step.

### How the plan changed
All of the recommended changes were adopted, with one change of mechanism:
- **Rejected: a CRT report hook for native A3's message.** `CDCC_CHECK` prints with `fprintf(stderr)` and then calls
  `abort()` (`base/assert.cc:32-42`). With the no-dialog setup, `abort()` doesn't report through the CRT, so a
  report hook never sees the text. The test addon reopens its `stderr` onto `<--apptest-log>.stderr` at init
  instead, and the harness reads that file after a crash.

What changed:
- `install` refuses any second `napi_env`, on any thread, and registers its cleanup hook once. A new risk, R12, covers
  the `dlopen` case. `napi_load` and T4/T14's native twin `dlopen` the file again and expect the refusal.
- R8 is settled by a type scan inside `napi_load`, not by loading.
- R5's example is the disposed `session.ctx` passed to `get`. Step 2 adds that test on both backends.
- R6's test awaits `setImmediate` between `gc()` calls and checks that the host was finalized.
- A "Teardown" design: an exit listener disposes open sessions while litestl's foreign host exists. The rest is
  left to finalizers, and the teardown test in `spike_napi` checks it under Node. In NW.js, tests assert `crashed()`
  instead of an exit code. Open question 11 is new.
- Native A3 asserts `crashed()` and expects no dump.
- The steps were re-cut so each passes the gates and CI alone. Step 1 carries the header tool, `COREDCC_NAPI`, the
  `napi` matrix key, the workflow's header step and `native-clang-debug`'s installs. The former step 5 (gates and
  CI) is gone; its parts went to the steps whose tests need them.
- The script tests carry ctest labels (`napi`, `napi-node`, `napi-nw`), and gate steps leave labels out. So
  pre-commit doesn't run `testapp_napi`, and only `native-clang-debug` runs the suites. The CI estimate was redone per
  job: `native-msvc-release` comes to about 16.5 minutes, within a minute of `wasm-st-debug`.
- Smaller fixes:
  - `run.mjs --addon` also drives `gen_paths`;
  - the launcher has `appDir` and `addon` options;
  - `LITESTL_NODE_API_HEADERS` is set with `FORCE`;
  - `binding_napi` is in the `SYSTEM` loop;
  - the `debugging.md` entry for a missing hook says the page dies;
  - R2's facade uses one joined `std::thread`, with a same-thread fallback on wasm-st.
- The research and the risk table moved to the appendix, `2026-11-28-p6a-napi-addon-risks.md`, to keep the plan
  body (everything before "Pressure test") to about 30 KB. The pressure test record refers to the old step numbers.
