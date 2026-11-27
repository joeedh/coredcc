/* Regressions from the review of library IO, round 2, by the report's numbers
 * (docs/reports/feature-report-library-io-2026-11-24.md). Finding 3 is test_adopt.cc's.
 */
#include "coredcc/lib/attachment.h"
#include "coredcc/lib/duplicate.h"
#include "coredcc/lib/pin.h"

#include "io_fixture.h"

#include <chrono>
#include <type_traits>

test_init;

using namespace coredcc;
using namespace coredcc_test;

namespace {

BlockId cid(uint64_t n, ClientId client = ClientId::of(kCoredccClient))
{
  return BlockId{client, LocalId{7, n}};
}

struct Watch {
  std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
  double seconds() const
  {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
        .count();
  }
};

BlockHandle transient(IoWorld &w, uint64_t n, const char *name)
{
  Result<BlockHandle> h =
      w.lib.createWithId(kTransient, cid(n), name, litestl::alloc::New<Thing>("Thing"));
  test_assert(h.ok());
  return *h;
}

} // namespace

/* ---- 4: what can be attached, and 13: what its type says ---- */

static void test_attach()
{
  IoWorld w;
  const BlockHandle a = w.doc(1, "a");
  w.root(a);
  test_assert(w.lib.attach(a, kNote, nullptr).code() == StatusCode::InvalidArgument);
  test_assert(!w.lib.hasAttachment(a, kNote));
  w.note(a, 1, BlockId());
  Note own;
  test_assert(w.lib.attach(a, kNote, &own).code() == StatusCode::AlreadyExists);
  test_assert(w.lib.hasAttachment(a, kNote) &&
              !w.lib.hasAttachment(BlockHandle(), kNote));
  OpaqueAttachment o;
  o.typeName = "test.Note";
  test_assert(w.lib.attachOpaque(a, o).code() == StatusCode::AlreadyExists);
  o.typeName = "";
  test_assert(w.lib.attachOpaque(a, o).code() == StatusCode::InvalidArgument);
  o.typeName = "x.One";
  test_assert(w.lib.attachOpaque(BlockHandle(), o).code() == StatusCode::NotFound);
  test_assert(w.lib.attachOpaque(a, o).ok());
  test_assert(w.lib.attachOpaque(a, o).code() == StatusCode::AlreadyExists);
  IoWorld r;
  test_assert(r.load(w.save()).ok() && sameLibraries(w.lib, r.lib));
}

static void test_storage()
{
  IoWorld w;
  registerBare(w.rt, "test.Loose", BlockStorage::Unspecified);
  const BlockHandle a = w.doc(1, "a"), orphan = w.doc(2, "orphan");
  w.root(a);
  test_assert(w.lib.attach(a, kBare, &w).ok()); /* Transient: it says so */
  test_assert(w.lib.attach(orphan, TypeId::of("test.Loose"), &w).ok());
  const Bytes bytes = w.save();
  test_assert(w.lib.attach(a, TypeId::of("test.Loose"), &w).ok());
  Status s = saveLibrary(w.lib).status();
  test_assert(
      s.code() == StatusCode::FailedPrecondition &&
      hasMsg(s,
             "these types neither serialize nor are Transient: attachment test.Loose"));
  test_assert(w.lib.detach(a, TypeId::of("test.Loose")).ok() &&
              sameBytes(w.save(), bytes));
  /* A type that contradicts itself isn't registered. */
  Runtime rt;
  NoteHost host;
  const auto add = [&](const char *impl, BlockStorage storage, auto *provider) {
    using P = std::remove_pointer_t<decltype(provider)>;
    test_assert((registerImpl<AttachmentProvider, P>(rt, impl).ok()));
    AttachmentTypeInfo info;
    info.name = std::string(impl).substr(0, 3).c_str();
    info.storage = storage;
    return rt.ext<AttachmentTypeRegistry>()
        .add(info, std::move(own<AttachmentProvider>(rt, impl, provider)).value())
        .status();
  };
  test_assert(hasMsg(
      add("t.A#p", BlockStorage::Transient, litestl::alloc::New<NoteProvider>("p")),
      "is Transient, but has serialize"));
  test_assert(
      hasMsg(add("t.B#p", BlockStorage::Payload, litestl::alloc::New<BareProvider>("p")),
             "is Payload, but has no serialize"));
  test_assert(
      add("t.C#p", BlockStorage::Payload, litestl::alloc::New<NoteProvider>("p")).ok());
  registerNote(rt, host);
  test_assert(rt.ext<AttachmentTypeRegistry>().find(kNote)->info.storage ==
              BlockStorage::Payload);
}

/* ---- 6: what hangs on a Transient block ---- */

static void test_transient()
{
  IoWorld w;
  const BlockHandle a = w.doc(1, "a"), through = w.doc(2, "through"),
                    bare = w.doc(3, "bare");
  const BlockHandle t = transient(w, 10, "actor"), plain = transient(w, 11, "plain");
  const BlockHandle loose = transient(w, 12, "loose");
  w.root(a);
  w.addOptEntry(a, w.id(t));
  w.addOptEntry(a, w.id(plain));
  test_assert(w.lib.writeRef(thing(w.lib, t)->owner, w.id(through)).ok());
  test_assert(w.lib.writeRef(thing(w.lib, plain)->owner, w.id(bare)).ok());
  w.note(t, 4, w.id(a));
  test_assert(w.lib.setSlot(t, kNote, 2, w.id(a), RefKind::OptStrong).ok());
  test_assert(w.lib.attach(plain, kBare, &w).ok()); /* which isn't saved either */
  w.note(loose, 5, w.id(a));                        /* nothing leads to it */
  SaveReport report;
  const Bytes bytes = w.save(&report);
  test_assert(report.issues.total == 0 && report.blocks == 4);
  test_assert(report.transient.size() == 2 && report.transient[0] == t &&
              report.transient[1] == plain);
  test_assert(report.throughTransient.size() == 1 && report.throughTransient[0] == bare);

  IoWorld r;
  test_assert(r.load(bytes).ok());
  const BlockHandle rt = r.lib.find(cid(10));
  const Datablock &b = *r.lib.get(rt);
  test_assert(b.placeholderReason() == PlaceholderReason::MissingBlock &&
              b.type() == kTransient && strcmp(b.name().c_str(), "actor") == 0);
  test_assert(r.lib.attachment(rt, kNote) && r.lib.slot(rt, kNote, 2)->target == cid(1));
  /* Its references keep what they kept. */
  test_assert(*r.lib.collect() == 1 && r.at(2) && !r.at(3));
  test_assert(
      r.lib.createWithId(kTransient, cid(10), "back", litestl::alloc::New<Thing>("T"))
          .ok());
  test_assert(r.lib.resolveRef(r.data(r.at(1))->entries[0].refs[0]) == rt);
  test_assert(static_cast<Note *>(r.lib.attachment(rt, kNote))->value == 4);
  test_assert(r.lib.slot(rt, kNote, 2)->target == cid(1));

  /* A slot alone, an attachment (one nobody can read too), or a newer build's fields are
   * enough. */
  for (int what = 0; what < 3; what++) {
    IoWorld v;
    const BlockHandle h = transient(v, 10, "actor");
    v.root(h);
    if (what == 0) {
      test_assert(v.lib.setSlot(h, kNote, 2, BlockId(), RefKind::OptStrong).ok());
    } else if (what == 1) {
      v.note(h, 4, BlockId());
    } else {
      OpaqueAttachment o;
      o.typeName = "x.Other";
      test_assert(v.lib.attachOpaque(h, o).ok());
    }
    test_assert(v.save(&report).size() > 0 && report.blocks == 1 &&
                report.transient.size() == 1);
  }
  RawFile f;
  f.names[1] = "test.Transient";
  RawRecord raw;
  raw.as = 1;
  raw.extension = std::string("\5\0\0\0\0\0\0\0", 8);
  f.records.push_back(raw);
  IoWorld x;
  test_assert(x.load(f.file()).ok());
  test_assert(
      x.lib.createWithId(kTransient, cid(1), "x", litestl::alloc::New<Thing>("T")).ok());
  test_assert(x.save(&report).size() > 0 && report.blocks == 1);
}

/* ---- 7: pins aren't saved ---- */

static void test_host_pins()
{
  IoWorld w;
  const BlockHandle root = w.doc(1, "root"), pinned = w.doc(2, "pinned"),
                    under = w.doc(3, "under");
  const BlockHandle both = w.doc(4, "both"), history = w.doc(5, "history");
  w.root(root);
  w.link(root, both);
  w.link(pinned, under, both);
  w.link(under, both);
  LibraryPin a(w.lib, pinned), b(w.lib, both), c(w.lib, root);
  LibraryPin h(w.lib, history, PinKind::History);
  SaveReport report;
  const Bytes bytes = w.save(&report);
  test_assert(report.blocks == 4 && report.hostPinned.size() == 2);
  test_assert(report.hostPinned[0] == pinned && report.hostPinned[1] == under);
  IoWorld r;
  test_assert(r.load(bytes).ok() && r.lib.size() == 4);
  const Bytes second = r.save(&report);
  test_assert(report.blocks == 2 && report.hostPinned.size() == 0);
  IoWorld again;
  test_assert(again.load(second).ok() && sameBytes(again.save(), second));
  /* Flagged before the save, it stays. */
  w.lib.setFlag(pinned, BlockFlags::FakeUser, true);
  const Bytes kept = w.save(&report);
  test_assert(report.blocks == 4 && report.hostPinned.size() == 0);
  IoWorld k;
  test_assert(k.load(kept).ok() && sameBytes(k.save(), kept));
}

/* ---- 8: where a placeholder came from ---- */

static void test_source()
{
  const BlockId missing = cid(9, kAcme);
  Bytes bytes;
  {
    IoWorld w;
    const BlockHandle a = w.doc(1, "a");
    w.root(a);
    PlaceholderInfo bare;
    bare.reason = PlaceholderReason::MissingBlock;
    test_assert(w.lib.addPlaceholder(missing, bare).ok());
    w.addOptEntry(a, missing);
    PlaceholderInfo stored;
    stored.typeName = "x.Gone";
    stored.sourceName = "stored.cdcc";
    test_assert(w.lib.addPlaceholder(cid(2), stored).ok());
    bytes = w.save();
  }
  const auto source = [](const IoWorld &w, BlockId id) {
    return std::string(w.lib.get(w.lib.find(id))->sourceName().c_str());
  };
  Knows k;
  k.doc = false;
  IoWorld w(k);
  LoadOptions options;
  options.sourceName = "C:/secret/path.cdcc";
  test_assert(w.load(bytes, nullptr, options).ok());
  test_assert(source(w, cid(1)) == "C:/secret/path.cdcc" &&
              source(w, cid(2)) == "stored.cdcc");
  test_assert(source(w, missing) == "C:/secret/path.cdcc");
  test_assert(sameBytes(w.save(), bytes));
  /* Nor does a copy have it, while what was stored is copied. */
  const BlockHandle set[] = {w.at(1), w.at(2)};
  Result<IdMap> ids = duplicateBlocks(w.lib, set);
  test_assert(ids.ok() && source(w, ids->find(cid(1))).empty() &&
              source(w, ids->find(cid(2))) == "stored.cdcc");
  /* Filling clears it, and it doesn't come back with a later save. */
  registerDoc(w.rt, w.docs);
  test_assert(detail::resolvePlaceholders(w.lib).ok() && source(w, cid(1)).empty());
  test_assert(
      w.lib.createWithId(kAcmeDoc, missing, "x", litestl::alloc::New<Doc>("Doc")).ok());
  test_assert(source(w, missing).empty());
  /* Taken back, it is there again. */
  Result<BlockHandle> kept = w.lib.unadopt(w.lib.find(missing));
  test_assert(kept.ok() && source(w, missing) == "C:/secret/path.cdcc");

  IoWorld into(k);
  AppendOptions from;
  from.sourceName = "lib.cdcc";
  Result<AppendResult> r = into.append(bytes, from);
  test_assert(r.ok() && source(into, r->ids.find(cid(1))) == "lib.cdcc");
  const Bytes saved = into.save();
  const std::string text(reinterpret_cast<const char *>(saved.data()),
                         size_t(saved.size()));
  test_assert(!has(text, "lib.cdcc") && has(text, "stored.cdcc"));
}

/* ---- 10: the library is busy while it is written ---- */

static void test_busy()
{
  IoWorld w;
  const BlockHandle a = w.doc(1, "a"), b = w.doc(2, "b");
  w.root(a);
  w.link(a, b);
  w.note(a, 1, w.id(b));
  int calls = 0;
  const auto mutate = [&]() {
    calls++;
    test_assert(w.lib.busy());
    test_assert(w.lib.markDeleted(b).code() == StatusCode::Busy);
    test_assert(w.lib.collect().status().code() == StatusCode::Busy);
    test_assert(w.lib.rename(b, "x").code() == StatusCode::Busy);
    test_assert(saveLibrary(w.lib).status().code() == StatusCode::Busy);
    test_assert(writePayload(w.lib, b).status().code() == StatusCode::Busy);
    const int pins = w.lib.get(b)->pins();
    w.lib.pin(b);
    test_assert(w.lib.get(b)->pins() == pins); /* later */
    test_assert(w.lib.find(cid(2)) == b);
  };
  w.docs.onSerialize = mutate;
  w.notes.onSerialize = mutate;
  SaveReport report;
  const Bytes bytes = w.save(&report);
  test_assert(calls == 3 && report.blocks == 2 && report.issues.total == 0);
  test_assert(!w.lib.busy() && w.lib.get(b)->pins() == 3 &&
              w.lib.get(b)->state() == BlockState::Live);
  Result<BlockPayload> p = writePayload(w.lib, b);
  test_assert(p.ok() && calls == 4 && !w.lib.busy() && w.lib.get(b)->pins() == 4);
  for (int i = 0; i < 4; i++) {
    w.lib.unpin(b);
  }
  w.docs.onSerialize = nullptr;
  w.notes.onSerialize = nullptr;
  test_assert(checkPayloadRefs(w.lib, b, *p).ok() && sameBytes(w.save(), bytes));
  /* A forEachRef too. */
  const BlockHandle t = makeThing(w.lib, "t");
  int seen = 0;
  w.things.onForeach = [&]() {
    seen++;
    test_assert(w.lib.markDeleted(b).code() == StatusCode::Busy);
  };
  test_assert(checkPayloadRefs(w.lib, t, BlockPayload()).ok() && seen == 1);
  w.things.onForeach = nullptr;
  test_assert(!w.lib.busy());
}

/* ---- 12: an attachment's references ---- */

static void test_attachment_refs()
{
  for (bool audit : {true, false}) {
    IoWorld w;
    const BlockHandle a = w.doc(1, "a"), b = w.doc(2, "b");
    w.root(a);
    w.note(a, 1, w.id(b));
    w.lib.setAuditRelations(audit);
    w.notes.skipRef = true;
    SaveReport report;
    Result<Bytes> r = saveLibrary(w.lib, &report);
    const char *what =
        "'a', attachment 'test.Note': forEachRef reports a reference that serialize";
    if (audit) {
      test_assert(r.status().code() == StatusCode::Internal && hasMsg(r.status(), what));
    } else {
      test_assert(r.ok() && report.issues.total == 1 &&
                  has(issueText(report.issues), what));
      test_assert(report.issues.first[0].block == cid(1));
    }
    w.notes.skipRef = false;
    test_assert(saveLibrary(w.lib, &report).ok() && report.issues.total == 0);
    w.lib.setAuditRelations(true);
  }
}

/* ---- 14: an attachment that can't be read goes ---- */

static void test_opaque()
{
  IoWorld w;
  const BlockHandle a = w.doc(1, "a"), b = w.doc(2, "b");
  w.root(a);
  w.note(a, 1, w.id(b));
  const Bytes bytes = w.save();
  Knows k;
  k.note = false;
  IoWorld r(k);
  test_assert(r.load(bytes).ok() && r.lib.hasAttachment(r.at(1), kNote));
  test_assert(r.lib.detachOpaque(r.at(1), kBare).status().code() == StatusCode::NotFound);
  test_assert(r.lib.detachOpaque(BlockHandle(), kNote).status().code() ==
              StatusCode::NotFound);
  test_assert(r.lib.usersOf(r.at(2)).size() == 1);
  Result<OpaqueAttachment> o = r.lib.detachOpaque(r.at(1), kNote);
  test_assert(o.ok() && strcmp(o->typeName.c_str(), "test.Note") == 0 &&
              o->payload.refs.size() == 1);
  test_assert(!r.lib.hasAttachment(r.at(1), kNote) && r.lib.usersOf(r.at(2)).size() == 0);
  test_assert(r.lib.detachOpaque(r.at(1), kNote).status().code() == StatusCode::NotFound);
  test_assert(r.lib.attachOpaque(r.at(1), *o).ok() && r.lib.usersOf(r.at(2)).size() == 1);
  test_assert(sameBytes(r.save(), bytes));
  /* Busy, as every change. */
  r.docs.onSerialize = [&]() {
    test_assert(r.lib.detachOpaque(r.at(1), kNote).status().code() == StatusCode::Busy);
    test_assert(r.lib.attachOpaque(r.at(1), *o).code() == StatusCode::Busy);
  };
  r.save();
  r.docs.onSerialize = nullptr;
  /* Read later, the relations follow (34). */
  registerNote(r.rt, r.notes);
  const uint64_t epoch = r.lib.epoch();
  Result<LoadReport> resolved = detail::resolvePlaceholders(r.lib);
  test_assert(resolved.ok() && resolved->blocks.size() == 1 && r.lib.epoch() > epoch);
  test_assert(r.lib.attachment(r.at(1), kNote));
}

/* ---- 15: ids that a file chose ---- */

static void test_hostile_names()
{
  constexpr uint32_t kNames = 40000;
  /* Namespaces that differ in the high half alone. */
  BytesWriter n, b, d;
  n.u32(kNames + 2);
  n.u8(0);
  n.str("coredcc");
  n.u8(0);
  n.str("x.Type");
  for (uint32_t i = 0; i < kNames; i++) {
    n.u8(1);
    n.u64(uint64_t(i + 1) << 32 | 5);
  }
  RawRecord r;
  r.slots = [](BytesWriter &out) {
    out.u32(kNames);
    for (uint32_t i = 0; i < kNames; i++) {
      out.u32(3 + i);
      out.i64(0);
      RawRecord::row(out, 2, LocalId());
    }
  };
  b.u32(1);
  r.write(b);
  d.u32(0);
  FileBuilder f;
  f.chunk(detail::libio::kTagName, n.take());
  f.chunk(detail::libio::kTagBlocks, b.take());
  f.chunk(detail::libio::kTagData, d.take());
  IoWorld w;
  test_assert(w.load(f.build()).ok());
  const Watch watch;
  test_assert(w.save().size() > kNames);
  test_assert(watch.seconds() < 3);

  const Watch names;
  NameTable table;
  for (uint32_t i = 0; i < kNames; i++) {
    test_assert(table.debugAddAs(IdDomain::Type, uint64_t(i + 1) << 32 | 5, "name"));
  }
  test_assert(table.nameOf(IdDomain::Type, 1ull << 32 | 5) &&
              !table.nameOf(IdDomain::Type, 1ull << 33 | 6));
  test_assert(names.seconds() < 3);
}

/* ---- 17: a value is read as it was written, or kept ---- */

static void test_ranges()
{
  IoWorld w;
  PropertyBag bag;
  test_assert(bag.add(PropertyDef::floatProp("f").range(0, 10)).ok());
  test_assert(bag.add(PropertyDef::intProp("i").range(0, 10)).ok());
  test_assert(bag.add(PropertyDef::vecProp("v", 3).range(0, 10)).ok());
  test_assert(bag.add(PropertyDef::boolProp("b")).ok());
  test_assert(bag.add(PropertyDef::floatProp("wide")).ok());
  const auto read = [&](std::initializer_list<std::pair<const char *, PropValue>> values,
                        Vector<BlockEntry> &unread) {
    BlockWriter out(w.lib);
    Vector<BlockEntry> entries;
    for (const auto &v : values) {
      entries.append(entryOf(v.first, v.second));
    }
    out.entries(std::span<const BlockEntry>(entries.data(), size_t(entries.size())));
    Result<BlockPayload> p = PayloadAccess::take(out, 1);
    PropertyBag to = bag;
    BagRefs refs;
    BlockReader in(w.lib, *p);
    in.bag(to, refs, unread);
    test_assert(in.bytes().ok());
    return to;
  };
  const auto keeps = [&](const char *name, PropValue v) {
    Vector<BlockEntry> unread;
    const PropertyBag b = read({{name, v}}, unread);
    return unread.size() == 1 && unread[0].value == v && !b.isSet(name);
  };
  const auto takes = [&](const char *name, PropValue v, PropValue as) {
    Vector<BlockEntry> unread;
    const PropertyBag b = read({{name, v}}, unread);
    return unread.size() == 0 && b.isSet(name) && *b.get(name) == as;
  };
  test_assert(keeps("f", PropValue::ofFloat(50)) && keeps("f", PropValue::ofFloat(-1)));
  test_assert(keeps("i", PropValue::ofInt(11)) && keeps("i", PropValue::ofFloat(11)));
  test_assert(keeps("f", PropValue::ofInt(11)) && keeps("i", PropValue::ofFloat(1.5)));
  test_assert(keeps("v", PropValue::ofVec3(1, 2, 30)));
  test_assert(takes("f", PropValue::ofFloat(5), PropValue::ofFloat(5)));
  test_assert(takes("f", PropValue::ofInt(5), PropValue::ofFloat(5)));
  test_assert(takes("i", PropValue::ofFloat(5), PropValue::ofInt(5)));
  test_assert(takes("b", PropValue::ofInt(1), PropValue::ofBool(true)));
  test_assert(takes("v", PropValue::ofVec3(1, 2, 3), PropValue::ofVec3(1, 2, 3)));
  /* A number that a double can't hold. */
  test_assert(keeps("wide", PropValue::ofInt((int64_t(1) << 53) + 1)));
  test_assert(keeps("wide", PropValue::ofInt(INT64_MAX)));
  test_assert(takes("wide", PropValue::ofInt(INT64_MIN), PropValue::ofFloat(-0x1p63)) ||
              keeps("wide", PropValue::ofInt(INT64_MIN)));
  /* What an earlier entry of the name set stays. */
  Vector<BlockEntry> unread;
  const PropertyBag b =
      read({{"f", PropValue::ofFloat(3)}, {"f", PropValue::ofFloat(50)}}, unread);
  test_assert(unread.size() == 1 && *b.get("f") == PropValue::ofFloat(3) && b.isSet("f"));

  /* Through a file, which a build without the range could have written. */
  Bytes bytes;
  {
    IoWorld from;
    const BlockHandle a = from.doc(1, "a");
    from.root(a);
    from.data(a)->unread.append(entryOf("size", PropValue::ofFloat(50)));
    bytes = from.save();
  }
  IoWorld r;
  test_assert(r.load(bytes).ok() && r.data(r.at(1))->unread.size() == 1);
  test_assert(!r.data(r.at(1))->bag.isSet("size") && sameBytes(r.save(), bytes));
}

/* ---- 26: what loading it takes ---- */

static void test_load_cost()
{
  for (int large = 0; large < 2; large++) {
    IoWorld w;
    const std::string name(large ? 3000 : 5, 'n');
    for (uint64_t i = 1; i <= (large ? 40u : 2u); i++) {
      const BlockHandle h = w.doc(i, name.c_str(), i % 3 ? kDoc : kAcmeDoc);
      w.root(h);
      w.data(h)->mesh.target = cid(500 + i % 7, kAcme); /* missing */
      w.note(h, 1, cid(600));
      test_assert(w.lib.setSlot(h, kNote, 2, cid(700 + i), RefKind::Strong).ok());
      test_assert(w.lib.setSlot(h, kNote, 3, cid(800 + i), RefKind::Weak).ok());
    }
    test_assert(
        w.lib.setSlot(w.at(1), kNote, 4, cid(2), RefKind::Strong).ok()); /* not missing */
    PlaceholderInfo info;
    info.typeName = "x.Gone";
    info.sourceName = "somewhere";
    for (int i = 0; i < 5000; i++) {
      info.payload.bytes.append(uint8_t(i % 5));
    }
    test_assert(w.lib.addPlaceholder(cid(300), info).ok());
    OpaqueAttachment o;
    o.typeName =
        ("x." + std::string(large ? 900 : 5, 'o')).c_str(); /* names compress too */
    o.payload.bytes = bytesOf({1, 2, 3});
    test_assert(w.lib.attachOpaque(w.lib.find(cid(300)), o).ok());
    w.lib.noteRefsChanged();
    SaveReport report;
    const Bytes bytes = w.save(&report);
    LoadOptions options;
    options.limits.maxBytes = report.loadBytes;
    options.limits.maxBlocks = uint32_t(report.loadBlocks);
    {
      IoWorld r;
      test_assert(r.load(bytes, nullptr, options).ok() &&
                  int64_t(r.lib.size()) == report.loadBlocks);
    }
    {
      IoWorld r;
      options.limits.maxBytes--;
      test_assert(r.load(bytes, nullptr, options).code() == StatusCode::OutOfRange);
    }
    IoWorld r;
    options.limits.maxBytes++;
    options.limits.maxBlocks--;
    test_assert(r.load(bytes, nullptr, options).code() == StatusCode::OutOfRange);
  }
  RawFile f; /* with fields of a newer build */
  RawRecord raw;
  raw.extension = std::string("\5\0\0\0\20\0\0\0", 8) + std::string(16, 'x');
  f.records.push_back(raw);
  IoWorld r;
  test_assert(r.load(f.file()).ok());
  SaveReport report;
  const Bytes bytes = r.save(&report);
  LoadOptions options;
  options.limits.maxBytes = report.loadBytes;
  IoWorld a, b;
  test_assert(a.load(bytes, nullptr, options).ok());
  options.limits.maxBytes--;
  test_assert(b.load(bytes, nullptr, options).code() == StatusCode::OutOfRange);
}

/* ---- 29: a failed duplicate frees what it made ---- */

static void test_duplicate_fails()
{
  for (DocFault fault :
       {DocFault::LateSticky, DocFault::NoData, DocFault::Error, DocFault::Sticky})
  {
    IoWorld w;
    const BlockHandle a = w.doc(1, "a"), b = w.doc(2, "b");
    w.link(a, b);
    bool severed = false;
    w.docs.onDestroy = [&](const Datablock &d) {
      const Doc *made = static_cast<const Doc *>(d.clientData());
      severed = made && made->mesh.isNull();
    };
    w.docs.fault = fault;
    const BlockHandle set[] = {a};
    Status s = duplicateBlocks(w.lib, set).status();
    test_assert(!s.ok() && w.lib.size() == 2 && !w.rt.hasStickyError());
    test_assert(w.docs.destroyed == (fault == DocFault::LateSticky ? 1 : 0));
    test_assert(severed == (fault == DocFault::LateSticky));
    test_assert(fault != DocFault::LateSticky || hasMsg(s, "threw late"));
    test_assert(w.valid() && !w.lib.busy());
    w.docs.fault = DocFault::None;
    w.docs.onDestroy = nullptr;
    test_assert(duplicateBlocks(w.lib, set).ok());
  }
}

/* ---- 30: a client gives its block the new id ---- */

static void test_alias_loser()
{
  RawFile f;
  f.names = {"old", "acme.Doc", "acme"};
  for (uint32_t client : {1u, 3u}) {
    RawRecord r;
    r.client = client;
    r.as = 1;
    r.name = client == 1 ? "loser" : "winner";
    f.records.push_back(r);
  }
  const Bytes bytes = f.file();
  for (int answer = 0; answer < 4; answer++) {
    Knows k;
    k.acme = false;
    IoWorld w(k);
    const char *aliases[] = {"old"};
    test_assert(w.rt.ext<ClientRegistry>().add("acme", aliases).ok());
    registerDoc(w.rt, w.docs, 1, "acme.Doc", kAcme);
    int asked = 0;
    w.docs.onRekey = [&](const Datablock &b, const RekeyContext &c) {
      asked++;
      test_assert(b.id() == cid(1, kAcme) && strcmp(b.name().c_str(), "loser") == 0);
      test_assert(!c.isFree(cid(1, kAcme)) && c.isFree(cid(99, kAcme)));
      if (answer == 3) {
        w.rt.setStickyError(Status::error(StatusCode::Internal, "rekey threw"));
      }
      return answer == 0 ? cid(99, kAcme) : answer == 1 ? cid(1, kAcme) : BlockId();
    };
    LoadReport report;
    test_assert(w.load(bytes, &report).ok() && asked == 1 && w.lib.size() == 2);
    w.docs.onRekey = nullptr;
    const BlockId got = w.id(report.blocks[0]);
    const std::string issues = issueText(report.issues);
    test_assert(got.client == kAcme && w.id(report.blocks[1]) == cid(1, kAcme));
    test_assert((got == cid(99, kAcme)) == (answer == 0));
    test_assert(has(issues, answer == 0 ? "it got a new one" : "a random one"));
    test_assert(has(issues, "rekey threw") == (answer == 3) && !w.rt.hasStickyError());
  }
}

/* ---- 34 ---- */

/* An alias that came since the load is taken in before an append starts. */
static void test_append_alias()
{
  IoWorld from;
  from.root(from.doc(1, "a"));
  const Bytes bytes = from.save();
  IoWorld w;
  for (const char *client : {"old", "now"}) {
    PlaceholderInfo info;
    info.typeName = "x.Type";
    test_assert(w.lib.addPlaceholder(cid(1, ClientId::of(client)), info).ok());
  }
  const char *aliases[] = {"old"};
  test_assert(w.rt.ext<ClientRegistry>().add("now", aliases).ok());
  /* Two records have one id now. */
  Result<AppendResult> r = w.append(bytes);
  test_assert(r.status().code() == StatusCode::FailedPrecondition && w.lib.size() == 2);
  test_assert(hasMsg(r.status(), "a sticky error is pending"));
  test_assert(w.rt.takeStickyError().code() == StatusCode::AlreadyExists);
  test_assert(w.append(bytes).ok());
  /* (validate() would say so: one of them goes.) */
  for (BlockHandle h : r.ok() ? r->report.blocks : Vector<BlockHandle>()) {
    test_assert(w.lib.get(h));
  }
}

static void test_client_names()
{
  const std::string one = "DfOQuVnD5x9129oU",
                    other = "AjGW269UOJAMwZyS"; /* one FNV-1a hash */
  test_assert(ClientId::of(one) == ClientId::of(other));
  RawFile f;
  f.names = {one, "x.Type", other};
  for (uint32_t client : {1u, 3u}) {
    RawRecord r;
    r.client = client;
    r.local.lo = client;
    f.records.push_back(r);
  }
  IoWorld w;
  Status s = w.load(f.file());
  test_assert(s.code() == StatusCode::InvalidArgument && hasMsg(s, "has the id of"));
  /* Against a registered client's. */
  f.records.pop_back();
  IoWorld known;
  test_assert(known.rt.ext<ClientRegistry>().add(other).ok());
  s = known.load(f.file());
  test_assert(s.code() == StatusCode::InvalidArgument &&
              hasMsg(s, "has the id of 'AjGW"));
}

static void test_probe()
{
  test_assert(detail::libio::probeMemory(1 << 20).ok());
  test_assert(detail::libio::probeMemory(UINT64_MAX / 2).code() ==
              StatusCode::OutOfRange);
}

int main()
{
  test_attach();
  test_storage();
  test_transient();
  test_host_pins();
  test_source();
  test_busy();
  test_attachment_refs();
  test_opaque();
  test_hostile_names();
  test_ranges();
  test_load_cost();
  test_duplicate_fails();
  test_alias_loser();
  test_append_alias();
  test_client_names();
  test_probe();
  return test_end();
}
