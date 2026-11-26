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
- **Fix:** cast the index (`v[int(i)]`). The pre-commit gate builds with clang only, so
  MSVC's errors show in CI. `node tools/gate.mjs ci --only native-msvc-debug` shows them sooner.
- **The same for C4458** ("declaration of 'lib' hides class member"): a member function's parameter has
  the name of a member. clang-cl doesn't warn. Rename the member (drive/'s `State::library`).

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
  drawing; nothing calls it by itself. It evaluates again only the objects whose blocks were
  notified, what is below them, and the bounds it can't keep; a move of the epoch or of the
  time, or `notifyAll`, evaluates everything.
- **If it stays old after evaluateScene:** something changed an object without a notification
  (embedding.md, "Objects and geometry of a host"). Set `SceneSettings::incremental = false`: if
  that fixes it, find the write and notify the block after it.
- **If the bounds stay old after it:** the evaluator keeps those of a type that implements Invalidation, and the
  data block may keep them too. Whoever changes a block's data
  directly must call `invalidateData(ctx, block, kinds)`, which calls the block's `Invalidation`
  and posts the change for watchers. The kinds must include Topology or Positions.
- **If a driven channel stays old:** ask the Library's `OverrideProvider` itself, for that time. Only one that
  answers for another time or an older document can be the reason.
- **Commands and tools** that must not see old results use `computeState`, which reads the
  objects themselves.
- **The same goes for picking:** `pickScene` answers for what was drawn. Objects that were added or
  moved since are where they were. An object without bounds (no Spatial, or an empty
  `localBounds`) can't be picked at all.

### An object doesn't follow its driver graph
- **Is there a `SceneDrivers` for the Library,** and does the document's list of drivers name the graph
  (`DriverList::allGraphs`, or the path `driverGraphs`)? Without both the graph is data and drives nothing.
  A list that is a placeholder (its type wasn't registered when the file was loaded) drives nothing, and
  is a diagnostic.
- **Look at `SceneDrivers::diagnostics()`:** a channel the transform type doesn't have, an adapter of another
  kind, a channel driven twice (also by one subgraph used twice), a link that would make a cycle, an
  adapter inside a foreach body.
- **The object is ModelFailed:** `computeState` and the commands give the reason (FailedPrecondition, "its
  drivers: ..."). A graph that reads an object whose driver failed fails too.
- **It moves only after another edit:** the host changed something that a graph reads without a notification
  or an epoch move. `SceneDrivers` re-reads the document when the edit counter, the epoch or the time moves.
- **A cache of driven objects is stale after a graph edit:** driven objects are notified when the graphs are
  evaluated, not when they are edited. Call `SceneDrivers::update(time)` (evaluateScene does) before reading
  the cache.
- **An inner provider's change isn't seen:** it must notify its objects (`kDrivenAspect` is enough); graphs
  that read them are evaluated again then.

### A TS test's scene doesn't change after a script's model changed
- **Symptom:** after a transform model written in TS starts answering otherwise (it throws now, say),
  `evaluateScene` still gives the old result and doesn't fail.
- **Cause:** the scene's pass is incremental (P5c, part B). It evaluates only objects that were notified,
  whose parent moved, or that failed last time. A model whose answer changes without a change to the
  document isn't pure, and nothing tells the scene.
- **Fix:** call `notifyChanged(ctx, object)` after the change (`tests/wasm/scene.test.ts`), or set
  `SceneSettings::incremental = false` natively.
- **How to spot it next time:** `computeState` (TS `worldMatrix`) gives the new answer while the evaluated
  scene keeps the old one.

### `TypeMismatch: the pointer's target is of another struct type` from a Ref
- **Symptom:** `set(ctx, '<graph>.nodes[#n].params.object', ref)` fails in TS with a Ref from
  `core.object.add`, while native tests pass.
- **Cause:** TS Refs carry the block's own struct (`coredcc.Object`); native tests' pointers often have no
  struct. `coerce` compares struct ids exactly, so a param whose target is the base `coredcc.SceneObject`
  refused them.
- **Fix:** `GraphDef::setParamRef` makes a pointer to a struct that inherits from the target one to the
  target (`ApiRegistry::isA`) before it coerces.
- **How to spot it next time:** a native test that sets a pointer param with a typed `StableRef`
  (`testDerivedPointer` in `test_drive_review`).

### A TS test sees an object where it shouldn't be
- **Symptom:** an object made with `core.object.add(name="b")` is where the object made before it is.
- **Cause:** commands remember their last inputs (`LastUsedStore`): `location` came from the call before.
- **Fix:** give the inputs that matter each time.

### A benchmark's numbers don't fit the code
- **Symptom:** `bench_scene` gave 15 ms, and the same commit gave 21 ms elsewhere. Targets seemed met for hours.
- **Cause:** the program was old. The benchmarks aren't in `all`, so building and gating the rest doesn't
  rebuild them.
- **Fix:** `node tools/bench.mjs` builds before it runs, and prints the commit and the link time. CI builds
  the benchmarks, so that they compile.
- **How to spot it next time:** compare the program's time with the commit's. To find where a slowdown came
  from, build several commits from scratch, and run the programs in turns: that rules out the machine.

### Evaluating many objects is slower, and no code in its path changed
- **Symptom:** `evaluateScene` became slower by half, in two steps.
- **Cause:** `Object` had grown to 2,152 bytes. Two inline `Vector<BlockEntry>`s that only loading uses had been
  added to each object.
- **Fix:** what few objects have is allocated when it is needed. A `static_assert` in `object.cc` limits the
  size (`fd1aa75`).
- **How to spot it next time:** `sizeof` of what is evaluated in bulk, before looking for slow code.

### wasm-mt-release: "ReferenceError: ENV is not defined"
- **Symptom:** `test_base_id` fails under `wasm-mt-release`, every time, before `main`.
- **Cause:** `tests/support/node_env.js` copies the environment to `ENV`, which emscripten leaves out of a release
  build when the program doesn't read the environment.
- **Fix:** none yet. The debug build, which the gate uses, isn't affected.

### CI: clang-cl fails in MSVC's `<format>` ("implicitly-deleted default constructor of 'basic_format_arg'")
- **Symptom:** both clang jobs fail in litestl's `binding.cc`, inside `__msvc_ranges_tuple_formatter.hpp`. MSVC
  builds, and the same commit builds locally.
- **Cause:** GitHub's Windows image has LLVM 20 in `C:\Program Files\LLVM\bin`, before Visual Studio's clang on the
  PATH. MSVC 14.52's STL needs a newer clang. The "Compiler versions" step showed `clang version 20.1.8`.
- **Fix:** `tools/devshell.cmd` puts Visual Studio's `VC\Tools\Llvm\x64\bin` first.
- **How to spot it next time:** an error inside the STL's own headers, under clang only: check which clang ran.

### CI: a test's time limit fails on GitHub's machines
- **Symptom:** `test_graph_limits` failed in `native-msvc-release`: "the doubling graph took 1.16693 s". It passed
  locally in 0.5 s.
- **Cause:** GitHub's runners are about 2.3 times slower than the development machine, and run four tests at a time
  on four cores.
- **Fix:** limits on time are for catching runaway work (hours), so they get room: 5 s there.
- **How to spot it next time:** a test with `steady_clock` that fails only in CI. `grep -rln steady_clock tests`
  lists them.

### The test app's screenshot shows only the background
- **Symptom:** in TA3, `shot()` gave a canvas of the background colour, although `frame()` had drawn the
  gizmo without a WebGPU error.
- **Cause:** the test app runs in a hidden NW.js window. A hidden window doesn't composite frames that were
  drawn outside `requestAnimationFrame`, and rAF doesn't run there, so `capturePage` returns an old frame.
- **Fix:** the testbus method `pixel(x, y)` reads the canvas texture back from the GPU
  (`copyTextureToBuffer`; the canvas is configured with `COPY_SRC`). `ta3.test.ts` asserts on that.
- **How to spot it next time:** a screenshot that doesn't change after a draw, in a window that isn't
  shown. Read the pixel from the GPU before suspecting the renderer.

### A rotate gizmo's turn has the wrong sign, or none, in some views
- **Symptom:** a drag-math test failed for views where the eye is close to the gizmo: a turn came out with
  the wrong sign, or as no turn at all.
- **Cause:** the ring passed behind the eye. Such a ring projects to no closed curve on the screen, so it
  has no orientation there, and the sign of a turn can't be taken from it.
- **Fix:** the turn's sign comes from the screen's handedness at the pivot (`screenHandedness`). The test
  checks the sign only where the whole ring is in front of the eye (step 3's notes).
- **How to spot it next time:** check whether every point of the ring has `ViewPoint::behind == false`
  before blaming the turn's math.

### MSVC refuses a `std::span` of a struct (C2233) only in Unreal
- **Symptom:** the Unreal spike's module failed with C2233 ("arrays of objects containing zero-size
  arrays are illegal") at a `std::span<const InputEvent>`. Native builds were fine.
- **Cause:** `InputEvent` had a `litestl::util::Vector<PointerSample, 0>`. litestl keeps the inline storage
  in an array, and a zero-size array is an MSVC extension, which Unreal's flags turn into an error.
- **Fix:** `Vector<PointerSample, 1>` (`tool/input.h`). Don't give a litestl Vector 0 inline elements in
  a type that a host may see.
- **How to spot it next time:** C2233 pointing at a litestl Vector member. `tools/unreal-spike.mjs` builds
  the headers with Unreal's flags (S11).

### Eigen's C4668 in an Unreal module, although coredcc's includes are wrapped
- **Symptom:** C4668 in Eigen (`__LDBL_MANT_DIG__`, `__BYTE_ORDER__`) in translation units that include
  ITF headers, and in the module's generated file. The coredcc includes were all inside the warning push.
- **Cause:** the module put `extern/litestl/` on the include path, so engine headers that include
  `"Math/Vector.h"` (CoreUObject's `Misc/WorldCompositionUtility.h`) found litestl's `math/vector.h`,
  because Windows' file system ignores case. litestl's math, and Eigen with it, came in outside the push.
  Moving litestl's root to the system path broke litestl instead: its own `"math/matrix.h"` then found
  Unreal's.
- **Fix:** litestl includes its own headers, and Eigen, by relative path, so the module's include roots
  are only `source/` and `extern/`, and C4668 is an error again outside the push. Don't put
  `extern/litestl/` back on a host's include path.
- **How to spot it next time:** compile the failing file with `/showIncludes` from `Engine\Source` (where
  UBT runs the compiler) and look for an `extern\litestl` path under an engine header.

### A View from an `FSceneView` validates as "not a perspective"
- **Symptom:** `ViewFromSceneView` failed with "the projection isn't a perspective one: ortho is missing"
  for a view that a test built.
- **Cause:** `FViewMatrices::GetProjectionNoAAMatrix()` is only filled when the renderer adds TAA's jitter
  (`SaveProjectionNoAAMatrix`). Before that it is the identity.
- **Fix:** `ComputeProjectionNoAAMatrix()`, which takes the jitter out of the live projection.
- **How to spot it next time:** log the matrix. An identity projection is the sign.

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
