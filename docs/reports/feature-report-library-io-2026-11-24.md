# Feature report: library IO (P2a), 2026-11-24

- **Reviewed:** `lib/` (payloads, the library section, append, duplicate, lifetime), `command/`
  (block, attachment and library commands, snapshot undo) and `api/library_io.h` at `3e6cc1d`,
  against architecture §8a and the P2 plan's A1 to A6.
- **Reviewer:** a fresh agent given the code and the spec, not the author's reasoning. It ran most
  of its findings in scratch programs, built from an exported copy of the sources, and made 40
  mutations.
- **This file:** a rewrite of its review. Each finding ends with its resolution. Round 1 is commit
  `319b81d` (what other phases build on: 1, 2, 5, 9, 23 to 25 and part of 31). Round 2 is the
  commit that adds this file.

## Summary
- **What held up:**
  - a load adds placeholders and fills them in place, so `resolvePlaceholders` is the same code;
  - parsing into plain structures first, so a damaged file changes nothing;
  - ref tables beside the payloads, and payloads of newer or failing types that round-trip;
  - names from files never reach the Runtime, and ids use keyed hashes;
  - block types that can't be saved fail the save, and the audit of blocks' references;
  - do and undo leave the saved bytes equal for delete, detach, duplicate, rename and append;
  - `LibraryRef`, `LibraryPin` and `HistoryBlock`;
  - the tests: 36 of the 40 mutations were caught.
- **What didn't:**
  - things that a save lost without a word: what hangs on Transient blocks, attachments, values out
    of a range, blocks that only Host pins keep;
  - undo around placeholders: adopting a missing block, reading one after a command cut its
    reference, and attachments that couldn't be read;
  - provider slots that didn't say which document or block they were called for;
  - several libraries with one executor.

## High
1. **`BlockWriter::bag` wrote a pointer's stale id.** A duplicate's bag pointer went back to the
   original after a load, or failed every later save with the audit on.
   - *Fixed:* `BagRefs` (`lib/bag_refs.h`) holds a bag's cells by name, and `BlockWriter::bag` and
     `BlockReader::bag` take it. The cell says which block.
2. **`onWeakTargetLost` was called for placeholders, with null data,** for Detached users, and for
   weak references of slots and attachments.
   - *Fixed:* only for data that is there, for its own references. Attachments have the hook too.
3. **Adopting a missing block, undoing that and saving lost the record** and the links to it. The
   plan's claim 6 was false.
   - *Fixed:* the record keeps what it was. `Library::unadopt` takes the adoption back: the record
     is a MissingBlock again with its name, references, slots, attachments and flags, and the data
     moves to a Doomed record that the undo step pins. `readopt` is the redo. `CreatedBlocks` uses
     them. Host pins on the block keep the data alive as before.
4. **`attach` took null data, twice, and the save couldn't be loaded.**
   - *Fixed:* null data is InvalidArgument (also for a tag), and `hasAttachment` tests by type. The
     API can't make a block with one attachment or slot twice any more, which is what a load
     refuses.
5. **A pointer's key escaped the load's memory budget** (about 20 times over).
   - *Fixed:* at most 1,024 segments, written and read, and entries are charged for them.
     `embedding.md` says what the budget doesn't cover.
6. **Slots and attachments of Transient blocks were lost without a word.**
   - *Fixed:* a reachable Transient block with slots, attachments that are saved, or fields of a
     newer build is written as a record without data, with its references. The host fills it, and
     it has them again. It is in `SaveReport::transient` as before.

## Medium
7. **Blocks that only a Host pin keeps were lost at the second save.**
   - *Fixed:* `SaveReport::hostPinned` lists them, and what only they lead to. They stay save roots.
     The claim that a loaded file saves to the same bytes names its exceptions, the OptStrong
     pointer to a deleted block among them.
8. **`sourceName` leaked a local path into files,** and MissingBlock records had none.
   - *Fixed:* what a load says is kept in memory only. A save writes only what a file stored, or
     what `addPlaceholder` was given. Filling clears it. Missing blocks get it.
9. **Provider slots lacked context.**
   - *Fixed:* `Datablock::library()`; a `RekeyContext` for `rekey`; attachment slots get their
     block (except `destroy`: the history holds detached data for longer than the block exists);
     `SnapshotProvider` slots get the Context, and `capture` is a Status with the blob as an
     out-parameter, since a `Result` can't be a bound return type.
   - *Fixed:* an append fills a MissingBlock whose id `rekey` answers (`isFree` is true for it), if
     the record is of no type or the same one and has no fields of a newer build. The file's slots
     replace the record's; an attachment the record has stays, with an issue for the file's. Undo
     takes it back.
10. **A save didn't make the library busy.**
    - *Fixed:* `saveLibrary`, `writePayload` and `checkPayloadRefs` use the load's Scope: calls that
      return a Status are Busy, and void ones wait.
11. **Undo of `unlinkRef` after `resolvePlaceholders` failed with an unclear message** and dropped
    history.
    - *Fixed in the docs and the message:* `resolvePlaceholders` isn't undoable, so the host clears
      the history first, as before a load. The undo still fails, and says why.
12. **Attachment payloads weren't audited.**
    - *Fixed:* what `serialize` wrote is compared with `foreachRef`, as for blocks: a failure when
      the library audits, else an issue.
13. **An attachment type without `serialize` was dropped silently.**
    - *Fixed:* `AttachmentTypeInfo::storage`. A type without `serialize` says Transient, or a save
      of a block that has one fails and names it. `SaveReport::skippedAttachments` is gone.
14. **Opaque attachments couldn't be removed.**
    - *Fixed:* `Library::detachOpaque` and `attachOpaque`, and `core.block.detach` takes one out.
15. **A hostile file made every save quadratic** through raw ids with equal low halves.
    - *Fixed:* the writer's name table and `NameTable` are ordered maps.
16. **Several libraries with one executor:** undoing through another document's Context changed that
    document, and undoing after the Library was gone crashed.
    - *Fixed:* every built-in command and `SnapshotUndo` remembers its Library through a
      `LibraryRef`, and undo and redo are FailedPrecondition when it is gone or isn't the Context's
      (`detail::libraryOf`). A refused undo drops the step, as any undo that fails.
17. **A bag read clamped silently.**
    - *Fixed:* an entry whose value coercion would change stays unread, so a save writes it back. A
      number of another type with the same value, or an enum's key, is still taken.
18. **Nothing is bound.**
    - *Fixed in the docs:* `embedding.md` says that TS hosts are planned.
    - *Deferred to B4:* minimal save, load, append and resolve facades with their reports (P2 plan,
      "Step B4").
19. **Append is all or nothing.**
    - *Fixed:* `AppendOptions` is its own struct.
    - *Deferred:* choosing blocks and listing a file's contents (P2 plan, "Deferred by the review
      of library IO").
20. **No salvage of a damaged file.** *Deferred:* `LoadOptions::ignoreChecksum` (the same list).
21. **Types can't be renamed.**
    - *Fixed:* every lookup of a type by a stored name goes through `detail::typeNamed`
      (`lib/type_names.h`).
    - *Deferred:* the aliases (the same list).

## Low
22. **Who owns data when attaching fails.**
    - *Fixed:* `attachUndo` always takes it. The exception is InvalidArgument, for null data or a
      type that isn't registered: nobody here could destroy that. `Library::attach` says that the
      data stays the caller's.
23. **Unknown codes in a newer file came back as damage.** *Fixed:* Unsupported in a newer file.
24. **Old readers dropped trailing record bytes.** *Fixed:* each record ends with an extension of
    tagged fields, kept and written back, with a critical bit.
25. **No fixed-width arrays in the byte streams.** *Fixed:* `values`, `array`, `i32` and `f32`.
26. **Save limits exceed load limits.** *Fixed:* `SaveReport::loadBytes` and `loadBlocks` say what
    the limits must be. Entries that providers read come on top.
27. **The history counted only `sizeof`.**
    - *Fixed:* `BlockProvider::memSize` (optional), `Library::memSize` and `CreatedBlocks::memSize`.
      Deleted and undone blocks count their data, and the history measures a step again after its
      undo or redo. `core.block.detach` counts an opaque attachment. Other attachments have no size.
28. **`core.block.duplicate` took one block.** *Fixed:* `blocks`, a list as text
    (`formatBlockIds`), and it outputs the copies in order.
29. **A failed duplicate leaked the copy.** *Fixed:* the record comes first, so the data always has
    one; on a failure its references are severed and the provider destroys it.
30. **The loser of an alias got a random id without asking `rekey`.** *Fixed:* a foreign client's
    block asks. A refusal still gets a random id, with an issue that says so.
31. **Leaky or uneven API.**
    - *Fixed:*
      - the library's side of the payload classes is `lib/payload_access.h`;
      - the `detail::` functions are in `lib/io/load.h`, and compression in `lib/io/pack.h`, which is
        all that `snapshot_undo.cc` uses;
      - `AttachmentTypeInfo::payloadVersion`; `setSlot`, `slot` and `removeSlot` (was
        `forceUnlink`); `unlinkRef` (was `forceUnlinkRef`) and `relinkRef`;
      - `SnapshotRegistry::validate`; `unlinkRef` posts a change of `references`.
    - *Rejected:* `saveLibrary` taking a Context. It posts nothing and needs none; its header says
      why the two differ.
32. **Documentation.**
    - *Fixed:* `registry.h`, `provider.h` (Doomed blocks, the hook's states), `embedding.md` (what is
      saved, Host pins, checking SaveReport) and the task list.
    - *Not here:* `CLAUDE.md` says that only `base/` exists. No phase edits it.
33. **"Equal bytes" holds for one lz4 version.** *Fixed:* the header says so.
34. **Test gaps.**
    - *Added:* the four surviving mutations (new aliases before an append, the epoch after a late
      attachment, `probeMemory`, client names that collide), undo of an adoption, a second
      generation of Host-pinned blocks, duplicated bag pointers, Transient blocks with attachments,
      null attachments, placeholders as weak users, and two libraries.
    - *Not added:* files over 2 GiB, real out-of-memory behaviour, and ASan runs.

## Also in round 2
- **Deleting many blocks was quadratic** (found by P3: each `core.block.delete` rebuilt the relations,
  and each block that left the history scanned the library).
  - `core.block.delete` takes `blocks`, as duplicate does; `deletePreview` takes a set and builds the
    relations once; `Library::destroyFromHistory` takes a set. A test bounds the time.
