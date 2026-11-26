# Feature report: the scene evaluator (`drive/` and part B, P5c), 2026-11-25

- **Reviewed:** `drive/`, the incremental direct pass, the commands and the script facades at `de20fe9`
  (P5c steps 1-9), against the [P5c plan](../plans/2026-11-25-p5c-scene-evaluator.md).
- **Reviewer:** a fresh agent given the code and the plan, not the author's reasoning. It wrote probes
  linked against the branch's RelWithDebInfo libraries, and says which findings it proved by running them
  and which it only reasoned out.
- **This file:** a rewrite of its review. Each finding ends with its resolution. The bugs come first, then
  the design findings.
- **The fixes** are in `46a09ee`. The probes that fit are regressions in `tests/drive/test_drive_review.cc`.
  The facade fixes are tested in `test_drive_facades` and `tests/wasm`.

## Summary
- **What held up:** the generated graph matches a sync from scratch (the random test's oracle); part B's
  incremental pass matches a full pass for edits that notify; the read-after-write path of commands.
- **What didn't:**
  - two paths where a change never reached what reads it (an inner provider's notification, and a failure of
    the drivers' call as a whole);
  - a name collision that broke every driver of a Library;
  - ids and names that weren't checked (aliases, the `scene.` namespace);
  - parts of the API that disagree with the plan or invite misuse.

## Bugs
1. **An inner provider's changes didn't reach graphs that read its objects** (proved).
   - The transform kernel followed every aspect of its object except `kDrivenAspect`, which is the aspect an
     inner provider (animation, say) uses to say its answers changed.
   - So a graph that read such an object kept the old value, while the object itself moved: in the probe the
     leader moved to y=9 and its follower stayed at y=5, in the pass and in `computeState`.
   - The provider test missed it because it notified everything with `notifyAll`.
   - *Fixed:* the kernel follows every aspect. SceneDrivers' own notifications don't make a loop: they come
     after the graphs are evaluated, only for objects whose values moved. An object that is both driven and
     read costs one more evaluation of its transform node, which then gives the same value.
   - *Tested:* `testInnerNotification`.
2. **A failure of the drivers' call as a whole left stale results** (proved).
   - When the generated graph's evaluation failed as a whole (over the frame budget, say), no driven object
     was notified. The incremental pass skips objects that weren't notified, so it kept them Ok at their old
     values while `computeState` failed. A full pass failed them, as it should.
   - *Fixed:* when the call's error changes (it starts failing, stops, or fails otherwise), every driven
     object is notified.
   - *Tested:* `testWholeCallFailure`, with the reviewer's doubling nest of 2^20 frames, both ways.
3. **A transform type named "missing" broke every driver in the Library** (proved).
   - Its node type got the name of the node type for unregistered types. The kernel then took it for the
     missing type, answered stale each time, and the whole call failed.
   - *Fixed:* the missing type's node type is `scene.missingTransform`, outside the prefix that type names
     are put under.
   - *Tested:* `testTypeNamedMissing`.
4. **Scripts could register node types under `scene.`,** so one registered before drive/ used a name like
   `scene.transform.<type>` would be used by the generated graph, with the script's kernel.
   - *Fixed:* `scene.` is reserved for script node types, as `core.` and `graph.` are. drive/ also keeps the
     transform node types it registered, and won't use another of the same name (AlreadyExists for that
     object; what reads it fails).
   - Registering the transform types eagerly isn't possible: a transform type can be registered at any time.
   - *Tested:* `testForeignTransformType` natively; `tests/wasm/graphs.ts` for the reserved namespace.
5. **Ids weren't made canonical** (reasoned).
   - The scan of listed graphs keyed driven and read objects by the raw id in the node's param; the provider
     looks objects up by their canonical id. With an alias's id (a file saved before the alias was
     registered) the driver was never applied, and driving the same object through both ids wasn't seen.
   - *Fixed:* the scan makes ids canonical with `ClientRegistry::canonical`.
   - *Tested:* `testAliasIds`, which also checks that the alias and the canonical id drive the object twice.
6. **`generated()` and `instance()` dereferenced null before the first sync.** *Fixed:* they return
   pointers, null until then. *Tested:* `testAccessors`.
7. **The JSON diagnostics dropped the group path:** an issue about a group (a nesting too deep) has groups but
   no node, and was written as "" (the graph). *Fixed:* such a path is written as its groups. *Tested:*
   `test_drive_facades`, with a chain of 70 nested groups.
8. **The TS wrappers quoted the graph's ref by hand,** which breaks on refs that contain `"`, and duplicated
   `invoke`. *Fixed:* `addSceneGraph` and `removeSceneGraph` (now `addDriverGraph` and `removeDriverGraph`) use `run`
   with JS values.

## Design findings
- **A. Every scene's list drives the whole document** (duplicates make objects "driven twice", and it is in
  the file format). The reviewer's probes: duplicating a scene with its graph drives the original object
  twice, and a graph listed only by an unrelated scene still drives. *Fixed* (the user's decision of
  2026-11-26), in `a9604a5`:
  - one document-wide list of driver graphs, a block of drive/'s (`coredcc.DriverList`), replaces
    `Scene::graphs`; scenes don't list graphs;
  - the commands are `core.drivers.addGraph` and `core.drivers.removeGraph`, without a `scene` input;
  - a copy of the list is empty, and a graph that two lists name (after an append) drives once;
  - the plan has the design, why a block and not a part of the file, and its pressure test.
  - *Tested:* `test_drive_list`, with the probes P3 (a duplicated scene, or list, drives nothing twice) and
    P5 (the graphs drive objects of every scene; no scene has a `graphs` member).
- **B. Driven objects are notified only when someone evaluates the graphs.** After a graph edit, nothing is
  notified until `prepare`, `update` or `overridesOf` runs; and `computeState` and a command's `canRun` can
  post notifications. *Kept, and documented as the contract* in `drive.h` and `docs/debugging.md`. Why:
  - Which objects moved is known only by evaluating the graphs. Notifying on edit means evaluating on every
    edit of a listed graph, which during a drag is many evaluations per frame that nothing reads.
  - Evaluating from a ChangeBus listener would re-enter the bus from inside a notification, which is worse
    than the re-entrance the reviewer found.
  - The direct pass already calls `prepare` once a frame, so evaluated scenes see every change. A cache of
    driven objects calls `update` first, as evaluateScene does.
  - *Tested:* `testNotifiedOnEvaluation` (no notification from the edit, one from `update`).
- **C. The commands lacked the promised `scene` input,** and undo of `removeGraph` restored one cell where
  it had removed several.
  - *Fixed in `46a09ee`:* an optional `scene` input, and undo that brings back every removed cell at its
    place.
  - *Then superseded by A:* the commands are `core.drivers.addGraph` and `core.drivers.removeGraph`, and take
    no scene. Undo still brings back every cell, in every list. This also settles the naming question.
  - *Tested:* `test_drive_list`.
- **D. Scripts and hosts' drivers.**
  - `driverDiagnostics` saw only drivers made by scripts. *Fixed:* `SceneDrivers::of(lib)` finds the
    Library's drivers, whoever made them; the facade reports those, and `enableDrivers` refuses when a host
    made some.
  - A diagnostic's `graph` was id text. *Fixed:* it is a pointer's text, a `Ref` in TS.
  - Scripts can't set an inner provider. *Deferred:* it needs a script implementation of OverrideProvider.
    Tracked in the plan's "Not done yet".
- **E. `diagnostics()` didn't sync, and returned a reference that later calls cleared.** *Fixed:* it syncs
  (outside an evaluation) and returns a copy.
- **F. `kDrivenAspect` was in `base/invalidate.h`.** *Fixed:* it is in `api/change_bus.h`, beside
  `ChangeAspects`.
- **G. `SceneDrivers::State` was public.** *Fixed:* it is private.
- **H. Transform node types are registered in the Runtime's registry when first needed,** so users can place
  them, and the first registration forced a second sync (the node type count is in the sync key).
  - *Fixed:* the sync key leaves out the types drive/ registers itself. A transform node placed in a listed
    graph is reported as a diagnostic and does nothing.
  - They stay in the Runtime's registry: the generated graph is an ordinary GraphDef, whose nodes need
    registered types. *Tested:* `testPlacedTransformNode`.
