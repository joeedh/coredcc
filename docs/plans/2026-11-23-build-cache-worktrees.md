# Build cache and worktrees (end of P1)

Status: the first cache design failed its pressure test (2026-11-23). The user had the
sccache fork fixed (2026-11-24), and this second design replaces it. To be pressure-tested.
Replaces the
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

## Measured (2026-11-24, the fixed fork)
Two copies of the tree, 98 Debug compiles, the launcher described below.

| | first tree, all misses | second tree, 98/98 hits |
|---|---|---|
| clang-cl | 14.5-18.1 s | 4.7-5.9 s |
| MSVC, `/Z7` | 16.2-23.5 s | 5.9-9.6 s |

Both second trees pass 37/37. A string define with spaces, quotes and `&` survives
`cmake -E env`, nothing about it is in `compile_commands.json`, and nothing listens on
4226.

## Measured before the fix (2026-11-23)
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

## Design (second version, 2026-11-24)
The first version (a launcher binary, a registry and server restarts) is in git at
`e68c314`. With the fork fixed, the launcher can be `cmake -E env`.

### The cache folder: `<parent of the main checkout>/coredcc-cache/`
- Holds `sccache.toml` and the cache itself (`objects/`). Not shared with faberforge.
- `sccache.toml` is written only by `tools/cache.mjs`: `basedirs` (every checkout that
  `git worktree list` names, the main one included), `[cache.disk] dir` and `size` (5 GB).
- The pipe is `sccache-coredcc-<user>`.

### CMake: `cmake/CoredccCache.cmake`
Included by the top-level `CMakeLists.txt` before `project()`, only when coredcc is the
top-level project, on Windows, and not for Emscripten.
- `CMAKE_MSVC_DEBUG_INFORMATION_FORMAT` defaults to `Embedded` (`/Z7`), with or without the
  cache. Policy CMP0141 is NEW (implied by `cmake_minimum_required` 3.25).
- Option `COREDCC_SCCACHE` (default ON). When it is ON, the folder's `sccache.toml` exists
  and `sccache` is on PATH, the compiler launchers become:
  `cmake -E env --unset=SCCACHE_BASEDIRS --unset=SCCACHE_DIR --unset=SCCACHE_DIRECT
  SCCACHE_SERVER_PIPE=... SCCACHE_CONF=... SCCACHE_IGNORE_SERVER_IO_ERROR=1 <sccache>`.
  So a build can never start a TCP server, whatever shell runs it.
- They are normal variables, not cache entries: turning the option off takes effect on the
  next configure. A launcher the user passed with `-D` wins.
- Otherwise the build doesn't use sccache; there is no bare-`sccache` fallback. Configure
  prints which case applies.

### `tools/cache.mjs`
- `setup`: writes `sccache.toml` (to a temporary file, then rename) when its contents
  change. If a running server reports other basedirs, it stops it (`--stop-server`): the
  next compile starts one with the new file. Safe during builds, as the fork now outwaits
  a stopping server.
- `stats`, `zero`, `stop`: the sccache command with the pipe and config set. This is the
  only supported way to run sccache by hand here: `--zero-stats` and `--start-server`
  start a server, and without the config it would have no basedirs.
- Prints a notice and exits 0 when sccache isn't on PATH.

### `tools/new-worktree.mjs <branch> [--base <ref>] [--deps]`
- Runs from any checkout. Creates `../coredcc-<branch>` beside the main checkout, with `/`
  in the branch replaced by `-`.
- `git worktree prune`, then refuses if 3 worktrees exist besides the main checkout, or
  the folder exists. An existing branch is checked out; otherwise `-b <branch> <base>`
  (base: `master`).
- `git submodule update --init` in it, with the main checkout's `extern/litestl` as
  `--reference` when it is there.
- Runs `cache.mjs setup`. With `--deps`, `npm ci` in `tests/wasm` and `pnpm install` in
  `apps/testapp`; without them those two ctests skip.
- Prints the configure and build commands. It builds nothing, and doesn't remove
  worktrees; removal follows the user's procedure, then `cache.mjs setup`.

### emsdk in a worktree
- `cmake/Emscripten.cmake` becomes the presets' toolchain file. It picks the first of
  these that exists, remembers it in the cache as `COREDCC_EMSDK_DIR`, and includes the
  real toolchain file:
  1. `COREDCC_EMSDK_DIR` (cache, then environment);
  2. `<root>/build/deps/emsdk`;
  3. `<main checkout>/build/deps/emsdk`, from `git rev-parse --git-common-dir`.
- `install-emsdk.mjs` uses the main checkout's folder when run in a linked worktree.
- No junctions: deleting a worktree could follow one into the 1.9 GB install. Existing
  wasm build folders keep the old toolchain file until recreated.

### Paths
- `detail::assertFailed` prints the path from the last `source/coredcc/` or `tests/` on, so
  an assertion can't name another checkout's file. `__FILE__` is used nowhere else.
- Debug info names the checkout that compiled the object. To debug, configure with
  `-DCOREDCC_SCCACHE=OFF`. This goes in `docs/debugging.md`.
- No string define may hold a path: the compiler sees the value, the key doesn't.

### Line endings
- `.gitattributes`: `* text=auto eol=lf`, keeping `*.cmd` CRLF. The main checkout is
  renormalized once. litestl gets the same by PR.

### Memory
- Three cold MSVC builds at once ran out of memory. `new-worktree.mjs` prints a suggested
  `-j`; the launcher doesn't limit anything.

## Tests
- `tools/cache.test.mjs` (`node --test`), with a private pipe and folder: `setup` writes the
  file, is idempotent, includes a new worktree, and stops a server whose basedirs differ.
  A compile through the CMake launcher, from a shell with `SCCACHE_BASEDIRS`,
  `SCCACHE_DIR` and no pipe, opens no listener on 4226 and uses the private cache.
- By hand, once: two real worktrees, both native presets; the second's first build is all
  hits, `ninja -t deps` names only its own headers, a header edit rebuilds, 37/37 pass.
- The full gate afterwards.

## Risks
- A server idle for 10 minutes exits; the next compile starts one with the current file.
- A server started by hand with the pipe but without the config gives misses, not wrong
  objects.
- The savings are about 9 s (clang-cl) and 11-13 s (MSVC) per fresh checkout today.

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
  neither mode. (Corrected 2026-11-24: MSVC with the mode off isn't sound either. `/EP`
  misses blank lines moved between a header and its includer, and a renamed header.)
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

## The fork, fixed (2026-11-24)
Four commits on `C:\dev\sccache`, branch `windows-named-pipe-support`, `b7429e73` to
`d64d0f07`. Not pushed. A fresh agent tried to break them; what it found that the changes
caused or made reachable is fixed in them.

- **Keys:** cl and clang-cl preprocess with `-E`, so line markers are hashed. Objects still
  hit across trees listed in `SCCACHE_BASEDIRS`.
- **Dependencies:** the preprocessor cache is skipped when a hit couldn't report this
  tree's headers. This also affected `clang++ -MD -MF` with basedirs and default settings,
  which is how faberforge builds.
- **Pipe:** a restart under load no longer fails compiles (12 clients, a stop every 7 s:
  204 of 1005 failed before, 0 of 527 after; 24 clients, a stop every 0.6 s: 0 of 1024).
- **`SCCACHE_IGNORE_SERVER_IO_ERROR=1`** covers connecting. It doesn't cover an invalid
  client setting, such as a relative `SCCACHE_BASEDIRS`.
- **`strip_basedirs`** (upstream bug): some non-ASCII text in a source could give a wrong
  object or a failed compile.

Two trees, 98 compiles, default mode and mode off, clang-cl and MSVC: the second tree gets
98/98 hits, records only its own headers, rebuilds 54 objects after touching `status.h`
(0 before, in default mode), and passes 37/37.

### What this changes in the design
- Both compilers can be cached. `SCCACHE_DIRECT` can stay at its default.
- Restarting the server when a worktree joins is safe, so fixed worktree names aren't
  needed.
- Still needed: `/Z7`, LF line endings, a pipe and cache folder apart from faberforge's, no
  string defines that hold paths, and the corrections listed above.

### Known limits that remain
- Columns: `source_location::column()` and clang-cl's debug columns can be stale.
- With `/Z7`, the source checksum can be stale after edits that only change line endings,
  encoding or trailing blank lines.
- Debug info names the tree that compiled the object.
- `clang++` with `SCCACHE_DIRECT=false` preprocesses with `-P`, and so has the stale-line
  problem. Upstream does this on purpose, and a test asserts it.
- `clang++ -MD -MF` without basedirs replays the depfile of an earlier compile of the same
  source to another output, and `-MMD` and `-MD` share an entry.
