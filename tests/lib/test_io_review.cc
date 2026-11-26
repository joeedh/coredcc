/* Regressions from the review of library IO
 * (docs/reports/feature-report-library-io-2026-11-24.md), by the report's numbers. */
#include "coredcc/lib/attachment.h"
#include "coredcc/lib/duplicate.h"

#include "io_fixture.h"

test_init;

using namespace coredcc;
using namespace coredcc_test;

namespace {

BlockId cid(uint64_t n, ClientId client = ClientId::of(kCoredccClient))
{
  return BlockId{client, LocalId{7, n}};
}

PropValue runtimePointer()
{
  StableRef r;
  r.owner = OwnerKey::ofRuntime(TypeId::of("owner:tool").value);
  r.type = StructId::of("acme.Mesh");
  return PropValue::ofPointer(r);
}

std::vector<BlockId> cellsOf(BagRefs &refs)
{
  std::vector<BlockId> out;
  RefVisitor v(
      [](void *to, RefCell &c) {
        static_cast<std::vector<BlockId> *>(to)->push_back(c.target);
      },
      &out);
  refs.visit(v);
  return out;
}

Bytes extension(std::initializer_list<std::pair<uint32_t, int>> fields)
{
  BytesWriter out;
  for (const auto &f : fields) {
    out.u32(f.first);
    const uint8_t byte = uint8_t(f.second);
    out.blob(std::span(&byte, 1));
  }
  return out.take();
}

std::string textOf(const Bytes &b)
{
  return std::string(reinterpret_cast<const char *>(b.data()), size_t(b.size()));
}

bool sameSpan(std::span<const uint8_t> a, const Bytes &b)
{
  return a.size() == size_t(b.size()) &&
         (a.empty() || memcmp(a.data(), b.data(), a.size()) == 0);
}

} // namespace

/* ---- 1: the cells say which block a bag's pointer is into ---- */

static void test_bag_refs()
{
  PropertyBag bag;
  test_assert(bag.add(PropertyDef::floatProp("size")).ok());
  test_assert(bag.add(PropertyDef::pointerProp("p", StructId::of("acme.Mesh"))).ok());
  BagRefs refs(RefKind::Weak);
  test_assert(refs.kind() == RefKind::Weak && !refs.find("p") &&
              refs.value(bag, 1) == bag.value(1));
  test_assert(refs.set(bag, "nope", PropValue()).code() == StatusCode::NotFound);
  test_assert(refs.set(bag, "size", PropValue::ofFloat(2)).ok() && !refs.find("size"));
  test_assert(refs.value(bag, 0) == PropValue::ofFloat(2));
  test_assert(!refs.set(bag, "p", PropValue::ofFloat(2)).ok() && !refs.find("p"));
  test_assert(!refs.isSet(bag, 1) && refs.isSet(bag, 0));

  test_assert(refs.set(bag, "p", pointerTo(cid(1), "faces")).ok());
  test_assert(refs.find("p")->target == cid(1) && refs.find("p")->kind == RefKind::Weak);
  test_assert(refs.value(bag, 1) == pointerTo(cid(1), "faces") && refs.isSet(bag, 1));
  /* A remap writes the cell; the rest of the pointer is the bag's. */
  refs.cell("p").target = cid(2);
  test_assert(refs.value(bag, 1) == pointerTo(cid(2), "faces"));
  test_assert(bag.value(1) == pointerTo(cid(1), "faces"));
  test_assert(cellsOf(refs) == std::vector<BlockId>{cid(2)});
  /* An unlink too. */
  refs.cell("p").target = BlockId();
  test_assert(refs.value(bag, 1) == PropValue::nullPointer() && refs.isSet(bag, 1));
  test_assert(refs.set(bag, "p", pointerTo(cid(3))).ok() &&
              refs.find("p")->target == cid(3));
  test_assert(refs.reset(bag, "p").ok() && refs.find("p")->isNull() &&
              !refs.isSet(bag, 1));
  test_assert(refs.reset(bag, "size").ok() &&
              refs.reset(bag, "nope").code() == StatusCode::NotFound);

  /* A cell written before the bag: the block itself. */
  BagRefs cold(RefKind::Strong);
  PropertyBag empty = bag;
  cold.cell("p").target = cid(4);
  test_assert(cold.isSet(empty, 1) &&
              cold.value(empty, 1).asPointer().type == StructId::of("acme.Mesh"));
  test_assert(cold.value(empty, 1).asPointer().owner == OwnerKey::ofBlock(cid(4)) &&
              cold.value(empty, 1).asPointer().key.size() == 0);
  /* A pointer into a runtime owner has no block. */
  test_assert(cold.set(empty, "p", runtimePointer()).ok() && cold.find("p")->isNull());
  test_assert(cold.value(empty, 1) == runtimePointer());
}

static void test_bag_payload()
{
  IoWorld w;
  const BlockHandle a = w.doc(1, "a"), b = w.doc(2, "b"), c = w.doc(3, "c");
  Doc &d = *w.data(a);
  test_assert(d.bagRefs.set(d.bag, "target", pointerTo(w.id(b))).ok());
  d.bagRefs.cell("target").target = w.id(c);
  d.bagRefs.cell("other").target = w.id(b); /* as PointerAccessor::cell does */
  w.lib.noteRefsChanged();
  Result<BlockPayload> p = writePayload(w.lib, a);
  test_assert(p.ok() && p->refs.size() == 4);
  const BlockHandle back = emptyDoc(w.lib, "back");
  test_assert(readPayload(w.lib, back, *p).ok());
  const Doc &e = *w.data(back);
  test_assert(e.bagRefs.value(e.bag, 1) == pointerTo(w.id(c)));
  test_assert(e.bagRefs.find("other")->target == w.id(b) &&
              e.bagRefs.value(e.bag, 3).asPointer().owner == OwnerKey::ofBlock(w.id(b)));
  /* Unlinked: a null pointer, and no row. */
  w.data(back)->bagRefs.cell("target").target = BlockId();
  w.lib.noteRefsChanged();
  Result<BlockPayload> cut = writePayload(w.lib, back);
  test_assert(cut.ok() && cut->refs.size() == 3);
  const BlockHandle again = emptyDoc(w.lib, "again");
  test_assert(readPayload(w.lib, again, *cut).ok());
  test_assert(*w.data(again)->bag.get("target") == PropValue::nullPointer() &&
              w.data(again)->bag.isSet("target"));

  /* Set behind the cells' back: it fails instead of losing the reference. */
  test_assert(w.data(again)->bag.set("extra", PropValue::ofString("x")).ok());
  Doc &bad = *w.data(b);
  test_assert(bad.bag.set("target", pointerTo(w.id(c))).ok());
  test_assert(hasMsg(writePayload(w.lib, b).status(), "no ref for it"));
  test_assert(bad.bag.reset("target").ok());

  /* A row more than the pointer needs is a newer build's: kept as it is. */
  BlockWriter out(w.lib);
  BlockEntry odd = entryOf("target", pointerTo(w.id(b)));
  odd.refs.append(odd.refs[0]);
  BlockEntry number = entryOf("size", PropValue::ofFloat(1));
  number.refs.append(odd.refs[0]);
  const BlockEntry all[] = {odd, number};
  out.entries(all);
  Result<BlockPayload> q = PayloadAccess::take(out, 1);
  BlockReader in(w.lib, *q);
  PropertyBag bag = makeDocBag(w.docs);
  BagRefs refs;
  Vector<BlockEntry> unread;
  in.bag(bag, refs, unread);
  test_assert(in.bytes().ok() && unread.size() == 2 && unread[0].refs.size() == 2);
  test_assert(!bag.isSet("target") && !bag.isSet("size") && !refs.find("target"));
}

/* A duplicate whose bag pointed at its source saves, and points at itself. */
static void test_duplicated_bag()
{
  for (TypeId type : {kDoc, kPlain}) {
    for (bool audit : {true, false}) {
      IoWorld w;
      const BlockHandle a = w.doc(1, "a", type);
      w.root(a);
      test_assert(
          w.data(a)->bagRefs.set(w.data(a)->bag, "target", pointerTo(w.id(a))).ok());
      w.lib.noteRefsChanged();
      w.lib.setAuditRelations(audit);
      const BlockHandle set[] = {a};
      Result<IdMap> ids = duplicateBlocks(w.lib, set);
      test_assert(ids.ok());
      const BlockId copy = ids->find(w.id(a));
      w.lib.setFlag(w.lib.find(copy), BlockFlags::Root, true);
      SaveReport report;
      const Bytes bytes = w.save(&report);
      test_assert(report.issues.total == 0);
      IoWorld r;
      test_assert(r.load(bytes).ok() && sameLibraries(w.lib, r.lib));
      const Doc &d = *r.data(r.lib.find(copy));
      test_assert(d.bagRefs.value(d.bag, 1) == pointerTo(copy));
      test_assert(r.data(r.at(1))->bagRefs.find("target")->target == cid(1));
    }
  }
}

/* ---- 2 and 9: the hooks' arguments ---- */

namespace {

struct Logged {
  RefCell weak{RefKind::Weak};
};

std::string g_log;
const Library *g_lib = nullptr;
bool g_leaveBytes = false;

void logCall(const char *what, const Datablock &b)
{
  g_log += std::string(what) + " " + b.name().c_str() + ";";
  test_assert(&b.library() == g_lib);
}

/* Logs the block each slot is called for. */
struct LogProvider {
  void destroy(void *data)
  {
    litestl::alloc::Delete(static_cast<Logged *>(data));
  }
  void *create(const Datablock &block)
  {
    logCall("create", block);
    return litestl::alloc::New<Logged>("Logged");
  }
  void *copy(const Datablock &from, const void *data, const Datablock &to)
  {
    logCall("from", from);
    logCall("to", to);
    test_assert(from.id() == cid(1) && to.id() != cid(1));
    return litestl::alloc::New<Logged>("Logged", *static_cast<const Logged *>(data));
  }
  Status serialize(const Datablock &block, const void *data, BlockWriter &out) const
  {
    logCall("serialize", block);
    out.ref(static_cast<const Logged *>(data)->weak);
    out.bytes().u8(1);
    return Status();
  }
  Status deserialize(const Datablock &block, BlockReader &in)
  {
    logCall("deserialize", block);
    Logged *l = litestl::alloc::New<Logged>("Logged");
    in.ref(l->weak);
    if (!g_leaveBytes) {
      in.bytes().u8();
    }
    in.setData(l);
    return Status();
  }
  void forEachRef(const Datablock &block, const void *data, RefVisitor &v) const
  {
    test_assert(&block.library() == g_lib);
    v.visit(const_cast<Logged *>(static_cast<const Logged *>(data))->weak);
  }
  void onWeakTargetLost(const Datablock &block, void *data, const Datablock &lost)
  {
    test_assert(static_cast<Logged *>(data)->weak.target == lost.id());
    logCall("lost", block);
    logCall("that", lost);
  }
  static lbt::Struct<LogProvider> *defineBindings()
  {
    auto *st = new lbt::Struct<LogProvider>("test::LogProvider", sizeof(LogProvider));
    BIND_STRUCT_METHOD(st, destroy, MARGS("data"));
    BIND_STRUCT_METHOD(st, create, MARGS("block"));
    BIND_STRUCT_METHOD(st, copy, MARGS("from", "data", "to"));
    BIND_STRUCT_METHOD(st, serialize, MARGS("block", "data", "out"));
    BIND_STRUCT_METHOD(st, deserialize, MARGS("block", "in"));
    BIND_STRUCT_METHOD(st, forEachRef, MARGS("block", "data", "visitor"));
    BIND_STRUCT_METHOD(st, onWeakTargetLost, MARGS("block", "data", "lost"));
    return st;
  }
};

TypeId registerLog(Runtime &rt)
{
  const std::string impl = "test.Log#provider";
  test_assert((registerImpl<AttachmentProvider, LogProvider>(rt, impl).ok()));
  Result<Owned<AttachmentProvider>> owned =
      own<AttachmentProvider>(rt, impl, litestl::alloc::New<LogProvider>("provider"));
  AttachmentTypeInfo info;
  info.name = "test.Log";
  info.onDuplicate = DuplicatePolicy::Copy;
  return *rt.ext<AttachmentTypeRegistry>().add(info, std::move(owned).value());
}

} // namespace

static void test_weak_lost()
{
  ThingHost things;
  Runtime rt;
  registerThing(rt, things);
  Library lib(&rt);
  const BlockHandle target = makeThing(lib, "target");
  const auto user = [&](const char *name) {
    const BlockHandle h = makeThing(lib, name);
    lib.setFlag(h, BlockFlags::Root, true);
    thing(lib, h)->parent.target = lib.get(target)->id();
    return h;
  };
  const BlockHandle live = user("live");
  /* An attachment that has no reference to it isn't told. */
  g_lib = &lib;
  const TypeId log = registerLog(rt);
  test_assert(lib.attach(live, log, litestl::alloc::New<Logged>("Logged")).ok());
  const BlockHandle deleted = user("deleted"), detached = user("detached");
  const BlockHandle slotted = makeThing(lib, "slotted");
  lib.setFlag(slotted, BlockFlags::Root, true);
  test_assert(lib.setSlot(slotted, kThing, 0, lib.get(target)->id(), RefKind::Weak).ok());
  PlaceholderInfo info;
  info.typeName = "test.Thing"; /* registered, but its payload is newer */
  info.name = "newer";
  RefCell row(RefKind::Weak);
  row.target = lib.get(target)->id();
  info.payload.refs.append(row);
  test_assert(lib.addPlaceholder(cid(9), info).ok());
  lib.noteRefsChanged();
  test_assert(lib.markDeleted(deleted).ok());
  test_assert(lib.onHostDestroyed(lib.get(detached)->id()).ok());
  things.onLost = [&]() { test_assert(lib.busy()); };
  test_assert(*lib.collect() == 1);
  test_assert(g_log.empty());
  test_assert(things.log == "lost live->target;lost deleted->target;destroy target;");
  test_assert(lib.validate().ok());
}

static void test_attachment_blocks()
{
  Bytes bytes;
  {
    IoWorld w;
    g_lib = &w.lib;
    const TypeId type = registerLog(w.rt);
    const BlockHandle a = w.doc(1, "a"), gone = w.doc(2, "gone");
    w.root(a);
    AttachmentProvider p = w.rt.ext<AttachmentTypeRegistry>().find(type)->provider.get();
    test_assert(p.hasOnWeakTargetLost());
    Logged *l = static_cast<Logged *>(p.create(*w.lib.get(a)));
    l->weak.target = w.id(gone);
    test_assert(w.lib.attach(a, type, l).ok());
    bytes = w.save();
    const BlockHandle set[] = {a};
    Result<IdMap> ids = duplicateBlocks(w.lib, set);
    test_assert(ids.ok() && w.lib.rename(w.lib.find(ids->find(w.id(a))), "copy").ok());
    test_assert(g_log == "create a;serialize a;from a;to a;");
    g_log.clear();
    test_assert(w.lib.markDeleted(a).ok()); /* its attachments are told all the same */
    test_assert(*w.lib.collect() == 2);     /* the copy is an orphan */
    test_assert(g_log == "lost a;that gone;");
    g_log.clear();
  }
  {
    IoWorld r;
    g_lib = &r.lib;
    registerLog(r.rt);
    test_assert(r.load(bytes).ok() && g_log == "deserialize a;");
  }
  /* Data that a failed read made is severed and destroyed. */
  IoWorld f;
  g_lib = &f.lib;
  registerLog(f.rt);
  g_leaveBytes = true;
  test_assert(f.load(bytes).ok());
  g_leaveBytes = false;
  g_log.clear();
}

static void test_rekey_context()
{
  Bytes bytes;
  {
    IoWorld from;
    const BlockHandle a = from.doc(1, "a", kAcmeDoc), b = from.doc(2, "b", kAcmeDoc);
    from.root(a);
    from.root(b);
    bytes = from.save();
  }
  IoWorld w;
  w.root(w.doc(5, "here", kAcmeDoc));
  int calls = 0;
  w.docs.onRekey = [&](const Datablock &b, const RekeyContext &context) {
    calls++;
    test_assert(&b.library() == &w.lib && &context.library() == &w.lib);
    test_assert(!context.isFree(BlockId()) && !context.isFree(cid(5, kAcme)));
    test_assert(context.isFree(cid(7, kAcme)));
    test_assert(context.isFree(cid(6, kAcme)) ==
                (calls == 1)); /* the first block got it */
    return cid(calls == 1 ? 6 : 7, kAcme);
  };
  test_assert(w.append(bytes).ok() && calls == 2 && w.lib.size() == 3);
  w.docs.onRekey = nullptr;
}

/* ---- 23 and 24: what a newer version may add ---- */

static void test_newer_codes()
{
  const auto with = [](uint32_t version,
                       litestl::util::function<void(RawFile &)> change) {
    RawFile f;
    f.version = version;
    f.records.push_back(RawRecord());
    change(f);
    f.file();
    return f;
  };
  const auto refused = [](const RawFile &f, const char *what) {
    IoWorld w;
    Status s = w.load(f.build());
    const StatusCode want =
        f.version > 1 ? StatusCode::Unsupported : StatusCode::InvalidArgument;
    return s.code() == want && hasMsg(s, what) && w.lib.size() == 0;
  };
  for (uint32_t version : {1u, 2u}) {
    test_assert(refused(
        with(version, [](RawFile &f) { f.records[0].rows = {{9, LocalId{7, 2}}}; }),
        "unknown kind of reference"));
    test_assert(
        refused(with(version, [](RawFile &f) { f.records[0].as = 9; }), "unknown way"));
    RawFile blob = with(version, [](RawFile &f) {
      f.records[0].blob = 1;
      f.blobs.push_back(bytesOf({1}));
    });
    blob.chunks[2].bytes[4] = 7; /* the codec */
    test_assert(refused(blob, "compressed in an unknown way"));
    RawFile name = with(version, [](RawFile &) {});
    name.chunks[0].bytes[4] = 7; /* the form */
    test_assert(refused(name, "unknown kind of name"));
  }
}

static void test_extension()
{
  const Bytes kept = extension({{0, 10}, {5, 20}});
  const auto file = [&](const Bytes &ext, uint8_t as = 1, const char *type = "test.Doc") {
    RawFile f;
    f.names[1] = type;
    RawRecord r;
    r.as = as;
    r.extension = textOf(ext);
    f.records.push_back(r);
    return f.file();
  };
  /* A file of an older build's is read, kept and written again, whatever the record is.
   */
  IoWorld w;
  test_assert(w.load(file(kept)).ok());
  const BlockHandle h = w.at(1);
  test_assert(sameSpan(w.lib.get(h)->fileExtension(), kept));
  const Bytes again = w.save();
  IoWorld r;
  test_assert(r.load(again).ok() && sameSpan(r.lib.get(r.at(1))->fileExtension(), kept));
  test_assert(sameBytes(r.save(), again));
  /* Filled, it still has it. */
  Doc *d = litestl::alloc::New<Doc>("Doc");
  d->bag = makeDocBag(r.docs);
  test_assert(r.lib.createWithId(kDoc, cid(1), "a", d).ok());
  const Bytes live = r.save();
  IoWorld l;
  test_assert(l.load(live).ok() && l.lib.get(l.at(1))->state() == BlockState::Live);
  test_assert(sameSpan(l.lib.get(l.at(1))->fileExtension(), kept));
  /* So do copies, and what is appended. */
  const BlockHandle set[] = {l.at(1)};
  Result<IdMap> ids = duplicateBlocks(l.lib, set);
  test_assert(ids.ok() &&
              sameSpan(l.lib.get(l.lib.find(ids->find(cid(1))))->fileExtension(), kept));
  Result<AppendResult> appended = l.append(live);
  test_assert(appended.ok() &&
              sameSpan(l.lib.get(appended->report.blocks[0])->fileExtension(), kept));

  /* A critical field: the data isn't read, then or later, and it is written back. */
  const Bytes critical = extension({{2, 1}, {detail::libio::kFieldCritical | 3, 9}});
  RawFile f;
  f.names[1] = "test.Doc";
  f.records.push_back(RawRecord());
  f.records[0].extension = textOf(critical);
  f.records[0].blob = 1;
  f.blobs.push_back(bytesOf({1, 2, 3}));
  IoWorld c;
  LoadReport report;
  test_assert(c.load(f.file(), &report).ok() && report.missingType == 1);
  test_assert(has(issueText(report.issues), "newer build") && c.docs.lastVersion == 0);
  Result<LoadReport> resolved = detail::resolvePlaceholders(c.lib);
  test_assert(resolved.ok() && resolved->blocks.size() == 0 &&
              resolved->missingType == 1);
  IoWorld c2;
  test_assert(c2.load(c.save()).ok() &&
              c2.lib.get(c2.at(1))->placeholderPayload().size() == 3);
  test_assert(sameSpan(c2.lib.get(c2.at(1))->fileExtension(), critical));
  IoWorld host;
  test_assert(host.load(file(critical)).ok());
  Doc own;
  Status s = host.lib.createWithId(kDoc, cid(1), "a", &own).status();
  test_assert(s.code() == StatusCode::Unsupported && hasMsg(s, "newer build"));
  test_assert(host.lib.get(host.at(1))->state() == BlockState::Placeholder);

  /* Damage. */
  const auto damaged = [&](const Bytes &ext, const char *what) {
    IoWorld bad;
    Status st = bad.load(file(ext));
    return st.code() == StatusCode::InvalidArgument && hasMsg(st, what) &&
           bad.lib.size() == 0;
  };
  test_assert(damaged(extension({{5, 1}, {1, 1}}), "out of order"));
  test_assert(damaged(extension({{5, 1}, {5, 1}}), "out of order"));
  test_assert(damaged(bytesOf({1, 0, 0}), "extension"));
  test_assert(damaged(bytesOf({1, 0, 0, 0, 9, 0, 0, 0, 1}), "extension"));
  /* It is charged. */
  Bytes large;
  large.resize(2 << 20);
  for (int i = 0; i < int(large.size()); i++) {
    large[i] = 0;
  }
  large[4] = 0xf8; /* one field, of all but its 8 bytes */
  large[5] = 0xff;
  large[6] = 0x1f;
  LoadOptions options;
  options.limits.maxBytes = 1 << 20;
  IoWorld small;
  test_assert(small.load(file(large), nullptr, options).code() == StatusCode::OutOfRange);
  test_assert(small.load(file(large)).ok());
}

/* ---- 5: what a pointer's key takes ---- */

static void test_key_budget()
{
  IoWorld w;
  constexpr int kSegments = 1000;
  StableRef r;
  r.owner = OwnerKey::ofRuntime(1);
  for (int i = 0; i < kSegments; i++) {
    r.key.append(KeySegment::member("a"));
  }
  BlockWriter out(w.lib);
  Vector<BlockEntry> entries;
  for (int i = 0; i < 100; i++) {
    entries.append(entryOf("p", PropValue::ofPointer(r)));
  }
  out.entries(std::span<const BlockEntry>(entries.data(), size_t(entries.size())));
  Result<BlockPayload> p = PayloadAccess::take(out, 1);
  test_assert(p.ok());
  /* Less than what the entries take is refused before it is taken. */
  for (uint64_t limit : {uint64_t(4) << 20, uint64_t(64) << 20}) {
    BlockReader in(w.lib, *p);
    uint64_t left = limit;
    PayloadAccess::setBudget(in, &left);
    const size_t before = litestl::alloc::getMemorySize();
    Vector<BlockEntry> back;
    in.entries(back);
    const size_t took = litestl::alloc::getMemorySize() - before;
    test_assert(took <= limit - left + 4096);
    test_assert(in.bytes().ok() == (limit > (uint64_t(4) << 20)));
    test_assert(in.bytes().ok() ? back.size() == 100
                                : in.bytes().status().code() == StatusCode::OutOfRange);
  }
}

int main()
{
  test_bag_refs();
  test_bag_payload();
  test_duplicated_bag();
  test_weak_lost();
  test_attachment_blocks();
  test_rekey_context();
  test_newer_codes();
  test_extension();
  test_key_budget();
  return test_end();
}
