/* Saving a library: round trips against a deep comparison, golden bytes, equal bytes for
 * equal libraries, what is saved, and random libraries. */
#include "fuzz.h"
#include "io_fixture.h"

#include "library_golden.h"

#include <algorithm>
#include <chrono>

test_init;

using namespace coredcc;
using namespace coredcc_test;

namespace {

BlockPayload
payload(uint32_t version, Bytes bytes, std::initializer_list<RefCell> refs = {})
{
  BlockPayload p;
  p.version = version;
  p.bytes = std::move(bytes);
  for (const RefCell &c : refs) {
    p.refs.append(c);
  }
  return p;
}

RefCell cell(RefKind kind, BlockId target = BlockId())
{
  RefCell c(kind);
  c.target = target;
  return c;
}

BlockId cid(uint64_t n, ClientId client = ClientId::of(kCoredccClient))
{
  return BlockId{client, LocalId{7, n}};
}

BlockHandle placeholder(IoWorld &w,
                        BlockId id,
                        const char *type,
                        const char *name,
                        PlaceholderReason reason = PlaceholderReason::MissingType,
                        BlockPayload data = {},
                        const char *source = "")
{
  PlaceholderInfo info;
  info.reason = reason;
  info.typeName = type;
  info.name = name;
  info.sourceName = source;
  info.payload = std::move(data);
  Result<BlockHandle> h = w.lib.addPlaceholder(id, std::move(info));
  test_assert(h.ok());
  return h.ok() ? *h : BlockHandle();
}

/* Loads `bytes` into a world that knows everything and returns whether it equals `w`, and
 * whether it saves as the same bytes. */
bool roundTrips(IoWorld &w, const Bytes &bytes)
{
  IoWorld r;
  LoadReport report;
  Status s = r.load(bytes, &report);
  if (!s.ok()) {
    fprintf(stderr, "load: %s\n", s.message());
    return false;
  }
  return sameLibraries(w.lib, r.lib) && r.lib.validate().ok() &&
         sameBytes(r.save(), bytes);
}

/* Two slots, two attachments, an aliased client's placeholder, every kind of reference.
 */
void buildGolden(IoWorld &w, bool reversed = false)
{
  BlockHandle a, b, c;
  const auto makeA = [&]() { a = w.doc(1, "\xC3\xA9"); };
  const auto makeB = [&]() { b = w.doc(2, "b", kAcmeDoc); };
  const auto makeC = [&]() {
    c = placeholder(w,
                    cid(3, ClientId::of("old.acme")),
                    "gone.Type",
                    "c",
                    PlaceholderReason::MissingType,
                    payload(9,
                            bytesOf({1, 2, 3}),
                            {cell(RefKind::Strong, cid(1)),
                             cell(RefKind::Weak, cid(77)),
                             cell(RefKind::OptStrong)}),
                    "there.cdcc");
  };
  if (reversed) {
    makeC(), makeB(), makeA();
  } else {
    makeA(), makeB(), makeC();
  }
  w.link(a, b, a);
  w.root(a);
  w.lib.setFlag(b, BlockFlags::FakeUser, true);
  w.addOptEntry(a, cid(2, kAcme));
  test_assert(w.data(a)->bag.set("size", PropValue::ofFloat(0.5)).ok());
  w.data(b)->mesh.target = cid(50); /* a block that isn't there */
  const auto slots = [&](int which) {
    if (which == 0) {
      test_assert(w.lib.setSlot(a, kDoc, 5, cid(2, kAcme), RefKind::OptStrong).ok());
    } else {
      test_assert(w.lib
                      .setSlot(a,
                               TypeId{0xABCDEF},
                               -1,
                               cid(3, ClientId::of("old.acme")),
                               RefKind::Weak)
                      .ok());
    }
  };
  slots(reversed ? 1 : 0);
  slots(reversed ? 0 : 1);
  w.note(a, 42, cid(2, kAcme));
  w.note(b, 43, cid(1));
  w.lib.noteRefsChanged();
}

std::string hex(const Bytes &b)
{
  static const char *digits = "0123456789abcdef";
  std::string s;
  for (uint8_t v : b) {
    s += digits[v >> 4];
    s += digits[v & 15];
  }
  return s;
}

Bytes unhex(const char *s)
{
  Bytes b;
  const auto digit = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
  for (; s[0] && s[1]; s += 2) {
    b.append(uint8_t(digit(s[0]) * 16 + digit(s[1])));
  }
  return b;
}

} // namespace

static void test_golden()
{
  Bytes first;
  {
    IoWorld w;
    buildGolden(w);
    /* The MissingBlock that loading makes is there already, for the comparison. */
    placeholder(w, cid(50), "", "", PlaceholderReason::MissingBlock);
    first = w.save();
    test_assert(roundTrips(w, first));
    if (hex(first) != kLibraryGolden) {
      fprintf(stderr, "the golden is now:\n%s\n", hex(first).c_str());
      test_assert(false);
    }
  }
  {
    IoWorld w;
    buildGolden(w, true);
    test_assert(sameBytes(w.save(), first)); /* without the MissingBlock, too */
  }
  {
    /* The golden itself, read back: what is in it doesn't depend on the writer. */
    IoWorld w;
    LoadReport report;
    test_assert(w.load(unhex(kLibraryGolden), &report).ok());
    test_assert(w.lib.size() == 4 && report.blocks.size() == 4);
    test_assert(report.missingType == 1 && report.missingBlock == 1 &&
                report.issues.total == 1);
    const BlockHandle a = w.at(1), b = w.at(2, kAcme),
                      c = w.at(3, ClientId::of("old.acme"));
    test_assert(a && b && c && w.at(50));
    const Doc *d = w.data(a);
    test_assert(d->number == 10 && d->mesh.target == w.id(b) &&
                d->parent.target == w.id(a));
    test_assert(d->mesh.kind == RefKind::Strong && d->parent.kind == RefKind::Weak);
    test_assert(d->entries.size() == 1 &&
                d->entries[0].refs[0].kind == RefKind::OptStrong);
    test_assert(*d->bag.get("size") == PropValue::ofFloat(0.5));
    test_assert(w.lib.get(a)->flags() == BlockFlags::Root);
    test_assert(w.lib.get(b)->flags() == BlockFlags::FakeUser);
    test_assert(strcmp(w.lib.get(a)->name().c_str(), "\xC3\xA9") == 0);
    const RefCell *s = w.lib.slot(a, kDoc, 5);
    test_assert(s && s->kind == RefKind::OptStrong && s->target == w.id(b));
    s = w.lib.slot(a, TypeId{0xABCDEF}, -1);
    test_assert(s && s->kind == RefKind::Weak && s->target == w.id(c));
    const Note *n = static_cast<const Note *>(w.lib.attachment(a, kNote));
    test_assert(n && n->value == 42 && n->about.target == w.id(b));
    const Datablock &p = *w.lib.get(c);
    test_assert(strcmp(p.placeholderType().c_str(), "gone.Type") == 0);
    test_assert(strcmp(p.sourceName().c_str(), "there.cdcc") == 0);
    test_assert(p.placeholderData().version == 9 && p.placeholderData().refs.size() == 3);
    test_assert(sameBytes(p.placeholderData().bytes, bytesOf({1, 2, 3})));
    test_assert(w.lib.resolveRef(p.placeholderData().refs[0]) == a);
  }
}

static void test_round_trip()
{
  IoWorld w;
  const BlockHandle a = w.doc(1, "");
  const BlockHandle b = w.doc(2, "two", kAcmeDoc);
  const BlockHandle host = w.doc(3, "hosted");
  const BlockHandle gone =
      placeholder(w,
                  cid(4),
                  "x.Gone",
                  "gone",
                  PlaceholderReason::MissingType,
                  payload(1, Bytes(), {cell(RefKind::Strong, cid(6))}));
  const BlockHandle hosted =
      placeholder(w,
                  cid(5, kAcme),
                  "x.Host",
                  "host",
                  PlaceholderReason::MissingBlock,
                  payload(4, Bytes(), {cell(RefKind::Strong, cid(1))}),
                  "f");
  const BlockHandle missing =
      placeholder(w, cid(6), "", "", PlaceholderReason::MissingBlock);
  const BlockHandle slotted =
      placeholder(w, cid(7), "", "", PlaceholderReason::MissingBlock);
  test_assert(w.lib.setSlot(slotted, kNote, 0, cid(1), RefKind::Weak).ok());
  w.root(a);
  w.root(slotted);
  w.link(a, b, gone);
  w.link(host, missing);
  test_assert(w.lib.onHostDestroyed(w.id(host)).ok());
  for (int64_t key : {int64_t(3), INT64_MIN, INT64_MAX, int64_t(-3)}) {
    test_assert(w.lib.setSlot(a, kDoc, key, w.id(gone), RefKind::Strong).ok());
  }
  test_assert(w.lib.setSlot(hosted, TypeId(), 1, BlockId(), RefKind::OptStrong).ok());
  w.note(b, -1, w.id(missing));
  w.note(hosted, 7, BlockId());
  w.note(gone, 8, w.id(a));
  /* Enough to be compressed, and an entry of a type this build doesn't know. */
  for (int i = 0; i < 100; i++) {
    w.addOptEntry(b, w.id(a), "a long entry name that repeats itself");
  }
  BlockEntry unknown;
  unknown.name = "later";
  unknown.unknownTag = 200;
  unknown.raw = bytesOf({9, 8, 7});
  w.data(a)->unread.append(unknown);

  SaveReport report;
  const Bytes bytes = w.save(&report);
  test_assert(report.blocks == 6 && report.issues.total == 0); /* all but `missing` */
  test_assert(report.detached.size() == 1 && report.detached[0] == host);
  test_assert(report.transient.size() == 0 && report.throughTransient.size() == 0);
  test_assert(report.hostPinned.size() == 0);
  test_assert(bytes.size() < 4000); /* compressed: the entries alone are more than 6 KB */
  IoWorld r;
  LoadReport loaded;
  test_assert(r.load(bytes, &loaded).ok() && sameLibraries(w.lib, r.lib));
  /* The Detached block is a MissingBlock that the host fills under its old id. */
  const BlockHandle again = r.lib.find(w.id(host));
  test_assert(
      r.lib.createWithId(kDoc, w.id(host), "hosted", litestl::alloc::New<Doc>("Doc"))
          .ok());
  test_assert(r.lib.get(again)->state() == BlockState::Live);
  test_assert(loaded.missingType == 1 && loaded.missingBlock == 4);

  /* An attachment nobody can read is carried along, and its references count. */
  Knows k;
  k.note = false;
  IoWorld without(k);
  test_assert(without.load(bytes).ok());
  const BlockHandle b2 = without.lib.find(w.id(b));
  int opaque = 0;
  without.lib.forEachOpaqueAttachment(b2, [&](const OpaqueAttachment &o) {
    opaque++;
    test_assert(strcmp(o.typeName.c_str(), "test.Note") == 0 && o.payload.version == 2);
  });
  test_assert(opaque == 1);
  test_assert(without.lib.usersOf(without.lib.find(w.id(missing))).size() >= 2);
  test_assert(sameBytes(without.save(), bytes));
  /* Severed and destroyed with its block. */
  test_assert(without.lib.markDeleted(b2).ok());

  IoWorld empty;
  SaveReport none;
  const Bytes nothing = empty.save(&none);
  test_assert(none.blocks == 0 && roundTrips(empty, nothing));
}

/* Attachments are in the order of their names, read or not. */
static void test_attachment_order()
{
  RawFile f;
  f.names = {"coredcc", "test.Doc", "z.After", "test.Note", "a.Before", "s.Slots"};
  RawRecord r;
  r.as = 1;
  r.attachments = [](BytesWriter &out) {
    out.u32(3);
    for (uint32_t type : {3u, 4u, 5u}) {
      out.u32(type);
      out.u32(1);
      out.u32(type == 4 ? 1 : 0);
      out.u32(type == 4 ? 1 : 0);
      if (type == 4) {
        RawRecord::row(out, 0, LocalId{7, 1});
      }
    }
  };
  r.slots = [](BytesWriter &out) {
    out.u32(1);
    out.u32(6);
    out.i64(0);
    RawRecord::row(out, 2, LocalId{7, 1});
  };
  f.records.push_back(std::move(r));
  f.blobs.push_back(bytesOf({1, 0, 0, 0, 0, 0, 0, 0}));
  const Bytes file = f.file();
  Knows k;
  k.noteVersion = 1;
  IoWorld reads(k);
  k.note = false;
  IoWorld doesnt(k);
  test_assert(reads.load(file).ok() && doesnt.load(file).ok());
  test_assert(reads.lib.attachment(reads.at(1), kNote));
  test_assert(sameBytes(reads.save(), doesnt.save()));
  /* A slot's namespace is kept as a name. */
  test_assert(reads.lib.fileNames().nameOf(IdDomain::Type, TypeId::of("s.Slots").value));
}

static void test_alias()
{
  IoWorld w;
  placeholder(w, cid(3, ClientId::of("old.acme")), "gone.Type", "c");
  const Bytes bytes = w.save();
  test_assert(!w.rt.nameOf(ClientId::of("old.acme")));
  /* A client name nobody registered is written as its id, and one a file had as its name.
   */
  IoWorld r;
  const char *aliases[] = {"old.acme"};
  test_assert(r.rt.ext<ClientRegistry>().registerClient("new.acme", aliases).ok());
  test_assert(r.load(bytes).ok());
  test_assert(r.at(3, ClientId::of("new.acme")) && r.at(3, ClientId::of("old.acme")));
  const Bytes renamed = r.save();
  IoWorld r2;
  test_assert(r2.load(renamed).ok());
  test_assert(!r2.rt.nameOf(ClientId::of("new.acme")));
  test_assert(
      strcmp(r2.lib.fileNames().nameOf(IdDomain::Client, ClientId::of("new.acme").value),
             "new.acme") == 0);
  test_assert(sameBytes(r2.save(), renamed));
}

/* Deleted targets in every table. */
static void test_deleted_targets()
{
  IoWorld w;
  const BlockHandle a = w.doc(1, "a");
  const BlockHandle del = w.doc(2, "deleted");
  const BlockHandle det = w.doc(3, "detached");
  w.root(a);
  w.root(del);
  const BlockId ids[3] = {w.id(del), w.id(det), cid(99)};
  const BlockHandle gone = placeholder(w,
                                       cid(4),
                                       "x.Gone",
                                       "gone",
                                       PlaceholderReason::MissingType,
                                       payload(1,
                                               Bytes(),
                                               {cell(RefKind::OptStrong, ids[0]),
                                                cell(RefKind::OptStrong, ids[1]),
                                                cell(RefKind::OptStrong, ids[2]),
                                                cell(RefKind::Strong, ids[0]),
                                                cell(RefKind::Weak, ids[0])}));
  test_assert(w.lib.markDeleted(del).ok() && w.lib.onHostDestroyed(ids[1]).ok());
  for (int i = 0; i < 3; i++) {
    w.addOptEntry(a, ids[i]);
    test_assert(w.lib.setSlot(a, kDoc, i, ids[i], RefKind::OptStrong).ok());
  }
  test_assert(w.lib.setSlot(a, kDoc, 10, ids[0], RefKind::Strong).ok());
  test_assert(w.lib.setSlot(a, kDoc, 11, ids[2], RefKind::Weak).ok());
  SaveReport report;
  const int before = w.lib.size();
  const Bytes bytes = w.save(&report);
  test_assert(w.lib.size() == before && w.lib.get(del)->state() == BlockState::InHistory);
  test_assert(report.blocks == 3);

  IoWorld r;
  test_assert(r.load(bytes).ok());
  const BlockHandle ra = r.at(1);
  const auto &entries = r.data(ra)->entries;
  test_assert(entries[0].refs[0].isNull() && entries[1].refs[0].target == ids[1] &&
              entries[2].refs[0].isNull());
  test_assert(r.lib.slot(ra, kDoc, 0)->isNull() &&
              r.lib.slot(ra, kDoc, 1)->target == ids[1] &&
              r.lib.slot(ra, kDoc, 2)->isNull());
  test_assert(r.lib.slot(ra, kDoc, 10)->target == ids[0] &&
              r.lib.slot(ra, kDoc, 11)->target == ids[2]);
  const auto &rows = r.lib.get(r.at(4))->placeholderData().refs;
  test_assert(rows.size() == 5 && rows[0].isNull() && rows[1].target == ids[1] &&
              rows[2].isNull() && rows[3].target == ids[0] && rows[4].target == ids[0]);
  /* The Strong ones got a record, which undoing the delete's session can't: a new file.
   */
  test_assert(r.at(2) && r.lib.get(r.at(2))->state() == BlockState::Placeholder);
  test_assert(!r.at(99)); /* only OptStrong and Weak */
  (void)gone;

  /* An opaque attachment's table follows the same rule. */
  IoWorld w2;
  const BlockHandle x = w2.doc(1, "x"), y = w2.doc(2, "y");
  w2.root(x);
  w2.root(y);
  w2.note(x, 1, w2.id(y))->about.kind = RefKind::OptStrong;
  const Bytes with = w2.save();
  Knows k;
  k.note = false;
  IoWorld w3(k);
  test_assert(w3.load(with).ok() && w3.lib.markDeleted(w3.at(2)).ok());
  IoWorld w4(k);
  test_assert(w4.load(w3.save()).ok());
  bool nulled = false;
  w4.lib.forEachOpaqueAttachment(w4.at(1), [&](const OpaqueAttachment &o) {
    nulled = o.payload.refs.size() == 1 && o.payload.refs[0].isNull();
  });
  test_assert(nulled && !w4.at(2));
  /* A read attachment's, too. */
  test_assert(w2.lib.markDeleted(y).ok());
  IoWorld w5;
  test_assert(w5.load(w2.save()).ok());
  test_assert(static_cast<Note *>(w5.lib.attachment(w5.at(1), kNote))->about.isNull());
}

static void test_what_is_saved()
{
  IoWorld w;
  const BlockHandle orphan = w.doc(1, "orphan");
  const BlockHandle undo = w.doc(2, "only undo holds it");
  const BlockHandle used = w.doc(3, "in use");
  const BlockHandle fake = w.doc(4, "fake");
  const BlockHandle child = w.doc(5, "child");
  const BlockHandle weak = w.doc(6, "weakly referenced");
  w.lib.pin(undo, PinKind::History);
  w.lib.pin(used, PinKind::Host);
  w.lib.setFlag(fake, BlockFlags::FakeUser, true);
  w.link(fake, child, weak);
  const BlockHandle del = w.doc(7, "deleted");
  w.root(del);
  w.link(del, orphan); /* a deleted block keeps nothing in the file */
  test_assert(w.lib.markDeleted(del).ok());
  SaveReport report;
  IoWorld r;
  test_assert(r.load(w.save(&report)).ok() && report.blocks == 3 && r.lib.size() == 3);
  test_assert(r.at(3) && r.at(4) && r.at(5));
  test_assert(r.data(r.at(4))->parent.target == w.id(weak)); /* kept: it may come back */
  test_assert(r.lib.get(r.at(3))->pins() == 0);
  w.lib.unpin(undo, PinKind::History);
  w.lib.unpin(used, PinKind::Host);

  /* A Doomed block is gone from the file, also while Host pins hold it. */
  w.lib.pin(del);
  test_assert(w.lib.destroyFromHistory(del).ok());
  test_assert(w.lib.get(del)->state() == BlockState::Doomed);
  IoWorld r2;
  test_assert(r2.load(w.save()).ok() && r2.lib.size() == 2);
  w.lib.unpin(del);

  /* A placeholder that only stands for a missing block is written for what it carries, if
   * something needs it. */
  IoWorld w3;
  const auto missing = [&](uint64_t n, const char *name) {
    return placeholder(w3, cid(n), "", name, PlaceholderReason::MissingBlock);
  };
  const BlockHandle needs = w3.doc(1, "needs");
  w3.root(needs);
  missing(2, "nobody needs it");
  w3.addOptEntry(needs, w3.id(missing(3, "")));
  test_assert(w3.lib.setSlot(missing(4, ""), kDoc, 1, BlockId(), RefKind::Weak).ok());
  w3.addOptEntry(needs, cid(4));
  w3.note(missing(5, ""), 1, BlockId());
  w3.addOptEntry(needs, cid(5));
  missing(6, "named");
  w3.addOptEntry(needs, cid(6));
  const Bytes bytes = w3.save(&report);
  test_assert(report.blocks == 4);
  IoWorld r3;
  test_assert(r3.load(bytes).ok() && r3.lib.size() == 5 && !r3.at(2));
  test_assert(r3.at(3) && r3.lib.slot(r3.at(4), kDoc, 1) &&
              r3.lib.attachment(r3.at(5), kNote));
  test_assert(sameBytes(r3.save(), bytes));
  Knows k;
  k.note = false;
  IoWorld r4(k);
  test_assert(r4.load(bytes).ok() && sameBytes(r4.save(), bytes));
}

static void test_transient()
{
  IoWorld w;
  const BlockHandle root = w.doc(1, "root");
  const BlockHandle through = w.doc(2, "through");
  const BlockHandle both = w.doc(3, "both");
  Thing *t = litestl::alloc::New<Thing>("Thing");
  const BlockId tid = cid(10);
  Result<BlockHandle> th = w.lib.createWithId(kTransient, tid, "cache", t);
  test_assert(th.ok());
  w.root(root);
  w.link(root, *th, both);
  w.addOptEntry(root, w.id(both));
  w.addOptEntry(root, tid);
  test_assert(w.lib.writeRef(t->owner, w.id(through)).ok());
  test_assert(w.lib.writeRef(t->material, w.id(both)).ok());
  /* Not reachable: not reported. */
  test_assert(
      w.lib.create(kTransient, "loose", litestl::alloc::New<Thing>("Thing")).ok());
  test_assert(w.lib.attach(root, kBare, &w).ok());
  SaveReport report;
  const Bytes bytes = w.save(&report);
  test_assert(report.blocks == 3 && report.hostPinned.size() == 0);
  test_assert(report.transient.size() == 1 && report.transient[0] == *th);
  test_assert(report.throughTransient.size() == 1 &&
              report.throughTransient[0] == through);
  IoWorld r;
  test_assert(r.load(bytes).ok() && r.lib.size() == 4);
  const BlockHandle missing = r.lib.find(tid);
  test_assert(r.lib.get(missing)->placeholderReason() == PlaceholderReason::MissingBlock);
  test_assert(r.data(r.at(1))->entries[1].refs[0].target == tid); /* not nulled */
  /* The host adopts it under its stable id. */
  test_assert(
      r.lib.createWithId(kTransient, tid, "cache", litestl::alloc::New<Thing>("T")).ok());
  test_assert(r.lib.resolveRef(r.data(r.at(1))->mesh) == missing);
  /* The report starts empty each time. */
  IoWorld empty;
  empty.save(&report);
  test_assert(report.blocks == 0 && report.transient.size() == 0 &&
              report.throughTransient.size() == 0);
}

static void test_save_fails()
{
  IoWorld w;
  const BlockHandle a = w.doc(1, "a");
  w.root(a);
  /* test.Thing neither serializes nor is Transient. */
  const BlockHandle loose = makeThing(w.lib, "loose");
  test_assert(saveLibrary(w.lib).ok()); /* unreachable: no matter */
  const BlockHandle loose2 = makeThing(w.lib, "loose2");
  w.root(loose);
  w.root(loose2);
  Result<Bytes> r = saveLibrary(w.lib);
  test_assert(r.status().code() == StatusCode::FailedPrecondition);
  test_assert(hasMsg(r.status(), "Transient: test.Thing") &&
              !strstr(r.status().message(), ", "));
  for (BlockHandle h : {loose, loose2}) {
    w.lib.setFlag(h, BlockFlags::Root, false);
  }

  w.docs.fault = DocFault::Error;
  r = saveLibrary(w.lib);
  test_assert(r.status().code() == StatusCode::Unsupported &&
              hasMsg(r.status(), "saving 'a': not"));
  w.docs.fault = DocFault::Sticky;
  test_assert(hasMsg(saveLibrary(w.lib).status(), "saving 'a': serialize threw"));
  w.docs.fault = DocFault::None;
  w.note(a, 1, BlockId());
  w.notes.fault = DocFault::Error;
  test_assert(
      hasMsg(saveLibrary(w.lib).status(), "attachment 'test.Note': no notes today"));
  w.notes.fault = DocFault::None;
  w.notes.failForeach = true;
  test_assert(hasMsg(saveLibrary(w.lib).status(), "saving 'a': note forEachRef threw"));
  w.notes.failForeach = false;
  test_assert(!w.rt.hasStickyError());

  /* The ref audit: an error with the relations audit on, else an issue. */
  const BlockHandle b = w.doc(2, "b");
  w.link(a, b, b);
  w.docs.fault = DocFault::SkipRef;
  test_assert(saveLibrary(w.lib).status().code() == StatusCode::Internal);
  w.lib.setAuditRelations(false);
  SaveReport report;
  w.save(&report);
  test_assert(report.issues.total == 1 && report.issues.first[0].block == w.id(a));
  test_assert(has(issueText(report.issues), "a: 'a': forEachRef reports"));
  w.docs.fault = DocFault::None;
  w.save(&report);
  test_assert(report.issues.total == 0);

  w.rt.setStickyError(Status::error(StatusCode::Internal, "before"));
  test_assert(saveLibrary(w.lib).status().code() == StatusCode::FailedPrecondition);
  w.rt.takeStickyError().ignore();

  bool busy = false;
  w.docs.onDestroy = [&](const Datablock &) {
    busy = saveLibrary(w.lib).status().code() == StatusCode::Busy;
  };
  const BlockHandle orphan = w.doc(3, "orphan");
  test_assert(orphan && w.lib.collect().ok() && busy);
  w.docs.onDestroy = nullptr;

  /* Also when no provider would be asked to serialize. */
  IoWorld only;
  placeholder(only, cid(1), "x.Type", "p");
  only.doc(2, "orphan");
  busy = false;
  only.docs.onDestroy = [&](const Datablock &) {
    busy = saveLibrary(only.lib).status().code() == StatusCode::Busy;
  };
  test_assert(only.lib.collect().ok() && busy);
  only.docs.onDestroy = nullptr;

  IoIssues issues;
  for (int i = 0; i < 1500; i++) {
    issues.add(BlockId(), "x", Status::error(StatusCode::Internal, "y"));
  }
  test_assert(issues.total == 1500 && int(issues.first.size()) == IoIssues::kMaxKept);
}

/* Libraries of anything the API can make: they save, load the same, and save the same. */
static void test_random()
{
  for (int round = 0; round < 60 * testScale(); round++) {
    SplitMix64 rng(uint64_t(round) * 7919 + 1);
    IoWorld w;
    Vector<BlockId> ids;
    Vector<BlockHandle> blocks;
    const auto name = [&]() {
      std::string s;
      for (uint32_t n = rng.below(6); n > 0; n--) {
        s += char(1 + rng.below(255));
      }
      return s;
    };
    const ClientId clients[] = {
        ClientId::of(kCoredccClient), kAcme, ClientId(), ClientId{rng.next()}};
    const int count = 1 + int(rng.below(30));
    for (int i = 0; i < count; i++) {
      const BlockId id{clients[rng.below(4)],
                       LocalId{rng.below(2) ? rng.next() : 0, rng.next() | 1}};
      if (w.lib.find(id)) {
        continue;
      }
      BlockHandle h;
      const bool live = id.client == clients[0] || id.client == clients[1];
      switch (live ? rng.below(4) : 2 + rng.below(2)) {
      case 0:
      case 1: {
        Doc *d = litestl::alloc::New<Doc>("Doc");
        d->bag = makeDocBag(w.docs);
        d->number = int64_t(rng.next());
        h = *w.lib.createWithId(id.client == kAcme ? kAcmeDoc : kDoc, id, name(), d);
        break;
      }
      case 2: {
        Bytes b;
        for (uint32_t n = rng.below(2) ? rng.below(1500) : 0; n > 0; n--) {
          b.append(uint8_t(rng.below(rng.below(2) ? 256 : 2)));
        }
        h = placeholder(w,
                        id,
                        ("x" + name()).c_str(),
                        name().c_str(),
                        PlaceholderReason::MissingType,
                        payload(uint32_t(rng.next()), std::move(b)),
                        name().c_str());
        break;
      }
      default:
        h = placeholder(w,
                        id,
                        ("y" + name()).c_str(),
                        name().c_str(),
                        PlaceholderReason::MissingBlock,
                        payload(uint32_t(rng.next()), Bytes()));
      }
      ids.append(w.id(h));
      blocks.append(h);
    }
    const auto target = [&]() {
      return rng.below(5) == 0 ? BlockId() : ids[int(rng.below(uint32_t(ids.size())))];
    };
    for (BlockHandle h : blocks) {
      w.root(h); /* everything is saved */
      w.lib.setFlag(h, BlockFlags::FakeUser, rng.below(2) == 0);
      const Datablock &b = *w.lib.get(h);
      if (b.state() == BlockState::Live) {
        Doc &d = *w.data(h);
        d.mesh.target = target();
        d.parent.target = target();
        for (uint32_t n = rng.below(4); n > 0; n--) {
          const BlockId t = target();
          d.entries.append(
              t.isNull()
                  ? entryOf(name().c_str(), PropValue::ofInt(int64_t(rng.next())))
                  : entryOf(name().c_str(), pointerTo(t, "k"), RefKind(rng.below(3))));
        }
      }
      for (uint32_t n = rng.below(4); n > 0; n--) {
        const TypeId ns = rng.below(2) ? kDoc : TypeId{rng.next() | 1};
        const BlockId t = target();
        test_assert(w.lib
                        .setSlot(h,
                                 ns,
                                 int64_t(rng.next()),
                                 t,
                                 t.isNull() ? RefKind::OptStrong : RefKind(rng.below(3)))
                        .ok());
      }
      if (rng.below(3) == 0) {
        w.note(h, int(rng.next()), target())->about.kind = RefKind(rng.below(3));
      }
      if (b.state() == BlockState::Live && rng.below(8) == 0) {
        test_assert(w.lib.onHostDestroyed(b.id()).ok());
      }
    }
    w.lib.noteRefsChanged();
    /* A Note's `about` is Strong when it is read, whatever it was. */
    w.lib.forEach([&](BlockHandle h, const Datablock &) {
      if (void *n = w.lib.attachment(h, kNote)) {
        static_cast<Note *>(n)->about.kind = RefKind::Strong;
      }
    });
    if (!roundTrips(w, w.save())) {
      fprintf(stderr, "round %d\n", round);
      test_assert(false);
    }
  }
}

/* Saving and loading are linear. */
static void test_scale()
{
  const auto t0 = std::chrono::steady_clock::now();
  IoWorld w;
  w.lib.setAuditRelations(false);
  const int n = 20000;
  BlockHandle prev;
  for (int i = 1; i <= n; i++) {
    const BlockHandle h = w.doc(uint64_t(i) * 0x9E3779B97F4A7C15ull, "one of many");
    w.root(h);
    w.data(h)->mesh.target = prev ? w.id(prev) : BlockId();
    prev = h;
  }
  w.lib.noteRefsChanged();
  const Bytes bytes = w.save();
  IoWorld r;
  LoadReport report;
  test_assert(r.load(bytes, &report).ok() && r.lib.size() == n &&
              report.issues.total == 0);
  const double s =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  test_assert(s < 60);
}

int main()
{
  test_golden();
  test_round_trip();
  test_attachment_order();
  test_alias();
  test_deleted_targets();
  test_what_is_saved();
  test_transient();
  test_save_fails();
  test_random();
  test_scale();
  return test_end();
}
