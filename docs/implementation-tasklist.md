# Implementation task list

The master list of plans and their state. The architecture plan
(`docs/plans/2026-11-19-architecture.md`) defines the phases. Each phase gets its own
implementation plan before work starts, and each plan is pressure-tested first (see
CLAUDE.md).

States: `todo`, `planning`, `pressure-tested`, `in progress`, `done`, `reviewed`.

## Plans

| # | Plan | Phase | Depends on | State | Feature report |
|---|---|---|---|---|---|
| 1 | [Architecture](plans/2026-11-19-architecture.md) | all | — | pressure-tested | — |
| 2 | [Integration test app](plans/2026-11-19-test-app.md) | TA0–TA4 | staged with P0–P6 | in progress (TA0 done) | — |
| 3 | [P0 scaffold and interface system](plans/2026-11-19-p0-scaffold-interfaces.md) | P0 | 1, 4 | reviewed | [interfaces](reports/feature-report-interfaces-2026-11-20.md) |
| 4 | [P0b litestl PRs](plans/2026-11-19-p0-scaffold-interfaces.md#litestl-prs-p0b) | P0b | 1 | done (litestl #4 B, #5 A, #6 C1 merged; C2 moves to P1f) | — |
| 5 | [P1 application spine: library, props, datapath, commands](plans/2026-11-20-p1-application-spine.md) | P1 | 3 | in progress (P1a-P1d reviewed; P1e done) | [props and parsers](reports/feature-report-props-parse-2026-11-21.md); [library and datapath](reports/feature-report-lib-api-2026-11-21.md); commands and undo |
| 6 | P2 layout and library IO | P2 | 5 | todo | layout |
| 7 | P3 objects, transforms and BVH | P3 | 5 | todo | objects and transforms; BVH |
| 8 | P4 tools, overlay rendering, Unreal spike | P4 | 7 | todo | tools and overlay |
| 9 | P5 graph and instancing | P5 | 5 | todo | graph |
| 10 | P5c scene evaluator | P5c | 7, 9 | todo | scene evaluator |
| 11 | P6 integration: N-API addon, faberforge and ubertopo spikes | P6 | 8, 10, litestl N-API PR | todo | integration |

Each plan file is created in `docs/plans/` when its phase starts, and its row is linked
then.

## Process

### Feature reviews
- When a major architectural feature is complete, a fresh agent reviews the feature's
  whole API surface. It gets the headers, the bindings and the generated TS, and the
  relevant plan section, but not the author's reasoning.
- It looks for: inconsistent naming and conventions, leaky or missing abstractions,
  ownership and lifetime traps, things the bindings or foreign implementations can't
  express, gaps against the plan, and APIs that will be hard to change later.
- Its findings are rewritten (not pasted) into
  `docs/reports/feature-report-<feature>-<YYYY-MM-DD>.md`, with each finding's
  resolution: fixed, deferred (with where it's tracked), or rejected (with why).
- The feature's row above links the report and moves to `reviewed`.
- The features to review are those named in the "Feature report" column.

### Where work happens
- **Serial phase: directly on `master`.** P0, P0b and P1 are a chain, so there is nothing
  to parallelize until P1 is done.
- **litestl changes** are made in coredcc's own submodule checkout (`source/litestl`) on a
  branch, and go up as PRs against `joeedh/litestl`. Never in `C:\dev\litestl`.
  - The gitlink on master only points at litestl master commits. coredcc work that needs
    an unmerged PR waits on one local branch until the PR merges.
- **Parallel phase: worktrees, at most 3 at a time.** After P1, P2, P3 and P5 are
  independent. Then P4 (after P3) and P5c (after P3 and P5).
  - Worktrees are siblings of the repo: `C:\dev\coredcc-<branch>`.
  - Each branch is rebased onto master and merged with `git merge --ff-only`.

### Build cache for the parallel phase
Set up before the first worktree, following sculptcore and faberforge:
- **sccache** as the compiler launcher, with faberforge's cross-worktree launcher
  (`webgl-app-framework/tools/sccache-wrapper`). It keeps the union of every live
  worktree's root in `SCCACHE_BASEDIRS`, so one sccache server shares hits across all
  worktrees.
- coredcc gets its own copy of `setup.mjs` and `sccache_launcher.cc` in
  `tools/sccache-wrapper/`. It writes into the same shared directory,
  `C:\dev\sccache-worktrees`, so coredcc and faberforge worktrees share one server.
- **Caveats carried over from sculptcore:**
  - Cache only clang-cl builds. MSVC objects are cacheable only with `/Z7`, which `/Zi`
    defeats.
  - The launcher mangles command-line `-D"..."` string defines on Windows. Pass strings
    through generated config headers instead.
  - After each build, grep the log for `FAILED`, because of an occasional sccache
    pipe-busy flake. Retry after `sccache --stop-server`.
- sccache is not installed on this machine yet (checked 2026-11-19); install it first.

## Current focus
- litestl #4, #5 and #6 merged; coredcc pins litestl master `7bab16b` (2026-11-21).
- P1 on master:
  - P1a (props) and P1b (parsers) are done and reviewed. The review's findings are
    fixed or tracked in `docs/reports/feature-report-props-parse-2026-11-21.md`.
  - P1c (lib) and P1d (api: resolver, canonical paths, ChangeBus) are done and
    reviewed together: `docs/reports/feature-report-lib-api-2026-11-21.md`.
  - P1e (command: executor, LinearHistory, undoable sets, LastUsed, block commands) is
    done; its review comes with P1f.
  - Next: P1f (WASM toolchain, bind/ facades, the TS spike, TA1). It picks up the
    binding-surface items from both reviews.
- Test app TA0 is done (6 tests pass): `cd apps/testapp && pnpm install && pnpm test`.
