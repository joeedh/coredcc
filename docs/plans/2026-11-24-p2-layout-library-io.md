# P2: library IO (P2a) and layout (P2b)

Status: pressure-tested and revised (2026-11-24); not yet implemented. The record is at the
end. Architecture: §8a ("Files, linking and import", "Attachments"), §12, §14 P2. Test app
stage TA2 (`docs/plans/2026-11-19-test-app.md`). Branch `p2-layout`.

- **P2a, library IO:** steps A1 to A6, then its feature review ("library IO", added to the
  task list row).
- **P2b, layout and TA2:** steps B1 to B5, then its feature review ("layout").
- A1 is the shared step: it merges to master first, and P3 and P5 rebase onto it.

## Ownership between the parallel phases
| Phase | Owns |
|---|---|
| P2 | Value bytes; `BlockProvider`'s optional slots; **library lifetime** (pin kinds, deferred destruction of pinned blocks, `LibraryRef`, `LibraryPin`); attachments; `Library::rename`; snapshot undo and lz4; CreatedBlocks with BlockIds; `layout/`; TA2; `gen-paths`. Value bytes, slots, lifetime and the attachment registry are step A1, merged early. |
| P3 | `scene/`, `bvh/`, `draw/View` if it needs it for screen distance, `EvalCtx`, `CollectionAccessor::setActive`, the Context scene slot, display units and frames. Its providers' `serialize` is its last step. |
| P5 | `graph/`, the Runtime's global version counter, payloads, and **immediate change notification**: ChangeBus listeners, notify before the `update` hook, `notifyAll()` (its step 1, merged early, independent of P2). GraphDef serialization is its last step. |
| Shared files | `source/coredcc/CMakeLists.txt`, `tests/CMakeLists.txt`, the task list, `docs/debugging.md`, `docs/embedding.md`: small appended changes only. Task list: each phase edits its own row and focus lines. No phase edits `CLAUDE.md`. |

**Files P2 shares with P5 and P3** (real conflicts, so each side keeps its change small):
- `lib/`: P2 only, after A1. P5 no longer edits `destroyFromHistory` or `unpin`.
- `api/change_bus.*`, the write path in `api/resolve.cc`, `base/runtime.h`, the Busy checks in
  `command/executor.cc`: P5. P2's change in `executor.cc` is `inputsFromInvocation` only.
- `api/struct.h`: P3 adds `setActive`; P2 adds `PointerAccessor::cell` and
  `CollectionAccessor::staticItem` (one optional field each).

## Scope

### In scope, with where it was deferred from
| Item | From | Step |
|---|---|---|
| `serialize`, `deserialize`, `duplicate`, `rekey` slots | P1c notes | A1 |
| Attachment type registry, serializer, duplicate policy, opaque attachments | P1c notes | A1, A4 |
| Attach and detach commands | P1e notes | A3 |
| `Library::rename`; enumerating slots, attachments, overrides | lib and api review D8 | A2 |
| `unlinkRef` (planned as `forceUnlink`) for references inside a placeholder | §8a | A2 |
| `CreatedBlocks` with BlockIds | command review, design 9 | A3 |
| Library section: save, load, placeholders | §8a, §14 | A4 |
| Append with rekeying; duplicate | §8a | A5 |
| Snapshot undo (before and after blobs), lz4 | command review, design 8 | A6 |
| Text form of Pointer values | props review D5; TA1 review S2 | B1 |
| `meta(path)`, enum and flags names, `resolve` giving a kind | TA1 review S3, S5 | B1 |
| Layout nodes, builder, XML subset, `toJson`, validator, example layouts | §12, §14 | B2, B3 |
| TA2: the property panel built from a layout file | test-app plan | B4 |
| `gen-paths` (typed paths for TS) | §13 (no phase owned it) | B5 |

### Out of scope
- **A scene file, or file system access.** The section is bytes in and bytes out.
- **Link** (blocks that stay in another file), and unregistering types at run time.
- **Host-owned block storage,** until the Unreal spike (P4). See "Blocks without a payload".
- **Snapshot undo over single blocks** (`BlockSnapshotUndo`, `replaceData`): cut, no client.
- **Block and attachment providers written in TS.** Their slots use `void *`, which
  `foreignImplementable` refuses.
- Frontends beyond the test app's panel. Mat4 writes, registry removal, enum inputs and
  ranges in script def builders (deferred in P1f). A list widget in layouts.
- **Migrating payloads:** a provider reads its own older versions.

## Module placement
`base` ← `props`, `parse` ← `lib` ← `api` ← `command` ← `layout` ← `bind`
- `lib` gains `props` (allowed by P1's layering) and, from A4, litestl's `io` target (lz4).
- The root CMake's SYSTEM loop for host-supplied litestl gains `io` and `lz4`.

---

# P2a

## Step A1: the shared step
Three commits: lifetime, bytes and values, slots and attachments. Saving isn't in it.

### Library lifetime
```cpp
// lib/block.h
enum class BlockState : uint8_t { Live, InHistory, Detached, Placeholder, Doomed };
enum class PinKind : uint8_t { Host, History };
int Datablock::pins() const;                 // both kinds
int Datablock::pins(PinKind) const;
uint64_t Datablock::stateChanges() const;    // bumped by every state transition

// lib/library.h
void pin(BlockHandle h, PinKind kind = PinKind::Host);
void unpin(BlockHandle h, PinKind kind = PinKind::Host);
bool busy() const;                           // sweeping or collecting

// lib/pin.h
class LibraryRef {                           // weak, copyable; from Library::ref()
  Library *get() const;                      // null once the Library is being destroyed
};
class LibraryPin {                           // RAII, move-only, over a LibraryRef
  LibraryPin(Library &lib, BlockHandle h, PinKind kind = PinKind::Host);
  LibraryPin copy() const;
  void reset();
  Library *library() const;
  BlockHandle handle() const;
  bool dataAlive() const;                    // library alive; Live, InHistory or Doomed
};
```
- **Who uses which:** graph payloads and evaluated state use Host. Undo records
  (SetProperty, CreatedBlocks, P5's edits) use History. GC treats both as roots. A save
  treats only Host pins as roots (A4), or the undo stack would put deleted content in files.
- **`destroyFromHistory` on a block with Host pins** (after its usual check for Strong users)
  returns ok and makes it **Doomed**:
  - it leaves the id index at once: `find` gives nothing, and `createWithId` and `reviveAs`
    accept its id. `get(handle)` still works; `forEach`, `usersOf` and saves skip it;
  - `onWeakTargetLost` runs now (later the id may name another block);
  - as a GC root it keeps its strong targets alive. Nothing else can keep it alive;
  - it is destroyed when its last Host pin goes. Inside a sweep or a no-collect scope, that
    waits for the end of it.
  - **Rechecked at that time:** if a Live or Detached block holds a Strong ref to its id, and
    no other record has the id, it goes back to InHistory instead, as if
    `destroyFromHistory` had been refused. If another record has the id, that ref now means
    the other record, so the block is destroyed.
- History pins don't doom: the records holding them can't undo past the one being dropped.
  This is today's behaviour. An unpin through a stale handle does nothing, as today.
- **Errors from a deferred destroy** are taken from the sticky error and kept on the Library
  (16 at most, plus a count). The next `collect()` returns them after doing its work.
- `LibraryPin` and `unpin` are main-thread only. A payload released on a worker must hand its
  pin back to the main thread; that is P5's side.
- **The epoch is also bumped by** `attach`, `detach`, `setOverride` and `clearOverride`: they
  change what `query` answers.
- A host destroying data (`onHostDestroyed`) isn't delayed by pins. `dataAlive()` says so.

### Bytes: `base/bytes.h`
```cpp
class BytesWriter {
  void u8(uint8_t); void u16(uint16_t); void u32(uint32_t); void u64(uint64_t);
  void i64(int64_t); void f64(double); void boolean(bool);
  void str(std::string_view);                       // u32 length; stops at a NUL, as litestl strings do
  void blob(std::span<const uint8_t>);              // u32 length
  void fail(Status);  const Status &status() const;
  const litestl::util::Vector<uint8_t> &data() const;
  litestl::util::Vector<uint8_t> take();
};
class BytesReader {
  BytesReader(std::span<const uint8_t> bytes);      // borrowed
  uint8_t u8(); ... bool boolean(); double f64();
  std::string_view str();                           // a view; a NUL inside is a failure
  std::span<const uint8_t> blob();
  uint32_t count(size_t minItemBytes);              // fails unless n * minItemBytes <= remaining()
  size_t remaining() const;  size_t offset() const;  bool atEnd() const;
  void fail(Status);  const Status &status() const; // the first failure, with its offset
};
```
- Its own 100 lines rather than a wrapper: litestl's `ByteReader` is bounds-safe, but wrapping
  it would put its unprefixed include (`"util/vector.h"`) in a coredcc header.
- **The writer never emits what the reader rejects.** Text is any bytes without NUL: no UTF-8
  check, and no length limit but the remaining bytes. P1 already accepts Latin-1 and
  100,000-byte names, and a save must not produce a file that can't be opened.
- A write past `kMaxBytes` (1 GiB; 256 MiB on 32-bit) is a sticky OutOfRange.

### Names: `base/name_table.h`
```cpp
class NameTable {                                   // id -> name, per IdDomain; bounded (16 MiB)
  bool add(IdDomain, std::string_view name);        // false when full or the id has another name
  const char *nameOf(IdDomain, uint64_t id) const;
};
NameTable &Library::fileNames();
```
- Names that only a file knows (a missing add-on's struct, a runtime owner) go here, **never
  into the Runtime's registry**: a file's name must not make a plug-in's registration fail.
- On the wire a name is `u8 form`: null, name, or raw id. The writer looks in the Runtime, then
  the table, and falls back to the raw id, so an unknown name never fails a save.
- A name in a file that hashes to an id the Runtime registered under another name fails the
  read (InvalidArgument). Only that file suffers.

### Values: `props/value_io.h`
```cpp
class ValueCodec {                                  // an ordinary virtual class, internal to C++
  ValueCodec(const Runtime &rt, NameTable *names);  // names may be null (names then come back as ids)
  virtual void writeBlockId(BytesWriter &, const BlockId &);      // default: client name + 128 bits
  virtual void readBlockId(BytesReader &, BlockId &);
  void writeName(BytesWriter &, IdDomain, uint64_t id) const;
  uint64_t readName(BytesReader &, IdDomain);
};
void writeValue(BytesWriter &out, const PropValue &v, ValueCodec &codec);
PropValue readValue(BytesReader &in, ValueCodec &codec);       // None after a failure

struct BagIssue { litestl::util::string name; Status why; };
void writeBag(BytesWriter &out, const PropertyBag &bag, ValueCodec &codec);
void readBag(BytesReader &in, PropertyBag &bag, ValueCodec &codec,
             litestl::util::Vector<BagIssue> &issues);          // set entries into code's defs
```
- **Wire tags** are their own constants, not `PropValue::Type`'s numbers.
- Floats keep their bits (NaN, -0.0). A Pointer is: owner kind; block id through the codec, or
  the runtime owner's name; key segments; the struct's name. Handles must be non-negative.
- **A bag entry** is name, tag, u32 length, bytes. The length lets an unknown tag be skipped.
  Only set entries are written. Enum and Flags are written as keys (`"EDIT"`, `"A|B"`), or as
  numbers when there is no item for the value; `coerce` then rejects the number on read, so
  the entry is an issue (and, in a block, an unread entry that is kept).
- These bag functions are for data outside blocks (preferences, clipboards). Blocks use the
  entries below.

### Payloads: `lib/block_io.h`
```cpp
/** A named value in a payload. */
struct BlockEntry {
  litestl::util::string name;
  PropValue value;                          // None when unknown
  litestl::util::Vector<RefCell, 1> refs;   // the block a Pointer value names; the truth for its id
  uint8_t unknownTag = 0;                   // set: a tag this build doesn't know
  litestl::util::Vector<uint8_t> raw;       // its bytes
};

class BlockWriter {
  BytesWriter &bytes();
  void ref(const RefCell &cell);            // appends a row to the ref table, writes its index
  void entry(const BlockEntry &e);          // refs[0].kind is the row's kind
  void entries(std::span<const BlockEntry> all);
  void bag(const PropertyBag &bag, RefKind pointerKind, std::span<const BlockEntry> unread = {});
  static const lbt::Struct<BlockWriter> *defineBindings();      // opaque
};
class BlockReader {
  uint32_t version() const;                 // the payload version it was written with
  BytesReader &bytes();
  void ref(RefCell &cell);                  // sets the target; the cell keeps its own kind
  void entries(litestl::util::Vector<BlockEntry> &out);
  /** Entries no def takes, or that don't coerce, land in `unread`; keep and rewrite them. */
  void bag(PropertyBag &bag, litestl::util::Vector<BlockEntry> &unread);
  void setData(void *data);                 // becomes the record's clientData
  Library &library() const;                 // ids and names only; no other block's data
  static const lbt::Struct<BlockReader> *defineBindings();      // opaque
};
class BlockCopy {
  void setData(void *data);
  static const lbt::Struct<BlockCopy> *defineBindings();        // opaque
};

struct BlockPayload {
  uint32_t version = 1;
  litestl::util::Vector<uint8_t> bytes;
  litestl::util::Vector<RefCell> refs;      // the ref table
};
/** What save, load and duplicate use, and P3's and P5's tests. */
Result<BlockPayload> writePayload(const Library &lib, BlockHandle h);
Status readPayload(Library &lib, BlockHandle h, const BlockPayload &p);
```
- **A payload holds no BlockId, by convention:** it holds indices into the block's ref table,
  which the library stores beside it. GC, remapping, append and placeholders work on the
  table alone. `bytes()` can't enforce that, so the audit checks it.
- **The table is written as is.** What a save nulls is decided by the save, on every table (the
  provider's, slots, placeholders, attachments), not by the writer: duplicating must keep refs
  to deleted blocks.
- **Audit:** the *set* of non-null targets and strengths in the table, against what
  `foreachRef` reports. Rows aren't deduplicated, and listing a ref twice is fine. A difference
  is Internal with the relations audit on (debug), else an issue on the save.
- **The raw entry's refs are laid out in the entry's header,** outside `raw`, so an unknown
  entry's rows survive rewriting. A provider that keeps `unread` entries reports their `refs`
  from `foreachRef`.
- **For P5:** a node's Pointer parameter is a `BlockEntry` (or has its own RefCell) whose cell is
  the truth; the `PropValue`'s block id is a copy. A node of an unknown type keeps its
  `BlockEntry` list.

### Provider slots
```cpp
// BlockProvider, all optional
Status serialize(const Datablock &block, BlockWriter &out) const;
Status deserialize(const Datablock &block, BlockReader &in);
Status duplicate(const Datablock &src, BlockCopy &out);
BlockId rekey(const Datablock &imported) const;       // null: refuse the append

// BlockTypeInfo
enum class BlockStorage : uint8_t { Unspecified, Payload, Transient };
BlockStorage storage = BlockStorage::Unspecified;     // becomes Payload when the slots exist
```
- `deserialize` makes new data and calls `setData`; on an error it has freed what it made. The
  record exists (id, name, slots); its data isn't there until it returns ok.
- **Deviations from §8a:** `duplicate` is per block and copies refs verbatim; the library then
  rewrites them through `foreachRef` (handles are per Library, providers per Runtime). `rekey`
  gets the imported record: payloads hold no ids.
- `BlockTypeRegistry::add` refuses one of `serialize` and `deserialize` without the other, and
  Transient with either.

### Attachments
```cpp
struct AttachmentProvider : IfaceRef {                 // lib/attachment.h
  void destroy(void *data);
  void *create();                                      // optional
  void *copy(const void *data);                        // optional
  Status serialize(const void *data, BlockWriter &out) const;      // optional, with the next
  Status deserialize(BlockReader &in);                 // optional; in.setData
  void foreachRef(const void *data, RefVisitor &visitor) const;    // optional
};
enum class DuplicatePolicy : uint8_t { Copy, Drop };
struct AttachmentTypeInfo { string name; StructId dataStruct; uint32_t version = 1;
                            DuplicatePolicy onDuplicate = DuplicatePolicy::Drop; };
class AttachmentTypeRegistry {                         // a Runtime extension
  Result<TypeId> add(AttachmentTypeInfo info, Owned<AttachmentProvider> provider);
  const Entry *find(TypeId type) const;
};
Status Library::attach(BlockHandle h, TypeId type, void *data);    // the type must be registered
```
- It replaces P1's `attach(h, type, data, destroyFn)`. Its callers are three lines of one test;
  P3 registers one attachment type for its script overrides.
- `foreachRef` also visits attachments. `const void *` parameters need the same binding support
  as `void *`; if litestl lacks it, the slots take `void *`.

### A1's tests
- Lifetime: P5's S4 to S6 scenarios: doomed then revive under the id; last unpin in a sweep, in
  a no-collect scope and outside; a Strong user restored meanwhile (id free, id taken); a failing
  destroy; pins after `~Library`; `LibraryRef` during `~Library`; `stateChanges` on every
  transition; the epoch bumps.
- Bytes and values: every type; every truncation; counts near 2^32; the writer's output always
  reads back (random ugly strings, names, keys).
- Payloads through `writePayload` and `readPayload`, natively and through the fake dispatcher;
  unknown tags and unread entries rewritten with their refs; the audit both ways.

## Step A2: library bookkeeping
- **Deleting any state:** `markDeleted` takes Live, Placeholder and Detached blocks;
  InHistory remembers the state it came from, and `restoreFromHistory` goes back to it.
  `onHostDestroyed` and `onHostRestored` on an InHistory block change the remembered state.
  - `foreachRef` and `destroyBlocks` treat an InHistory placeholder as a placeholder.
  - `deletePreview` and `destroyFromHistory` count Placeholder users too. Detached users stay
    out of the cascade (they are the host's).
- **Placeholder refs:** a placeholder keeps its ref table, visited by `foreachRef`.
  `forceUnlinkRef(h, index)` nulls a row (indices stay valid), and `core.block.unlinkRef` is the
  undoable command. `forceUnlink` on slots stays. (Both were renamed by the review: `unlinkRef` and
  `removeSlot`.)
- **Two reasons** (`PlaceholderReason`):
  - `MissingType`: a record from a file whose type or payload can't be read. A GC root.
  - `MissingBlock`: the target of a Strong or OptStrong ref that has no record, or a record saved
    without a payload. A GC root only if it was a record (it has a type name).
  - `sourceName()` records where it came from.
- **Adoption:** `createWithId` and `reviveAs` on a `MissingBlock` record fill it in place, keeping
  its handle, pins, slots and attachments (a stored type name must match). It drops the ref
  table: the provider is the truth from then on. This is a state change, not AlreadyExists.
- **Rename:** `Library::rename`, epoch bump; `core.block.rename`, undoable, which also calls
  `notify(owner, "name")`. Names stay non-unique.
- `forEachSlot`, `forEachAttachment`, `forEachOverride`.
- **The id index** is keyed by a hash seeded per Library (from the GUID source): `LocalId`'s hash
  is invertible, so a file could make every id collide. The relations map uses the same key.
- **SetProperty keeps links to blocks that aren't Live.** `PointerAccessor::cell` (optional)
  returns the member's `RefCell`. With it, SetProperty saves and restores the target id, so
  setting a pointer away from a placeholder and undoing restores the link. Without it, as
  today. P3 and P5 fill it for their settable block pointers.

## Step A3: undo of block commands
- `CreatedBlocks` and DeleteBlock store the handle *and* the BlockId: the handle while it names
  a record with that id, else `find(id)`. They pin with `PinKind::History`, and unpin only the
  handle they pinned (an id could now name another record).
- `core.block.attach` (needs `create`), `core.block.detach`, and
  `attachUndo(ctx, h, type, data)`. The command owns detached data until it leaves the history.
- `addAttachments(ApiStruct &)` (`api/`): the `attachments` collection of a block struct, keyed
  by type name, so `objects["A"].attachments["my.data"].x` resolves and undoes.

## Step A4: the library section
```cpp
struct IoIssue { BlockId block; string blockName; Status why; };
struct IoIssues { Vector<IoIssue> first; int64_t total; };       // at most 1,000 kept
Result<Vector<uint8_t>> saveLibrary(const Library &lib, SaveReport *report = nullptr);
struct LoadLimits { uint64_t maxBytes; uint32_t maxBlocks = 1u << 20; };  // 2 GiB; 128 MiB on 32-bit
Result<LoadReport> loadLibrary(Context &ctx, std::span<const uint8_t>, const LoadOptions &);
Result<LoadReport> resolvePlaceholders(Context &ctx);            // after types were registered
```
- They take a Context so they can notify: they are in `api/library_io.h`, over functions in
  `lib/io/`. A load, an append and a filled placeholder call `notify` for each block they
  touched. (Adoption by a bare `createWithId` is seen through `stateChanges` and the epoch.)

### What is saved
- **Mark** from the save roots: Root, FakeUser, Host pins, Detached blocks, and placeholders that
  were records. InHistory and Doomed blocks aren't written. Saving frees nothing.
- **Blocks without a payload:**
  - `Unspecified` type (no slots, nothing declared): **the save fails** with FailedPrecondition
    naming the types. Forgetting to serialize is an error, not a loss.
  - `Transient` type: not written. The mark goes through it, and the report lists every reachable
    Transient block, and every block reachable only through one (kept in this file, but an
    orphan after loading until the host links it again).
  - A Detached block, whatever its type: its data is the host's and is gone. It is written as a
    record without a payload: name, type, flags, slots, attachments, and the refs its provider
    reports. It loads as `MissingBlock`, keeps those refs alive, and the host adopts it with
    `createWithId`. The report lists it. This covers host-owned blocks until P4: such a type is
    Transient while Live, its targets are refs the host re-creates, and refs *to* it load as
    `MissingBlock` records that the host adopts under their stable ids.
- **Refs to blocks that aren't saved,** in every table: OptStrong to an InHistory block, or to an
  id with no record, is written null (the user deleted it). Strong and Weak keep the id.
- Blocks are sorted by (client name, id), slots by (namespace, key), attachments by type name, so
  equal libraries give equal bytes.
- A `serialize` that fails fails the save, naming the block.

### Format, version 1
Little-endian.
```
"CDCCLIB\n" | u32 version | u32 minReader | u64 totalBytes | u32 chunkCount (<= 16)
chunk: u32 tag | u32 flags (1 critical, 2 lz4) | u64 raw | u64 stored | bytes
NAME  critical, once: strings, by index
BLCK  critical, once: records, each u32-length-prefixed:
      client name, 128-bit local id, type name, block name, u32 flags, u8 storage
      (Payload, MissingType, NoPayload), u32 payloadVersion, source name,
      ref table: rows of (u8 kind, client name, 128 bits; null allowed),
      slots: (namespace name, i64 key, row), attachments: (type name, u32 version, blob index,
      ref table), payload blob index or none
DATA  critical, once: blobs: u8 lz4, u64 raw, u64 stored, bytes
u64 fnv1a64 of everything before it
```
- Blobs of 512 bytes or more are compressed (lz4hc's default level) when that is smaller; `NAME`
  and `BLCK` are compressed as chunks. `raw <= stored * 255 + 64` is checked before allocating.
- **Versions:** newer `version` with `minReader` at most ours: read; unknown non-critical chunks
  and trailing record bytes are skipped, and each kind skipped is an issue (a re-save drops it).
  A newer `minReader` or an unknown critical chunk is Unsupported. The header is checked before
  the checksum, so a newer file is reported as newer.
- A payload newer than the registered version loads as `MissingType` (it round-trips).
- Missing or repeated chunks, and bytes after the checksum, are errors.

### Loading
- **Needs a library with no records.** Busy to mutation while it runs (the sweep's rule).
- **Phase A, parse:** into a plain structure; the Library and Runtime are untouched. Corruption is
  InvalidArgument with the offset; limits are OutOfRange.
  - **One memory budget** (`maxBytes`) is charged with in-memory cost before each allocation:
    about 1 KiB per record, the size of rows, entries and strings, and raw blob bytes times 1.5
    (litestl `Vector` growth). `maxBlocks` counts the `MissingBlock` records a load would make.
  - Before a large allocation the same size is allocated and released through litestl's
    `alloc`, because its Vector writes through a null pointer when memory runs out.
  - Duplicate checks sort. Nothing is quadratic in one record or over records.
- **Phase B, ids:** client aliases and name collisions with the Runtime are checked.
  - Two records that become one id through an alias: the one under the current name keeps it, the
    other gets a fresh id and an issue. The document still opens.
  - Nothing is registered in the Runtime. Names go to `fileNames()`.
- **Phase C, fill;** it can't fail as a whole:
  - records first, then slots, attachments and tables, then each `deserialize`;
  - an unknown type, a type of another client than the record's, or a `deserialize` that fails (a
    sticky error is taken) makes a `MissingType` placeholder, with an issue. Nothing is lost;
  - an unknown attachment type becomes an **opaque attachment** (type name, version, bytes, ref
    table), visited by `foreachRef` and written back;
  - a null Strong slot is kept, with an issue;
  - `MissingBlock` records are made from what `foreachRef` reports after `deserialize` (a table
    row's kind may be out of date), and from tables for placeholders.

## Step A5: append and duplicate
- `appendLibrary(ctx, bytes, options) -> {IdMap, LoadReport}`; ids are planned first:
  - `coredcc` blocks, `MissingType` placeholders of the `coredcc` client included, always get
    fresh ids; other clients' go through `rekey`. No slot, no type, a null answer, an id in use or
    in another client refuses the whole append, listing the types.
  - Refs within the appended set follow the map. Others resolve in the target library or become
    `MissingBlock`.
  - Root flags become FakeUser, so appended content survives the next save.
- `duplicateBlocks(lib, handles) -> IdMap`: the slot, else `writePayload` and `readPayload`. Refs
  within the set are remapped through `foreachRef`, others are shared. Slots are copied;
  attachments follow their policy, opaque ones are copied; Root, pins and overrides aren't. A
  failure undoes the whole call. The tables aren't nulled, so refs to deleted blocks survive.
- Commands over CreatedBlocks: `core.block.duplicate(block=...)` (outputs `block`), and
  `appendUndo(ctx, bytes, options)` through `runInstance`.

## Step A6: snapshot undo
- `SnapshotRegistry` (a Runtime extension): `Owned<SnapshotProvider>` by name. Interface values
  aren't stored.
- `SnapshotUndo(name)`: `begin(ctx)` at the start of exec captures before; **`end(ctx)` at the end
  of exec captures after**, sub-commands included. `undo` restores before, `redo` restores after.
  - An interactive command's tool calls `end` before `commit`. Without it, undo works and redo is
    FailedPrecondition.
  - `rollback(ctx)` restores before when exec fails partway. A failed capture fails exec.
  - Both blobs exist when the history reads `memSize`, so the budget counts them.
  - Folding: the head takes the folded command's after blob.
- Blobs are lz4 (plain), raw when that is smaller.
- After a restore: `Library::noteRefsChanged`, `ChangeBus::bumpEpoch` and **`notifyAll()`** (P5's):
  an opaque blob can't name what it changed. If P5's step 1 isn't merged yet, A6 waits for it.
- A failed restore is the sticky error (undo) or the Status (redo).
- A provider whose blobs hold block ids says which (`SnapshotProvider` optional slot
  `foreachRef(blob, RefVisitor)`); SnapshotUndo History-pins those targets until `discard`, as
  SetProperty does.

---

# P2b

## Step B1: pointer text, names and `meta`
- **`api/ref_text.h`:** `formatRef(rt, ref)`, `parseRef(rt, text)`:
  `@block(<client>, <32 hex>).key["x"][#3] : <struct>`, `@owner(<name>).key : <struct>`; the
  struct is optional; empty text is null. Names are quoted unless they are dotted identifiers.
- **Invocations:** `formatLiteral(rt, def, value)` writes a Pointer as quoted ref text.
  `inputsFromInvocation(Context &, ...)` accepts, for a Pointer input, ref text or a datapath,
  resolved now (with the Context's prefix) and made canonical. LastUsed still doesn't keep them.
- **TS:** a pointer reads as a frozen `Ref {text}` or null, and both can be written; a `Value` is
  then never a bound object. Arrays of strings write flags (`[]` is none). `toJs` takes the
  context. `get(ctx, path, {names: true})` gives keys. `set(ctx, path, value, gesture?)`: a new
  facade `setValueGesture`, so a slider drag folds into one undo step (§12).
- **`meta(path)`:** JSON from `pathMeta(ctx, path)`: kind (PropKind, Struct, Component, EnumKey,
  Length), struct, members, subtype, texts, access, readOnly, ui, ranges, step, precision, unit,
  items, maxLength, target, default, canonical. Infinities are omitted; integers past 2^53 are
  strings. The writer is `base/json_writer.h`.
- **Testbus:** `{$ref: text}` both ways, null as null; `get(path, {names})`; `meta`; `resolve`
  returns the kind. The host gains a Pointer member, a block type with a payload, and an int64.

## Step B2: layout nodes, XML, JSON, builder
```cpp
enum class LayoutKind : uint8_t { Column, Row, Panel, Tabs, Tab, Prop, Tool, ToolOptions, Label,
                                  Separator, Custom };
struct LayoutNode { LayoutKind kind; int32_t parent, firstChild, nextSibling;
                    uint32_t attrBegin, attrCount, offset; };
struct LayoutAttr { LayoutAttrId id; string name; string value; };   // name: Custom only
class Layout { Vector<LayoutNode> nodes; Vector<LayoutAttr> attrs; ... };   // preorder
```
- Attributes are stored sparsely: a node with a field per attribute is 1.2 KB.
- Attributes: `path`, `label`, `icon`, `description`, `id`, `class`, `visibleIf`, `enabledIf`,
  `invocation` (tool), `target` (toolOptions), `widget` (prop; a free-form hint), `closed`,
  `expand`, `iconsOnly`, `showLabel`.
- **Builder:** scopes are indices. `finish()` checks what the parser checks.
- **XML subset:** elements, attributes, comments, the five entities, text only inside `label`,
  `tool` and `custom`. Optional BOM and `<?xml?>`. Rejected by name: DOCTYPE, processing
  instructions, CDATA, numeric references, namespaces, duplicate attributes, control characters,
  invalid UTF-8, stray text. Root `<layout>`; `sep` is `separator`. Unknown tags and attributes
  are errors with suggestions, except under `custom`. Errors have offset, line and column. No
  recursion.
- **Limits:** depth 64, 16,384 nodes, 64 attributes per element, 1 MiB of text.
- **path.ux:** `tabs`, `tab`, `panel`, `row`, `column`, `prop path`, `label` text and a container's
  `path` mean the same. Its adapter handles `tool` (`path` and text there), `toolPanel`, the
  missing `visibleIf` and `enabledIf`, and prefixes joined as strings.
- **Paths:** prefixes join as parsed segments; a leading `/` means from the root. `effectivePath`.
  A `tool` runs with the container's prefix as the Context's path prefix (§5), for both its
  Pointer arguments and the command.
- `toJson`: non-default fields, effective paths, children. No JSON parser.

## Step B3: static resolution and the validator
- **`api/static_resolve.h`** walks a Path over a *set* of possible structs:
  - a pointer's target or candidates, and their registered subtypes; a collection's `itemStruct`
    and subtypes, or `CollectionAccessor::staticItem(key)` when it has one (LastUsedStore does, so
    `commandDefaults["id"].input` is checked, and its struct is registered on demand);
  - accepted if any has the member, an error with suggestions if none; a pointer with no static
    type makes the rest unknown (a warning);
  - an enum key a dynamic hook adds is an error: the resolver uses the static def too.
- `validateLayout(rt, root, layout) -> Vector<LayoutIssue {node, offset, severity, status}>`:
  - `prop`: a Value or Pointer member, Component, EnumKey or Length;
  - `visibleIf`, `enabledIf`: `[!]path` to Bools only; a container's `path`: a struct;
  - `tool`: the command, its argument names and literals; Pointer arguments statically, under the
    prefix; `toolOptions`: the command (tool ids come with P4).
- Validate after commands and types are registered: later ones aren't known.

## Step B4: bindings and TA2
- `layoutJson(xml)`, `validateLayoutXml(ctx, xml)`, `invokeAt(ctx, prefix, invocation)`.
  **Deviation from §13:** LayoutNode isn't bound; frontends get JSON.
- **`PathReader`** (owned by the script): `add(path)`, `read(ctx)`, `count()`, `value(i)`,
  `status(i)`, and `watch(ctx)`, which watches the paths through the ChangeBus and marks what
  fired; `changed(i)`, and `flushChanges(ctx)` runs the bus. This is §13's batched `getValues`
  plus watchers. The session disposes its readers before its host, since watchers hold the
  Context; `value(i)` is owned, and the TS layer copies and disposes it.
- **Panel** (`apps/testapp/src/panel/`): DOM from layout JSON; widgets for Bool, Int, Float,
  String, Enum, Flags, vectors, Pointer (text) and tools. It refreshes what `flushChanges` marks.
- **Layout file:** `apps/testapp/layouts/testapp.xml`. Testbus: `panel({xml} | {file})`,
  `panelState()`, `panelSet(id, value)`, `panelClick(id)`; the last two dispatch DOM events on the
  widget. `info().build` is `ta2`.
- **Library IO for TS hosts** (review of library IO, 18): minimal facades for save, load, append and
  resolve, with their reports.

## Step B5: `gen-paths`
- `apiCatalog(ctx)` (JSON: every ApiStruct's members through `meta`'s writer, and the commands
  with their inputs), and a generator next to `gen.ts`: one TS interface per struct, and a command
  catalog. No flat union of paths. `tsc` checks the output in the spike. Under a day, since the
  JSON exists; if it isn't, it moves to P6 and the task list says so.

---

## Files (each under 35 KB)
- `base/`: `bytes`, `name_table`, `json_writer`. `props/value_io`.
- `lib/`: `pin`, `lifetime.cc` (pins, doomed), `block_io`, `attachment`, `placeholder.cc`,
  `bookkeeping.cc`, `duplicate.cc`, `io/section`, `io/save.cc`, `io/load.cc`, `io/append.cc`.
- `api/`: `library_io`, `ref_text`, `static_resolve`, `attachments`.
- `command/`: `snapshot_undo`, `attachment_commands.cc`, `library_commands.cc`.
- `layout/`: `layout`, `builder`, `xml`, `json.cc`, `validate`.
- `bind/`: `meta`, `path_reader`, `layout_facades.cc`, `ts/gen_paths.ts`.

## Tests (beyond A1's)
- **Section:** golden bytes for a library with two slots, two attachments, an alias client and
  every ref kind; **a deep comparison of the loaded Library with the saved one** (a field dropped
  by both save and load must fail), then equal bytes; libraries built in different orders give
  equal bytes; every byte flip and truncation, and 20,000 mutations with the checksum fixed up,
  each within a time and memory bound; bombs; colliding ids; 1M `MissingBlock` rows under the
  budget; newer versions.
- **Every writer output loads:** random libraries with ugly names and unknown ids.
- **Load:** phase A and B failures change nothing; phase C's cases; placeholders without the type,
  then with it; alias duplicates; a mutation from `deserialize` is Busy; no name leaks into the
  Runtime.
- **Save:** orphans; History pins; InHistory, dropped and Detached targets in every table; Transient
  and Unspecified types.
- **Bookkeeping:** delete and undo of placeholders and Detached blocks; adoption; SetProperty over a
  placeholder; CreatedBlocks across an alias conflict and a revive.
- **Append and duplicate:** twice from one file; cycles; refusal; saving right after; undo and redo.
- **Snapshots:** with sub-commands (start 4, exec 109, undo 4, redo 109), a failing exec, folding,
  interactive, budget accounting, a purge between.
- **Layout:** goldens, fuzz, limits, builder equals parser, validator cases, prefixes.
- **TA2:** each widget kind, visibility, a tool under a prefix, a drag as one undo step, a pointer
  round trip, `meta`, names, a broken layout, a value changed from C++ reaching the panel through
  the ChangeBus. The shipped layout has no issues.
- `compile_embed` gets every new header.

## Steps
| # | Step | Needs |
|---|---|---|
| A1 | Lifetime; bytes and values; payloads, slots, attachment registry. **Merged to master.** | — |
| A2 | Bookkeeping: states, placeholders, adoption, rename, seeded index, pointer cells | A1 |
| A3 | CreatedBlocks, attach and detach commands, attachment paths | A2 |
| A4 | Library section: save, load, placeholders | A3 |
| A5 | Append and duplicate | A4 |
| A6 | Snapshot undo. *Feature review: library IO.* | A3; P5's `notifyAll` |
| B1 | Pointer text, names, `meta` | A1 |
| B2 | Layout nodes, XML, JSON, builder | — |
| B3 | Static resolution, validator | B2 |
| B4 | Bindings and TA2 | B1, B3 |
| B5 | `gen-paths`. *Feature review: layout.* | B4 |

Each step is committed with the full gate green. P2a and P2b merge to master separately.

## Exit criteria
- The gate: native tests in four presets, wasm-st-debug with `spike_ts` and `testapp`. wasm-mt
  builds and passes its native suite.
- A library with registered and unregistered types, slots, attachments and every ref kind survives
  save and load (deep comparison), and its second save has equal bytes.
- The section and layout fuzz tests run clean, natively and under node, within their bounds.
- TA2 passes with the panel built from a layout file.
- `docs/embedding.md` lists lz4, the limits, and what a host does about Transient blocks.

## Claims to check while implementing
Those the pressure test couldn't, or that the revision added:
1. Nothing outside `lib/` depends on `destroyFromHistory` destroying a Host-pinned block at once.
2. A Doomed block never reappears through relations, saves, `deletePreview` or `validate`.
3. The writer's output always loads, for any library P1's API can build. *Broke (review, 4):* null and
   repeated attachments. Fixed in round 2: the API refuses them.
4. Loading is linear, within the budget, on any input. *Broke (review, 5):* a pointer's key escaped the
   budget, and ids with equal low halves made saves quadratic (15). Fixed in rounds 1 and 2.
5. `markDeleted` on Placeholder and Detached breaks no P1 rule (`destroyBlocks`' severing,
   the audit, weak-target notices).
6. Adoption keeps every pin, slot and attachment, and undo of the adopting command is sound.
   *Broke (review, 3):* undo lost the record. Fixed in round 2 with un-adopt.
7. An unread entry and its refs survive any number of load and save rounds.
8. wasm: an allocation beyond the memory limit fails the probe, and doesn't corrupt memory.
9. DOM events work in a hidden NW.js window (TA0 already runs hidden).
10. litestl's bindings carry `const void *` slot parameters, `BlockId` by value, and `PathReader`.

## Risks
- **A1 freezes API for P3 and P5.** Changes after its merge go to both.
- **A1 is three commits of core code** before P3 and P5 can serialize. They need it only for their
  last steps, except P5's lifetime tests (its step 5) and P3's overrides (its step 9).
- **A format fixed too early:** version 1 may change until P6; the golden test makes it deliberate.
- **TS `Value` change** may surface in the spike and TA1; both are in the gate.

---

## Pressure test record (2026-11-24)
A fresh agent had the plan and the code. It ran experiments linked against coredcc's libraries
(native-clang RelWithDebInfo) and an lz4 test in 32 and 64 bits. The author checked each finding
against the code. Its "B7" is named in the verdict but isn't in the report.

### Survived
- The lz4 bound (claim 2): 400 inputs, worst ratio 251.6. lz4hc gives the same bytes in 32 and 64
  bits (claim 18, compression).
- litestl's `ByteReader` and `decompressBlock` are bounds-safe, by reading (claim 1).
- Severing a destroyed placeholder is harmless (9). No path starts with `@` (17). `BlockId` by value
  follows `QueryAnswer` (14). `io` builds everywhere and a host has it (3, partly). Within P1, `attach`
  and `setSlot` have only test callers (13). Nothing contradicts a `Ref` without WASM memory (16).
- `lib` depending on `props`; folding; the section's structure and parse phase.

### Broke, and what changed
**Blocking**
- **B1, nulling InHistory targets in the writer loses refs in duplicate and undo.** Nulling moved to
  the save, over every table. *The suggested writer purpose was rejected:* payloads hold only
  indices, so the save can null rows afterwards, and providers needn't know why they are written.
- **B2, a name read from a file makes the next save fail.** `NameTable` per Library, and raw ids as a
  last resort. *Registering them in the Runtime was rejected,* because of S10.
- **B3, saves could write files that don't load.** The reader accepts any bytes without NUL, of any
  length; the writer stops at a NUL. Tested as a property.
- **B4, placeholders and Detached blocks couldn't be deleted or undone.** A2's states.
- **B5, the lazy after blob is wrong with sub-commands** (checked in `CommandRecord::undo` and
  `redo`). `end()` captures it in exec.
- **B6, HostOwned lost GC edges and couldn't be restored.** HostOwned is cut; Detached records keep
  their refs through a save, and are adopted. *`onHostRestored(id, data)` was rejected* as part of
  the cut; adoption gives hosts what it would.

**Serious**
- **S1, deleted blocks came back as MissingBlock.** OptStrong rows to an id without a record are
  nulled at save, in every table.
- **S2, MissingBlock blocked the host from recreating a block.** Adoption. The append fill-in is cut.
- **S3, alias duplicates failed phase C.** Checked in phase B; the second gets a fresh id. *Failing
  the load was rejected:* registering an alias would make a document unopenable. Records of another
  client than their type's become placeholders; null Strong slots are kept.
- **S4, snapshot blobs don't keep targets alive.** With BlockSnapshotUndo cut, only opaque blobs
  remain: the optional `foreachRef` slot and History pins.
- **S5, after blobs escaped the budget.** Fixed by `end()`.
- **S6, SetProperty undo lost links to placeholders.** `PointerAccessor::cell`.
- **S7, P5's `notify` wasn't called.** Load, append, placeholder filling and rename notify per
  block; opaque snapshots call `notifyAll()`. P5 hooks `notify` and `notifyAll`, so `bumpEpoch`
  needs no listener.
- **S8, incomplete limits.** One budget by in-memory cost, `maxBlocks` over placeholders, capped
  issues, probed allocations, 128 MiB on 32-bit.
- **S9, colliding ids are quadratic.** A seeded index, sorting instead of searching, time bounds in
  the tests.
- **S10, registered file names aren't harmless.** Nothing is registered.
- **S11, the ownership table was wrong.** Rewritten; PinKind and the attachment registry are in A1.
- **S12, P5's needs from bags.** `BlockEntry`, unread entries, a kind per Pointer, the audit as a set
  comparison without deduplication. `duplicate` stays without an IdMap, and P5's revision accepts
  that. With `replaceData` cut, nothing restores a GraphDef's old counters.
- **S13, `replaceData`'s ownership.** Cut.
- **S14, CreatedBlocks by id alone.** Both handle and id; unpin by handle.
- **S15, step order.** New order: bookkeeping, undo, attachments, section, append.
- **S16, static resolution of `commandDefaults`.** `staticItem`.
- **S17, Transient by default.** Unspecified types fail the save; Transient is explicit and reported.

**Minor**
- M1 slots and attachments sorted; a deep comparison test. M2 MissingBlock records from `foreachRef`.
  M3 placeholder users in the cascade, and `core.block.unlinkRef`. M4 the checksum covers
  everything; chunk rules stated. M5 an unknown dynamic enum key is an error. M6 own reader. M7
  stated as a convention, checked by the audit. M8 sparse attributes. M9 skipped data is an issue.
  M10 `rollback`, and a failed capture fails exec. M11 kept as an unread entry. M12 the SYSTEM loop.
  M13 opaque attachments are copied, Root becomes FakeUser.
- **Undeclared deviations:** LayoutNode as JSON is now declared; the panel uses the ChangeBus;
  `setValueGesture` added; prefixes apply to tool arguments.
- **Size:** BlockSnapshotUndo, replaceData, HostOwned, the append fill-in and the closed widget list
  are cut. The phase is split. `gen-paths` is B5.

### From P5's pressure test (its S4 to S6) and its revised plan
- Payload lifetime: `LibraryRef`, `LibraryPin`, `dataAlive()`. Doomed blocks leave the id index and
  are rechecked when destroyed. Deferred destroy errors stay off the sticky error.
  `stateChanges()`, `busy()` and the epoch bumps are in A1.
- **Differs from P5's text:** if a Strong user appears and the id is taken by another record, the
  doomed block is destroyed rather than kept until `collect()`: the user's ref names the other
  record.
- **P5's item 8** (refusing to alias HostOwned types): there is no HostOwned. P5 can refuse
  Transient types, and must check `dataAlive()` in either case.

### Couldn't be verified
- wasm out-of-memory behaviour; DOM events in a hidden window; `compile_embed` with the new headers;
  an actual FNV-1a collision. The first three are in "Claims to check while implementing".

## Implementation notes

### A1, commit 1: lifetime (2026-11-24)
- The published API is unchanged. Where the plan left a choice:
  - **`data(h)` is null for a Doomed block,** as for every block that isn't Live. Its data is
    reached through `get(h)->clientData()` or the pointer the pin's holder already has.
  - **`size()` doesn't count Doomed blocks.** It matches `forEach` and the index.
  - **A cached handle to a Doomed block isn't trusted** by `resolveRef`: the record keeps its
    old id, which may name another block.
  - **Unpinning a block that has no pin of that kind aborts,** as the single count did.
  - The recheck at destruction counts only Strong refs (RefKind::Strong), the same check
    `destroyFromHistory` makes.
  - A sticky error that is pending when a deferred destroy runs is put back afterwards.

### A1, commit 2: bytes and values (2026-11-24)
- Added to the published API:
  - `BytesWriter(size_t maxBytes = kMaxBytes)`, and `beginSection()` / `endSection(token)`, which
    write what the reader gets as one `blob()`. Entries use them for their length.
  - `BytesReader::ok()` and `fail(StatusCode, const char *)`. A failure moves the reader to its
    end, so `atEnd()` alone never means success.
  - `NameTable::debugAddAs`, a test hook for collisions, like the Runtime's.
  - `ValueTag`, `valueTag`, `isKnownValueTag`, `writeValueBody`, `readValueBody` and
    `storedBagValue`: payload entries write the same tags and bodies.
- A name whose hash isn't its id (a collision forced by a test hook) is written as the raw id,
  so it reads back.
- A `NameTable` counts 64 bytes for each entry on top of its text, so millions of short names are
  bounded too.
- `readBag` keeps at most 1000 issues, and an entry whose body is damaged fails the read instead of
  being an issue.
- A Pointer with a negative handle in its key fails the writer (InvalidArgument). Nothing in the
  API makes one.

### A1, commit 3: payloads, slots, attachments (2026-11-24)
- **An entry's layout:** name, tag, a ref count and the row indices, then the body with its length.
  A block pointer's body holds an index into the entry's own refs (always 0 today), not a row, so
  an unknown entry's `raw` stays right when its rows move.
- `BlockWriter::entry(e)` is `entries()` of one, so `BlockReader::entries` reads it back.
- **A Pointer into a block without a ref in `refs` fails the writer** (Internal), as does `raw` under
  a tag this build knows: the reader would reject or misread either.
- `BlockWriter::bag` writes the unread entries first. If the bag has since set the same name, the set
  value wins on read. A bag's pointers are copies: the provider keeps a RefCell for each, and
  reports it.
- **Added to the published API:**
  - `checkPayloadRefs(lib, h, payload)`: the audit on its own. `writePayload` runs it when the
    library's relations audit is on (`Library::auditsRelations()`), and A4's save runs it
    otherwise, to report an issue.
  - `Library::foreachDataRef`: the provider's references without slots and attachments, which is
    what the table is compared with.
  - Public constructors, `BlockWriter::take(version)`, `BlockReader::data()` and
    `BlockCopy::data()`: A4 and A5 use them for attachments and copies, and tests do now.
  - `has...` queries on the new optional slots, as the existing ones have.
- **`readPayload`** needs a Live record without data, opens a no-collect scope around the provider,
  and takes back data from a provider that returned ok although the reader failed, bytes were left
  over, or (with data) something else was wrong: it severs the data's refs and calls `destroy`
  under the sweep's rules. Only a provider that returns an error cleans up after itself.
  `setData(nullptr)` is the same as not calling it.
- `writePayload` accepts Live and InHistory blocks, and refuses one with a sticky error pending, as
  `collect` does.
- The audit compares strong against weak: Strong and OptStrong rows count the same.
- `const void *` parameters bind as `void *` does, so the attachment slots keep `const`.
- `BlockTypeRegistry::add` also refuses `Payload` without the slots.
- Attachment types and block types share TypeId names. Nothing stops one name being both.

### A2: library bookkeeping (2026-11-24)
- **The attachment and block type name overlap** (listed after A1) is refused: each registry's `add`
  rejects a name the other has (AlreadyExists). No API change. Slots belong to either kind by
  TypeId, so a shared name would have mixed them.
- `Datablock::dataState()` is the state a deleted block remembers (else its state).
  `LibraryPin::dataAlive` and `writePayload` use it: a block deleted while Detached or a
  placeholder has no data.
- `addPlaceholder(id, PlaceholderInfo)` carries the reason, type, names and a `BlockPayload` (which
  moved to `lib/block.h`). The four-argument form stays, for a MissingType without refs.
- `Library::relinkRef(h, index, target)` is the undo of `forceUnlinkRef`, which is `relinkRef` to
  null.
- **The id index uses SipHash-1-3** seeded from `newLocalId()`, so constructing a Library takes one
  id from the GUID source. The relations map, the weak-notice map and the payload audit use keyed
  hashes too.
- `HistoryBlock` (`command/history_block.h`) is A3's "handle and id", moved forward for the two new
  commands. It finds by id alone: the index never holds a Doomed record, so a handle that still
  names a record of the id is what the index gives.
- **`PointerAccessor::cell`:** undo and redo go through `setValue` when the target was and is Live,
  and write the cell directly (with the same notification and update hook) otherwise, which also
  covers a placeholder that was filled in between.
- `api/struct.h` includes `lib/block.h` for `RefCell`.

### A3: blocks by id, attach and detach (2026-11-24)
- CreatedBlocks and DeleteBlock hold `HistoryBlock`s. Dropping either from the history destroys
  only blocks that are InHistory then: a block that was recreated under its id, or restored behind
  the command's back, is left alone.
- DeleteBlock's `canRun` uses the block commands' shared input check.
- `attachUndo` takes the data: on an error the provider has destroyed it, unless the type isn't
  registered. The command behind it, `core.block.attachData`, is Private.
- A command that holds detached data History-pins what the data's `foreachRef` reports. A
  `foreachRef` that throws fails the detach and puts the data back.
- The `attachments` collection lists attachments whose type has a `dataStruct`; its item struct id
  is `coredcc.Attachment`, which has no struct, so items are only reached through their own type's.
- Attach and detach post a change of the owner's `attachments` member.

### A4: save and load (2026-11-24)
- **Files:** `lib/io/library_io.h` (saving, limits, reports), `lib/io/{save,parse,plan_ids,load,fill,
  pack}.cc`; `api/library_io.h` has `loadLibrary` and `resolvePlaceholders`, which notify.
- **A load adds every record as a placeholder, then fills it in place**, which is exactly what
  `resolvePlaceholders` does later. So a half-loaded library has no Live block without data:
  `resolveRef` gives null for a block that isn't read yet, and `usersOf` uses the tables.
- **Format changes from the plan:**
  - Two kinds of storage, `Payload` and `NoPayload`. A `MissingType` kind would have made a build
    without a type rewrite the file; now it writes the same bytes.
  - A `NAME` entry is text or an id (the A1 rule for names nobody knows). A block's own name is
    inline; `NAME` holds client, type, namespace and source names.
  - Indices are stored plus 1, with 0 for none. Each blob is used at most once, and in this version
    each must be used, so a blob can't be copied many times.
- **Costs:** `kRecordCost` is `sizeof(Datablock)` (about 930) plus 640; rows, slots, attachments and
  names have their own costs in `format.h`, which include Vector growth. A 1,048,576-block file fits
  in 2 GiB, barely. `BlockReader::setBudget` bounds what entries allocate (208 bytes for 13 stored):
  past it the block stays a placeholder.
- **Name collisions are refused (InvalidArgument)**, and they are a real attack: FNV-1a 64 collisions
  between readable names take seconds to make (the test has a pair, found with lattice reduction).
  Without the check a file could pass its blocks off as another type's.
- **A missing-block placeholder that was never a record** is written if it is reachable and has a
  name, flags, slots or attachments.
- **An attachment type without `serialize`** is counted in `SaveReport::skippedAttachments`.
- A name table that is full doesn't fail the load: those names are written as ids later, with an
  issue.
- `Library::attach` is AlreadyExists over an opaque attachment of the type. Nothing removes one.
  Its references can be cut (A5).
- `Library::busy()` is also true in a load; the Busy message says "a sweep or a load".
- A deleted (InHistory) placeholder isn't resolved; after its restore, the next
  `resolvePlaceholders` does.
- `BytesWriter::raw`, `BytesReader::raw` and `Library::fileNames() const` are new.

### A5: append and duplicate (2026-11-24)
- **Files:** `lib/id_map.h`, `lib/duplicate.{h,cc}`, `command/library_commands.{h,cc}`; append is in
  `lib/io/plan_ids.cc` and `load.cc`, and `api/library_io.h` has `appendLibrary`, which notifies.
- **The opaque-attachment gap is closed:** `Library::refTable`, `forceUnlinkRef` and `relinkRef` take
  an attachment's TypeId, and `core.block.unlinkRef` has an `attachment` input (a type name).
- **Undoing an append leaves the MissingBlock records it made** (History-pinned until the discard).
  The host may have filled one with `createWithId` by then, and undo mustn't take that block.
- **A stored id and its canonical id are both in the IdMap.** When two records become one through an
  alias, the canonical id maps to the one stored under the current name.
- **`rekey` sees a temporary Datablock** in the Placeholder state, with the imported id, type and name,
  and the library busy. An id that another record of the append got counts as in use.
- **`LoadLimits::maxBlocks` bounds what an append adds**, not the library's size.
- **Duplicating a placeholder** copies a MissingType one's payload. InHistory and Detached blocks and
  missing blocks are FailedPrecondition: they have no data to copy.
- FakeUser is copied. A duplicate runs no notification: the block is new, so nobody listens to it.

### A6: snapshot undo (2026-11-24)
- **Files:** `command/snapshot_undo.{h,cc}`; `SnapshotProvider::foreachRef` is in `undo_strategies.h`.
- **`rollback` restores twice:** at once, and at the discard, which the executor does after it has
  undone the sub-commands. A sub-command's undo may write the captured state's old values.
- **A failed `end` in exec needs a `rollback` too**; SnapshotUndo doesn't do it, because an interactive
  command's tool may go on after a failed `end`.
- **A failed redo can leave a value from the middle of the step**, when a sub-command writes the
  captured state itself: the executor undoes the sub-commands after the failure, and a nested
  command isn't discarded until its parents are undone. The history drops the step.
- `notifyAll` bumps the epoch, so `bumpEpoch` isn't called. A failed restore notifies as well.
- **Pins on targets are for the blocks that have a record at the capture**, once per blob. A Context
  without a Library pins nothing.
- Blobs under 512 bytes are raw, and so are those that lz4 doesn't make smaller. A blob over lz4's
  limit (about 2 GiB) is OutOfRange.

### B1: pointer text form and meta (2026-11-24)
- **Files:** `api/ref_text.{h,cc}`, `bind/meta.{h,cc}`, `base/json_writer.{h,cc}` and
  `base/utf8.{h,cc}` (B2's parser uses it too), and `testapp/items.{h,cc}`.
- **An id without a registered name** is written `#` and 16 hex digits, for clients, owners and
  structs, so every ref has a text form. A member that isn't an identifier is quoted.
- **`parseRef` makes a client alias canonical**, and doesn't check that the target exists.
- **A pointer input accepts a target whose struct inherits the input's.** The value's type becomes the
  input's struct, as the kind check needs. A ref text need not resolve; a datapath must.
- **A datapath to a Pointer member means its target** as a pointer input and as a Context prefix.
- **An array of flag names reaches C++ as `"A|B"`**, so the facade resolves the names with the same
  code as text. An empty array is Flags 0.
- **`pathMeta` has more than `PropertyDef`:** `target` (pointers, collections, lengths), `of` (the kind
  of the member that an enum key or a component belongs to), `component`, `key`, and for a struct its
  `members` without Private ones, its own first.
- **`toJs` and `fromJs` take an optional context**: a graph's nodes have none, and their values are
  never pointers (TypeError).
- **Block ids alone have text too** (`formatBlockId(s)`, `parseBlockId(s)`): `client,hex32`, lists
  separated by `;`. P3's `blockIdText` becomes this, and commands on many blocks take it as a String.
- **The wasm test host's pointer member is `activeItem`**: `active` is reserved for collections.
- **Mutations:** 85 in `json_writer`, `utf8`, `ref_text`, `literal`, `resolve`, `meta` and `facades`.
  All are caught but one, which is equivalent: `unsignedInteger` one past 2^53 reaches `integer`,
  which quotes it as well.

### B2: layout nodes, XML, JSON, builder (2026-11-25)
- **Files:** `layout/{layout,builder,xml,draft}.{h,cc}` and `json.cc`. `draft` is what the parser and the
  builder fill in, and the checks they share.
- **The root is a node of its own kind** (`Layout`). It may have a `path`, `id` and `class`.
- **Syntax is checked when a layout is made:** paths, conditions, invocations, bools, unique ids, and
  which children and attributes each kind may have. So a `Layout` is always well formed, and joining
  paths can't fail. Whether names exist is B3's.
- **A fifth limit, 1,024 bytes for a path joined with its prefixes.** Otherwise 1 MiB of text makes
  gigabytes of effective paths.
- **A node's own paths, its conditions too, are relative to its parent's prefix,** never to its own
  `path`.
- **`custom` has a required `tag`.** Unknown elements inside one are Customs whose tag is their name, and
  elements of known names keep their meaning. Attributes that aren't a Custom's own are free ones.
- **Text:** in `label` and `tool` it is the label, with blanks collapsed; a label attribute and text are
  an error. In `custom` it is kept but for the space around it, and can't be mixed with elements.
- **JSON has joined paths** for conditions (`{path, not}`) and `prefix` for tools, so a frontend never
  joins paths itself.
- **The builder's bools are set with `setFlag`:** an overload of `set` took string literals.
- **Mutations:** 142, of which 139 fail a test. The rest change only speed or memory (a break in a loop
  that the next line makes redundant, one node more in the builder), or were a duplicated check that is
  removed.

### B3: static resolution and the validator (2026-11-25)
- **Files:** `api/static_resolve.{h,cc}`, `layout/validate.{h,cc}`. `coredcc_layout` now depends on
  `coredcc_command`.
- **Members take one segment or two** (a collection's item is the second), so the walk keeps the
  possible structs per place in the path, not one set.
- **"No static type" doesn't exist:** `ApiRegistry::add` requires a target, candidates or an item struct.
  What is unknown, and a warning, is a struct that isn't registered. A root that isn't is an error.
- **A member found through several structs of one parent counts once.** If none has it, the error names
  the structs and suggests from all of them.
- **A prefix that is wrong is reported once,** and relative paths in it aren't. So are unknown prefixes.
- **A Custom's path may name anything;** if it isn't a struct, relative paths in it aren't checked.
- **At most 1,000 issues.**
- **Shared with the resolver and the executor:** `detail::atSegment`, `detail::unknownInput` and
  `coercePointer`, so the messages are the same. A test resolves what a document has both ways.
- **Found by the hostile tests and fixed:** `parseInvocation` compared each argument's name with all
  before it (87 s for 100,000 arguments, in Debug); the walk added the structs of one target once per
  member that points to it; suggestions were made for errors that were dropped; and the validator went
  on after its last issue. The largest layouts now take at most 4 s in Debug.
- **Mutations:** 91, of which 86 fail a test. Four change only speed (the memo of subtypes, the late
  suggestions, and stopping after the last issue, in two places), and one doesn't compile.
- **Docs:** `docs/embedding.md`, "Layouts".

### B4: bindings and TA2 (2026-11-25)
- **A run keeps its prefix.** A tool's prefix applies to the command's own paths as well as to its
  arguments, so `CommandRecord` holds it, and undo, redo and reruns set it again in every slot.
  Two runs under different prefixes don't fold. `invokeAt` refuses a prefix that names no struct.
- **PathReader's calls** are free functions: `readerAdd/Count/Read/Value/Watch/Unwatch/TakeChanged`.
  `readerTakeChanged` replaces `changed(i)`: it gives the indices that fired since it was last asked.
- **ChangeBus:** watchers that are gone are dropped from the list once they are half of it, so
  that many of them go in linear time; a callback is destroyed as its connection ends.
- **LastUsedStore** bumps the bus's epoch when a value changes. Until then a watcher of
  `commandDefaults[...]` missed what a run remembered.
- **Library IO for scripts:** `librarySave`, `libraryLoad`, `libraryAppend`, `libraryResolve` and
  `clearHistory`. Bytes cross in chunks (`ScriptBytes`), reports as JSON with lists of
  `{total, first}` that hold at most 10,000 entries. `libraryResolve` refuses while the history
  has anything to undo or redo, hence `clearHistory`.
- **TS files:** `core.ts` (shared), `layout.ts`, `reader.ts` and `library.ts`, next to `index.ts`.
- **Panel:** `panelDrag(id, values)` was added for gestures, and `panelSet` takes a component or a
  flag's key. The panel's timer flushes every 50 ms. A prop's widget is made from `meta` when its
  path first has a value; a pointer that then leads to a struct whose member has another kind or
  range keeps the widget until the path has had no value.
- The test app's root has `commandDefaults`.

### B5: gen-paths (2026-11-25)
- `apiCatalog(ctx)` (`bind/catalog.h`); `bind/ts/catalog.ts` has its types, and
  `bind/ts/gen_paths.ts` the generator, `generatePaths(catalog)`, which returns the text and
  imports nothing from the module, so a build script can run it on a saved catalog.
- An interface has the inherited members too, since an own one may hide an inherited one of
  another type, which `extends` would refuse. A struct that others inherit from also has a union
  `Any<Name>` of them, which is what references to it give.
- Private members, inputs, outputs and commands are left out of the catalog. Structs are private by
  no flag, so `CommandDefaults` and its items are in it when they are registered.
- The spike's `tsc` checks the test app's paths and those of a hostile catalog
  (`tests/wasm/paths.types.ts`).

### Review of library IO, round 1 (2026-11-24)
Numbers are the report's (`docs/reports/feature-report-library-io-2026-11-24.md`, with round 2).
- **1:** `BagRefs` (`lib/bag_refs.h`) is a bag's cells by name. The cell says which block, the bag's
  value says the rest. `BlockWriter::bag` and `BlockReader::bag` take it, and an entry with more ref
  rows than its value needs is kept as unread.
- **2 and 9:** `Datablock::library()`, so every slot that gets a block knows the document. Attachment
  slots get their block, except `destroy`: the history holds detached data for longer than the
  block and the Library exist. `rekey` gets a `RekeyContext` (`library`, `isFree`).
  `onWeakTargetLost` runs for data that is there (Live, or InHistory from Live), for the data's own
  weak references only, and attachments have it as well.
- **9:** `SnapshotProvider` slots take the Context, and `capture` is a Status with the blob as an
  out-parameter: a `Result` can't be a bound return type. Filling a MissingBlock through an append's
  `rekey` waits for round 2's un-adopt.
- **5:** a pointer's key has at most 1,024 segments, written and read, and entries are charged for
  them. The cap alone doesn't bound it: many short keys take as much.
- **23:** codes this build doesn't know are Unsupported in a newer file, and damage in one of ours.
- **24:** each record ends with an extension: fields of a tag and bytes, kept and written back by
  builds that don't know them. A tag with the top bit is critical: the block stays a placeholder.
- **25:** `values` (no count) and `array` (with one) for fixed-width numbers, `i32` and `f32`.
- **31:** `take`, `setBudget` and the data getters are the library's (`lib/payload_access.h`).
  `AttachmentTypeInfo::version` is `payloadVersion`, as a block type's.
- **The golden file changed** (4 more bytes per record), and its names are shorter so that BLCK stays
  below the compression threshold.
- **Mutations:** 60 in these changes, all caught (8 of them only after the tests were extended).

### Review of library IO, round 2 (2026-11-24)
- **3 and 9, un-adopt:** `Library::unadopt` and `readopt` (`lib/adopt.cc`). The record keeps its handle,
  id and pins; the data moves to a second record in the Doomed state, which already means "held only
  by Host pins, reached only by its handle". What the record was is a stack, because an append's
  record without data can fill a missing block that the host then fills. Claim 6 holds now.
  - Host pins on the record hold the data through a pin of their own (`Adoption::lent`).
    `LibraryPin::dataAlive` is false while it is taken back.
  - `onHostDestroyed` and `onHostRestored` reach the data while it is taken back.
  - Slots go back as a whole. Attachments that came with the data go with it.
- **6:** the references of a Transient block that gets a record are in its ref table, so what they lead
  to survives a load until the host fills it.
- **7:** a save marks up to three times, the extra ones only when there are Host pins or Transient
  blocks that are left out.
- **10:** `Scope` takes a const Library and casts it: a save takes one, and what its providers may do is
  deferred. Scopes nest. `lib/payload_access.h` has the forms that run inside one.
- **13:** `BlockStorage` serves attachment types too. `SaveReport::skippedAttachments` is gone.
- **16:** refused, not redirected to the command's own Library: the Context's root and owners are the
  other document's. The executor then drops the step, as for any failed undo.
- **17:** taken: the same value, a number of the other type that converts back, or another type
  altogether (an enum's key). Kept as unread: the rest.
- **26:** `loadBytes` is exact for a load of this build, which a test checks to the byte.
- **27:** `LinearHistory` measures a step again after its undo and redo (`executor.cc`). Sizes are what
  providers say: without `memSize`, only the record counts.
- **28 and the batch delete:** `core.block.delete` and `core.block.duplicate` have `block` and `blocks`;
  either may be empty. C++ has `deletePreview(lib, span)` and `destroyFromHistory(span)`.
- **30:** a `rekey` that fails in a load is an issue, and the block gets a random id.
- **Mutations:** 133, of which 127 fail a test. The other 6:
  - two were redundant code, now removed (`destroyBlocks` releasing lent pins, which a block without
    Host pins doesn't have, and `CreatedBlocks::discard` dropping a pin early);
  - two can't be seen: a stale handle left in `Adoption::lent`, and `canRun` of a detach, whose `exec`
    refuses the same;
  - the bound before converting a float to an int (undefined without it, and x86 gives the same
    answer);
  - a linear scan in `deletePreview`, which is too cheap for the time test's bound.

### Review of layouts, round 1 (2026-11-25)
The findings and what was done are in the
[report](../reports/feature-report-layout-2026-11-25.md). What isn't there:

- **1:** `CommandRecord::prefix()` is a `const StableRef *`, and a Context's prefix is a Path or an
  address. A struct that its owner's accessors reach by key has its key in its address, as
  `SetProperty`'s has.
- **2:** the order in which a bag's values are set is `PropertyBag::setOrder`: those that aren't
  dynamic first. Dynamic inputs may not depend on each other.
- **4:** a watcher compares the effective def whole (`PropertyDef::sameAs`), so a getter isn't called
  again for it. `flush` counts a watcher once, also if both of its functions ran.
- **12:** the validator's options path goes through the root's static structs, so a root struct that
  isn't registered gives no issue.
- **16:** `LinearHistory::head()` is now also there after `popHead`, but nothing folds into it. That is
  what lets a gesture of several members be cancelled.
- **18:** what a widget writes is made inside the panel's `act`, by a function. A tool's label needs the
  catalog, which the panel reads once, and only if a tool has no label.
- **24:** `Path::fromRoot`. `resolve`, `get`, `set` and watchers accept `/`, and `/` alone is the root.
  Canonical refs don't have it.
- **Edits in others' areas:** `api/context.h` (a prefix that is an address, `anyPrefix`),
  `api/change_bus` (the second function, the watcher's prefix, watchers of dynamic members),
  `command/executor` (`start`, `updateInteractive`, `revertHead`, `head()`).
- **Mutations:** 207 (119 native, 88 of the panel), of which 197 fail a test. Of the others, 7 were redundant
  code, now removed (three fields of a watcher's meta, a gate of `cancelGesture`, a default of
  `commandDefaults`, and two uses of a field's copy of its text), and 3 cannot be seen: the kind and the kind of meta in
  `sameAs` each imply the other, and `.` and `/` are as long as each other in the limit of a joined path.

### Review of layouts, round 2 (2026-11-25)
- **Findings** and their resolutions:
  [report](../reports/feature-report-layout-2026-11-25.md), 6 to 11, 13 to 15, 19 to 23, 25 to 27.
- **New files:** `bind/run_facades.*`, `bind/ts/run.ts`, `bind/ts/typed.ts`. The TS suites are
  `spike.test.ts` and `ui.test.ts`, over `fixture.ts`.
- **In P5's areas:** ChangeBus's pending changes and `watch` (a `start` to read into), and the
  executor's `canRun` and `interactiveDef`.
- **`canRun` refuses with an uncollected sticky error,** and leaves it: the next command reports it, as
  it would have.
- **A reader's values are JSON,** not a packed buffer: 1,000 take about 6 ms in a wasm debug build, and
  JSON needs no decoder of its own.
- **`typed` takes literal paths.** One that isn't a literal (a layout's) is as with the plain calls.
- **A Tool has its own `path`,** as a Prop does, rather than an attribute that clears the prefix: "/"
  does that.

### Deferred by the review of layouts
- **A registry of layouts by name,** `include` and `slot`: when there are add-ons that bring layouts.
- **List templates** (`list`) and **menus** (`menu`): with the first frontend that needs them. The
  names are reserved, and refused by this version.
- **Localization:** texts are as written. A host can translate `label` and `description` by node id.
- **Units per host:** `displayUnit` is of a def. A preference (metric, imperial) is the host's, which
  passes another unit to `formatNumber`.

### Deferred by the review of library IO
Not scheduled; whoever needs one adds it to a plan.
- **19:** choosing which blocks of a file to append, and listing its contents without loading it. The
  options go into `AppendOptions`.
- **20:** `LoadOptions::ignoreChecksum`: read on past a wrong checksum, with the damaged records as
  issues. The format allows it: records are length-prefixed, and blobs have their own sizes.
- **21:** aliases for renamed block types, attachment types, structs, runtime owners and slot
  namespaces. `detail::typeNamed` is where the types' go.
