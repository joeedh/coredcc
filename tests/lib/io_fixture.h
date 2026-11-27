#pragma once

/* For the library section's tests: a world with block and attachment types that
 * serialize, a deep comparison of two libraries that doesn't use the writer, and files
 * built by hand. */

#include "doc_fixture.h"

#include "coredcc/lib/attachment.h"
#include "coredcc/lib/io/format.h"
#include "coredcc/lib/io/library_io.h"
#include "coredcc/lib/io/load.h"

#include <string>
#include <utility>
#include <vector>

namespace coredcc_test {

using Bytes = Vector<uint8_t>;

inline std::span<const uint8_t> view(const Bytes &b)
{
  return std::span<const uint8_t>(b.data(), size_t(b.size()));
}
inline bool sameBytes(const Bytes &a, const Bytes &b)
{
  return a.size() == b.size() &&
         (a.size() == 0 || memcmp(a.data(), b.data(), a.size()) == 0);
}
inline bool has(const std::string &text, const char *needle)
{
  return text.find(needle) != std::string::npos;
}

/* ---- attachments ---- */

struct Note {
  int value = 0;
  RefCell about{RefKind::Strong};
};

struct NoteHost {
  Runtime *rt = nullptr;
  int alive = 0;
  int severed = 0;
  DocFault fault = DocFault::None;
  bool failForeach = false;
  /** serialize: the reference isn't written. */
  bool skipRef = false;
  litestl::util::function<void()> onSerialize;
  litestl::util::function<void()> onCopy;
  ~NoteHost()
  {
    test_assert(alive == 0); /* each destroyed once */
  }
};

struct NoteProvider {
  NoteHost *host = nullptr;
  void destroy(void *data)
  {
    Note *n = static_cast<Note *>(data);
    host->alive--;
    host->severed += n->about.isNull() ? 1 : 0;
    litestl::alloc::Delete(n);
  }
  void *copy(const Datablock &, const void *data, const Datablock &)
  {
    if (host->onCopy) {
      host->onCopy();
    }
    if (host->fault == DocFault::NoData) {
      return nullptr;
    }
    if (host->fault == DocFault::Sticky) {
      host->rt->setStickyError(Status::error(StatusCode::Internal, "copy threw"));
      return nullptr;
    }
    host->alive++;
    return litestl::alloc::New<Note>("Note", *static_cast<const Note *>(data));
  }
  Status serialize(const Datablock &, const void *data, BlockWriter &out) const
  {
    if (host->onSerialize) {
      host->onSerialize();
    }
    if (host->fault == DocFault::Error) {
      return Status::error(StatusCode::Unsupported, "no notes today");
    }
    const Note *n = static_cast<const Note *>(data);
    out.bytes().u32(uint32_t(n->value));
    out.ref(host->skipRef ? RefCell(RefKind::Strong) : n->about);
    return Status();
  }
  Status deserialize(const Datablock &, BlockReader &in)
  {
    if (host->fault == DocFault::Error) {
      return Status::error(StatusCode::Unsupported, "no notes today");
    }
    if (host->fault == DocFault::Sticky) {
      host->rt->setStickyError(Status::error(StatusCode::Internal, "note threw"));
      return Status();
    }
    host->alive++;
    Note *n = litestl::alloc::New<Note>("Note");
    n->value = int(in.bytes().u32());
    in.setData(n);
    in.ref(n->about);
    if (host->fault == DocFault::LeaveBytes) {
      in.bytes().u8();
    }
    return Status();
  }
  void forEachRef(const Datablock &, const void *data, RefVisitor &v) const
  {
    if (host->failForeach) {
      host->rt->setStickyError(
          Status::error(StatusCode::Internal, "note forEachRef threw"));
      return;
    }
    v.visit(const_cast<Note *>(static_cast<const Note *>(data))->about);
  }
  static lbt::Struct<NoteProvider> *defineBindings()
  {
    auto *st = new lbt::Struct<NoteProvider>("test::NoteProvider", sizeof(NoteProvider));
    BIND_STRUCT_METHOD(st, destroy, MARGS("data"));
    BIND_STRUCT_METHOD(st, copy, MARGS("from", "data", "to"));
    BIND_STRUCT_METHOD(st, serialize, MARGS("block", "data", "out"));
    BIND_STRUCT_METHOD(st, deserialize, MARGS("block", "in"));
    BIND_STRUCT_METHOD(st, forEachRef, MARGS("block", "data", "visitor"));
    return st;
  }
};

/* An attachment type that isn't saved. */
struct BareProvider {
  void destroy(void *)
  {
  }
  static lbt::Struct<BareProvider> *defineBindings()
  {
    auto *st = new lbt::Struct<BareProvider>("test::BareProvider", sizeof(BareProvider));
    BIND_STRUCT_METHOD(st, destroy, MARGS("data"));
    return st;
  }
};

inline const TypeId kNote = TypeId::of("test.Note");
inline const TypeId kBare = TypeId::of("test.Bare");
inline const TypeId kTransient = TypeId::of("test.Transient");
inline const TypeId kPlain = TypeId::of("test.Plain");
inline const TypeId kAcmePlain = TypeId::of("acme.Plain");
inline const TypeId kAcmeDoc = TypeId::of("acme.Doc");
inline const ClientId kAcme = ClientId::of("acme");

inline void registerNote(Runtime &rt,
                         NoteHost &host,
                         uint32_t version = 2,
                         DuplicatePolicy onDuplicate = DuplicatePolicy::Drop,
                         const char *name = "test.Note")
{
  host.rt = &rt;
  const std::string impl = std::string(name) + "#provider";
  test_assert((registerImpl<AttachmentProvider, NoteProvider>(rt, impl).ok()));
  NoteProvider *p = litestl::alloc::New<NoteProvider>("provider");
  p->host = &host;
  AttachmentTypeInfo info;
  info.name = name;
  info.payloadVersion = version;
  info.onDuplicate = onDuplicate;
  Result<Owned<AttachmentProvider>> owned = own<AttachmentProvider>(rt, impl, p);
  test_assert(owned.ok() &&
              rt.ext<AttachmentTypeRegistry>().add(info, std::move(owned).value()).ok());
}

inline void registerBare(Runtime &rt,
                         const char *name = "test.Bare",
                         BlockStorage storage = BlockStorage::Transient)
{
  const std::string impl = std::string(name) + "#provider";
  test_assert((registerImpl<AttachmentProvider, BareProvider>(rt, impl).ok()));
  AttachmentTypeInfo info;
  info.name = name;
  info.storage = storage;
  Result<Owned<AttachmentProvider>> owned =
      own<AttachmentProvider>(rt, impl, litestl::alloc::New<BareProvider>("provider"));
  test_assert(owned.ok() &&
              rt.ext<AttachmentTypeRegistry>().add(info, std::move(owned).value()).ok());
}

/* A Thing whose type says it is never saved. Its id is the client's, as a host's would
 * be. */
inline void registerTransient(Runtime &rt, ThingHost &host)
{
  host.rt = &rt;
  const char *impl = "test.Transient#provider";
  test_assert((registerImpl<BlockProvider, ThingProvider>(rt, impl).ok()));
  ThingProvider *p = litestl::alloc::New<ThingProvider>("provider");
  p->host = &host;
  Result<Owned<BlockProvider>> owned = own<BlockProvider>(rt, impl, p);
  BlockTypeInfo info;
  info.name = "test.Transient";
  info.client = ClientId::of(kCoredccClient);
  info.storage = BlockStorage::Transient;
  test_assert(owned.ok() &&
              rt.ext<BlockTypeRegistry>().add(info, std::move(owned).value()).ok());
}

/* ---- worlds ---- */

/* What a world knows. The saved kinds are test.Doc, acme.Doc and the attachment
 * test.Note. */
struct Knows {
  bool doc = true;
  bool acme = true;
  bool note = true;
  uint32_t docVersion = 3;
  uint32_t noteVersion = 2;
  DuplicatePolicy copyNotes = DuplicatePolicy::Drop;
};

struct IoWorld {
  ThingHost things;
  DocHost docs;
  NoteHost notes;
  Runtime rt;
  Library lib{&rt};

  explicit IoWorld(Knows k = {})
  {
    registerThing(rt, things);
    registerTransient(rt, things);
    registerBare(rt);
    if (k.acme) {
      test_assert(rt.ext<ClientRegistry>().add("acme").ok());
      registerDoc(rt, docs, 1, "acme.Doc", kAcme);
      registerDoc<DocProviderT<false>>(rt, docs, 1, "acme.Plain", kAcme);
    }
    registerDoc<DocProviderT<false>>(rt, docs, 1, "test.Plain");
    if (k.doc) {
      registerDoc(rt, docs, k.docVersion);
    }
    if (k.note) {
      registerNote(rt, notes, k.noteVersion, k.copyNotes);
    }
    lib.setAuditRelations(true);
  }
  ~IoWorld()
  {
    test_assert(valid());
  }

  BlockId id(BlockHandle h) const
  {
    return lib.get(h)->id();
  }
  bool valid() const
  {
    return lib.validate().ok() && rt.validateAll().ok() && !rt.hasStickyError();
  }
  /* A Doc with a chosen id. */
  BlockHandle doc(uint64_t n, const char *name, TypeId type = kDoc)
  {
    Doc *d = litestl::alloc::New<Doc>("Doc");
    d->bag = makeDocBag(docs);
    d->number = int64_t(n) * 10;
    const ClientId client =
        type == kAcmeDoc || type == kAcmePlain ? kAcme : ClientId::of(kCoredccClient);
    Result<BlockHandle> h =
        lib.createWithId(type, BlockId{client, LocalId{7, n}}, name, d);
    test_assert(h.ok());
    return h.ok() ? *h : BlockHandle();
  }
  Doc *data(BlockHandle h) const
  {
    return static_cast<Doc *>(lib.get(h)->clientData());
  }
  /* Sets a doc's refs; null handles leave them null. */
  void link(BlockHandle h, BlockHandle mesh, BlockHandle parent = BlockHandle())
  {
    Doc *d = data(h);
    d->mesh.target = mesh ? id(mesh) : BlockId();
    d->parent.target = parent ? id(parent) : BlockId();
    lib.noteRefsChanged();
  }
  Note *note(BlockHandle h, int value, BlockId about)
  {
    Note *n = litestl::alloc::New<Note>("Note");
    notes.alive++;
    n->value = value;
    n->about.target = about;
    test_assert(lib.attach(h, kNote, n).ok());
    return n;
  }
  void addOptEntry(BlockHandle h, BlockId target, const char *name = "opt")
  {
    data(h)->entries.append(entryOf(name, pointerTo(target)));
    lib.noteRefsChanged();
  }
  void root(BlockHandle h)
  {
    lib.setFlag(h, BlockFlags::Root, true);
  }
  Bytes save(SaveReport *report = nullptr)
  {
    Result<Bytes> r = saveLibrary(lib, report);
    if (!r.ok()) {
      fprintf(stderr, "save: %s\n", r.status().message());
    }
    test_assert(r.ok());
    return r.ok() ? std::move(r).value() : Bytes();
  }
  Status
  load(const Bytes &b, LoadReport *report = nullptr, const LoadOptions &options = {})
  {
    Result<LoadReport> r = detail::loadLibrary(lib, view(b), options);
    if (r.ok() && report) {
      *report = std::move(r).value();
      return Status();
    }
    return r.status();
  }
  Result<AppendResult> append(const Bytes &b, const AppendOptions &options = {})
  {
    return detail::appendLibrary(lib, view(b), options);
  }
  BlockHandle at(uint64_t n, ClientId client = ClientId::of(kCoredccClient)) const
  {
    return lib.find(BlockId{client, LocalId{7, n}});
  }
};

inline std::string issueText(const IoIssues &issues)
{
  std::string s;
  for (const IoIssue &i : issues.first) {
    s += std::string(i.blockName.c_str()) + ": " + i.why.message() + "\n";
  }
  return s;
}

/* ---- comparing ---- */

inline bool differs(const char *what, const Datablock &b)
{
  fprintf(stderr, "'%s' differs in %s\n", b.name().c_str(), what);
  return false;
}

inline bool sameCell(const RefCell &a, const RefCell &b)
{
  return a.target == b.target && a.kind == b.kind;
}

inline bool sameCells(const Vector<RefCell> &a, const Vector<RefCell> &b)
{
  bool same = a.size() == b.size();
  for (int i = 0; same && i < int(a.size()); i++) {
    same = sameCell(a[i], b[i]);
  }
  return same;
}

inline bool samePayload(const BlockPayload &a, const BlockPayload &b)
{
  return a.version == b.version && sameBytes(a.bytes, b.bytes) &&
         sameCells(a.refs, b.refs);
}

inline bool sameEntries(const Vector<BlockEntry> &a, const Vector<BlockEntry> &b)
{
  bool same = a.size() == b.size();
  for (int i = 0; same && i < int(a.size()); i++) {
    same = strcmp(a[i].name.c_str(), b[i].name.c_str()) == 0 &&
           a[i].value == b[i].value && a[i].unknownTag == b[i].unknownTag &&
           sameBytes(a[i].raw, b[i].raw) && a[i].refs.size() == b[i].refs.size();
    for (int k = 0; same && k < int(a[i].refs.size()); k++) {
      same = sameCell(a[i].refs[k], b[i].refs[k]);
    }
  }
  return same;
}

inline bool sameDocs(const Doc &a, const Doc &b)
{
  bool same = a.number == b.number && sameCell(a.parent, b.parent) &&
              sameCell(a.mesh, b.mesh) && sameEntries(a.entries, b.entries) &&
              sameEntries(a.unread, b.unread);
  for (const char *name : {"size", "target", "extra", "other"}) {
    const int x = a.bag.indexOf(name), y = b.bag.indexOf(name);
    same =
        same && (x < 0 || y < 0 ? x == y
                                : a.bagRefs.value(a.bag, x) == b.bagRefs.value(b.bag, y));
  }
  return same;
}

/* A record of `a` and the same record of `b`, loaded from a save of `a`. */
inline bool
sameRecord(const Library &la, BlockHandle ha, const Library &lb, BlockHandle hb)
{
  const Datablock &a = *la.get(ha);
  const Datablock &b = *lb.get(hb);
  if (strcmp(a.name().c_str(), b.name().c_str()) != 0) {
    return differs("name", a);
  }
  if (strcmp(a.sourceName().c_str(), b.sourceName().c_str()) != 0) {
    return differs("source name", a);
  }
  if (a.type() != b.type() || a.flags() != b.flags()) {
    return differs("type or flags", a);
  }
  if (a.state() == BlockState::Detached) {
    Vector<RefCell> refs;
    la.forEachDataRef(ha, [&](RefCell &c) { refs.append(c); }).ignore();
    if (b.state() != BlockState::Placeholder ||
        b.placeholderReason() != PlaceholderReason::MissingBlock ||
        !sameCells(refs, b.placeholderData().refs))
    {
      return differs("what a Detached block becomes", a);
    }
  } else if (a.state() != b.state()) {
    return differs("state", a);
  } else if (a.state() == BlockState::Placeholder) {
    if (a.placeholderReason() != b.placeholderReason() ||
        strcmp(a.placeholderType().c_str(), b.placeholderType().c_str()) != 0 ||
        !samePayload(a.placeholderData(), b.placeholderData()))
    {
      return differs("placeholder", a);
    }
  } else if (!sameDocs(*static_cast<const Doc *>(a.clientData()),
                       *static_cast<const Doc *>(b.clientData())))
  {
    return differs("data", a);
  }

  int slots = 0, slotsB = 0, attachments = 0, attachmentsB = 0;
  bool same = true;
  la.forEachSlot(ha, [&](TypeId ns, int64_t key, const RefCell &cell) {
    const RefCell *other = lb.slot(hb, ns, key);
    same = same && other && sameCell(cell, *other);
    slots++;
  });
  lb.forEachSlot(hb, [&](TypeId, int64_t, const RefCell &) { slotsB++; });
  la.forEachAttachment(ha, [&](TypeId type, void *data) {
    const Note *x = static_cast<const Note *>(data);
    const Note *y = static_cast<const Note *>(lb.attachment(hb, type));
    same = same && y && x->value == y->value && sameCell(x->about, y->about);
    attachments++;
  });
  lb.forEachAttachment(hb, [&](TypeId, void *) { attachmentsB++; });
  la.forEachOpaqueAttachment(ha, [&](const OpaqueAttachment &x) {
    bool found = false;
    lb.forEachOpaqueAttachment(hb, [&](const OpaqueAttachment &y) {
      found = found || (strcmp(x.typeName.c_str(), y.typeName.c_str()) == 0 &&
                        samePayload(x.payload, y.payload));
    });
    same = same && found;
    attachments++;
  });
  lb.forEachOpaqueAttachment(hb, [&](const OpaqueAttachment &) { attachmentsB++; });
  return (same && slots == slotsB && attachments == attachmentsB) ||
         differs("slots or attachments", a);
}

/* Every record of `a` is in `b`, the same, and nothing else is. */
inline bool sameLibraries(const Library &a, const Library &b)
{
  bool same = a.size() == b.size();
  if (!same) {
    fprintf(stderr, "%d records, not %d\n", b.size(), a.size());
  }
  a.forEach([&](BlockHandle ha, const Datablock &block) {
    const BlockHandle hb = b.find(block.id());
    same = same && hb && sameRecord(a, ha, b, hb);
  });
  return same;
}

/* ---- files by hand ---- */

/* Recomputes totalBytes and the checksum. */
inline void fixUp(Bytes &file)
{
  if (file.size() < 36) {
    return;
  }
  const auto put = [&](size_t at, uint64_t v) {
    for (int i = 0; i < 8; i++) {
      file[int(at) + i] = uint8_t(v >> (8 * i));
    }
  };
  put(16, uint64_t(file.size()));
  const std::string_view all(reinterpret_cast<const char *>(file.data()),
                             file.size() - 8);
  put(size_t(file.size()) - 8, coredcc::detail::fnv1a64(all));
}

/* A file built from chunk contents, without the writer. */
struct FileBuilder {
  struct Chunk {
    uint32_t tag;
    uint32_t flags;
    Bytes bytes;
    uint64_t raw;
  };
  uint32_t version = 1;
  uint32_t minReader = 1;
  std::vector<Chunk> chunks;

  void chunk(uint32_t tag, Bytes bytes, uint32_t flags = 1)
  {
    const uint64_t raw = uint64_t(bytes.size());
    chunks.push_back(Chunk{tag, flags, std::move(bytes), raw});
  }
  Bytes build() const
  {
    BytesWriter out;
    out.raw(detail::libio::kMagic);
    out.u32(version);
    out.u32(minReader);
    out.u64(0);
    out.u32(uint32_t(chunks.size()));
    for (const Chunk &c : chunks) {
      out.u32(c.tag);
      out.u32(c.flags);
      out.u64(c.raw);
      out.u64(uint64_t(c.bytes.size()));
      out.raw(view(c.bytes));
    }
    out.u64(0);
    Bytes file = out.take();
    fixUp(file);
    return file;
  }
};

/* A record of a hand-made file. Names are indices plus 1. */
struct RawRecord {
  uint32_t client = 1;
  LocalId local{7, 1};
  uint32_t type = 2;
  std::string name = "a";
  uint32_t flags = 1;
  uint8_t as = 0;
  uint32_t version = 1;
  uint32_t source = 0;
  std::vector<std::pair<uint8_t, LocalId>> rows; /* of client 1, or null */
  uint32_t blob = 0;
  std::string extension;
  litestl::util::function<void(BytesWriter &)> slots, attachments, after;

  static void row(BytesWriter &out, uint8_t kind, LocalId local, uint32_t client = 1)
  {
    out.u8(kind);
    out.u32(local.isNull() ? 0 : client);
    out.u64(local.hi);
    out.u64(local.lo);
  }
  void write(BytesWriter &out) const
  {
    const size_t t = out.beginSection();
    out.u32(client);
    out.u64(local.hi);
    out.u64(local.lo);
    out.u32(type);
    out.blob(std::span(reinterpret_cast<const uint8_t *>(name.data()), name.size()));
    out.u32(flags);
    out.u8(as);
    out.u32(version);
    out.u32(source);
    out.u32(uint32_t(rows.size()));
    for (const auto &r : rows) {
      row(out, r.first, r.second);
    }
    if (slots) {
      slots(out);
    } else {
      out.u32(0);
    }
    if (attachments) {
      attachments(out);
    } else {
      out.u32(0);
    }
    out.u32(blob);
    out.blob(
        std::span(reinterpret_cast<const uint8_t *>(extension.data()), extension.size()));
    if (after) {
      after(out);
    }
    out.endSection(t);
  }
};

struct RawFile : FileBuilder {
  std::vector<std::string> names = {"coredcc", "x.Type"};
  std::vector<RawRecord> records;
  std::vector<Bytes> blobs;

  Bytes file()
  {
    BytesWriter n, b, d;
    n.u32(uint32_t(names.size()));
    for (const std::string &s : names) {
      n.u8(0);
      n.str(s);
    }
    b.u32(uint32_t(records.size()));
    for (const RawRecord &r : records) {
      r.write(b);
    }
    d.u32(uint32_t(blobs.size()));
    for (const Bytes &x : blobs) {
      d.u8(0);
      d.u64(uint64_t(x.size()));
      d.u64(uint64_t(x.size()));
      d.raw(view(x));
    }
    chunks.clear();
    chunk(detail::libio::kTagName, n.take());
    chunk(detail::libio::kTagBlocks, b.take());
    chunk(detail::libio::kTagData, d.take());
    return build();
  }
};

inline Bytes bytesOf(std::initializer_list<int> list)
{
  Bytes b;
  for (int v : list) {
    b.append(uint8_t(v));
  }
  return b;
}

} // namespace coredcc_test
