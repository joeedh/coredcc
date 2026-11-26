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
| Allocation tracker | `util/alloc.cc` (`getMemList()`, guarded by a mutex; a `thread_local` list for some paths) | Every `alloc::New`/`alloc` block is recorded, so leak reports cover the whole process, not one Runtime. `PermanentGuard` counts for each thread. |
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
- **From TypeScript:** `saveLibrary`, `loadLibrary` and `appendLibrary` (`bind/ts/library.ts`). Block and
  attachment providers can't be written in TS.
- **Several libraries** can share an executor and a history. A command is undone through a Context of
  the library it was done in; through another one, or when its library is gone, the undo fails.

## Graphs
- **Destroy order:** GraphInstances, then standalone GraphDefs, then the Library, then the Runtime. An
  instance of a block may outlive the block: it evaluates to errors then, until it is back.
- Payloads that a kernel made may outlive the Library. One that aliases a block's data is null then, so
  `get()` can be null even for a value that was there.
- A host that changes block data itself must call `ChangeBus::notify` with its owner, or graphs that read
  it aren't evaluated again. It may say which aspects of the data changed (`ChangeAspects`), so that
  kernels that don't use those don't run.
- **What a block answers to a query** (`Library::query`, and `NodeExecContext::queryBlock` in a kernel) may
  change only with a notification of the block, or a move of the Library's epoch: an override does move
  it. That holds for providers' `queryInterface` too. Otherwise a graph keeps the old answer, and calls a
  table or `self` that may be gone.
- **Kernels are pure:** what they give depends on their inputs and params, and on blocks they got with
  `useBlock`, `paramBlock` and `queryBlock`. One that reads anything else (a file, a clock, its own state,
  `userData` that changes) keeps its old outputs until `GraphInstance::clear`, `drop` or an edit.
- **An error is kept as a value is,** until something the kernel read changes, or `retry`. Cancelled isn't
  kept: the node runs again in the next evaluation that needs it.
- **Stopping and progress:** `GraphInstance::setMonitor`. Its functions are called while a graph evaluates,
  so they can't edit or evaluate.
- **Threads:** kernels run one at a time, on the main thread. `NodeTypeFlags::ThreadSafe` says that a kernel
  wouldn't mind otherwise; nothing uses it yet.
- **Memory:** every kernel that was needed keeps its outputs, and every element of a foreach with a state of
  its own keeps a graph's worth of them (`maxCached` of the node, `setMaxCachedKernels` of the instance).
  `memory` and `report` say how much, `drop` gives up a node's, and `clear` everything.
- **Values pin blocks:** a payload that aliases a block's data keeps the block, deleted or not, for as long
  as a socket or a client has the payload. `clear` lets go of the instance's.
- **Reloading code:** `NodeTypeRegistry::replaceKernel` (`replaceNodeType` in TS). Sockets, params and the
  version must be as they were.
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
- **The JSON** (`layoutJson`, `parseLayout` in TS) has `version`, 1, at its root: fields are only added
  under it. Each node has its `kind` and what was written, and the joined `effectivePath`, `prefix` (a
  tool's, with its own `path`), `optionsPath` and condition paths, so a frontend joins nothing. `attrs` has the
  host's `data-` attributes, and all of a `custom` node's.
- **Versions.** `<layout version="2">` in a coredcc that knows 1: elements and attributes that it doesn't
  know are skipped, and `validateLayout` warns of each. Without a version, or with 1, they are errors.
  `list`, `menu`, `include` and `slot` are reserved, also inside a `custom` node.
- **Conditions** are of a Bool, an enum's key, a pointer (it points to something), a collection's
  length (it isn't 0) or a String (it isn't empty). One without a value is false, also if it is
  negated. `conditionHolds` is the rule, in C++ and in TS.
- **What isn't there.** A container whose own `path` names nothing now is hidden (`containerIsThere`),
  unless `validateLayout` has an Error about it. A prop shows why it has no value, and a tool is
  disabled when `canInvoke` gives a reason.
- **`canInvoke` and `ToolChecks`** ask each command's `canRun`, so keep those cheap. An open
  interactive command, or a stray error, is a reason for every tool.
- **The active item as a prop** is the collection's own pointer member (`activeItem`), not
  `items.active`, which can't be null.
- **`invokeAt` runs a tool under its prefix,** which is resolved when it starts: undo and redo run where
  it ran, also if the prefix's path leads elsewhere by then. The command's own paths are under the prefix
  too, unless they start with `/`.
- **One `beginGesture` for each drag,** and `cancelGesture` for Escape. It takes back the gesture's sets
  as far as they are the last commands.

## Watching paths from scripts
- **A `PathReader` that watches holds the Context.** Dispose of it, or `unwatch`, before the Context
  goes; a `Session` does that for its readers.
- **Nothing is called back.** `flushChanges` runs the ChangeBus, and `takeChanges` gives the paths of
  a reader that changed since they were last taken, with their values or errors, in one call. Call
  them as often as the UI should follow. After `watch`, `takeAll` gives them all.
- **Widgets may need to be made again.** A change's `metaChanged` says that its path leads to another
  struct or member, or to a dynamic def that is another now (its range, its items). `meta` marks such defs with
  `dynamic`. Their watchers are read at every flush that has changes, so a hook should be cheap.
- **Pending changes are merged.** Past 4,096 distinct ones the next flush reads every watcher. A host
  that never flushes holds no more than that.
- **Types.** `generatePaths(apiCatalog(ctx))` is a file of types, for `typed<Root, Commands>(ctx)`. Make
  it again when structs or commands change. It describes what is registered at that time.
- **`libraryResolve` needs an empty history;** `clearHistory` first.

## Threading
- The public API is single-threaded, on the host's main thread.
- Foreign (JS) interface implementations are thread-affine. Parallel algorithms check
  the `threadAffine` flag and either run serially or gather foreign data on the main
  thread first.
- **Queries of a `GeometryBvh`, `Bvh` or `AnyBvh` are const** and may run on several threads at once, but
  not while it is built or refitted.
- **coredcc uses litestl's task pool** (`parallel_for`) in these places, and nowhere else so far:
  - BVH builds of more than 4,096 primitives and refits of more than 16,384 (a partial refit by the
    leaves it touches). `BvhOptions::parallel = false` keeps one on the calling thread. A build that
    already runs on a worker is serial.
  - `evaluateScene`, for the scene's BVH only. `rt.ext<SceneSettings>().bvh.parallel = false` keeps it
    off the pool; `leafSize` and `bins` are there too.

## Objects and geometry of a host
- A block type that implements `Transformable` is an object, and one that implements `Spatial` has
  geometry. coredcc asks them when it evaluates or picks, and keeps nothing that it can't ask again.
- **Graphs and `evaluateScene` don't ask again** unless they hear of a change. What a block answers to `queryBlock` (whether it
  implements an interface, and everything the interface's slots return) may change only with one of:
  - a notification of the block: what `setValue` and the commands do, `writeTransform`, `writeParent`, and
    `ChangeBus::notify` with its owner after the host moved the object itself;
  - `invalidateData` after the host changed its geometry (also for the data block of an object: notify
    the block that has the Spatial). Its kinds are the change's aspects, so a kernel that asked for the
    positions doesn't run for a selection: name all that changed;
  - a move of the Library's epoch: creating, deleting and restoring blocks, `onHostDestroyed` and
    `onHostRestored`, `writeRef` and `noteRefsChanged`, overrides. A block's data pointer may change only
    between the two host calls.
  If a graph's result is old, `GraphInstance::setIncremental(false)` says whether a notification is missing.
  If a scene's is, `SceneSettings::incremental = false` does the same: evaluateScene then evaluates every
  object each time. The scene's time is compared in each evaluation, so writing `Scene::frame` needs no
  notification.
- **An object that the host takes away** (`onHostDestroyed`) isn't an error for undo, redo or a drag. What
  they would have written to it is written when it is back: `onHostRestored` calls its `writeChannels`, so
  its data must be there by then. If it can't be written just then (an evaluation is going on, an error is
  pending), the next `evaluateScene` does it.
- **Bounds are kept** for a data block (or an object) whose type implements Invalidation. Its bounds are asked for
  again after `invalidateData` with Topology or Positions, another notification of the block, or a move of the
  Library's epoch. Without Invalidation they are asked for in each evaluation.
- **Flags and locks** are written with the optional slots `setFlags` and `setLocks` of Transformable. The commands
  for them leave out objects that don't have the slot.
- **Paths to a host's object** (`scene.objects[0].transform.location`) go through `coredcc.SceneObject`: they read a
  copy of its channels, one for each block that was asked for, and write with `writeChannels`.
- **Channels that something else drives** (animation, a rig): implement `OverrideProvider` and give it to the
  Library with `setOverrideProvider`. Evaluation and all commands ask it. The stored values stay. Post a
  notification when its answers change. Clear it before it ends.
- **Parents are kept alive** by their children, and saved with them, also when they are in no scene.

## A host's own scenes
- The Context's scene may be of any type for paths and layouts: `scene.x` goes through its struct.
- `evaluateScene`, `pickScene` and the commands need a `coredcc.Scene`. There is no interface for another type's
  membership yet.
- **To mirror a scene of the host's:**
  - make one `coredcc.Scene` for each of them (`newScene`), not a GC root if it shouldn't be saved as one;
  - give the host's objects blocks whose type implements Transformable, as above;
  - when the host adds or removes objects, call `Scene::addMany` and `Scene::removeMany` with the scene, outside
    of undo (the host's own undo does it again);
  - leave out `core.scene.link`, `core.scene.unlink` and `core.object.delete`, or give them the host's commands'
    work: they change the mirror only;
  - the selection is the mirror's. Listen to `selectedObjects` and `activeObject` of the scene, and set them with
    `setSelection` and `activeCell` when the host's change.
- **For drawing:** `Scene::results()`, with `lastChanges` for what is different. Scripts have `sceneResults`.

## Graphs that drive objects
- `registerDriveTypes(rt)`, after the scene's and the graph's types and commands. Then one `SceneDrivers` per
  Library that should be driven: `SceneDrivers::make(lib)` makes it the Library's `OverrideProvider`.
- **Destroy it before the Library,** as any graph instance. If the Library goes first, it does nothing more.
- **The document's list of drivers** (`drive/driver_list.h`, a block named "Drivers") names the graph blocks
  that drive the document's objects. `core.drivers.addGraph` makes it when there is none;
  `core.drivers.removeGraph` takes a graph out of every list. Scenes don't list graphs, so a duplicated scene
  drives nothing twice. An appended file may bring a second list: every list drives, each graph once.
- **For paths,** give the root struct `addDriverGraphsMember(root)`: the collection `driverGraphs`.
- In them: `scene.drive.<kind>` drives a channel of an object, `scene.object.state` reads an object's evaluated
  state, `scene.time` gives the time. A graph evaluated by itself has no states to read.
- **Another provider** (animation) goes through `SceneDrivers::setInner`. It is asked first; the graphs win where
  both drive a channel.
- What can't work is in `SceneDrivers::diagnostics()`; the objects concerned are ModelFailed, and so is what
  a graph computes from them.

## Scenes in files
- Objects and scenes are saved with the library. The Context's scene isn't: set it after a load.
- Register transform types before loading. The objects of a type that isn't there stand where their
  parents are; their channels are kept, and read when it is registered.
- **A transform type in TS** has all of it: `defineTransformType` takes full channel specs, `version`, `upgrade`,
  `pivot` and `evaluateMany`. With thousands of objects, give it `evaluateMany`: a call into a script for each
  object costs more than the work in it.
- **A transform type that changes its channels** raises `TransformTypeInfo::version`, and has an `upgrade`
  slot that changes what older files have. A newer file's channels are kept unread.
- What a newer build put into an object or a scene is kept, and saved again. `dataMatrix` and `parentKey`
  are reserved. `ObjectEval::dataWorld` is what to draw an object's data with.
- A duplicated or appended scene isn't a GC root (`BlockFlags::Root`); an appended one has FakeUser.

## Units and frames
- Values are stored in base units (meters, radians, seconds). A scene's `units` say how they are shown,
  and hold the frame rate. `Context::units()` gives those of the active scene.
- `formatValue` and `parseValue` (`api/format.h`; `cdcc.format` and `cdcc.parse`) turn a path's value into
  text and back.
- A host's own scene type has units when its struct calls `addUnitSettings`.

## Picking
- **Positions are in device pixels** (x and y of `rayFromPixel`, `regionFromRect` and `pickScene`), y
  down. **Sizes are in logical pixels** (`PickOptions::radius`, `PickHit::distance`, the pixel radii of
  a sink's points and segments), which are `View::dpiScale` device pixels. What is in device pixels all
  the same says so (`radiusDevicePixels`).
- `pickScene` answers for the last `evaluateScene`. A `pick` slot is called while nothing may change.
- To find the first hit only, as a hover does, set `maxHits` to 1: objects behind it aren't asked.

## Tools and the viewport
- **One `ToolManager` per viewport**, given to the Context with `Context::setTools`. Call `shutdown` with a
  Context before destroying it, then destroy it before the Runtime.
- **Events:** `dispatch(ctx, view, event)` for each one, with the View of the viewport the pointer is in.
  - Positions are device pixels, y down, as for picking.
  - A move carries the positions the host coalesced into it (`addSample`), oldest first. Strokes use them.
  - It returns `eventresult::kHandled` when coredcc used the event. Otherwise the host's own navigation may
    have it.
  - A modal grabs every event until its gesture ends. Send `Cancel` when the host wants a gesture stopped,
    and `FocusLost` when the window loses the focus.
- **Evaluate the scene after each event, as a frame would** (`evaluateScene`). Gizmos and `pickScene` see
  the last evaluation, so without one they place themselves and pick against old matrices.
- **Draw:** `draw(ctx, view, list)` gives a DrawList of the tool's, the gizmos' and the modals' overlays.
  The host renders it after its own scene, with its depth buffer. [drawlist-format.md](drawlist-format.md)
  is the format, and `DrawListReader` reads it in C++.
  - Positions are relative to the list's origin, and the origin is the camera's position. Compute them in
    double, then convert to float for the GPU.
  - `DepthMode::Test` is drawn with the scene's depth, `Always` over it, and `XRay` twice: tested, and then
    with the hidden part at a third of the alpha.
  - Screen-space records (`Line2D`, `Circle2D`) are in device pixels. Their radii and widths are in logical
    pixels.
- **Undo during a gesture** cancels it (`CommandExecutor::undo`). A host with its own undo stack should
  refuse its own undo while `interactiveOpen()` is true, or cancel the gesture first. Unreal's adapter holds a
  transaction open for that.
- **The test app** (`apps/testapp/src/overlay/`) is a WebGPU renderer of DrawLists, and
  `apps/unreal-spike/Source/CoredccSpike/Private/DrawListPdi.cpp` draws one through Unreal's PDI and canvas.

## Keys
A printable key is the code point it gives without Shift: 'g' for both g and G, '1' for 1 and !. The others
are `keys::*` in `tool/input.h` (`Keys` in TS):

| coredcc | DOM `KeyboardEvent.key` | Unreal `EKeys` |
|---|---|---|
| `kEscape`, `kEnter`, `kTab` | `Escape`, `Enter`, `Tab` | `Escape`, `Enter`, `Tab` |
| `kBackspace`, `kDelete` | `Backspace`, `Delete` | `BackSpace`, `Delete` |
| `kLeft`, `kRight`, `kUp`, `kDown` | `ArrowLeft`, `ArrowRight`, `ArrowUp`, `ArrowDown` | `Left`, `Right`, `Up`, `Down` |
| `kHome`, `kEnd`, `kPageUp`, `kPageDown` | `Home`, `End`, `PageUp`, `PageDown` | `Home`, `End`, `PageUp`, `PageDown` |
| `kShift`, `kCtrl`, `kAlt`, `kMeta` | `Shift`, `Control`, `Alt`, `Meta` | `LeftShift`/`RightShift`, `LeftControl`/`RightControl`, `LeftAlt`/`RightAlt`, `LeftCommand`/`RightCommand` |
| `kF1` + n | `F1` to `F12` | `F1` to `F12` |

- DOM's `key` is the shifted character. For a printable key, take the lower case of a letter, and the digit
  or symbol from `KeyboardEvent.code` (`Digit1` is '1').
- Modifiers go in `InputEvent::modifiers` as well as in key events of their own.

## Strokes
- **A brush is the client's:** a `StrokeBrush`, registered by name (`addNativeStrokeBrush`, or
  `registerForeignStrokeBrush` and TS `defineStrokeBrush`). `core.tool.stroke`'s `brush` setting names it.
- **One brush instance per stroke**, and the stroke's undo step owns it. Its `undo` puts back what its
  `begin` and `apply` wrote. On redo, `begin` runs again and the recorded samples are applied again, so a
  brush must give the same result from the same samples.
- **`apply` may only write values.** It runs inside the open gesture, where commands can't run.
- **Notify what a brush changes:** values notify by themselves; geometry needs `invalidateData`, and any other
  write `ChangeBus::notify` (`notifyChanged` in scripts, allowed in a brush's slots). `evaluateScene` is
  incremental since P5c, so an unnotified block keeps its old result.
- **Rays hit the stroke's object only**, through `pickScene`, so the scene must have been evaluated, as for
  any pick. Off the object, samples lie on the plane through the last hit.
- **Symmetry** of the object's data block (or of the object) mirrors the samples when the `symmetry` setting
  is on. Each copy has `mirror` set.

## Unreal
`apps/unreal-spike/` is an Editor module that hosts coredcc's history and tools (`node tools/unreal-spike.mjs`;
the plan's U-A and U-B notes have the findings). What a real integration takes from it:
- **Build coredcc as Unreal builds:** `/EHsc /GR- /MD`, release CRT, `_ITERATOR_DEBUG_LEVEL=0`
  (`apps/unreal-spike/coredcc/CMakeLists.txt`). The include roots are `source/`, `extern/`, `extern/litestl/`
  and `extern/litestl/extern/`.
- **C4668 must not be an error around coredcc's headers:** Eigen tests GCC's macros in `#if`. Unreal's engine headers can reach litestl's `math/` headers by name, because Windows' file system
  ignores case and litestl's root answers `"Math/Vector.h"`. So C4668 has to be a warning for the whole module
  (`CppCompileWarningSettings.UndefinedIdentifierWarningLevel`), not only inside a push.
- **History:** each coredcc command is one Unreal transaction holding an `FCommandChange` on a transactional
  UObject (`UnrealHistory.cpp`). Expire the changes before the Runtime goes, and root the document object.
- **Tools:** one `UInteractiveTool` adapter forwards ITF's click-drag and hover behaviors as InputEvents, holds
  one transaction open while a coredcc gesture is open, and draws the DrawList in `Render` and `DrawHUD`.
- **The View from an `FSceneView`:** `UnscaledViewRect`, the view matrix, and
  `ComputeProjectionNoAAMatrix()`, with depth 0 to 1, reversed Z, an infinite far plane, left-handed and z
  up. `GetProjectionNoAAMatrix()` is the identity until the renderer adds jitter.
  `ViewFromSceneView.cpp` is the conversion.

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
