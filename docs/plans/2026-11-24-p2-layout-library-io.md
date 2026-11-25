# P2: layout and library IO

Status: draft, not yet pressure-tested (2026-11-24). Architecture: §8a ("Files, linking
and import", "Attachments"), §12, §14 P2. Test app stage TA2
(`docs/plans/2026-11-19-test-app.md`). Branch `p2-layout`.

## Ownership between the parallel phases
The same table is in the P2, P3 and P5 plans.

| Phase | Owns |
|---|---|
| P2 | Value bytes (bounded, versioned reader and writer for `PropValue` and `PropertyBag`); `BlockProvider`'s optional `serialize`, `deserialize`, `duplicate` and `rekey`; the attachment type registry and the attach and detach commands; `Library::rename`; snapshot undo and lz4; CreatedBlocks with BlockIds; `layout/`; TA2. The shared pieces (value bytes, provider slots) are its first step, merged to master early so P3 and P5 can rebase onto them. |
| P3 | `scene/`, `bvh/`, `draw/View` if it needs it for screen distance, `EvalCtx`, `CollectionAccessor::setActive`, the Context scene slot, display units and frames. Its providers' `serialize` is its last step. |
| P5 | `graph/`, the Runtime's global version counter, payloads. GraphDef serialization is its last step. |
| Shared files | `source/coredcc/CMakeLists.txt`, `tests/CMakeLists.txt`, the task list, `docs/debugging.md`, `docs/embedding.md`: small appended changes only. Task list: each phase edits its own row and focus lines. No phase edits `CLAUDE.md`. |

## Scope

### In scope, with where it was deferred from
| Item | From |
|---|---|
| Layout nodes, builder, XML subset parser, `toJson`, validator, example layouts | architecture §12, §14 |
| Library section: save, load, placeholders | architecture §8a, §14 |
| Append with rekeying; duplicate | architecture §8a |
| `serialize`, `deserialize`, `duplicate`, `rekey` slots | P1c notes |
| Attachment type registry, serializer, duplicate policy, opaque attachments; attach and detach commands | P1c and P1e notes |
| `Library::rename`; enumerating slots, attachments and overrides | lib and api review D8 |
| `forceUnlink` for references held inside a placeholder's payload | architecture §8a (P1 only unlinks slots) |
| Snapshot undo (before and after blobs), lz4 | command review design 8 |
| `CreatedBlocks` with BlockIds | command review design 9 |
| A text form for Pointer values (invocation arguments, testbus) | props review D5; TA1 review S2 |
| `meta(path)`, enum and flags names, `resolve` returning a kind | TA1 review S3, S5 |
| TA2: the property panel built from a layout file | test-app plan |

### Out of scope
- **A scene file, or any file system access.** The library section is bytes in and bytes
  out; the host puts them in its file.
- **Link** (blocks that stay in another file), and unregistering types at run time.
- **Block providers and attachment providers written in TS.** `BlockProvider::data`
  returns `void *`, which `foreignImplementable` refuses. Native fakes only.
- **`gen-paths`** (typed paths for TS). It belongs to no phase yet; the review of P2
  should say whether it goes to P6.
- **Frontends beyond the test app's panel:** path.ux, Slate and ImGui adapters.
- **Mat4 writes, registry removal, enum inputs and ranges in script def builders:** still
  deferred (P1f notes).
- **A list or collection widget in layouts.** A `prop` on a Collection is an error.
- **Migrating payloads:** a provider reads its own older versions.

## Module placement
`base` ← `props`, `parse` ← `lib` ← `api` ← `command` ← `layout` ← `bind`

- `base/bytes.h` (new): the byte reader and writer.
- `props/value_io.h` (new): value bytes.
- `lib` gains a dependency on `props` (allowed by the P1 layering, not used until now)
  and on litestl's `io` target (lz4).
- `layout/` (new module) depends on `command`, because the validator checks invocations.

## Step 1: shared pieces (merged to master before the rest)

### What litestl gives us
- `litestl/io/bytes.h`'s `ByteReader` is safe on hostile input: every read checks the
  remaining size, an over-read is sticky and gives zeros, and `str()` compares the length
  as 64 bits before narrowing (so it holds on wasm32). 200,000 random buffers ran clean in
  a scratch test (clang-cl, no sanitizer).
- What it doesn't do, and callers must:
  - bound counts before allocating;
  - narrow a `u64` safely before calling `view(size_t)` (wasm32);
  - report where and why it failed;
  - `ByteWriter::put` casts sizes to `int`, so it overflows at 2 GB.
- `litestl/io/compress.h` (namespace `sculptcore::io`) uses `LZ4_decompress_safe` and
  checks the exact size. `rawSize` is allocated up front, so the caller must bound it. The
  best possible lz4 ratio is below 255:1 (measured 254.9 on zeros), which gives the bound.
  200,000 corrupted blocks decoded without a crash, and 16,406 of them decoded
  "successfully" to wrong bytes or an unintended length, so lz4 is not an integrity check:
  the section needs checksums.
- The `io` target builds in native-clang-debug, native-msvc-debug and wasm-st-debug today,
  with exceptions off.

### `base/bytes.h`
- `BytesWriter`: `u8 u16 u32 u64 i64 f64 boolean str bytes`, over a litestl
  `Vector<uint8_t>`. A write that would pass `kMaxBytes` (1 GiB; 512 MiB when pointers are
  32 bits) sets a sticky OutOfRange and writes nothing more.
- `BytesReader`: the same reads, over a borrowed span. Wraps litestl's `ByteReader`.
  - The first failure is kept as a Status with the byte offset; later reads give zeros.
  - `count(minItemBytes)` reads a u32 and fails when
    `n * minItemBytes > remaining()`, in 64-bit arithmetic. It is the only way to read a
    count.
  - `str(maxLen)`: rejects a NUL (litestl strings end at NUL, and `lsString` aborts on
    one), and invalid UTF-8.
  - `fail(msg)`, `status()`, `remaining()`, `atEnd()`.
- These, not litestl's, appear in coredcc headers, so `compile_embed` covers them and the
  litestl include (`"util/vector.h"`, unprefixed) stays in a `.cc`.

### `props/value_io.h`
```cpp
/** How block ids inside Pointer values are stored. */
class BlockIdCodec {            // coredcc-internal polymorphism: an ordinary virtual class
  virtual Status write(BytesWriter &out, const BlockId &id) = 0;
  virtual Status read(BytesReader &in, BlockId &id) = 0;
};
/** Ids inline as client name + 128 bits: for preferences, clipboards and undo blobs. */
class InlineIdCodec final : public BlockIdCodec;

Status writeValue(const Runtime &rt, BytesWriter &out, const PropValue &v, BlockIdCodec &ids);
Result<PropValue> readValue(BytesReader &in, BlockIdCodec &ids);

struct BagIssue { string name; Status why; };   // unknown name, or a value that won't coerce
Status writeBag(const Runtime &rt, BytesWriter &out, const PropertyBag &bag, BlockIdCodec &ids);
/** Into a bag whose defs come from code. Set entries only; issues don't fail the read. */
Status readBag(BytesReader &in, PropertyBag &bag, BlockIdCodec &ids, Vector<BagIssue> &issues);
```
- **Value:** u8 wire tag, then the payload. Wire tags are their own constants, not
  `PropValue::Type`'s numbers, with a static mapping both ways, so reordering the enum
  can't change files.
  - Vec, Quat and Mat4: their doubles. NaN and -0.0 round-trip bitwise.
  - Pointer: owner kind, then the block id through the codec, or the runtime owner's
    *name*; key segments (at most 256, strings at most 64 KiB, handles non-negative);
    the struct's *name* (empty for null). Names come from `Runtime::nameOf`. An id with no
    registered name fails the write with InvalidArgument: files store names, never hashes.
- **Bag:** u32 count; per entry: name, u32 byte length, value. The length lets an unknown
  wire tag be skipped, so an older build can read a newer bag.
  - Enum and Flags entries are written as their keys (`"EDIT"`, `"A|B"`), because `coerce`
    already accepts keys and item numbers may change. A value with no matching item is
    written as a number.
  - Entries that aren't `wasSet` are not written.
- **No format version of its own.** Value bytes live inside versioned containers (a
  payload version, the section's version).

### `lib/block_io.h`
```cpp
/** What a provider writes a block's payload with. Bound opaque, so slots can take it. */
class BlockWriter {
  BytesWriter &bytes();
  void ref(const RefCell &cell);                         // index into the block's ref table
  void value(const PropValue &v, RefKind pointerKind);   // Pointer ids go through the table
  void bag(const PropertyBag &bag, RefKind pointerKind);
  void fail(std::string_view why);
};
class BlockReader {
  uint32_t version() const;                              // the payload version it was written with
  BytesReader &bytes();
  void ref(RefCell &cell);                               // sets target; keeps the cell's own kind
  Result<PropValue> value();
  Status bag(PropertyBag &bag);                          // issues go into the load report
  void setData(void *data);                              // becomes the block's clientData
  void fail(std::string_view why);
};
class BlockCopy { void setData(void *data); };
```
- **The payload rule** (§8a) becomes a mechanism: a payload never holds a `BlockId`. It
  holds indices into the block's **ref table**, `(RefKind, BlockId)` rows the library
  stores beside the payload. GC, remapping, append and placeholders work on the table
  without understanding the payload.
- `BlockWriter::ref` writes a null row for a null target and for an InHistory target: its
  undo doesn't survive a save, and a saved optional ref to it would otherwise load as a
  placeholder.

### `BlockProvider` slots (all optional)
```cpp
Status serialize(const Datablock &block, BlockWriter &out) const;
Status deserialize(const Datablock &block, BlockReader &in);
Status duplicate(const Datablock &src, BlockCopy &out);
BlockId rekey(const Datablock &imported) const;    // null: refuse the append
```
- `deserialize` makes new data and calls `in.setData`. On an error it has freed what it
  made. It never sees a partly filled library: no other block's data may be read from it
  (blocks load in id order). Refs are set as ids and resolve later.
- **Deviation from §8a:** `duplicate` is per block, without a handle list or an `IdMap`.
  Handles are per Library and providers per Runtime (P1c note). The provider copies data,
  refs verbatim; the library then rewrites the copy's refs through `foreachRef`.
- **Deviation from §8a:** `rekey` gets the imported record, not a payload and a map, since
  payloads hold no ids. It returns the new id, in the same client.
- `BlockTypeRegistry::add` rejects a provider with only one of `serialize` and
  `deserialize`. It also gains `BlockTypeInfo::storage`:
  - `Payload` (default when the slots exist);
  - `HostOwned`: no payload; the record loads as Detached until `onHostRestored`;
  - `Transient` (default when they don't): not saved; a saved strong ref to one is listed
    in the save report.

### Rules for P3 and P5
- Serialize through `BlockWriter`, refs through `ref()`, parameters through `bag()`.
- **A Pointer in a bag must also have a RefCell that `foreachRef` reports.** A `PropValue`
  holds a `StableRef`, which `RefVisitor` can't visit. P5's node parameters keep one
  `RefCell` per Pointer parameter, as the truth, and build the `PropValue` from it.

## Step 2: library bookkeeping
- `Library::rename(h, name)`: InvalidArgument for a NUL or over `kMaxNameLength`
  (64 KiB); bumps the epoch so watchers re-read. Names stay non-unique.
  - `core.block.rename(block=... name="...")`, undoable, by BlockId.
- `forEachSlot`, `forEachAttachment`, `forEachOverride` (const, record order).
- `setSlot` requires `ns` to be a registered type name (InvalidArgument otherwise).
  P1 accepts any hash, which a file can't store. The two existing callers are tests: one
  uses a registered type, and one (a placeholder's slot) must register its name.
- **Pin kinds:** `pin(h, PinKind = Host)`, with `History` for SetProperty and
  CreatedBlocks. GC treats both as roots. A save treats only Host pins as roots, or an
  unlinked block would be saved because the undo stack remembers it.
- **Placeholder refs:** `Datablock` keeps a placeholder's ref table, and `foreachRef`
  visits it, so `usersOf`, GC and remap see through placeholders.
  - `Library::forceUnlinkRef(h, index)` nulls one row of it (the indices the payload holds
    stay valid). `forceUnlink` on slots stays as it is.
- **Placeholders have two reasons:** `MissingType` (has a payload) and `MissingBlock` (a
  strong target no file held; no payload, no type).
  - A `MissingBlock` placeholder stops being a GC root: it is collected when nothing
    references it. `MissingType` stays a root.
  - `Datablock::sourceName()` records where it came from.
- `Library::resolvePlaceholders()`: after types are registered, deserializes the
  `MissingType` placeholders whose type now exists (an add-on enabled late).

## Step 3: the library section

### API
```cpp
struct SaveOptions { bool compress = true; };
struct SaveReport  { int blocks; Vector<IoIssue> issues; };
Result<Vector<uint8_t>> saveLibrary(const Library &lib, const SaveOptions &, SaveReport *);

struct LoadLimits  { uint64_t maxRawBytes = 1 GiB (512 MiB on 32-bit); uint32_t maxBlocks = 1 << 22; };
struct LoadOptions { std::string_view sourceName; LoadLimits limits; };
struct LoadReport  { Vector<BlockHandle> loaded; int placeholders; Vector<IoIssue> issues; };
Result<LoadReport> loadLibrary(Library &lib, std::span<const uint8_t> bytes, const LoadOptions &);
struct IoIssue { BlockId block; string blockName; Status why; };
```
- `saveLibrary` marks from the save roots and writes what it reaches. It doesn't collect,
  so a save never frees anything. Roots: Root, FakeUser, Host pins, Detached blocks and
  `MissingType` placeholders. InHistory blocks aren't written.
- Blocks are written sorted by (client name, local id), so equal documents give equal
  bytes whatever the pool order.
- `loadLibrary` needs an empty library (no records), else FailedPrecondition. Append is
  step 4.

### Format, version 1
Little-endian. All counts and lengths are checked as in step 1.
```
header:  "CDCCLIB\n" | u32 version | u32 minReader | u32 chunkCount
chunk:   u32 tag | u32 flags (1 critical, 2 lz4) | u64 stored | u64 raw | u64 fnv1a64(stored) | bytes
NAME  (critical) strings: every name used below, by index
BLCK  (critical) count, then records, each u32-length-prefixed:
      client name, 128-bit local id, type name, block name, u32 flags (Root, FakeUser),
      u8 storage (Payload, HostOwned, MissingType, MissingBlock), u8 detached,
      u32 payloadVersion, source name,
      ref table: count * (u8 kind, client name, 128 bits; null allowed)
      slots: count * (ns name, i64 key, ref row)
      attachments: count * (type name, u32 version, u32 bytes index, ref table)
      payload index, or none
DATA  (critical) payload and attachment blobs: count, then (u8 lz4, u64 raw, u64 stored, bytes)
```
- A blob of at least 512 bytes is compressed when that makes it smaller. `BLCK` and `NAME`
  are compressed as chunks. Saves use lz4hc's default level, undo uses plain lz4.
- **Bombs:** a blob's or chunk's `raw` must be at most `stored * 255 + 64` and the running
  total at most `maxRawBytes`, both checked before allocating.
- **Versions:**
  - `version` above ours with `minReader` at most ours: read; unknown non-critical chunks
    and trailing record bytes are skipped (and not preserved).
  - `minReader` above ours, or an unknown critical chunk: Unsupported, naming both.
  - A payload newer than the registered `payloadVersion` loads as a `MissingType`
    placeholder (it round-trips) with an issue.

### Failure behaviour
- **Phase A, parse:** the whole section is parsed and checked into a plain structure. The
  Library and the Runtime are untouched. Errors: bad magic, a checksum mismatch, a short
  read, a limit, a duplicate or null block id, an unknown ref kind, an index out of range,
  a duplicate slot or attachment key. All are InvalidArgument with the byte offset, except
  limits (OutOfRange) and versions (Unsupported).
- **Phase B, register names:** client, type and slot names are registered. A hash
  collision is AlreadyExists, and nothing has been added to the library. (Names stay
  registered; they are harmless.)
- **Phase C, fill:** records are added, then refs, slots and attachments, then each
  provider's `deserialize`. It can't fail as a whole:
  - an unregistered type, or a `deserialize` that fails (or leaves a sticky error, which
    is taken): the block becomes a `MissingType` placeholder with its payload and table,
    and the report has an issue. Nothing is lost on the next save;
  - an unregistered attachment type becomes opaque bytes;
  - a strong or optional-strong target with no record becomes a `MissingBlock`
    placeholder. A weak one creates nothing.
- **During phase C** the library is Busy to mutation (the sweep's rule), and reads work.
- A provider whose `serialize` fails: the whole save fails with its Status and the block's
  name. Saving the rest would lose data silently.
- **Audit:** after each `serialize`, the rows it wrote are compared with what `foreachRef`
  reports. A difference means GC and the file disagree. With the relations audit on (debug
  builds) it is an Internal error, else an issue.
- Detached blocks of a Payload type have no data to write: the record and slots are
  written, and they load as Detached.

## Step 4: append and duplicate
- `appendLibrary(lib, bytes, options) -> {IdMap, LoadReport}`:
  - ids are planned before anything changes: `coredcc` client blocks always get fresh ids;
    other clients' blocks go through `rekey`. No `rekey` slot, a null answer, an id
    already in use, or an id in another client refuses the whole append
    (FailedPrecondition listing the types).
  - Placeholders of unregistered clients can't be rekeyed: refused the same way.
  - Refs inside the appended set follow the `IdMap`. Refs out of the set resolve in the
    target library, or become `MissingBlock` placeholders.
  - A `MissingBlock` record with a planned id is filled in place, keeping its handle.
- `duplicateBlocks(lib, handles) -> IdMap`: the `duplicate` slot, else
  serialize-then-deserialize through the table. Refs inside the set are remapped, others
  are shared. Slots are copied and remapped; attachments follow their policy; flags other
  than Root are copied; pins and overrides aren't. All or nothing.
- Commands: `core.block.duplicate(block=...)` (outputs `block`), and
  `appendUndo(ctx, bytes, options)` (through `runInstance`, as `setValueUndo` is), both
  over CreatedBlocks.

## Step 5: attachments
- `AttachmentProvider` (§2a interface): `destroy(void *)`; optional `create() -> void *`,
  `copy`, `serialize`, `deserialize`, `foreachRef(void *, RefVisitor)`.
- `AttachmentTypeRegistry` (a Runtime extension):
  `{name, version, StructId, DuplicatePolicy Copy|Drop}` plus the provider. Copy needs
  `copy`; `serialize` and `deserialize` come together.
- `Library::attach(h, type, data)` requires a registered type, and replaces P1's
  `destroy` function pointer (its callers are three lines of one test).
  - Attachment refs are visited by `foreachRef`, after the provider's and the slots'.
  - Opaque attachments keep the type name, version, bytes and ref table. They are written
    back unchanged.
- Commands: `core.block.attach(block=... type="...")` (needs `create`),
  `core.block.detach(...)`, and `attachUndo(ctx, h, type, data)` for a native payload. The
  command owns detached data until it leaves the history.
- `addAttachments(ApiStruct &)` (`api/`): an `attachments` collection keyed by type name,
  for block structs, so `objects["A"].attachments["my.data"].x` works.

## Step 6: undo
- `CreatedBlocks` stores BlockIds and finds handles when it needs them. DeleteBlock does
  the same. `add(lib, h)` is unchanged.
- `SnapshotRegistry` (a Runtime extension): `Owned<SnapshotProvider>` by name, because
  interface values must not be stored.
- `SnapshotUndo(rt, name)`: `begin()` captures the before blob (in exec). The **after**
  blob is captured by the first `undo`, when the state is exactly what the command left,
  so a command that is never undone pays for one blob. `undo` restores before, `redo`
  restores after.
  - Blobs are kept lz4-compressed (plain lz4), raw when that is smaller; `memSize` is what
    they hold.
  - After a restore it calls `Library::noteRefsChanged` and `ChangeBus::bumpEpoch`.
  - A failed capture in exec fails exec. A failed restore is the sticky error (undo) or the
    Status (redo), which makes LinearHistory drop the command, as for any failed undo.
- `BlockSnapshotUndo`: the same over named blocks' payloads, using the provider's
  serialize and deserialize. It gives any Payload-type client undo without a journal.
  - `Library::replaceData(h, bytes)` deserializes new data and swaps it in. The record,
    handle, pins and slots are unchanged.
  - Old data is destroyed at once, or on the last Host unpin if the block is host-pinned
    (a graph payload may alias it).
  - Unsupported when `data(block) != clientData` or the block has overrides (they hold
    `self` pointers).
  - It doesn't cover blocks made or deleted by the command: combine it with CreatedBlocks.

## Step 7: pointer text and `meta`
- **`api/ref_text.h`:** `formatRef(rt, StableRef)` and `parseRef(rt, text)`:
  ```
  @block(<client name>, <32 hex digits>).key["x"][#3] : <struct name>
  @owner(<runtime owner name>).key : <struct name>
  ```
  The `: struct` part is optional. Empty text is the null pointer. Names are quoted with
  the path quoting rules when they aren't dotted identifiers.
- **Literals:** `formatLiteral(rt, def, value)` writes a Pointer as a quoted ref text.
  `inputsFromInvocation(Context &, ...)` accepts, for a Pointer input, a quoted string that
  is either ref text (it starts with `@`) or a datapath, resolved now and made canonical.
  A path to a Pointer member gives its target; a path to a struct gives the struct.
  - The `Runtime &` overload stays, and refuses paths (Unsupported). LastUsed still
    doesn't keep Pointers.
- **Facades:** `makePointer(ctx, text)`, `readPointer(ctx, v)`, `pathMeta(ctx, path)`
  (a JSON string), and `PathReader` (below).
- **TS layer:** a pointer reads as a `Ref` (`{text}`, frozen, no WASM memory) or `null`,
  and both can be written. A `Value` is then never an owned bound object.
  `set(ctx, 'flags', ['SELECT', 'HIDE'])` joins the keys; `[]` is no flags. `toJs` takes
  the context.
  - `get(ctx, path, {names: true})` gives an enum's key, and a flags member's keys as an
    array. A value with no item stays a number.
- **`meta(path)`:** `kind` (a PropKind, or Struct, Component, EnumKey, Length), `struct`,
  `members` (non-Private names, for a Struct), `subtype`, `uiName`, `description`, `icon`,
  `access`, `readOnly`, `ui`, ranges, step, precision, unit, `items`, `maxLength`,
  `target`, `default`, `canonical` (ref text).
  - JSON numbers: infinities are omitted, 64-bit integers past 2^53 are strings. One small
    writer, `base/json_writer.h`, shared with the layout's `toJson`.
- **Testbus:** a pointer crosses as `{$ref: text}` both ways, null as `null`;
  `get(path, {names})`; `meta(path)`; `resolve(path)` returns the kind. The host gains a
  Pointer member, a block type with a payload, and an int64 member (TA1 test gaps).

## Step 8: `layout/`

### Nodes
```cpp
enum class LayoutKind : uint8_t { Column, Row, Panel, Tabs, Tab, Prop, Tool, ToolOptions,
                                  Label, Separator, Custom };
struct LayoutNode {
  LayoutKind kind; int parent, firstChild, nextSibling;   // indices into Layout
  string path, label, icon, description, id, styleClass, visibleIf, enabledIf;
  string invocation;       // Tool
  string target;           // ToolOptions: a command id
  string widget;           // Prop: a hint from a closed list ("slider", "color", ...)
  bool closed, expand, iconsOnly, showLabel;
  string tag; Vector<Attr> attrs;   // Custom
  uint32_t offset;                  // in the source text; 0 for built nodes
};
class Layout { Vector<LayoutNode> nodes; ... };   // preorder; node 0 is the root Column
```
- A flat array with indices: litestl's `Vector` has inline storage, so a node can't hold a
  `Vector` of itself, and an array binds and copies simply.
- **Builder:** `LayoutBuilder b; auto p = b.panel("Brush").path("brush"); p.prop("radius")...`
  Scopes are handles (indices), not pointers. `std::move(b).finish() -> Result<Layout>`
  checks what the parser checks.
- **Limits:** depth 64, 65,536 nodes, 1 MiB of text, 64 attributes per element.

### XML subset
- Elements, attributes (either quote), comments, `&lt; &gt; &amp; &quot; &apos;`, and text
  only inside `label`, `tool` and `custom`. UTF-8, with an optional BOM.
- Rejected with a message that names the feature: DOCTYPE, processing instructions (the
  `<?xml ...?>` declaration is accepted at the start), CDATA, numeric references,
  namespaces, duplicate attributes, control characters, stray text.
- The root is `<layout>`. `sep` is accepted for `separator`.
- Unknown tags and attributes are errors with suggestions, except under `custom`
  (`<custom tag="my-widget" ...>`, any attributes, no child elements).
- Errors carry the byte offset, line and column. No recursion: an explicit stack.
- **Compatibility with path.ux xmlpage:** the tags `tabs`, `tab`, `panel`, `row`,
  `column`, `prop path`, `label` text and a container's `path` prefix mean the same.
  Differences, for its adapter: xmlpage's `tool` takes `path` and text, ours takes
  `invocation`, `label` or text; `toolPanel` is `toolOptions`; `visibleIf` and `enabledIf`
  don't exist there; xmlpage prefixes by string, ours by path segments.

### Paths
- A container's `path` prefixes its children's. Prefixes join as parsed segments.
- A leading `/` means "from the root", ignoring prefixes. It is the layout's syntax and is
  removed before `parsePath`.
- `effectivePath(node)` is what frontends bind to.
- `visibleIf` and `enabledIf`: `[!]path`, with the same prefix rules.

### `toJson`
`{kind, path (effective), label, ..., children: [...]}`, only non-default fields. It is for
frontends: there is no JSON parser.

### Validation
`validateLayout(Runtime &, StructId root, const Layout &) -> Vector<LayoutIssue {node,
offset, Severity, Status}>`.
- **Static paths** (`api/static_resolve.h`): walks a Path over ApiStructs with a *set* of
  possible structs, no instances:
  - a pointer's static target or candidates, and their registered subtypes (the resolver
    accepts subtypes, and takes an untyped target's type from its owner);
  - a collection's `itemStruct` and its subtypes;
  - a path is accepted if any struct in the set has the member. It is an error, with
    suggestions from all of them, only if none has;
  - a pointer with no static type ("any struct") makes the rest unknown: a warning;
  - an unknown enum key on a dynamic def is a warning, since the hook may add it.
- `prop`: a Value or Pointer member, a Component, an EnumKey or a Length. A Struct or a
  Collection is an error. A widget hint that doesn't fit the kind is a warning.
- `visibleIf` / `enabledIf`: every possible result is a Bool or an EnumKey.
- `tool`: the invocation parses, the command exists, its arguments exist and their literals
  parse. A Pointer argument's path is checked statically.
- `toolOptions`: the command exists. (Tool ids come with P4.)
- A container's `path` must be a struct.

## Step 9: bindings and TA2
- Facades: `layoutJson(xml)`, `validateLayoutXml(ctx, xml)` (JSON issues, against the
  context root's struct), and `PathReader`:
  ```cpp
  class PathReader {           // owned by the script: newPathReader()
    Status add(const string &path);      // parsed once
    void read(Context &ctx);             // all paths, one call
    int32_t count() const;  PropValue value(int32_t i) const;  Status status(int32_t i) const;
  };
  ```
  It is the bound form of `getValues`. An index out of range gives None and OutOfRange.
- **Panel** (`apps/testapp/src/panel/`): builds DOM from a layout's JSON. Widgets for
  Bool, Int, Float, String, Enum (select), Flags (checkboxes), vectors (one input per
  component), Pointer (its text) and `tool` (a button). It refreshes after every testbus
  method that can change the document, through one `PathReader`.
- **Layout file:** `apps/testapp/layouts/testapp.xml`, read at launch and by `panel(file)`.
- **Testbus:** `panel({xml} | {file})`, `panelState()` (per widget: node id, path, kind,
  label, visible, enabled, value, error), `panelSet(id, value)` and `panelClick(id)`, which
  go through the widget's own DOM event handler, so the test covers it.
- `build` in `info()` becomes `ta2`.

## Files (each under 35 KB)
- `base/bytes.{h,cc}`, `base/json_writer.{h,cc}`, `base/utf8.h`
- `props/value_io.{h,cc}`
- `lib/block_io.{h,cc}`, `lib/attachment.{h,cc}`, `lib/library_records.cc` (rename,
  enumeration, placeholders: `library.cc` is 18 KB and would otherwise grow past 30),
  `lib/io/section.{h,cc}` (parse and write, no Library), `lib/io/save.cc`,
  `lib/io/load.cc`, `lib/io/append.cc`, `lib/duplicate.cc`
- `api/ref_text.{h,cc}`, `api/static_resolve.{h,cc}`, `api/attachments.{h,cc}`
- `command/snapshot_undo.{h,cc}`, `command/attachment_commands.cc`,
  `command/library_commands.cc`
- `layout/layout.{h,cc}`, `layout/builder.{h,cc}`, `layout/xml.{h,cc}`, `layout/json.cc`,
  `layout/validate.{h,cc}`
- `bind/meta.{h,cc}`, `bind/path_reader.{h,cc}`, `bind/layout_facades.cc`

## Tests
- **Bytes and values:** every type round-trips; NaN, -0.0, int64 limits, 64-bit flags;
  truncation at every byte; counts near 2^32; NUL and invalid UTF-8; unknown tags skipped
  in bags; renamed, removed and retyped bag entries; enum keys surviving renumbering.
- **Section:** golden bytes for a small library (so the format can't drift unnoticed);
  save, load, save gives equal bytes; every byte flipped, every truncation, and 20,000
  random mutations (`COREDCC_TEST_SCALE`) load or fail cleanly, with the checksum fixed up
  so the mutation reaches the parser; bombs; duplicate ids; newer versions and chunks.
- **Load:** each failure in phases A and B leaves `size()`, the epoch and `validate()`
  unchanged; failing and sticky-error `deserialize`; unregistered client, type and
  attachment; newer payload; missing strong, optional and weak targets; placeholder round
  trip without the type, then with it, then `resolvePlaceholders`; client aliases;
  `forceUnlinkRef`; a mutation from inside `deserialize` is Busy.
- **Save:** orphans, history pins, InHistory targets, Transient and HostOwned types,
  Detached; the ref audit.
- **Append and duplicate:** twice from one file; cycles; `rekey` refusing; refs out of the
  set; filling a `MissingBlock` record; undo, redo and purge between them.
- **Attachments, undo, rename:** as specified, plus GC through attachment refs, replaced
  data under a host pin, memory budgets, and a failed restore.
- **Refs and meta:** format and parse round-trip, hostile text, Pointer arguments by path
  and by ref, undo after the target was deleted.
- **Layout:** parser goldens, a fuzz test like `test_parse_fuzz`, limits, builder equals
  parser, JSON escapes, the validator over subtypes, candidates, refine, collections,
  dynamic defs, prefixes and `/`.
- **TA2** (`ta2.test.ts`): build the panel from the file; read and set each widget kind;
  visibility; a tool button; undo and redo refresh it; a pointer round trip; `meta`; names;
  a broken layout is reported, not fatal. The shipped layout validates with no issues.
- **compile_embed** gets the new headers. **Foreign (native fake) provider** for the new
  slots; every forwarder called.

## Steps, each committed with the full gate green
1. Shared pieces: bytes, value IO, `BlockWriter`/`BlockReader`, slots. **Merge to master.**
2. Bookkeeping: rename, enumeration, pin kinds, placeholder refs.
3. Library section: format, save, load.
4. Append and duplicate.
5. Attachments.
6. Undo: CreatedBlocks, SnapshotUndo, BlockSnapshotUndo, the block commands.
7. Pointer text and `meta`, with the TS layer and testbus changes.
8. Layout: nodes, XML, JSON, builder, then static resolve and the validator.
9. Bindings and TA2.

After 6: a feature review of library IO (not in the task list's column today; it is a
major feature, so this plan proposes adding it). After 9: the `layout` review.

## Exit criteria
- The gate: native tests in all four presets, and wasm-st-debug with `spike_ts` and
  `testapp`. wasm-mt builds and passes its native suite.
- A library with blocks of a registered and an unregistered type, slots, attachments and
  every ref kind survives save, load, save byte for byte.
- The section and layout fuzz tests run clean, natively and under node.
- TA2 passes, with the panel built from a layout file.
- `docs/embedding.md` lists lz4 and the size limits.

## Claims to falsify
1. litestl's `ByteReader` never reads out of bounds or hangs, on 32 and 64 bits.
2. The lz4 bound `raw <= stored * 255 + 64` never rejects a block `compressBlock` made.
3. litestl's `io` target links into coredcc's tests in all presets and wasm, also when a
   host supplies litestl.
4. Every block id a payload needs can go through the ref table, so placeholders, remap and
   append never need to understand a payload.
5. The InHistory rule (null rows) and the pin-kind split make a saved file free of blocks
   and placeholders the user deleted.
6. Phases A and B leave the Library unchanged on every failure, and phase C can't fail as a
   whole.
7. `serialize` then `deserialize` through the table is a correct duplicate for any
   Payload type.
8. Making `MissingBlock` placeholders collectable, and adding placeholder and attachment
   refs to `foreachRef`, breaks no P1 rule (relations audit, `destroyBlocks`' severing,
   `deletePreview`, `destroyFromHistory`).
9. Severing the refs of a destroyed placeholder is harmless, because destroyed records are
   never saved.
10. Capturing the after blob at the first undo sees exactly the state the command left,
    with sub-commands, folding, `rerunHead` and interactive commands.
11. `replaceData` is safe with undo records, watchers and host pins that outlive the swap.
12. CreatedBlocks and DeleteBlock behave the same with BlockIds, including a client
    becoming an alias between exec and undo.
13. No existing caller breaks when `setSlot` requires a registered `ns` and `attach` a
    registered type.
14. The binding system carries the new slots (`BlockWriter &`, `BlockId` by value), and
    `PathReader`, `Status` and `PropValue` returns, in TS.
15. Static resolution accepts every path the dynamic resolver can accept, given that
    accessors return declared types (no false errors).
16. A `Ref` needs no WASM memory, so no TS caller has to dispose of values any more, and
    making `toJs` take a context breaks nothing but coredcc's own callers.
17. The ref text can't collide with a datapath in an invocation (no path starts with `@`).
18. Sorting by (client name, id) makes equal libraries give equal bytes (compression
    included).
19. The panel's DOM events work in a hidden NW.js window.

## Risks
- **Size.** Steps 2 to 6 touch `library.cc` and the undo code heavily. P3 and P5 only read
  them; conflicts should stay in CMake lists and `compile_embed.cc`.
- **The step 1 API is frozen early.** P3 and P5 write providers against it, so changes
  after the merge need both told.
- **A format fixed too early.** Nothing ships yet: version 1 may change until P6, and the
  golden test makes each change deliberate.
- **Memory:** load holds the parsed section and the new data together, about twice the raw
  size. `maxRawBytes` is the host's control.
- **TS layer change** (`Value` no longer contains PropValue) may surface in the spike and
  TA1 tests; both are in the gate.
</content>
</invoke>
<invoke name="Bash">
<parameter name="command">cd /c/dev/coredcc-p2-layout && wc -c docs/plans/2026-11-24-p2-layout-library-io.md source/coredcc/lib/library.cc && cat > /tmp/p2msg.txt <<'E'
plan: P2 layout and library IO

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01JrpoSSToc98mTvZFN5g64p
E
git add docs/plans/2026-11-24-p2-layout-library-io.md && git commit -q -F /tmp/p2msg.txt -- docs/plans/2026-11-24-p2-layout-library-io.md && git log --oneline -1 && git status --short