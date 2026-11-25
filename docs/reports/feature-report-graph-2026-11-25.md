# Feature report: graphs (`graph/`, P5), 2026-11-25

- **Reviewed:** `graph/`, its bindings and tests at `702acec` (P5 steps 1-11), against the
  [P5 plan](../plans/2026-11-24-p5-graph.md) and what P5c, P6 and a node editor will need.
- **Reviewer:** a fresh agent given the code and the plan, not the author's reasoning. It ran
  experiments in a scratch clone, and says which of its findings it ran and which it only reasoned.
- **This file:** a rewrite of its review. Each finding ends with its resolution. Round 1 is wrong
  results and lost data, round 2 cost, round 3 the API.
- **Times** are the reviewer's, RelWithDebInfo, on a machine that other agents used too.

## Summary
- **What held up:**
  - the traces with resolved origins: no stale result was found across rewires, undo, interface type
    changes, late types, host destroy and restore, replaced defs, or notifications while evaluating,
    other than M1 and L4;
  - fresh stamps everywhere, and equality instead of ordering;
  - the pull on an explicit stack, and its cycle errors;
  - foreach's level 1: an unrelated edit of 100,000 elements costs 11-13 microseconds;
  - reading files, and the random tests with independent oracles.
- **What didn't:**
  - undo through datapaths wrote defaults back as values (H2);
  - an instance kept what it knew of removed nodes forever (H3);
  - costs: each edit verifies everything reachable (H1), links of interior sockets on one group are
    cubic (H4), and foreach's cache is bounded by elements, not memory (H5).

## High
1. **H1, every edit verifies the whole reachable graph.** 3.8-5.2 ms for 10,000 transforms and 63-83 ms
   for 100,000, where one leaf changed; an edit anywhere in the Runtime counts. Half of it is hashing
   socket paths. The target is 2 ms for 10,000. *Round 2.*
2. **H2, undo of an Optional param gave another value.** A path that set an unset param (or literal, or
   interface default) was undone by setting the default. For an Optional param that isn't the same value,
   and it was saved. *Fixed:*
   - `ValueAccessor::save`: an accessor may return a function that puts back what is stored. SetProperty
     uses it for undo and redo. Graph params, literals, group sockets and interface defaults have one.
   - So an unset value is unset again, a stored value that its def doesn't accept comes back, and nothing
     is pinned to today's default. The note in the plan's step 6 is corrected.
   - *Tested:* `test_graph_undo`, and the random test now compares what is stored, not what it evaluates to.
3. **H3, results of removed sockets stayed.** `value()` still gave them, and 100 removed nodes kept 100
   payloads alive. *Fixed:*
   - NotFound drops the result. Each `evaluate` that comes after a structural edit (or a Library change,
     which can write a group's reference) drops the results of sockets the defs no longer have, in nested
     groups too.
   - A group that isn't there may come back, so results inside it stay.
   - `forget(SocketPath)`.
   - *Tested:* `test_graph_results`, and the random test checks every path it ever asked for.
4. **H4, interior sockets on one group:** 1,600 inputs and outputs take 10 s to link. *Round 2.*
5. **H5, foreach's cache is far too big:** 1 GB for 16,384 elements of a 50-node body. *Round 2.*

## Medium
1. **M1, an instance with the wrong Library served stale results.** *Fixed:* of a def that the client
   keeps, a null `lib` means the def's. Another Library, or another Runtime, makes `evaluate` and `setInput`
   InvalidArgument. Not an abort: scripts make instances too.
2. **M2, `evaluate` could destroy its own instance.** Replacing a graph's data drops the old frames, and
   with them the last pin of a block that owns the instance. *Fixed:* the no-collect scope opens before the
   frames are touched, so the block goes when `evaluate` has nothing left to do. The destructor has a scope
   too: blocks it lets go are destroyed when all of the instance is.
3. **M3, a stored reference's strength overrode its node type's.** *Fixed:* the type's wins when a node is
   read, inserted or restored, and when the type is registered later. For the last, node types are found when
   they are registered and not at the next look at the node (the registry knows the Runtime's defs). Until
   then the stored strength counts.
4. **M4, small hostile files hang the first evaluation.** *Round 2.*
5. **M5, block dependencies are per block:** the notified member is dropped. *Round 3.*
6. **M6, deleting children of a hub is quadratic.** *Round 2.*
7. **M7, a structural edit rebuilds the whole topology.** *Deferred to P5c*, tracked in the plan.
8. **M8, foreach has a cliff past `maxCached`.** *Round 2.*
9. **M9, foreach can't be used without native code.** *Round 3.*
10. **M10, what a node editor needs and can't get.** *Round 3*, but for:
    - per-node UI data: nodes have extras (`GraphDef::setExtra`, with undo, kept in files). Those of a kind
      this build doesn't know, and references in them, are kept too.
    - `nodes[i]` is O(1), also after removals.
11. **M11, what TS kernels can't express.** *Round 3.* Geometry payloads in TS: *deferred to P6*.
12. **M12, parallel evaluation isn't free later.** *Round 3.*
13. **M13, P5c can't cheaply find out that adapters changed.** *Round 3.*
14. **M14, nothing guards performance.** *Round 2* (growth tests).

## Low
1. **L1, `setInput` takes a name only.** *Fixed* in round 1, with L2: a name or a key.
2. **L2, the value of a removed interface input can't be unset.** *Fixed, though not by pruning:* undo brings
   the input back under its id, and dropping the value then would lose it. None by the input's key unsets it,
   and `clear()` drops the values of inputs the interface doesn't have.
3. **L3, the key of foreach's `elements` socket didn't resolve**, and a crafted empty key was read past its
   end. *Fixed:* keys that aren't `#<id>` exactly are string keys, which lookup tries before names. Files
   can't have sockets without a key.
4. **L4, `Library::rename` doesn't notify.** *Fixed:* lib/ is below the ChangeBus and can't. A block counts
   its renames, and that is part of what a kernel that uses it depends on.
5. **L5, undoing a removal put the node last.** *Fixed:* node order is a list, and the undo record knows which
   node was before (`NodeState::after`). Saved bytes don't depend on the order links, params, literals or extras
   were stored in, so undo gives the same file.
6. **L6, an over-budget evaluation costs 0.5 s each time.** *Round 2.*
7. **L7, `GraphEditRecord::pin` is quadratic.** *Round 2.*
8. **L8, unused nodes keep their outputs.** *Round 3.*
9. **L9, node types have no version.** *The format has it:* `NodeTypeDef::version`; nodes keep the version they
   were made with, in files too. There is no migration hook yet.
10. **L10, an edit costs O(groups), also of unrelated graphs.** *Round 2.*
11. **L11, `GraphSystem::versions_` and `TypeChains` only grow.** *Round 3.*
12. **L12, an interface socket named `#123` shadows a key.** *Fixed:* names can't start with `#` or `@`, in
    files neither.
13. **L13, docs.** *Round 3.*
14. **L14, all inputs of a node are needed.** *Deferred*, tracked in the plan.

## Also found
- `graph.create` kept a raw Library pointer, which undo read after the Library was gone, and didn't count
  what an undone creation holds. `GraphEditRecord` edited the graph it was made for when undone through
  another document's Context. *Fixed* as finding 16 of the
  [library IO review](feature-report-library-io-2026-11-24.md).
