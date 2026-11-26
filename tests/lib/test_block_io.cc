/* Block payloads: round trips, unknown data kept, the ref audit, errors, hostile input.
 */
#include "coredcc/lib/block_io.h"

#include "doc_fixture.h"
#include "fuzz.h"

#include <chrono>

test_init;

using namespace coredcc;
using namespace coredcc_test;

namespace {

struct World {
  ThingHost things;
  DocHost docs;
  Runtime rt;
  Library lib{&rt};
  World()
  {
    registerThing(rt, things);
    registerDoc(rt, docs);
    lib.setAuditRelations(true);
  }
  BlockId id(BlockHandle h) const
  {
    return lib.get(h)->id();
  }
  bool valid() const
  {
    return lib.validate().ok() && rt.validateAll().ok() && !rt.hasStickyError();
  }
};

bool sameCell(const RefCell &a, const RefCell &b)
{
  return a.target == b.target && a.kind == b.kind;
}

bool sameEntries(const Vector<BlockEntry> &a, const Vector<BlockEntry> &b)
{
  bool same = a.size() == b.size();
  for (int i = 0; same && i < int(a.size()); i++) {
    same = strcmp(a[i].name.c_str(), b[i].name.c_str()) == 0 &&
           a[i].value == b[i].value && a[i].unknownTag == b[i].unknownTag &&
           a[i].refs.size() == b[i].refs.size() && a[i].raw.size() == b[i].raw.size();
    for (int k = 0; same && k < int(a[i].refs.size()); k++) {
      same = sameCell(a[i].refs[k], b[i].refs[k]);
    }
    for (int k = 0; same && k < int(a[i].raw.size()); k++) {
      same = a[i].raw[k] == b[i].raw[k];
    }
  }
  return same;
}

bool samePayload(const BlockPayload &a, const BlockPayload &b)
{
  bool same = a.version == b.version && a.bytes.size() == b.bytes.size() &&
              a.refs.size() == b.refs.size();
  for (int i = 0; same && i < int(a.bytes.size()); i++) {
    same = a.bytes[i] == b.bytes[i];
  }
  for (int i = 0; same && i < int(a.refs.size()); i++) {
    same = sameCell(a.refs[i], b.refs[i]);
  }
  return same;
}

/* A Doc with something of everything. */
BlockHandle makeFull(World &w, BlockHandle a, BlockHandle b)
{
  const BlockHandle h = makeDoc(w.lib, w.docs, "full");
  Doc *d = doc(w.lib, h);
  d->number = -7;
  test_assert(w.lib.writeRef(d->parent, w.id(a)).ok());
  test_assert(w.lib.writeRef(d->mesh, w.id(b)).ok());
  d->entries.append(entryOf("n", PropValue::ofInt(3)));
  d->entries.append(entryOf("p", pointerTo(w.id(b), "faces"), RefKind::Strong));
  d->entries.append(entryOf("null", PropValue::nullPointer()));
  d->entries.append(entryOf("weak", pointerTo(w.id(a)), RefKind::Weak));
  test_assert(d->bag.set("size", PropValue::ofFloat(2)).ok());
  test_assert(d->bagRefs.set(d->bag, "target", pointerTo(w.id(a))).ok());
  test_assert(d->bag.set("extra", PropValue::ofString("\xff")).ok());
  w.lib.noteRefsChanged();
  return h;
}

} // namespace

static void test_round_trip()
{
  World w;
  const BlockHandle a = makeThing(w.lib, "a"), b = makeThing(w.lib, "b");
  const BlockHandle h = makeFull(w, a, b);
  Result<BlockPayload> p = writePayload(w.lib, h);
  test_assert(p.ok() && p->version == 3);
  /* parent, mesh, then p, weak and the bag's target; the null pointer has no row. */
  test_assert(p->refs.size() == 5);
  test_assert(p->refs[0].target == w.id(a) && p->refs[0].kind == RefKind::Weak);
  test_assert(p->refs[1].target == w.id(b) && p->refs[1].kind == RefKind::Strong);
  test_assert(p->refs[2].kind == RefKind::Strong &&
              p->refs[4].kind == RefKind::OptStrong);

  const BlockHandle back = emptyDoc(w.lib, "back");
  const uint64_t epoch = w.lib.epoch();
  test_assert(readPayload(w.lib, back, *p).ok() && w.lib.epoch() > epoch);
  const Doc *d = doc(w.lib, h), *e = doc(w.lib, back);
  test_assert(e && e->number == -7 && sameCell(d->parent, e->parent) &&
              sameCell(d->mesh, e->mesh));
  test_assert(sameEntries(d->entries, e->entries) && e->unread.size() == 0);
  for (int i = 0; i < d->bag.size(); i++) {
    test_assert(d->bag.value(i) == e->bag.value(i) &&
                d->bag.wasSet(i) == e->bag.wasSet(i));
  }
  test_assert(w.docs.lastVersion == 3 && w.lib.usersOf(b).size() == 2);
  Result<BlockPayload> again = writePayload(w.lib, back);
  test_assert(again.ok() && samePayload(*p, *again));

  /* References to deleted blocks are kept; null ones are rows too. */
  test_assert(w.lib.markDeleted(a).ok());
  doc(w.lib, h)->mesh.target = BlockId();
  w.lib.noteRefsChanged();
  Result<BlockPayload> del = writePayload(w.lib, h);
  test_assert(del.ok() && del->refs.size() == 5 && del->refs[0].target == w.id(a) &&
              del->refs[1].isNull());
  test_assert(w.lib.markDeleted(h).ok() && writePayload(w.lib, h).ok()); /* InHistory */
  test_assert(w.valid());
}

static void test_golden()
{
  World w;
  const BlockHandle h = makeDoc(w.lib, w.docs, "g");
  Doc *d = doc(w.lib, h);
  d->number = 1;
  d->entries.append(
      entryOf("p", pointerTo(BlockId{ClientId::of(kCoredccClient), {1, 2}}, "v")));
  Result<BlockPayload> p = writePayload(w.lib, h);
  const uint64_t mesh = StructId::of("acme.Mesh").value;
  uint8_t golden[] = {
      1,  0, 0, 0, 0,   0,  0, 0,         /* number */
      0,  0, 0, 0, 1,   0,  0, 0,         /* parent, mesh: rows 0 and 1 */
      1,  0, 0, 0,                        /* one entry */
      1,  0, 0, 0, 'p', 12,               /* its name, the Pointer tag */
      1,  0, 0, 0, 2,   0,  0, 0,         /* one ref: row 2 */
      24, 0, 0, 0,                        /* the body */
      1,  0, 0, 0, 0,                     /* a block: the entry's ref 0 */
      1,  0, 0, 0, 0,   1,  0, 0, 0, 'v', /* one member segment */
      2,  0, 0, 0, 0,   0,  0, 0, 0,      /* the struct: a raw id */
      0,  0, 0, 0,                        /* an empty bag */
  };
  for (int i = 0; i < 8; i++) {
    golden[sizeof(golden) - 12 + i] = uint8_t(mesh >> (8 * i));
  }
  test_assert(p.ok() && size_t(p->bytes.size()) == sizeof(golden));
  test_assert(p.ok() && memcmp(p->bytes.data(), golden, sizeof(golden)) == 0);
}

/* An entry of a type this build doesn't know, and entries no def takes: kept, with refs.
 */
static void test_unknown_kept()
{
  World w;
  const BlockHandle a = makeThing(w.lib, "a"), b = makeThing(w.lib, "b");
  const uint8_t future[] = {9, 8, 0, 7};
  const auto rowOf = [](BlockId id, RefKind kind) {
    RefCell c(kind);
    c.target = id;
    return c;
  };
  BlockWriter out(w.lib);
  out.bytes().i64(5);
  out.ref(RefCell(RefKind::Strong)); /* `parent` was strong in an older version */
  out.ref(RefCell(RefKind::Strong));
  BlockEntry unknown;
  unknown.name = "future";
  unknown.unknownTag = 200;
  unknown.refs.append(rowOf(w.id(a), RefKind::Strong));
  unknown.refs.append(rowOf(w.id(b), RefKind::Weak));
  for (uint8_t x : future) {
    unknown.raw.append(x);
  }
  out.entry(unknown);
  /* The bag: the same unknown entry, one without a def, one that doesn't coerce. */
  Vector<BlockEntry> bag;
  bag.append(unknown);
  bag.append(entryOf("gone", pointerTo(w.id(b))));
  bag.append(entryOf("size", PropValue::ofString("huge")));
  bag.append(entryOf("extra", PropValue::ofString("kept")));
  out.entries(std::span<const BlockEntry>(bag.data(), size_t(bag.size())));
  Result<BlockPayload> p = PayloadAccess::take(out, 3);
  test_assert(p.ok());

  const BlockHandle h = emptyDoc(w.lib, "h");
  test_assert(readPayload(w.lib, h, *p).ok());
  Doc *d = doc(w.lib, h);
  test_assert(d->parent.kind == RefKind::Weak); /* the cell keeps its own kind */
  test_assert(d->entries[0].raw.size() == 4 &&
              memcmp(d->entries[0].raw.data(), future, 4) == 0);
  test_assert(d->entries.size() == 1 && d->entries[0].value.isNone());
  test_assert(d->entries[0].unknownTag == 200 && d->entries[0].refs.size() == 2);
  test_assert(d->unread.size() == 3 && d->unread[0].unknownTag == 200);
  test_assert(d->unread[1].value == bag[1].value && d->unread[1].refs.size() == 1);
  test_assert(*d->bag.get("extra") == PropValue::ofString("kept") &&
              !d->bag.isSet("size"));
  /* Their refs are the block's refs. */
  test_assert(w.lib.usersOf(a).size() == 1 && w.lib.usersOf(b).size() == 1);

  /* Written again: the same bytes and rows, and the audit agrees. */
  Result<BlockPayload> again = writePayload(w.lib, h);
  test_assert(again.ok() && again->refs.size() == p->refs.size());
  const BlockHandle h2 = emptyDoc(w.lib, "h2");
  test_assert(readPayload(w.lib, h2, *again).ok());
  test_assert(sameEntries(doc(w.lib, h2)->unread, d->unread));
  test_assert(sameEntries(doc(w.lib, h2)->entries, d->entries));

  /* Remapping the refs remaps what is written. */
  d->unread[1].refs[0].target = w.id(a);
  w.lib.noteRefsChanged();
  Result<BlockPayload> moved = writePayload(w.lib, h);
  const BlockHandle h3 = emptyDoc(w.lib, "h3");
  test_assert(moved.ok() && readPayload(w.lib, h3, *moved).ok());
  test_assert(doc(w.lib, h3)->unread[1].value == pointerTo(w.id(a)));

  /* A value the bag has since set wins over the unread one of that name. */
  test_assert(d->bag.set("size", PropValue::ofFloat(4)).ok());
  Result<BlockPayload> set = writePayload(w.lib, h);
  const BlockHandle h4 = emptyDoc(w.lib, "h4");
  test_assert(set.ok() && readPayload(w.lib, h4, *set).ok());
  test_assert(*doc(w.lib, h4)->bag.get("size") == PropValue::ofFloat(4));

  /* A build without `extra`, then one with it again. */
  w.docs.bagHasExtra = false;
  const BlockHandle old = emptyDoc(w.lib, "old");
  test_assert(readPayload(w.lib, old, *p).ok() && doc(w.lib, old)->unread.size() == 4);
  Result<BlockPayload> viaOld = writePayload(w.lib, old);
  w.docs.bagHasExtra = true;
  const BlockHandle h5 = emptyDoc(w.lib, "h5");
  test_assert(viaOld.ok() && readPayload(w.lib, h5, *viaOld).ok());
  test_assert(*doc(w.lib, h5)->bag.get("extra") == PropValue::ofString("kept"));
  test_assert(w.valid());
}

static void test_writer_refuses()
{
  World w;
  const BlockHandle a = makeThing(w.lib, "a");
  {
    BlockWriter out(w.lib);
    BlockEntry e;
    e.name = "p";
    e.value = pointerTo(w.id(a)); /* no ref */
    out.entry(e);
    test_assert(hasMsg(PayloadAccess::take(out, 1).status(), "no ref"));
  }
  {
    BlockWriter out(w.lib);
    BlockEntry e;
    e.unknownTag = uint8_t(ValueTag::Int);
    out.entry(e);
    test_assert(hasMsg(PayloadAccess::take(out, 1).status(), "known type"));
  }
}

/* Damage inside an entry fails the read. */
static void test_bad_entries()
{
  World w;
  const auto readEntry = [&](uint8_t tag, std::initializer_list<int> body) {
    BlockWriter out(w.lib);
    out.bytes().u32(1);
    out.bytes().str("e");
    out.bytes().u8(tag);
    out.bytes().u32(0);
    const size_t at = out.bytes().beginSection();
    for (int b : body) {
      out.bytes().u8(uint8_t(b));
    }
    out.bytes().endSection(at);
    Result<BlockPayload> p = PayloadAccess::take(out, 1);
    test_assert(p.ok());
    BlockReader in(w.lib, *p);
    Vector<BlockEntry> entries;
    in.entries(entries);
    test_assert(in.bytes().ok() == (entries.size() == 1));
    return Status(in.bytes().status());
  };
  const uint8_t boolean = uint8_t(ValueTag::Bool);
  test_assert(readEntry(boolean, {1}).ok());
  test_assert(hasMsg(readEntry(boolean, {1, 1}), "left over"));
  test_assert(hasMsg(readEntry(boolean, {2}), "boolean"));
  test_assert(
      hasMsg(readEntry(uint8_t(ValueTag::Pointer), {1, 0, 0, 0, 0}), "ref it doesn't"));
  BlockWriter out(w.lib);
  out.bytes().u32(0xffffffffu);
  Result<BlockPayload> p = PayloadAccess::take(out, 1);
  test_assert(p.ok());
  for (int bag = 0; bag < 2; bag++) {
    BlockReader in(w.lib, *p);
    Vector<BlockEntry> entries;
    PropertyBag values;
    if (bag) {
      BagRefs refs;
      in.bag(values, refs, entries);
    } else {
      in.entries(entries);
    }
    test_assert(hasMsg(in.bytes().status(), "a count of more") && entries.size() == 0);
  }
}

static void test_audit()
{
  World w;
  const BlockHandle a = makeThing(w.lib, "a"), b = makeThing(w.lib, "b");
  const BlockHandle h = makeFull(w, a, b);
  struct Case {
    DocFault fault;
    const char *msg;
  };
  const Case cases[] = {{DocFault::SkipRef, "didn't write"},
                        {DocFault::ExtraRef, "doesn't report"},
                        {DocFault::WrongStrength, "strength"}};
  for (const Case &c : cases) {
    if (c.fault != DocFault::ExtraRef) {
      /* The other rows name the same blocks: cut them, so one row decides. */
      Doc *d = doc(w.lib, h);
      d->entries.clear();
      test_assert(d->bagRefs.reset(d->bag, "target").ok());
      w.lib.noteRefsChanged();
    }
    w.docs.fault = c.fault;
    Result<BlockPayload> p = writePayload(w.lib, h);
    test_assert(p.status().code() == StatusCode::Internal && hasMsg(p.status(), c.msg));
    /* Without the audit it is written, and a save can still report it. */
    w.lib.setAuditRelations(false);
    p = writePayload(w.lib, h);
    test_assert(p.ok() && !checkPayloadRefs(w.lib, h, *p).ok());
    w.lib.setAuditRelations(true);
    w.docs.fault = DocFault::None;
    p = writePayload(w.lib, h);
    test_assert(p.ok() && checkPayloadRefs(w.lib, h, *p).ok());
  }
  /* Slots and attachments aren't the provider's to write. */
  test_assert(w.lib.setSlot(h, kDoc, 1, w.id(b), RefKind::Weak).ok());
  test_assert(writePayload(w.lib, h).ok() && w.valid());
}

static void test_errors()
{
  World w;
  const BlockHandle a = makeThing(w.lib, "a"), b = makeThing(w.lib, "b");
  const BlockHandle h = makeFull(w, a, b);
  Result<BlockPayload> p = writePayload(w.lib, h);
  test_assert(p.ok());
  const auto writeCode = [&](BlockHandle x) {
    return writePayload(w.lib, x).status().code();
  };
  const auto readCode = [&](BlockHandle x) { return readPayload(w.lib, x, *p).code(); };

  test_assert(writeCode(a) == StatusCode::Unsupported); /* Thing has no serialize */
  test_assert(writeCode(BlockHandle()) == StatusCode::NotFound);
  test_assert(readCode(BlockHandle()) == StatusCode::NotFound);
  test_assert(readCode(h) == StatusCode::FailedPrecondition); /* has data */
  Result<BlockHandle> bare = w.lib.create(kThing, "bare", nullptr);
  test_assert(bare.ok() && readCode(*bare) == StatusCode::Unsupported);
  const BlockHandle gone = emptyDoc(w.lib, "gone");
  test_assert(w.lib.onHostDestroyed(w.id(gone)).ok());
  test_assert(readCode(gone) == StatusCode::FailedPrecondition &&
              writeCode(gone) == StatusCode::FailedPrecondition);
  w.rt.setStickyError(Status::error(StatusCode::Internal, "mine"));
  test_assert(writeCode(h) == StatusCode::FailedPrecondition &&
              readCode(gone) == StatusCode::FailedPrecondition);
  test_assert(hasMsg(w.rt.takeStickyError(), "mine"));

  /* Busy inside a sweep, also from a provider. */
  bool busy = false;
  w.things.onDestroy = [&](const Datablock &) {
    busy = writeCode(h) == StatusCode::Busy;
  };
  w.lib.setFlag(h, BlockFlags::Root, true);
  makeThing(w.lib, "garbage");
  test_assert(w.lib.collect().ok() && busy);
  w.things.onDestroy = {};

  w.docs.fault = DocFault::Error;
  test_assert(writeCode(h) == StatusCode::Unsupported &&
              readCode(emptyDoc(w.lib, "e")) == StatusCode::Unsupported);
  w.docs.fault = DocFault::Sticky;
  test_assert(hasMsg(writePayload(w.lib, h).status(), "serialize threw"));
  test_assert(hasMsg(readPayload(w.lib, emptyDoc(w.lib, "e"), *p), "deserialize threw"));
  test_assert(!w.rt.hasStickyError());
  w.docs.fault = DocFault::None;

  /* Versions: older is read, newer isn't. */
  p->version = 2;
  test_assert(readPayload(w.lib, emptyDoc(w.lib, "old"), *p).ok() &&
              w.docs.lastVersion == 2);
  p->version = 4;
  test_assert(readCode(emptyDoc(w.lib, "new")) == StatusCode::Unsupported);
  p->version = 3;

  /* What ok from deserialize doesn't cover: the data is destroyed, the record stays. */
  const BlockHandle e = emptyDoc(w.lib, "e");
  w.docs.destroyed = 0;
  bool severed = false;
  w.docs.onDestroy = [&](const Datablock &block) {
    const Doc *d = static_cast<const Doc *>(block.clientData());
    severed = d && d->parent.isNull();
    const bool before = w.lib.get(h)->hasFlag(BlockFlags::FakeUser);
    w.lib.setFlag(h, BlockFlags::FakeUser, !before); /* deferred, as in a sweep */
    test_assert(w.lib.get(h)->hasFlag(BlockFlags::FakeUser) == before);
  };
  w.docs.fault = DocFault::LeaveBytes;
  test_assert(hasMsg(readPayload(w.lib, e, *p), "left over"));
  test_assert(w.docs.destroyed == 1 && w.lib.get(e) && !w.lib.get(e)->clientData());
  test_assert(w.lib.get(h)->hasFlag(BlockFlags::FakeUser));
  w.docs.fault = DocFault::NoData;
  test_assert(hasMsg(readPayload(w.lib, e, *p), "setData") && w.docs.destroyed == 1);
  w.docs.fault = DocFault::None;
  severed = false;
  BlockPayload cut = *p;
  cut.bytes.resize(size_t(cut.bytes.size()) - 1);
  test_assert(hasMsg(readPayload(w.lib, e, cut), "at byte") && w.docs.destroyed == 2);
  test_assert(severed); /* its refs had been read */
  w.docs.onDestroy = {};

  /* Nothing is collected under a deserialize. */
  StatusCode inner = StatusCode::Ok;
  w.docs.onDeserialize = [&](BlockReader &in) {
    inner = in.library().collect().status().code();
  };
  test_assert(readPayload(w.lib, e, *p).ok() && inner == StatusCode::Busy);
  w.docs.onDeserialize = {};
  test_assert(doc(w.lib, e)->number == -7);
  w.lib.setFlag(h, BlockFlags::FakeUser, false);
  test_assert(w.lib.collect().ok() && w.valid());
}

static void test_names()
{
  /* The struct's name travels in the file's table, not the Runtime. */
  DocHost host;
  BlockPayload p;
  {
    Runtime rt;
    test_assert(rt.registerId<IdDomain::Struct>("acme.Mesh").ok());
    registerDoc(rt, host);
    Library lib(&rt);
    const BlockHandle h = makeDoc(lib, host, "h");
    doc(lib, h)->entries.append(entryOf("p", pointerTo(lib.get(h)->id())));
    Result<BlockPayload> r = writePayload(lib, h);
    test_assert(r.ok());
    p = *r;
  }
  Runtime rt;
  registerDoc(rt, host);
  Library lib(&rt);
  const BlockHandle h = emptyDoc(lib, "h");
  test_assert(readPayload(lib, h, p).ok());
  const StructId mesh = StructId::of("acme.Mesh");
  test_assert(!rt.nameOf(mesh));
  test_assert(strcmp(lib.fileNames().nameOf(IdDomain::Struct, mesh.value), "acme.Mesh") ==
              0);
  Result<BlockPayload> again = writePayload(lib, h);
  test_assert(again.ok() && samePayload(p, *again));
}

/* ---- duplicate, rekey, the registry's rules ---- */

static void test_other_slots()
{
  World w;
  const BlockHandle a = makeThing(w.lib, "a"), b = makeThing(w.lib, "b");
  const BlockHandle h = makeFull(w, a, b);
  BlockProvider p = w.rt.ext<BlockTypeRegistry>().provider(kDoc);
  test_assert(p.hasSerialize() && p.hasDeserialize() && p.hasDuplicate() && p.hasRekey());
  BlockCopy copy;
  test_assert(p.duplicate(*w.lib.get(h), copy).ok());
  Doc *d = static_cast<Doc *>(PayloadAccess::data(copy));
  test_assert(d && d != doc(w.lib, h) && sameEntries(d->entries, doc(w.lib, h)->entries));
  test_assert(d && d->mesh.target == w.id(b));
  litestl::alloc::Delete(d);
  const BlockId re = p.rekey(*w.lib.get(h), RekeyContext(w.lib, nullptr, nullptr));
  test_assert(re.local.hi == (w.id(h).local.hi ^ 1) && re.local.lo == w.id(h).local.lo);
  BlockProvider t = w.rt.ext<BlockTypeRegistry>().provider(kThing);
  test_assert(!t.hasSerialize() && !t.hasDeserialize() && !t.hasDuplicate() &&
              !t.hasRekey());
}

/* A provider with one of the two. */
template <bool Write> struct HalfProvider {
  void *data(const Datablock &b) const
  {
    return b.clientData();
  }
  void forEachRef(const Datablock &, RefVisitor &) const
  {
  }
  void destroy(const Datablock &)
  {
  }
  Status serialize(const Datablock &, BlockWriter &) const
  {
    return Status();
  }
  Status deserialize(const Datablock &, BlockReader &)
  {
    return Status();
  }
  static lbt::Struct<HalfProvider> *defineBindings()
  {
    auto *st = new lbt::Struct<HalfProvider>(Write ? "test::W" : "test::R",
                                             sizeof(HalfProvider));
    BIND_STRUCT_METHOD(st, data, MARGS("block"));
    BIND_STRUCT_METHOD(st, forEachRef, MARGS("block", "visitor"));
    BIND_STRUCT_METHOD(st, destroy, MARGS("block"));
    if constexpr (Write) {
      BIND_STRUCT_METHOD(st, serialize, MARGS("block", "out"));
    } else {
      BIND_STRUCT_METHOD(st, deserialize, MARGS("block", "in"));
    }
    return st;
  }
};

template <class P>
static Status addType(Runtime &rt, const char *name, BlockStorage storage, P *provider)
{
  const std::string impl = std::string(name) + "#provider";
  test_assert((registerImpl<BlockProvider, P>(rt, impl.c_str()).ok()));
  Result<Owned<BlockProvider>> owned = own<BlockProvider>(rt, impl, provider);
  test_assert(owned.ok());
  BlockTypeInfo info;
  info.name = name;
  info.client = ClientId::of(kCoredccClient);
  info.storage = storage;
  return rt.ext<BlockTypeRegistry>().add(info, std::move(owned).value()).status();
}

static void test_registry_rules()
{
  Runtime rt;
  const auto half = [&](auto *p, const char *name) {
    return addType(rt, name, BlockStorage::Unspecified, p);
  };
  test_assert(hasMsg(half(litestl::alloc::New<HalfProvider<true>>("p"), "t.W"),
                     "both or neither"));
  test_assert(hasMsg(half(litestl::alloc::New<HalfProvider<false>>("p"), "t.R"),
                     "both or neither"));
  const auto docType = [&](const char *name, BlockStorage s) {
    return addType(rt, name, s, litestl::alloc::New<DocProvider>("p"));
  };
  const auto thingType = [&](const char *name, BlockStorage s) {
    return addType(rt, name, s, litestl::alloc::New<ThingProvider>("p"));
  };
  test_assert(hasMsg(docType("t.1", BlockStorage::Transient), "Transient"));
  test_assert(hasMsg(thingType("t.2", BlockStorage::Payload), "no serialize"));
  test_assert(docType("t.3", BlockStorage::Payload).ok() &&
              docType("t.4", BlockStorage::Unspecified).ok());
  test_assert(thingType("t.5", BlockStorage::Transient).ok() &&
              thingType("t.6", BlockStorage::Unspecified).ok());
  const auto storage = [&](const char *name) {
    return rt.ext<BlockTypeRegistry>().find(TypeId::of(name))->info.storage;
  };
  test_assert(storage("t.4") == BlockStorage::Payload &&
              storage("t.5") == BlockStorage::Transient &&
              storage("t.6") == BlockStorage::Unspecified);
  test_assert(!rt.ext<BlockTypeRegistry>().find(TypeId::of("t.1")) &&
              rt.validateAll().ok());
}

/* ---- a foreign provider ---- */

struct FakeHost : ForeignDispatcher {
  std::string log;
  bool fail = false;
  Status call(const IfaceTable &table, void *, int slot, void **args, void *ret) override
  {
    const char *name = table.ifaceDesc->methods[slot]->name.c_str();
    log += std::string(name) + ";";
    const Datablock &b = *static_cast<const Datablock *>(args[0]);
    if (fail) {
      return Status::error(StatusCode::Internal, "TypeError: it threw");
    }
    Doc *d = static_cast<Doc *>(b.clientData());
    if (strcmp(name, "data") == 0) {
      new (ret) void *(d);
    } else if (strcmp(name, "forEachRef") == 0) {
      if (d) {
        static_cast<RefVisitor *>(args[1])->visit(d->mesh);
      }
    } else if (strcmp(name, "destroy") == 0) {
      litestl::alloc::Delete(d);
    } else if (strcmp(name, "serialize") == 0) {
      BlockWriter &out = *static_cast<BlockWriter *>(args[1]);
      out.bytes().i64(d->number);
      out.ref(d->mesh);
      new (ret) Status();
    } else if (strcmp(name, "deserialize") == 0) {
      BlockReader &in = *static_cast<BlockReader *>(args[1]);
      d = litestl::alloc::New<Doc>("Doc");
      d->number = in.bytes().i64();
      in.ref(d->mesh);
      in.setData(d);
      new (ret) Status();
    } else if (strcmp(name, "duplicate") == 0) {
      Doc *copy = litestl::alloc::New<Doc>("Doc");
      copy->number = d->number;
      static_cast<BlockCopy *>(args[1])->setData(copy);
      new (ret) Status();
    } else if (strcmp(name, "rekey") == 0) {
      BlockId id = b.id();
      id.local.lo = 77;
      if (!static_cast<const RekeyContext *>(args[1])->isFree(id)) {
        id.local.lo++;
      }
      new (ret) BlockId(id);
    }
    return Status();
  }
};

static void test_foreign()
{
  Runtime rt;
  FakeHost host;
  const char *slots[] = {
      "data", "forEachRef", "destroy", "serialize", "deserialize", "duplicate", "rekey"};
  test_assert(
      registerForeign<BlockProvider>(rt, "test.Js#provider", &host, nullptr, slots).ok());
  int jsObject = 1;
  BlockProvider p = query<BlockProvider>(rt, TypeId::of("test.Js#provider"), &jsObject);
  BlockTypeInfo info;
  info.name = "test.Js";
  info.client = ClientId::of(kCoredccClient);
  test_assert(
      rt.ext<BlockTypeRegistry>().add(info, Owned<BlockProvider>(p, nullptr)).ok());
  Library lib(&rt);
  lib.setAuditRelations(true);
  Doc *d = litestl::alloc::New<Doc>("Doc");
  d->number = 12;
  Result<BlockHandle> h = lib.create(TypeId::of("test.Js"), "h", d);
  test_assert(h.ok() && lib.writeRef(d->mesh, lib.get(*h)->id()).ok());
  Result<BlockPayload> payload = writePayload(lib, *h);
  test_assert(payload.ok() && payload->refs.size() == 1);
  Result<BlockHandle> back = lib.create(TypeId::of("test.Js"), "back", nullptr);
  test_assert(back.ok() && readPayload(lib, *back, *payload).ok());
  test_assert(doc(lib, *back)->number == 12 &&
              doc(lib, *back)->mesh.target == lib.get(*h)->id());
  BlockCopy copy;
  test_assert(p.duplicate(*lib.get(*h), copy).ok());
  test_assert(PayloadAccess::data(copy) &&
              static_cast<Doc *>(PayloadAccess::data(copy))->number == 12);
  litestl::alloc::Delete(static_cast<Doc *>(PayloadAccess::data(copy)));
  const auto odd = [](const void *, const BlockId &id) { return (id.local.lo & 1) != 0; };
  const auto even = [](const void *, const BlockId &id) {
    return (id.local.lo & 1) == 0;
  };
  test_assert(p.rekey(*lib.get(*h), RekeyContext(lib, nullptr, odd)).local.lo == 77);
  test_assert(p.rekey(*lib.get(*h), RekeyContext(lib, nullptr, even)).local.lo == 78);

  Result<BlockHandle> none = lib.create(TypeId::of("test.Js"), "none", nullptr);
  host.fail = true;
  test_assert(hasMsg(writePayload(lib, *h).status(), "TypeError"));
  test_assert(hasMsg(readPayload(lib, *none, *payload), "TypeError"));
  host.fail = false;
  test_assert(!rt.hasStickyError() && lib.validate().ok());
}

/* ---- hostile input ---- */

/* Any bytes and any table: no crash, the record has data or it doesn't, and time and
 * memory stay bounded. */
static void test_fuzz()
{
  World w;
  const BlockHandle a = makeThing(w.lib, "a"), b = makeThing(w.lib, "b");
  const BlockHandle h = makeFull(w, a, b);
  {
    BlockEntry unknown;
    unknown.name = "future";
    unknown.unknownTag = 99;
    unknown.refs.append(doc(w.lib, h)->mesh);
    unknown.raw.append(uint8_t(1));
    doc(w.lib, h)->unread.append(unknown);
    w.lib.noteRefsChanged();
  }
  Result<BlockPayload> p = writePayload(w.lib, h);
  test_assert(p.ok());
  for (int cut = 0; cut < int(p->bytes.size()); cut++) {
    BlockPayload part = *p;
    part.bytes.resize(size_t(cut));
    const BlockHandle e = emptyDoc(w.lib, "e");
    test_assert(!readPayload(w.lib, e, part).ok() && !w.lib.get(e)->clientData());
  }
  for (int rows = 0; rows < int(p->refs.size()); rows++) {
    BlockPayload part = *p;
    part.refs.resize(size_t(rows));
    test_assert(hasMsg(readPayload(w.lib, emptyDoc(w.lib, "e"), part), "ref table"));
  }
  w.lib.setFlag(h, BlockFlags::Root, true);
  SplitMix64 rng(3);
  const auto start = std::chrono::steady_clock::now();
  size_t worst = 0;
  int read = 0;
  for (int i = 0; i < 6000 * testScale(); i++) {
    BlockPayload bad = *p;
    for (uint32_t e = 1 + rng.below(4); e > 0; e--) {
      const int at = int(rng.below(uint32_t(bad.bytes.size())));
      bad.bytes[at] = rng.below(3) == 0 ? 0xff
                      : rng.below(2)    ? uint8_t(rng.below(13))
                                        : uint8_t(rng.next());
    }
    const BlockHandle e = emptyDoc(w.lib, "e");
    const size_t before = litestl::alloc::getMemorySize();
    const Status s = readPayload(w.lib, e, bad);
    worst = std::max(worst, litestl::alloc::getMemorySize() - before);
    test_assert(s.ok() == (w.lib.get(e)->clientData() != nullptr));
    if (s.ok()) {
      read++;
      /* What was read can be written, and the audit agrees. */
      Result<BlockPayload> again = writePayload(w.lib, e);
      test_assert(again.ok());
      const BlockHandle e2 = emptyDoc(w.lib, "e2");
      test_assert(again.ok() && readPayload(w.lib, e2, *again).ok());
    }
    if (i % 200 == 0) {
      test_assert(w.lib.collect().ok() && !w.rt.hasStickyError());
    }
  }
  const double seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  test_assert(read > 100 && worst < 64 * 1024);
  test_assert(seconds < 30.0 * testScale());

  /* The densest inputs: what is allocated is a small multiple of the size. */
  const uint32_t kItems = 100000;
  for (int kind = 0; kind < 2; kind++) {
    BlockWriter out(w.lib);
    out.bytes().i64(0);
    out.ref(RefCell(RefKind::Weak));
    out.ref(RefCell(RefKind::Weak));
    out.bytes().u32(kind == 0 ? kItems : 1);
    for (uint32_t i = 0; i < (kind == 0 ? kItems : 1); i++) {
      out.bytes().str("");
      out.bytes().u8(0);
      out.bytes().u32(kind == 0 ? 0 : kItems); /* refs, all of row 0 */
      for (uint32_t k = 0; kind == 1 && k < kItems; k++) {
        out.bytes().u32(0);
      }
      out.bytes().blob({});
    }
    out.entries({});
    Result<BlockPayload> dense = PayloadAccess::take(out, 3);
    const BlockHandle e = emptyDoc(w.lib, "dense");
    const size_t before = litestl::alloc::getMemorySize();
    test_assert(dense.ok() && readPayload(w.lib, e, *dense).ok());
    test_assert(litestl::alloc::getMemorySize() - before <
                100 * size_t(dense->bytes.size()));
  }
  test_assert(w.lib.collect().ok() && w.valid());
}

int main()
{
  test_round_trip();
  test_golden();
  test_unknown_kept();
  test_writer_refuses();
  test_bad_entries();
  test_audit();
  test_errors();
  test_names();
  test_other_slots();
  test_registry_rules();
  test_foreign();
  test_fuzz();
  return test_end();
}
