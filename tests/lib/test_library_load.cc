/* Loading a library: types that are missing, newer or fail, names and aliases, Busy
 * rules, and resolvePlaceholders. Damaged files are test_library_hostile.cc's. */
#include "io_fixture.h"

#include "coredcc/lib/pin.h"

test_init;

using namespace coredcc;
using namespace coredcc_test;

namespace {

BlockId cid(uint64_t n, ClientId client = ClientId::of(kCoredccClient))
{
  return BlockId{client, LocalId{7, n}};
}

/* a -> b, with a slot to b and a note about c, which is only the note's. */
Bytes sample()
{
  IoWorld w;
  const BlockHandle a = w.doc(1, "a"), b = w.doc(2, "b"), c = w.doc(3, "c");
  w.root(a);
  w.link(a, b);
  test_assert(w.lib.setSlot(a, kDoc, 1, w.id(b), RefKind::Strong).ok());
  w.note(a, 5, w.id(c));
  return w.save();
}

Status slotError(LoadReport &report)
{
  return report.issues.first.size() ? report.issues.first[0].why : Status();
}

} // namespace

static void test_refused()
{
  const Bytes bytes = sample();
  IoWorld w;
  const BlockHandle h = w.doc(9, "x");
  test_assert(hasMsg(w.load(bytes), "already has records"));
  w.root(h);
  test_assert(w.lib.markDeleted(h).ok());
  test_assert(w.load(bytes).code() ==
              StatusCode::FailedPrecondition); /* undo may hold it */
  test_assert(w.lib.destroyFromHistory(h).ok());
  {
    EvaluationScope scope(w.rt);
    test_assert(w.load(bytes).code() == StatusCode::Busy);
    test_assert(detail::resolvePlaceholders(w.lib).status().code() == StatusCode::Busy);
  }
  w.rt.setStickyError(Status::error(StatusCode::Internal, "before"));
  test_assert(w.load(bytes).code() == StatusCode::FailedPrecondition);
  test_assert(w.rt.takeStickyError().code() == StatusCode::Internal);
  const uint64_t epoch = w.lib.epoch();
  test_assert(w.load(bytes).ok() && w.lib.size() == 3 && w.lib.epoch() > epoch);
}

static void test_missing_types()
{
  const Bytes bytes = sample();
  Knows k;
  k.doc = k.note = false;
  IoWorld w(k);
  LoadReport report;
  LoadOptions options;
  options.sourceName = "scene.cdcc";
  test_assert(w.load(bytes, &report, options).ok());
  test_assert(w.lib.size() == 3 && report.blocks.size() == 3 && report.missingType == 3);
  test_assert(report.missingBlock == 0);
  test_assert(report.issues.total == 4); /* three types and an attachment */
  test_assert(has(issueText(report.issues), "a: no block type 'test.Doc' is registered"));
  test_assert(has(issueText(report.issues), "a: attachment 'test.Note': the type isn't"));
  test_assert(report.issues.first[0].block == cid(1));
  const Datablock &a = *w.lib.get(w.at(1));
  test_assert(a.state() == BlockState::Placeholder &&
              a.placeholderData().refs.size() >= 2);
  test_assert(strcmp(a.sourceName().c_str(), "scene.cdcc") == 0);
  /* No name went into the Runtime. */
  test_assert(!w.rt.nameOf(kDoc) && !w.rt.nameOf(kNote));
  test_assert(w.lib.fileNames().nameOf(IdDomain::Type, kDoc.value));
  /* Nothing reachable is lost, and it saves as it was: where it came from isn't written.
   */
  test_assert(sameBytes(w.save(), bytes));
  test_assert(*w.lib.collect() == 0);
  {
    IoWorld again;
    LoadOptions other;
    other.sourceName = "other";
    test_assert(again.load(w.save(), nullptr, other).ok());
    const BlockHandle h = again.at(1);
    test_assert(again.lib.get(h)->state() == BlockState::Live);
    test_assert(again.lib.get(h)->sourceName().size() == 0);
  }

  {
    IoWorld again(k);
    LoadOptions other;
    other.sourceName = "other";
    test_assert(again.load(w.save(), nullptr, other).ok());
    test_assert(strcmp(again.lib.get(again.at(1))->sourceName().c_str(), "other") == 0);
  }

  /* Then the types come. */
  Result<LoadReport> none = detail::resolvePlaceholders(w.lib);
  test_assert(none.ok() && none->blocks.size() == 0 && none->missingType == 3 &&
              none->issues.total == 4);
  registerDoc(w.rt, w.docs);
  const BlockHandle ha = w.at(1);
  const uint64_t changes = w.lib.get(ha)->stateChanges();
  Result<LoadReport> some = detail::resolvePlaceholders(w.lib);
  test_assert(some.ok() && some->blocks.size() == 3 && some->missingType == 0);
  test_assert(some->issues.total == 1); /* the note */
  test_assert(w.lib.get(ha)->state() == BlockState::Live && w.at(1) == ha);
  test_assert(w.lib.get(ha)->stateChanges() > changes);
  test_assert(w.lib.get(ha)->placeholderType().size() == 0 &&
              w.lib.get(ha)->placeholderData().bytes.size() == 0);
  test_assert(w.data(ha)->number == 10 && w.lib.resolveRef(w.data(ha)->mesh) == w.at(2));
  registerNote(w.rt, w.notes);
  test_assert(w.lib.attach(ha, kNote, &w).code() ==
              StatusCode::AlreadyExists); /* opaque */
  some = detail::resolvePlaceholders(w.lib);
  test_assert(some.ok() && some->blocks.size() == 1 && some->blocks[0] == ha);
  const Note *n = static_cast<const Note *>(w.lib.attachment(ha, kNote));
  test_assert(n && n->value == 5 && w.lib.resolveRef(n->about) == w.at(3));
  some = detail::resolvePlaceholders(w.lib);
  test_assert(some.ok() && some->blocks.size() == 0 && some->issues.total == 0);
}

static void test_unreadable()
{
  const Bytes bytes = sample();
  /* A payload or attachment newer than the type. */
  Knows k;
  k.docVersion = 2;
  k.noteVersion = 1;
  {
    IoWorld w(k);
    LoadReport report;
    test_assert(w.load(bytes, &report).ok() && report.missingType == 3);
    test_assert(has(issueText(report.issues), "it was written by a newer version"));
    test_assert(
        has(issueText(report.issues), "attachment 'test.Note' was written by a newer"));
    test_assert(sameBytes(w.save(), bytes) && w.notes.alive == 0);
  }
  /* The record's client isn't the type's. */
  {
    IoWorld from;
    const BlockHandle h = from.doc(1, "a", kAcmeDoc);
    from.root(h);
    const Bytes acme = from.save();
    Knows no;
    no.acme = false;
    IoWorld w(no);
    test_assert(w.rt.ext<ClientRegistry>().registerClient("usurper").ok());
    registerDoc(w.rt, w.docs, 1, "acme.Doc", ClientId::of("usurper"));
    LoadReport report;
    test_assert(w.load(acme, &report).ok() && report.missingType == 1);
    test_assert(has(issueText(report.issues), "belongs to another client"));
    test_assert(sameBytes(w.save(), acme));
  }
  /* A type that is Transient in this build. */
  {
    RawFile f;
    f.names[1] = "test.Transient";
    f.records.push_back(RawRecord());
    IoWorld w;
    LoadReport report;
    test_assert(w.load(f.file(), &report).ok() && report.missingType == 1);
    test_assert(has(issueText(report.issues), "isn't read from files"));
  }
  /* An attachment type that isn't read. */
  {
    RawFile f;
    f.names.push_back("test.Bare");
    RawRecord r;
    r.attachments = [](BytesWriter &out) {
      out.u32(1);
      out.u32(3);
      out.u32(1);
      out.u32(0);
      out.u32(0);
    };
    f.records.push_back(std::move(r));
    IoWorld w;
    LoadReport report;
    test_assert(w.load(f.file(), &report).ok());
    test_assert(
        has(issueText(report.issues), "attachment 'test.Bare': the type isn't read"));
  }
  /* Every way a deserialize can fail leaves a placeholder with the same payload. */
  for (DocFault fault :
       {DocFault::Error, DocFault::Sticky, DocFault::LeaveBytes, DocFault::NoData})
  {
    IoWorld w;
    w.docs.fault = fault;
    LoadReport report;
    test_assert(w.load(bytes, &report).ok() && report.missingType == 3 &&
                report.issues.total == 3);
    test_assert(w.docs.destroyed == (fault == DocFault::LeaveBytes ? 3 : 0));
    const char *why = fault == DocFault::Error        ? "not today"
                      : fault == DocFault::Sticky     ? "deserialize threw"
                      : fault == DocFault::LeaveBytes ? "left over"
                                                      : "without calling setData";
    test_assert(has(issueText(report.issues), why) && !w.rt.hasStickyError());
    w.docs.fault = DocFault::None;
    test_assert(sameBytes(w.save(), bytes));
    Result<LoadReport> fixed = detail::resolvePlaceholders(w.lib);
    test_assert(fixed.ok() && fixed->blocks.size() == 3 && fixed->issues.total == 0);
    test_assert(sameBytes(w.save(), bytes));
  }
  for (DocFault fault : {DocFault::Error, DocFault::Sticky, DocFault::LeaveBytes}) {
    IoWorld w;
    w.notes.fault = fault;
    LoadReport report;
    test_assert(w.load(bytes, &report).ok() && report.missingType == 0 &&
                report.issues.total == 1);
    const char *why = fault == DocFault::Error ? "'test.Note': no notes"
                      : fault == DocFault::Sticky
                          ? "'test.Note': note threw"
                          : "'test.Note': the data ends too early";
    test_assert(has(issueText(report.issues), why) && !w.rt.hasStickyError());
    test_assert(w.notes.alive == 0 &&
                w.notes.severed == (fault == DocFault::LeaveBytes ? 1 : 0));
    test_assert(!w.lib.attachment(w.at(1), kNote));
    test_assert(sameBytes(w.save(), bytes));
  }
  /* A provider that fails to report its references: the load goes on. */
  {
    IoWorld w;
    w.notes.failForeach = true;
    LoadReport report;
    test_assert(w.load(bytes, &report).ok() && report.issues.total == 1);
    test_assert(has(issueText(report.issues), "a: note forEachRef threw"));
    w.notes.failForeach = false;
  }
}

static void test_missing_blocks()
{
  IoWorld from;
  const BlockHandle a = from.doc(1, "a");
  from.root(a);
  from.data(a)->mesh.target = cid(50);
  from.data(a)->parent.target = cid(51); /* weak: no record */
  test_assert(from.lib.setSlot(a, kDoc, 0, cid(52), RefKind::Strong).ok());
  test_assert(from.lib.setSlot(a, kDoc, 1, cid(50), RefKind::Strong).ok());
  from.note(a, 1, cid(53));
  from.lib.noteRefsChanged();
  const Bytes bytes = from.save();
  {
    IoWorld w;
    LoadReport report;
    test_assert(w.load(bytes, &report).ok());
    test_assert(w.lib.size() == 4 && report.blocks.size() == 4 &&
                report.missingBlock == 3);
    for (uint64_t n : {50, 52, 53}) {
      const Datablock &b = *w.lib.get(w.at(n));
      test_assert(b.placeholderReason() == PlaceholderReason::MissingBlock && !b.type());
    }
    test_assert(!w.at(51) && sameBytes(w.save(), bytes));
  }
  /* From tables alone, when nothing can be read. */
  {
    Knows k;
    k.doc = k.note = false;
    IoWorld w(k);
    test_assert(w.load(bytes).ok() && w.lib.size() == 4 && w.at(50) && w.at(53));
  }
  {
    IoWorld w;
    LoadOptions options;
    options.limits.maxBlocks = 4;
    test_assert(w.load(bytes, nullptr, options).ok());
    IoWorld w2;
    options.limits.maxBlocks = 3;
    Status s = w2.load(bytes, nullptr, options);
    test_assert(s.code() == StatusCode::OutOfRange && hasMsg(s, "more than 3 records"));
    test_assert(w2.lib.size() == 0);
  }
  /* A table whose kinds are out of date: what the provider reports counts, within the
   * limit. */
  {
    IoWorld stale;
    const BlockHandle h = stale.doc(1, "stale");
    stale.root(h);
    stale.data(h)->mesh.target = cid(50);
    stale.lib.noteRefsChanged();
    stale.lib.setAuditRelations(false);
    stale.docs.fault = DocFault::WrongStrength; /* the table says weak */
    const Bytes file = stale.save();
    stale.docs.fault = DocFault::None;
    {
      IoWorld w;
      LoadReport report;
      test_assert(w.load(file, &report).ok() && w.at(50) && report.missingBlock == 1);
    }
    {
      IoWorld w;
      LoadReport report;
      LoadOptions options;
      options.limits.maxBlocks = 1;
      test_assert(w.load(file, &report, options).ok() && !w.at(50) &&
                  report.missingBlock == 0);
      test_assert(has(issueText(report.issues), "too many missing blocks"));
    }
    Knows k;
    k.doc = false;
    IoWorld w(k);
    test_assert(w.load(file).ok() && !w.at(50));
    registerDoc(w.rt, w.docs);
    Result<LoadReport> r = detail::resolvePlaceholders(w.lib);
    test_assert(r.ok() && w.at(50) && r->blocks.size() == 2 && r->missingBlock == 1);
  }
  {
    RawFile f;
    f.names[1] = "test.Note2";
    f.records.push_back(RawRecord());
    f.records[0].rows = {
        {detail::libio::kRowWeak, LocalId{7, 60}}}; /* a table out of date */
    f.records[0].as = 1;
    f.records[0].slots = [](BytesWriter &out) {
      out.u32(1);
      out.u32(2);
      out.i64(0);
      RawRecord::row(out, detail::libio::kRowStrong, LocalId());
    };
    IoWorld w;
    LoadReport report;
    test_assert(w.load(f.file(), &report).ok() && w.lib.size() == 1);
    test_assert(report.issues.total == 1 &&
                hasMsg(slotError(report), "a Strong slot that"));
    const RefCell *s = w.lib.slot(w.at(1), TypeId::of("test.Note2"), 0);
    test_assert(s && s->isNull() && s->kind == RefKind::Strong);
  }
}

static void test_names()
{
  const Bytes bytes = sample();
  /* Another name has the id in the Runtime, in the library, or in the file. */
  {
    Knows k;
    k.doc = false;
    IoWorld w(k);
    test_assert(w.rt.debugRegisterNameAs(IdDomain::Type, kDoc.value, "impostor").ok());
    Status s = w.load(bytes);
    test_assert(s.code() == StatusCode::InvalidArgument);
    test_assert(hasMsg(s, "the name 'test.Doc' has the id of 'impostor'"));
    test_assert(w.lib.size() == 0 &&
                !w.lib.fileNames().nameOf(IdDomain::Type, kNote.value));
  }
  {
    Knows k;
    k.note = false;
    IoWorld w(k);
    test_assert(w.lib.fileNames().debugAddAs(IdDomain::Type, kNote.value, "impostor"));
    test_assert(hasMsg(w.load(bytes), "has the id of 'impostor'"));
  }
  {
    /* An id that has a name elsewhere, but as an id in the file, is that name. */
    RawFile f;
    f.records.push_back(RawRecord());
    BytesWriter n;
    n.u32(2);
    n.u8(1);
    n.u64(kAcme.value);
    n.u8(0);
    n.str("x.Type");
    f.file();
    f.chunks[0].bytes = n.take();
    f.chunks[0].raw = f.chunks[0].bytes.size();
    IoWorld w;
    test_assert(w.load(f.build()).ok() && w.at(1, kAcme));
  }
  {
    /* More names than a table holds: nothing is lost but the text. */
    RawFile f;
    f.names.push_back(std::string(NameTable::kMaxBytes, 'n'));
    f.records.push_back(RawRecord());
    f.records[0].type = 3;
    IoWorld w;
    LoadReport report;
    test_assert(w.load(f.file(), &report).ok() && w.at(1));
    test_assert(has(issueText(report.issues), "too many names to keep"));
    const TypeId ns = TypeId::of("x.Type");
    test_assert(
        !w.lib.fileNames().nameOf(IdDomain::Type, ns.value)); /* unused in the file */
  }
}

static void test_aliases()
{
  /* Two old names and the current one, with one LocalId; each refers to the next. */
  RawFile f;
  f.names = {"c.old1", "x.Type", "c.now", "c.old2"};
  for (int i = 0; i < 3; i++) {
    RawRecord r;
    r.client = i == 0 ? 1 : i == 1 ? 3 : 4;
    r.name = i == 0 ? "old1" : i == 1 ? "now" : "old2";
    r.slots = [i](BytesWriter &out) {
      out.u32(1);
      out.u32(2);
      out.i64(0);
      RawRecord::row(out,
                     detail::libio::kRowStrong,
                     LocalId{7, 1},
                     i == 0   ? 3
                     : i == 1 ? 4
                              : 1);
    };
    f.records.push_back(std::move(r));
  }
  const Bytes bytes = f.file();
  IoWorld w;
  const char *aliases[] = {"c.old1", "c.old2"};
  test_assert(w.rt.ext<ClientRegistry>().registerClient("c.now", aliases).ok());
  LoadReport report;
  test_assert(w.load(bytes, &report).ok() && w.lib.size() == 3 &&
              report.blocks.size() == 3);
  BlockHandle h[3];
  for (int i = 0; i < 3; i++) {
    h[i] = report.blocks[i];
    test_assert(w.id(h[i]).client == ClientId::of("c.now"));
  }
  test_assert(w.id(h[1]) == cid(1, ClientId::of("c.now")));
  test_assert(w.id(h[0]) != w.id(h[1]) && w.id(h[2]) != w.id(h[1]) &&
              w.id(h[0]) != w.id(h[2]));
  const auto slot = [&](BlockHandle b) {
    return w.lib.slot(b, TypeId::of("x.Type"), 0)->target;
  };
  test_assert(slot(h[0]) == w.id(h[1]) && slot(h[1]) == w.id(h[2]) &&
              slot(h[2]) == w.id(h[0]));
  int told = 0;
  for (const IoIssue &i : report.issues.first) {
    told += i.why.code() == StatusCode::AlreadyExists ? 1 : 0;
  }
  test_assert(told == 2);
  /* References find the moved ones, however many there are. */
  {
    RawFile more;
    more.names = f.names;
    for (uint64_t n = 1; n <= 8; n++) {
      for (uint32_t c : {3u, 1u, 4u}) {
        RawRecord r;
        r.client = c;
        r.local.lo = n;
        r.slots = [c, n](BytesWriter &out) {
          out.u32(1);
          out.u32(2);
          out.i64(0);
          RawRecord::row(out, detail::libio::kRowStrong, LocalId{7, n}, c);
        };
        more.records.push_back(std::move(r));
      }
    }
    IoWorld many;
    test_assert(many.rt.ext<ClientRegistry>().registerClient("c.now", aliases).ok());
    LoadOptions options;
    options.limits.maxBlocks = 24; /* none of them is missing */
    test_assert(many.load(more.file(), &report, options).ok() && many.lib.size() == 24);
    for (BlockHandle b : report.blocks) {
      test_assert(many.lib.slot(b, TypeId::of("x.Type"), 0)->target == many.id(b));
    }
  }
  /* Without the current one, the first in order keeps the id. */
  f.records.erase(f.records.begin() + 1);
  IoWorld w2;
  test_assert(w2.rt.ext<ClientRegistry>().registerClient("c.now", aliases).ok());
  test_assert(w2.load(f.file(), &report).ok());
  const int kept = int(w2.id(report.blocks[0]) == cid(1, ClientId::of("c.now"))) +
                   int(w2.id(report.blocks[1]) == cid(1, ClientId::of("c.now")));
  test_assert(kept == 1);
  /* A GUID source that repeats itself doesn't make duplicates. */
  f.records.clear();
  for (uint32_t c : {1u, 3u}) {
    RawRecord r;
    r.client = c;
    f.records.push_back(std::move(r));
  }
  IoWorld w3;
  test_assert(w3.rt.ext<ClientRegistry>().registerClient("c.now", aliases).ok());
  w3.rt.setGuidSource([]() { return LocalId{7, 1}; });
  Status s = w3.load(f.file());
  test_assert(s.code() == StatusCode::Internal && w3.lib.size() == 0);
  int calls = 0;
  w3.rt.setGuidSource([&calls]() { return LocalId{7, uint64_t(++calls < 3 ? 1 : 2)}; });
  test_assert(w3.load(f.file()).ok() && w3.lib.size() == 2 &&
              w3.at(2, ClientId::of("c.now")));
}

static void test_busy()
{
  const Bytes bytes = sample();
  IoWorld w;
  int calls = 0;
  bool sawOther = false;
  LibraryPin pin;
  w.docs.onDeserialize = [&](BlockReader &in) {
    calls++;
    Library &lib = in.library();
    test_assert(&lib == &w.lib && lib.busy() && lib.size() == 3);
    test_assert(lib.create(kDoc, "x", nullptr).status().code() == StatusCode::Busy);
    test_assert(lib.markDeleted(w.at(1)).code() == StatusCode::Busy);
    test_assert(lib.collect().status().code() == StatusCode::Busy);
    test_assert(saveLibrary(lib).status().code() == StatusCode::Busy);
    test_assert(detail::resolvePlaceholders(lib).status().code() == StatusCode::Busy);
    test_assert(w.load(bytes).code() == StatusCode::Busy);
    lib.setFlag(w.at(2), BlockFlags::FakeUser, true); /* deferred */
    test_assert(!lib.get(w.at(2))->hasFlag(BlockFlags::FakeUser));
    if (calls == 1) {
      pin = LibraryPin(lib, w.at(1));
    }
    /* Blocks that have no data yet resolve to nothing; asking who uses what is safe. */
    sawOther = sawOther || lib.data(w.at(3)) != nullptr || lib.data(w.at(2)) != nullptr;
    lib.usersOf(w.at(2));
  };
  LoadReport report;
  test_assert(w.load(bytes, &report).ok() && calls == 3 && report.issues.total == 0);
  test_assert(sawOther && !w.lib.busy());
  test_assert(w.lib.get(w.at(2))->hasFlag(BlockFlags::FakeUser));
  test_assert(w.lib.get(w.at(1))->pins(PinKind::Host) == 1);
  pin.reset();
  w.docs.onDeserialize = nullptr;
  /* What is deferred in a load waits for its end, also when a half-read block is
   * destroyed. */
  IoWorld w2;
  w2.docs.fault = DocFault::LeaveBytes;
  w2.docs.onDeserialize = [&](BlockReader &in) {
    in.library().setFlag(w2.at(2), BlockFlags::FakeUser, true);
  };
  w2.docs.onDestroy = [&](const Datablock &b) {
    test_assert(w2.lib.busy() && !w2.lib.get(w2.at(2))->hasFlag(BlockFlags::FakeUser));
    test_assert(b.state() == BlockState::Live);
  };
  test_assert(w2.load(bytes).ok() && w2.lib.get(w2.at(2))->hasFlag(BlockFlags::FakeUser));
  w2.docs.onDestroy = nullptr;
  w2.docs.onDeserialize = nullptr;
  w2.docs.fault = DocFault::None;
}

static void test_resolve_aliases()
{
  /* A placeholder whose client turns out to be an alias of the type's. */
  IoWorld from;
  const BlockHandle h = from.doc(1, "a", kAcmeDoc);
  from.root(h);
  const Bytes bytes = from.save();
  Knows k;
  k.acme = false;
  IoWorld w(k);
  test_assert(w.load(bytes).ok());
  const char *aliases[] = {"acme"};
  test_assert(w.rt.ext<ClientRegistry>().registerClient("acme2", aliases).ok());
  registerDoc(w.rt, w.docs, 1, "acme.Doc", ClientId::of("acme2"));
  Result<LoadReport> r = detail::resolvePlaceholders(w.lib);
  test_assert(r.ok() && r->blocks.size() == 1 && r->issues.total == 0);
  test_assert(w.lib.get(w.at(1, ClientId::of("acme2")))->state() == BlockState::Live);

  /* A deleted placeholder is left alone, and so is a block of a missing host. */
  IoWorld w2(k);
  test_assert(w2.load(bytes).ok() && w2.lib.markDeleted(w2.at(1, kAcme)).ok());
  PlaceholderInfo info;
  info.reason = PlaceholderReason::MissingBlock;
  info.typeName = "test.Doc";
  test_assert(w2.lib.addPlaceholder(cid(5), std::move(info)).ok());
  test_assert(w2.rt.ext<ClientRegistry>().registerClient("acme").ok());
  registerDoc(w2.rt, w2.docs, 1, "acme.Doc", kAcme);
  r = detail::resolvePlaceholders(w2.lib);
  test_assert(r.ok() && r->blocks.size() == 0 && r->issues.total == 0 &&
              r->missingType == 0);
  test_assert(w2.lib.get(w2.at(5))->state() == BlockState::Placeholder);
}

int main()
{
  test_refused();
  test_missing_types();
  test_unreadable();
  test_missing_blocks();
  test_names();
  test_aliases();
  test_busy();
  test_resolve_aliases();
  return test_end();
}
