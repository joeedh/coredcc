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

### A native test dies with exit code 0xC00000FD (-1073741571)
- **Symptom:** a test exits with no output, and ctest reports it as a SegFault.
- **Cause:** a stack overflow (STATUS_STACK_OVERFLOW). Windows gives the main thread
  1 MB, and Debug frames are large: nested commands overflowed at 52 levels, about 20
  KB each, on both compilers.
- **Fix:** bound the recursion. Commands nest at most `CommandExecutor::kMaxNesting`
  (32) deep.
- **How to spot it next time:** run the test's exe directly and check `$LASTEXITCODE`.
  Print a depth counter to stderr to find where it stops.

### The test app says "the wasm-st module aborted"
- **Symptom:** every testbus call fails with "the wasm-st module aborted: ...", and
  `info().backendError` says the same.
- **Cause:** a trap, an abort (a failed `CDCC_CHECK`) or a poisoned module. The message
  ends with the module's last stderr lines, which hold the C++ check's text.
- **How to spot it next time:** read the message's tail, or the run's `cdp-log.json`
  under `apps/testapp/test-artifacts/`, which a failed run keeps.

### An object's world or bounds are old
- **Symptom:** `Scene::evaluated`, or `evaluated.objects[...]`, shows where an object was, or the
  bounds of geometry it had.
- **Cause:** results are from the last `evaluateScene`. Hosts call it once a frame, before
  drawing; nothing calls it by itself. It computes everything again, so it needs no
  notification.
- **If the bounds stay old after it:** the data block caches them. Whoever changes a block's data
  directly must call `invalidateData(ctx, block, kinds)`, which calls the block's `Invalidation`
  and posts the change for watchers.
- **Commands and tools** that must not see old results use `computeState`, which reads the
  objects themselves.
- **The same goes for picking:** `pickScene` answers for what was drawn. Objects that were added or
  moved since are where they were. An object without bounds (no Spatial, or an empty
  `localBounds`) can't be picked at all.

## Techniques

### Checking that a regression test can fail
- Disable the fix by changing its condition (`if (false && ...)`), not by removing the
  call, so the rest of the code path still runs. The test should fail; then restore the
  fix.
- For TS, copy the file to the scratchpad first and copy it back afterwards, then grep
  for the disabled marker to confirm it's gone.

### MSVC: LNK1235 "corrupt or invalid COFF symbol table"
- **Cause:** incremental linking. A build that changes many files (a rebase, a checkout) links
  some executables against stale `.ilk` files. The library named in the error is fine, and the
  next build works. Reproduced with the build cache off by building `4e3be76` then `3e6cc1d`
  (2 of 2 with `/INCREMENTAL`, 0 of 2 without).
- **Fix:** coredcc's own builds link with `/INCREMENTAL:NO` (`CMakeLists.txt`).

## Build cache and worktrees
Design: `docs/plans/2026-11-23-build-cache-worktrees.md`. Windows only, native builds only.

- **New worktree:** `node tools/new-worktree.mjs <branch> [--deps]`.
- **Removed a worktree:** run `node tools/cache.mjs setup` afterwards. Its own litestl
  repository goes with it, so push litestl branches first.
- **Is it working?** Configure prints `coredcc: using the build cache`, or why not.
  `node tools/cache.mjs stats` has the hits. An all-hit build still preprocesses every
  source and links everything.
- **Suspect the cache:** configure with `-DCOREDCC_SCCACHE=OFF` (recompiles everything).
- **Debugging:** an object's debug info names the checkout that compiled it. If the debugger
  opens another checkout's file, rebuild with the cache off.
- **Never run `sccache` by hand.** `--show-stats` and the like start nothing, but
  `--zero-stats`, `--start-server` and compiles start a server with the wrong settings. Use
  `tools/cache.mjs`.
- **No string define may hold a path.** The key has its checkout's folder removed, so
  another checkout would get the wrong string.
- **wasm in a worktree** uses the main checkout's emsdk (`cmake/Emscripten.cmake`). Build
  folders from before 2026-11-24 need `cmake --fresh --preset wasm-st`.
- **Line endings:** `git ls-files --eol` shows files that differ from a fresh checkout
  (`w/crlf` in coredcc, `w/lf` in litestl). They only cost misses. To fix an *unmodified*
  one, delete it and `git checkout -- <file>`; `git add --renormalize` doesn't rewrite it.
- **Tests of the tools:** `node --test tools/cache.test.mjs`.
