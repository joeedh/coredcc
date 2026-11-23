# Debugging guide

Running notes on how to diagnose problems in this project. Add an entry whenever a bug
took real effort to find, or when you learn a diagnostic technique worth reusing.

## Entry format

- **Symptom:** what you saw
- **Cause:** what was actually wrong
- **Fix:** what changed (commit hash if useful)
- **How to spot it next time:** the check, log, or tool that reveals it

## Entries

### A context silently reads another document after GC
- **Symptom:** in the TS spike, `cdcc.get(ctx, 'count')` returned another host's value,
  and `cdcc.set` wrote into the other document. No error.
- **Cause:** `spike.context(host)` returns a non-owning view. When nothing held the Host
  wrapper, its finalizer freed the Host, and the next `spike.create()` reused the block.
  The view still held the old pointer.
- **Fix:** `Session(host, ctx)` in the TS layer: the ctx keeps the host alive, and
  disposing of the session zeroes the ctx (`2790143`). Hosts hand scripts Sessions.
- **How to spot it next time:** run under `--expose-gc` (or `setFlagsFromString` plus
  `runInNewContext('gc')`), drop the owner, `gc()`, allocate another, then use the view.
  Any bound function returning a pointer into an argument can do this.

### A script command reports success but does nothing
- **Symptom:** an `async exec` was recorded in the history as a success, `count` never
  changed, and an unhandled rejection ("bound object was disposed") named no command.
- **Cause:** the TS `Adapter` called the user's method and returned `undefined`, so
  litestl's "returned a Promise" check never saw the Promise. The body ran after the
  slot closed, against a dead ctx.
- **Fix:** each adapter slot rejects a returned thenable (`2790143`).
- **How to spot it next time:** an unhandled rejection mentioning a disposed ctx means
  some script code outlived its slot: an `await`, a timer, or a ctx kept in a field.

### A test expecting NotFound gets Internal after a failed undo
- **Symptom:** `test_command_values` failed: undo returned Internal instead of the
  NotFound the failing write produced.
- **Cause:** `LinearHistory::undo` built its own Internal error and ignored the sticky
  error the failing slot had left on the Runtime.
- **Fix:** take the sticky error and keep its code, combined with the "dropped" message
  (`b881658`).
- **How to spot it next time:** when a code changes between the slot and the caller,
  look for a layer that makes a new Status instead of passing on or taking the sticky one.

### "NotFound: NotFound: ..." in script errors
- **Symptom:** errors from a script command's slot had their code prefix repeated once
  per nesting level.
- **Cause:** the TS layer passed `statusText(s)` (which includes the code) back as the
  message of the next Status.
- **Fix:** `ScriptError` carries `detail` (the Status's own message), and slots pass that
  on (`e7bf769`).
- **How to spot it next time:** compare `e.detail` with `e.message`; only `message` should
  have the "Code: " prefix.

### Generated TS fails to compile on interface names
- **Symptom:** `tsc` errors on the generated bindings, at identifiers like
  `coredcc.Command`.
- **Cause:** litestl derives TS identifiers from binding names, and a `.` in a name isn't
  turned into a namespace. Only `::` is.
- **Fix:** bind names as `coredcc::X`, never `coredcc.X` (`1d0f667`).
- **How to spot it next time:** grep the generated directory for the name; any binding
  name with a `.` is suspect.

### `alloc::New` fails with a deleted copy constructor
- **Symptom:** a compile error about `ForeignBindings`' deleted copy constructor, at a
  call to `litestl::alloc::New<T>(name, obj)`.
- **Cause:** `alloc::New` takes its constructor arguments by value, so passing an object
  copies it.
- **Fix:** pass a pointer (`this`) and have the constructor take `T *` (`1d0f667`).

### `spike_ts` missing from ctest, or skipped
- **Symptom:** wasm-st ctest shows `spike_ts` as Skipped (or, before `2790143`, ran 36
  tests and passed).
- **Cause:** `tests/wasm/node_modules` isn't installed. `run.mjs` exits 77, which
  `SKIP_RETURN_CODE` turns into a skip.
- **Fix:** `npm ci` in `tests/wasm`.
- **How to spot it next time:** check the ctest summary for Skipped, not only the pass
  count.

### A path with `%` breaks a node tool on Windows
- **Symptom:** esbuild wrote to `pWindows_NTq` when asked for `p%OS%q`.
- **Cause:** `node_modules/.bin/*.cmd` shims run through `cmd.exe`, which expands `%VAR%`
  even inside quotes.
- **Fix:** run the package's JS entry point with `process.execPath`, with no shell
  (`2790143`).

### Builds pass on clang-cl but fail on MSVC
- **Symptom:** `native-msvc` fails with C4267 (`size_t` to `int`) where `native-clang`
  builds cleanly.
- **Cause:** litestl's `Vector::operator[]` takes `int`, and MSVC warns on the narrowing
  that clang-cl accepts. With `/WX` it is an error.
- **Fix:** cast the index (`v[int(i)]`). This is why every C++ change builds all four
  presets.

### The test app has no backend, or TA1's tests all skip
- **Symptom:** `info().backend` is `'none'`, and TA1's tests are skipped (outside ctest)
  or fail with "the page has no backend" (under ctest). `info().backendError` and the
  CDP log say why.
- **Causes, by message:**
  - "no --apptest-module given": the page was launched without the launcher.
  - A failed import of `backend-wasm.js`: `build.mjs` found no wasm-st module and built
    the page without the backend. Build the wasm-st preset, then `pnpm run build`.
  - "not compiled for this environment (did you build to HTML...)": a Debug web module
    (emscripten ASSERTIONS) saw `process.versions.node`, which NW.js has. The page hides
    `globalThis.process` around the module factory (`src/backend/wasm.ts`,
    `instantiate`); this error means that was bypassed.
  - "Failed to fetch dynamically imported module: node:module": the page loaded a module
    built for node too (`-sENVIRONMENT=web,node`). NW.js takes the node path; use the
    web-only `coredcc_testapp.mjs`.
- **How to spot it next time:** `node apps/testapp/cdp.mjs --port <port> call info`.

## Techniques

### Checking that a regression test can fail
- Disable the fix by changing its condition (`if (false && ...)`), not by removing the
  call, so the rest of the code path still runs. The test should fail; then restore the
  fix.
- For TS, copy the file to the scratchpad first and copy it back afterwards, then grep
  for the disabled marker to confirm it's gone.
