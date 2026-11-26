# P5c: the scene evaluator

- Status: pressure-tested and revised (2026-11-25). The record is at the end.
- Branch: `p5c-evaluator`, in the worktree `C:\dev\coredcc-p5c-evaluator`, from master `8ca89b9`.
- Architecture: §7 ("Original vs evaluated data"), §8 ("Scene and the scene evaluator"), §14 (P5c).
- Depends on: P3 (`docs/plans/2026-11-24-p3-objects-bvh.md`, "What P4 and P5c rely on") and P5
  (`docs/plans/2026-11-24-p5-graph.md`, "What P5c (the scene evaluator) uses").
- Feature review: **scene evaluator**.

## Summary
- User graphs drive channels of objects, and may read the world transforms of other objects, in the same
  evaluation, without a frame of lag.
- The generated graph holds a transform node only for objects whose state a graph reads, and for their
  ancestors. All other objects are evaluated by P3's direct pass, which gets the driven channels through
  the `OverrideProvider`. Putting every object into the graph would miss P3's targets by a factor of 2 to 3
  (measured below).
- The graphs are listed by the document's list of drivers (`coredcc.DriverList`, since 2026-11-26; at
  first by scenes). One `SceneDrivers` object per Library is the
  Library's `OverrideProvider`, so commands, drags and `computeState` see the same driven values as
  `evaluateScene`.
- Part B makes P3's direct pass incremental: an evaluation recomputes only the objects whose blocks were
  notified, and their descendants. This is what P3's plan left to P5c for drags ("P5c is the answer").
  It can be cut from P5c without changing part A.

## Decisions (user, 2026-11-25)
The open questions at the end were answered as the plan recommends:
1. Only the objects that a graph reads, and their ancestors, get a node. The deviation from §8 is accepted.
2. Driver graphs are listed per scene (`Scene::graphs`) and drive the whole document. (Replaced on
   2026-11-26 by one document-wide list.)
3. Part B (the incremental direct pass) stays in P5c, separable as planned.
4. Step 8 adds the missing notifications to P3's tests, and `incremental` is on by default.
5. A graph that reads an object whose driver failed fails too. There is no silent fallback.

## Cross-check with P4's plan (2026-11-25)
Checked against `docs/plans/2026-11-25-p4-tools.md` in P4's worktree, section "Ownership between P4 and P5c".
No conflict was found.
- **P4's edits in others' files** (`api/context.h`, `command/executor.*`, `slotGate` and `runCommand` in
  `bind/facades.cc`, the Read kind at five `slotGate` calls in `bind/scene_facades.cc`, `bind/foreign_command.*`,
  and appends to `bind/manager.cc`, `bind/CMakeLists.txt` and `bind/ts/index.ts`) are not in files that P5c
  edits. The exception is the appends, which merge trivially. P5c's facades go in `bind/drive_facades.cc`
  and call `slotGate` as it is. Whichever branch merges second adapts those calls to P4's Read/Write kind
  (`driverDiagnostics` reads; the others write). P5c doesn't restructure `slotGate` or `runCommand`.
- **P4 promises not to touch** `scene/`, `graph/` and `bind/graph_*`. That covers every file P5c owns there,
  including `bind/graph_facades.cc`.
- **Element edits notify** with `invalidateData(Positions)` in exec, undo and redo, which is what part B needs
  for kept bounds.
- **What P4 needs from P5c is kept:**
  - ApplyDelta and `core.object.dragDelta`/`applyDelta` aren't changed;
  - `computeState` and `computePivot` answer with overrides outside a pass, for the time given (they call
    `prepare` at that time);
  - `evaluateScene` stays callable once a frame while an interactive command is open, because only an
    `EvaluationScope` makes it Busy. The pressure test ran that a drag update notifies and doesn't move the
    epoch;
  - an evaluation after a drag update gets cheaper with part B.
- **One note for P4:** its tests have a stub `OverrideProvider`. The new optional `prepare` has a default, so
  the stub still compiles and is asked for every object. With part B, a stub whose answers change between
  evaluations must notify the objects, as P3's contract already says.
- `bind/ts/index.ts` is added to P5c's shared, append-only files, for the exports of `drive` functions.

## Ownership between the parallel phases
P4 (tools, gizmos, drags, DrawList, ToolManager) runs at the same time in `C:\dev\coredcc-p4-tools`.

| Owner | What |
|---|---|
| P5c owns | the new module `source/coredcc/drive/`; `scene/evaluator.{h,cc}` and the files split from it; `scene/evaluator_batch.cc`; `scene/overrides.{h,cc}`; `scene/scene_bvh.{h,cc}`; `scene/evaluated.{h,cc}`; the `graphs` member of `Scene` (`scene/scene.{h,cc}`, `scene/scene_io.cc`); `bind/drive_facades.{h,cc}`; `tests/drive/`; `tests/bench/bench_drive.cc`; the cases of `tests/bench/bench_scene.cc` |
| P5c edits, small and additive | `graph/node_type.h` (one flag, `NodeTypeFlags::InputAdapter`); `bind/graph_facades.cc` (the prefix `scene.` reserved for scripts, next to `core.` and `graph.`); `base/invalidate.h` (one aspect bit, `Driven`); `bind/ts/scene.ts` (new functions at its end) |
| P5c edits in step 8 | P3's tests in `tests/scene/` that write channels, parents or a provider's values without notifying and then evaluate (raw writes are in 22 files): they get the notification a host must post. What they check stays. |
| P5c only reads | the rest of `scene/` (objects, models, solvers, commands, `ApplyDelta`, picking), `graph/`, `api/`, `command/`, `lib/` |
| Shared, appended only | `source/coredcc/CMakeLists.txt`, `tests/CMakeLists.txt`, `tests/bench/CMakeLists.txt`, `tools/bench.mjs` (one name in `NAMES`), `bind/manager.cc` and `bind/CMakeLists.txt` (one registration each), the task list (row 10 and its focus lines), `docs/debugging.md`, `docs/embedding.md`, `docs/modules.md`, `docs/index.md`. No phase edits `CLAUDE.md`. |

### What P5c needs from P4
- **Don't edit** the files that P5c owns, above. In particular `evaluateScene`, `SceneEvaluator`,
  `OverrideProvider` and `SceneBvh`.
- **Don't implement `OverrideProvider`**, and don't call `setOverrideProvider`: a Library has one, and
  P5c's `SceneDrivers` is meant to be it. A tool that needs channels driven during a drag uses
  `ApplyDelta`, which writes the stored channels.
- **Write objects only in ways that notify:** commands, `ApplyDelta`, `writeTransform`, `setValue`, or
  `ChangeBus::notify` with the object's owner. Change geometry with `invalidateData`, naming
  Positions or Topology when the bounds may change. Part B relies on this. P3's evaluator recomputes
  everything, so a test that writes channels directly and then evaluates passes today and would fail
  after Part B. (`bench_scene`'s "every root moved" does this; P5c changes it.)
- **Read results through** `Scene::results()`, `lastChanges()`, `evaluated()`, `children()`, `bvh()`
  and `pickScene`, not through `SceneEvaluator`.
- **Don't add tests to `tests/scene/`** that write objects without notifying and then evaluate. Step 8 adds
  notifications to the existing ones.
- **Know that a drag freezes driven values.** `ApplyDelta` reads the overrides once, at its start
  (`apply_delta.cc:126-135`), so a channel driven by an object that moves during the drag keeps its value
  from the start. This is P3's behaviour, and P5c doesn't change it.

### What P5c promises P4
- The signatures and meaning of `evaluateScene`, `EvaluateResult`, `Scene::results`, `lastChanges`,
  `evaluated`, `children`, `pickScene`, `computeState`, `computeParentState`, `computePivot`,
  `setWorldOf` and `ApplyDelta` stay as they are.
- `OverrideProvider::overridesOf` keeps its signature and its contract, including "post a notification
  when its answers change". P5c adds one optional virtual, `prepare`, whose default keeps today's
  behaviour.
- A drag costs less after Part B: at 100,000 objects, one moved object should take at most 2 ms, not 16.
- A driven channel behaves as P3's contract says: the solvers leave it alone, and the object is counted
  in `driven`.

## Decisions (user, 2026-11-26)
Finding A of the [review](../reports/feature-report-scene-evaluator-2026-11-25.md) (per-scene lists drive
the whole document; duplicating a scene with its list drives objects twice):
1. **One document-wide list of driver graphs** replaces `Scene::graphs`, like Houdini's `/obj`. A listed graph
   drives the whole document, as before. Scenes no longer list graphs, so duplicating a scene can't make a
   second driver. Neither Blender nor Houdini scopes drivers by scene.
2. **A graph attached to the object it drives** (Blender's drivers, "acting on self") was the other option.
   It isn't done now; see "Out".

### The document's list (design, 2026-11-26)
- **Where it lives: a block type of drive/'s, `coredcc.DriverList`** (a block named "Drivers", a GC root).
  Why a block and not a Library-level part of the file:
  - The library section of a file holds blocks and nothing else (`lib/io/format.h`). A document-level field
    would need a new part of the format, its own undo and its own datapath root, all in lib/, which is below
    graphs and doesn't know them.
  - A block gets all of that already: it is saved and loaded by its provider (with unread fields kept), its
    OptStrong cells keep the graphs alive and are followed by save, collect, append and duplicate; commands
    undo it with `HistoryBlock` and `CreatedBlocks`; and a struct makes it reachable by datapath.
  - The block type lives in drive/, so scene/ no longer has anything about graphs.
- **One list, in practice:** `DriverList::find(lib)` is the document's list: of the Live list blocks, a Root
  one before others, then the least canonical id. Not the Library's order, which slot reuse and loading
  change. The commands make it (undoably, `CreatedBlocks`) when there is none, and hold it by
  `HistoryBlock`, never by `find` again.
- **More than one can exist** after an append (its list is FakeUser, not Root). `SceneDrivers` reads them
  all, sorted by id, and a graph that two name counts once.
- **A duplicate of a list is empty,** so duplicating a list with its graphs drives nothing twice.
- **Commands:** `core.drivers.addGraph(graph)` and `core.drivers.removeGraph(graph)`, with no `scene` input.
  - addGraph checks the graph and AlreadyExists against every list before it makes a list.
  - removeGraph takes the graph out of every list; undo brings back each cell, at its place, in each list.
  - Both fail with FailedPrecondition while a list is a placeholder, and `SceneDrivers` reports such a list,
    so that a list that becomes readable later can't bring back graphs that were removed.
  - TS: `addDriverGraph`, `removeDriverGraph`.
- **Datapath:** the struct `coredcc.DriverList` has the collection `graphs` (read-only). A host gives its root
  the collection `driverGraphs` with `addDriverGraphsMember(root)`: every list's graphs, each once, never
  null. It looks at every block on each read.
- **Files:** the list block's payload has the field `graphs`. A scene's `graphs` field, written by builds of
  this branch before 2026-11-26, is kept as an unread field and drives nothing. No released file has it.
- **SceneDrivers' scan** looks for list blocks instead of scenes; the placeholder tracking (B5) stays, for a
  list block that is a placeholder until drive/'s types are registered.

## Scope

### In
- **From architecture §14:** generated transform nodes, override inputs, adapter nodes and the apply
  phase.
- **Where graphs that drive objects are listed:** the document's list (`coredcc.DriverList`, 2026-11-26),
  with commands, paths and files.
- **`SceneDrivers`:** the Library's `OverrideProvider`, which owns the generated graph and its instance.
- **Node types for users' graphs:** `scene.time`, `scene.object.state` (reads an object's state) and
  `scene.drive.<kind>` (drives a channel).
- **Bindings and TS** for all of it, and a wasm test.
- **Part B, deferred to P5c by P3:** an incremental direct pass, and a SceneBvh refit of only the boxes
  that changed (P3, round 2: "Not done").
- **Benchmarks** with targets, and the P3 benchmark case that needs a notification.

### Out
- **Putting every object into the graph** (see "Key decisions", 1).
- **M7 of the graph review** (a structural edit rebuilds a def's topology). The generated def holds the
  objects that graphs read, not all objects, so its rebuilds stay small. It stays deferred, with the
  condition under which it is needed (see "Risks").
- **Evaluated geometry** (a graph that makes an object's mesh), `dataMatrix` and instancing (P3 finding
  18). The apply phase is written so that more outputs than channels can be added later.
- **Animation curves.** A host's provider can be combined with `SceneDrivers` (`setInner`).
- **Adapters inside foreach bodies.** Interior sockets can't lead into a foreach (P5, step 9), so the
  driver scan reports them as diagnostics.
- **An incremental pass after a time change or an epoch move.** Both stay full passes (see Part B).
- **Parallel evaluation** of the generated graph or the direct pass.
- **A graph attached to the object it drives** (Blender-style drivers acting on self; option 2 of the
  2026-11-26 decision). Possible later work: such graphs could be listed by the object, and scanned with
  the document's list.

## What the code says
Checked against master `8ca89b9`.

### P3's evaluator (`scene/evaluator.{h,cc}`, `scene/scene.h`)
- `evaluateScene` (`evaluator.cc`, end of file) checks Busy, the Library's `busy()` and a pending sticky
  error, flushes `PendingWrites`, pins the scene, opens `beginNoCollect`, and calls
  `SceneEvaluator::run`, which opens the `EvaluationScope` itself. So a graph can be evaluated between
  the flush and `run`, outside the scope, as P3's plan intended.
- `SceneEvaluator::run` revalidates its lookups when `LookupKey` (the Library's epoch, the interface
  table count, the client generation) moves, then calls `evaluateAll`, which recomputes every member's
  transform and bounds, compares each result bitwise with the front store, and swaps the stores if one
  differs.
- `evaluateAll` calls `Library::data` again for every object and data block, and compares `read`'s
  parent and data ids with the cached ones (`Pass::Stale`, at most two attempts).
- The direct pass asks the provider once per object (`evaluateOne` → `detail::overridesOf`).
- When anything differs, `evaluateAll` copies the whole front store into the back store first
  (`out.objects[order_[j]] = old.objects[order_[j]]`). At 100,000 objects that is 30 MB (measured below).
- `SceneBvh::update` refits all boxes or rebuilds (`scene_bvh.h`). `Bvh::refitPositions` (`bvh/bvh.h`)
  can refit some leaves, and `SceneBvh` doesn't use it.
- The bounds listener (`run`, `changeListener_`) already follows ChangeBus notifications by BlockId and
  aspects, for blocks whose type implements Invalidation. Part B extends it to transforms.
- `Scene` has no list of graphs. Its references are `cells_`, `active_`, `selection_` and the unread
  `fields_` (`Scene::foreachCell`). Its file format is named entries (`scene_io.cc`), open to more.
- `Scene::frame` is a public field. Its datapath member notifies the scene's block; a direct write doesn't.

### The overrides contract (`scene/overrides.{h,cc}`)
- One `OverrideProvider` per Library, kept in a Runtime extension with a `LibraryRef`.
  `setOverrideProvider` and `clearOverrideProvider` each call `notifyAll`.
- `detail::overridesOf` resets `out` to the type's channel count, calls the provider, and turns a sticky
  error into the result. evaluateScene, `computeState`, `computePivot` and the commands all go through
  it.
- The provider is asked inside the `EvaluationScope` of evaluateScene's pass. There it can't evaluate a
  graph, because `GraphInstance::evaluate` is Busy while `Runtime::evaluating()`.

### The graph (`graph/`)
- `GraphInstance` evaluates prepared `SocketHandle`s and reports which moved (`instance.h`). A
  `SocketPath` may name any socket of a node inside groups (`NodePath::groups`).
- `NodeExecContext` gives `paramBlock`, `useBlock(id, aspects)`, `queryBlock<I>`, `library()`,
  `runtime()`, `userData()` and `isLinked(i)` (`exec_context.h`). A kernel may take the sticky error.
- `NodeTypeFlags` has `OutputAdapter` and `ThreadSafe`. `GraphDef::forEachNode(flags, fn)` costs the
  flagged nodes only (`def.h`). Readers have no flag, so the stand-in (`tests/graph/test_graph_scene.cc`)
  scans every node to find them.
- An input's kind gives its socket type (`graphTypeFor`, `graph/value.cc`): Bool, Int, Float, String,
  Vec2-4, Mat4, Quat. An Enum def has no socket type, so an Enum channel becomes an Int input.
- `GraphDef` has `structureVersion`, `contentVersion`, `refsVersion` and `serial`. A Library that writes a
  cell moves its epoch, not `refsVersion` (graph review, M13).
- `linkAll` makes many links at once and checks for cycles once. Interior socket keys look like
  `@7.value` and `@3/7.value` (`graph/interior.h`).
- The stand-in evaluator in `test_graph_scene.cc` (about 120 lines) builds a transform node per object,
  links parents, drivers and users' graphs, and evaluates in one batch. It shows that the API in "What
  P5c uses" is enough. Its transform payload is a 1-D toy.

### Lifetimes
- `docs/embedding.md`, "Graphs": GraphInstances, then standalone GraphDefs, then the Library, then the
  Runtime. So `SceneDrivers`, which holds one of each, must go before its Library.
- `Library::size()` moves when a record is made or destroyed. `Library::removedCount()` moves for every
  destroyed record (`lib/gc.cc:251`, `lib/lifetime.cc:202`).

## Measured
Provisional: the machine was busy (other sessions of the user were building), native-clang
RelWithDebInfo, Intel Core Ultra 9 386H, 2026-11-25, code of `8ca89b9`.

### P3's direct pass (`node tools/bench.mjs scene -- --reps 15`)
100,000 objects, each with a data block, in chains of four. Best and median, ms.

| Case | Best | Median | P3 target |
|---|---|---|---|
| unchanged | 16.4 | 18.1 | 20 |
| every root moved | 17.1 | 19.2 | 20 |
| epoch moved (selection) | 23.9 | 25.2 | 40 |
| structural (a parent) | 64.8 | 66.7 | 80 |
| structural (unlink, link) | 62.4 | 64.2 | 80 |

### The graph with a node per object (`node tools/bench.mjs graph`)
The scene tree of `bench_graph`, which has a 1-D toy payload; a real `TransformState` is heavier. ms.

| Case | 10,000 | 100,000 |
|---|---|---|
| first evaluation | 18.7 | 170 |
| unchanged | 0.29 | 5.4 |
| a leaf's block | 0.28 | 5.0 |
| the root's block (runs all) | 8.0 | 86 |
| `touch`: everything verified, as after an epoch move | 8.8 | 70 |
| a leaf put under the root (structural) | 15.8 | 130 |
| memory per node, with handles and paths | 1,848 bytes | 1,848 bytes |

- At 100,000 objects, an epoch move (every selection click) would cost 70 ms or more against P3's
  target of 40, a structural change 130 ms against 80, and the graph would take 185 MB.

### A scratch prototype (`proto_p5c.cc` in the session scratchpad, not committed)
The scene of `bench_scene` (100,000 objects, 200,001 blocks), 21 repetitions, ms.

| What | Best | Median |
|---|---|---|
| `Library::forEach` over all blocks, counting scenes | 1.67 | 1.71 |
| copying a store of 100,000 `ObjectEval`s (304 bytes each) | 2.02 | 2.12 |
| copying 1,000 of them at their places | 0.010 | 0.011 |
| 25,000 notifications, with a listener that looks up a `Map<BlockId, int>` | 1.95 | 2.03 |
| 100,000 `localBounds` through `Library::query` | 3.39 | 4.32 |
| 100,000 `localBounds` through kept `IfaceRef`s | 0.33 | 0.34 |
| evaluateScene, unchanged | 12.9 | 15.0 |
| 1,000 `computeState` of objects at depth 3 | 1.23 | 1.26 |

- A scan for scenes on every epoch move would add 1.7 ms to each selection click. "Finding the scenes"
  below avoids most of them.
- Copying the whole store, as `evaluateAll` does, costs 2 ms. Part B copies only the places that changed.
- Asking every data block for its bounds through kept tables costs 0.3 ms, so Part B can keep P3's rule
  (bounds of a type without Invalidation are asked for in each evaluation) and still be fast.

## Key decisions
1. **The generated graph holds only the objects whose state a graph reads, and their ancestors.**
   - Architecture §8 has a transform node for every object. The numbers above show that this costs 70 ms
     per epoch move and 185 MB at 100,000 objects.
   - The set `G` is closed under parents. Every other object goes through P3's direct pass, with the
     driven channels from the provider.
   - The world matrices that graphs read are still graph outputs, computed in the same evaluation, so
     there is no frame of lag when a driver's parent is driven (architecture §8's reason).
   - An object in `G` is evaluated twice per pass, by its node and by the direct pass. Both call
     `evaluateObject` with the same channels, overrides, time and parent state, so they agree bitwise for
     models that are pure, as `TransformModel` requires. A test checks it.
2. **Graphs that drive objects are listed by scenes,** (*Superseded on 2026-11-26:* scenes no longer list graphs; see "The document's list".) in `Scene::graphs` (OptStrong cells to graph
   blocks), and they drive the document: `SceneDrivers` serves the union of the lists of the Library's
   Live scenes.
   - The P3 contract asks for one provider per Library, whose answer doesn't depend on a scene. A list per
     scene that only drove that scene's objects would break it, because an object can be in two scenes.
   - The alternatives were a document block that holds the list (no such block exists, and an append of
     two files would give two), and every graph block with adapters (a graph used as a group inside
     another would then drive twice). This is open question 1.
3. **`SceneDrivers` is the provider, and the host owns it.** It is made with a Library, calls
   `setOverrideProvider`, holds the generated `GraphDef` and its `GraphInstance`, and must be destroyed
   before the Library (embedding.md's destroy order). An inner provider (a host's animation) can be
   combined behind it.
4. **The apply phase is `OverrideProvider::prepare`.** evaluateScene calls it after the pending writes and
   before the `EvaluationScope`. It syncs the generated def, sets the time, evaluates all driver sockets in
   one batch, and copies their values into a table by object. During the pass, `overridesOf` answers from
   that table. `prepare` also says which objects the provider drives, so the pass asks it only for those.
   - **Changes are posted as notifications,** as P3's contract asks: whenever an evaluation of the driver
     sockets, by `prepare` or by `overridesOf`, moves an object's values, `SceneDrivers` notifies the
     object's block with the aspect `Driven`. So no caller can use up a change that another one needs
     (pressure test, B1).
   - Outside a pass (commands, drags, `computeState`), `overridesOf` evaluates first if anything moved,
     which costs little when nothing did. Inside a scope with the wrong time or an older document, it
     fails with FailedPrecondition, as the contract allows.
5. **A transform node type per transform type** (`scene.transform.<type name>`), registered when the type is
   first needed. Its inputs are `parent` (a `coredcc.TransformState` payload), `time`, and one input per
   channel def, named `c_<channel>`, so that no channel name collides with `parent`, `time`, `length` or
   `active` (pressure test, B9). An Enum channel is an Int input, its default converted too. A linked
   channel input is an override; an unlinked one means the
   stored value. So each driven channel is one link, and links are type-checked by the graph.
6. **Model failures stay inside the kernel** (P3's rule): a failed model gives the parent's state and a
   status output (`EvalStatus` as an Int), not a graph error. **Driver failures are graph errors:** what a
   graph reads downstream of a failed driver fails too, and in the direct pass the driven object is
   ModelFailed and stands where its parent is.
7. **Driver errors reach commands as FailedPrecondition,** so a command that works on many objects leaves
   out the ones whose driver fails (P3 contract), and doesn't fail as a whole.
8. **Part B relies on the notification rule** that P5 and `embedding.md` state: what a block answers to
   `queryBlock`, including the values its slots return, changes only with a notification of the block or a
   move of the Library's epoch. P3's evaluator didn't rely on it. `SceneSettings::incremental = false`
   goes back to full passes, to find a missing notification.
9. **No litestl changes** are planned.

## Design, part A: graphs that drive objects

### Where graphs are listed (`scene/`)
- *Superseded on 2026-11-26:* scenes no longer list graphs; see "The document's list".
- `Scene` gets `graphs_`, a list of OptStrong `RefCell`s, enumerated by `foreachCell`, saved as the entry
  `graphs` (a list of references) and read back. Old files have no such entry: the list is empty.
- `scene/` doesn't look at what the cells point to. The datapath has a read-only collection `graphs`;
  the commands that change it are `drive/`'s, which know graphs.
- `Scene::graphs(const Library &)`: the Live targets, in order, without doubles. Cached by the
  Library's epoch, as `objects()` is.

### Finding the scenes (`drive/`)
- *Since 2026-11-26* it finds the list blocks instead (`DriverList::allLists`), in the same way.
- `SceneDrivers` keeps the handles of the Library's Scene records, found by `Library::forEach`.
- It scans again only when `Library::size()` or `Library::removedCount()` differ from the last scan, or
  after `notifyAll`. A record that is made or destroyed moves one of the two.
- **Records can change type in place:** `createWithId` or `reviveAs` on a MissingBlock placeholder turn it
  into a Live block of another type, and move neither counter (pressure test, B5). So the scan also keeps
  the handles of placeholder records, and each sync checks their types and the states of the known scenes.
  There are few of both.
- On each sync it reads the graph cells of the known Live scenes (a few cells each), so a change to a
  list is seen without a scan.

### Node types for users' graphs (`drive/node_types.cc`)
All of them are native, registered by `registerDriveTypes(rt)`, with the prefix `scene.` (reserved for
scripts, as `core.` and `graph.` are).
- **`scene.time`** (flag `InputAdapter`): input `time` (Float), output `seconds`. `SceneDrivers` links the
  input to the generated def's interface input `time`. Unlinked (a graph evaluated by itself), it is the
  literal, 0 by default.
- **`scene.object.state`** (flag `InputAdapter`): param `object` (Weak Pointer, struct
  `coredcc.SceneObject`), input `state` (payload `coredcc.TransformState`); outputs `world` (Mat4),
  `location` (Vec3, the matrix's translation), `rotation` (Quat), `scale` (Vec3, the local scale) and
  `status` (Int, an `EvalStatus`). `SceneDrivers` links `state` from the object's transform node. A null
  state fails with NotFound, naming the object: it isn't there, it isn't Transformable, or the graph is
  evaluated without `SceneDrivers` (a preview of the user's graph alone can't read objects).
- **`scene.drive.<kind>`** for bool, int, float, vec2, vec3, vec4 and quat (flag `OutputAdapter`): params
  `object` (as above) and `channel` (String, a channel def's name); input `value` of that kind, output
  `value`, passed through. An Enum channel is driven by `scene.drive.int`, a Bool channel by
  `scene.drive.bool`.
  - The adapter's kind must be the channel def's kind (Enum counts as Int). Otherwise it is a diagnostic,
    and the object fails.
- **`graph/node_type.h`** gets `NodeTypeFlags::InputAdapter = 1 << 2`, so that readers are found at the
  cost of the flagged nodes. Nothing else in `graph/` changes. (`forEachNode(flags)` costs every node with
  any flag, ThreadSafe included; no built-in type has that one yet.)
- **The scan goes by type name, not by flag alone:** a script may register a node type with
  `OutputAdapter`. Only the types above count, and the prefix `scene.` is reserved for scripts in
  `bind/graph_facades.cc`.

### The adapters of a graph (`drive/adapters.cc`)
- For each listed graph, `SceneDrivers` needs its adapters with their node paths inside it, through nested
  groups. Interior socket keys take such paths (`@3/7.value`).
- **Each def is scanned once,** with `forEachNode(OutputAdapter)`, `forEachNode(InputAdapter)` and its group
  nodes. The result is cached by the def's `serial` and `contentVersion`, and the Library's epoch (a cell
  write moves only that). A nested group's list is the child def's list, with the group node in front.
- Recursion is bounded by P5's depth limit (64) and by the groups the graph already refuses.
- An adapter inside a foreach body isn't reachable by an interior key: it is a diagnostic.
- **Diagnostics** (`SceneDrivers::diagnostics()`): graph, node path, message. For an object that isn't
  Transformable or isn't there, a channel that the type doesn't have, a kind that doesn't match, a channel
  driven twice, a link that would make a cycle, and an adapter in a foreach body.
- **A channel driven twice** includes one subgraph used as two group nodes, or a listed graph that is also
  nested in another listed graph: both copies drive it, and the object fails with a diagnostic.

### The generated def (`drive/generated.cc`)
A standalone `GraphDef` with the Library, edited directly (no block, no undo), as P5 planned.
- **Interface:** an input `time` (Float).
- **One group node per listed graph**, in the order of the scenes and their lists, each graph once.
- **`G`:** the objects named by readers, and all their ancestors, found with `Transformable::read`. Parents
  that aren't Live or aren't Transformable end the walk, as in `computeState`.
- **A transform node per object of `G`,** of the node type of its transform type, with params `object`
  (Weak Pointer), `parentId` (the parent id it was linked for) and `cycle` (Bool). Links:
  - the parent's `state` into `parent`, unless the object is on a cycle of parents (then `cycle` is set
    and it has none, as P3 does);
  - the root input `time` into `time`;
  - `@<adapter path>.value` of a group into the channel input of a driven object of `G`.
- **Into users' graphs:** the root input `time` into `@<path>.time` of each `scene.time`, and an object's
  `state` into `@<path>.state` of each reader.
- **Sync** runs in `prepare`, and in `overridesOf` outside a pass, when any of these moved: the Library's
  epoch, the scene scan's key, a listed def's `contentVersion`, the transform registry's count, or a
  transform node reported that it is stale (below). It computes the wanted nodes and links, and edits
  only the difference, with `linkAll` for the new links.
- **Links come in a fixed order, and parent links are never dropped** (pressure test, B4). The order:
  parent links, time links, reader links, then driver links by scene, list place and node path. When
  `linkAll` refuses a batch for a cycle, every driver and reader link is removed and they are made again one
  at a time in that order; those that would close a cycle are left out and reported. So which link is left
  out depends on the document, not on the order of the edits, and a load gives the same graph.
- **Objects of a transform type that isn't registered** get a node of the type `scene.transform.missing`,
  which has no channel inputs and gives its parent's state with MissingModel, as the direct pass does
  (`object.cc:217-237`). The walk up through it goes on, so their children have the right parent.
- **A stale node:** the kernel compares what `read` gives with its params. Another parent or another
  transform type fails it with FailedPrecondition and a fixed message. The sync then rebuilds that part,
  and the evaluation runs once more. A second stale answer in one call is the error, as P3 has
  "an object's `read` answers otherwise each time".

### The transform kernel (`drive/transform_nodes.cc`)
- `paramBlock("object")`, then `queryBlock<Transformable>`, then `read(info, channels)`. Not Live, or not
  Transformable: NotFound.
- Checks `info.parent` against the param `parentId`, and `info.transformType` against its node type's.
- Uses the object's block with every aspect but `Driven`: it reads stored channels, and the driven values
  come through its links. So `SceneDrivers`' own notifications don't run it again.
- Overrides: for each linked channel input, its value, flattened into a `ChannelOverrides`.
- `evaluateObject(rt, type, EvalCtx(channels, time, overrides), parent, out, error)`: the parent's state
  and ModelFailed on an exception or a non-finite result, as P3. An exception (the sticky error) is taken
  by the kernel. evaluateScene reports it from the direct pass, which calls the model too.
- Outputs `state` (payload, equal bitwise) and `status` (Int). `cycle` turns an Ok status into Cycle.
- A null `parent` payload (not linked) is the identity.

### The provider (`drive/provider.cc`)
- **The table:** for each driven object (by BlockId), its entries: channel def name, kind, and the
  `SocketHandle` of the adapter's output inside its group (`SocketPath` with the group's path).
- **`prepare(lib, time, driven)`:** call `lib.find(BlockId())` first, so that a pending renormalization of
  ids moves the epoch now and not in the middle of the pass (pressure test, B7); sync if needed;
  `setInput("time")` if it differs; one `evaluate(handles, moved)`; for the entries that moved, read the
  values and notify their objects (aspect `Driven`). It fills `driven` with the objects it drives.
  - A sticky error that a kernel or a foreign slot left is taken here and becomes `prepare`'s error, so
    the pass doesn't blame the first object for it.
  - A call that fails as a whole (Busy, the budget) is kept, and every driven object then fails with it.
    They are ModelFailed, which the direct pass evaluates again in each pass, so they recover when the
    drivers do.
- **`overridesOf(lib, object, type, time, out)`:**
  - Inside an `EvaluationScope`: if the table is of this time and of the document as it is, it answers
    from it. Otherwise FailedPrecondition. "As it is" is the graph's own pass key (`GraphPassKey`: the edit
    counter, the Library's epoch, the client generation and the number of interface tables), with the time.
  - Outside one: `prepare` with this time first. Its cost when nothing moved is the key compare.
  - Each entry's channel index comes from the type's channel defs, cached by (type, name). A missing name
    or another kind is FailedPrecondition. A socket in error is FailedPrecondition with the graph error's
    origin and message. A channel driven twice is FailedPrecondition naming both.
  - The inner provider, if any, is asked first; the graph's values win where both drive a channel.
- **Change reports:** notifications of the driven objects, as above. They are posted outside any
  `EvaluationScope` (in `prepare`, or in `overridesOf` called from a command), where notifying is allowed.
  A notification of the aspect `Driven` alone runs no transform kernel, and the scene evaluator's listener
  marks the object dirty (part B).

### The `OverrideProvider` change (`scene/overrides.h`)
```cpp
class OverrideProvider {
public:
  /** Called by evaluateScene before its pass, outside the EvaluationScope, for the scene's time.
   * `driven` gets the objects that it drives, or `all` if it can't say. The pass asks only those. */
  virtual Status prepare(const Library &lib, double time, DrivenObjects &driven);   // default: all = true
  virtual Status overridesOf(...) = 0;                                             // as now
};
struct DrivenObjects { bool all = true; uint64_t version = 0; litestl::util::Vector<BlockId> ids; };
```
- evaluateScene calls it after `PendingWrites::flush` and before `beginNoCollect`. An error doesn't stop
  the evaluation: `overridesOf` reports it for each object it affects.
- **Why a set of driven objects:** the pressure test measured the provider call itself at 0.2 to 1 ms per
  pass at 100,000 objects, with a no-op provider. The evaluator keeps a flag per member, made again when
  `version` or its lookups move, and calls `overridesOf` only for flagged members. It is a set, not a list
  of changes, so no caller can use it up.

### Commands and paths (`drive/commands.cc`)
- *Superseded on 2026-11-26:* the commands are `core.drivers.addGraph` and `core.drivers.removeGraph`,
  without a `scene` input; see "The document's list".
- `core.scene.addGraph(graph)` and `core.scene.removeGraph(graph)`: Pointer inputs; the scene is the
  Context's, or the optional input `scene`. Undo through the cells, as membership commands do. `addGraph`
  refuses a block that isn't a `coredcc.GraphDef` (InvalidArgument) and one that is listed
  (AlreadyExists).
- The Scene struct's `graphs` collection is read-only by path; its items are the graph blocks.

## Design, part B: an incremental direct pass

### What makes an object dirty
- **A notification of its block** (the ChangeBus listener, which already exists for bounds): its transform,
  and through it its descendants'. A notification of a data block: the bounds of the objects that use it,
  as today for Invalidation types.
- **The provider's notifications:** the objects whose driven values moved (aspect `Driven`). A provider
  without `prepare` notifies, as P3's contract asks.
- **Objects whose last status was ModelFailed:** each pass, so that an exception is reported each time, as
  now.
- **Bounds of a type without Invalidation, or behind an override:** asked for in each pass, as P3 does.
  This is a kept table call (0.3 ms at 100,000), and the world box is made again only when the local
  bounds or the transform changed.
- **Everything (a full pass, as P3):** a move of the Library's epoch or of the lookup key, `notifyAll`, the
  scene's time (compared with the last pass's, since `Scene::frame` is a field that may be written without
  a notification), a move of the driven set's `version`, `SceneSettings::incremental == false`, and the
  first evaluation.
  - An epoch move must stay full: the notification rule allows any block's answers to change with it. So
    a selection click stays at P3's 24 ms.
  - A time change stays full: models and providers may read the time without saying so.

### How it runs
- One walk over `order_` (parents first): an object is evaluated if it is dirty or its parent's state
  changed in this pass. A state that comes out bitwise the same stops the walk below it.
- `evaluateMany` batches are made from the dirty objects of each level, as now from all.
- **Data pointers** are kept with the lookups until the epoch moves. `embedding.md` says that a block's
  data pointer changes only between `onHostDestroyed` and `onHostRestored`, which move the epoch.
- **The back store** is brought up to date from the front store only at the places that the last pass
  *that swapped* wrote, and at all places that a pass ended as Stale wrote, before this pass writes its
  own (pressure test, B6). No full copy (2 ms at 100,000). A full pass, or other members, copy everything.
- **SceneBvh** refits the leaves of the changed places (`Bvh::refitPositions`) only while they are few.
  The pressure test measured, at 100,000 boxes: a full refit 0.29 ms; a partial one 0.23 ms for 1,000
  boxes, 3.5 ms for 25,000 and 10.5 ms for all (B3). So above 1% of the boxes it refits all.
- **`growth()`** (0.76 ms at 100,000, called on every refit by `SceneBvh::update`) is measured after a full
  refit, and after partial ones once they have touched 10% of the boxes since the last measure. Today's
  rule for a rebuild is otherwise kept.
- `lastChanges`, `EvaluateResult` and the `evaluated` notification are as now.

## API (C++)
```cpp
// drive/drive.h
Status registerDriveTypes(Runtime &rt);   // the payload type, node types, commands; after the scene and graph types

class SceneDrivers final : public OverrideProvider {
public:
  /** Becomes the Library's provider; fails as setOverrideProvider does. Destroy it before the Library. */
  static Result<std::unique_ptr<SceneDrivers>> make(Library &lib);
  ~SceneDrivers() override;                         // clears the provider
  void setInner(OverrideProvider *inner);           // asked first; may be null
  const litestl::util::Vector<DriverDiagnostic> &diagnostics();   // syncs first; Busy-safe
  /** The objects of G, and the instance: for tests and a node editor. */
  const GraphInstance &instance() const;
  int generatedObjects() const;
  Status prepare(const Library &, double time, DrivenObjects &) override;
  Status overridesOf(const Library &, BlockHandle, const TransformType &, double, ChannelOverrides &) override;
};
struct DriverDiagnostic { BlockId graph; NodePath node; litestl::util::string message; };

// drive/driver_list.h (2026-11-26; replaces Scene::graphs)
class DriverList { static BlockHandle find(const Library &); const Vector<BlockHandle> &graphs(const Library &) const; ... };
Result<BlockHandle> newDriverList(Library &);
void addDriverGraphsMember(ApiStruct &root);
// scene/scene_bvh.h: SceneSettings::incremental (default true)
```

## Bindings and TS
- **Facades** (`bind/drive_facades.{h,cc}`, BIND_FACADE): `enableDrivers(ctx)` and `disableDrivers(ctx)`
  (a Runtime extension keeps one `SceneDrivers` per Library, by `LibraryRef`; `disableDrivers` must come
  before the Library goes, and a host's `Session` calls it), `driverDiagnostics(ctx) -> string` (JSON).
- **Commands** run through the existing facades: `core.drivers.addGraph(graph=@block(...))`.
- **Graph editing** uses P5's facades: `graphAddNode(ctx, path, "scene.drive.vec3", x, y)`, and the param
  paths `nodes[#id].params.object` and `.channel`.
- **TS** (`bind/ts/scene.ts`): `enableDrivers`, `disableDrivers`, `addDriverGraph(ctx, graph)`,
  `removeDriverGraph`, `driverDiagnostics() -> DriverDiagnostic[]`.
- **wasm test** (`tests/wasm/drive.test.ts`): a graph that makes one object follow another's location,
  evaluated, undone, saved and loaded.

## Files (each under 35 KB)
```
drive/   drive.h  node_types.{h,cc}  transform_nodes.{h,cc}  adapters.{h,cc}  generated.{h,cc}
         provider.{h,cc}  commands.cc  builtins.cc  CMakeLists.txt
scene/   evaluator.{h,cc} (passes)  evaluator_lookup.cc (lookUp, stillValid, members; split from
         evaluator.cc, which is 22 KB and would pass 35 with part B)  evaluator_dirty.cc (part B)
         overrides.{h,cc}  scene.{h,cc}  scene_io.cc  scene_bvh.{h,cc}  evaluated.{h,cc}
graph/   node_type.h (one flag)
bind/    drive_facades.{h,cc}  manager.cc (one line)  ts/scene.ts
tests/   drive/  scene/test_scene_incremental.cc  wasm/drive.test.ts  bench/bench_drive.cc
```
- The new target `coredcc_drive` uses `graph` and `scene`; `bind` uses it. `docs/modules.md` gets its row.

## Tests
- **`test_drive_nodes`:** each node type by itself, through a GraphInstance: readers, drivers of each kind,
  time; the transform kernel against `computeState` for TRS, FBX (all three inherit types) and Matrix,
  with overrides; model failures as a status; stale parents and types.
- **`test_drive_basic`:** the stand-in's cases on real objects: parents, a driver with time, a reader that
  drives another object, errors and what depends on them, delete and undo of an object, of a graph, and
  of a scene; garbage collection (the generated def keeps nothing alive).
- **`test_drive_provider`:** commands see driven channels: `applyDelta` and drags (`driven` counted),
  `setWorld` EXACT and CLOSEST, `setParent` with keepWorld, `setTransformType` baking; `computeState` of
  a driven object outside a pass; a command that writes an object a graph reads, then computes another;
  inside a scope with another time (FailedPrecondition); an inner provider.
- **`test_drive_sync`:** listing and unlisting graphs, undo and redo; two scenes listing one graph; a graph
  in a deleted scene; nested groups; adapters added, removed, retargeted by path and by a cell write;
  transform types registered late; an object's type changed; reparenting inside and outside `G`.
- **`test_drive_cycles`:** a reader of a child that drives its ancestor (refused at link, reported); a cycle
  made later inside a user graph; a hierarchy cycle from undo.
- **`test_drive_random`:** random documents, graphs and edits (with undo and redo), checked against a naive
  oracle that computes every object's state with `computeState` and a provider that evaluates every
  driver graph from scratch with a fresh GraphInstance. States compared bitwise; the states of `G`
  compared with the direct pass's; diagnostics compared.
- **`test_drive_files`** and **`test_drive_list`** (2026-11-26): the list saved and loaded; old files; append; a graph block that is a
  placeholder.
- **`test_drive_lifetime`:** `SceneDrivers` destroyed before the Library; a second provider refused;
  `disableDrivers` from scripts. `SceneDrivers` holds a `LibraryRef`; when the Library is gone, its
  destructor drops the instance and the def and doesn't call `clearOverrideProvider`. The facades' Runtime
  extension checks the ref on each call.
- **`test_scene_incremental`** (part B): random edits that notify, against the same document evaluated with
  `incremental = false`, bitwise, with `lastChanges` compared; the back store across passes that don't
  swap and passes that end Stale; growth (the cost of one moved object doesn't grow with the scene).
- **P3's scene tests** pass with `incremental` on, after step 8 adds the notifications they lacked.
- **Cases from the pressure test**, each a test: a change used up by `computePivot` before evaluateScene (B1);
  a reparent that closes a cycle with a driver, compared with a fresh load (B4); a Scene made from a
  placeholder (B5); two scenes that share an object; a duplicated scene; `disableDrivers` forgotten before the
  Library goes.
- **Mutation checks** for each step, as in P3 and P5.

## Benchmarks
- **`tests/bench/bench_drive.cc`** (`node tools/bench.mjs drive`). 100,000 objects as in `bench_scene`; 10
  driver graphs that drive 1,000 objects and read 100, so `G` is about 400 objects.
- **`bench_scene`** gets "one object moved" and "a root with 1,000 below moved", and "every root moved"
  notifies as hosts must.

| Case, 100,000 objects | Target |
|---|---|
| Part A, without `SceneDrivers` | P3's numbers within noise |
| `SceneDrivers` without graphs: unchanged; epoch moved | +0.1 ms; +0.3 ms |
| With the drivers: unchanged | P3's unchanged + 1 ms |
| With the drivers: a read object moved | P3's unchanged + 1 ms |
| With the drivers: the time changed | P3's unchanged + 3 ms |
| With the drivers: epoch moved | P3's epoch moved + 3 ms |
| `computeState` of a driven object outside a pass, nothing moved | 5 µs |
| Part B: unchanged | 2 ms |
| Part B: one object moved | 2 ms |
| Part B: a root with 1,000 below moved | 3 ms |
| Part B: every root moved; epoch moved; structural | P3's targets: 20; 40; 80 (with a full refit, above 1% of the boxes) |

Targets are for an idle machine. Numbers from a busy one are recorded as provisional.

## Order of work
Each step is a commit that passes `node tools/gate.mjs pre-commit --jobs 2`.

| Step | What | Needs |
|---|---|---|
| 1 | `Scene::graphs` (cells, entry, path); `OverrideProvider::prepare` and its call; `NodeTypeFlags::InputAdapter` | — |
| 2 | `drive/`: the payload type, `scene.time`, `scene.object.state`, `scene.drive.*`, the transform node types and kernel; `test_drive_nodes` | 1 |
| 3 | Adapter scan with nested groups and diagnostics | 2 |
| 4 | The generated def and its sync; `SceneDrivers` as the provider; the commands; basic, provider, sync and cycle tests | 3 |
| 5 | The random test, files, lifetime | 4 |
| 6 | Bindings, TS, the wasm test | 4 |
| 7 | `bench_drive`, targets of part A; docs for part A (`embedding.md`, `debugging.md`, `modules.md`) | 4 |
| 8 | Part B: split `evaluator.cc`; dirty tracking; the back store by places; SceneBvh partial refit and `growth` by fraction; `incremental`; notifications in P3's tests; `test_scene_incremental`; `bench_scene` cases | 1; 4 for its cases with drivers |
| 9 | Mutation checks; the feature review "scene evaluator" and its fixes | all |

- Step 8 needs only step 1 for scenes without drivers, so it can go before step 5 if P4 needs drags to be
  fast sooner. Its tests with drivers come after step 4.

## Exit criteria
- The gate passes; wasm-st and wasm-mt build, and the drive and scene tests pass in node.
- The random tests agree with their oracles over at least 1,000 seeds in clang Release and MSVC Release, and
  at the default scale in every gate.
- The benchmark targets above, measured on an idle machine, or recorded as provisional with how busy it was.
- A driver graph built and evaluated from TS in wasm-st.
- `docs/embedding.md` says how to enable drivers, the destroy order, and that evaluateScene relies on
  notifications (part B). Its rule for providers ("post a notification when its answers change") stays as
  it is. `docs/debugging.md` says what to do when an object doesn't follow its driver.

## Deviations from the architecture
Recorded in the architecture when the phase is done.
- §8: a transform node only for the objects that graphs read and their ancestors, not for every object.
- §8: the scene evaluator doesn't own the graph; the Library's `SceneDrivers` does, and each scene's evaluator
  asks it through `OverrideProvider`.
- §7: the apply phase is `OverrideProvider::prepare`, and the evaluated state of an object comes from the
  direct pass.

## What was built differently
Kept as the work goes; each item says what changed from the design above and why.
- **Files:** `drive/` has `node_types`, `adapters`, `sync.cc` (the description and the generated def),
  `provider.cc`, `commands`, `builtins.cc` and an internal `state.h`. There is no `transform_nodes` or
  `generated` file: the kernels are small enough for `node_types.cc`.
- **Steps 2 to 4 are one commit.** The node types, the scan and the provider were written together and
  tested together.
- **The generated def is made again from scratch** whenever its description differs, instead of being
  edited by difference. The description is the listed graphs with their adapters, the objects of `G` with
  their parents, types and cycles, and the scan's diagnostics. Equal descriptions give the same graph, so a
  document gives the same graph whatever the order of its edits (B4 holds by construction). It is cheap at
  the sizes planned (400 objects of `G`); an object moved or the time changed doesn't rebuild.
- **Links are made one at a time, in the fixed order** (parents and time with `linkAll`, then readers and
  drivers by graph and node), not as one batch with a retry. A link that would close a cycle is left out and
  reported.
- **Transform nodes have a String param `error`.** When `SceneDrivers` fails an object of `G` (a channel the
  type lacks, another kind, driven twice, a cycle), its node fails with that message, so a graph that reads it
  fails too (decision 5). The direct pass fails it with the same message.
- **Readers have an Int input `status`,** linked from the transform node's `status`, so `scene.object.state`
  gives the object's EvalStatus.
- **The inner provider is asked by the transform kernel too** (through `GraphInstance::setUserData`), so the
  states of `G` agree with the direct pass when animation drives an object that a graph reads. A failure of
  the inner provider fails the node.
- **`SceneDrivers::update(time)`, `generatedState(id)`, `transformNode(id)`, `transformCount()` and
  `rebuilds()`** are there for tests, tools and benchmarks. `instance().value()` gives only the sockets that
  were asked for, so the states of `G` are asked for too.
- **The sync key** is the Library's epoch and size and removed count, the client generation, the number of
  interface tables, the transform and node type counts, and the edit counter. A notification moves the edit
  counter, so a sync after an edit reads the description again (a few cells and the cached scans), and
  rebuilds only if it differs.
- **`test_drive_random`'s oracle** is the same document saved, loaded and given a `SceneDrivers` of its own,
  instead of a naive evaluator: it checks the incremental sync against a sync from scratch, and the states of
  `G` against the direct pass, bitwise.
- **The description has the driven objects' transform types,** so a channel that a type lacks, or of
  another kind, is a diagnostic for every driven object, not only those of `G`, and a type change is seen.
  A driven object whose type isn't registered gets a diagnostic, but isn't failed: it is MissingModel, as
  in the direct pass.
- **Bindings (step 6):** `bind/drive_facades.{h,cc}` keep the drivers in a Runtime extension
  (`ScriptDrivers`); a TS `Session` calls `disableDrivers` when it closes. `addSceneGraph` (now `addDriverGraph`) and
  `removeSceneGraph` run the commands. `test_drive_facades` tests the facades natively.
- **The wasm test (`tests/wasm/drive.test.ts`) wasn't run here:** this worktree has no emsdk, and only the
  pre-commit gate runs locally; CI runs the wasm suites. It was type-checked, with the TS layer, against the
  TS that the native `coredcc_gen_ts` generates (the test app's own types stubbed).
- **Part B (step 8):**
  - `evaluator.cc` (28 KB) isn't split: it stays under 35 KB. The dirty tracking is in `evaluator_dirty.cc`.
  - The incremental walk is inside `evaluateAll`, with a flag: an object is evaluated if it was notified, its
    parent's state moved in this pass, or it was ModelFailed; otherwise only its bounds, if they aren't kept.
  - **A pass is full when evaluateMany types are registered:** their batches are whole levels. Such
    documents keep P3's costs.
  - **Each member's Spatial is kept** from its last full visit when it is a table's (the data pointer stays
    until the epoch moves). A notification of a data block drops the kept Spatial of the objects that use
    it, since P3's tests move a data pointer without an epoch move, with a notification.
  - Kept boxes that weren't valid when a pass began are asked for by every object that shares them
    (`boxStale_`): the first one's call makes the box valid again.
  - Notifications that arrive while a pass runs leave their objects dirty for the next one.
  - **P3's tests:** the fixture's `obj(h)` and `host(h)` notify, as `shape(h)` already did, and `objRaw`
    and `hostRaw` don't. Listeners that read objects use the raw ones. `test_scene_cache` notifies the
    blocks whose data it swaps; `test_scene_overrides` notifies when its provider's answers change.
  - The box-by-box BVH refit is tested in `test_scene_incremental`'s growth case: the random documents are
    too small for it (it needs fewer than 1% of the boxes).
- **After the merge, CI (2026-11-26):** a graph param that points to `coredcc.SceneObject` accepts a pointer
  to a struct that inherits from it (TS Refs to objects are `coredcc.Object`), in `GraphDef::setParamRef`.
  Scripts get `notifyChanged(ctx, block)`, since the incremental pass evaluates only what was notified; P3's
  TS test of throwing models uses it.
- **The document's list (2026-11-26, `a9604a5`)** replaces `Scene::graphs`, as designed in "The document's list", with
  the pressure test's changes. `scene/` is back to what it was before P5c, apart from part B. P3's
  `test_scene_graphs` keeps only its `prepare` cases, as `test_scene_prepare`; the list's tests are in
  `test_drive_list`. The commands' `scene` input added after the review is gone again.
- **After the review** ([feature report](../reports/feature-report-scene-evaluator-2026-11-25.md), `46a09ee`):
  - The commands take an optional `scene` input (the Context's scene when unset), as planned; the first build
    lacked it. Undo of `removeGraph` brings back every cell it removed, at its place.
  - Notifications come when the graphs are evaluated, not when they are edited (documented in `drive.h`).
  - The missing type's node type is `scene.missingTransform`. A transform node type that drive/ didn't
    register isn't used, and `scene.` is reserved for script node types.
  - `generated()` and `instance()` return pointers; `diagnostics()` syncs and returns a copy;
    `SceneDrivers::of(lib)` finds the Library's drivers, whoever made them.
  - `kDrivenAspect` is in `api/change_bus.h`.
- **Not done yet:** a Scene made from a placeholder (B5) has code but no test; the foreach diagnostic has no
  test; the wasm suites haven't run with part B or with the review's fixes; scripts can't set an inner
  provider; the placeholder refusal of the list's commands has no test (making a list a placeholder needs a
  file loaded before drive/'s types are registered).

### Measured, part A (`node tools/bench.mjs drive -- --reps 15`, busy machine, provisional)
| Case, 100,000 objects | Best ms | Target |
|---|---|---|
| No drivers: unchanged; epoch moved | 15.7; 22.4 | P3's numbers |
| `SceneDrivers` without graphs: unchanged; epoch moved | 15.5; 21.9 | +0.1; +0.3 |
| With the drivers (G 400 objects, 1,000 driven): unchanged | 15.8 | P3's + 1 |
| A read object moved | 16.8 | P3's + 1 |
| The time changed | 19.6 | P3's + 3 |
| Epoch moved | 22.1 | P3's epoch moved + 3 |
| `computeState` of a driven object, nothing moved | 0.0004 | 0.005 |

- `test_drive_random` passes 1,000 seeds in clang Release (`COREDCC_TEST_SCALE=25`, 5 s).

### Measured with part B (busy machine, provisional)
`node tools/bench.mjs scene -- --reps 15` and `node tools/bench.mjs drive -- --reps 15`, best ms at 100,000
objects:

| Case | Before part B | With part B | Target |
|---|---|---|---|
| bench_scene: unchanged | 16.4 | 2.7 | 2 |
| One object moved | (new) | 2.6 | 2 |
| A root with 1,000 below moved | (new) | 2.7 (median 5.7) | 3 |
| Every root moved (notified) | 17.1 | 7.6 (median 21.7) | 20 |
| Epoch moved (selection) | 23.9 | 21.5 | 40 |
| Structural: a parent; unlink and link | 64.8; 62.4 | 65.1; 65.7 | 80 |
| bench_drive, drivers: unchanged | 15.8 | 2.7 | P3's + 1 |
| A read object moved | 16.8 | 3.6 | P3's + 1 |
| The time changed (a full pass) | 19.6 | 21.3 | P3's unchanged + 3 (19.4) |
| Epoch moved | 22.1 | 24.6 | P3's epoch moved + 3 (26.9) |
| `computeState` of a driven object | 0.0004 | 0.0004 | 0.005 |

- The time change is a full pass by design, so it is measured against P3's full pass; it misses its target
  by 2 ms on this machine. Medians spread widely because the machine is busy.

### Mutation checks (step 9)
Each was applied alone, built and run; each made a test fail.
- Part B: children not evaluated after their parent moved; bounds never compared (always "same"); the back
  store not brought up to date; the box-by-box BVH refit skipped (caught only once the growth case asked the
  BVH; the random documents are too small for that path).
- drive/: no `Driven` notification; linked channel inputs ignored by the kernel; the `error` param ignored;
  no second evaluation after a stale node (caught once its test wrote the host without a notification); a
  channel driven twice not detected.

## Risks
- **`G` can be all objects,** if a graph reads every object (a constraint on all of them). Then the graph's
  costs above apply: about 70 ms per epoch move at 100,000. Readers of many objects need M7 and a cheaper
  full pass in `graph/`. Measured by `bench_drive`'s "read all" case, not targeted.
- **Two evaluations of `G`** (graph and direct pass) cost twice for those objects, and must agree. A foreign
  model that isn't pure breaks the agreement; the direct pass's result is what is shown.
- **A driver graph at another time than the scene's** (two scenes at different frames) evaluates its
  time-dependent nodes again at each switch.
- **Part B changes what hosts can get away with.** A host that writes objects without notifying sees old
  results. `incremental = false` finds it.
- **P4's tests** may write channels without notifying (see "What P5c needs from P4").
- **A duplicated scene** copies its list of graphs (`scene_io.cc:153`), but the copied objects aren't driven:
  the adapters name the originals. Documented, not changed.

## Claims this plan depends on
1. Putting every object into the generated graph misses P3's targets (measured above, with a toy payload).
2. `OverrideProvider::prepare` can run between `PendingWrites::flush` and `beginNoCollect`, and evaluate a
   graph there.
3. A `SocketHandle` can be prepared for an output of an adapter inside a group, and through nested groups.
4. Linking `@<path>.value` of a group into a transform node's channel input, and an object's `state` into
   `@<path>.state`, gives no cycle unless the user's graphs and the hierarchy make one.
5. The graph's state of an object in `G` equals the direct pass's, bitwise.
6. `(Library::size(), Library::removedCount())`, the known scenes' and placeholders' states and `notifyAll`
   cover every way a Scene record can appear. (Broke without the placeholders: B5.)
7. Inside an `EvaluationScope`, `GraphPassKey` with the time says whether the table is of the document as it
   is, once `lib.find` has run before the scope. (Broke with a key of three parts: B7.)
8. An Enum channel can be an Int input, and every built-in channel kind has a socket type.
9. (Dropped: `Scene::frame` can be written without a notification, B8. Part B compares the time.)
10. With part B, every way that a Transformable's answers or a data pointer can change either notifies the
    block or moves the epoch.
11. The ChangeBus listener is called for every notification, including those made inside an
    `EvaluationScope` and those of `PendingWrites`.
12. `Bvh::refitPositions` gives the same tree as a full refit of the same boxes.
13. A `SceneDrivers` that outlives its scenes, or sees its Library's scenes deleted, does no harm.
14. A notification with only the aspect `Driven` runs no generated transform kernel, and so doesn't loop.
15. Partial SceneBvh refits below 1% of the boxes, and `growth` by fraction, keep "one object moved" within
    2 ms.

## Open questions for the user
1. **Where driver graphs are listed:** per scene, serving the document (this plan), or somewhere else.
2. **The deviation from §8:** only objects that graphs read get a node. Accept it?
3. **Part B in P5c,** or as a phase of its own after P4. It changes evaluateScene from "recomputes
   everything" to "relies on notifications".
4. **Readers downstream of a failed driver fail** (they don't read the driven object's fallback state). Is
   that the wanted rule?
5. **P3's tests get notifications** in step 8. They are P3's files; the alternative is to leave
   `incremental` off by default, which gives up part B's speed for hosts.

## litestl PRs
- None planned. If one turns out to be needed, it is a branch `coredcc/<topic>` in this worktree's
  `extern/litestl`, and a PR against `joeedh/litestl`, as the P0b and P3 PRs were.

## Pressure test record (2026-11-26): the document's list
A fresh agent got the design of "The document's list" and the code, not the author's reasoning. It worked
read-only.
- **Survived:** a block is the right home (the library section holds only blocks, and save, load, unread
  fields, GC roots, append and undo of a creation all work for one); an empty list and a placeholder list
  are kept and saved; the old scene field is harmless.
- **Broke, and changed:**
  - "The first Live list in the Library's order" isn't stable: slots are reused, and a load orders records
    by id. *Changed:* Root first, then the least id; the scan sorts the lists by id.
  - The commands edited one list while every list drives, so a graph in an appended list couldn't be
    removed. *Changed:* removeGraph and the AlreadyExists check cover every list.
  - A duplicated list (not Root) could be the one `find` chose, and wasn't saved; a list duplicated with its
    graphs drove every object twice. *Changed:* Root first, and a duplicate is empty.
  - A placeholder list next to a new one would come back driving. *Changed:* the commands refuse, and a
    diagnostic says so.
  - The pointer member `drivers` showed one list, and could be null. *Changed:* the collection
    `driverGraphs`, the union.
- **Kept:** reading `driverGraphs` walks the Library each time. Watchers of the root aren't notified when a
  list is made; they are notified of the list's `graphs`.

## Pressure test record (2026-11-25)
A fresh agent got the draft and the code, without the author's reasoning. It built probes in a scratch
folder (`pt-p5c\pt_probe.cc` and `pt_bench.cc` in the session scratchpad), as temporary targets that it
removed afterwards. Its timings are from a busy machine. The author read each finding against the code.

### Survived
- **evaluateScene's order** leaves room for `prepare` before the scope (`evaluator.cc:676-712`): claim 2.
- **The listener sees every notification at once,** `notifyAll` included (`change_bus.cc:222-238`): claim 11.
- **Drags and commands notify and don't move the epoch** (run): `beginInteractive`, each `updateInteractive`,
  `commit`, `applyDelta`, undo and redo each give one notification and no epoch move. So part B stays
  incremental through a P4 drag.
- **A partial refit gives the same tree** as a full one (`tests/bvh/test_bvh_refit.cc:366-367`): claim 12.
- **Interior keys and paths into nested groups exist,** and the cycle check goes through groups by
  sockets (`cycles.cc:73-168`): claims 3 and 4. A cycle made later inside fails at evaluation.
- **Channel kinds** are Bool, Int, Float, Enum, Vec2-4 and Quat (`channel_io.cc:10-28`): claim 8, with
  Enum as Int.
- **`lastEdit` is a sound key for what graphs read** (`graph/system.cc:53-83`), and `InputAdapter = 1 << 2`
  is free.
- **The full store copy and the full refit** are as the plan says (`evaluator.cc:574-579`,
  `scene_bvh.cc:9-27`). 185 MB is 1,848 bytes times 100,000.

### Broke, and what changed
- **B1, the list of changes from `prepare` could be used up** (severe, reasoned). `overridesOf` outside a
  pass called `prepare`, so `computePivot` from a gizmo, any command, or another scene's evaluation took
  the changes, and the next evaluateScene left a driven object where it was. *Changed:* `SceneDrivers`
  notifies the driven objects whose values moved (aspect `Driven`), whoever evaluated, as P3's contract
  asks. `prepare` gives the set of driven objects, which can't be used up. The transform kernel ignores
  `Driven`, so the notifications don't loop.
- **B2, part B breaks P3's tests** (severe, read). Raw writes without notification are in 22 files under
  `tests/scene/`, for example `test_scene_eval.cc:271-277` and `375-376`, and `test_scene_overrides.cc:266-271`.
  *Changed:* step 8 adds the notifications, and the ownership table says so. Open question 5.
- **B3, a partial BVH refit costs more than a full one when many boxes move** (measured, 100,000 boxes: full
  0.29 ms; partial 0.23 for 1,000, 3.5 for 25,000, 10.5 for all), and `growth()` costs 0.76 ms on every
  refit. *Changed:* partial refits below 1% of the boxes only; `growth` after full refits and after partial
  ones that touched 10%.
- **B4, which link the cycle check dropped depended on the order of edits** (medium, reasoned). Reparenting
  under a driver's target refused the parent link, and a fresh load would refuse the driver link.
  *Changed:* parent links are never dropped; on a cycle, reader and driver links are made again in a fixed
  order.
- **B5, a Scene can appear without moving `size()` or `removedCount()`** (run): `createWithId` on a
  MissingBlock placeholder (`adopt.cc:58-80`). *Changed:* the scan keeps placeholder records and checks
  their types on each sync.
- **B6, the back store copied too little** (medium, reasoned): after a pass with no change, which doesn't
  swap, the next one copied nothing, and an older place came back. *Changed:* it copies what the last pass
  that swapped wrote, and all that a Stale pass wrote.
- **B7, the key inside a pass missed the client generation, the table count, and a lazy renormalization of
  ids** (`library.cc:194-226`), which moves the epoch at the first `find` in the pass. *Changed:*
  `GraphPassKey` with the time, and `lib.find(BlockId())` at the start of `prepare`.
- **B8, `Scene::frame` can be written without a notification** (run, `scene.h:123`). *Changed:* claim 9 is
  dropped; part B compares the time.
- **B9, channel names can collide with fixed inputs or be refused** (`node_type.cc:56`, `414`).
  *Changed:* channel inputs are named `c_<channel>`; Enum defaults are converted.
- **B10, objects of a transform type that isn't registered had no node.** *Changed:*
  `scene.transform.missing`, which gives the parent's state, so the walk goes on as in the direct pass.
- **The provider call itself costs 0.2 to 1 ms per pass** at 100,000 objects, even a no-op one (measured,
  noisy). *Changed:* `prepare` gives the driven set, and the pass asks only those objects.

### Smaller findings, and what changed
- `prepare` failing as a whole: the driven objects fail and are ModelFailed, which is evaluated again each
  pass. Written down.
- A sticky error left in `prepare`: taken there, so the first object isn't blamed. Written down.
- A drag freezes driven values (`apply_delta.cc:126-135`): P3's behaviour, told to P4.
- A duplicated scene copies its graphs, which still drive the originals: a risk, documented.
- One subgraph used twice drives a channel twice: a diagnostic.
- A user's graph with readers can't be previewed alone: the error says why.
- `disableDrivers` forgotten: `SceneDrivers` holds a `LibraryRef` and doesn't touch a Library that is gone.
- Script node types may have `OutputAdapter`: the scan goes by type name, and `scene.` is reserved
  (`bind/graph_facades.cc:186`, now in the ownership table).
- `embedding.md`'s provider rule stays as it is, since `SceneDrivers` notifies.
- Step 8's tests with drivers need step 4; the order of work says so.

### Not verified
- Nothing of `SceneDrivers` or part B exists, so B1, B4, B6 and B10 are reasoned.
- Whether `G` of 400 objects keeps an epoch move within +3 ms.
- MSVC and wasm. Whether a script's `evaluateMany` agrees with `evaluate` bitwise (a contract only).
