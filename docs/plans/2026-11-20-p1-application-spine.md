# P1: application spine

Implements architecture §14 P1: the library core (§8a), properties (§4), the datapath
layer (§5), and commands and undo (§6), plus the first WASM build and a TS spike. The
interface system (§2a, P0) is the foundation; see
`docs/plans/2026-11-19-p0-scaffold-interfaces.md` and
`docs/reports/feature-report-interfaces-2026-11-20.md`.

Status: pressure-tested (2026-11-20) and revised; the record is at the end.

## Exit criteria (from architecture §14)
- Native tests pass on msvc and clang-cl, in Debug and RelWithDebInfo.
- `wasm-st` and `wasm-mt` build, and the native test suite passes as `wasm-st` under
  node.
- **A `wasm-st` TS spike** that does four things:
  1. resolves a path;
  2. gets and sets a value;
  3. undoes the set;
  4. implements the `Command` interface in TS through a foreign table, and runs it
     through the executor, including undo.
- A fuzz test for the path parser, and another for the invocation parser.
- Interleaving tests: an interactive command combined with a set, and with an undo.
- A feature report for each new module (props, lib, api, command), in
  `docs/reports/feature-report-<module>-<date>.md`. Findings are either resolved or
  tracked in the task list.

## Where the work happens
- **P1 is on master.** litestl #4, #5 and #6 merged on 2026-11-21, and the interface
  system's `spine` branch fast-forwarded into master.
- **C2** is a litestl PR: TS generator support for implementable interfaces, plus the
  foreign-dispatch hook. Only the spike needs it, and it comes after the WASM
  toolchain.
- **Parallel phase:** P1 is still serial. The worktree and sccache setup in the task
  list happens at the end of P1.

## WASM toolchain
- **Emscripten, pinned to sculptcore's version (`emsdkVersion.txt`: 5.0.6), including its
  emsdk commit (`2a9b469`).** The reasons:
  - coredcc is to become the core of faberforge, which embeds sculptcore's emcc build
    (pthreads, `SHARED_MEMORY`, `IMPORTED_MEMORY`, WebGPU). One module needs one
    toolchain with matching flags.
  - The foreign dispatcher uses `EM_JS`.
- **Gate result (pressure test, 2026-11-20):**
  - emcc 5.0.6 compiles litestl's `binding.cc` and every coredcc base TU with
    `-Wall -Wextra -Werror`.
  - The `std::format` failure seen with emsdk 3.1.53 does not reproduce.
  - The base tests pass as `wasm-st` under node. `test_iface` and `test_signal` also pass
    as `wasm-mt`.
  - Re-run the gate on the final interface code, which changed after the test.
- **Install:** `tools/install-emsdk.mjs` clones emsdk at the pinned commit into
  `build/deps/emsdk` (gitignored), then runs `install` and `activate`, without
  `--permanent`.
- **Every WASM TU** gets `-DWASM`, and CMake runs with `BUILD_WASM=ON`, as sculptcore does.
  Without them:
  - `alloc.cc` fails a `MemHead` static assert;
  - `Vector`'s layout disagrees with litestl's TUs and with `boundVector.ts`;
  - `platform/wasm.cc` isn't built, so `cpu_core_count` is undefined at link time.
- **Presets** (Emscripten toolchain file, Ninja Multi-Config):
  - **`wasm-st`:**
    - `-sALLOW_MEMORY_GROWTH=1`;
    - `-sSTACK_SIZE=8388608` (emcc's default stack is 64 KB);
    - `-sEXPORTED_RUNTIME_METHODS=wasmMemory,HEAPU8`, which litestl's TS runtime needs to
      patch `memory.grow`;
    - `CMAKE_NINJA_FORCE_RESPONSE_FILE`, because em++.bat hits the 8191-character
      command-line limit (sculptcore does the same).
  - **`wasm-mt`:** the same, plus `-pthread -sSHARED_MEMORY=1`. Expect
    `-Wpthreads-mem-growth`.
  - **Tests** are built with `-sENVIRONMENT=node`, plus a `--pre-js` that copies
    `process.env` into `ENV`. Without it `COREDCC_TEST_SCALE` and `LITESTL_TEST_NO_BREAK`
    are invisible. The Emscripten toolchain file already sets
    `CMAKE_CROSSCOMPILING_EMULATOR` to node.
  - **Test app:** its module is built with `-sENVIRONMENT=web` for st and `web,worker`
    for mt, as the test-app plan says.
- **To check at configure time:** whether litestl's `EXCLUDE_FROM_ALL` WASM target
  (`--use-port=emdawnwebgpu`) triggers a network fetch.
- **No `size_t`, `long` or `unsigned long` in a slot or a bound member.** litestl has no
  `Binder<unsigned long>`, and that is `size_t` on wasm32. It compiles natively and fails
  under emcc. Use `int64_t` or `uint64_t`. The wasm-st build is part of the
  pre-commit gate, because the native builds can't catch this.

## Module layering
`base` ← `props`, `parse` ← `lib` ← `api` ← `command` ← `bind`

`parse` (the path and invocation parsers, and `PathCache`) depends only on `base`.

- **Binders live with their types.**
  - Every type in a slot needs its `Binder` visible where the interface is declared.
  - So `Status` binds in `base/`, `BlockHandle` and `RefCell` in `lib/`, `Context` in
    `api/`, and `Refusal` in `command/`.
  - `bind/` only registers types with a `BindingManager`, implements the dispatchers,
    and owns the TS entry points.
- **Runtime extensions.** `Runtime` lives in `base` and can't name the later modules'
  registries. It gets a type-erased extension slot:
  - `rt.ext<T>()` creates the slot on first use and destroys it with the Runtime, in
    reverse order of creation;
  - each module provides `registerBuiltins(Runtime&)` (architecture §2), for example the
    built-in `coredcc` client from `lib/`;
  - `Runtime::validateAll()` calls each extension's validator.
- **New ids:**
  - `base/block_id.h`: `LocalId` (128 bits) and `BlockId {ClientId client; LocalId local}`;
  - `IdDomain` gains `Struct` and `Command`, with domain tags `"struct:"` and
    `"command:"`. Tags are part of the file format;
  - the name table becomes an array sized from the enum (it is `names_[3]` today).
- **Block types and capabilities.**
  - A block type's name is the implementation name of its *data*'s capabilities in the
    interface registry, so `query<I>(BlockHandle)` resolves through
    `(typeId, provider.data(block))`.
  - The provider object registers under its own name, `<type>#provider`. One name can't
    carry two C++ types: the registry rejects it.
- **One sticky error, on the Runtime.** Calls that can't return a Status (failed foreign
  calls) set it. The executor checks it after each command, and GC checks it after each
  provider call.
- **Libraries.** A document's blocks live in a `Library`, which the host creates. A
  Runtime can have several.

## P1a: `props/`
- **`PropKind`:** Bool, Int, Float, String, Enum, Flags, Vec2, Vec3, Vec4, Mat4, Quat,
  Pointer, Collection. Capability queries: `isNumeric`, `isVector`, `componentCount`.
- **`PropSubtype`:** None, Color, ColorGamma, Angle, Distance, Factor, Percentage, Time,
  Direction, Euler, Xyz, FilePath, Password.
- **`PropAccess`** flags: ReadOnly, Private, NoUndo, Optional, SaveLastValue, Animatable.
- **`PropUi`:** iconsOnly, expand, slider style, multiline.
- **Kind-specific metadata:** `std::variant<std::monostate, NumericMeta, EnumMeta,
  StringMeta, VectorMeta>`.
  - It is read only with `std::get_if`.
  - This works without exceptions under MSVC (pressure test).
  - Bindings see accessors, never the variant.
- **`EnumItem {key, value: int64, uiName, description, icon}`.** Enum values are int64 and
  Flags values are uint64.
- **`PropertyDef`:**
  - a value type with a fluent builder;
  - `uiName` derived from `apiName` (camelCase and snake_case);
  - `defaultValue`;
  - `dynamic`, a `litestl::util::function` (that is `std::function`) that patches a copy
    of the def at read time. It must be tested before it is called: an empty one
    fail-fasts.
- **`PropValue`:**
  - an opaque tagged struct: None, Bool, Int (int64), Float (double), String,
    Vec2/3/4 (double), Mat4 (double, column-major), Quat (xyzw), Enum (int64),
    Flags (uint64), Pointer (`StableRef`);
  - checked accessors (`asFloat()`), and optional ones (`tryFloat()`);
  - **`operator==` and `hash()` are bitwise for floats.** NaN equals itself and
    `-0.0 != 0.0`, so the watcher diff and the hash agree. `approxEqual` is separate.
- **`coerce(def, value) -> Result<PropValue>`** is the single conversion point:
  - Int → Float;
  - Float → Int when integral *and* inside the int64 range;
  - Enum by key or value, Flags from a key set, a Vec from a broadcast scalar;
  - hard-range clamping;
  - an error otherwise, with a suggestion for enum keys.
- **`StableRef` and `OwnerKey`** (§5) live in `props/ref.h`:
  - `OwnerKey` is a `BlockId` or a runtime owner `{uint64 nameHash}`;
  - `Key` is a vector of segments, each a member name, a string key or an int64
    handle;
  - a component (`location[2]`) or flag-key access canonicalizes to the whole member:
    undo stores the whole old and new value.
- **`PropertyBag`:** an ordered `Vector<{PropertyDef, PropValue, wasSet}>`.
- **Units** (`props/units.h`):
  - a table of families (Length, Angle, Time, Mass, Factor), each with base and display
    units;
  - `format(value, family, unit, precision)` and
    `parse(text, family, displayUnit) -> Result<double>`;
  - a bare number is read in `displayUnit`;
  - no expressions.
- **Tests:**
  - coercion tables, including int64 overflow;
  - clamping;
  - enum key suggestions;
  - units round trips, with the tolerance `10^-precision × unitScale`;
  - NaN and -0 equality and hashing.

## P1b: path and invocation parsers
- **Path grammar:**
  - `a.b.c`;
  - `list[3]`, `list["key"]`, `list['key']`, `list[#123]`;
  - `vec[2]`;
  - `enum[KEY]`.
  - `active` and `length` parse as plain member names. The resolver interprets them only
    on collections, because `bone.length` and `curve.length` are real members.
  - **String escapes:** `\\`, `\"`, `\'`, `\n`, `\t`. Any other escape is an error.
  - **NUL** is rejected, raw or escaped: litestl strings truncate at NUL when they cross
    a `stringref`.
  - **Integers** that overflow int64, in either an index or a `#handle`, are errors.
- **Output:** `Path {Vector<Segment>}`, where `Segment {kind, name, int64 index, string key}`
  and kinds are Member, Index, StringKey, HandleKey, EnumKey. `toString()` prints the
  canonical spelling.
- **Errors:** `ParseError {size_t offset; Status status}`, with `0 ≤ offset ≤ len`. The
  offset can equal the length, for end-of-input errors such as `"a."` and `""`.
- **`PathCache`** is a Runtime extension. It stores parsed paths in refcounted nodes and
  returns `PathPtr`, a refcounted handle, because litestl `Map` moves its values on
  rehash. It is bounded by LRU.
- **Invocation strings:** `mesh.extrude(dist=1.0 mode='X')`.
  - The tokenizer yields the command id and `(name, rawValue)` pairs, separated by
    spaces or commas.
  - Raw values are numbers, quoted strings, identifiers and bracketed tuples.
  - A Pointer input takes a quoted path (`target="objects[\"A\"]"`), which `coerce`
    canonicalizes in `command/`.
- **Fuzz tests** (seeded splitmix64, scaled by `COREDCC_TEST_SCALE`):
  - random bytes, plus mutations of a corpus of valid paths;
  - no crashes;
  - a valid path round-trips;
  - every error has `0 ≤ offset ≤ len`;
  - NUL and int64 overflow are always errors.

## P1c: `lib/`
- **`ClientRegistry`** (a Runtime extension):
  - `registerClient(name, aliases) -> Result<ClientId>`;
  - `BlockId`s are normalized to the canonical `ClientId` when they are built from a
    name, and when they are looked up, so a renamed client's old ids still resolve.
- **Entropy:** `Runtime::setGuidSource(fn)`. The default is libc `getentropy()`, which
  emcc implements safely for shared memory. Raw `crypto.getRandomValues` on a shared
  view throws in browsers.
- **`BlockProvider`**, an interface with these slots:
  - `data(BlockHandle) -> void*`;
  - `foreachRef(BlockHandle, RefVisitor)`;
  - `destroy(BlockHandle)`;
  - optional `serialize` and `deserialize`, signatures now and implementations in P2;
  - optional `duplicate` and `rekey`;
  - optional `queryInterface(BlockHandle, InterfaceId) -> BlockHandle`, the hook in
    §2a's query order;
  - optional `onWeakTargetLost(BlockHandle, BlockId)`.
- **`BlockTypeRegistry`** (a Runtime extension): `registerBlockType(client, name, StructId,
  payloadVersion, provider)`.
- **`Library`:**
  - `HandlePool<Datablock>`;
  - `Map<BlockId, BlockHandle>`, which holds **at most one record per BlockId** (checked
    by `validateAll`);
  - the epoch;
  - the relations cache;
  - GC state.
- **`Datablock`:**
  - `BlockId`, `TypeId`, name, flags (Root, FakeUser, `hasOverrides`), state, pin count;
  - ref slot tables, keyed by `(namespace TypeId, key)`;
  - attachments;
  - the per-block interface override table.
- **`query<I>(lib, BlockHandle)`** checks, in this order:
  1. the override table, if `hasOverrides` is set;
  2. a per-BlockType cache;
  3. the provider's `queryInterface`.
- **States and transitions** (§8a):
  - `markDeleted` and `restoreFromHistory`, `onHostDestroyed` and `onHostRestored`,
    `addPlaceholder`.
  - `reviveAs(id)` is defined for every state:
    - a Live id gives AlreadyExists;
    - an InHistory, Detached or Placeholder record gives FailedPrecondition, naming the
      state (callers restore those, not revive them);
    - an unknown id creates a record.
- **Refs:**
  - `RefCell {BlockId target; mutable BlockHandle cache; RefKind kind}` is a bound struct;
    `Ref<T>`, `OptRef<T>` and `WeakRef<T>` wrap it.
  - `get(lib)` always compares the record's `BlockId`, not just the handle's generation.
    Two Libraries hand out the same first handle.
  - Foreign (TS) data holds refs as bound `RefCell` objects in WASM memory, never JS
    copies, so remap can rewrite them through `visit(RefCell&)`.
- **Relations:**
  - `usersOf` is computed lazily and cached by epoch;
  - every ref write through the library, every restore, and every `setValue` on a
    Pointer member (P1d) bump the epoch.
  - The debug audit at `collect()` catches a stale cache: a client that wrote a ref
    without the library seeing it. It can't catch a provider that consistently omits a
    ref. Its test is named for the staleness case.
- **GC:**
  - mark from the roots (Root, FakeUser, history pins, host pins);
  - a two-phase sweep;
  - `collect()` is a library call, not a command (GC isn't undoable). It returns Busy
    while an interactive command is open or an evaluation is running. An ordinary
    `exec` doesn't block it, so a purge command can collect;
  - a sticky error after any provider call (a TS `foreachRef` that threw) aborts the
    collection before the sweep;
  - inside `destroy`, reads are allowed, void mutations are queued until after the
    sweep, and creates return Busy;
  - `forceUnlink(user, slot)` exists for refs held by placeholders.
- **Attachments, runtime owners, quarantine mode:** as §8a.
- **Tests:**
  - ref kinds against every state;
  - cross-library handle aliasing;
  - `reviveAs` in every state;
  - GC roots, pins and cycles;
  - re-entrant `destroy`;
  - an aborted collection on a sticky error;
  - placeholders;
  - weak refs resolving again;
  - the relations audit catching a stale cache;
  - a foreign provider through the fake dispatcher;
  - client aliases;
  - the query order.

## P1d: `api/` resolver and ChangeBus
- **`PointerRef {void *data; StructId type; OwnerKey owner}`** is transient, C++ only.
  A pointer accessor returns a full `PointerRef`, including the owner. That's how
  `canonicalize` finds the innermost block behind a hop such as `data`.
- **`ApiStruct`, `ApiMember`, the builder and `ApiRegistry`** (a Runtime extension):
  - `keyOf` is mandatory;
  - a `refine` must declare its candidates;
  - `fromBinding<T>()` builds members only.
- **`Context`:**
  - the Runtime, the Library, the executor, an optional path prefix, and `root()`;
  - no sticky error of its own: it is the Runtime's.
- **Resolver:**
  - `resolve`, `getValue`, `setValue` (the five steps of §5, plus an epoch bump for
    Pointer members), `getMeta` and `getValues` (batched; it is also bound);
  - edit-distance suggestions for unknown members;
  - `active`/`length` as collection pseudo-members.
- **`canonicalize(ctx, Path) -> Result<StableRef>`:**
  - roots at the innermost owning block;
  - indices become `keyOf` keys, and `active` resolves to a key;
  - component and enum-key accesses become the whole member.
- **ChangeBus:**
  - `notify(owner[, member[, index]])`, plus the structural epoch;
  - watchers hold a `Path` and a callback, and `watch` returns a `ScopedConnection`;
  - `flush()` re-resolves each watcher, matches it against the changes, and diffs values
    bitwise.
- **Tests:**
  - every grammar form;
  - a canonical path surviving a change of `active`;
  - the hop through `data`;
  - read-only, clamping and the update hook;
  - aliases firing watchers;
  - NaN not refiring a watcher;
  - a dangling `StableRef` failing cleanly.

## P1e: `command/`
- **`Command`**, an interface with these slots:
  - `canRun(Context&) -> Refusal`;
  - `exec(Context&) -> Status`;
  - `undo(Context&)`;
  - optional `redo(Context&)`;
  - `memSize() -> uint64_t`;
  - `discard()`;
  - optional `foldKey()` and `foldFrom(Command)`.
- **`CommandDef`:**
  - id, uiName, description, icon, flags (Private, NoUndo, UndoBarrier, Interactive);
  - input and output `PropertyDef`s;
  - the implementation `TypeId`. A def with the Interactive flag whose table lacks
    `redo` is rejected at registration;
  - a factory. It is a C++ callable for native commands. For foreign commands it is a
    `CommandFactory` foreign entry that returns an opaque foreign handle, and `bind/`
    wraps that in `Owned<Command>` with a deleter that releases the JS object.
- **`CommandExecutor`:**
  - `run(ctx, def, inputs)`, which checks the sticky error afterwards;
  - **`runSub` is an executor function** (`ctx.executor().runSub(ctx, cmd)`), so `api/`
    doesn't depend on `command/`;
  - **a failed `exec` undoes its sub-command group in reverse**, then returns the error.
    Nothing reaches the history.
- **`UndoHistory`** is a C++ interface. `LinearHistory` implements it:
  - a cursor, with redo truncation that calls `discard()`;
  - a memory budget, measured with `memSize()`;
  - barriers;
  - folding only at the cursor, when nothing has been undone since the push;
  - `rerunHead(newInputs)`: on failure it re-executes the old inputs. If that also
    fails, it drops the head and reports both errors.
- **Interactive protocol:**
  - `beginInteractive`, `commit` and `cancel`;
  - every other `exec` returns Busy while one is open;
  - **undo while one is open only cancels it**: it doesn't also undo the head;
  - `collect()` returns Busy.
- **`SetPropertyCommand`:** canonical path, whole-member old and new values, and a fold
  key of the canonical path plus `gestureId`.
- **Undo strategy helpers:** Custom, and Delegated. Snapshot gets its interface now and
  lz4 in P2.
- **Block commands:**
  - create, which records its `BlockId`s; redo uses `reviveAs`;
  - delete, with the cascade preview;
  - attach and detach.
- **`LastUsedStore`**, exposed as `commandDefaults["mesh.extrude"].dist`, with
  `serialize()`/`load()`.
- **Invocation:** `invoke(ctx, "mesh.extrude(dist=1.0 mode='X')")`.
- **Tests:**
  - undo/redo sequences, fold rules, barriers and the memory budget;
  - a failed `exec` rolling back its sub-commands;
  - a failed `rerunHead`;
  - interleaving: a drag plus a set returns Busy; a drag plus undo only cancels;
  - create, undo, redo keeping ids;
  - the delete cascade;
  - the LastUsed round trip;
  - a purge command that collects.

## P1f: bindings, WASM and the TS spike
1. The emsdk install script and the `wasm-st`/`wasm-mt` presets. Re-run the gate.
2. The native suite as `wasm-st` under node.
3. **C2** (a litestl PR):
   - implementable TS interfaces in the generator;
   - a foreign-dispatch hook in `IBindingBackend`, with call and release. It is
     implemented for Emscripten (`EM_JS`), for litestl's WASI loader (an `lstl` import, so
     litestl's own conformance suite covers it), and for N-API;
   - the TS half, which:
     - builds argument views from the slot descriptors;
     - copy-constructs the return into `ret`;
     - disposes temporaries;
     - catches JS exceptions as an error Status, except `RuntimeError` and `ExitStatus`
       (an abort inside C++ called from TS), which it rethrows;
     - wraps each dispatch in `stackSave`/`stackRestore`.
4. **`bind/`:**
   - registration for `Context` (get, set, `getValues`), Status, Refusal, PropValue and
     the executor;
   - `implement` / `CommandFactory`;
   - the Emscripten `ForeignDispatcher`.
5. **The spike:** a `node:test` suite under `tests/wasm/` against the `wasm-st` module. It
   does the four exit-criteria steps, including `class TsCommand implements Command`
   run through the executor and undone.
6. **TA1:** the test app loads the module (`-sENVIRONMENT=web`), and drives the same steps
   over CDP.

## Order of work
1. P1a props and P1b parsers, including the `IdDomain` and Runtime-extension changes in
   `base/`.
2. P1c lib, then P1d api, then P1e command.
3. P1f: the toolchain, C2, `bind/`, the spike, TA1.
4. Feature reviews: props and parsers after step 1; lib, api and command after step 2.
5. End of P1: sccache, faberforge's `tools/sccache-wrapper`, and the parallel phase.

## Risks
- **Foreign dispatch cost** (2.3 µs per call in the prototype, unmeasured since): watch
  `foreachRef` from TS providers during GC.
- **Scope:** P1 is the largest phase. If it slips, TA1 moves to P2 without blocking P2,
  P3 or P5.

## Implementation notes
### P1a and P1b (2026-11-21)
Where the code differs from the plan above:
- **Parse errors** are `Result<T>` plus an optional `size_t *errorOffset` out-parameter,
  not a `ParseError` struct. The message names the offset too.
- **`parse` is its own module** (`coredcc_parse`), not part of `api/`, so `command/`'s
  invocation parsing doesn't need the resolver.
- **Metadata:** no `VectorMeta`. `NumericMeta` applies to each component of vectors,
  `Quat` and `Mat4`. `TargetMeta {StructId}` was added for Pointer and Collection.
- **An infinite soft-range bound** means the hard range's bound, so a def can set
  `range()` without `softRange()`.
- **`coerce` additions:** Bool from Int 0 or 1. A flag key set is a string, `"X|Z"`
  (`""` is no flags, and an empty piece is an error).
- **Invocation tuples hold scalars only.** They don't nest: litestl `Vector` stores
  elements inline, so it can't hold its own type, and flat tuples leave the fuzzer no
  recursion depth to exhaust. A Mat4 is written as 16 numbers.
- **`PathCache`** is indexed by the text's hash. A collision between two texts leaves
  the second uncached, which is correct, only slower.
- **Suggestions** use optimal string alignment distance, so a swap of adjacent letters
  (`SHRAP`) counts as one edit.
- **Not done yet:**
  - `PropValue`'s binding is deferred to P1f, with the rest of `bind/`.
  - The wasm-st pre-commit gate waits for the toolchain (P1f).
- **Fuzzing:**
  - About 20% of the generated inputs are valid, so the round-trip property is
    exercised.
  - At `COREDCC_TEST_SCALE=20`, 800,000 inputs pass in 6 s.
  - A planted bug in `Path::toString` (index + 1) failed on the first valid input.

### P1c (2026-11-21)
- **Provider slots take `const Datablock &`, not `BlockHandle`.** Handles are per
  Library, and providers are per Runtime, so a handle alone can't name a block to a
  provider. The record gives the provider the block's id, type and `clientData`.
- **`queryInterface` returns `QueryAnswer {TypeId implType; void *self}`**, which the
  library looks up in the interface registry. That serves native and foreign answers
  alike. It doesn't return a `BlockHandle`.
- **`RefVisitor` is a concrete bound class** with a `visit(RefCell&)` method, not an
  interface. A TS provider calls it like any bound method.
- **`serialize`, `deserialize`, `duplicate` and `rekey` move to P2** entirely, signatures
  included. They are optional slots, so adding them later breaks no provider.
- **No per-BlockType query cache.** The registry lookup is already one hash lookup.
- **Every non-Live record is a GC root** (InHistory, Detached and Placeholder blocks are
  held by the history, the host or a file).
- **Sticky errors:**
  - `Library::foreachRef` reports a provider failure without consuming the error.
  - `collect()` takes the error and returns it, and refuses to start while one is
    pending.
- **Attachments are minimal:** attach, detach and lookup, with a destroy callback. The
  attachment type registry (copy callback, duplicate policy, serializer) comes with
  library IO in P2.
- **`Library` is not a Runtime extension.** Hosts create Libraries and destroy them
  before their Runtime.
- **GUID source:** `Runtime::setGuidSource`. The default is `rand_s` (RtlGenRandom) on
  Windows and `getentropy` elsewhere.
- **Bindings:** ids, `LocalId` and `BlockId` are bound (`base/bindings.cc`), and so are
  `RefCell` (its target), `RefVisitor` and `QueryAnswer`. `Datablock` is bound without
  members for now.

### P1d (2026-11-21)
- **`PointerRef::owner` is an `OwnerKey`**, set by whichever accessor crosses into
  another block or runtime owner. A nested struct inside the same owner leaves it
  null, and its members keep the key path from that owner.
- **`Context`** holds the Runtime, an optional Library, the root `PointerRef`, a path
  prefix and a slot for the P1e executor.
  - The UndoHistory comes with P1e.
  - The ToolManager and scene come with P3.
- **Runtime owners** register by name in `ApiRegistry` (`addRuntimeOwner`). The id is
  `TypeId::of("owner:<name>")`, so a hash collision with another name is caught by the
  Runtime's id registry. This resolves review item D9.
- **The Context-aware dynamic hook** is `ApiMember::dynamic(def, self, ctx)`. It runs
  after any props-level hook, and `getMeta`, `setValue` and `coerce` all see the
  result. This resolves review item D3.
- **Collection pseudo-members:**
  - `length` and `active` are reserved member names, checked at registration.
  - `.length` reads as a read-only Int.
  - Any other name after a collection is NotFound, with those two offered as
    suggestions.
- **Component and enum-key paths:**
  - They read as a Float or a Bool.
  - A write reads the whole member, changes one part, and goes through `coerce` and
    the full write path.
  - An enum key can be set but not cleared; a flag key can be both.
- **Pointer writes** take a Pointer value and resolve it to its target. The target's
  struct must match the member's target or one of its candidates. The write calls
  `Library::noteRefsChanged`, which bumps the library's reference epoch.
- **ChangeBus:**
  - Changes are `(owner, member name)`. An empty name means the whole owner. The
    `index` part of the spec isn't used yet, because the value diff makes it
    unnecessary.
  - A watcher stores its `Context *`, and the Context must outlive it.
  - Only watchers whose last resolution touched a changed member are re-read, unless
    the structural epoch moved.
  - A watcher whose path currently fails is re-read at every flush that has changes.
  - Changes posted during a flush wait for the next one.
- **Deferred:**
  - `fromBinding<T>()` moves to P1f, where `bind/` reads litestl descriptors.
  - `getMeta` still returns a copy of the def (review item D1).
  - Display units and the subtype/unit agreement check (D12) move to P3, where
    scene settings exist.

### P1c/P1d review corrections (2026-11-21)
`docs/reports/feature-report-lib-api-2026-11-21.md` has the details.
- **The walk's rules for canonical paths:**
  - A struct crossed into in another owner must be that owner's root, or the walk is
    Unsupported.
  - A settable pointer inside its owner needs `PointerAccessor::address`.
  - Fixed pointers and collection items append to the key.
- **Pointer type checks accept subtypes** (`ApiRegistry::isA`).
- **Detached blocks contribute their provider's refs**, so their targets survive GC.
- **`CollectionAccessor::setActive` (§5) moves to P3**, with the Scene. Until then,
  code that changes `active` bumps the ChangeBus epoch. Library changes refire watchers
  through the Library epoch.
- **`Library::resolve(RefCell)` is now `resolveRef`.**

### P1e (2026-11-21)
- **`canRun` returns a Status**: ok, or the refusal with its reason. Status is bound
  (`base/bindings.cc`) and already carries a message, so it does the job a separate
  `Refusal` type would.
- **Slots read their bags through the Context.** The executor sets `ctx.inputs()` and
  `ctx.outputs()` around every slot call and restores the outer ones after, so
  sub-commands nest. Context is bound without members so slots can take it.
- **`foldKey` is a `uint64_t`**, and 0 means "never fold". SetPropertyCommand hashes
  the canonical ref with the gesture id.
- **The interactive protocol lives in the executor.** The plan put
  `beginInteractive`/`commit`/`cancel` on `UndoHistory`.
  - The executor holds the open command and pushes it on commit.
  - Host adapters that need a transaction held open for the whole drag (Unreal) get
    begin/end hooks on `UndoHistory` with them in P6.
  - While a command is open, `Library::beginNoCollect` makes `collect()` Busy.
- **`run` from inside a slot is refused**, with "use runSub" in the message.
- **Redo without a `redo` slot** drops the old sub-commands, then runs `exec` again,
  which records new ones.
- **A failed redo** drops that command and everything after it from the history.
- **Undo reports errors through the sticky error.** Its slot returns void, so the
  executor's `undo` and `redo` return the sticky error when one is set.
- **The redo of a create uses `restoreFromHistory`, not `reviveAs`.** Undo keeps the
  record and its data InHistory. `reviveAs` is for redo after the data was dropped:
  snapshot undo and library IO, in P2.
- **The block commands are** `core.block.delete` (with `deletePreview`, the transitive
  set of users whose non-null `Ref` would break) and `core.block.purge`. CreatedBlocks
  is the helper for client commands that create blocks.
- **Attach and detach commands move to P2.** A generic attach needs the attachment
  type registry (copy, destroy, serializer), which comes with library IO.
- **The undo strategies are** `DelegatedUndo` (callbacks) and the `SnapshotProvider`
  interface, for blobs of bytes. lz4 comes in P2.
- **Value literals** are `formatLiteral` and `parseLiteral` (`command/literal.h`). They
  cover review item D5, except for Pointer values, which have no literal form yet.
  - LastUsed files are one invocation per line, and `load` skips commands and inputs
    that no longer exist.
  - `commandDefaults["id"].input` is registered on demand, as one ApiStruct per command
    (`CommandDefaults:<id>`), with the NoUndo flag.

### P1e review corrections (2026-11-21)
`docs/reports/feature-report-command-2026-11-21.md` has the details.
- **Reentrancy:** only `exec` may call back into the executor, and only through
  `runSub`. Every other slot call is counted, and nested executor calls are refused.
- **Input bags** are conformed to the def (`def.newInputs()` plus the caller's set
  values); a mismatch is InvalidArgument.
- **Sticky errors** are collected after every slot call. A stray one fails the next
  top-level call, naming it.
- **History pins (§8a)** now exist: SetProperty pins Pointer targets, and
  `CreatedBlocks::add(lib, h)` pins created blocks, until the discard.
- **Deviation:** `core.setProperty` keeps its path and value in the impl, not in def
  inputs. rerunHead refuses runInstance records and Interactive ones.
- **Deviation:** rerunHead is on the executor (it works with any UndoHistory), and
  `tryFold` is part of `push`. A failed re-run redoes the old record and puts it back.
- **Deviation:** CreatedBlocks holds handles until P2 makes them `BlockId`s.
- **Executor binding:** a Context is bound to the executor only for the length of
  each call. `invoke` and `setValueUndo` need `ctx.setExecutor`.
- **Deferred:** scripting interactive commands (P3); host-adapter hooks (P6);
  snapshot undo (P2); registry removal and the binding surface (P1f).

### P1f, steps 1-2: toolchain and the native suite under node (2026-11-21)
- **`tools/install-emsdk.mjs`** installs the pin from `tools/emsdk.json` into
  `build/deps/emsdk`. `--adopt DIR` moves in an existing checkout (after checking its
  commit and version), and `--check` reports the state.
  - From a sandboxed shell the move can be refused with EPERM although nothing holds
    the files. Moving the directory with PowerShell works.
- **Presets:** `wasm-st` and `wasm-mt` (`COREDCC_WASM_THREADS`), each with `-debug` and
  `-release` build and test presets.
  - Under Emscripten the root CMakeLists sets `BUILD_WASM`, `-DWASM` and the link flags.
  - Tests also get `-sENVIRONMENT=node`, `-sEXIT_RUNTIME=1` and the
    `tests/support/node_env.js` pre-js.
  - `EXIT_RUNTIME` is needed: without it, main's return value never reaches node's exit
    code, and failing tests pass.
- **Gate re-run:** emcc 5.0.6 builds every coredcc and litestl TU with `-Werror`. All 30
  tests pass on both `wasm-st` and `wasm-mt`. `compile_embed` is Windows-only.
  `wasm-mt` warns `-Wpthreads-mem-growth`, as expected.
- **The gate found one portability bug:** a moved-from `std::function` (which is
  `litestl::util::function`) keeps a small target under libc++, so `DelegatedUndo`'s
  defaulted move released its step twice. Moves now clear the source explicitly.
  - Don't rely on a moved-from `std::function` being empty.

## C2 design: foreign dispatch in litestl (2026-11-21, revised after its pressure test)
The litestl half of "implementing in TS" (architecture §2a, "Bindings and foreign
implementations"). litestl knows nothing of coredcc: it calls a method descriptor on a
host object named by a handle. coredcc's `ForeignDispatcher` (in `bind/`) is a thin
adaptor over it.

### C++: `binding/foreign.h`
- **Handles:** `foreign::Handle` is a `uint32_t` naming a JS object in the host's
  table. 0 is none.
  - Handles are issued in increasing order and never reused, so a stale handle fails
    as unknown rather than reaching another object.
- **`bool foreign::call(Handle h, const types::Method *m, void **args, void *ret,
  util::string *error)`:**
  - `args` and `ret` use the thunk ABI;
  - on success a non-void `ret` has been constructed;
  - on failure `ret` is unconstructed and `*error` holds the message.
  - It refuses, before calling the host:
    - reference returns and `ReturnPolicy::Owned` returns;
    - a call off the main thread (`emscripten_is_main_runtime_thread()`; the N-API
      host records its thread). It doesn't proxy: the main thread may be blocked
      waiting for the caller.
- **`void foreign::release(Handle h)`** drops one count. Contract: a handle given to C++
  carries one count, and C++ releases it exactly once, failure paths included.
- **`foreign::setHost(const Host *)`** is per thread (`thread_local`). The N-API runtime
  installs its host for its env's thread, and removes it in an env cleanup hook.
- **The default host** (none set):
  - Emscripten: `EM_JS` calling `Module['__lstlHooks'].foreignCall` and
    `.foreignRelease`;
  - litestl's WASI build: imports `lstl.foreignCall` and `lstl.foreignRelease`;
  - native: `call` fails with "no foreign host", and `release` does nothing.
  - A missing hooks object makes `call` fail and `release` a no-op, which also covers
    teardown.

### TS runtime: `typescriptRuntime/foreign.ts`
- **Per module, not per manager:**
  - `createWasmHelpers` creates `__lstlHooks` on the module object before copying it,
    and never replaces it.
  - `instantiateWasi` creates it and binds trampolines,
    `(...a) => hooks.foreignCall(...a)`.
  - The hooks object holds the one `ForeignObjects` table and the dispatch, shared by
    every manager of that module.
- **The table:** `register(obj)` returns the object's handle and adds a count. A new
  handle is issued after the count reaches 0. `release(h)` drops a count.
- **Poisoning:**
  - A `WebAssembly.RuntimeError`, a `WasiExit`, or anything named `ExitStatus` during a
    dispatch is rethrown, and the module is marked poisoned.
  - The C++ frames the rethrow unwinds never pop their stack or restore their state, so
    nothing is safe afterwards. Foreign calls then fail fast, and calls into C++
    (`MethodType.invoke`, `constructTo`) throw.
  - No `stackSave`/`stackRestore`.
- **The dispatch,** `foreignCall(handle, method, args, ret, error) -> 0 | 1`:
  - It refreshes the heap views on entry, and again after the JS method returns,
    because memory can grow without the patched `grow` seeing it (shared memory, other
    threads).
  - **Finding the method:** the descriptor comes from a cache keyed by pointer. The method
    is `obj[jsName]`, a function that isn't `Object.prototype`'s. Anything else is
    missing.
  - **Arguments,** by parameter type:
    - scalars, enums and bools;
    - `String` and `CString`;
    - vectors copied (`Uint8Array` for `uint8`);
    - pointers as a wrapper of the pointee, or `null`;
    - other structs, by value or reference, as non-owning wrappers.
    - Every non-owning wrapper created while the dispatch runs is tracked, including
      members reached through an argument, and invalidated afterwards (`ptr = 0`).
      Generated accessors throw on `ptr === 0`.
    - Wrappers of const-reference arguments are read-only: their setters throw.
  - **Converters,** per struct type name: `setForeignConverter(typeName, {arg?, ret?})`.
    - `arg` maps an argument wrapper to what JS sees (coredcc: an `IfaceRef` back to
      its JS object, for `foldFrom`).
    - `ret` maps the JS return to what is written (coredcc: `undefined` to an ok
      `Status`).
  - **The return,** after its converter:
    - scalars, enums and bools;
    - `String`, built in place;
    - vectors of scalars from arrays or typed arrays;
    - a struct from a bound object of exactly that type, through its copy constructor;
    - a pointer from a bound object, or `null`.
    - A thenable is an error: slots are synchronous.
    - Storage already constructed when a later step fails is destroyed before returning
      the error.
    - There are no plain-object struct returns: the setters can't check types on WASM,
      and embedded structs can't be set.
  - **Exceptions** become the error message (`<jsName>: <message>`, or `String(e)` for a
    non-Error).
- **`IBindingBackend`** gains:
  - `foreignHandle(obj): number`;
  - `foreignCount(): number`;
  - `setForeignConverter(typeName, converters)`.

### N-API
- The same rules, per env: a handle table of `napi_ref`s, and one handle scope per
  call.
- Vector arguments are copied, not live views.
- Struct wrappers are tracked and invalidated, and const ones are read-only.
- Converters are JS functions held by reference.
- **Exceptions:**
  - If an exception is already pending, `call` fails without clearing it.
  - Otherwise a pending exception after `napi_call_function` is taken and becomes the
    message.

### Generator
- **For an `isInterface` struct**, the bound `Probe` interface is emitted as before, and
  `ProbeImpl` alongside it for implementers:
  - no `[Symbol.dispose]` and no `new(...)`;
  - `name?(...)` for optional methods;
  - `bigint` for int64.
- A JS implementation is a `ProbeImpl`, not a `Probe`, so the compiler rejects passing
  one where C++ expects a bound interface value.
- **Deviation from §2a:** no `Readonly<>` for const references. A mapped type drops the
  construct signatures every bound struct carries, so a `Readonly<Vec2>` can't be
  passed on. The read-only wrapper enforces it at run time instead.

### Tests
- **The fixture** gets the interface `fixture::Probe`. Its methods cover a float, an
  int64, an enum, a string in and out, a struct by const reference and by value, a
  struct return, a nullable pointer, a `uint8` vector in and out, and an optional
  method.
- **Fixture functions** call a handle's methods through `foreign::call`, record the last
  error, and release handles.
- **The conformance suite**, the same on both backends:
  - `class JsProbe implements ProbeImpl`;
  - round trips of every type;
  - a thrown Error and a thrown string;
  - a missing optional method;
  - a wrong return type, with no leak;
  - a thenable return;
  - wrappers invalidated, including members;
  - a const wrapper's setter throwing;
  - handle counts and non-reuse;
  - converters.
- **Not covered by litestl:** the EM_JS path. coredcc's spike covers it.

### Left to coredcc's `bind/`
- `Status` needs bound constructors, plus the `ret` converter.
- `QueryAnswer` and `void *` returns (`queryInterface`, `BlockProvider::data`) aren't
  implementable from TS. `bind/` decides between a JS-side adaptor and refusing such
  interfaces for foreign tables.
- **The spike module's link flags:** `--no-entry` (or no `EXIT_RUNTIME`), and
  `-sENVIRONMENT=node` or `web`.
- `IfaceFlags::ThreadAffine` is never checked. `foreign::call`'s main-thread check
  covers foreign tables.

### Pressure test (2026-11-21)
A fresh agent got the design and the code, but not the author's reasoning. It ran
experiments with emcc 5.0.6, node 24, tsc, clang-cl and a small N-API addon.
- **Survived:**
  - EM_JS reaches a property set on the module after instantiation, with and without
    MODULARIZE (write it as `Module['__lstlHooks']` for closure builds);
  - the hooks object is shared through `createWasmHelpers`' shallow copies;
  - WASI imports can be late-bound through trampolines;
  - copy-constructing into `ret` works (write `ret` before invalidating wrappers);
  - N-API catch-and-clear works, nested too;
  - reentrancy from a hook into exports works;
  - one count per `Owned<I>` fits coredcc;
  - `class X implements` works with optional methods and bigint.
- **Broke, and what changed:**
  - `stackSave`/`stackRestore`: not exported, and probing them aborts -O0 modules.
    Restoring at the hook doesn't restore the unwound frames' stack either → poison
    the module instead.
  - The rethrow list missed `WasiExit` → added.
  - Zeroing `ptr` didn't stop member accessors, and missed wrappers made during the
    call and N-API's live vector views → track every wrapper, accessors throw, copy
    vectors.
  - Generated types: bound optional slots became type errors, `using` failed,
    `Readonly<>` blocked passing values on, and JS implementations type-checked as
    bound values → a separate `ProbeImpl`, and no `Readonly<>`.
  - Plain-object returns couldn't be type-checked on WASM, and leaked on failure →
    dropped. Failed returns destroy what they constructed.
  - Two managers in one module → one table per module.
  - Threads: EM_JS on a pthread has no hooks, and N-API off-thread is undefined
    behaviour → `call` refuses off the main thread.
  - `setHost` was process-global across N-API envs → per thread, with env cleanup.
  - N-API with an exception already pending swallowed it → checked first.
- **Added:**
  - refreshing heap views;
  - N-API handle scopes;
  - rejecting thenables;
  - refusing reference and Owned returns;
  - teardown no-ops;
  - excluding `Object.prototype`;
  - converters for coredcc's `Status` and `foldFrom`;
  - `Uint8Array` vectors.
- **Recorded, not fixed here:** `binding.ts`'s `bindingCache` is module-global and keyed
  by pointer, so two wasm instances in one realm share descriptors. This matters for
  TA1 and for tests that re-instantiate.

### C2 implementation (litestl #9, merged 2026-11-21 as `9e5ca5d`)
- **Implemented as designed:**
  - `binding/foreign.{h,cc}`;
  - `typescriptRuntime/foreign.ts`;
  - `binding/napi/napi_foreign.cc`;
  - the generator's `<Name>Impl`.
- **The conformance suite** has seven foreign-dispatch tests.
  - WASI passes 23/23.
  - N-API passes 22/22 on vs-clang and msvc. Its one skip is the WASM-only trap test.
  - The trap test calls a bound function that traps from inside a foreign call, then
    checks that later calls and constructors throw "poisoned". It runs last, because it
    poisons the module.
- **Details settled while implementing:**
  - `<Name>Impl` writes nullable pointers as `| null`.
    - `argIsNullable` is what marks a pointer parameter nullable. Pointer parameters
      are non-null by default.
  - Vector arguments are `Uint8Array` for `uint8`, and `T[]` otherwise. Vector returns
    accept `ArrayLike`.
  - A pointer to a non-struct is refused as a parameter type on both backends.
  - N-API keeps object identity in a JS `WeakMap`, so JS objects aren't mutated.
  - The N-API host returns errors and uses no C++ exceptions, like the rest of litestl.
- **Gap, still to fix (a follow-up litestl PR):**
  - N-API was meant to fail *without clearing* an exception that was already pending
    when a call starts. `napi_foreign.cc` instead takes that exception as the call's
    error.
- **Not covered:** the Emscripten EM_JS host. coredcc's wasm-st gate compiles it. The
  spike (step 5) is its first run.

## P1f step 4: `bind/` design (2026-11-21, revised after its pressure test)
`source/coredcc/bind/` (target `coredcc_bind`) is what scripts see:
- one BindingManager;
- facades over the C++ API, in shapes the binding system can carry;
- foreign `Command` implementations.

It links every other module. Nothing links it except hosts and tests.

### Prerequisites in `command/` (bugs the pressure test found; native code hits them too)
- **Redo must see the sticky error.**
  - A failed foreign slot returns a value-initialized result (an *ok* `Status`), and
    only sets the sticky error.
  - `CommandRecord::redo` and `LinearHistory::redo` branch on the returned status, so
    a redo that failed this way advanced the cursor. The next undo then undid a command
    that was never redone.
  - **Fix:** `CommandRecord::redo` folds the Runtime's sticky error into its status after
    each slot call (`exec` for redo-by-exec, `redo`), before deciding to roll back.
    `undo` can't be rolled back, so it is unchanged: its error still reaches the caller
    through the executor's `collect`.
- **Registration checks run before anything is registered.**
  - `CommandRegistry::add` checked these after the implementation was already
    registered, so a failed `add` left the implementation's table behind, and a retry
    then failed with "already implements":
    - an Interactive command without `redo`;
    - `foldKey` without `foldFrom`, or the reverse;
    - an id hash collision.
  - **Fix:**
    - `checkDef` also refuses a colliding id.
    - A new `CommandRegistry::checkSlots(def, hasSlot)` makes the slot checks from a
      predicate. `add` calls it with the table.
    - `addNativeCommand` calls it before `registerImpl`, from the Impl's descriptor.
    - The foreign path calls it from the slot names.
  - `addForeign` still registers the impl's TypeId name before it fails. That entry is
    harmless, and is left alone.

### What the binding system can't carry, and the facade rule
- These aren't bindable: `std::string_view` parameters, `Result<T>`, `std::optional`,
  `std::span`, `Owned<I>`, and `Vector<string>` arguments on WASM. (`buildArgs` fills
  vectors with `writeScalar`.)
- Facades are free functions in `coredcc::script`, with JS names `coredcc::<name>`.
  They take `const litestl::util::string &` and return one of:
  - a `Status`;
  - a value struct;
  - a result struct `{Status status; T value;}`.

  They never set the sticky error.
- **Misuse from a script returns an error. It never aborts.** Methods whose misuse
  aborts (`PropValue::as*` on the wrong type, `component(i)` out of range, `Status::error`
  with `Ok`, an unchecked bag index) are not bound. Checked facades replace them.
- **Result structs:** `BoolResult`, `IntResult` (int64), `FloatResult` (double),
  `StringResult`, `ValueResult` (a PropValue) and `BagResult` (a PropertyBag).
  - Each is bound with its two members and a copy constructor.
  - The TS layer (step 5) turns a failed status into an exception.
  - The agent compiled them under clang-cl /W4 /WX with no offsetof warning.

### Bound types (additions)
- **`StatusCode`:** an enum.
- **`Status`:**
  - a default constructor (ok);
  - `code()`, added to `ok` and `message`;
  - the facade `statusError(code, message)`, which maps `Ok` to InvalidArgument instead
    of aborting.
- **`PropValue`:** a copy constructor, `type()` (the bound enum `PropValue::Type`), and
  `componentCount()`. Checked facades do everything else:
  - **makers:** `makeBool`, `makeInt`, `makeFloat`, `makeString`, `makeVec2`, `makeVec3`
    and `makeVec4`;
  - **readers:**
    - `readBool`, `readInt`, `readFloat` and `readString` return result structs. The
      wrong type is InvalidArgument.
    - `readComponent(v, i)` returns a FloatResult. An out-of-range `i` is OutOfRange.
  - Pointer and Mat4 values can be read as a type and a component count, but are not yet
    made from TS.
- **`PropertyBag`:** read-only facades only. A script that could write to a bag could
  rewrite a recorded command's inputs, which LastUsed and rerunHead read later.
  - `bagGet(bag, name)` returns a ValueResult. A missing name is NotFound.
  - `bagSize(bag)`.
  - `bagName(bag, i)` returns a StringResult, with a range check.
- **`Context`:** no bound accessors. A command's bags are reached by facades instead:
  - `inputGet(ctx, name)` returns a ValueResult;
  - `outputSet(ctx, name, value)` returns a Status;
  - `outputGet(ctx, name)` returns a ValueResult.

  Outside a command slot they return FailedPrecondition.
- **Also added to the manager:**
  - `Command`, so the generator emits `CommandImpl`;
  - `ForeignFactory`;
  - `CommandDef`, as an opaque, owned builder.

### Facades over Context (the spike's steps 1-3)
- **`resolvePath(ctx, path)`** returns a Status. It is ok if the path resolves, and
  otherwise the resolver's error with suggestions. This is step 1, "resolves a path".
  `canonicalize`'s StableRef prints only a debug form, so it isn't used.
- **`getValue(ctx, path)`** returns a ValueResult.
- **`setValue(ctx, path, value)`** calls `setValueUndo`, so the set is undoable. It
  needs an executor on the context.
  - At top level it is a command in the history.
  - Inside a command's exec it becomes a sub-command (`runSub`), undone before its
    parent.
- **`writeValue(ctx, path, value)`** calls the raw `api::setValue`, which isn't
  recorded. A command's exec and undo use it on state they capture themselves.
  - Don't set the same member through both `setValue` and `writeValue` in one command.
- **`invoke(ctx, invocation)`** returns a BagResult with the outputs, through the
  invocation parser (`"mesh.extrude(distance=2)"`).
- **`undo(ctx)` and `redo(ctx)`** go through `ctx.executor()`. Without an executor
  they return FailedPrecondition.
- **Reentrancy guard.** `ForeignBindings` counts the foreign calls in progress (the
  dispatcher increments it).
  - While the count is above 0, `invoke`, `undo` and `redo` return Busy. So does
    `setValue`, unless the executor is running a command (inside exec).
  - Without the guard, a script's `discard`, run when the host clears the history,
    could push into the history it is being cleared from. That hits `HistoryCall`'s
    check and aborts.
- **No `gestureId`**, so TS can't fold drags yet.
- **Deviation:** `getValues` (several paths at once) is left out. Nothing in P1 needs
  it.

### Foreign commands
- **`ForeignBindings`** is a Runtime extension. It holds:
  - the one `ScriptDispatcher` (a `ForeignDispatcher`);
  - the foreign-call depth;
  - the factory handles it has been given, which it releases when it is destroyed.
- **`ScriptDispatcher::call(table, self, slot, args, ret)`:**
  - `self` holds the instance's foreign handle, as `void *`.
  - It raises the depth, then calls `litestl::binding::foreign::call(handle,
    table.ifaceDesc->methods[slot], args, ret, &err)`.
  - On failure it returns Internal with `err`. `FwdCall` sets the sticky error, and the
    executor folds that in: through `collect`, and now through `CommandRecord::redo`.
- **`ForeignFactory`** is an interface struct (`isInterface`) with one slot,
  `uint32_t make()`.
  - `make` returns a new instance's handle, with a fresh count (`foreignHandle(new
    Cls())`). The TS layer writes `make`, so no script returns a cached handle.
  - It is called only through `foreign::call`, never through a table.
- **`newCommandDef(id)`** returns an owned CommandDef. Facades build it:
  - `defUiName(def, s)`;
  - `defFlags(def, uint32)`;
  - `defInput(def, name, PropValue default)`, which adds a PropertyDef of the default's
    type (Bool, Int, Float, String or Vec2-4). Other types are InvalidArgument.
- **`registerForeignCommand(ctx, def, factoryHandle, slots)`.** `slots` is one string
  of space-separated slot names; the TS layer intersects the class's methods with the
  Command slots.
  - **C++ always takes the factory count**, and releases it on failure. This is the C2
    contract: a handle given to C++ is released exactly once.
  - **In order:**
    1. `checkDef`, and `checkSlots` from the names.
    2. Refuse `foldKey`/`foldFrom`: folding isn't supported from TS yet.
       - The native fold protocol downcasts `next`, which a foreign command can't do.
       - A native `next` read as a handle could alias a live JS object.
    3. Refuse an interface with slots that can't be implemented from TS: `void *` or
       `QueryAnswer` in a parameter or return (`foreignImplementable(desc)`, which is
       general). Command has none.
    4. `registerForeign<Command>(rt, implName, dispatcher, nullptr, slots)`.
    5. `def.impl(implName, factory)`, then `CommandRegistry::add`. Nothing can fail
       here any more except allocation.
- **The factory**, per run:
  - It calls `make` on the factory handle.
  - A failure, or a returned handle of 0, fails the run with a message. A 0 handle is
    never released.
  - Otherwise it returns `Owned<Command>` holding the table and the new handle. Its
    deleter calls `foreign::release(handle)`.
  - The agent's prototype showed exactly one release per instance on every path:
    - refusal, failed exec, fold;
    - redo truncation, budget eviction;
    - cancel, rerunHead;
    - history teardown.
- **Threads:** `foreign::call` refuses off the main thread (Emscripten) and off the
  host's thread (N-API).
- **Teardown order:** the host destroys the executor and the history before the
  Runtime.
  - Records hold `Owned<Command>`s whose tables, dispatcher and handles belong to the
    Runtime and its extensions. Destroying the Runtime first is a use-after-free.
  - A script's `discard` or `undo` that throws during a host-initiated `history.clear()`
    leaves a sticky error. The host takes it (`rt.takeStickyError()`). If it doesn't,
    the next top-level executor call reports "an earlier error was never collected".

### Hosts and the manager
- `coredcc::script::bindings()` returns the BindingManager with every bound coredcc type
  and facade.
  - It is built once, on first use, inside a `PermanentGuard`, and never changes after.
  - Like the binding descriptors it collects, it is write-once static data. It is not a
    registry: registries live in a `Runtime`.
- A WASM module exports `CDCC_GetManager` (`bind/wasm_entry.cc`, compiled only under
  Emscripten).
- The host-side setup (Runtime, Library, root struct, executor, history) belongs to the
  spike, in step 5. `Context`'s constructor isn't bindable.

### The TS layer (step 5, recorded here because it constrains step 4)
- `registerCommand(ctx, spec, Cls)`:
  - builds the def;
  - writes the factory's `make`;
  - intersects `Cls`'s methods with the Command slot names;
  - installs the `coredcc::Status` `ret` converter: `undefined` is ok, a thrown Error
    becomes an error result, and a bound Status passes through.
- `Cls` implements a hand-written `ScriptCommand` interface, not the generated
  `CommandImpl`. Two reasons:
  - `CommandImpl.exec` is typed to return `Status`, which rules out `undefined`;
  - `memSize` is typed bigint.

  The compiler still checks `class TsCommand implements ScriptCommand`.

### Tests (native, step 4)
- **`tests/command/`:**
  - a redo whose slot fails through the sticky error leaves the cursor in place;
  - a failed `add` (an Interactive command without redo, or a lone foldKey) leaves no
    table behind, and a corrected retry succeeds.
- **`tests/bind/test_bind_foreign.cc`** installs a fake `litestl::binding::foreign::Host`
  with `setHost`. Its "JS objects" are C++ structs looked up by handle, with methods
  selected by name. It checks:
  - registration:
    - a missing required slot is refused;
    - a fold slot is refused;
    - in both cases nothing stays registered and the factory count is released;
  - exec and undo through the executor;
  - redo with `redo` absent, which re-runs exec;
  - a failing slot is the run's error status, with the host's message;
  - a failing redo leaves the history consistent;
  - a failing `make`, and a `make` that returns 0;
  - every instance handle is released exactly once, ending in history teardown before
    Runtime teardown;
  - the reentrancy guard: a foreign `discard` calling `invoke` during `clear()` gets
    Busy, not an abort.
- **`tests/bind/test_bind_facades.cc`:**
  - `resolvePath`, `getValue`, `setValue` plus `undo`, `writeValue` and `invoke`;
  - the bag, input and output facades;
  - the checked readers on the wrong type and out of range.

  They run on the native fixture struct the api tests use.
- **The generated TS** (`coredcc_gen_ts`) is type-checked in step 5.

### Pressure test (2026-11-21)
A fresh agent got the design and the code, but not the author's reasoning. It built a
native prototype of the dispatcher, factory and registration with a fake host
(experiments A-F).
- **Survived:**
  - `addForeign` rejects a missing required slot and stores no table;
  - the table can be found by impl name, and table pointers are stable;
  - `void *self` can carry a handle (`as<T>()` is null for foreign tables, and a native
    `foldFrom` given a foreign `next` returns false);
  - a failing exec, canRun or `make` becomes the run's status;
  - `Owned<Command>` releases exactly once on every executor and history path;
  - a fake host drives everything natively;
  - executor reentrancy from exec is refused;
  - wrappers outliving a call throw;
  - string parameters, borrowed pointer returns, by-value struct returns and int64
    returns bind as described;
  - the result structs are standard layout;
  - extension teardown order is safe once the history goes first.
- **Broke, and what changed:**
  - Registration leaked a table on failed `add` checks → the `command/` checks move
    first.
  - A foreign redo failure corrupted the history (cursor advanced; a later undo ran
    twice) → `CommandRecord::redo` folds the sticky error.
  - A script `discard` during `history.clear()` could reenter and abort → the
    foreign-call depth guard.
  - `Status`'s named error constructor doesn't compile, and `Ok` aborts →
    `statusError`.
  - `Vector<string>` can't be passed on WASM → a space-separated string.
  - "Leave the count with the caller" contradicted C2 and leaked factories → C++
    always takes it.
- **Gaps closed:**
  - registration takes a `Context` (Runtime isn't bound);
  - `Command`, `ForeignFactory` and `CommandDef` are in the manager;
  - folding is refused from TS;
  - slot names are intersected in TS;
  - aborting accessors are replaced by checked facades;
  - bags are read-only to scripts;
  - `setValue` inside exec is documented;
  - a 0 handle is refused;
  - the teardown order and the host's sticky error after `clear()` are documented;
  - the TS layer's types are decided;
  - "resolves a path" uses `resolvePath`;
  - the manager is write-once static data.
- **Not verified:**
  - the generated TS type-checking, including name collisions such as the nested
    `PropValue::Type`;
  - WASM runtime behaviour (read from the code);
  - threads.

## Pressure test record (2026-11-20)
A fresh agent got the plan and the code, but not the author's reasoning. It built emsdk
5.0.6 and ran experiments in its scratch directory.

### Survived
- **The emcc 5.0.6 gate:**
  - litestl `binding.cc` and all coredcc base TUs compile with `-Werror`;
  - the base tests pass as wasm-st under node;
  - two tests pass as wasm-mt.
- litestl's TS runtime keeps its views valid when Emscripten grows memory, given
  `wasmMemory` is exported.
- The Emscripten toolchain sets the CTest emulator.
- `std::variant` with `get_if`, without exceptions, under MSVC.
- `HandlePool` quarantine, `Signal`'s `ScopedConnection`, `Binder<void*>`, `spine` having
  no upstream, and faberforge's sccache wrapper all exist.

### Broke, and what changed
- **Toolchain:**
  - WASM needs `-DWASM` and `BUILD_WASM=ON` → added to the presets.
  - `size_t` and `long` have no binder on wasm32 → banned in slots and members;
    `memSize` returns `uint64_t`; wasm-st joins the pre-commit gate.
  - The module needs `EXPORTED_RUNTIME_METHODS=wasmMemory,HEAPU8` → added.
  - `getenv` doesn't see the environment under node → a `--pre-js`.
  - The test app's st environment is `web`, not `web,worker` → fixed.
- **Layering:**
  - `bind/` can't hold the binders → binders live with their types.
  - The Runtime can't name later modules' registries → `rt.ext<T>()` and
    `registerBuiltins`.
  - `ctx.runSub` puts a command dependency in `api/` → it is an executor function.
  - `IdDomain` needs a resized name table and new tags → added.
  - A block type name can't also name its provider → `<type>#provider`.
  - The sticky error was on `Context` → it is the Runtime's.
- **Parsers:**
  - Parse errors had no offset field, and "within the input" was wrong for
    end-of-input → `ParseError`, with `0 ≤ offset ≤ len`.
  - `PathCache` returned references into a rehashing `Map` → refcounted `PathPtr`.
  - NUL truncates litestl strings → rejected.
  - `.active` and `.length` stole real member names → they are members, and the resolver
    interprets them.
  - int64 overflow in indices → errors.
- **Values:**
  - `units::parse` had no display unit → it takes one, and the round-trip tolerance
    scales.
  - NaN and -0 broke the diff and hash contract → both are bitwise.
  - Float → Int could overflow → range-checked.
  - StableRef keys couldn't express components or flag keys → whole-member
    canonicalization.
- **Library:**
  - `reviveAs` was unspecified outside Live → defined for every state, with one record
    per id.
  - Cross-library handle aliasing → `get` compares the `BlockId`.
  - A purge command could never collect → `collect()` is blocked only by interactive or
    evaluation state.
  - Calls from `destroy` had nowhere to report → reads are allowed, void mutations
    queued, creates refused.
  - A TS `foreachRef` that throws under-marks → GC aborts on a sticky error.
  - TS refs couldn't be remapped → they are bound `RefCell`s in WASM memory.
  - A Pointer set left relations stale → it bumps the epoch.
  - The audit can't catch a consistent liar → the test is renamed to the staleness case.
  - Client aliases didn't resolve old ids → normalization.
  - Raw `crypto.getRandomValues` throws on shared views → `getentropy()`.
- **Commands:**
  - The Interactive-without-`redo` check had nothing to inspect → the def carries the
    implementation `TypeId`.
  - A TS command couldn't come from a C++ factory → `CommandFactory`, plus release in
    the C2 hook.
  - A failed `exec` left its sub-commands applied → rolled back.
  - A failed `rerunHead` lost the head → old inputs are re-run.
  - Undo mid-drag could undo two steps → it only cancels.
  - Canonical paths kept the `data` hop → pointer accessors return the owner.
  - Pointer inputs had no invocation syntax → a quoted path.
  - An abort inside TS → C++ → TS was swallowed → rethrown, with the stack restored.
- **C2 and missing architecture items:**
  - C2 was Emscripten-only while litestl tests WASI → an `lstl` import as well.
  - The plan omitted parts of the architecture → added:
    - the query override table, the `hasOverrides` bit and the `queryInterface` hook;
    - `onWeakTargetLost`, `forceUnlink`;
    - namespaced slot keys;
    - `validateAll` for the new registries;
    - binding `getValues`.

### Unverified
- A full `emcmake` configure through the presets, including the emdawnwebgpu port and
  response files.
- End-to-end litestl TS runtime on an Emscripten module (exports list, `MODULARIZE`).
- The foreign-call cost.
- The N-API half of C2.
