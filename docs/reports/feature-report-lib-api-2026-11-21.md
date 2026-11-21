# Feature report: library and datapath (`lib/`, `api/`), 2026-11-21

- **Reviewed:** the API surface of `lib/` (P1c) and `api/` (P1d) at `42c2dd3`, against
  architecture §5 and §8a and the P1 plan's P1c, P1d and implementation notes.
- **Reviewer:** a fresh agent given the code and the spec, not the author's reasoning.
  It confirmed 13 of its findings with scratch programs, built from an exported copy of
  HEAD so it stayed clear of the P1e work in progress.
- **This file:** a rewrite of its review. Each finding ends with its resolution.

## Summary
- **What held up:**
  - the GC's roots and two-phase sweep;
  - the per-Library handle checks;
  - path resolution over every grammar form;
  - the ChangeBus's alias and NaN behaviour.
- **What didn't:** the walk assumed things it never checked.
  - Every struct reached in another owner was assumed to be that owner's root.
  - Every pointer inside one owner was assumed fixed.
  - Both produced canonical paths that undo would follow to the wrong struct.
- **Also found:** GC ignoring Detached blocks' references, and several ways a misbehaving
  client could corrupt the id index or abort.

## Bugs
1. **B1, a non-root struct of another owner got a wrong canonical path.** An accessor
   returning `{&object->mod, Mod, owner: object}` canonicalized to `{object}.strength`, so
   a write landed in Object's own `strength`.
   - *Fixed:* when the owner changes, the struct must be that owner's root (compared with
     the block's data or the runtime owner's instance). Otherwise the result is
     Unsupported, with a message that says how to fix the accessor.
   - Nested structs inside one owner still append to the key, as before.
2. **B2, a Pointer value inside one owner stored a hop.** It meant "whatever
   `activeBrush` points at now", so undoing a pointer set did nothing.
   - *Fixed:* `PointerAccessor::address` gives the target its own address.
   - A settable pointer inside its owner with no `address` is Unsupported, both to read
     and to walk. Paths through it are rooted at the address.
   - A fixed pointer (no setter) keeps its hop, which is stable.
3. **B3, GC ignored a Detached block's provider refs.** The block's strong targets were
   collected, and came back dangling when the host restored it.
   - *Fixed:* `foreachRef` asks the provider for Detached blocks too. The provider
     contract says so, and to report nothing if the data is gone. `usersOf` sees
     Detached users as a result.
4. **B4, a placeholder saved under a client that later became an alias couldn't be
   found.**
   - *Fixed:* `ClientRegistry` has a generation counter that registering an alias bumps.
     The Library re-keys stale records under their canonical id on its next lookup,
     validate or collect.
   - Two records for one canonical id leave both in place and set a sticky
     AlreadyExists error.
5. **B5, a dynamic hook's ReadOnly was ignored by `setValue`.** *Fixed:* `setValue`
   checks the effective def. The enum-key `getMeta` reports the per-instance flag too.
6. **B6, `ChangeBus::flush` wrote through a dangling reference** when a getter added
   watchers during the re-read.
   - *Fixed:* the re-read goes into locals and is stored by index afterwards.
   - *Tested:* a getter that adds 64 watchers mid-flush.
7. **B7, a getter returning the wrong type aborted on component and enum-key paths.**
   *Fixed:* a value member's getter result is checked against its def, and a mismatch
   is Internal. That covers getValue, component and enum-key reads, and widened writes.
8. **B8, a repeated GUID corrupted the id index.** *Fixed:* `create` retries a used id up
   to four times, then returns Internal.
9. **B9, `destroyFromHistory` left Live Strong users dangling.**
   - *Fixed:* it refuses with FailedPrecondition, naming the user. The block stays
     InHistory, which GC treats as a root.
   - It also returns the sticky error when the provider's `destroy` fails, as `collect`
     does.
10. **B10, `keyOf` could return a non-key.** *Fixed:* a Member-kind key is Internal at the
    walk.
11. **B11, GC's mark phase wasn't guarded.** *Fixed:* a `collecting_` flag covers the
    whole of `collect()`, so library calls from a provider's `foreachRef` are Busy.

## Design
- **D1, no struct subtyping.**
  - *Fixed:*
    - `ApiRegistry::isA` walks the parent chain;
    - pointer type checks and `resolve(StableRef)`'s type check accept subtypes;
    - an untyped target in another owner takes that owner's type (a block's
      `dataStruct`), so a pointer declared as a base struct walks into the subtype's
      members.
- **D2, library changes never reached the ChangeBus.**
  - *Fixed:* each watcher records its Library's epoch, so creates, deletes, restores,
    detaches and reference writes refire it without a notify.
  - *Deferred to P3:* `CollectionAccessor::setActive`, which is in §5 and now recorded
    in the plan. Selection and active-object changes belong with the Scene; until then,
    the code that changes `active` bumps the structural epoch.
- **D3, a root with no owner dropped its change notifications.** *Fixed:* `Context`
  checks that a non-null root has an owner.
- **D4, "Strong means non-null" wasn't enforced.**
  - *Fixed in the docs:* `RefKind::Strong` now says it is meant to be non-null and is
    treated that way by UIs and the delete cascade. It isn't checked at write time,
    because a loader writes refs to blocks it hasn't read yet.
  - B9 covers the one place a Strong ref could break silently.
- **D5, `usersOf` loses the ref kind.** *Kept:* P1e's `deletePreview` filters by kind
  through `foreachRef` itself. A kind-aware `usersOf` can come when a second caller
  needs one.
- **D6, the P1f binding surface.** These are `std::string_view` parameters, overloads
  that share a name, templates, `WatchFn`, a raw `Context*` in watchers, `Datablock`
  having no members, and C++-only accessor closures.
  - *Deferred to P1f:* `bind/` gets binding-shaped facades with litestl strings,
    distinct names, Result returns and a JS-owned Context with an explicit lifetime.
  - TS-defined ApiStructs need foreign accessors, which `bind/` adds as a struct whose
    accessors are foreign calls.
- **D7, host misuse that aborts** (unbalanced pins, `endNoCollect`, builder kind errors,
  empty watch callbacks). *Deferred to P1f:* the `bind/` facades return Status for
  these. The C++ API keeps them as programmer errors.
- **D8, missing operations.**
  - *Fixed:* `ApiRegistry::structCount`/`structAt` and `ApiStruct::descriptionText`.
  - *Deferred to P2:* `Library::rename`, and enumerating slots, attachments and
    overrides for library IO.
- **D9, confusing names.**
  - *Fixed:* `Library::resolve(RefCell)` is now `resolveRef`.
  - *Kept:* "canonical" for client aliases and for StableRefs. Each is clear from its
    type.
  - *Kept:* the two epochs, which are documented where they are declared.
- **D10, `destroyBlocks` enumerates every survivor per destroy.** *Deferred:* use the
  relations cache for phase 1a when profiling shows it matters (TS providers in P1f).
- **D11, minor.**
  - *Fixed:*
    - the `slot()` pointer's lifetime is documented;
    - `destroyFromHistory` reports a failed destroy;
    - the ChangeBus `read` comment matches the code.
  - *Kept:* `usersOf` caching relations while a sticky error is pending. `collect`
    already refuses to run then.

## Test gaps
- *Added:*
  - every bug above;
  - a base-typed pointer walk and write-back;
  - a settable pointer with and without an address;
  - a nested struct inside a block;
  - library changes refiring watchers;
  - a re-entrant getter during flush;
  - a placeholder that becomes an alias;
  - a repeated GUID;
  - Detached users and GC;
  - a mark-phase create.
- *Not added:*
  - a `[#handle]` path through a collection (the fixture's collections are keyed by
    name);
  - a `refine` pointer walk;
  - a weak ref revived by `reviveAs`;
  - attachments freed by `destroyFromHistory`.

  These are tracked for the P1f test pass.

## Spec deviations
- **Now recorded in the plan:**
  - a struct crossed into must be its owner's root;
  - settable pointers inside an owner need `address`;
  - pointer type checks accept subtypes;
  - Detached blocks contribute provider refs;
  - `setActive` moves to P3.
- **Harmless:** the `addFloat(...)` builder became `addField`.
- **The reviewer judged these recorded deviations justified:**
  - provider slots taking a Datablock;
  - `QueryAnswer`;
  - the concrete `RefVisitor`;
  - serialization slots moving to P2;
  - `Library` not being a Runtime extension (B4 is now handled without making it one);
  - the ChangeBus index dropped in favour of the value diff.
