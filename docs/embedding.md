# Embedding coredcc

Rules for hosting coredcc inside another process: an Unreal editor module, Maya, a
Node/NW.js addon, or a WASM page. The requirement is in architecture §2: coredcc owns no
global mutable state. Every registry lives in a `coredcc::Runtime`, and several Runtimes
may coexist in one process.

## litestl's process globals
coredcc accepts these. They exist once per copy of litestl linked into the process. In
Unreal, coredcc is exactly one module, so they exist once.

| Global | Where | Notes |
|---|---|---|
| Allocation tracker | `util/alloc.cc` (`getMemList()`, guarded by a mutex; a `thread_local` list for some paths) | Every `alloc::New`/`alloc` block is recorded, so leak reports cover the whole process, not one Runtime. `PermanentGuard` uses a process-wide counter and is not thread-safe (P0b PR B). |
| Task pool | `util/task.cc` (`TaskPool pool`) | Shared by every `parallel_for` caller. coredcc never calls `task::run` and never raises `ScopedBandCount` above 1. Calling `parallel_for` from inside a worker can starve the pool. |
| String keys | `util/string.cc` (`keymap`, `stringkey_idgen`) | `get_stringkey` interns into a global map with no lock. coredcc never calls it from worker threads. Interned strings are never freed. |
| Binding descriptors | `binding/` | Allocated with `new` and never freed, by design. coredcc builds its cached interface descriptors under `alloc::PermanentGuard`, on the main thread only. |

## Host build flags
- coredcc builds with exceptions and RTTI off. When coredcc is the top-level project it
  turns them off for litestl too, so `_HAS_EXCEPTIONS` agrees in every translation unit.
  A host project decides its own flags.
- `/WX` is on only when coredcc is the top-level project (`COREDCC_WERROR`). coredcc
  marks the litestl targets it links as `SYSTEM`, so litestl's header warnings don't
  reach coredcc's own warnings-as-errors when a host supplies litestl.
- coredcc sets no floating-point flags. litestl's `attr` target has a PUBLIC
  `/fp:precise`, so linking `attr` would pass that to coredcc; coredcc doesn't link it.
- When the host already has litestl (ubertopo does, at `source/litestl`), coredcc uses
  that copy (`if(NOT TARGET util)`). The two pins must be compatible, and the host's pin
  wins.

- coredcc links litestl's `io` target, and through it the lz4 that litestl vendors. A host
  that has its own lz4 gets two copies of its symbols when both are linked statically.

## Files
- **What is saved:** the blocks that strong references lead to from the save roots. Those are Root
  and FakeUser blocks, blocks with Host pins, Detached blocks, and placeholders that were records in
  a file. A block that nothing leads to isn't saved, and neither are deleted ones.
- **A host checks `SaveReport` after each save.** It lists what a load won't give back:
  - issues: references that a provider's `serialize` and `foreachRef` disagree about;
  - blocks that only Host pins keep. Pins aren't saved, so set FakeUser on what must last;
  - Transient and Detached blocks, and what only Transient blocks lead to;
  - what loading it takes, to compare with `LoadLimits`: the limits of a 32-bit process are lower.
- A save fails, naming the types, when a block's or an attachment's type neither serializes nor says
  that it is Transient.
- `loadLibrary` takes damaged and hostile input. It is bounded by `LoadLimits`: 2 GiB and 1,048,576
  blocks by default, 128 MiB in a 32-bit process. The limits cover the library's own structures, the
  decompressed payloads, and the entries that providers read through `BlockReader`. Other memory
  that providers allocate in `deserialize` is theirs to bound.
- A load isn't undoable and needs a library without records: clear the history first.
  `resolvePlaceholders` isn't undoable either: clear the history before it too.
- `appendLibrary` adds a file's blocks to a library that has some, with new ids. `appendUndo` is the
  same as an undo step. Blocks of a client other than coredcc need its provider's `rekey`, which may
  answer the id of a missing block to fill it.
- **Transient blocks** (the host's data, such as actors) aren't in the file. References to them
  load as MissingBlock records. Create them again with `createWithId` under the same ids, so ids
  must be stable across sessions. One that has slots or attachments of its own is a record without
  data, which keeps them until then. A command that does it uses `CreatedBlocks`, whose undo
  makes the record a missing one again.
- A type that was registered after a load reads its blocks with `resolvePlaceholders`.
- **Saving and loading from TypeScript** is planned: the facades come with P2's step B4. Until then a
  TS host needs its own C++ for it. Block and attachment providers can't be written in TS.
- **Several libraries** can share an executor and a history. A command is undone through a Context of
  the library it was done in; through another one, or when its library is gone, the undo fails.

## Graphs
- **Destroy order:** GraphInstances, then standalone GraphDefs, then the Library, then the Runtime. An
  instance of a block may outlive the block: it evaluates to errors then, until it is back.
- Payloads that a kernel made may outlive the Library. One that aliases a block's data is null then, so
  `get()` can be null even for a value that was there.
- A host that changes block data itself must call `ChangeBus::notify` with its owner, or graphs that read
  it aren't evaluated again.
- A program that evaluates graphs for the user's objects builds a standalone GraphDef around theirs, and
  connects with interior sockets (`graph/interior.h`). `tests/graph/test_graph_scene.cc` is an example.

## Layouts
- **Text is untrusted.** `parseLayout` is linear in it and refuses what is over `LayoutLimits`. A Layout
  is well formed, but may name what doesn't exist.
- **Call `validateLayout` once everything is registered,** with the struct of the Context's root. A
  struct that is registered later only gives warnings.
- **It knows static defs only.** An enum key that only a dynamic hook adds is a warning, and a path that
  it accepts can still fail in a given document.
- **`toolOptions` needs the last-used inputs at the root:** `LastUsedStore::addCollectionTo`, with its
  default name. Its members are at the JSON's `optionsPath`.
- **A tool of an Interactive command** is a warning: the frontend begins it with `beginInteractive` under
  the tool's prefix, and changes it with `updateInteractive`.
- **`CollectionAccessor::staticItem`** lets it see the struct of one item by its key, for collections
  whose items have structs of their own.
- **A node's own paths are relative to its parent's prefix;** a leading `/` is the Context's root.
  The paths that a Layout and its JSON give start with `/`, so a Context's own prefix doesn't change them.
- **`invokeAt` runs a tool under its prefix,** which is resolved when it starts: undo and redo run where
  it ran, also if the prefix's path leads elsewhere by then. The command's own paths are under the prefix
  too, unless they start with `/`.
- **One `beginGesture` for each drag,** and `cancelGesture` for Escape. It takes back the gesture's sets
  as far as they are the last commands.

## Watching paths from scripts
- **A `PathReader` that watches holds the Context.** Dispose of it, or `unwatch`, before the Context
  goes; a `Session` does that for its readers.
- **Nothing is called back.** `flushChanges` runs the ChangeBus, and `takeChanged` says which paths of
  a reader changed since it was last asked. Call them as often as the UI should follow.
- **Widgets may need to be made again.** `takeChangedMeta` says which paths lead to another struct or
  member, or to a dynamic def that is another now (its range, its items). `meta` marks such defs with
  `dynamic`. Their watchers are read at every flush that has changes, so a hook should be cheap.
- **`libraryResolve` needs an empty history;** `clearHistory` first.

## Threading
- The public API is single-threaded, on the host's main thread.
- Foreign (JS) interface implementations are thread-affine. Parallel algorithms check
  the `threadAffine` flag and either run serially or gather foreign data on the main
  thread first.

## Interfaces across binary images
Interface identity is per binary image. These are all statics or addresses inside the
image that compiled them:
- `typeKeyOf<T>`;
- every `defineBindings()` cache;
- `CDCC_INTERFACE`'s descriptor.

Declare interfaces and register their implementations inside the image that links
coredcc. In Unreal, that's the one coredcc module. A second DLL that registers an
implementation builds its own copy of the interface descriptor. It is then rejected
("registered twice"), or its struct parameters fail as "different C++ types with the
same name". Stable, name-based keys are tracked for the Unreal spike (P4).
