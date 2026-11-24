# Build cache and worktrees (end of P1)

Status: the first cache design failed its pressure test (2026-11-23). The user had the
sccache fork fixed (2026-11-24). The second design was pressure-tested and changed
(2026-11-24); being implemented.
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

## Design (second version, 2026-11-24, as changed by its pressure test)
The first version (a launcher binary, a registry and server restarts) is in git at
`e68c314`, and the second before its pressure test at `313ddcc`. The pressure test's record
is the last section.

### The cache folder
- `<parent>/.<main checkout's name>-cache/`, so `C:/dev/.coredcc-cache`. `COREDCC_CACHE_DIR`
  (environment, read by CMake and the tools) overrides it. Not shared with faberforge.
- The main checkout is the parent of `git rev-parse --path-format=absolute
  --git-common-dir`, run in the script's directory. There is no cache when that folder isn't
  named `.git` (coredcc as a submodule).
- Contents, written only by `tools/cache.mjs`:
  - `sccache.toml`: `basedirs` only, one per checkout in `git worktree list`.
  - `launcher.txt`: the launcher after `cmake -E env`, one argument per line. CMake and the
    tool share it, so the settings exist once.
  - `objects/`: the cache, 5 GB.
- Pipe: `sccache-coredcc-<hash of the folder's path>`, so two clones never share a server.

### The launcher
- `cmake -E env`, then:
  - sets `SCCACHE_SERVER_PIPE`, `SCCACHE_SERVER_PORT` (private, in case the sccache on disk
    is ever replaced by one without pipes), `SCCACHE_CONF`, `SCCACHE_DIR`,
    `SCCACHE_CACHE_SIZE`, `SCCACHE_LOCAL_RW_MODE` and `SCCACHE_IGNORE_SERVER_IO_ERROR=1`;
  - unsets `SCCACHE_BASEDIRS`, `SCCACHE_DIRECT`, `SCCACHE_CLIENT_SIDE`, `SCCACHE_RECACHE`
    and `SCCACHE_MULTILEVEL_CHAIN`;
  - then `--` and the sccache path.
- The disk settings are variables, not file entries, because any one of them in the
  environment replaces the file's whole `[cache.disk]`.

### CMake: `cmake/CoredccCache.cmake`
Included right after `project()`, before any target, when coredcc is the top-level project,
on Windows, and not for Emscripten (none of which is known before `project()`).
- `CMAKE_MSVC_DEBUG_INFORMATION_FORMAT` defaults to `Embedded` (`/Z7`), cache or not.
- Option `COREDCC_SCCACHE` (default ON). The launchers are set when it is ON,
  `launcher.txt` exists, and the sccache it names contains `SCCACHE_SERVER_PIPE`.
- They are normal variables, so the option takes effect on the next configure (and
  recompiles everything). A launcher in CMake's cache, which is where `-D` and the
  environment's `CMAKE_<LANG>_COMPILER_LAUNCHER` end up, wins. Configure warns when that
  launcher is an sccache without a pipe.
- No bare-`sccache` fallback. Configure prints which case applies. `launcher.txt` is a
  configure dependency once it exists; after the first `setup`, reconfigure by hand.

### `tools/cache.mjs`
- `setup`: finds the fork on PATH, writes the two files, then compares the basedirs that
  `--show-stats` reports with the wanted ones (that reports the server's if one runs, and
  the file's otherwise) and stops the server when they differ. The next compile starts one.
  - Files are replaced by rename, retried for some seconds: builds hold the toml open.
  - Repeats if `git worktree list` changed meanwhile (two worktrees created at once).
- `stats`, `zero`, `stop`: sccache with the launcher's environment. The only supported way
  to run it by hand here. `stop` accepts that no server runs.
- Without the fork on PATH: a notice and exit 0. A PATH entry that isn't the fork counts as
  absent.

### `tools/new-worktree.mjs <branch> [--base <ref>] [--deps]`
- Runs from any checkout. Creates `<parent>/<main checkout's name>-<branch>`, `/` replaced
  by `-`. Branch names are limited to `[A-Za-z0-9._/-]`, not starting with `-`.
- `git worktree prune`; refuses a fourth worktree. An existing branch is checked out;
  otherwise `-b <branch> <base>` (base: `master`). A folder that already is this branch's
  worktree is resumed (the submodule step needs the network and can fail).
- `git submodule update --init`, with `--reference <main>/extern/litestl --dissociate` when
  that has a `.git`. Without `--dissociate`, pruning the main one could break the worktree.
- Runs `cache.mjs setup`. With `--deps`, `npm ci` in `tests/wasm` and `pnpm install` in
  `apps/testapp`.
- Prints the next commands and a suggested `-j`: three cold MSVC builds at once ran out of
  memory. It doesn't remove worktrees: that follows the user's procedure, then
  `cache.mjs setup`. `git worktree remove --force` also deletes the worktree's own litestl
  repository, so unpushed litestl branches in it are lost.

### The pre-commit hook
- Git sets `GIT_DIR` for hooks in linked worktrees, so the hook's `git -C extern/litestl`
  used coredcc's repository and rejected every gitlink. It unsets `GIT_DIR`,
  `GIT_INDEX_FILE`, `GIT_WORK_TREE` and `GIT_PREFIX` for those calls.

### emsdk in a worktree
- `cmake/Emscripten.cmake` becomes the presets' toolchain file. It picks the first that
  exists, and includes the real toolchain file:
  1. `COREDCC_EMSDK_DIR` (cache, then environment);
  2. `<root>/build/deps/emsdk`, from `CMAKE_CURRENT_LIST_DIR`;
  3. `<main checkout>/build/deps/emsdk`.
- `COREDCC_EMSDK_DIR` is in `CMAKE_TRY_COMPILE_PLATFORM_VARIABLES`, or try_compile would
  pick another SDK.
- `install-emsdk.mjs` uses the main checkout's folder when run in a linked worktree.
- No junctions: deleting a worktree could follow one into the 1.9 GB install. Existing
  wasm build folders keep the old toolchain file until recreated.

### Paths
- `detail::assertFailed` prints the path from the last `source/coredcc/` or `tests/` on,
  with either separator (clang-cl gives `...\source\coredcc/base/status.h`).
- Debug info names the checkout that compiled the object. To debug, configure with
  `-DCOREDCC_SCCACHE=OFF`.
- No string define may hold a path: the compiler sees the value, the key doesn't.

### Line endings
- `.gitattributes`: `* text=auto eol=lf` first, then `*.cmd` CRLF (the last match wins).
- The main checkout's 12 CRLF files, when unmodified, are deleted and checked out again.
  **Never `git add --renormalize` or a reset:** they don't rewrite such files, and they
  would take the user's uncommitted edits along.
- litestl: no PR. Its blobs are LF, and with the machine's `autocrlf` every fresh checkout
  is CRLF. The main checkout's 18 LF files are rewritten the same way, to match. Drift
  costs misses, not correctness.

## Tests
- `tools/cache.test.mjs` (`node --test`), in a clone with `COREDCC_CACHE_DIR` set: `setup`
  writes the files, is idempotent, includes a new worktree, and stops a server whose
  basedirs differ. A CMake build from a shell that sets `SCCACHE_BASEDIRS`, `SCCACHE_DIR`,
  `SCCACHE_CACHE_SIZE` and a private `SCCACHE_SERVER_PORT` but no pipe uses the private
  cache and adds no listener on 4226. The hook accepts a valid gitlink in a worktree.
- By hand, once: two real worktrees, both native presets; the second's first build is all
  hits, `ninja -t deps` names only its own headers, a header edit rebuilds, tests pass.
- The full gate afterwards.

## Risks
- A server idle for 10 minutes exits; the next compile starts one with the current file.
- Checkouts under a non-ASCII path hit less (6 of 30 in a test); the objects are right.
- A hand-edited, malformed `sccache.toml` fails every compile.
- Concurrent wasm builds share one emsdk cache, which emcc locks itself. Not tested.
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
`d64d0f07`, pushed to `joeedh/sccache`. A fresh agent tried to break them; what it found that the changes
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

## Pressure test of the second design (2026-11-24)
A fresh agent built the launcher in a clone with real linked worktrees. No scenario gave a
wrong object.

Broke, and changed above:
- **Renormalizing** the main checkout rewrote nothing (185 CRLF files before and after, in a
  fresh clone) and committed an uncommitted `CLAUDE.md` edit.
- **Before `project()`**, `EMSCRIPTEN` and `PROJECT_IS_TOP_LEVEL` are empty and `WIN32` is 1
  even when cross-compiling. sccache fails (exit 2) on emcc instead of passing it through.
- **The pre-commit hook** rejected every gitlink committed from a linked worktree.
- **`--reference`** without `--dissociate` left `fatal: bad object HEAD` after a gc in the
  main submodule, and fails when the main `extern/litestl` is an empty folder.
- **Three unsets** didn't isolate the launcher: with `SCCACHE_CACHE_SIZE` or
  `SCCACHE_LOCAL_RW_MODE` set, sccache used the user's default cache folder.
- **One pipe per user** would be shared by two clones with different cache folders.
- **A branch called `cache`** put a worktree in the cache folder.
- **Renaming `sccache.toml`** failed 124 times of 202 while 16 clients ran.
- **An sccache without pipe support** would have used TCP 4226. An environment
  `CMAKE_CXX_COMPILER_LAUNCHER` is copied into CMake's cache and stays.
- Smaller: try_compile lost `COREDCC_EMSDK_DIR`; `--git-common-dir` is relative in the main
  checkout and points into `.git/modules` in a submodule; `;` in branch names; a rerun after
  a failed submodule step; both separators in `__FILE__`; `--stop-server` without a server
  exits 2; `=` in a launcher path needs `--`.

Held:
- A real linked worktree with MSVC: 98/98 hits, 37/37 tests, only its own headers, the same
  dependency counts as an uncached build.
- Four concurrent builds with a `--stop-server` every 2 s (13 stops): all succeeded.
- try_compile and Ninja's `/showIncludes` detection don't use the launcher; litestl gets
  `/Z7`; an empty `-D` launcher is respected; `basedirs` of `coredcc` and `coredcc-w1` don't
  match each other.

Not tested: emcc builds in a worktree, a host that embeds coredcc, memory use.
