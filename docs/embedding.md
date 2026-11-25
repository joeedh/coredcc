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
- `loadLibrary` is safe on any input. It is bounded by `LoadLimits`: 2 GiB and 1,048,576 blocks
  by default, 128 MiB in a 32-bit process. Memory that providers allocate in `deserialize` is
  theirs to bound.
- A load isn't undoable and needs a library without records: clear the history first.
- **Transient blocks** (the host's data, such as actors) aren't in the file. References to them
  load as MissingBlock records. Create them again with `createWithId` under the same ids, so ids
  must be stable across sessions. `SaveReport` lists them, and the blocks only they lead to.
- A type that was registered after a load reads its blocks with `resolvePlaceholders`.

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
