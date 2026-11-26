# Feature report: tools and overlay (`tool/`, `draw/`), 2026-11-26

- **Reviewed:** P4 at `bcc330c`. That covers `tool/` (the ToolManager, the built-in tools and gizmos,
  element transforms and strokes), `draw/` (DrawList, its reader and View), the tool facades, and
  `bind/ts/tool.ts` and `drawlist.ts`. They were checked against architecture §11 and the P4 plan.
- **Reviewer:** a fresh agent given the code and the spec, not the author's reasoning. Its experiments
  (E1 to E14) were a scratch program linked against the release libraries.
- **This file:** a rewrite of its review. Each finding ends with its resolution. The bugs come first, then
  the design findings (A to M).
- **Fixes:** all in `ba60694`, which also holds their tests. What is deferred is tracked in the P4 plan,
  under "After the review".

## Summary
- **What held up:**
  - the dispatch order and the orphan check;
  - undo during a drag;
  - the stroke goldens;
  - the DrawList's precision under a far origin;
  - the record format's sizes and counts;
  - the Unreal spike.
- **What didn't:**
  - the manager's own bookkeeping: clearing gizmos, gizmo ids, a settings callback from a draw, and a
    status that the end of a gesture erased;
  - a command that a tool cancelled although it hadn't begun it;
  - deltas and strokes that nothing should accept, which the settings allowed: points moved to NaN, and a
    stroke of 30 million samples;
  - a pivot taken from the last frame rather than from the objects;
  - the C++ reader and the TS decoder, which checked different things.

## Bugs
1. **`clearGizmos` outside a dispatch or draw skipped every other gizmo,** and read one that it had just
   freed: three gizmos left one (E14). It called `removeGizmo` while walking the vector, and
   `removeGizmo` erases at once outside a call. *Fixed:* it marks them all, then erases once.
   - *Tested:* `test_tool_manager` (`test_review_fixes`).
2. **A press with the stroke tool cancelled a drag it didn't begin.** Suppose the host had
   `core.object.dragDelta` open, so `core.stroke` couldn't begin. The tool's error path then cancelled
   whatever was open (E1). *Fixed:* it cancels a command only if its serial moved during the tool's own
   press.
   - *Tested:* `test_tool_stroke` (`testHostDragKept`).
3. **`ElementsDelta::update` wrote points that aren't finite, and committed them** (E9, E9b). ApplyDelta
   refused the same deltas: a NaN translation, a turn about a zero axis, and a scale in a zero basis.
   *Fixed:*
   - `TransformDelta::valid` is now the one check for both commands. It refuses:
     - a kind that doesn't exist;
     - numbers that aren't finite;
     - a basis of length 0;
     - a rotation axis of length 0.
   - ElementsDelta also checks every result before it writes any block.
   - For objects, a zero basis is now refused too. Their other cases are as before.
   - *Tested:* `test_tool_elements` (`testReviewFixes`).
4. **`Tool.settingsChanged` ran from `draw`,** although it is an event slot. A command that it began
   there wasn't checked. In `dispatch`, it ran before the serial for the orphan check was taken.
   *Fixed:* only `dispatch` calls it. It takes the serial first and runs the orphan check after. A change
   made between events reaches the tool at the next event.
   - *Tested:* `test_tool_manager`.
5. **A stroke's samples had no bound** (E13). The step's floor was 1e-5 pixels, and the settings allowed a
   radius of 1e-9 and a spacing of 1e-4. At those values, 400 pixels made 30,000,001 samples. *Fixed:*
   - Samples are at least half a device pixel apart (`kMinStepPixels`). In World space, that is half a
     pixel's size at the segment's middle.
   - The smallest settings are a spacing of 0.01 and a radius of 1e-6 (`StrokeSettings::kMinSpacing` and
     `kMinRadius`). `configure` refuses smaller ones.
   - One poll gives at most 4,096 samples (`kMaxSamplesPerPoll`). The ones past that are counted without
     being walked (`StrokeSampler::dropped`), and the stroke tool says so in the status.
   - *Tested:* `test_tool_stroke` (`testFlood`):
     - the 300 pixels that are fixed give about 600 samples;
     - a jump of 3,000 pixels is capped.
6. **The gizmo scaled and turned about the last evaluation's pivot** (E4). Take a box at (1, 1, 0) that a
   command then moves by 3 in x. Scaled by 2 before the next evaluation, it went to (7, 1, 0) instead of
   (4, 1, 0). *Fixed:* `beginDrag` takes the pivot from the command it begins (`ApplyDelta::startState`,
   or `ElementsDelta::centre`). The drawn frame still gives the screen geometry, since the press was on
   what was drawn.
   - *Tested:* `test_tool_drag` (`testPivotAfterCommand`).
7. **`drawTools` in TS threw the whole frame away when one slot failed.** It checked the draw's status
   before it read the bytes, and the test app draws through it. *Fixed:* it reads the bytes first and
   returns `error` beside them. The test app renders what was drawn, then throws.
   - *Tested:* `tests/wasm/tool.test.ts`.
8. **The select tool's error text was erased.** The tool set the status and finished, and finishing
   emptied the status. *Fixed:* `ToolManager::setOutcome` (`setToolOutcome` in TS) sets the text that the
   status becomes when the gesture ends. The select tool uses it.
   - A first attempt kept whatever text the finishing event set. It failed, because the transform modal
     sets its text on every event, the release too.
   - *Tested:* `test_tool_manager` and `tests/wasm/tool.test.ts`.
9. **Gizmo record ids wrapped after 65,535 gizmos.** A record's id is the gizmo's id times 65,536, and
   ids only grew. *Fixed:*
   - A new gizmo gets the smallest free id.
   - `addGizmo` refuses (returns -1) when there are `kMaxGizmos` (65,535) already.
   - An id that is removed during a call becomes free when the call ends.
   - *Tested:* `test_tool_manager`.
10. **A click on a mirror plane made an undo step.** Every update set the plane coordinate of an
    on-plane point to exactly 0. So a point that was within the plane's tolerance, but not exactly on it,
    changed on a click with no motion. *Fixed:* a delta that moves nothing (`TransformDelta::identity`)
    writes the starting positions, unsnapped, partners included. A point of weight 0 isn't snapped.
    - *Tested:* `test_tool_elements`.
11. **`dragUpdateEach` cast any number to a DeltaKind.** For NaN and out-of-range values that was
    undefined behaviour, and other values became Scale. *Fixed:* the kind must be 0, 1 or 2, or it is
    InvalidArgument.
    - *Tested:* `test_bind_tool`.
12. **An object's `drawOverlay` could pop the push that its caller made for it.** The records after it
    then drew in world space, and the caller ignored the failure of its own pop. *Fixed:*
    - A DrawList has a floor, and a pop below it is refused.
    - `DrawSlotScope` sets the floor around a slot. It pops what the slot left pushed, and reports it.
    - The scene's overlays and every slot of `ToolManager::draw` run inside one. The caller's own pop is
      checked.
    - *Tested:* `test_drawlist` (`testFloor`) and `test_tool_manager`.
13. **The C++ reader and the TS decoder disagreed.**
    - The reader didn't check the header's flags or its reserved word, the padding, or that matrices are
      affine.
    - The decoder accepted:
      - odd vertex counts;
      - unknown flags;
      - numbers that aren't finite;
      - negative widths;
      - records larger than their payload.
    - *Fixed:* both check one list of rules, in `drawlist-format.md` ("Errors"), which now names every
      rule. Both also refuse a negative radius, and the writer zeroes the padding itself.
    - *Tested:* `test_drawlist` (`testRules`) and `tests/wasm/tool.test.ts` make the same changes to a
      list, and both refuse every one.

## Design
- **A. Script facades acted on `ctx.tools()`, not on the manager that called the slot.** So with two
  viewports sharing a Context, one viewport's modal could be pushed on the other. *Fixed:* during each of
  its calls, the manager is the Context's ToolManager. The previous one is put back after.
  - TS slots still aren't handed the manager. With this fix, `ctx` reaches the right one, and the TS API
    stays one argument shorter.
- **B. A modal's claim on a command was a serial, with no executor.** *Fixed:* the modal records its
  executor. It is valid only while that executor's open command has that serial.
- **C. Slots' arguments are borrowed.** *Fixed:* this is now documented on ToolManager and in `tool.ts`.
  A TS Drawer is closed when its slot returns, and throws if it is used after that.
- **D. A modal couldn't hand over to a new gesture.** Pruning a modal whose command had ended also
  cancelled the modals above it. That included a new modal that owned a new command. *Fixed:* pruning
  removes only the stale modals.
  - *Tested:* `test_tool_manager`. A modal commits, begins the next drag and pushes a modal for it, and
    the new modal stays.
- **E. The DrawList's origin was always the eye.** *Fixed:*
  - `ToolManager::drawOrigin` lets the host choose it. Unset, the default, it is the camera's position.
  - `drawlist-format.md` used to say that nothing far from the origin is ever a float. That was wrong:
    points are stored as floats relative to the origin, so precision falls with the distance from it. The
    format now says so.
- **F. Events.**
  - Modals took every pointer's events. *Fixed:* a modal pushed during a pointer event gets only that
    pointer's events. Other pointers' events are Unhandled, for the host.
  - `setType` and `setPointer` took any integer. *Fixed:* `InputEvent::valid`, and `dispatch` refuses an
    event that isn't valid.
  - There is no text input event. *Deferred* (P4 plan, "After the review").
- **G. `dispatch` lost whether the event was used when a gizmo's update or hit test failed.** *Fixed:*
  - An optional `late` argument receives that error, and the result is still returned.
  - The facade returns both.
  - TS's `sendEvent` throws an error whose `used` is set.
- **H. `ToolRegistry::validateDef` registered a struct, twice for each tool with a layout.** *Fixed:* the
  struct is named by the settings' names and kinds, so a second check finds the one the first made.
  - Validating still registers one struct for each set of settings, because checking a layout needs a
    registered struct. This is documented.
  - *Tested:* `test_tool_manager`: a tool with a layout adds one struct.
- **I. `toolRemoveGizmo` has the Read gate.** *Rejected.*
  - Removing a gizmo changes the manager, not the document.
  - A removal during a call waits for the call's end, so even a draw slot can do it safely.
    Deactivating does exactly that.
  - A gate kind for "changes the manager only" would be one more distinction to maintain, for one call.
- **J. The built-in transform gizmo couldn't be parameterised.** *Fixed:* `toolAddGizmo` takes
  `settingsOf`, a tool whose settings the gizmo follows, under core.tool.transform's setting names. In
  TS, this is `addGizmo(ctx, {kind: 'transform', settingsOf})`.
- **K. TS.**
  - The comment on `defineTool` said each activation got new slot state, but every activation calls the
    same spec. *Fixed:* the comment says so.
  - `buttons` defaulted to 1 on a pointermove. *Fixed:* it defaults to 1 on pointerdown only.
  - There is no way to unregister or replace a tool, which hot reload needs. *Deferred* (P4 plan, "After
    the review").
- **L. Naming.**
  - `TestBox.size` bound `Box::half`. *Fixed:* it is named `half`.
  - These are *deferred* to one naming pass before P6 binds the API for N-API, because each one renames
    bound names (P4 plan, "After the review"):
    - "depth" means three things: the depth mode, the matrix stack's depth, and a hit's distance.
    - Result values are int32 constants, while DepthMode and ShapeKind are enum classes.
    - The TS names reverse the facades' names.
    - The DrawList's script methods mix name forms.
- **M. Hard to change later.**
  - The record id encoding (`kIdStride`). *Decided:* it stays, now that gizmo ids are reused and capped.
    It is documented in the format ("Record ids").
  - There is no signal that a redraw is needed, and no cursor signal. *Deferred* (P4 plan, "After the
    review").

## Not checked
- The Unreal spike wasn't rebuilt. `dispatch`'s new argument has a default, so the spike's call is
  unchanged.
