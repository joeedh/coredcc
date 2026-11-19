# coredcc architecture plan

- Status: revised after a pressure test (see "Pressure test record" at the end).
- Date: 2026-11-19.
- Prior-art findings that this plan relies on: `docs/research/prior-art.md`.

## 1. Goal and scope

coredcc is a C++20 foundation library for 3D DCC tools, built on litestl. It has to work
in four settings:
- As WASM inside a browser or WebGPU frontend. faberforge is the first such host.
- As a native N-API addon inside NW.js or Chromium. ubertopo works this way, and faberforge
  can too.
- Statically linked into a native editor. Unreal's editor is the main target, with Maya
  possible. Blender is explicitly not a target.
- Later, as the core of faberforge and as the interactive-test harness for ubertopo.

**In scope for v1:**
- Core DCC:
  - a dependency execution graph with subgraph instancing;
  - a minimal 3D object interface whose transform model is pluggable;
  - a BVH over a client-supplied entity interface, with coarse built-in sources for
    triangles, lines with radii and points with radii.
- Application infrastructure:
  - property metadata;
  - undoable commands, with pluggable undo histories so that host editors can own undo;
  - a datapath layer in the style of Blender's RNA;
  - a UI layout description;
  - an overlay rendering interface;
  - an interactive tool and gizmo system;
  - binding bridges, including host-language implementations of coredcc interfaces.

**Out of scope for v1:** see the deferred list in §14.

## 2. Hard constraints

- **litestl conventions.**
  - Use its containers, `alloc`, `hash` and `path`.
  - Files are `.h`/`.cc` and tests use its harness.
  - litestl is ours to change. General-purpose fixes go into litestl on a feature branch
    (see §14, P0b), not into coredcc.
- **No exceptions and no RTTI.** Unreal builds with both off by default.
  - litestl already compiles this way; the pressure test checked this with MSVC and
    clang-cl.
  - Errors use a coredcc `Status`, which carries a code, a message and optional
    suggestions. `util::ValueOrErrors` is not used, because its error names are
    compile-time literals.
  - Downcasts use coredcc type ids. A type id is a hash of a stable type name, never an
    address, because addresses differ across DLLs.
- **No third-party libraries** beyond what litestl vendors (Eigen, lz4). This matches
  ubertopo DEC-002.
- **Don't set global FP flags.** coredcc must neither set FP flags nor depend on
  fast-math.
  - ubertopo sets `-ffp-contract=off -fno-fast-math`.
  - Header-only templates are compiled with the host's flags anyway.
- **Embeddable headers.** coredcc's public headers must compile in an Unreal module. That
  means:
  - with UE's `check`, `verify`, `ensure` and `TEXT` macros defined;
  - with `<windows.h>` included without `NOMINMAX`;
  - with shadowing warnings treated as errors.

  litestl currently fails all three, so the fixes are a P0b work item. A compile test
  (`tests/compile_embed/`) enforces this from then on.
- **No coredcc-owned global mutable state.**
  - Every registry (ApiStructs, commands, socket types, tools) belongs to a `Runtime`
    instance.
  - Built-in types are registered into a Runtime by `registerBuiltins(Runtime&)`. They
    are not process-global statics.
  - Objects and models return a `StructId`, which the Runtime resolves; they do not
    return `ApiStruct*`.
  - litestl's own process globals are accepted, with these rules:
    - the task pool, alloc tracking and string interning are listed in
      `docs/embedding.md`;
    - in Unreal, coredcc is exactly one module, so these globals exist once;
    - string interning is never called from worker threads.
- **Threading.**
  - Public API calls are single-threaded, on the host's main thread.
  - BVH build and graph evaluation may use `task::parallel_for`. coredcc never calls
    `task::run` and never uses `ScopedBandCount` above 1.
  - With one worker, `parallel_for` runs serially. That is the only mode that works in a
    WASM build without pthreads.
- **Two WASM build flavours.**
  - `wasm-mt`: pthreads with shared memory. It needs COOP/COEP.
  - `wasm-st`: no pthreads. A `-pthread` binary cannot even instantiate without
    COOP/COEP, so this cannot be a runtime switch.
- **Precision.**
  - Object transforms, world bounds and the scene-level BVH use `double`, for CAD and
    large worlds.
  - Per-object geometry BVHs are templated on the scalar type.
  - Overlay geometry is `float` relative to a `double` origin (§10).

## 2a. Interfaces (`base/iface`)

Every contract a client implements uses one interface system. That covers object
capabilities, `TransformModel`, `BlockProvider`, `SnapshotProvider`, `AnyBvhSource`,
`Command`, `Tool` and `Gizmo`. coredcc's internal polymorphism may still use ordinary
virtual classes.

**History of this section:**
- **Draft 1** was hand-rolled fat pointers declared with an X-macro. It was
  pressure-tested and then revised.
- **Draft 2** (at the user's suggestion) builds interfaces on litestl's binding system.
  A litestl method descriptor already carries a name, parameter names and types, a
  return type, a const flag, and a `MethodThunk(void *self, void **args, void *ret)`.
  That is a vtable slot in all but name, and it is the ABI that foreign implementations
  use anyway.
- Draft 2 was pressure-tested by building a working prototype with clang-cl, MSVC and
  emcc. It was then revised into the text below.
- Both records are at the end of the plan.

### Why not multiple inheritance
- An adaptor would need a wrapper object per entity, and you can't add an interface to a
  type you don't own.
- Pointer adjustment breaks litestl's `static_cast<T*>(void*)` thunks.
- With RTTI off, we'd still need a query layer.
- Foreign implementations would need a shim class for every combination of interfaces.

### Declaring an interface
An interface is a bound struct whose descriptor is marked `isInterface`. It has no fields
and no constructors except "copy". Its bound methods are the slot list. Forwarders are
written by hand.

```cpp
struct Spatial : IfaceRef {   // IfaceRef = {const IfaceTable *table; void *self;}
  bool rayCast(const Ray &ray, Hit &hit) const { return CDCC_CALL(rayCast, ray, hit); }
  int count() const { return CDCC_CALL(count); }
  double cost() const { return CDCC_HAS(cost) ? CDCC_CALL(cost) : count() * 2.0; }

  CDCC_INTERFACE(Spatial, "coredcc.Spatial") {   // body of a cached defineBindings()
    BIND_STRUCT_METHOD(st, rayCast, MARGS("ray", "hit"));
    BIND_STRUCT_METHOD(st, count, MARGS());
    BIND_OPTIONAL_METHOD(st, cost, MARGS());
  }
};
```

- **`CDCC_CALL(name, args...)` is mandatory.** It takes the return type and parameter
  types from the forwarder's own member pointer (`Fwd<&Self::name>`), which is the same
  pointer the descriptor was built from, so the two cannot diverge.
  - A misspelled name is a compile error.
  - The draft's `invoke<R>(slot, args...)` took its types from the call expression and
    checked nothing. In the prototype, passing an `int` to an int64 slot returned
    garbage, and a wrong `R` overflowed the return buffer.
- **Slot index.** It is resolved by name on first call and cached in a function-local
  static.
  - That cache is immutable once set. It is a documented exception to the "no global
    mutable state" rule, because binding descriptors are process-wide anyway.
  - Its guard costs about 0.4 ns.
- **`CDCC_INTERFACE` caches its descriptor before binding any methods.** Self-referencing
  interfaces need this: `TransformModel::world(..., TransformModel parent)` and
  `Command::foldFrom(Command)` overflowed the stack without the cache.
- **Optional slots.** A null thunk falls back to the default in the forwarder.

**Declaration rules:**
- No overloads, because the TS runtime keeps only the last method of each name.
- No template methods.
- No function-pointer or `function_ref` parameters.
- No `Span` parameters (there is no binder for it). Use `Vector&` or a `Sink` interface.
- No reference returns, which litestl cannot bind.
- No primitive out-parameters, because JS receives a primitive pointer as a plain number.
  Use a bound struct.
- A class that implements several interfaces shouldn't need the same slot name with
  different signatures. The escape hatch is an adaptor.

### Implementing an interface
- **A bound class** implements an interface when its own bindings have methods with
  matching names and signatures. Extra methods are fine.
  - Registration: `registerImpl(runtime, Spatial, MeshImpl, "test.MeshImpl") -> Status`.
  - Type names are always explicit, because compiler-derived names differ between clang
    and MSVC.
  - A template implementation, such as a `Bvh<S>` source, needs `->template` in the
    binding macros; this is a P0b fix.
- **Matching happens at registration, by method name.**
  - Types are compared by full name *and* size *and* a per-C++-type key (`&typeKey<T>`)
    stored on the `Struct`. Full names alone failed in the prototype: a type bound under
    another type's name was accepted, and corrupted memory.
  - Descriptors record const-ness (P0b fix), so `const Ray&` and `Ray&` no longer match.
    In the prototype const was erased, and an implementation wrote through a caller's
    `const` object.
  - Return types must match exactly. A const slot needs a const method.
  - A mismatch fails with a Status naming the slot and the candidate, for example
    `slot count [int32 count() const]: candidate int64 count(): return type int64 != int32`.
  - Each implementation's descriptor is built once per Runtime and cached.
- **The resulting `IfaceTable`** holds the interface id, `implType`, flags (including
  `threadAffine`), and one thunk per slot.
- **Adaptors** over types we don't own use `BIND_ADAPTOR_METHOD(def, name, &fn, MARGS(...))`
  with `fn` of the form `R fn(Adapted&, A...)`.
  - No adaptor object is allocated: `self` is the adapted object.
  - It is a small litestl extension: `std::invoke` in `MethodBuilder`, an 8-line
    free-function `MethodTraits` specialization, and the macro.
  - Adaptor descriptors are never registered with `BindingManager`, because two adaptors
    of one type share its name.

### Losing compile-time conformance
- Forwarder-to-slot typing *is* checked at compile time, by `CDCC_CALL`.
- Implementation-to-interface conformance is checked at registration.
- `Runtime::validateAll()` runs at startup in debug builds and in every test binary.
  - It took about 2.5 ms for 300 interfaces × 8 slots.
  - Registering all of them, including building the descriptors, took about 46 ms.
- If litestl's planned constexpr redesign of the binding system lands, conformance can
  move to compile time without any change to declarations.

### Inheritance
- **Single base:** `interfaceStruct<Derived>(name, Base)` puts the base's slots first,
  so slot indices coincide.
- **Other bases** are reached by explicit upcast through their own per-base `IfaceTable`,
  which registration builds.
- Concatenating several bases is forbidden. In the prototype it ran the wrong slot,
  because each base's forwarder indices are relative to that base.

### Query and downcast
- The registry maps `(TypeId, InterfaceId) -> IfaceTable` and lives in the Runtime.
- `query<I>(BlockHandle)` checks, in order:
  1. the per-block override table, skipped unless the block's `hasOverrides` bit is set;
  2. a per-BlockType cache;
  3. the provider's `queryInterface` hook.
- `as<T>()` checks `implType`.

### Bindings and foreign implementations
- **Calling from TS or Python.** Interface structs are bound like any struct.
  - `query<I>` is bound as `queryRaw(block, InterfaceId)`, which returns an `IfaceRef`,
    plus a checked constructor.
  - `Owned<I>` is never bound.
- **Implementing in TS.** The generator emits interface structs as implementable TS
  interfaces:
  - no `[Symbol.dispose]` or `new` members;
  - `name?(...)` for optional slots;
  - `Readonly<>` for const references;
  - `bigint` for int64.

  The TypeScript compiler then checks `class JsMesh implements Spatial`.
- **Foreign tables.** `rt.implement(block or typeName, "coredcc.Spatial", jsObject)`
  builds a foreign `IfaceTable` per JS class. Slots for missing optional methods are null.
- **The dispatcher.** There is one per host: `EM_JS` under WASM, and `napi_call_function`
  under N-API.
  - It marshals arguments using the slot's parameter descriptors. Each slot's marshal
    closure is precompiled when the table is built.
  - **Return ownership.** The dispatcher copy-constructs the JS result into `ret`, then
    disposes any temporary the JS side owned. In the prototype, a struct created in JS
    and returned leaked.
  - It catches every JS exception and reports failure.
  - Slots that can return `Status` do. The rest set a sticky Runtime error, which the
    executor checks after each call.
- **Cost (prototype, WASM).** 85 ns for a scalar slot, and 2.3 µs for a slot with two
  struct arguments. The 2.3 µs comes from the naive marshalling; precompiling the
  closures is aimed at that.
- **Threads.** Foreign tables are always `threadAffine`. Parallel algorithms check the
  flag, and either run serially or gather the foreign data on the main thread first.

### Lifetimes, cost and build time
- **Lifetimes:**
  - An interface value is transient. Stored references hold a `BlockId` plus an
    `InterfaceId`.
  - A foreign slot that returns an interface may return only a `BlockHandle`, or an
    interface of an object held by the override table.
  - TS wrappers around arguments point into the C++ stack, so they are invalidated after
    the call returns.
  - Debug builds check the `BlockHandle` generation on each call.
- **Native call cost** (clang-cl /O2; MSVC figures in brackets):

  | Call | Direct virtual | Draft 2 interface |
  |---|---|---|
  | `rayCast` | 0.96 ns [1.78] | 1.74 ns [2.56] |
  | `count` | 0.84 ns [1.50] | 1.24 ns [2.11] |
  | struct return | — | 3.0 ns [4.0] |

  That is irrelevant at object granularity. Hot inner loops use the templated path
  (`Bvh<S>`).
- **Build time.** Bound methods compile slowly: about 40–80 ms per method at /O2, against
  4 s for 1000 plain virtual methods.
  - Binding code therefore lives in per-module `bindings.cc` translation units, following
    sculptcore's pattern. It is never in headers.
  - P0 measures the real cost on the first modules. This cost already applies to
    everything bound for TS.
- **Debugging.** At /Od there are five frames between a forwarder and its implementation.
  A `.natstepfilter` steps over them. Release builds show just the thunk.

### litestl work this needs (P0b)
- **New descriptor fields:** `isInterface` on `Struct` and `isOptional` on `Method`, each
  mirrored in the `BindingInfo` offsets and the TS classes.
  - `findMethod(name)`.
  - A per-C++-type key on `Struct`.
- **Const-recording descriptors:** `Binder<const T&>` and `Binder<const T*>` record
  const-ness in the full name. noexcept `MethodTraits`.
- **Adaptor methods:** `std::invoke` in `MethodBuilder::invokeImpl`, the free-function
  `MethodTraits` specialization, and `BIND_ADAPTOR_METHOD`.
- **Macro fixes:** `->template` in the binding macros. `BIND_OPTIONAL_METHOD` must be
  variadic, because `MARGS` expands to a type containing a comma.
- **TS generator:**
  - implementable interfaces;
  - optional methods;
  - `Readonly<>`;
  - `bigint` for int64.
- **TS runtime:**
  - an Enum case in `getBoundPointer`;
  - reading reference arguments through `ptrType`;
  - a new return writer of about 40 lines. The existing `setBoundPointer` keys on C names
    and destructs uninitialized storage.
  - the dispatcher hook.
- **N-API:** the matching reverse-direction marshalling. This is unverified.
- **Smaller fixes:**
  - make `PermanentGuard` thread-safe, or per-thread;
  - use `.c_str()` in the `std::format` calls, needed for emsdk 3.1.53.

## 3. Repository layout

```
source/
  litestl/              git submodule, pinned at master 8a98264 or later, or a P0b PR branch
  coredcc/
    base/               Status, type ids, generational handles + HandlePool, Signal, Runtime,
                        iface/ (interface definitions, registry, Iface<I>, Owned<I>)
    props/              PropertyDef, PropValue, units, enum items, PropertyBag
    api/                datapath: ApiStruct, ApiMember, Path parser, resolver, ChangeBus
    command/            CommandDef, Command, executor, UndoHistory, LinearHistory,
                        registry, defaults, invoke parser
    graph/              socket types, NodeType, GraphDef, GraphInstance, evaluator, serialization
    scene/              Object, TransformModel, capabilities, Scene, scene evaluator
    bvh/                Bvh<Source>, built-in sources, SceneBvh
    draw/               View (camera math), DrawList (overlay command buffer)
    tool/               InputEvent, Tool, Modal, ToolManager, gizmos, StrokeSampler
    layout/             LayoutNode, XML-subset parser, validator, JSON writer
    bind/               litestl binding registration, foreign-implementation shims
  hosts/headless/       test host: fake View, DrawList recorder, scripted events
tests/                  native tests (litestl harness), benchmarks, compile_embed/
```

- The include root is `source/`.
- CMake targets are prefixed `coredcc_`. coredcc adds litestl only
  `if(NOT TARGET util)`, so a parent project such as ubertopo, which has its own litestl
  submodule, can supply litestl instead. The two pins must be compatible. The parent's
  pin wins, and this is documented.
- Build: CMake plus presets for native builds. Emscripten presets for `wasm-st` and
  `wasm-mt` come in P1. A `make.mjs` dispatcher is added only when the N-API build needs
  one (P6).

## 4. Properties (`props/`)

Design aim: one metadata type serves both roles, command input and datapath member.

### PropertyDef
`PropertyDef` is a plain value type holding metadata only, never a value:
- **Identity:** `apiName`, `uiName` (derived from `apiName` when empty) and `description`.
- **`kind`:** Bool, Int, Float, String, Enum, Flags, Vec2, Vec3, Vec4, Mat4, Quat,
  Pointer and Collection.
  - Kinds are a closed enum, with capability queries such as `isNumeric(kind)`. There is
    no bit-per-type scheme.
- **`subtype`:** a presentation and unit hint, for example Color, ColorGamma, Angle,
  Distance, Factor, Percentage, Time, Direction, Euler, Xyz, FilePath or Password.
- **`access` flags:** ReadOnly, Private, NoUndo, Optional, SaveLastValue, Animatable.
- **`ui` hints:** iconsOnly, expand, slider style, multiline. These are kept apart from
  `access`.
- **Kind-specific metadata,** in a `std::variant`. This is internal to C++; bindings
  expose it through accessors.
  - `NumericMeta`: hard and soft ranges, step, precision, base and display units.
  - `EnumMeta`: a list of `EnumItem {key, value: int64, uiName, description, icon}`.
  - `StringMeta`: max length.
  - `VectorMeta`: `NumericMeta` plus per-component names.
- **`defaultValue`,** a `PropValue`.
- **`dynamic`,** an optional callback `(PointerRef, Context&) -> patch`. It covers enum
  items or ranges that depend on the object at read time.

### Values and bags
- `PropValue` is an opaque struct with a kind tag, typed accessors and factory functions.
  It is not a `std::variant`, because litestl can't bind one.
  - The kinds are bool, int64, double, string, `double2/3/4`, `mat4d`, quat, enum,
    flags (uint64), Pointer and None.
  - A Pointer value stores a `StableRef` (§5), never a raw address.
- `PropertyBag` is an ordered list of `(PropertyDef, PropValue, wasSet)`.
- Units: a table of unit families, each with base and display conversion, and text
  formatting and parsing.
- coredcc's props do not depend on sculptcore's `source/props`; see §15.

## 5. Datapath layer (`api/`)

This is an RNA-style layer: the data model exposed to users does not have to match the
implementation.

### References
- **`PointerRef {void *data; StructId type; OwnerId owner;}`** is a transient C++-only
  reference, valid only during one call. It is never stored and never bound.
- **`StableRef {OwnerKey owner; StructId type; Key key;}`** is the storable form.
  - `Key` is a path of stable keys from the owner.
  - It is resolved again on every use and fails cleanly when the target is gone.
  - It is what undo records and the host language hold.
  - Block references stored *inside* a block are library refs (§8a), not `StableRef`s.
- **`OwnerKey`** is one of two kinds:
  - a `BlockId` (§8a) with a cached `BlockHandle`. Identity survives undo, redo, snapshot
    restore and save, because it never depends on the handle;
  - a **runtime owner**: tool settings, preferences, LastUsedStore, `Context::root()`
    and UI state. Runtime owners are registered by a stable name. They are excluded from
    GC and from library files, but can still own undoable and notifiable data.

### ApiStruct
`ApiStruct` has an id, uiName, description, an optional parent struct, and an ordered
list of `ApiMember`s.
- **`ApiMember`** is a `PropertyDef`, an accessor, and an optional
  `update(PointerRef, Context&)` hook.
- **Accessor kinds:**
  - `memberOffset<&T::field>()`;
  - `getter/setter` lambdas taking `(PointerRef, Context&)`;
  - `pointer`: a nested struct, either static or resolved per instance by
    `refine(PointerRef) -> StructId`.
    - `refine` must declare its candidate structs so that static validation (§12) can
      check paths through it.
  - `collection`: callbacks for `length`, `at(i)`, `lookup(key)`, `keyOf(item)`,
    `active`, `setActive` and `itemStruct(item)`.
    - `keyOf` returns a stable key (a handle or a name). It is mandatory, because undo
      depends on it.
- **Fluent builder:** `st.addFloat("radius", &Brush::radius).ui(...).range(...).soft(...).subtype(...).update(cb)`.
- **`fromBinding<T>()`** builds members from a litestl struct descriptor. It does not use
  litestl's broken `Struct::inherit`.

### Context and root
- `Context` holds the Runtime, the active UndoHistory and executor, the ToolManager, the
  scene, and an optional **path prefix override**. The prefix lets a panel run commands
  against its own data path, like Blender's context overrides.
- `Context::root()` is a host-defined struct.

### Paths
- **Grammar:**
  - `a.b.c`
  - `list[3]`, `list["key"]`, `list[#handle]` (a stable key), `list.active`,
    `list.length`
  - `vec[2]`
  - `enum[KEY]` / `flags[KEY]`
- A path is parsed once and cached. Errors carry the failing segment and edit-distance
  suggestions.
- `ResolvedPath` is C++-only and transient.
- **Canonicalisation:** `canonicalize(ctx, path)` rewrites indirect segments (`.active`,
  numeric indices, hops through block refs) into a `StableRef` rooted at the
  **innermost block** that owns the member.
  - For example, `objects.active.data.radius` becomes `{owner: meshBlockId, key: radius}`.
    It does not keep the hop through `data`, which could be reassigned outside the
    history.
  - Undo records always store canonical paths.
  - Without this, undo writes into whatever object is active at undo time.
- `getValue` / `setValue` / `getMeta`. `setValue` is the single write path:
  1. Reject the write if the member is read-only.
  2. Clamp to the hard range.
  3. Call the accessor.
  4. Call the `update` hook.
  5. Post to the ChangeBus.
- **Batched access:** `getValues(paths[])` returns many values in one call. Bindings use
  it so a UI doesn't pay one boundary crossing per widget per frame.

### ChangeBus
- **Changes** are `(owner, member, index)` plus a **structural epoch**. The epoch is
  bumped by changes to active, insert, remove or reorder in any collection.
- **Watchers** hold a `Path`. At `flush()`:
  1. Each watcher's path is resolved again.
  2. The watcher fires if its resolved chain touches a changed `(owner, member)`, or if
     the epoch moved.
  3. A per-watcher value diff suppresses spurious calls.

  Resolving again handles aliases: `objects["A"].x` and `objects.active.x` both see the
  change.
- C++ code that mutates data directly calls `notify(owner[, member])`.

### Undoable set
`SetPropertyCommand` (§6):
- Its inputs are a canonical path and a value. It stores the old value.
- Its fold key is `canonicalPath + gestureId`.
- Widgets call `setValueUndo(ctx, path, value, gestureId)`.

## 6. Commands and undo (`command/`)

### CommandDef and Command
- `CommandDef` holds `id` (`"mesh.extrude"`), uiName, description, icon, flags (Private,
  NoUndo, UndoBarrier), input and output `PropertyDef`s, and a factory.
- `Command` is a §2a interface. The executor owns each instance through `Owned<Command>`
  and keeps its `inputs`/`outputs` bags. The slots are:
  - `canRun(Context&) -> Refusal` (with a reason);
  - `exec(Context&) -> Status`;
  - `undo(Context&)`;
  - `redo(Context&)` (optional);
  - `memSize()`;
  - `discard()`;
  - `foldKey()` / `foldFrom(Command)`. `foldFrom` downcasts with `as<T>()`.
- **Redo:**
  - When the optional `redo` slot is null, the forwarder runs `exec` again. That is only
    correct for deterministic, input-driven commands.
  - An interactive command must fill the `redo` slot, either by replaying recorded samples
    or through a journal. Registration fails with a Status if the slot is null.
- **Sub-commands:** `ctx.runSub(cmd)` inside `exec` runs `cmd` and appends it to an
  implicit group owned by the current command.
  - The group is undone in reverse before the parent's own `undo`.
  - Only top-level commands reach the history.
  - This covers "duplicate then move", and update hooks that need structural edits.

### Undo strategies
These are helpers a command composes:
- **Custom:** the command captures and restores its own state.
- **Delegated:** a domain-journal step handle plus undo, redo and free callbacks. This
  covers sculptcore MeshLog and ubertopo `Editor<C>` savepoints (physical undo, redo by
  re-execution).
- **Snapshot:** a host-registered `SnapshotProvider {capture() -> Blob; restore(Blob)}`.
  It is synchronous, with lz4.

### Executor and pluggable history
The pressure test showed that native hosts own undo: Unreal uses
`GEditor->Trans`/`FChange`, and Maya uses `MPxCommand`. So execution and history are
separate.
- **`CommandExecutor`**:
  - checks `canRun`;
  - runs `exec` with sub-command grouping;
  - writes LastUsed values;
  - hands the finished command to an `UndoHistory`.
- **`UndoHistory`** is an interface:
  - `push(cmd)`;
  - `tryFold(cmd)`;
  - `beginInteractive` / `commit` / `cancel`;
  - `canUndo` / `undo` / `redo`.
- **`LinearHistory`** is the built-in implementation, used for web and headless hosts:
  - a cursor;
  - redo truncation that calls `discard()`;
  - a memory budget that keeps the newest N;
  - barriers;
  - `rerunHead(newInputs)` for last-command panels.
  - Folding happens only when the head is at the cursor and nothing has been undone since
    the head was pushed.
- **Host adapters** (P6+):
  - **`UnrealHistory`:** wraps each command in an `FCommandChange` inside a
    `FScopedTransaction`. An interactive command maps to a transaction held open for the
    whole drag, and folding maps onto that open transaction.
  - **`MayaHistory`:** an `MPxCommand` whose `undoIt`/`redoIt` forward to the command.

### Interactive commands
- `beginInteractive(cmd)` captures undo state. The tool then mutates data live.
- `commit()` pushes the command. `cancel()` undoes it and drops it.
- **While an interactive command is open:**
  - Every other `exec` returns `Status::Busy`. This covers panel edits and hotkeys.
  - Undo and redo requests first cancel the open command.
  - This avoids interleaved histories in which undo restores a mid-drag value.

### Registry, defaults and invocation
- `CommandRegistry` belongs to the Runtime. A duplicate id is an error Status.
- `LastUsedStore` maps `commandId -> {input -> PropValue}`.
  - It is exposed as the datapath `commandDefaults["mesh.extrude"].dist`. The key form is
    required because ids contain dots.
  - `serialize()`/`load()` produce bytes. The host decides where to store them.
- **Invocation strings:** `mesh.extrude(dist=1.0 mode='X')`. Arguments may be separated
  by spaces (path.ux's existing syntax) or by commas. Values are parsed against the
  input's `PropertyDef`.

## 7. Dependency graph (`graph/`)

### Socket types and values
- `SocketTypeRegistry` belongs to the Runtime. Each entry has an id and value operations:
  default, copy, equal, hash and serialize.
- Built-ins: bool, int, float, vec2/3/4, mat4, quat, string, enum, and `Payload<T>`.
  - `Payload<T>` is a refcounted, immutable payload for heavy data, shared along edges and
    copied before writing. It was called `Ref<T>` in the first draft, which clashed with
    the library's block references.
  - A payload that aliases a block's data holds a host pin on that block (§8a), so GC
    cannot free memory the graph is still reading.

### Node types and graph definitions
- `NodeType` has an id, socket declarations, parameters (`PropertyDef`s, exposed through
  the datapath), and `exec(NodeExecContext&) -> Status`.
  - `exec` reads its inputs and writes only its outputs. It touches no other shared state.
- `GraphDef`:
  - nodes with stable `NodeId`s, links and an interface;
  - a cached topological order;
  - a list of the GraphInstances that use it, so an edit can bump them all and their
    roots' `updateGeneration`.
  - Linking rejects cycles and recursive group references.
  - `GraphDef` serializes through litestl `io/bytes.h`, with a schema version. This is in
    P5, because ubertopo's procedural graphs need it even though scene files are
    deferred.

### Subgraph instancing
- A `GroupNode` references a def and exposes that def's interface as its sockets.
- A `GraphInstance` holds per-node output caches, a per-node `lastEvalVersion`, and a
  child instance per group node.
- Topology is shared through the def. State is per instance.

### Evaluation and versions
- Evaluation is pull-based: only stale nodes run.
- **Global versions.** Every version stamp (parameter, output, def) comes from one
  monotonic counter in the Runtime.
  - The counter is never stored in snapshots.
  - Restoring a snapshot gives every restored item a fresh version.
  - This prevents serving a stale cache after an undo, when a version number would
    otherwise repeat (the ABA case in the pressure-test record).
- **Tagging.** Datapath `update` hooks on node parameters call `graph.tag(nodeId)`. This
  invalidates that node in every instance of the def.
- **Dynamic instancing (P5b)** uses a foreach node. It keeps a pool of transient
  instances keyed by element id, evicted when an element disappears and capped by an LRU
  limit.

### Original vs evaluated data
The graph never writes user-editable ("original") data through `setValue`. This prevents
two things:
- feedback loops through update hooks;
- user edits and graph outputs overwriting each other.

How it works:
- Each object has an **evaluated state**: world matrix, channel overrides and evaluated
  geometry refs. It is owned by the scene evaluator and is not undoable.
- Output adapter nodes write only to their outputs. The scene evaluator's **apply phase**
  copies those outputs into the evaluated state after graph execution, which keeps the
  "exec touches no shared state" contract.
- `TransformModel::local(EvalCtx&)` reads channel overrides from `EvalCtx`.

## 8. Objects and transforms (`scene/`)

### TransformModel
`TransformModel` is a §2a interface, agnostic about how the matrix is built. Every
method receives the parent context. `parent` is a nullable interface value (null at the root).

- `local(EvalCtx&) -> mat4d`.
- `components(EvalCtx&) -> TransformComponents`. This returns the global rotation and
  scale and the local scale, which children with FBX inherit types need.
  - The default implementation decomposes `local`, so a TRS parent works under an FBX
    child.
- `world(EvalCtx&, const mat4d &parentWorld, TransformModel parent) -> mat4d`.
  - The default is `parentWorld * local`.
  - FBX RrSs and RSrs use the parent's global R and S. Rrs uses the parent's local scale
    via `parent->components(ctx)`.
- `setWorld(EvalCtx&, const mat4d &target, const mat4d &parentWorld, TransformModel parent, SetMode) -> Status`.
  - The model inverts its own inheritance rule. This is not
    `parentWorld⁻¹ · target` in general.
  - It returns an error when the target can't be represented.
- `applyDelta(EvalCtx&, const TransformDelta&, const mat4d &parentWorld, TransformModel parent)`.
  - The delta is a translate, rotate or scale with a space and a pivot.
  - Gizmos use it so accumulated rotation doesn't wrap Euler angles.
  - The default goes through `setWorld`.
- `structId()`: the model's channels are exposed as datapaths.

**Built-in models:**
- `TrsTransform`: Euler with an order, or a quaternion.
- `FbxTransform`: implemented in P3, with RrSs, RSrs and Rrs, to prove the interface is
  agnostic.

**Gizmo rule:** gizmos and transform tools skip descendants of selected objects.

**Math prerequisites (P0b):** litestl's `quat.h` has never compiled, and litestl has no
Euler conversion and no matrix decompose. P0b fixes the quaternion type and adds:
- slerp and axis-angle;
- Euler conversion in all 6 orders;
- TRS decompose, with an option to report shear;
- translate, rotate, scale, perspective and lookAt helpers.

### Object
- `id` (a generational handle), `name`, flags, a parent handle, `transform()`,
  `structId()`.
- `localBounds() -> AABB<double3>`. World bounds transform all 8 corners.
- Capabilities are interfaces (§2a), queried with `query<I>(objectBlock)`. They are
  named after faberforge's `geometry-contract.md` where one exists there:
  - **Invalidation** (v1): `invalidate(Topology|Positions|Attributes|Selection|Materials)`.
    The scene evaluator and SceneBvh use it to choose between refit and rebuild.
  - **Spatial** (v1): `closestElements(co, radius, domain)` for snapping, plus the
    pick queries in object space. This was called Pickable in the first draft.
  - **OverlayDrawable** (v1, coredcc's own capability): draws into a DrawList.
  - **Elements** (v1): handles plus `topoStamp`.
  - **TransformableElements** (P4): like faberforge's `ITransDataType`.
  - **Symmetry** (P4): the transform and stroke tools need it.
  - **Attributes, Triangles, ActiveElement**: deferred until a client needs them.

### Scene and the scene evaluator
- `Scene` holds:
  - a strong ref list of its objects. An object's id *is* its `BlockHandle`; there is no
    separate pool. Per-element instances, such as 1M scattered copies, are not blocks;
    they live inside an instancer block's data.
  - the active object and selection;
  - parenting, with cycle checks;
  - a `SceneBvh`.
- **The scene evaluator is a graph client.** It owns an internal GraphInstance that it
  builds from the hierarchy:
  - one generated transform node per object;
  - parent links become graph edges;
  - user graphs connect to per-object override inputs.
  - So world matrices are graph outputs, which matches faberforge's §2.4 and avoids a
    one-frame lag when a driver's parent is graph-driven.
- `evaluate()` runs the graph, then the apply phase (§7), then updates SceneBvh from
  Invalidation flags.
- The full datablock library is still out of scope (§15).

## 8a. Datablocks (`lib/`)

Datablocks exist mainly to solve references. Every block has a unique id, and there is an
API for one block to reference another. Requirements from the user, 2026-11-19:
- A datablock is separate from its underlying data, so blocks can wrap data owned by
  anyone, Unreal included.
- Clients can attach arbitrary data to any block.
- Blocks list their references explicitly, for garbage collection.
- References come in non-nullable and nullable (weak) kinds.
- The client builds a block's local id. The full id is that local id plus the client's
  own id.

This section was revised after a pressure test. See the record at the end of the plan.

### Terms
- **Client:** the system that provides datablocks and owns the data behind them. Examples
  are coredcc itself, an Unreal adapter, ubertopo, and faberforge (through foreign
  vtables, §2a).
  - A client registers under a stable name such as `"unreal.actor"`.
  - `ClientId` is a 64-bit SipHash of the name. The collision chance is negligible.
  - The name is stored in files.
  - A client may declare **alias names**, so that renaming a client doesn't turn every
    old block into a placeholder.
- **`BlockType`:** registered by a client. It has a hashed type id, the client, a
  `StructId`, a payload version, and a `BlockProvider` (an interface, §2a).
- **`Datablock`:** a coredcc-side record that clients never subclass. It holds:
  - `BlockId`, type, name and flags;
  - a **state**;
  - a pin count;
  - per-block ref slot tables;
  - attachments.

### Ids and identity
- **`BlockId = {ClientId client; LocalId local}`.** `LocalId` is 128 bits.
- **The client guarantees uniqueness within itself.** Derived or deterministic ids
  aren't unique across documents (ubertopo's operator-derived ids), and Unreal may repeat
  actor GUIDs under PIE or Level Instances; this is checked in the P4 spike. So the
  adapter guide requires clients to **salt `LocalId`s per document or instance**.
- **The built-in `coredcc` client** takes `LocalId`s from a Runtime entropy source,
  backed by a host GUID function or OS randomness, never a seeded PRNG.
- **`BlockHandle`** is a generational, session-local cache, used only for speed.
  - Everything persistent or undo-related stores `BlockId`.
  - Handles are re-bound by `BlockId` after a snapshot restore or a revert.
- **Identity across undo.** A create command records the `BlockId`s it generated. Redo
  revives them with `lib.reviveAs(BlockId)` rather than drawing new ids, so later
  commands' references still resolve. This mirrors Blender's `session_uid`.

### Block states
Each block is in one of these states:
- **Live.**
- **InHistory:** deleted, but restorable by undo. The record is kept and the data is
  owned by the history.
- **Detached:** the host destroyed the data out from under coredcc, for example an actor
  deleted in Unreal's Outliner. The record and slots are kept.
- **Placeholder:** a missing client or type. The record keeps the original type name and
  payload bytes, so the file round-trips.

Rules that follow from the states:
- `data()` resolves on every use. The Unreal adapter, for example, goes through a
  `TWeakObjectPtr`.
- Hosts report out-of-band changes with `onHostDestroyed(BlockId)` and
  `onHostRestored(BlockId)`. An Unreal Ctrl+Z that resurrects an actor moves its block
  from Detached back to Live.

### References: the provider enumerates, the library caches
The first draft synced a slot table on every write. The pressure test showed this drifts:
- faberforge's `lib_users` drift needed a validator and a recount at load.
- ubertopo DEC-015 dropped refcounts for this reason.
- Physical journal undo, snapshot restore and Unreal transactions all change client data
  without calling `setRef`.

So:
- **`BlockProvider::foreachRef(block, visitor)` is the single source of truth.** Each
  block's references are listed explicitly, but by the client, from its own data, on
  demand. This is Blender's `foreach_id` and Unreal's reflected GC.
  - GC, save, remap, import and "who uses me" all pull from it.
- **Incoming relations** are computed lazily and cached by a library epoch, which is
  bumped by any ref write or restore. This is Blender's `BKE_main_relations`. They are
  never maintained incrementally.
- **A debug audit** at `collect()` checks the cached relations against a fresh
  enumeration.
- **Ref kinds.** Strength and nullability are independent:
  - `Ref<T>`: strong, with a non-null record.
  - `OptRef<T>`: strong and nullable, for an optional material slot or an empty
    object's data.
  - `WeakRef<T>`: not strong, and nullable.
  - Every kind stores a `BlockId` plus a cached handle.
  - "Non-null" promises a record, not data. `get()` returns null for a target that is
    Detached, a Placeholder or InHistory, and callers must handle that.
  - A weak ref is never actively nulled. It simply resolves to null while its target is
    absent, and resolves again if the target comes back, for instance on reload or redo.
  - The client hook `onWeakTargetLost` lets, for example, a child keep its world
    transform when its parent goes away.
- **Element-level references** go through a per-block indexed slot table, like Blender's
  material slots. 1M faces using one material cost one slot, not 1M refs.
- **Datapath.** Any Pointer-kind member stored inside a block is a ref, and is enumerated
  by `foreachRef`. That includes GraphDef node parameters and attachment members. This
  means GC, remap and import see them. `StableRef` Pointer values are reserved for
  transient and undo-record use.
- **Slot keys** are namespaced `(provider or attachment type, key)`, so a block's client
  and an attaching client can't collide.

### Lifetime and garbage collection
- **Algorithm.** Mark and sweep over strong refs, enumerated by `foreachRef`.
  - **Roots:** Root and FakeUser flags, the Scene root, history pins, and host pins.
    Graph payloads and the evaluated state hold host pins on blocks whose data they
    alias.
- **When it runs.** Orphans persist until the user saves, does an explicit purge, or
  reloads, as in Blender. There is no idle collection.
  - This means a block that a script has created but not yet assigned is never freed out
    from under it.
  - `collect()` returns Busy while evaluation is running or an interactive command is
    open.
- **Two-phase sweep:**
  1. Sever every ref out of the garbage set.
  2. Call `destroy` on each block.
  - Library mutations from inside `destroy` are deferred to after the sweep. This is
    needed because a TS `destroy` might call into the library.
- **Deletion cascades with a preview.**
  - Nullable refs to the deleted block resolve to null.
  - Users whose non-null `Ref` would break are listed in a preview, and deleted with it
    if the user confirms.
  - The delete is a command, so undo restores everything from the InHistory state.
  - `forceUnlink` exists for refs held by placeholders, which have no UI to edit.
- **Cross-heap rule (Unreal).**
  - Blocks with strong incoming refs keep their UObject alive through `FGCObject`.
  - A UObject that holds a host pin on a block must not be kept alive by that same
    block's strong refs, or neither GC can ever collect the cycle.
  - The adapter guide documents both rules. A debug check detects such cycles.
- **History pins under host-owned undo.** They are released in the `FCommandChange` or
  `MPxCommand` destructor.
  - Unverified: whether Unreal destroys its transaction buffer after coredcc's module
    shuts down. If it does, the destructor would call into a dead Runtime. The P4 spike
    checks this.

### BlockProvider (an interface, §2a)
- `data(block) -> void*`. This may return null when the block is Detached.
- `foreachRef(block, visitor)`.
- `destroy(block)`. For a host-owned block it releases only the wrapper.
- Optional `serialize` / `deserialize`, with the payload version.
- Optional `duplicate(const Vector<BlockHandle>&, IdMap&)`, which duplicates several blocks at once.
  Refs among the duplicated blocks are rewritten through the map.
- Optional `rekey(payload, IdMap&)`, for import.
- **Payload rule.** A payload refers to other blocks only through ref slots, never by
  embedding a `BlockId`. Then rekeying never needs to understand the payload.

### Attachments
- `attach(block, AttachmentTypeId, data)` and `detach` are commands, so they are
  undoable.
- An attachment type registers:
  - a copy callback;
  - a duplicate policy: copy or drop;
  - an optional serializer;
  - an optional `StructId`.
- When the attaching client unloads, or its type is unknown at load, its attachments
  become **opaque bytes**, as faberforge's `OpaqueCustomDataElem` does. They round-trip
  and never run unloaded code.

### Files, linking and import
- Block records, ref slots (as `BlockId`s), attachments and provider payloads are
  written to the file.
- **On load:**
  - A missing strong-ref target becomes a placeholder that records its source file.
  - A missing weak target creates nothing; the weak ref just resolves to null.
- **Append** (copying blocks in from another file) always rekeys coredcc-client blocks.
  For other clients it calls `rekey`, or refuses the append.
- **Link** (referencing blocks that stay in another file, as Blender library links and
  Unreal soft pointers do) needs `BlockId` to carry a source-library scope. It is
  deferred, but ids leave room for it.

### Relation to other sections
- `Object`, `Scene` and `GraphDef` are block types of the built-in `coredcc` client.
  - An object's parent is a `WeakRef`, and its data is an `OptRef`.
  - Scene membership is strong.
- A GraphDef's list of its instances is runtime bookkeeping, not refs.
- Tool settings, preferences and defaults are runtime owners (§5), not blocks.
- **Testing:** the library has a quarantine mode that never reuses freed handle slots,
  so a forgotten root shows up as a dangling reference in tests. ubertopo learned this
  the hard way.

## 9. BVH (`bvh/`)

### Generic core
- `template<BvhSource S> class Bvh`. The concept requires `Scalar`, `size()`, `bounds(i)`
  and `centroid(i)`.
- Per-query callbacks are concept-checked: `rayHit`, `coneHit`, `distSq`,
  `frustumTest`.
- **Nodes:** a flat array of binary nodes. Each leaf covers a contiguous range of a
  reordered index array.
  - Built-in sources store their primitives in leaf order, structure-of-arrays.
- **Build:** binned SAH with 16 bins. It uses `parallel_for`, which is serial with one
  worker.
- **Updates:** `refit()`, plus an explicit `rebuild()`.
- **Type-erased wrapper:** `AnyBvhSource` (a §2a interface) for bindings and foreign
  sources. When a foreign source is `threadAffine`, the build gathers its bounds on the
  main thread before splitting in parallel.

### Built-in sources
- `TriangleSource`.
- `CapsuleSource`: segments with radii.
- `SphereSource`: points with radii.
- Primitive radii are in **object-space units**. A pixel tolerance belongs to the query,
  not the primitive.

### Queries
- Ray (closest, any, all), cone, nearest-within-radius, sphere overlap and frustum.
- **Non-uniform scale.** A circular cone or a sphere in world space becomes elliptical in
  object space. So SceneBvh works in two phases:
  1. A **conservative broad phase in object space.** The cone or sphere is bounded using
     the largest singular value of the inverse transform.
  2. **Exact scoring in world or screen space.** Candidates are transformed back.
- Every hit reports its world position, its depth and its **screen distance in pixels**.
  Picks across objects rank by `(screenDist, depth)`.

### Leaf size
- The default is chosen by `tests/bench_bvh_leafsize.cc`.
- It sweeps leaf sizes from 16 to 1024 against 10k, 1M and 10M triangles, capsule and
  sphere sets, and ray and cone queries.
- Results go in `docs/bvh-leafsize.md`.
- The starting default is 128 until the benchmark has run.

### SceneBvh
- A two-level scheme: a double-precision top level over object world bounds, which calls
  each object's Spatial capability.

## 10. Overlay rendering interface (`draw/`)

### View conventions
The host supplies a `View` with every event and draw. It states every convention
explicitly:
- the viewport rect;
- the view and projection matrices (`mat4d`);
- the depth range: `[0,1]` for WebGPU and D3D, `[-1,1]` for GL;
- a reversed-Z flag, and infinite far plane, as Unreal uses;
- handedness and up axis: Y-up or Z-up;
- the DPI scale;
- whether the projection is orthographic.

Helpers: `project`, `unproject`, `rayFromPixel`, `coneFromPixel(radiusPx)` and
`pixelSizeAt(point)`. Each is tested against every convention combination.

### DrawList
`DrawList` is an append-only buffer of plain POD records.
- **Records:** Lines, Points, Triangles, Circle2D, Line2D, Shape (a built-in id plus a
  matrix), and Push/PopMatrix.
  - Depth modes are test on, test off, and x-ray.
- **Precision.** Each DrawList has a `double3` origin. Vertices are `float` offsets from
  it, which avoids jitter at geographic or CAD scale.
- **Alignment.** Records are 8-byte aligned.
- **Lifetime contract:**
  - The buffer is frozen between `end()` and the next `begin()`.
  - Each frame, the host fetches the pointer and length again. It never caches a
    typed-array view, because Vector reallocation and WASM heap growth both invalidate
    views.
  - Under Electron, N-API copies the buffer, because the V8 sandbox refuses external
    ArrayBuffers. That is accepted: it is still one copy per frame, not one call per
    primitive.
- **Format doc:** the format is versioned and specified in `docs/drawlist-format.md`,
  written in P4.
- **Host notes:**
  - Unreal's PDI has no "fade when occluded", so x-ray is drawn in two passes.
  - Screen-space records go through `DrawHUD`/`FCanvas`, not PDI.
- Object mesh rendering stays with the host or sculptcore.

## 11. Interactive tools (`tool/`)

### Input events
`InputEvent` is one of: pointer down/move/up, wheel, key down/up, cancel or focus lost.

Pointer events carry:
- view-local position and button;
- pointer type (mouse, pen or touch);
- pressure, tilt and twist;
- modifiers and a timestamp;
- optional coalesced sub-samples.

### Tool
`Tool` is the persistent, user-selected tool.
- Methods: `activate`, `deactivate`, `onEvent -> Handled | Unhandled`, `hover` and
  `drawOverlay`.
- Its settings are an ApiStruct, and its options panel is a layout (§12).

### Modal
`Modal` is a temporary event grab for one gesture. It holds a `beginInteractive` handle
(§6).
- Finish commits the command.
- Cancel rolls it back.

### Gizmos
- Gizmos form a tree. Each has `hitTest` (scored by screen distance and depth), `draw`
  (constant screen size) and `beginDrag -> Modal`.
- They edit objects through `applyDelta` with the parent context. Gizmos skip descendants
  of selected objects (§8).
- Built-ins: translate, rotate, scale.

### ToolManager dispatch
Events are dispatched in this order:
1. The top Modal.
2. On press, the gizmo under the cursor.
3. The active tool.
4. Otherwise the event is returned as Unhandled to the host.

### StrokeSampler (P4b)
A generic port of sculptcore's `brush/stroke_driver.h`. It records the samples it
produces so that a stroke command can override `redo` by replaying them.

### Host mapping
- **faberforge:** forwards pointer events and executes the DrawList in WebGPU.
- **Unreal:** one adapter `UInteractiveTool` plus PDI and FCanvas, with
  `UnrealHistory`. Feasibility is verified in a spike before P4 is finalised.
- **Maya:** an `MPxContext` adapter, deferred.

## 12. UI layout description (`layout/`)

### LayoutNode
`LayoutNode` is a tree with these node kinds:
- **Containers:** `column`, `row`, `panel`, `tabs`, `tab`.
- **Leaves:**
  - `prop` (a path);
  - `tool` (an invocation string);
  - `toolOptions` (a command or tool id);
  - `label`;
  - `separator`. It is emitted as `sep` for path.ux.
- **Escape hatch:** `custom` (a tag name and free attributes).

Shared attributes:
- `path` (a prefix for children's relative paths).
- `visibleIf` / `enabledIf`. These take a datapath to a bool, optionally negated with
  `!`.
  - `enum[KEY]` covers equality tests.
  - There is no expression language. Anything more complex is written as a computed bool
    member on the struct, which keeps the logic testable C++ rather than strings in
    layouts.
  - path.ux xmlpage has no such attributes, so its adapter has to add them.
- `label`, `icon`, and style hints.

C++ builds the same tree through a builder API.

### Authoring format: an XML subset
The authoring format is an XML subset: elements, attributes, comments and the five
standard entities.

**Why XML:**
- Layouts are trees of mixed node types with a few attributes each, which XML expresses
  directly.
- path.ux `xmlpage` already covers most of the tags, and its JSX front end compiles to
  that XML, so faberforge and TS authors get layouts with little new code.
- Comments are allowed.
- A dependency-free subset parser is about 400 lines.

**Rejected alternatives:**
- **JSON:** no comments, and trees become noisy nested `children` arrays.
  - JSON is still emitted as a wire format through `toJson()`.
- **YAML:** indentation-sensitive. The spec is large, so our own parser would be a lot of
  work, and implicit typing causes surprises (`no` becomes false).
- **A custom DSL:** no tooling.

### Validation
- `validateLayout(Runtime&, rootStruct, LayoutNode&)` resolves paths statically.
  - Through `refine()` pointers it checks the path against each declared candidate
    struct. A path is accepted if any candidate has the member, and an error is reported
    only if none does.
- Command ids and invocation arguments are checked as well.
- Every layout shipped with coredcc is validated in a test.

### Frontends
path.ux (xmlpage), Unreal (Slate), ImGui (debug). Each binds widgets to batched
`getValues`, `setValueUndo` and ChangeBus watchers.

## 13. Bindings (`bind/`)

### Generic surface
- litestl's binding system binds a small surface rather than every type:
  - `Runtime`, `Context`, `View`, `PropertyDef` (accessor methods), `EnumItem`;
  - `PropValue` (an opaque struct with methods), `StableRef`, `Status`;
  - `CommandExecutor`, `UndoHistory`, `CommandDef`;
  - `ToolManager`, `InputEvent`;
  - `DrawList` (exposed as `data()` returning an address plus `size()`, since `std::span`
    can't be bound);
  - `LayoutNode`.
- `ResolvedPath` and `PointerRef` are never bound.
- **Typed paths for TS:** `gen-paths` emits one TS interface per ApiStruct, with members
  typed as their value types or as references to other interfaces, plus a command catalog.
  - It deliberately doesn't emit a flat union of all paths, which would be infinite for
    recursive structs such as `object.parent.parent…`.
- DCC data is never exposed as directly bound members, because litestl's TS accessors
  write the heap directly and bypass `setValue`.

### Foreign implementations
faberforge's geometry providers, tools, transform models and snapshot providers are
written in TS. During migration TS must be able to implement coredcc interfaces. litestl
has no path from JS into C++ callbacks today. So:
- A foreign implementation is a foreign `IfaceTable` built per JS class (§2a). Every call
  goes to one host dispatcher, which marshals arguments using the slot's binding
  descriptors. No per-interface shim classes are needed.
- The dispatcher is:
  - under WASM: one `EM_JS` function;
  - under N-API: `napi_ref` plus `napi_call_function`.
  - Under both, it catches JS exceptions and converts them to Status.
- Foreign vtables are `threadAffine`. Off-thread calls deadlock or trap, so parallel
  algorithms check the flag (§2a).
- The dispatcher hook goes into litestl's binding system (P0b). `rt.implement` and the
  per-JS-class tables live in `bind/`.

### Required litestl work
This is the P0b branch. Without it nothing else can proceed.
- **String and array marshalling** in both the TS runtime (`buildArgs`) and N-API
  (`marshalArg`). Without it the P1 spike can't pass a path string.
- **The TS return-buffer leak.**
- **The interface support listed at the end of §2a.** This includes the foreign
  dispatcher hook, `Binder<const T>` and noexcept `MethodTraits`. Without the last two,
  no interface with `const T&` parameters can be bound.
- **Embedding hygiene** (§2):
  - `#undef _` after `binding_number.h`;
  - prefix `force_inline`, `array_size` and `flatten_inline`;
  - stop using `ensure` and `verify` as identifiers;
  - fix the C4458 shadowing in `binding_struct.h` and `binding_constructor.h`;
  - make `vector.h`, `string.h` and `task.h` safe with `<windows.h>` min/max;
  - move the Eigen include out of the public `matrix.h`, behind the matrix
    implementation.
- **Math:** quaternion, Euler, decompose and transform helpers (§8).
- **N-API runtime.** Another session is moving it into litestl (branch
  `litestl/napi-bindings`, arriving as a PR on litestl's GitHub repo). P6 builds against
  that once it merges.

### Other language targets
- Python is deferred. It will use the same surface.
- Unreal links C++ directly.

## 14. Phases

Each phase ends with tests passing on native. P1 onward also builds as `wasm-st` and
`wasm-mt`.

### P0 — Scaffold
- litestl submodule with the `if(NOT TARGET)` guard.
- `coredcc_` targets and presets.
- `base/`: Status, hashed type ids, generational `HandlePool` (`util::Pool` has no
  generations), Signal, Runtime.
- The interface system (§2a): `IfaceRef`, `IfaceTable`, `invoke`, `registerImpl` with
  signature matching, the registry, `Owned<I>` and `validateAll`. It depends on P0b's
  binding additions, so in practice P0 and P0b proceed together.
  - Proved with one interface implemented four ways:
    - a bound class;
    - an adaptor over a foreign type;
    - a stand-in foreign table driven by a native fake dispatcher;
    - a deliberately mismatched class, which `registerImpl` must reject with a useful
      message.
- Test harness wiring and the `compile_embed` test (expected to fail until P0b).

### P0b — litestl changes, as PRs
- Marshalling, the leak fix, the callback type, embedding hygiene and math (§13), plus
  the interface support listed in §2a.
- Every litestl change goes up as a pull request against `joeedh/litestl`, the way
  ubertopo's `ubertopo/p4-support` did (merged as PR #2). Split it into reviewable PRs
  along the §13 groups rather than one large branch.
- Work happens in a sibling worktree of `C:\dev\litestl`, never in that checkout: another
  session has it on `litestl/napi-bindings`.
- coredcc's submodule pins the PR branch until the PR merges, then moves to master.

### P1 — Application spine
- The library core (§8a): clients, block types, ids and handles, block states, the ref
  kinds, `foreachRef` with lazily cached relations, `reviveAs`, GC with pins and the
  two-phase sweep, attachments, runtime owners, and quarantine mode. File IO for the
  library follows in P2.
- props, api (canonicalisation, ChangeBus), the command executor, LinearHistory,
  SetPropertyCommand, sub-commands, the interactive protocol, LastUsedStore and the
  invocation parser.
- **Exit criteria:**
  - A `wasm-st` TS spike:
    1. Resolve a path.
    2. Get and set a value.
    3. Undo.
    4. Implement the Command interface in TS through a foreign vtable.
  - A fuzz test for the path parser.
  - Interleaving tests: an interactive command combined with a set or an undo.

### P2 — Layout and library IO
- Builder, XML subset parser, `toJson`, validator, example layouts.
- Library file section: placeholders for missing blocks, and import remapping.

### P3 — Objects and BVH
- TransformModel, TrsTransform and FbxTransform (with tests for the three inherit types).
- Object, its capabilities, Scene.
- BVH, its built-in sources, SceneBvh with the two-phase queries.
- **Exit criteria:** the leaf-size benchmark has run and its results are recorded. The
  scene evaluator waits for P5.

### P4 — Tools
- View (with the convention tests), the DrawList and its format doc, InputEvent, Tool,
  Modal, ToolManager.
- Transform gizmos, TransformableElements, Symmetry.
- Headless host with scripted-event tests: drag, then undo, then redo.
- An Unreal feasibility spike, run in parallel.
- **P4b:** StrokeSampler.

### P5 — Graph
- Socket types, node types, GraphDef with serialization, GraphInstance, group nodes,
  global versions and pull evaluation.
- P5 depends only on P1, so it can run in parallel with P3 and P4.
- **P5b:** foreach instancing.

### P5c — Scene evaluator
- Generated transform nodes, override inputs, adapter nodes and the apply phase.
- It needs P3 and P5.

### P6 — Integration
- N-API addon build, against litestl's N-API runtime (see §13).
- A faberforge spike: a property sheet from a layout, and a gizmo tool, with a TS object
  implemented in TS through foreign vtables.
- A ubertopo spike: commands delegating to `Editor<C>`.
- `UnrealHistory` if the P4 spike succeeded.

### Deferred
- Python.
- The Unreal plugin proper. `MayaHistory` and the Maya adapter.
- CAD integration.
- Mass-set paths and macros.
- Cyclic graph regions.
- Content-addressed and parallel graph evaluation.
- Animation.
- A full scene file format beyond the library section.
- The Attributes, Triangles and ActiveElement capabilities.

## 15. Decisions on the former open questions (user, 2026-11-19)

1. **Datablock references are enumerated by the client** (`foreachRef`), not stored in a
   coredcc-maintained list. `OptRef`, cascade-with-preview delete and the 64-bit
   `ClientId` are accepted too.
2. **sculptcore's `source/props` will migrate onto coredcc props** eventually. Not
   scheduled; props should not paint themselves into a corner that blocks it.
3. **Rejecting graph cycles in v1 is fine.**
4. **The N-API runtime** is being moved into litestl by another session, and will arrive
   as a litestl PR.
5. **litestl changes go up as PRs** against litestl's GitHub repo, as ubertopo's did.
   ubertopo's `ubertopo/p4-support` branch is now merged into litestl master (`8a98264`).

## Pressure test record

The plan was pressure-tested on 2026-11-19 by a fresh subagent. It was given the plan and
the sibling repos but none of the author's reasoning. It ran compile experiments with
MSVC, clang-cl and emcc.

### Claims that survived
- **litestl builds with exceptions and RTTI off.** It contains no
  try/throw/typeid/dynamic_cast, and the headers and core .cc files compile with
  `/GR- /EHs-c-`.
- **TS member accessors poke the heap directly** (`typescriptRuntime/bind.ts:77-163`).
  This confirms that DCC data must not be bound as members.
- **The TS return-buffer leak** (`binding.ts:461-477`) and **`Struct::inherit` duplicating
  members** are both real. `inherit` also fails to adjust parent offsets.
- **lz4 is available through `litestl/io/compress.h`** (in namespace `sculptcore::io`).
- **`AABB` and `mat4d` exist.**
- **ubertopo's FP flags** and its **`Editor<C>` journal undo** are as described.
- **faberforge's 2-corner bounds bug.**
- **path.ux xmlpage and its JSX → XML front end exist.**
- **Cone picking has prior art** in sculptcore (`spatial.h:177-190`).

### Claims that broke, and the resulting changes

**litestl and bindings**
- **litestl provides a usable quaternion.** Broke: `quat.h` has never compiled, and there
  is no Euler conversion and no decompose.
  - Change: math fixes added to P0b.
- **The binding system can bind a variant PropValue and a byte span.** Broke: there is no
  `Binder` for `std::variant` or `std::span`, and `types::Union` describes a union of
  structs, not a value.
  - Change: PropValue is an opaque struct; DrawList is exposed as `data()` plus `size()`.
- **Only N-API lacks string marshalling.** Broke: the WASM TS runtime lacks it too
  (`binding.ts:340-357`).
  - Change: string marshalling moved to P0b, ahead of the P1 spike.
- **`ValueOrErrors` can carry error detail.** Broke: its error names are compile-time
  literals.
  - Change: use coredcc `Status` everywhere.

**Embedding and globals**
- **"Public headers define no short macros" is achievable.** Broke: litestl leaks `_`,
  `force_inline`, `array_size` and `MARGS`. Its use of `ensure` and `verify` collides with
  UE macros. The `<windows.h>` min/max macros and C4458 shadowing warnings break it.
  Eigen sits in a public header.
  - Change: hygiene work added to P0b, plus the `compile_embed` test.
- **No global registries.** Partially broke: `Object::api()` forced static ApiStructs,
  and address-based type ids differ across UE DLLs.
  - Change: a Runtime owns every registry, objects return `StructId`, type ids are hashed
    names, and coredcc is a single UE module.
- **"Works with zero workers."** Reframed: the pool always has at least one worker, and a
  pthread build can't load without COOP/COEP.
  - Change: two WASM flavours, and coredcc never calls `task::run`.
- **Reuse sculptcore's N-API runtime via DEC-005's move.** Broke: DEC-006 defers that move.
  - Change: P6 uses sculptcore's copy; open question 4.

**Objects, transforms and picking**
- **Capabilities come from geometry-contract.md.** Broke: Pickable and OverlayDrawable are
  not in it, and Invalidation and Symmetry were dropped. The contract also makes object
  matrices graph outputs.
  - Change: capabilities renamed and extended, and the scene evaluator made a graph client
    (§8).
- **The TransformModel signature is enough for FBX.** Broke: `world` had no EvalCtx and
  there was no component query. `setLocal` and `applyDelta` lacked parent context.
  - Change: the signatures were redone in §8.
- **Cone queries give screen-space pick tolerance.** Partially broke under non-uniform
  object scale.
  - Change: a two-phase SceneBvh query, object-space radius units, and screen-distance
    scoring.

**Layout and phases**
- **The validator resolves every path statically.** Broke at `refine()` pointers.
  - Change: `refine` declares its candidate structs.
- **`commandDefaults.<id>.<input>`.** Broke, because ids contain dots.
  - Change: the key form `commandDefaults["id"]`.
- **P5 is independent of P3/P4.** Broke through the adapter nodes.
  - Change: those moved to P5c.

### Failure scenarios found, and how the plan now handles them
1. **Native hosts own undo.**
   - Handled by splitting the executor from a pluggable `UndoHistory`, with Unreal and
     Maya adapters.
2. **TS cannot implement coredcc interfaces.**
   - Handled by the foreign-implementation shims and a litestl callback type.
3. **Undo writes to the wrong object** (via `active` or an index), and raw pointers
   dangle.
   - Handled by path canonicalisation, `StableRef`, generational OwnerIds and a mandatory
     `keyOf`.
4. **An interactive command interleaved with other commands.**
   - Handled by returning `Busy` while a command is open, cancelling it before undo, and
     requiring interactive commands to override `redo`.
5. **Commands need sub-commands.**
   - Handled by `ctx.runSub` with implicit grouping.
6. **Graph writes conflicting with user edits, and feedback loops.**
   - Handled by the original/evaluated split, the apply phase, and the scene evaluator
     inside the graph.
7. **FBX and gizmo writes.**
   - Handled by the TransformModel redesign and the rule that gizmos skip descendants of
     selected objects.
8. **Version ABA after a snapshot undo.**
   - Handled by a global monotonic version counter that is never snapshotted.
9. **Embedding: macros, Eigen, duplicate statics, two litestl submodules.**
   - Handled by the P0b hygiene work, the `if(NOT TARGET)` guard, hashed type ids, and
     coredcc as one UE module.
10. **WASM threading and DrawList memory.**
    - Handled by the two build flavours, the DrawList freeze contract, a double origin
      and 8-byte alignment.
11. **ChangeBus matching.**
    - Handled by resolving watcher paths again at flush, a structural epoch and a value
      diff.
12. **Clip-space conventions and precision at large scale.**
    - Handled by explicit View conventions with tests, and the DrawList double origin.
13. **Non-uniform scale in picking.**
    - Handled by the two-phase query.
14. **Binding gaps.**
    - Handled by binding `Context` and `View`, adding batched `getValues`, and having
      `gen-paths` emit per-struct interfaces.
15. **Smaller items.**
    - Fold only at the cursor.
    - A def → instance list.
    - LRU eviction for the foreach pool.
    - Accept both comma and space separators in invocation strings.
    - No string interning on worker threads.
    - LastUsedStore serialization.
    - Context path-prefix overrides.
    - Snapping via the Spatial capability.
    - GraphDef serialization in P5.

### Still unverified
- **Unreal defaults to exceptions and RTTI off.** This is documented UE behaviour, but
  there was no UE install to test against.
- **A single `UInteractiveTool` adapter with PDI/FCanvas is enough.** The P4 spike checks
  this.

## Pressure test record: datablocks (§8a)

The section was pressure-tested on 2026-11-19 by a fresh subagent. It read faberforge's
`lib_api.ts`, `missing_addon.ts` and undo code; ubertopo DEC-009 and DEC-015 to DEC-017;
and litestl's rng, hash128 and pool store. Points about Blender and Unreal came from
knowledge, not from running code.

### Survived
- **Mark and sweep with a FakeUser root** is sound inside one heap. It matches DEC-015.
- **A 64-bit SipHash `ClientId` with the name stored** is enough.
- **Placeholders round-trip** when refs live in the library section. This matches
  faberforge's `MissingDataBlock`.
- **The 128-bit `LocalId` size** is adequate.
- **History pins can be released** from host undo-record destructors.

### Broke, and what changed
- **Keeping a stored ref list in sync on every write drifts from client data.**
  - Journal undo, snapshot restore and Unreal transactions bypass `setRef`, and faberforge's
    `lib_users` drift shows the same failure.
  - Change: `foreachRef` is the source of truth, relations are cached lazily, and a debug
    audit compares the two.
- **Block refs stored as `StableRef` PropValues** (graph node parameters, for example)
  bypassed GC and remap.
  - Change: Pointer members inside blocks are refs.
- **"`Ref<T>` always points at a live block"** fails when the host deletes or re-instances
  objects, and for placeholders and empties.
  - Change: block states (Live, InHistory, Detached, Placeholder), host events, and
    "non-null" meaning a non-null record. Strength and nullability are now independent,
    with `OptRef` added.
- **Undo identity built on handles** breaks on redo of a create and on snapshot restore.
  - Change: persistent refs store `BlockId`, creates are revived with `reviveAs`, and
    handles are only caches.
- **A Scene `HandlePool` duplicating `BlockHandle`.**
  - Change: an object's id is its block handle, and instances are not blocks.
- **"Only blocks own undoable data"** excluded tool settings, preferences and defaults.
  - Change: runtime owners.
- **`LocalId` uniqueness.** Derived ids collide across documents, Unreal may repeat GUIDs
  (to be confirmed in the P4 spike), and a seeded Philox repeats across sessions.
  - Change: per-document salting, and the coredcc client uses an entropy source.
- **`duplicate -> LocalId` couldn't handle import or duplicating several blocks at once.**
  - Change: `duplicate(span, IdMap&)`, `rekey`, and the rule that payloads never embed
    `BlockId`s.
- **Attachments ran unloaded code**, and slot keys collided.
  - Change: unknown attachments become opaque bytes, and slot keys are namespaced.
- **Sweep order and re-entrancy.**
  - Change: a two-phase sweep, with library mutations from `destroy` deferred.
- **Graph and evaluated state aliasing block data was not rooted.**
  - Change: host pins, and `collect()` returns Busy during evaluation or an interactive
    command.
- **Name clash** between the graph's and the library's `Ref<T>`.
  - Change: the graph's is now `Payload<T>`.
- **Canonical paths kept hops through refs.**
  - Change: re-root at the innermost block.
- **Refusing a delete is poor UX, and idle GC freed fresh orphans.**
  - Change: deletion cascades with a preview, and orphans persist until save or purge.

### Still unverified
- Whether Unreal repeats `ActorGuid` under PIE and Level Instances.
- Whether Unreal's transaction buffer outlives the coredcc module.
- Whether `FGCObject` plus the cross-heap rule is enough in practice.

All three go to the P4 Unreal spike.

## Pressure test record: interfaces, draft 1 (X-macro fat pointers)

This record covers draft 1 of §2a. Draft 2, built on the binding system, kept these
findings:
- `threadAffine`;
- the exception-catching dispatcher;
- explicit type names;
- `as<T>()`;
- `queryRaw`;
- never binding `Owned<I>`;
- the `const T&` and noexcept litestl fixes.

It dropped the X-macro, the compile-time foreign vtable and the embedded base vtables.

The section was pressure-tested on 2026-11-19 by a fresh subagent. It compiled experiments
with clang-cl, MSVC (without `/Zc:preprocessor`) and emcc 3.1.53 under node. The
experiments are in the session scratchpad, under `iface/`.

**Verdict:** fat pointers plus a registry hold up against multiple inheritance with
COM-style query, the Microsoft `proxy` library, dyno/te, and `std::function` bundles.

### Survived
- **The case against multiple inheritance:** a wrapper per entity, and the
  `static_cast<T*>(void*)` thunk at `binding_method.h:142`.
- **Query cost:** about 8 ns per uncached query.
- **TypeIds from hashed names,** provided the names are explicit.
- **Foreign vtables under `wasm-st`,** including struct, by-value, i64 and scalar-struct
  returns.
- **`registerImpl` conformance checking,** after two fixes:
  - member-template checks, needed for MSVC;
  - exact const-ness and return types.
- **One-place declaration** with a classic X-macro.

### Broke, and what changed
- **A templated `Iface<I>` cannot be bound.** All instantiations land in one TS file, and
  the generated return types are invalid.
  - Change: one non-template struct per interface.
- **`const T&` and `noexcept` methods cannot be bound.** `Binder<const T>` and the
  noexcept `MethodTraits` are missing.
  - Change: both added to P0b.
- **"Foreign vtables are never called off the main thread."**
  - Under `wasm-mt` the call traps. Under N-API it deadlocks inside `parallel_for`.
  - Change: a `threadAffine` flag, and serial fallback or main-thread gathering.
- **JS exceptions unwound through C++ frames and skipped destructors.**
  - Change: the dispatcher catches them, and slots return Status or set a sticky error.
- **`addFunction` per slot** needed per-signature ABI lowering and leaked table slots.
  - Change: one compile-time foreign vtable per interface, through a single
    `MethodThunk` dispatcher.
- **The `size` versioning field** cannot protect derived interfaces with embedded bases.
  - Change: the field is dropped, since coredcc is a single module.
- **TypeIds for types we don't own** cannot be inferred.
  - Change: explicit type names.
- **There was no downcast,** yet `foldFrom` needs one.
  - Change: `implType` in the header and `as<T>()`.
- **`query<I>` is a template and cannot be bound.**
  - Change: `queryRaw` plus a checked constructor.
- **`Owned<I>` can't be bound,** because it is move-only.
  - Change: bindings use handles.
- **The plan contradicted itself:** Command, AnyBvhSource and TransformModel were
  described as virtual classes or with pointer parameters.
  - Change: all three rewritten as interfaces.

### Still unverified
- Whether Unreal compiles with `/Zc:preprocessor`. The classic X-macro avoids the
  question.
- How the generated thunks look in a debugger. A natvis entry may be needed.
- The N-API foreign path. It was reasoned about but not built.

## Pressure test record: interfaces, draft 2 (binding descriptors)

The section was pressure-tested on 2026-11-19 by a fresh subagent that built a working
prototype.
- **Prototype:** `iface2/` in the session scratchpad, with a locally patched litestl copy.
- **Compilers:** clang-cl and MSVC (`/GR- /EHs-c-`), with identical results, and emcc
  3.1.53 under node 24, running the real TS runtime.

**Verdict:** draft 2 beats draft 1, provided the five most severe fixes below are made.
- Draft 1 already needed a bound non-template struct per interface for TS, so the
  binding descriptors were needed anyway.
- In draft 2, the C++ slot list and the TS slot list come from one place.

### Survived
- **Descriptors work as a slot list.** They give order, names, parameters, const-ness and
  return types. `isInterface` and `isOptional` are cheap to add.
- **Matching by name and signature** catches every ordinary mismatch, with clear
  messages. Overloads in the implementation and extra methods are fine.
- **Calls through the thunk.** References write through with no copy. Struct returns are
  placement-constructed into `ret`, and the object count balanced to zero. noexcept
  implementations work.
- **Adaptor methods** are a small extension. They also let one class implement two
  interfaces whose same-named slots differ.
- **The function-local slot cache** is correct with 16 threads racing on the first call,
  and lazy enough to survive static initialisation.
- **The TS generator** can emit implementable interfaces. tsc accepted a correct class and
  rejected an incomplete one.
- **A foreign dispatcher under emcc** drove a JS implementation through the real TS
  runtime: a scalar, a struct by reference, a struct return, int64, and an optional slot
  falling back to the C++ default.

### Broke, and what changed (most severe first)
1. **`invoke<R>(slot, args...)` checked no types.** It returned garbage and overflowed the
   return buffer.
   - Change: `CDCC_CALL` is mandatory, and takes its types from the forwarder's member
     pointer.
2. **Concatenating several base interfaces ran the wrong slot.**
   - Change: one concatenated base only; other bases by upcast.
3. **Self-referencing interfaces overflowed the stack.**
   - Change: `CDCC_INTERFACE` caches the descriptor first.
4. **Descriptors erased const,** so writes went through `const` objects in C++ and from JS.
   - Change: const-recording binders, and `Readonly<>` in TS.
5. **Type identity was only a name string,** so an aliased name was accepted and memory
   was corrupted.
   - Change: compare size and a per-C++-type key as well.
6. **Foreign return ownership** leaked a JS-created struct. **int64** was typed as
   `number` but passed as a BigInt.
   - Change: copy into `ret` then dispose, and `bigint`.
7. **Template implementations** couldn't use the binding macros.
   - Change: `->template`.
8. **Signatures the binding system can't express:** reference returns, primitive
   out-parameters and `Span` parameters.
   - Change: added to the declaration rules. §8a's `duplicate` now takes a
     `Vector<BlockHandle>`.

Also changed:
- **A missing forwarder slot** was caught on first call, not at startup. Now
  `CDCC_CALL` catches it at compile time.
- **`registerImpl`** rebuilt the implementation's descriptor once per interface. It is
  now cached per Runtime.

### Measured
- **Native call:** 1.24–1.74 ns, against 0.84–0.96 ns for a direct virtual call
  (clang-cl).
- **Startup:** 300 interfaces registered in about 46 ms, and `validateAll` took about
  2.5 ms.
- **Foreign call (WASM):** 85 ns for a scalar, and 2.3 µs for two struct arguments
  through the naive runtime.
- **Build time:** about 40–80 ms per bound method at /O2.
  - Change: binding code lives in per-module `bindings.cc` translation units.
  - P0 measures the real cost.

### Still unverified
- The N-API reverse marshalling.
- Two Runtimes in one process. This was reasoned about but not run.
