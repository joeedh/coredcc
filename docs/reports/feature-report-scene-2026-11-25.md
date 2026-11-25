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
3. **Commands will ignore P5c's channel overrides.** *Round 3.*
4. **Membership edits are quadratic.** *Round 2.*
5. **An undo that meets a Detached object failed,** dropped the history and left the object changed.
   *Fixed:* undo, redo, drags and cancelling skip objects that aren't Live, and what would have been written
   waits until they are (`PendingWrites`; the Library calls `addReviveListener`'s listeners from
   `onHostRestored` and `restoreFromHistory`). Several writes while it is away leave the last one.
   - If it can't be written just then, the next `evaluateScene` or undo does it. If the object refuses it,
     that is the sticky error.
   - *Tested:* drags, commits and cancels with the object away and back at each point; parents, types and
     channels; an object that is deleted while it is away.
6. **Scripts can't read results in bulk, write a world matrix, or walk the hierarchy.** *Round 3.*

## Medium
7. **Per-object ancestor walks make deep chains quadratic.** *Round 2.*
8. **One bad object blocked a command on all.** *Fixed* for `applyDelta`, `dragDelta`, `setParent`,
   `setTransformType` and `setRotationMode`: they leave out what they can't work on, count it in the output
   `skipped`, and fail when nothing is left. An object below one that is left out moves by itself. In a
   drag, an object that can't follow an update keeps its last place.
   - Exceptions of scripts are errors still, and so is a write that fails: all or nothing.
   - `core.scene.select` leaves out what isn't selectable, and counts it.
9. **The delta model has one pivot and one basis for all objects.** *Round 3.*
10. **There are no per-channel locks.** *Round 3.*
11. **Payload formats were positional.** *Fixed:* objects and scenes are named entries. What this build doesn't
    know is kept with its references, and written again. The scene's units are in the file now.
    - A setting whose value a build can't read stays the last of its name until it is set to something.
    - `BlockReader::entries` has a form that reads one entry at a time, which counts only those that are
      kept toward the load's limits: an object takes 3.1 KB of them, and took 9.1 otherwise.
12. **Transform types had no version or upgrade hook.** *Fixed:* `TransformTypeInfo::version`, written beside the
    type's name, and the optional slot `TransformModel::upgrade(version, ChannelEntries)`. It runs when an
    older object is first asked for its channels. A newer file's channels, and those that can't be
    upgraded, are kept unread; the object's status is Invalid, and the error is reported once.
    - Scripts get both in round 3, with finding 20.
13. **The member index missed raw cell writes.** *Fixed:* an id that isn't found is looked for again when the
    Library's epoch moved. `objects()` lists a block once, `prune` drops doubles, and `remove` takes all
    of them.
    - `add` doesn't look again, so that adding many objects isn't quadratic. A double that gets in that
      way does no harm.
14. **Host objects are second class by path.** *Round 3.*
15. **The Context says any host scene works.** *Round 3.*
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
19. **There is no object-level duplicate.** *Round 3.*
20. **A foreign transform type can declare very little.** *Round 3.*
21. **Change reporting is all or nothing.** *Round 2.*

## Low
22. **`registerSceneTypes` said Ok after a partial failure.** *Fixed:* it is done only when the last thing it
    registers is there, so it fails each time.
23. **FBX and Matrix miss the 20 ms target.** *Round 2.*
24. **Selected objects that are hidden or made unselectable later** were still transformed and deleted.
    *Decided:* they stay selected, so that showing them again brings the selection back, but commands that
    work on the selection leave them out. `objects` may name them. Documented in `commands.h`.
25. to 30.: *Round 3.*

## Found on the way
- **A transform type that is registered after its objects** changes what they answer. Registering one posts
  that anything may have changed, so that graphs ask again (the rule for `queryBlock` is in
  `embedding.md`).
