# Prior art survey (2026-11-19)

What coredcc can reuse, replace or must avoid in the sibling repos. Line references were
taken on 2026-11-19 and will drift.

## litestl (`C:\dev\litestl`, master `608afca`)

### Binding system (`binding/`)
- A member descriptor is `StructMember {name, offset, type}` (`binding_struct.h:22`). It has
  no range, unit, UI name, description, flags or default. The system has no doc strings.
- Enums are `EnumItem {name, int value}`. There is no UI name, icon or description, and
  values are `int`, so 64-bit flags don't fit.
- There are no getter/setter properties. The generated TS classes read and write the WASM
  heap directly at `ptr + offset` (`typescriptRuntime/bind.ts:201`), so a JS write never
  reaches C++: no change notification, undo or graph tagging.
- What's missing:
  - Setting a nested struct member throws.
  - Fixed C arrays need hand-built `types::Array` descriptors.
  - `std::span` is not bindable.
  - There's no callable type, so JS functions can't be passed in.
  - There's no polymorphic downcast.
- Known bugs:
  - `Struct::inherit` duplicates members (a stray `;`).
  - Every call that returns a non-struct value leaks its TS return buffer.
  - By-value struct arguments are never destructed.
  - The runtime assumes wasm32.
  - Bound classes are built with `eval`, so a browser host needs CSP `unsafe-eval`.
- Struct descriptors are stable singletons. Primitive and `Vec` descriptors are re-`new`ed
  on each `Bind<>()`, so they can't be used as map keys.
- The docs call the system scaffolding for a future constexpr redesign
  (`documentation/binding.md:393`).
- Ways to add metadata:
  - A side table keyed by `(struct descriptor, member name)`. This needs no litestl change.
  - Extending `StructMember`. This needs an ABI table update, an `LSTL_AbiVersion` bump and
    changes to the TS and N-API readers.
- litestl has no N-API runtime. sculptcore's `source/napi/` walks the same descriptors.
  ubertopo DEC-005 plans to move it into `litestl/binding/napi/`.

### Utilities
- Containers: `Vector`, `Map`, `Set`, `OrderedSet`, `BinaryHeap`, `BoolVector`,
  `IndexRange`, `util::Pool<T>` (a typed slab with stable pointers).
- Strings and callables: `util::string` (SSO), `StrLiteral` (usable as a template
  argument), `function` / `function_ref`.
- Errors and flags: `ValueOrErrors`, and the `MAKE_FLAGS_CLASS` macros.
- Hashing and randomness: `Hash128` (SipHash, domain tags) and Philox `rng::Stream`.
- `util/task.h` provides `parallel_for`, `parallel_reduce` and `parallel_exclusive_scan`.
  - It has no task graph or futures.
  - Calling `parallel_for` from inside a worker can starve the pool.
  - WASM needs pthreads (COOP/COEP) and a pre-warmed pool.
- Memory:
  - `alloc::` adds a debug header to each block under a mutex, so it is slow for many
    small nodes. Use `Pool`.
  - `shared_ptr` is not thread-safe. `RcBuffer<T>` has an atomic copy-on-write refcount.
  - There is no intrusive refcount, no generational handle and no signal type.
    `CallbackList` is minimal.

### Math
- Types:
  - `Vec<T,N>` with aliases `float3`, `double3` and so on.
  - `Matrix<F,N>` is Eigen-backed and column-major, with aliases `mat4f` / `mat4d`.
  - `Quat<F>`. **It doesn't compile and is never included:** it uses `var`, an undefined
    `T` and `M_SQRT2`, and has no `#pragma once`. This was found by the pressure test.
  - `AABB<V>`. Its default constructor sets `min` to the negative limit (a bug), so call
    `reset()`.
- Missing:
  - translate, rotate, scale, perspective, lookAt and decompose helpers;
  - quaternion slerp and axis-angle.
- `geom.h` has ray–AABB, ray–triangle, AABB–sphere, closest point on a segment or triangle,
  cone and frustum tests, and triangle–AABB overlap.

### IO and attributes
- `io/bytes.h` (ByteWriter/ByteReader) and `io/compress.h` (lz4 with a fast level for
  undo) are the right primitives for undo snapshots.
- `io/binfile.h` is in the `sculptcore::io` namespace and is unfinished. `serial.*` are
  stubs.
- `attr/` is a paged copy-on-write column store. Copying a group is a cheap snapshot.

### Build
- Targets are unprefixed (`util`, `math`, `io`, `platform`, `binding`, ...). coredcc
  targets need a prefix.
- The checkout directory must be named `litestl`. Turn `LITESTL_BUILD_TESTS` off when
  building it as a dependency.
- `build_wasm_post` hardcodes sculptcore flags (`--use-port=emdawnwebgpu`). coredcc needs
  its own link macro.
- `platform/export.h` is empty: there are no DLL export macros yet.

## path.ux (`C:\dev\path.ux`, TypeScript)

### Worth keeping
- **One property-metadata type serving two roles:** tool input and datapath member. This
  is what makes `prop(path)`, `toolPanel()` and the last-tool panel work without extra
  code.
- **The metadata set:**
  - uiname and description;
  - hard range, soft UI range, step and precision;
  - base and display units;
  - per-item name, description and icon for enums and flags;
  - the read-only, private, no-undo and save-last-value flags.
- **Datapath members:** get and set callbacks that receive the owner and the context, plus
  per-read metadata overrides (a dynamic enum or range).
- **Path grammar:**
  - `a.b.c`
  - `list[3]`, `list['key']`, `list.active`, `list.length`
  - `vec[2]`
  - enum and flag bits: `enum[KEY]`, `flag[KEY]`
  - Resolve errors include "did you mean" hints.
- **Tool definitions:** typed inputs and outputs, `canRun` with a refusal reason, op-owned
  `undoPre`/`undo`, and a linear stack with redo truncation.
- **Folding for drags.** `foldKey` / `foldFrom` merge a drag's repeated changes into one
  undo entry.
- **A generic "set value at path" op** that records old values per path.
- **Last-used input values** keyed by tool path and exposed as datapaths. Tools can be
  invoked from a string: `path(k=v ...)`.
- **Change notification** at the `setValue` choke point, coalesced per frame and matched
  by path prefix.
- **`xmlpage`, a declarative UI format** (`scripts/xmlpage/xmlpage.ts`). Its tags are
  `tabs`, `tab`, `panel`, `row`, `column`, `strip`, `prop path`, `tool path` and
  `toolPanel`, and a `path` attribute on a container sets a path prefix for its children.

### To avoid
- Per-call scope stored on a shared prop instance.
- Type ids as one bit per type, with collisions.
- Flags that mix meaning, presentation and internals. path.ux's own unlanded replacement
  is `documentation/plans/property-categories.md`.
- Numeric fields on the base class.
- Two serialization paths.
- An async tool lifecycle guarded by a promise lock.
- Modal tool and event handler fused into one class. `plans/tool-handlers.md` proposes
  splitting them.
- Dead default-undo handlers: the base `undoPre`/`undo` throw.
- A roughly 400-line special-case path resolver.

## faberforge (`C:\dev\webgl-app-framework`, TypeScript)

### Graph
- One generic graph (`scripts/core/graph.ts`) with typed sockets and push-based dirty
  propagation that copies values along edges.
- Evaluation and cycles:
  - Order comes from a recursive depth-first topological sort.
  - Cycles are rejected unless allowed; an allowed cycle runs a fixed-point loop of at
    most 64 iterations.
  - Runs are coalesced through `setTimeout` on the main thread.
- **Node groups and subgraph instancing are stubs** (`scripts/graph/node_group.js`).
- The scene's use of the graph is shallow: object `depend` sockets and a parent `matrix`
  input.

### Objects
- An object's matrix is built from Euler rotation in a chosen order, then scale, then
  translation, then multiplied by the parent's matrix. There is no pivot.
- `getBoundingBox` transforms only two corners, so the world box is wrong under rotation.
- `documentation/geometry-contract.md` is effectively the object-capability spec:
  - **Required:** world bounds, `applyMatrix`, draw entries, and four picking queries (ray,
    nearest, screen circle, screen rect).
  - **Optional:** element handles with `topoStamp`, `invalidate(kinds)`,
    `closestElements`, attributes, triangles, symmetry.

### Tools
- A persistent `ToolMode` receives events and draws overlays.
- A gizmo tree (`widgets.ts`) does screen-space picking and keeps gizmos a constant size
  on screen.
- Gizmos start modal ToolOps.
- Transform runs over a registry of `ITransDataType` implementations.

### Rendering
- Rendering is WebGPU-only.
- What overlays need:
  - lines with depth testing on or off;
  - points and triangle shapes;
  - 2D screen-space lines and circles;
  - camera `project`, `unproject` and `viewVec`.
- sculptcore provides a backend-neutral `gpu::DrawBatch`.

### Undo
- The default is a full-file snapshot whose reload is async, so it races with `rerun()`.
- Large state stays in C++: tools keep only a sculptcore `MeshLog` step id and report its
  size.
- There is a memory budget, 512 MB by default.

### UI
- UI is built imperatively with `container.prop/tool`. There are no layout files.

## sculptcore (`C:\dev\sculptcore`)

- `source/props` is a runtime property system: name and ui_name, flags, getter/setter
  `std::function`, min/max/step, and pen dynamics. Almost none of it crosses to TS.
- `source/spatial` is a BVH tied to its own `Mesh`:
  - It splits on the longest axis, with no SAH.
  - The leaf limit is 512 vertices.
  - It updates incrementally under dyntopo.
  - Queries are ray, sphere, cone and frustum.
- Undo is domain-specific: `MeshLog`, the multires stroke log, and VDM tile deltas.
- There is no generic command or undo system in C++.
- `source/gpu` describes a frame backend-neutrally. The host executes the batches.
- Build:
  - The litestl submodule is at `source/litestl`.
  - `make.mjs` drives the wasm, native and N-API (NW.js) builds.
  - The WASM flags are `-pthread -sSHARED_MEMORY=1 -sIMPORTED_MEMORY=1`.
- A C++ stroke driver exists in `source/brush/stroke_driver.h`. It produces spaced samples
  with pressure and tilt.

## ubertopo (`C:\dev\ubertopo`)

### Decisions
- coredcc must stay consistent with these:
  - C++20 on litestl, and concepts.
  - No third-party libraries in the core.
  - A native N-API addon rather than WASM, using raw C `node_api.h`.
  - Python is deferred.
- The root CMakeLists sets `-ffp-contract=off -fno-fast-math` globally for determinism.
  coredcc must not inject conflicting FP flags.
- It wants its own BVH: binned SAH, an f64 option, a two-level tree, refit versus rebuild,
  and batch queries.

### Undo
- `Editor<C>` savepoint transactions (DEC-016/017).
- Undo restores physical row pre-images; redo re-executes recorded operations.
- coredcc commands must delegate to that journal, not snapshot.

### Procedural research
- It asks for a demand-driven, incremental evaluation IR.
- Requirements: stable IDs, content-addressed caching, override layers and provenance
  (`docs/research/procedural-systems/critique-and-recommendations.md`).
- No existing ubertopo doc mentions coredcc.

### litestl versions
- ubertopo pins branch `ubertopo/p4-support`.
- sculptcore pins an older commit with no `attr/`.
- coredcc should pin `608afca` or later.
