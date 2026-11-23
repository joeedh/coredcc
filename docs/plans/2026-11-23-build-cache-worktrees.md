# Build cache and worktrees (end of P1)

Status: **the cache design below failed its pressure test (2026-11-23) and must not be
implemented as written.** The record and the options are in "Pressure test" at the end.
The worktree and emsdk parts survive with the corrections listed there. Replaces the
sketch in `docs/implementation-tasklist.md`, "Build cache for the parallel phase".

## Goals
- A new worktree's first native build is cache hits, with clang-cl and with MSVC.
- Up to 3 worktrees, plus the main checkout, build at the same time without colliding.
- Creating a worktree is one command. Removing one can't damage anything shared.

## Non-goals
- Caching WASM builds (emcc runs through a wrapper that sccache doesn't recognise).
- Caching for a host project that embeds coredcc. The host owns its launcher.
- Distributed or remote caching.

## sccache on this machine
- It is the user's fork, `C:\dev\sccache\target\release`, on the machine PATH (0.20.0).
- Its named-pipe transport is **opt-in**: without `SCCACHE_SERVER_PIPE` it listens on TCP
  4226. A hard-killed TCP server can leave that port orphaned until a reboot.
- **Rule: coredcc never runs sccache without `SCCACHE_SERVER_PIPE` set, and never
  force-kills a server.**

## Measured (2026-11-23)
The whole tree, Debug, 98 compiles. Two copies of the source at different paths, one
server with both roots in `SCCACHE_BASEDIRS`, a private pipe and a private cache.

| | no sccache | tree A, all misses | tree B, 98/98 hits |
|---|---|---|---|
| clang-cl | 13.4 s | 16.4–18.3 s | 4.0–5.1 s |
| MSVC, `/Z7` | 20.6 s | 20.1–21.1 s | 8.1–10.3 s |

- **MSVC with `/Zi` (CMake's default) fails**: concurrent compiles can't open the shared
  PDB (C1041). With `/Z7` it works, and all 98 hit. The old note "cache only clang-cl"
  is wrong.
- Tree B passes 37/37 on tree A's objects, with both compilers.
- A cached object keeps the paths of the tree that compiled it. Tree B's
  `test_command_review.exe` names tree A 20 times (clang-cl) or 46 times (MSVC).
- Turning preprocessor cache mode off (`SCCACHE_DIRECT=false`) kept 98/98 hits and cost
  no measurable time (5.1 s and 8.1 s, against 4.5 s and 10.3 s on).
- **What these numbers don't show:** hit counts and passing tests missed wrong line
  numbers and missing dependencies (see "Pressure test"). The two trees were also
  identical copies, which real worktrees aren't.

## Design

### The launcher: `tools/sccache-wrapper/`
- `sccache_launcher.cc` and `setup.mjs`, copied from faberforge
  (`webgl-app-framework/tools/sccache-wrapper`) with the changes below. The launcher
  keeps the union of every registered worktree root in `SCCACHE_BASEDIRS`, and restarts
  the server only when the union gains a root.
- **Environment it sets, when unset:**
  - `SCCACHE_SERVER_PIPE=sccache-<USERNAME>`, before any sccache call. This is
    faberforge's documented name.
  - `SCCACHE_DIRECT=false`. With basedirs, sccache's docs say preprocessor cache mode
    can silently return a stale object when one checkout has a header that another
    lacks. Feature branches do that.
  - `SCCACHE_IGNORE_SERVER_IO_ERROR=1`, so a server restart during someone else's build
    falls back to the compiler.
- **Arguments:** faberforge's launcher passes them through `_spawnv`, which joins them
  without quoting, and so breaks `-D"..."` string defines. Ours passes the raw tail of
  `GetCommandLineW()` to `CreateProcessW`.
- **Shared folder, separate binary:** the registry is `C:\dev\sccache-worktrees`, beside
  the worktrees, as faberforge's is. Two projects on one pipe share one server, so it
  must know both projects' roots.
  - The binary is `coredcc-sccache-launcher.exe`. Both `setup.mjs` scripts rebuild on
    mtime, so sharing one name would make the two projects overwrite each other's.
  - The "applied" state is per pipe, `.applied-<pipe>`: faberforge's launcher runs a TCP
    server unless the variable is set, and its `.applied` says nothing about ours.
- **Without sccache on PATH** it runs the bare compiler.

### CMake
- `cmake/CoredccCache.cmake`, included by the top-level `CMakeLists.txt` after
  `project()`, only when `PROJECT_IS_TOP_LEVEL`, on Windows, and not under Emscripten:
  - `CMAKE_MSVC_DEBUG_INFORMATION_FORMAT` defaults to `Embedded` (`/Z7`), with or
    without the cache, so builds don't differ by machine.
  - `COREDCC_SCCACHE` (default ON): if `../sccache-worktrees/coredcc-sccache-launcher.exe`
    exists, it becomes `CMAKE_C_COMPILER_LAUNCHER` and `CMAKE_CXX_COMPILER_LAUNCHER`,
    unless the user set those.
  - **No fallback to a bare `sccache` on PATH**, unlike faberforge: that would start a
    TCP server.
- litestl is a subdirectory, so it gets both settings.

### Paths in cached objects
- `detail::assertFailed` prints the path from `source/` or `tests/` on, so an assertion
  can't name another worktree's file. `__FILE__` is used nowhere else.
- Debug info still names the tree that compiled the object. To debug in a worktree,
  configure it with `-DCOREDCC_SCCACHE=OFF`. This goes in `docs/debugging.md`.

### emsdk in a worktree
- `cmake/Emscripten.cmake` becomes the presets' toolchain file, and includes the real
  one from the first of these that exists:
  1. `$ENV{COREDCC_EMSDK_DIR}`;
  2. `<root>/build/deps/emsdk`;
  3. in a linked worktree, `<main checkout>/build/deps/emsdk`, found from the worktree's
     `.git` file.
- No junctions: deleting a worktree could follow one into the 1.9 GB install.

### `tools/new-worktree.mjs <branch> [--base <ref>] [--deps]`
- Refuses when 3 worktrees already exist besides the main checkout, or when the target
  folder exists.
- Runs `git worktree add -b <branch> ../coredcc-<branch> <base>` (base: `master`), then
  `git submodule update --init` in it, and registers the root with `setup.mjs --root`.
- `--deps` also runs `npm ci` in `tests/wasm` and `pnpm install` in `apps/testapp`.
  Without them those two ctests skip.
- Prints the build commands. It builds nothing.
- Removal follows the user's procedure for Windows worktrees; the tool doesn't remove.

## Tests
- `tools/sccache-wrapper/test.mjs` (`node --test`), with a private pipe, cache and
  registry:
  - a string define reaches the compiler intact;
  - a missing sccache runs the bare compiler;
  - the launcher sets the pipe when unset, and keeps one that is set;
  - no TCP listener appears on 4226.
- By hand, once: two real worktrees, both presets; the second's first build is all hits
  and its tests pass.

## Risks
- An idle server exits after 10 minutes; the next compile starts one with the full
  union, because every start goes through the launcher. A server started by hand without
  `SCCACHE_BASEDIRS` would only cause misses.
- Restarting the server when a worktree joins can disturb a build in another worktree.
  It falls back to compiling locally.
- Savings are modest today (about 9–11 s per full Debug build), and grow with the tree.

## Pressure test (2026-11-23)
A fresh agent was given this plan and the code, not the author's reasoning. The author
reproduced the items marked "reproduced" with separate scripts.

### Broke
- **clang-cl with preprocessor mode off returns wrong objects** (reproduced). sccache
  then keys on `-EP` output, which has no line markers, and clang-cl collapses comment
  lines in it.
  - `std::source_location::current().line()` gave 4 from the cache where the plain
    compiler gives 7, after adding three comment lines above it. cl.exe was right.
  - Debug line numbers go stale on every comment-only edit.
  - A header first compiled as a system include, then under `/W4 /WX`, hit the cached
    success and skipped the warning. `warning_probe` guards against exactly that.
- **Preprocessor mode on loses dependency tracking** (reproduced, tree B, 98/98 hits).

  | | mode off | mode on |
  |---|---|---|
  | clang-cl: rebuilt after touching `status.h` | 54 | 0 (97 of 98 objects have no deps) |
  | MSVC: rebuilt after touching `status.h` | 54 | 0 (deps name tree A's headers) |

- **So with this sccache, only MSVC with the mode off is sound.** clang-cl is sound in
  neither mode.
- **Restarting the server fails other worktrees' builds**, 4 times out of 4: "All pipe
  instances are busy (os error 231)". `SCCACHE_IGNORE_SERVER_IO_ERROR` doesn't cover
  connecting. The launcher restarts the server whenever a worktree joins.
- **A real worktree isn't all hits.** `core.autocrlf=true`, and the main checkout has 174
  LF and 12 CRLF files (reproduced); a fresh checkout is all CRLF. MSVC hit 63/98.
- **Sharing faberforge's registry and cache folder.** Adding coredcc's roots makes
  faberforge's launcher restart its TCP server mid-build. Two servers on one cache
  folder "race and cause spurious build failures" (sccache's `Local.md`).
- **String defines holding paths cross trees:** `-DROOT="<tree>/..."` is not in the MSVC
  key, so tree B got tree A's path.
- **Smaller:** nothing registers the main checkout; branch names with `/` break the
  sibling folder; `.applied` never shrinks; `setup.mjs` compiles through a script
  coredcc lacks, and would relink a running exe; three cold MSVC builds at once ran out
  of memory (with about 41 of 49 GB already committed by other programs).

### Survived
- Pipe mode opens no TCP port, and a server autostarted inside ninja doesn't hang the
  caller or pin the worktree.
- MSVC `/Zi` fails under sccache with C1041, and `/Z7` works, PDBs included.
- Three trees building at once without a restart: no pipe errors.
- `submodule update --init` in a linked worktree, path lengths, and `__FILE__` being used
  only in `assert.h`.
- The emsdk wrapper toolchain. Existing build folders ignore it until recreated.

### Corrections to keep for the parts that survive
- **Worktree tool:** replace `/` in the branch name for the folder; run
  `git worktree prune` before counting; cap build jobs.
- **emsdk:** find the main checkout with `git rev-parse --git-common-dir`; store the
  chosen path in the build folder, so a re-run of cmake without the environment variable
  still works; teach `install-emsdk.mjs --check` about the shared install.
- **`assertFailed`:** trim at the last `source/coredcc/` or `tests/`, not the first.
- **Line endings:** `* text=auto eol=lf`, for coredcc and (by PR) litestl, whatever
  happens with the cache.

### Options for the cache (the user's decision)
1. **No cache for now** (recommended). Do the worktree parts. A full Debug build is 13 s
   (clang-cl) or 21 s (MSVC), so the cache saves about 10 s per fresh worktree. Revisit
   when a full build passes a minute.
2. **MSVC only, mode off.** Sound as far as tested. Needs `/Z7`, LF endings, and a server
   that is never restarted: a separate pipe, cache folder and `SCCACHE_BASEDIRS`, with
   fixed worktree folder names. Saves about 10 s on MSVC and nothing on clang-cl.
3. **Fix the fork first**, then cache both: preprocess clang-cl with line markers (`-E`);
   in preprocessor mode, refuse to cache when it can't restore the dependency file and
   rewrite MSVC's recorded paths; on a busy pipe, retry longer, then compile locally.
