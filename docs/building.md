# Building and testing

Only Windows is built and tested so far.

## What you need
- **Visual Studio 18** (Community), with the C++ and clang-cl components. `tools\devshell.cmd <command>`
  runs a command in its environment: cl, clang-cl, CMake (3.25 or later) and Ninja.
  - From Git Bash, set `MSYS_NO_PATHCONV=1`, or use PowerShell.
- **litestl**, the submodule: `git submodule update --init`.
- **The hook** that keeps the submodule on litestl's master: `git config core.hooksPath tools/hooks`, once
  for each clone.
- **For WASM, and the tests of scripts:**
  - Node 22.18 or later (of 23, 23.6 or later), and pnpm.
  - The pinned Emscripten SDK: `node tools/install-emsdk.mjs`. It goes into `build/deps/emsdk`.
  - `npm ci` in `tests/wasm`, and `pnpm install` in `apps/testapp`.

## Presets

| Configure preset | Compiler | Build and test presets |
|---|---|---|
| `native-clang` | clang-cl | `native-clang-debug`, `native-clang-release` |
| `native-msvc` | cl | `native-msvc-debug`, `native-msvc-release` |
| `wasm-st` | Emscripten, no threads | `wasm-st-debug`, `wasm-st-release` |
| `wasm-mt` | Emscripten, pthreads | `wasm-mt-debug`, `wasm-mt-release` |

- `-release` is RelWithDebInfo. A preset's build folder is `build/<configure preset>`.
- **Configure:** `tools\devshell.cmd cmake --preset native-clang`
- **Build:** `tools\devshell.cmd cmake --build --preset native-clang-debug -j 5`. Without `-j`, a machine with
  many cores may run out of memory.
- **Test:** `tools\devshell.cmd ctest --preset native-clang-debug`. One test: add `-R test_base_signal`.
  - Four tests run at a time (`jobs` in `CMakePresets.json`; `-j 1` for one). Those that are `SERIAL` in
    `tests/CMakeLists.txt`, and `testapp`, run alone: their time limits are relative or tight.
- WASM tests run in node.
- `wasm-mt` needs COOP and COEP headers in a browser. `wasm-st` doesn't.

## Gates
Run them by hand: they are not git hooks, and nothing else runs them.

| Gate | When | What |
|---|---|---|
| `node tools/gate.mjs pre-commit` | before a commit of anything but documentation | builds and tests `native-clang-release` |
| `node tools/gate.mjs pre-push` | before master is pushed, on the commit that is pushed | builds and tests the four native presets, `wasm-st-debug` and `wasm-mt-debug`; builds the benchmarks |

- **They fail** if a test fails, times out, is skipped, is disabled or can't run, and if the script tests
  (`spike_ts`, `testapp`) aren't there. Missing dependencies are a failure.
- **They fail if the code changes while they run.** Documentation may change: `docs/` and `*.md`.
- **After a failure** the other presets still run. Logs are `build/gate/<preset>.log`.
- **Options:** `--jobs N` (5), `--fresh` to configure from scratch, and `--only <preset>`, as often as you
  like, to run a part. A part isn't the gate.
- **What they don't do:** sanitizers, `wasm-*-release`, running benchmarks, formatting, or any other
  operating system. The test app's `pnpm run typecheck` is separate.
- The gate's own tests: `node --test tools/gate.test.mjs`.

## Formatting
`.clang-format` is litestl's, and all of `source/` and `tests/` is formatted with it (2026-11-25). Nothing checks
it. `tools\devshell.cmd clang-format -i <file>` formats a file.

## Test app
`apps/testapp` is an NW.js app that hosts the wasm-st module. Build `wasm-st-debug` first.
- **Tests:** `pnpm test` there, or the wasm-st ctest `testapp`. They launch NW.js on throwaway profiles and
  drive it over CDP.
- **By hand:** `pnpm run launch`, then `node apps/testapp/cdp.mjs --port <port> call info`.
- **Types:** `pnpm run typecheck`.
- Design: [plans/2026-11-19-test-app.md](plans/2026-11-19-test-app.md).

## Benchmarks
They aren't tests, and no normal build builds them. Always run them with these tools, which build first: a
program from an earlier build has given wrong numbers before.
- `node tools/bench.mjs graph|scene|scene_commands`
  - `--keep FILE` keeps the program as a baseline.
  - `--against FILE` runs the baseline and the new one in turns, and waits until other programs leave the
    machine alone.
- `node tools/bench-bvh.mjs` measures the BVH's leaf size, and writes [bvh-leafsize.md](bvh-leafsize.md).
- Run them on an idle machine, and never during a gate.

## Build cache and worktrees
Native builds share a cache between checkouts, if a fork of sccache with named-pipe support is on the PATH.
Without it they build without a cache.
- `node tools/cache.mjs setup`, `stats`, `zero` and `stop`. Never run `sccache` by hand.
- `node tools/new-worktree.mjs <branch> [--deps]` makes a worktree next to the main checkout.
- Details: [debugging.md](debugging.md#build-cache-and-worktrees).

## As part of another project
Add coredcc as a subdirectory. It uses the host's litestl if there is one, and leaves the flags to the host.
See [embedding.md](embedding.md#host-build-flags).
