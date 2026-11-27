# Implementation task list

The master list of plans and their state. The architecture plan
(`docs/plans/2026-11-19-architecture.md`) defines the phases. Each phase gets its own
implementation plan before work starts, and each plan is pressure-tested first (see
CLAUDE.md). `docs/index.md` lists all documentation.

States: `todo`, `planning`, `pressure-tested`, `in progress`, `done`, `reviewed`.

## Plans

| # | Plan | Phase | Depends on | State | Feature report |
|---|---|---|---|---|---|
| 1 | [Architecture](plans/2026-11-19-architecture.md) | all | — | pressure-tested | — |
| 2 | [Integration test app](plans/2026-11-19-test-app.md) | TA0–TA4 | staged with P0–P6 | in progress (TA0-TA3 done) | — |
| 3 | [P0 scaffold and interface system](plans/2026-11-19-p0-scaffold-interfaces.md) | P0 | 1, 4 | reviewed | [interfaces](reports/feature-report-interfaces-2026-11-20.md) |
| 4 | [P0b litestl PRs](plans/2026-11-19-p0-scaffold-interfaces.md#litestl-prs-p0b) | P0b | 1 | done (litestl #4 B, #5 A, #6 C1 merged; C2 moves to P1f) | — |
| 5 | [P1 application spine: library, props, datapath, commands](plans/2026-11-20-p1-application-spine.md) | P1 | 3 | reviewed | [props and parsers](reports/feature-report-props-parse-2026-11-21.md); [library and datapath](reports/feature-report-lib-api-2026-11-21.md); [commands and undo](reports/feature-report-command-2026-11-21.md); [script bindings](reports/feature-report-bind-2026-11-22.md); [TS spike](reports/feature-report-ts-spike-2026-11-22.md); [TA1](reports/feature-report-ta1-2026-11-23.md) |
| 6 | [P2 library IO and layout](plans/2026-11-24-p2-layout-library-io.md) | P2 | 5 | reviewed | [library IO](reports/feature-report-library-io-2026-11-24.md); [layout](reports/feature-report-layout-2026-11-25.md) |
| 7 | [P3 objects, transforms and BVH](plans/2026-11-24-p3-objects-bvh.md) | P3 | 5 | reviewed; timings to be confirmed on an idle machine | [objects and transforms](reports/feature-report-scene-2026-11-25.md); [BVH, View and picking](reports/feature-report-bvh-2026-11-25.md) |
| 8 | [P4 tools, overlay rendering, Unreal spike](plans/2026-11-25-p4-tools.md) | P4, P4b | 7 | reviewed | [tools and overlay](reports/feature-report-tools-overlay-2026-11-26.md) |
| 9 | [P5 graph and instancing](plans/2026-11-24-p5-graph.md) | P5 | 5 | reviewed | [graph](reports/feature-report-graph-2026-11-25.md) |
| 10 | [P5c scene evaluator](plans/2026-11-25-p5c-scene-evaluator.md) | P5c | 7, 9 | reviewed | [scene evaluator](reports/feature-report-scene-evaluator-2026-11-25.md) |
| 10b | [Naming pass](plans/2026-11-26-naming-pass.md) (audit: [naming](reports/naming-audit-2026-11-26.md)) | before P6 | 8, 10 | reviewed | [naming pass](reports/feature-report-naming-2026-11-26.md) |
| 11 | P6 integration: N-API addon, faberforge and ubertopo spikes | P6 | 8, 10, 10b, litestl N-API PR | todo | integration |

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
- **litestl changes** are made in coredcc's own submodule checkout (`extern/litestl`) on a
  branch, and go up as PRs against `joeedh/litestl`. Never in `C:\dev\litestl`.
  - The gitlink on master only points at litestl master commits. coredcc work that needs
    an unmerged PR waits on one local branch until the PR merges.
- **Parallel phase: worktrees, at most 3 at a time.** After P1, P2, P3 and P5 are
  independent. Then P4 (after P3) and P5c (after P3 and P5).
  - Worktrees are siblings of the repo: `C:\dev\coredcc-<branch>`.
  - Each branch is rebased onto master and merged with `git merge --ff-only`.
- **The remote** is https://github.com/joeedh/coredcc (public, since 2026-11-25). Master is pushed after each
  merge, once the pre-commit gate has passed. Worktree branches stay local.
- **Gates:** `node tools/gate.mjs pre-commit`, by hand, and CI (`.github/workflows/ci.yml`) for everything else,
  since 2026-11-25 (`docs/building.md`). After a push, its CI run is watched, and a failure is fixed first.

### Build cache for the parallel phase
- Done 2026-11-24: `docs/plans/2026-11-23-build-cache-worktrees.md`. How to use it:
  `docs/debugging.md`, "Build cache and worktrees".
- The user's sccache fork (`C:\dev\sccache`) does the caching. Never run it without
  `SCCACHE_SERVER_PIPE`.

## Current focus
- litestl #4-#6, #8-#11 and #14 merged; coredcc pins litestl master `e36c8fd` (2026-11-25).
- P1 on master:
  - P1a (props) and P1b (parsers) are done and reviewed. The review's findings are
    fixed or tracked in `docs/reports/feature-report-props-parse-2026-11-21.md`.
  - P1c (lib) and P1d (api: resolver, canonical paths, ChangeBus) are done and
    reviewed together: `docs/reports/feature-report-lib-api-2026-11-21.md`.
  - P1e (command: executor, LinearHistory, undoable sets, LastUsed, block commands) is
    done and reviewed: `docs/reports/feature-report-command-2026-11-21.md`.
  - P1f steps 1-2 (Emscripten toolchain, wasm-st/wasm-mt presets, the suite under
    node) are done.
  - P1f step 3, C2 (litestl foreign dispatch), is done: litestl #9, plus #10 (N-API
    keeps an already-pending exception).
  - P1f step 4, `bind/` (facades, script commands, the manager), is done and
    reviewed: `docs/reports/feature-report-bind-2026-11-22.md`.
  - P1f step 5, the TS spike, is done and reviewed:
    `docs/reports/feature-report-ts-spike-2026-11-22.md`. It is the wasm-st ctest
    `spike_ts` (`npm ci` in `tests/wasm` first, or it skips).
  - P1f step 6, TA1, is done: the test app loads the wasm-st module
    (`source/coredcc/testapp`) and drives the spike's steps over CDP. It is the wasm-st
    ctest `testapp` (`pnpm install` in `apps/testapp` first, or it skips). Reviewed:
    `docs/reports/feature-report-ta1-2026-11-23.md`.
  - The end of P1 (build cache, worktrees) is done (2026-11-24).
- P2, P3 and P5 are on master and reviewed (2026-11-25). P4 and P5c are next, in worktrees.
- P4 is done in its worktree (2026-11-26): tools, gizmos, element transforms, strokes, the DrawList and its
  WebGPU renderer (TA3), and the Unreal spike (feasible: 11 of 11 checks). Reviewed:
  `docs/reports/feature-report-tools-overlay-2026-11-26.md`; what it deferred is in the plan, "After the review".
- P5's benchmarks are recorded in its plan (step 11). Edits that the journal follows are within the target;
  verifying everything and structural edits are not.
- Open after P3:
  - Its timings on an idle machine. Provisional ones, on a busy machine, are under the targets but for
    unlinking and linking again (63 to 90 ms, target 80).
  - Whether the prefetch of channels (`b8e39eb`) stays: FBX is faster with it, but moving every root may
    cost 1 ms more.
  - wasm-mt-release: `test_base_id` fails, because `tests/support/node_env.js` uses `ENV`, which that
    build leaves out. All BVH and scene tests pass there.
- Test app TA0, TA1 and TA2 are done: build the wasm-st preset, then
  `cd apps/testapp && pnpm install && pnpm test`.
