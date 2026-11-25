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
   - *Fixed:* a path may start with `/`, also in an invocation's arguments (with 24). A `tool` has a
     `path` of its own: its prefix, and `path="/"` for none. `toolOptions` has one too, for hosts
     that run its command.
7. **A frontend can't ask whether a tool can run.**
   - *Fixed:* `CommandExecutor::canRun`, and `canInvoke` and `canInvokeAt` of an invocation. They
     make an instance, ask its `canRun`, and discard it. For scripts also `ToolChecks`, which asks
     of many tools in one call and returns those whose answer changed. The panel disables a tool
     that can't run, with the reason as its tooltip.
   - Their cost is that of the commands' `canRun`, as what it looks at isn't known.
8. **TS can only run commands through hand-written invocation strings.**
   - *Fixed:* `run` and `canRun` with JS values, under a prefix, with the last-used inputs or
     without; `beginInteractive`, `writeInteractive`, `commitInteractive`, `cancelInteractive` and
     `interactive`; `lastCommand` and `rerunLast`, for a redo panel.
9. **The gen-paths output isn't usable with the API.**
   - *Fixed:* it gives descriptions (`Prop`), and `ValueOf`, `NamedOf` and `WritableOf` the types of
     values: a pointer's is a `Ref` to the struct, an enum's has the numbers, an Optional one's has
     `undefined`. `typed<Root, Commands>(ctx)` checks paths (with keys, positions, handles, enum
     keys and components), values, command ids, inputs and outputs.
   - The catalog has the structs of all `commandDefaults` items, whatever was used before.
   - *Limits:* a key with a `"]` in it ends there for the types, a member that only some of a
     struct's heirs have is accepted, and prefixes aren't typed.
10. **PathReader isn't the batched read of architecture §13.**
    - *Fixed:* `takeChanges` and `takeAll` get everything in one call, as JSON. Enums are resolved
      again for their keys, and nothing else is. 1,000 values with names take about 6 ms in a wasm
      debug build, where they took 30.
11. **`ChangeBus::flush` is watchers × changes × touches.**
    - *Fixed:* pending changes are a set, in which a watcher's touches are looked up. Past 4,096
      distinct ones they are dropped, and the epoch moves, so the next flush reads every watcher.
      Listeners are called for each notification, as before.
12. **`validateLayout` missed things that can't work.**
    - *Fixed:* a tool of an Interactive command and any use of a Private one are Warnings: a host
      may handle the first with `beginInteractive`, and means the second. `toolOptions` is an Error
      if the root has no `commandDefaults`, and the JSON gives the `optionsPath`.
13. **Conditions can't express "there is one".**
    - *Fixed:* a condition may be of a pointer, a collection's length or a String. A condition
      without a value is false, also if it is negated. A container whose own path names nothing is
      hidden. `conditionHolds` and `containerIsThere` say so in C++ and TS, for every frontend.
14. **The format has no versioning or extension point.**
    - *Fixed:* `version` on `<layout>` (1 without one). In a layout of a newer version, elements and
      attributes that aren't known are skipped, and the validator warns of each; in one of this
      version they are errors. `data-` attributes are the host's, on any node. The JSON has its own
      `version`.
15. **Add-ons can't extend or reuse a layout.**
    - *Fixed:* `LayoutBuilder` starts from a Layout, and adds at any node of it. The names `list`,
      `menu`, `include` and `slot` are reserved.
    - *Deferred:* a registry of layouts, includes and slots, list templates, menus, and localization
      ([plan](../plans/2026-11-24-p2-layout-library-io.md), "Deferred by the review of layouts").
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
19. **`items.active` as a prop is an error, but works at run time.**
    - *Fixed:* the error says what to use: the pointer that holds the active item.
    - *Not changed:* it stays an error. Reading it gives a Ref while there is an active item, and an
      error when there is none, so a widget of it couldn't show "none" or be set.
20. **Texts and units in `meta`.**
    - *Fixed:* a component has a name ("X", or "R" of a color) and its own default, and a vector's
      meta has the names. An enum key's default says whether the enum's default has it.
      `NumericMeta` has a display unit, and scripts have `formatNumber` and `parseNumber`.
    - A quaternion's components are X, Y, Z, W, which is how they are stored.
21. **The sizes of hostile layouts.** *Fixed:* the joined paths of a layout may be 2 MiB in all.
22. **`Layout` parses paths on every query.** *Fixed:* it keeps the joined paths, and `findId` looks
    in a sorted list.
23. **PathReader is script-only.** *Fixed:* C++ has `invokeAt`, `canInvokeAt` and `getValues` of
    parsed paths. Watching stays one ChangeBus watcher per path, which is what PathReader does.
24. **A Context with its own prefix.** Layout paths were said to be from the root, but were resolved
    under the Context's prefix, and a flush inside a PrefixScope read every watcher under it.
    - *Fixed:* a `Path` can be from the root (`/a.b`), which a prefix doesn't apply to, and those
      that a Layout gives are. A watcher keeps the prefix it was made with.
25. **Watchers.** *Fixed:* a watcher fires when its error becomes another one. `readerWatch` reads
    each path once.
26. **Names.**
    - *Fixed:* architecture says `separator`. `toNamed` documents why a value that no item has is a
      number: items may be dynamic, and a document may hold a value that they no longer have.
    - *Not changed:* `target` of a `toolOptions`, a pointer and a collection. Each is what its node
      is about, no node or meta has two, and renaming one now would break files.
27. **Docs.** *Fixed:* embedding.md has the JSON, what isn't there, conditions, versions and
    `commandDefaults`; the test app's plan has the panel's methods. CLAUDE.md is the user's to
    update.
