/* Filling MissingBlock records, and taking it back (unadopt, readopt), by the host and by
 * an append. */
#include "coredcc/lib/duplicate.h"
#include "coredcc/lib/pin.h"

#include "io_fixture.h"

test_init;

using namespace coredcc;
using namespace coredcc_test;

namespace {

BlockId cid(uint64_t n, ClientId client = ClientId::of(kCoredccClient))
{
  return BlockId{client, LocalId{7, n}};
}

const BlockId kHosted = cid(5, kAcme);

/* A root "a" with an OptStrong reference to a block that was Detached: the record has a
 * name, references (to c), a slot and a note. */
Bytes sample()
{
  IoWorld w;
  const BlockHandle a = w.doc(1, "a"), c = w.doc(3, "c");
  const BlockHandle h = w.doc(5, "actor", kAcmeDoc);
  w.root(a);
  w.link(a, BlockHandle());
  w.addOptEntry(a, kHosted);
  w.link(h, c, a);
  w.note(h, 5, w.id(c));
  test_assert(w.lib.setSlot(h, kDoc, 4, w.id(a), RefKind::OptStrong).ok());
  test_assert(w.lib.onHostDestroyed(kHosted).ok());
  return w.save();
}

Doc *newDoc(IoWorld &w, BlockId mesh = BlockId())
{
  Doc *d = litestl::alloc::New<Doc>("Doc");
  d->bag = makeDocBag(w.docs);
  d->number = 77;
  d->mesh.target = mesh;
  return d;
}

bool isMissing(const IoWorld &w, BlockHandle h)
{
  const Datablock *b = w.lib.get(h);
  return b && b->state() == BlockState::Placeholder &&
         b->placeholderReason() == PlaceholderReason::MissingBlock && !w.lib.adopted(h);
}

} // namespace

static void test_and_back()
{
  const Bytes bytes = sample();
  IoWorld w;
  test_assert(w.load(bytes).ok() && sameBytes(w.save(), bytes));
  const BlockHandle h = w.lib.find(kHosted);
  test_assert(isMissing(w, h));
  test_assert(w.lib.unadopt(h).status().code() == StatusCode::FailedPrecondition);
  test_assert(w.lib.unadopt(BlockHandle()).status().code() == StatusCode::NotFound);

  const BlockHandle extra = w.doc(8, "extra");
  test_assert(w.lib.createWithId(kAcmeDoc, kHosted, "mine", newDoc(w, w.id(extra))).ok());
  test_assert(w.lib.adopted(h) && w.lib.get(h)->state() == BlockState::Live);
  /* What came since: a slot, another's target, an attachment, a flag and an override. */
  test_assert(w.lib.setSlot(h, kThing, 9, w.id(extra), RefKind::OptStrong).ok());
  test_assert(w.lib.setSlot(h, kDoc, 4, w.id(extra), RefKind::OptStrong).ok());
  OpaqueAttachment later;
  later.typeName = "x.Later";
  later.payload.bytes = bytesOf({1, 2});
  test_assert(w.lib.attachOpaque(h, later).ok());
  test_assert(w.lib.attach(h, kBare, &w).ok());
  w.lib.setFlag(h, BlockFlags::FakeUser, true);
  const Bytes filled = w.save();
  test_assert(!sameBytes(filled, bytes));

  const uint64_t changes = w.lib.get(h)->stateChanges();
  const int size = w.lib.size();
  Result<BlockHandle> kept = w.lib.unadopt(h);
  test_assert(kept.ok() && *kept != h && w.lib.size() == size && w.valid());
  LibraryPin pin(w.lib, *kept);
  test_assert(isMissing(w, h) && w.lib.find(kHosted) == h);
  test_assert(w.lib.get(h)->stateChanges() > changes && !w.lib.data(h));
  test_assert(strcmp(w.lib.get(h)->name().c_str(), "actor") == 0 &&
              w.lib.get(h)->flags() == BlockFlags::None);
  test_assert(!w.lib.slot(h, kThing, 9) && w.lib.slot(h, kDoc, 4)->target == cid(1));
  test_assert(w.lib.attachment(h, kNote) && !w.lib.attachment(h, kBare));
  test_assert(!w.lib.hasAttachment(h, TypeId::of("x.Later")));
  test_assert(w.lib.get(*kept)->state() == BlockState::Doomed && pin.dataAlive());
  test_assert(w.lib.get(*kept)->id() == kHosted && w.lib.get(*kept)->clientData());
  test_assert(sameBytes(w.save(), bytes));
  /* What the data references stays. */
  test_assert(*w.lib.collect() == 0 && w.lib.get(extra));
  test_assert(w.lib.unadopt(h).status().code() == StatusCode::FailedPrecondition);

  test_assert(w.lib.readopt(h, h).code() == StatusCode::NotFound);
  test_assert(w.lib.readopt(BlockHandle(), *kept).code() == StatusCode::NotFound);
  test_assert(w.lib.readopt(extra, *kept).code() == StatusCode::NotFound);
  test_assert(w.lib.readopt(h, *kept).ok() && w.valid() && w.lib.size() == size);
  pin.reset(); /* the record it named is gone */
  test_assert(!w.lib.get(*kept) && w.lib.get(h)->state() == BlockState::Live);
  test_assert(w.data(h)->number == 77 && w.lib.attachment(h, kBare) == &w);
  test_assert(sameBytes(w.save(), filled));

  /* Taken back for good: the data goes, the record stays. */
  kept = w.lib.unadopt(h);
  test_assert(kept.ok());
  const int destroyed = w.docs.destroyed;
  LibraryPin(w.lib, *kept).reset();
  test_assert(!w.lib.get(*kept) && w.docs.destroyed == destroyed + 1 && w.valid());
  test_assert(w.lib.readopt(h, *kept).code() == StatusCode::NotFound);
  test_assert(isMissing(w, h) && sameBytes(w.save(), bytes));
  test_assert(*w.lib.collect() == 1); /* extra */
  /* And the host can fill it again. */
  test_assert(w.lib.createWithId(kAcmeDoc, kHosted, "again", newDoc(w)).ok());
  test_assert(w.lib.unadopt(h).ok() && sameBytes(w.save(), bytes));
}

static void test_refusals()
{
  IoWorld w;
  test_assert(w.load(sample()).ok());
  const BlockHandle h = w.lib.find(kHosted);
  test_assert(w.lib.createWithId(kAcmeDoc, kHosted, "mine", newDoc(w)).ok());
  test_assert(w.lib.markDeleted(h).ok());
  test_assert(hasMsg(w.lib.unadopt(h).status(), "is deleted"));
  test_assert(w.lib.restoreFromHistory(h).ok());
  test_assert(w.lib.attach(h, kBare, &w).ok());
  Result<BlockHandle> kept = w.lib.unadopt(h);
  test_assert(kept.ok());
  LibraryPin pin(w.lib, *kept);
  /* An attachment of the same type in between, or another block. */
  test_assert(w.lib.attach(h, kBare, &w).ok());
  test_assert(hasMsg(w.lib.readopt(h, *kept), "got an attachment"));
  test_assert(w.lib.detach(h, kBare).ok());
  test_assert(w.lib.createWithId(kAcmeDoc, kHosted, "other", newDoc(w)).ok());
  test_assert(hasMsg(w.lib.readopt(h, *kept), "isn't missing any more"));
  /* That one can be taken back too, while the first is there. */
  Result<BlockHandle> second = w.lib.unadopt(h);
  test_assert(second.ok() && *second != *kept);
  test_assert(w.lib.readopt(h, *kept).ok());
  test_assert(strcmp(w.lib.get(h)->name().c_str(), "mine") == 0);
  LibraryPin(w.lib, *second).reset();
  test_assert(!w.lib.get(*second) && w.valid());
  /* A provider can't. */
  bool called = false;
  w.docs.onDuplicate = [&]() {
    called = true;
    test_assert(w.lib.unadopt(h).status().code() == StatusCode::Busy);
    test_assert(w.lib.readopt(h, h).code() == StatusCode::Busy);
  };
  const BlockHandle set[] = {w.at(1)};
  test_assert(duplicateBlocks(w.lib, set).ok() && called);
  w.docs.onDuplicate = nullptr;
}

/* Whoever pinned the block holds its data, whatever becomes of the record. */
static void test_host_pins()
{
  for (int end = 0; end < 4; end++) {
    IoWorld w;
    test_assert(w.load(sample()).ok());
    const BlockHandle h = w.lib.find(kHosted);
    test_assert(w.lib.createWithId(kAcmeDoc, kHosted, "mine", newDoc(w)).ok());
    LibraryPin user(w.lib, h);
    Result<BlockHandle> kept = w.lib.unadopt(h);
    test_assert(kept.ok() && !user.dataAlive());
    LibraryPin pin(w.lib, *kept);
    const int destroyed = w.docs.destroyed;
    if (end == 0) { /* the history's goes first */
      pin.reset();
      test_assert(w.lib.get(*kept) && w.docs.destroyed == destroyed);
      user.reset();
      test_assert(!w.lib.get(*kept) && w.docs.destroyed == destroyed + 1);
    } else if (end == 1) { /* the user's */
      user.reset();
      test_assert(w.lib.get(*kept) && w.docs.destroyed == destroyed);
      pin.reset();
      test_assert(!w.lib.get(*kept) && w.docs.destroyed == destroyed + 1);
    } else if (end == 2) { /* a redo */
      test_assert(w.lib.readopt(h, *kept).ok() && user.dataAlive());
      pin.reset();
      user.reset();
      test_assert(w.docs.destroyed == destroyed && w.lib.data(h));
    } else { /* the record goes before either */
      test_assert(w.lib.markDeleted(h).ok() && w.lib.destroyFromHistory(h).ok());
      test_assert(w.lib.get(h)->state() == BlockState::Doomed);
      pin.reset();
      test_assert(w.lib.get(*kept));
      user.reset();
      test_assert(!w.lib.get(*kept) && !w.lib.get(h) &&
                  w.docs.destroyed == destroyed + 1);
    }
    test_assert(w.valid());
  }
  /* The record is destroyed outright. */
  IoWorld w;
  PlaceholderInfo bare;
  bare.reason = PlaceholderReason::MissingBlock;
  Result<BlockHandle> h = w.lib.addPlaceholder(kHosted, bare);
  test_assert(h.ok() && w.lib.createWithId(kAcmeDoc, kHosted, "x", newDoc(w)).ok());
  w.lib.pin(*h);
  Result<BlockHandle> kept = w.lib.unadopt(*h);
  test_assert(kept.ok() && w.lib.get(*kept)->pins(PinKind::Host) == 1);
  w.lib.unpin(*h);
  test_assert(!w.lib.get(*kept));
  test_assert(*w.lib.collect() == 1 && w.lib.size() == 0);
}

/* The host destroys and restores its block while the record stands for a missing one. */
static void test_host_changes()
{
  IoWorld w;
  test_assert(w.load(sample()).ok());
  const BlockHandle h = w.lib.find(kHosted);
  test_assert(w.lib.onHostDestroyed(kHosted).code() == StatusCode::FailedPrecondition);
  test_assert(w.lib.createWithId(kAcmeDoc, kHosted, "mine", newDoc(w)).ok());
  Result<BlockHandle> kept = w.lib.unadopt(h);
  LibraryPin pin(w.lib, *kept);
  test_assert(w.lib.onHostRestored(kHosted).code() == StatusCode::FailedPrecondition);
  test_assert(w.lib.onHostDestroyed(kHosted).ok() && !pin.dataAlive());
  test_assert(w.lib.get(h)->state() == BlockState::Placeholder);
  test_assert(w.lib.readopt(h, *kept).ok() &&
              w.lib.get(h)->state() == BlockState::Detached);
  kept = w.lib.unadopt(h);
  pin = LibraryPin(w.lib, *kept);
  test_assert(w.lib.onHostRestored(kHosted).ok() && w.lib.readopt(h, *kept).ok());
  test_assert(w.lib.get(h)->state() == BlockState::Live && w.data(h)->number == 77);
}

/* A field of a newer build that the record had stays with it. */
static void test_extension()
{
  RawFile f;
  f.names[1] = "test.Doc";
  RawRecord r;
  r.as = 1;
  BytesWriter ext;
  ext.u32(5);
  const uint8_t three[] = {1, 2, 3};
  ext.blob(three);
  const Bytes extBytes = ext.take();
  r.extension =
      std::string(reinterpret_cast<const char *>(extBytes.data()), extBytes.size());
  f.records.push_back(r);
  IoWorld w;
  test_assert(w.load(f.file()).ok());
  const BlockHandle h = w.at(1);
  const Bytes bytes = w.save();
  test_assert(w.lib.createWithId(kDoc, cid(1), "x", newDoc(w)).ok() &&
              w.lib.unadopt(h).ok());
  test_assert(w.lib.get(h)->fileExtension().size() == extBytes.size());
  test_assert(sameBytes(w.save(), bytes));
}

/* ---- an append that fills a missing block ---- */

namespace {

/* An acme.Doc "hosted" with an extension, a slot, a note and a reference to "other". */
Bytes library(bool detached = false)
{
  IoWorld w;
  const BlockHandle h = w.doc(20, "hosted", kAcmeDoc), o = w.doc(21, "other");
  w.root(h);
  w.link(h, o);
  w.note(h, 9, w.id(o));
  test_assert(w.lib.setSlot(h, kDoc, 4, w.id(o), RefKind::OptStrong).ok());
  test_assert(w.lib.setSlot(h, kDoc, 5, w.id(o), RefKind::OptStrong).ok());
  if (detached) {
    test_assert(w.lib.onHostDestroyed(w.id(h)).ok());
  }
  return w.save();
}

struct Missing {
  IoWorld w;
  Bytes before;
  BlockHandle h;

  explicit Missing(Knows k = {}) : w(k)
  {
    test_assert(w.load(sample()).ok());
    before = w.save();
    h = w.lib.find(kHosted);
    w.docs.onRekey = [](const Datablock &, const RekeyContext &c) {
      test_assert(c.isFree(kHosted));
      return kHosted;
    };
  }
  ~Missing()
  {
    w.docs.onRekey = nullptr;
  }
};

} // namespace

static void test_append()
{
  Missing m;
  IoWorld &w = m.w;
  Result<AppendResult> r = w.append(library());
  test_assert(r.ok() && r->report.records == 2 && r->report.blocks[0] == m.h);
  test_assert(r->report.missingBlock == 0 && r->report.missingType == 0);
  test_assert(w.lib.adopted(m.h) && w.lib.get(m.h)->state() == BlockState::Live);
  test_assert(strcmp(w.lib.get(m.h)->name().c_str(), "hosted") == 0 &&
              w.data(m.h)->number == 200);
  test_assert(w.lib.get(m.h)->flags() == BlockFlags::FakeUser);
  /* What was there resolves to it. */
  test_assert(w.lib.resolveRef(w.data(w.at(1))->entries[0].refs[0]) == m.h);
  /* The file's slots, and this document's attachment. */
  const BlockId other = r->ids.find(cid(21));
  test_assert(w.lib.slot(m.h, kDoc, 4)->target == other &&
              w.lib.slot(m.h, kDoc, 5)->target == other);
  test_assert(static_cast<Note *>(w.lib.attachment(m.h, kNote))->value == 5);
  test_assert(r->report.issues.total == 1 &&
              has(issueText(r->report.issues), "has an attachment 'test.Note'"));
  IoWorld again;
  test_assert(again.load(w.save()).ok() && sameLibraries(w.lib, again.lib));

  /* Back, and again. */
  const Bytes filled = w.save();
  test_assert(w.lib.markDeleted(r->report.blocks[1]).ok());
  Result<BlockHandle> kept = w.lib.unadopt(m.h);
  test_assert(kept.ok() && isMissing(w, m.h) && sameBytes(w.save(), m.before));
  LibraryPin pin(w.lib, *kept);
  test_assert(w.lib.readopt(m.h, *kept).ok() &&
              w.lib.restoreFromHistory(r->report.blocks[1]).ok());
  test_assert(sameBytes(w.save(), filled));
  /* Another block can't have the id now. */
  w.docs.onRekey = [](const Datablock &, const RekeyContext &c) {
    test_assert(!c.isFree(kHosted));
    return kHosted;
  };
  test_assert(hasMsg(w.append(library()).status(), "can't get ids here: acme.Doc"));
}

/* The type isn't there, or its data doesn't read: it stays a placeholder, with the
 * payload. */
static void test_append_unread()
{
  Missing m;
  IoWorld &w = m.w;
  w.docs.fault = DocFault::Error;
  Result<AppendResult> r = w.append(library());
  w.docs.fault = DocFault::None;
  test_assert(r.ok() && r->report.missingType == 2);
  test_assert(w.lib.get(m.h)->placeholderReason() == PlaceholderReason::MissingType);
  test_assert(w.lib.adopted(m.h) && w.lib.get(m.h)->placeholderPayload().size() > 0);
  Result<BlockHandle> kept = w.lib.unadopt(m.h);
  LibraryPin pin(w.lib, *kept);
  test_assert(kept.ok() && isMissing(w, m.h));
  test_assert(w.lib.readopt(m.h, *kept).ok());
  test_assert(detail::resolvePlaceholders(w.lib).ok() &&
              w.lib.get(m.h)->state() == BlockState::Live && w.data(m.h)->number == 200);
  kept = w.lib.unadopt(m.h); /* Live by now */
  test_assert(kept.ok() && isMissing(w, m.h) &&
              w.lib.get(*kept)->dataState() == BlockState::Live);
}

/* A record without data fills it too, and the host fills that: two steps back. */
static void test_append_no_payload()
{
  Missing m;
  IoWorld &w = m.w;
  Result<AppendResult> r = w.append(library(true));
  test_assert(r.ok() &&
              w.lib.get(m.h)->placeholderReason() == PlaceholderReason::MissingBlock);
  test_assert(strcmp(w.lib.get(m.h)->name().c_str(), "hosted") == 0 &&
              w.lib.adopted(m.h));
  test_assert(w.lib.createWithId(kAcmeDoc, kHosted, "mine", newDoc(w)).ok());
  Result<BlockHandle> data = w.lib.unadopt(m.h);
  test_assert(data.ok() && strcmp(w.lib.get(m.h)->name().c_str(), "hosted") == 0);
  LibraryPin a(w.lib, *data);
  Result<BlockHandle> record = w.lib.unadopt(m.h);
  test_assert(record.ok() && isMissing(w, m.h) &&
              strcmp(w.lib.get(m.h)->name().c_str(), "actor") == 0);
  LibraryPin b(w.lib, *record);
  test_assert(w.lib.readopt(m.h, *record).ok() && w.lib.readopt(m.h, *data).ok());
  test_assert(w.lib.get(m.h)->state() == BlockState::Live && w.data(m.h)->number == 77);
}

static void test_not_free()
{
  const auto refuses = [](IoWorld &w, BlockId id) {
    bool free = true;
    w.docs.onRekey = [&](const Datablock &, const RekeyContext &c) {
      free = c.isFree(id);
      return id;
    };
    const bool ok = w.append(library()).ok();
    w.docs.onRekey = nullptr;
    return !ok && !free;
  };
  const auto missing = [](IoWorld &w, const char *type, PlaceholderReason reason) {
    PlaceholderInfo info;
    info.reason = reason;
    info.typeName = type;
    test_assert(w.lib.addPlaceholder(kHosted, info).ok());
  };
  {
    IoWorld w;
    missing(w, "acme.Plain", PlaceholderReason::MissingBlock); /* another type's */
    test_assert(refuses(w, kHosted));
  }
  {
    IoWorld w;
    missing(w, "acme.Doc", PlaceholderReason::MissingType); /* it has data */
    test_assert(refuses(w, kHosted));
  }
  {
    IoWorld w;
    missing(w, "", PlaceholderReason::MissingBlock); /* of any type */
    test_assert(!refuses(w, kHosted) &&
                w.lib.get(w.lib.find(kHosted))->state() == BlockState::Live);
  }
  {
    IoWorld w;
    missing(w, "", PlaceholderReason::MissingBlock);
    test_assert(w.lib.markDeleted(w.lib.find(kHosted)).ok());
    test_assert(refuses(w, kHosted));
  }
  {
    IoWorld w;
    test_assert(refuses(w, BlockId{kAcme, LocalId()}));
  }
  {
    RawFile f; /* fields of a newer build */
    f.names[0] = "acme";
    f.names[1] = "acme.Doc";
    RawRecord r;
    r.as = 1;
    r.extension = std::string("\5\0\0\0\0\0\0\0", 8);
    f.records.push_back(r);
    IoWorld w;
    test_assert(w.load(f.file()).ok() && refuses(w, cid(1, kAcme)));
  }
}

/* A refill whose file has an extension gives it back with the rest. */
static void test_append_extension()
{
  RawFile f;
  f.names[0] = "acme";
  f.names[1] = "acme.Doc";
  RawRecord r;
  r.as = 1;
  r.extension = std::string("\5\0\0\0\1\0\0\0\7", 9);
  f.records.push_back(r);
  Missing m;
  test_assert(m.w.append(f.file()).ok() && m.w.lib.get(m.h)->fileExtension().size() == 9);
  Result<BlockHandle> kept = m.w.lib.unadopt(m.h);
  test_assert(kept.ok() && m.w.lib.get(m.h)->fileExtension().empty());
  test_assert(sameBytes(m.w.save(), m.before));
  LibraryPin pin(m.w.lib, *kept);
  test_assert(m.w.lib.readopt(m.h, *kept).ok() &&
              m.w.lib.get(m.h)->fileExtension().size() == 9);
}

int main()
{
  test_and_back();
  test_refusals();
  test_host_pins();
  test_host_changes();
  test_extension();
  test_append();
  test_append_unread();
  test_append_no_payload();
  test_not_free();
  test_append_extension();
  return test_end();
}
