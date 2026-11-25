# Feature report: objects, transforms and the scene (`scene/`), 2026-11-25

- **Reviewed:** `scene/` (without picking), its commands, paths, files and script facades at `3811878`,
  against architecture §8 and the P3 plan (steps 6 to 11).
- **Reviewer:** a fresh agent given the code and the spec, not the author's reasoning. It ran its
  experiments in a scratch clone. Its timings are from a busy machine.
- **This file:** a rewrite of its review. Each finding ends with its resolution. Round 1 is wrong
  results, lost data and formats; round 2 is cost; round 3 is the API.

## Summary
- **What held up:**
  - rotation deltas against independent matrices (linear error up to 2.4e-15), long drags in every
    Euler order, `setWorld` at 1e12 and under uniform scales from 1e-11 to 1e6;
  - undo by stored values, all or nothing for a failing command, cancelling after a failure;
  - the evaluator's two stores and revalidation, channel files by name, deleting with
    keepChildrenWorld.
- **What didn't:**
  - a parent that isn't in a scene, which a save lost;
  - scale deltas along other axes than the object's own;
  - commands and undo when one of the objects can't be changed, or isn't there;
  - positional payloads;
  - membership edits and deep chains, which are quadratic;
  - what scripts and hosts can do with objects of their own.

## High
1. **A parent that isn't a scene member was lost on save and on collect,** and its children moved, with no
   report. *Fixed:* a parent is an OptStrong reference, so children keep it alive and a save follows it.
   Deleting it doesn't delete them.
   - *Checked:* appending and duplicating (the copy of a child keeps the parent), cycles that nothing else
     keeps, `usersOf`, `deletePreview`, undo. A deleted parent is written null.
   - Setting a parent outside the scene by path stays possible; the command refuses it.
2. **Scaling along other axes than the object's shrank it,** and mirrors were wrong although an exact result
   exists. *Fixed:*
   - A delta with negative factors is a mirror and then a scale by their sizes. The mirror changes the
     rotation, to the nearest one, and is exact under parents without shear. Along the object's own
     axes, only the scale's signs change, as before.
   - When other channels hold exactly what was asked for, they are taken (`solveWorld`, Exact).
   - A scale can't become 0 unless a factor is 0.
   - *Parents:* one whose largest scale is about 1e12 times its smallest counts as having no inverse.
     Uniform scales of 1e-11 and one axis of 1e-11 still work.
   - *Tested:* `test_scene_scale`, against 3 by 3 matrices made in the test: TRS and all FBX types, any
     basis and pivot, mirrored and scaled parents; own axes; drags through 0; results that are exact only
     with another rotation, found with singular vectors; a mirror by command, with a child.
   - *Not exact, as before:* factors that would shear. They are applied along the object's own axes.
3. **Commands will ignore P5c's channel overrides.** *Fixed:* `OverrideProvider`, one per Library, which
   evaluation, `computeState` and all commands ask through one function. Solvers leave driven channels as they
   are. The contract is in the P3 plan ("What P4 and P5c rely on").
   - *Tested:* `test_scene_overrides`, against documents that store the driven values, with drags at a time
     that isn't 0 under parents and models that depend on it (the mutant that survived).
4. **Membership edits were quadratic.** *Fixed:* `Scene::removeMany`, `putBack` and `addMany` take one pass,
   and unlink uses them; the membership commands prune only after records have gone
   (`Library::removedCount`); `deletePreview` looks at each user once; the history destroys what leaves it
   in one call (`detail::HistoryDestroys`).
   - 100,000 objects: unlinking 1,000 from 1,470 ms to 4 ms, 10,000 from 15.4 s to 13 ms; an added object from
     4.3 ms to 0.02 ms; deleting 10,000 from 3.4 s to 75 ms; the command after 200 undone adds from 3.0 s to
     16 ms. Undo and redo are alike.
   - *Tested:* against one at a time and against the old preview, at random; `test_scene_growth`.
5. **An undo that meets a Detached object failed,** dropped the history and left the object changed.
   *Fixed:* undo, redo, drags and cancelling skip objects that aren't Live, and what would have been written
   waits until they are (`PendingWrites`; the Library calls `addReviveListener`'s listeners from
   `onHostRestored` and `restoreFromHistory`). Several writes while it is away leave the last one.
   - If it can't be written just then, the next `evaluateScene` or undo does it. If the object refuses it,
     that is the sticky error.
   - *Tested:* drags, commits and cancels with the object away and back at each point; parents, types and
     channels; an object that is deleted while it is away.
6. **Scripts can't read results in bulk, write a world matrix, or walk the hierarchy.** *Fixed:* `sceneResults` (flat
   arrays, for some places or all, with `lastChanges`), `sceneObjects`, `children`, and `setWorld`, which is
   `core.object.setWorld`. C++ has `Scene::results`, `children`, and `ObjectEval`'s flags, parent and data.
   Paths and loops over the selection and the results no longer make a list at each step.

## Medium
7. **Per-object ancestor walks made deep chains quadratic.** *Fixed for commands:* `StateCache` and `Ancestry`
   answer for many objects, and compute each one once. Applies to applyDelta, setParent, setTransformType, delete
   and what keepWorld counts.
   - A chain of 4,000: applyDelta from 3.4 s to 8 ms, setParent from 1.6 s to 13 ms, setTransformType from 4.0 s
     to 11 ms, deleting every second one from 2.6 s to 16 ms.
   - `computeState` of one object still walks up its ancestors, as documented. Hosts that want many use
     `evaluateScene`.
8. **One bad object blocked a command on all.** *Fixed* for `applyDelta`, `dragDelta`, `setParent`,
   `setTransformType` and `setRotationMode`: they leave out what they can't work on, count it in the output
   `skipped`, and fail when nothing is left. An object below one that is left out moves by itself. In a
   drag, an object that can't follow an update keeps its last place.
   - Exceptions of scripts are errors still, and so is a write that fails: all or nothing.
   - `core.scene.select` leaves out what isn't selectable, and counts it.
9. **The delta model has one pivot and one basis for all objects.** *Fixed:* `ApplyDelta::update` takes a delta for
   each object, `startState` and `startPivot` say what they were, and `TransformModel` has an optional `pivot`.
10. **There are no per-channel locks.** *Fixed:* `ObjectInfo::locks`, `core.object.setLocks`, and in files by name
    (no new version). Solvers leave locked channels alone; keepWorld ignores locks.
11. **Payload formats were positional.** *Fixed:* objects and scenes are named entries. What this build doesn't
    know is kept with its references, and written again. The scene's units are in the file now.
    - A setting whose value a build can't read stays the last of its name until it is set to something.
    - `BlockReader::entries` has a form that reads one entry at a time, which counts only those that are
      kept toward the load's limits: an object takes 3.1 KB of them, and took 9.1 otherwise.
12. **Transform types had no version or upgrade hook.** *Fixed:* `TransformTypeInfo::version`, written beside the
    type's name, and the optional slot `TransformModel::upgrade(version, ChannelEntries)`. It runs when an
    older object is first asked for its channels. A newer file's channels, and those that can't be
    upgraded, are kept unread; the object's status is Invalid, and the error is reported once.
    - Scripts have both (finding 20).
13. **The member index missed raw cell writes.** *Fixed:* an id that isn't found is looked for again when the
    Library's epoch moved. `objects()` lists a block once, `prune` drops doubles, and `remove` takes all
    of them.
    - `add` doesn't look again, so that adding many objects isn't quadratic. A double that gets in that
      way does no harm.
14. **Host objects are second class by path.** *Fixed:* `coredcc.SceneObject` has all members of any Transformable
    block, and is what the scene's lists and pointers have.
15. **The Context says any host scene works.** *Documented* in `context.h`, and in `embedding.md` how to mirror a
    host's scene. *Deferred:* an interface for membership.
16. **`setTransformType` rewrote objects that have the type already.** *Fixed:* they stay to the bit. One whose
    channels are another type's is still converted.
17. **keepWorld didn't keep descendants that inherit FBX-style.** *Fixed by counting:* `approximate` counts every
    object below that isn't where it was, for `setParent`, `setTransformType` and `delete`, also those
    below an object that is kept only nearly. It looks only when the object's rotation or scale
    changed. *Documented:* which types these are. Tested against states before and after.
    - Solving them isn't attempted: a child's own answer may depend on its siblings' parent's type, and it
      would change objects the user didn't name.
18. **There is no geometric transform, instancing or object offset.** *Reserved:* `dataMatrix` and `parentKey` in
    the format, and `ObjectEval::dataWorld`, which bounds and picking use and which is the object's world
    for now. No feature yet.
19. **There is no object-level duplicate.** *Fixed:* `core.object.duplicate` and `core.scene.duplicate`.
20. **A foreign transform type can declare very little.** *Fixed:* channel specs with the names of `meta`, enums and
    quaternions, `version`, `upgrade`, `pivot` and `evaluateMany`. Bounds are kept by Invalidation.
21. **Change reporting was all or nothing.** *Fixed for C++:* `Scene::lastChanges` has a version and the places
    that differ from the evaluation before. Scripts have it in `sceneResults`.
    - *Deferred:* watchers of `evaluated` still hear of any change. An owner per object would be posted 100,000
      times in a drag of all.

## Low
22. **`registerSceneTypes` said Ok after a partial failure.** *Fixed:* it is done only when the last thing it
    registers is there, so it fails each time.
23. **FBX and Matrix missed the 20 ms target.** Measured on an idle machine, 100,000 objects without data blocks,
    unchanged, best ms (median): TRS 17 (18), FBX 25 (26), Matrix 28 (29). *Now:* TRS 17 (17), FBX 22 (23),
    Matrix 17 (18).
    - Matrix decomposes columns at right angles without `hypot`. A state that is decomposed only when a child asks
      for it wasn't needed for that.
    - FBX leaves out zero rotations. It is still above 20 ms; the rest is its pivots, matrix products and the
      rotation itself.
    *Correction:* it isn't known which build these numbers came from, as round 3's came from a program that had not
    been built. The code of round 3 took 24 ms for FBX, above the target. The plan's round 3 section has the cause and
    provisional new numbers from a busy machine (FBX 13.4).
24. **Selected objects that are hidden or made unselectable later** were still transformed and deleted.
    *Decided:* they stay selected, so that showing them again brings the selection back, but commands that
    work on the selection leave them out. `objects` may name them. Documented in `commands.h`.
25. **Error text repeated the name, and said "isn't Live" for what isn't Transformable.** *Fixed.*
26. **`SetWorldMode::Closest` isn't closest.** *Documented:* what it fits, and in which order.
27. **Nudges were a step each, and a click could record an empty one.** *Fixed:* the input `gesture` of
    `core.object.applyDelta`; `ApplyDelta::changed` tells a tool to cancel.
28. **`core.object.add` took no data, rotation or scale,** and didn't select; `core.scene.link` took one object.
    *Fixed.*
29. **`Counters::completed` was an API.** *Fixed:* `EvaluateResult`.
30. **Docs.** *Fixed:* architecture §8 and §9, and the plan on `parent`. `CLAUDE.md` is left to the user, who is
    changing it.

## Found on the way
- **A transform type that is registered after its objects** changes what they answer. Registering one posts
  that anything may have changed, so that graphs ask again (the rule for `queryBlock` is in
  `embedding.md`).
