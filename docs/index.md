# coredcc documentation

## Guides
- [Building and testing](building.md): prerequisites, presets, the gates, the test app, benchmarks.
- [Modules](modules.md): what is where, what depends on what, and where to start reading.
- [Embedding](embedding.md): the rules for a host: files, graphs, layouts, threads, objects, scenes, picking.
- [Debugging](debugging.md): problems that took effort to find, techniques, the build cache.
- [DrawList format](drawlist-format.md): the bytes of what tools and overlays draw, for a host's renderer.

## State
- [Task list](implementation-tasklist.md): the phases, what is done and reviewed, what is open.

## Plans
Each phase has one, pressure-tested before it was implemented, with notes on what was built differently.
- [Architecture](plans/2026-11-19-architecture.md): the design of all of it.
- [P0: scaffold and interfaces](plans/2026-11-19-p0-scaffold-interfaces.md)
- [P1: application spine](plans/2026-11-20-p1-application-spine.md): library, properties, datapaths, commands, bindings.
- [P2: layout and library IO](plans/2026-11-24-p2-layout-library-io.md)
- [P3: objects, transforms and BVH](plans/2026-11-24-p3-objects-bvh.md). "What P4 and P5c rely on" is the
  contract for overrides and drags.
- [P4: tools, overlay rendering and the Unreal spike](plans/2026-11-25-p4-tools.md)
- [P5: graph and instancing](plans/2026-11-24-p5-graph.md). It also has what P5c needs of it.
- [P5c: the scene evaluator](plans/2026-11-25-p5c-scene-evaluator.md): graphs that drive objects, and an
  incremental evaluation.
- [Integration test app](plans/2026-11-19-test-app.md)
- [Build cache and worktrees](plans/2026-11-23-build-cache-worktrees.md)

## Feature reports
A fresh review of each feature's API, and what became of each finding.

| Phase | Reports |
|---|---|
| P0 | [interfaces](reports/feature-report-interfaces-2026-11-20.md) |
| P1 | [properties and parsers](reports/feature-report-props-parse-2026-11-21.md), [library and datapaths](reports/feature-report-lib-api-2026-11-21.md), [commands and undo](reports/feature-report-command-2026-11-21.md), [script bindings](reports/feature-report-bind-2026-11-22.md), [TypeScript spike](reports/feature-report-ts-spike-2026-11-22.md), [test app TA1](reports/feature-report-ta1-2026-11-23.md) |
| P2 | [library IO](reports/feature-report-library-io-2026-11-24.md), [layout](reports/feature-report-layout-2026-11-25.md) |
| P3 | [objects and transforms](reports/feature-report-scene-2026-11-25.md), [BVH, View and picking](reports/feature-report-bvh-2026-11-25.md) |
| P4 | [tools and overlay](reports/feature-report-tools-overlay-2026-11-26.md) |
| P5 | [graph](reports/feature-report-graph-2026-11-25.md) |
| P5c | [scene evaluator](reports/feature-report-scene-evaluator-2026-11-25.md) |

## Measurements and research
- [BVH leaf size](bvh-leafsize.md), written by `tools/bench-bvh.mjs`.
- [Prior art](research/prior-art.md): what the sibling repositories have, as of 2026-11-19.
