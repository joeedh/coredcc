# P6a: research and risks (appendix)

The research behind [the P6a plan](2026-11-28-p6a-napi-addon.md): what the code said on 2026-11-28, what was
measured, and each unverified claim with the step that settles it. The plan's "Pressure test" section has what a
fresh agent found afterwards; the table at the end of this file includes it.

## What the code says (2026-11-28)

### litestl's N-API runtime
- **Target:** `binding_napi`, a static library of `napi_runtime.cc` and `napi_foreign.cc`
  (`extern/litestl/binding/napi/CMakeLists.txt:4-8`). It is built against `NAPI_VERSION=8`.
- **Configured only when the headers exist:** litestl looks for `node_api.h` under `LITESTL_NODE_API_HEADERS`,
  which defaults to `$LITESTL_DEPS_DIR/node-api-headers` or `extern/litestl/build/deps/node-api-headers`
  (`extern/litestl/CMakeLists.txt:32-45`). Without them it prints a status line and skips the target.
  - `configureEnv.mjs --fetch-deps` downloads node-api-headers 1.5.0, along with the WASI sysroot and the wasm32
    compiler-rt builtins (`extern/litestl/configureEnv.mjs:28,136-178`).
  - On this machine the headers are in `extern/litestl/build/deps/node-api-headers`, so coredcc's `native-clang` and
    `native-msvc` trees already configure `binding_napi`, but nothing builds it
    (`build/native-clang/CMakeCache.txt:272`). GitHub's runners don't have them.
- **The import library:** on Windows, `binding/napi/CMakeLists.txt:10-24` builds `node.lib` from the headers' `.def`
  file. That file says `NAME NODE.EXE`, so an addon linked with it imports `napi_*` from `node.exe`.
  `lt_add_napi_addon` (`:27-37`) links it directly, with no delay-load.
- **The exports** implement `IBindingBackend` (`typescriptRuntime/backend.ts:11-33`): `construct`, `constructWith`,
  `getFunction`, `getStatic`, `getClass`, `typeName`, `bindingKeys`, `generateTypeScript`, `getMemSize`,
  `foreignHandle`, `foreignCount`, `setForeignConverter`, and `kind: 'napi'` (`napi_runtime.cc:1299-1502`).
  `installExports` first runs `checkFunctions` and throws its error (`:1301-1306`). That check only refuses two
  functions with one JS name (`binding/manager.h:150-171`); it checks no signatures.
- **Supported in both directions:** numbers, booleans, int64 as `bigint`, enums (as numbers), `util::string` and
  `CString`, `Span` (a typed array of the same element type is passed in place, `:864-922`), `Vector` (a
  `Vector<uint8>` return is copied into a `Uint8Array`, `:1041-1051`), structs by value, reference and pointer,
  owned returns, static methods, template selection and inference, fixed arrays and embedded members as live views.
  Up to 32 arguments (`:347`).
- **Not supported:** discriminated unions (no `Union` case in `toJS` or `marshal`; coredcc binds none), reference
  and owned-pointer returns from foreign methods (`binding/foreign.cc:72-77`), assigning to an embedded struct
  member (`napi_runtime.cc:604`), and any JS-visible pointer: wrappers have no `ptr` property (`napi_runtime.h:5-6`).
- **Foreign dispatch** (`napi_foreign.cc`) is built and conformance-tested:
  - a handle table keyed by a `WeakMap` (`:129-150`); converters (`setForeignConverter`);
  - argument wrappers scoped to the call, read-only for const arguments (`:405-440`);
  - returns copy-constructed into `ret`; vectors of scalars from any array (`:476-590`);
  - errors become `false` plus a message that starts with the method's name: exceptions, missing methods, wrong
    return types, and Promises (`:258-327`);
  - an already-pending JS exception is refused and kept (`:268-273`, litestl #10);
  - the host is installed for the env's thread and removed in an env cleanup hook (`:599-614`).
- **Threads:** `foreign::call` goes to a `thread_local` host (`binding/foreign.cc:12,77-79`). On a thread without
  one it fails with "no foreign host" (`:91-97`), and `release` does nothing (`:101-114`). It never proxies or waits.
- **Conformance:** `tests/bindings/` builds one fixture three ways (generator, WASM, N-API addon), and
  `tests/bindings/js/run.mjs` runs `conformance.test.ts` against each with the same expectations, including that
  the backend's generated TS equals the native generator's (`conformance.test.ts:450`). Two tests are
  backend-specific and use `{skip: ...}` (`:458`, `:475`).
- **litestl's CI** (`extern/litestl/.github/workflows/ci.yml`) fetches the deps and runs the `binding_conformance`
  ctest on Linux (gcc, clang) and `windows-latest` with MSVC and Node 22. It never loads an addon in NW.js or
  Electron.

### What coredcc assumes of the backend
- **Dispatch is already host-neutral.** `ForeignBindings::Dispatcher` (`bind/foreign_command.cc:61-73`) calls
  `litestl::binding::foreign::call` (`:97-112`), and litestl routes that to EM_JS or to the N-API host. coredcc has no
  `__EMSCRIPTEN__` code on the foreign path. The N-API dispatcher "plugs in" when the addon's init runs
  `installExports`.
- **The other `__EMSCRIPTEN__` code:** `bvh/build.cc:20-30` (`poolUsable`: true natively), the manager export
  `bind/wasm_entry.cc`, and the test app's `host.cc:29,313-316`, which includes `emscripten.h` unconditionally.
- **`bind/CMakeLists.txt:27-37`** builds `coredcc_bind_wasm_entry` under Emscripten and the generator
  `coredcc_gen_ts` otherwise. `source/coredcc/testapp/CMakeLists.txt:6-8` builds the test app's host only for
  coredcc's own wasm-st build.
- **The TS layer is backend-neutral except for these** (all in `bind/ts/`):
  - `load()` accepts only an Emscripten module (`core.ts:144-168`).
  - `Session[Symbol.dispose]` invalidates the context by setting `ctx.ptr = 0`, and `checkCtx` throws unless
    `ctx.ptr` is truthy (`core.ts:210,224-226`). On N-API there is no `ptr`, so **`checkCtx` throws for every
    live context**. Worse, a disposed `session.ctx` still points at freed memory, and every function that doesn't
    call `checkCtx` reaches it: `get` doesn't (`index.ts:128`). (`spike.test.ts:584-587` is not that case: its `kept`
    is a slot argument, which both backends clear when the call ends.)
  - `moduleHeap()` reads `HEAPU8` (`core.ts:217-221`); `readDrawList(..., {view: true})` falls back to a copy
    without it (`drawlist.ts:157-171`), so it already works on N-API.
  - `isFatal` knows only `WebAssembly.RuntimeError` and `ExitStatus` (`core.ts:127-128`).
  - `gen.ts:13-16` generates the TS from a WASM module only.
- **`ThreadAffine` is set but never read.** Every foreign table gets it (`base/iface/registry.cc:271`); nothing
  checks it. What keeps foreign slots off workers today is that the parallel code reads gathered data only:
  `AnyBvh` gathers bounds on the calling thread first (`bvh/any_source.cc:40-74`), and the scene BVH is built from
  the evaluated store (`scene/scene_bvh.cc:9-30`). Graph kernels run on the main thread (`docs/embedding.md:85`).
- **Facade names:** litestl's runtime keys free functions by `qualifiedJsName` (`napi_runtime.cc:351-364`), the same
  key the WASM backend uses (`backend.ts:82-92`). The generated TS calls `bindFunction("coredcc::getValue")` on both.

### The test app
- The page imports `dist/backend-wasm.js` and calls `install(register, modulePath, ...)` (`src/main.ts:19-36`,
  `src/backend/wasm.ts:38-74`). `methods.ts` is shared by every backend and already names a future `native.ts`
  (`methods.ts:3,22`).
- `build.mjs` generates `gen/` from the wasm-st node module and bundles `backend-wasm.js` (`build.mjs:36,82-104`).
- Tests read `COREDCC_TESTAPP_BACKEND` in TA1 and TA2 (`ta1.test.ts:25`, `ta2.test.ts:79`); TA3 hard-codes
  `'wasm-st'` (`ta3.test.ts:53`). TA1 A3 (abort) and A4 (wrong module) are WASM-specific (`ta1.test.ts:310-331`).
- `ctest.mjs` builds the page and runs `tests/**/*.test.ts`; it exits 77 when NW.js or `pnpm install` is missing.

### Build, gates and CI
- Presets: `native-clang`, `native-msvc`, `wasm-st`, `wasm-mt`, each Debug and RelWithDebInfo (`CMakePresets.json`).
  `native-clang` links with `lld-link` (`build/native-clang/CMakeCache.txt:117`).
- coredcc adds litestl `EXCLUDE_FROM_ALL SYSTEM` and turns exceptions and RTTI off for it
  (`CMakeLists.txt:18-21,47-55`), so `node_api.h` is a system include for coredcc's `/W4 /WX` targets.
- Gates (`tools/gate.mjs:30-39`): pre-commit is `native-clang-release`; CI is six presets, and only `wasm-st-debug`
  requires script tests (`spike_ts`, `testapp`). `matrix()` installs the emsdk for `wasm-*` and the script deps
  where `tests` is set (`:46-48`).
- CI (`.github/workflows/ci.yml`): one `windows-2025-vs2026` job per preset, Node 24, no build cache.
  - Recent runs took 12m52s to 18m17s. In run 45925984288 the jobs took: native-clang-debug 10m10s,
    native-clang-release 11m10s, native-msvc-debug 12m09s, wasm-mt-debug 12m07s, native-msvc-release 14m58s,
    wasm-st-debug 17m15s. wasm-st-debug sets the wall-clock time.
  - Locally, `spike_ts` takes 7.7 s and `testapp` 9.7 s (`build/gate/wasm-st-debug.log:660,879`). Runners are
    about 2.3 times slower (`docs/debugging.md:235`).

## Measured during research (2026-11-28)
Scratch files: `scratchpad\napiprobe\` and `scratchpad\nwprobe\` of this session.
- **litestl's runtime compiles under coredcc's flags.** `napi_runtime.cc` and `napi_foreign.cc` built as objects in
  `build/native-clang` (Debug, clang-cl, no exceptions, `_HAS_EXCEPTIONS=0`). MSVC was not tried.
- **One addon loads in both Node 24 and NW.js.** A probe addon was linked against the headers' `node.lib` with
  `/DELAYLOAD:node.exe`, `delayimp.lib`, and a delay-load hook that answers "node.exe" with `node.dll` if that is
  loaded, else with the host executable. It loaded and ran in Node 24.22.1 and in NW.js 0.112.0, built with both
  `link.exe` and `lld-link`.
  - NW.js's `node.dll` exports 147 `napi_*` functions; `nw.exe` exports none. So an addon that imports
    `NODE.EXE` without the hook can't load in NW.js, and the test app plan's "a Node-side unit test needs its own
    build" (`2026-11-19-test-app.md:49-50`) doesn't hold with the hook.
- **NW.js 0.112 allows external ArrayBuffers.** `napi_create_external_arraybuffer` returned `napi_ok` and the bytes
  read back, in NW.js and in Node. The architecture's "the V8 sandbox refuses external ArrayBuffers"
  (`2026-11-19-architecture.md:955`) is about Electron and was not checked there.
- NW.js reports Node 26.1.0, modules 147, N-API 10, V8 14.9, Chromium 149.0.7827.29.

## Risks: the unverified claims, and how each is settled

| # | Claim | Where | Status (after the pressure test) | Settled by |
|---|---|---|---|---|
| R1 | "The N-API reverse marshalling is unverified" | architecture `:300`, `:1517`, `:1592` | litestl built and conformance-tests it; coredcc's interfaces never ran on it | step 3: the whole Node suite on the addon (script commands, graph kernels, transform models, tools, gizmos, modals, stroke brushes) |
| R2 | "Under N-API an off-thread call deadlocks inside `parallel_for`" | `:1116`, `:1492` | broke on reading: it fails with "no foreign host" (`binding/foreign.cc:77-97`); confirmed by the pressure test | step 3: a test-only facade calls a slot from one joined `std::thread` (not the pool: `parallel_for` runs a band on the caller, which would race on `ForeignBindings::frames_`); it must return Internal without hanging. On wasm-st the facade calls the slot on the caller's thread and reports that it did, so the shared suite type-checks and runs there |
| R3 | "The V8 sandbox refuses external ArrayBuffers" | `:955` | broke for NW.js 0.112 (measured twice); Electron not checked | step 6, only if zero-copy is pursued |
| R4 | "The NW.js addon links node.dll, so a Node test needs its own build" | test app plan `:48-50`, `:294` | broke: one binary with the delay-load hook loads in both (measured twice). Without the hook the NW.js page dies before its `catch` runs | step 1: `coredcc.node` loads in both |
| R5 | "The TS layer runs unchanged on N-API" | `bind/ts/index.ts:1-19`; `backend.ts:1-5` | broke: `checkCtx` throws for every live context; a disposed `session.ctx` passed to `get` is a use after free | step 2: litestl `detach`, the TS changes, and a test of exactly that case on both backends |
| R6 | GC finalizers may run destructors that call into JS | `napi_foreign.cc:243-255` | legal: N-API 8 finalizers run deferred, in their own task. Synchronous `gc()` alone finalizes nothing | step 3: `gc()`, `await new Promise(setImmediate)`, `gc()`, the same again, then check `foreignCount()` and that the object was finalized |
| R7 | Teardown is safe | `foreign_command.cc:82-90`, `core.ts:207-210` | broke as planned. Node at exit runs the env cleanup hooks first (litestl's foreign host goes), then every finalizer newest first, with JS calls failing (`napi_pending_exception`). NW.js runs no teardown at all, and exits 0 even after a crash | step 3: the orderly teardown of "Teardown" in the plan, tested in `spike_napi` under Node; in NW.js only `crashed()` is asserted |
| R8 | Every coredcc facade's signature is one the N-API runtime marshals | `napi_runtime.cc:1301-1306` | true by a scan (223 functions, 160 methods, 105 constructors, 37 slots; no unions; at most 6 arguments). But `checkFunctions` checks no signatures, so loading proves nothing | step 1: the type scan runs inside `napi_load` |
| R9 | MSVC builds `binding_napi` with coredcc's flags | — | compiles with `cl` (pressure test); link and `/W4` on coredcc's N-API files not yet | step 1: the `native-msvc` presets build the addons |
| R10 | A debug-CRT `abort()` in NW.js shows a dialog and hangs the test | — | with the no-dialog setup, `abort()` is `_exit(3)`: no dialog, no dump, and no stderr reaches nw.exe | step 4: native A3 asserts `crashed()` and reads the message from `--apptest-log`, written by a CRT report hook |
| R11 | Node 24 and NW.js behave the same for everything the suites touch | — | unverified beyond the probes | steps 3 and 4 |
| R12 | One environment per process | — | broke: `process.dlopen` of the loaded addon re-runs init with a new `napi_env` on the same thread, and litestl's `setHost` replaces the first host, so the first env's handles become unknown. A duplicate `napi_add_env_cleanup_hook(fn, arg)` aborts Node | step 1: `install` refuses any second env, on any thread; step 3: T4's native twin `dlopen`s the file again and expects the refusal |
