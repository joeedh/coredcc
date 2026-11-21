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
