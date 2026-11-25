# Feature report: UI layouts and TA2 (`layout/`, `bind/`, the test app's panel), 2026-11-25

- **Reviewed:** P2b's steps B1 to B5 at `a374d5c`, against architecture §13 and the
  [P2 plan](../plans/2026-11-24-p2-layout-library-io.md).
- **Reviewer:** a fresh agent given the code and the spec, not the author's reasoning. It ran
  experiments in TS, C++ and the test app, linked against builds of that commit.
- **This file:** a rewrite of its review. Each finding ends with its resolution. Round 1 took the
  findings that give wrong results, round 2 the API and the formats.

## Summary
- **What held up:**
  - an immutable Layout with one checker for the parser and the builder;
  - joined paths in the JSON, and the static resolver over sets of structs;
  - watchers of repointed paths, and the gates and lifetimes of the readers.
- **What didn't:**
  - A run's path prefix was kept as text, so undo and redo followed it to whatever it named by
    then.
  - Dynamic defs weren't followed anywhere that a frontend depends on: in invocations, last-used
    inputs, enum keys in paths, metadata and watchers.
  - The panel had bugs that its own tests stepped over.
- **What's missing:** ways to ask and to do from a script what a frontend needs (can it run, run it
  with values, take a drag back), and a format that can grow.

## High
1. **Undo, redo, rerun and fold used the prefix as written, so they could edit the wrong object.**
   A tool ran under `activeItem`, the active item was changed outside the history, and undo wrote
   the old item's value into the new one. A rename made undo fail, and two runs with another active
   object in between folded into one step.
   - *Fixed:* a run's prefix is resolved to its canonical `StableRef` when it starts, and the
     record keeps that. Undo, redo, rerun and the fold test use it. A prefix that names nothing is
     an error before the command runs.
   - *Tested:* the active item changed by `write`, a rename, a fold, and redo by exec.
2. **Dynamic command inputs didn't work in invocations or layouts, and the hook was given a
   pointer of the wrong type,** which a hook that cast it as its doc says would have misread.
   - *Fixed:* literals are parsed against the def that the bag resolves, with the inputs that
     aren't dynamic set first. LastUsed's entries are `PropertyBag`s, which the hooks are given.
     The validator checks only the form of a dynamic input's literal.
   - *Tested:* a hook that checks and reads its bag, through `commandDefaults`.
3. **A NUL in a script's string aborted the module.**
   - *Fixed:* `BIND_FACADE` wraps every facade that takes a string, which refuses it with the
     argument's number. `newCommandDef` and `newNodeType`, which have no status, replace it, and
     registering the def fails.
   - *Tested:* a test lists the bound functions, so that one without the check fails it.
4. **Metadata that changes at run time reached no frontend.**
   - *Fixed:* `meta` and the catalog say `dynamic`. `ChangeBus::watch` takes a second function,
     called when the path's struct, member or effective def changes, and PathReader has
     `takeChangedMeta`. A dynamic member is read at each flush that has changes, since its hook may
     depend on anything.
   - The panel makes such a widget again, but not while it is in use. The test app has a dynamic
     range and a dynamic enum.
5. **Dynamic enum keys were only half supported.**
   - *Fixed:* `camera[Cam1]` resolves through the effective def. A key that the static def doesn't
     have is a Warning in the validator. gen-paths gives `(string & {}) | 'NONE'`.

## Medium
6. **A prefix breaks commands that aren't written for it, and a tool can't opt out.**
   - *Round 1:* a path may start with `/`, also in an invocation's arguments (with 24).
   - *Round 2:* `path` on `tool` and `toolOptions`.
7. **A frontend can't ask whether a tool can run.** *Round 2.*
8. **TS can only run commands through hand-written invocation strings.** *Round 2.*
9. **The gen-paths output isn't usable with the API.** *Round 2.*
10. **PathReader isn't the batched read of architecture §13.** *Round 2.*
11. **`ChangeBus::flush` is watchers × changes × touches.** *Round 2.*
12. **`validateLayout` missed things that can't work.**
    - *Fixed:* a tool of an Interactive command and any use of a Private one are Warnings: a host
      may handle the first with `beginInteractive`, and means the second. `toolOptions` is an Error
      if the root has no `commandDefaults`, and the JSON gives the `optionsPath`.
13. **Conditions can't express "there is one".** *Round 2.*
14. **The format has no versioning or extension point.** *Round 2.*
15. **Add-ons can't extend or reuse a layout.** *Round 2.*
16. **Gesture ids were the caller's problem, and collided.**
    - *Fixed:* `beginGesture` gives an id that the Runtime has not given before, and scripts may
      only use those. `cancelGesture` undoes the gesture's sets at the history's head and drops
      them, so they can't be redone.
    - *Not changed:* C++'s `setValueUndo` still takes any non-zero id, since a host in C++ may have
      ids of its own. A member with NoUndo isn't taken back.
17. **An interactive command's live phase wasn't under its prefix.** *Fixed:*
    `CommandExecutor::updateInteractive` runs a live update under the open run's prefix and as one
    of its slots.
18. **Panel bugs.**
    - *Fixed:* text that isn't a number is an error at its widget, and the old value comes back; a
      hidden tab has no button, and another tab is chosen; made ids avoid the author's; a field
      that is typed back shows the document's value.
    - *Added:* icons, tooltips, `expand`, `iconsOnly`, radio buttons, a tool's uiName, dragging a
      number's name, and Escape. Tests of those and of `closed`.
    - A command's default uiName is now that of its id's part after the last dot.

## Low
19. **`items.active` as a prop is an error, but works at run time.** *Round 2.*
20. **Texts and units in `meta`.** *Round 2.*
21. **The sizes of hostile layouts.** *Round 2.*
22. **`Layout` parses paths on every query.** *Round 2.*
23. **PathReader is script-only.** *Round 2.*
24. **A Context with its own prefix.** Layout paths were said to be from the root, but were resolved
    under the Context's prefix, and a flush inside a PrefixScope read every watcher under it.
    - *Fixed:* a `Path` can be from the root (`/a.b`), which a prefix doesn't apply to, and those
      that a Layout gives are. A watcher keeps the prefix it was made with.
25. **Watchers.** *Round 2.*
26. **Names.** *Round 2.*
27. **Docs.** *Round 2.*
