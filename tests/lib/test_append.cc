/* Appending a file to a library: new ids, references, refusals, and what it leaves alone. */
#include "io_fixture.h"

#include "fuzz.h"

#include "litestl/util/alloc.h"

#include <chrono>

test_init;

using namespace coredcc;
using namespace coredcc_test;

namespace {

BlockId cid(uint64_t n, ClientId client = ClientId::of(kCoredccClient))
{
  return BlockId{client, LocalId{7, n}};
}

/* a -> b -> a, b has a weak ref to c, a has an entry, a slot and a note to c. a is a root. */
Bytes sample()
{
  IoWorld w;
  const BlockHandle a = w.doc(1, "a"), b = w.doc(2, "b", kAcmeDoc), c = w.doc(3, "c");
  w.root(a);
  w.lib.setFlag(c, BlockFlags::FakeUser, true);
  w.link(a, b);
  w.link(b, a, c);
  w.addOptEntry(a, w.id(c));
  test_assert(w.data(a)->bag.set("target", pointerTo(w.id(c))).ok());
  w.data(a)->bagTarget.target = w.id(c);
  test_assert(w.lib.setSlot(a, kDoc, 4, w.id(b), RefKind::OptStrong).ok());
  w.note(a, 5, w.id(c));
  w.lib.noteRefsChanged();
  return w.save();
}

std::string snapshotOf(const IoWorld &w)
{
  std::string s;
  w.lib.forEach([&](BlockHandle h, const Datablock &b) {
    s += std::to_string(h.index) + b.name().c_str() + std::to_string(int(b.state())) + ";";
  });
  return s + std::to_string(w.lib.epoch());
}

/* Refused, with nothing changed. */
bool refused(IoWorld &w, const Bytes &file, StatusCode code, const char *what,
             const LoadOptions &options = {})
{
  const std::string before = snapshotOf(w);
  Result<AppendResult> r = w.append(file, options);
  if (r.ok() || r.status().code() != code) {
    fprintf(stderr, "%s\n", r.ok() ? "ok" : r.status().message());
    return false;
  }
  return hasMsg(r.status(), what) && snapshotOf(w) == before && !w.rt.hasStickyError();
}

} // namespace

static void test_twice()
{
  const Bytes bytes = sample();
  IoWorld w;
  int rekeys = 0;
  w.docs.onRekey = [&](const Datablock &b) {
    test_assert(w.lib.busy() && b.id() == cid(2, kAcme) && b.type() == kAcmeDoc);
    test_assert(strcmp(b.name().c_str(), "b") == 0 && b.state() == BlockState::Placeholder);
    test_assert(!b.clientData());
    return cid(100 + uint64_t(rekeys++), kAcme);
  };
  const BlockHandle own = w.doc(1, "mine"); /* has an id of the file's */
  BlockHandle copies[2];
  for (int i = 0; i < 2; i++) {
    Result<AppendResult> r = w.append(bytes);
    test_assert(r.ok() && r->ids.size() == 3 && r->report.blocks.size() == 3);
    test_assert(r->report.records == 3 && r->report.issues.total == 0);
    const BlockId a = r->ids.find(cid(1)), b = r->ids.find(cid(2, kAcme)), c = r->ids.find(cid(3));
    test_assert(b == cid(100 + uint64_t(i), kAcme));
    test_assert(a.client == ClientId::of(kCoredccClient) && a != cid(1) && c != cid(3));
    test_assert(r->ids.find(cid(4)).isNull());
    const BlockHandle ha = w.lib.find(a), hb = w.lib.find(b), hc = w.lib.find(c);
    test_assert(ha && hb && hc);
    copies[i] = ha;
    const Doc &da = *w.data(ha);
    test_assert(da.number == 10 && da.mesh.target == b && w.data(hb)->mesh.target == a &&
                w.data(hb)->parent.target == c);
    test_assert(da.entries[0].refs[0].target == c);
    test_assert(da.entries[0].value.tryPointer()->owner.block == c);
    test_assert(da.bag.get("target")->tryPointer()->owner.block == c && da.bagTarget.target == c);
    test_assert(w.lib.slot(ha, kDoc, 4)->target == b);
    test_assert(static_cast<Note *>(w.lib.attachment(ha, kNote))->about.target == c);
    /* Kept, but not roots here. */
    test_assert(w.lib.get(ha)->flags() == BlockFlags::FakeUser);
    test_assert(w.lib.get(hc)->flags() == BlockFlags::FakeUser);
    test_assert(w.lib.get(hb)->flags() == BlockFlags::None);
  }
  test_assert(rekeys == 2 && copies[0] != copies[1] && w.lib.size() == 7);
  test_assert(w.lib.get(own)->state() == BlockState::Live && w.data(own)->mesh.isNull());
  test_assert(*w.lib.collect() == 1 && w.lib.size() == 6); /* "mine" was an orphan */

  /* What was appended saves and loads. */
  IoWorld r;
  test_assert(r.load(w.save()).ok() && sameLibraries(w.lib, r.lib));
  w.docs.onRekey = nullptr;
}

static void test_outside_refs()
{
  IoWorld from;
  const BlockHandle a = from.doc(1, "a");
  from.root(a);
  from.link(a, BlockHandle());
  from.data(a)->mesh.target = cid(50);   /* here */
  from.data(a)->parent.target = cid(51); /* nowhere, weak */
  test_assert(from.lib.setSlot(a, kDoc, 0, cid(52), RefKind::Strong).ok());
  from.lib.noteRefsChanged();
  const Bytes bytes = from.save();
  IoWorld w;
  const BlockHandle here = w.doc(50, "here");
  Result<AppendResult> r = w.append(bytes);
  test_assert(r.ok() && r->report.records == 1 && r->report.blocks.size() == 2);
  test_assert(r->report.missingBlock == 1 && w.lib.size() == 3 && !w.at(51));
  const BlockHandle ha = r->report.blocks[0];
  test_assert(w.lib.resolveRef(w.data(ha)->mesh) == here);
  test_assert(w.data(ha)->parent.target == cid(51));
  test_assert(w.lib.get(w.at(52))->placeholderReason() == PlaceholderReason::MissingBlock);
  /* Again: the missing block is there by now. */
  r = w.append(bytes);
  test_assert(r.ok() && r->report.blocks.size() == 1 && w.lib.size() == 4);
  /* A deleted block is a target like any other. */
  test_assert(w.lib.markDeleted(here).ok());
  r = w.append(bytes);
  test_assert(r.ok() && r->report.blocks.size() == 1 && w.at(50) == here);

  /* The limit counts what the append adds, and not what is there. */
  LoadOptions options;
  options.limits.maxBlocks = 2;
  IoWorld w2;
  w2.doc(50, "here");
  w2.doc(60, "more");
  w2.doc(61, "and more");
  r = w2.append(bytes, options);
  test_assert(r.ok() && r->report.missingBlock == 1 && w2.lib.size() == 5);
  options.limits.maxBlocks = 1;
  IoWorld w3;
  w3.doc(50, "here");
  test_assert(refused(w3, bytes, StatusCode::OutOfRange, "more than 1 records", options));
}

static void test_placeholders()
{
  const Bytes bytes = sample();
  /* The coredcc client's blocks are appended without their type, too. */
  Knows k;
  k.doc = k.note = false;
  IoWorld w(k);
  LoadOptions options;
  options.sourceName = "lib.cdcc";
  Result<AppendResult> r = w.append(bytes, options);
  test_assert(r.ok() && r->report.missingType == 2);
  const BlockHandle a = w.lib.find(r->ids.find(cid(1)));
  const BlockId b = r->ids.find(cid(2, kAcme)), c = r->ids.find(cid(3));
  int toB = 0, toC = 0;
  test_assert(w.lib.foreachRef(a, [&](RefCell &cell) {
    toB += cell.target == b ? 1 : 0;
    toC += cell.target == c ? 1 : 0;
    test_assert(cell.target != cid(3) && cell.target != cid(2, kAcme));
  }).ok());
  test_assert(toB == 2 && toC >= 3); /* tables, slot and opaque attachment */
  test_assert(strcmp(w.lib.get(a)->sourceName().c_str(), "lib.cdcc") == 0);
  registerDoc(w.rt, w.docs);
  registerNote(w.rt, w.notes);
  test_assert(detail::resolvePlaceholders(w.lib).ok());
  test_assert(w.data(a)->entries[0].value.tryPointer()->owner.block == c);
}

static void test_refused()
{
  const Bytes bytes = sample();
  {
    Knows k;
    k.acme = false;
    IoWorld w(k);
    w.doc(9, "x");
    test_assert(refused(w, bytes, StatusCode::FailedPrecondition, "can't get ids here: acme.Doc"));
    test_assert(!w.lib.fileNames().nameOf(IdDomain::Type, kAcmeDoc.value));
  }
  IoWorld w;
  w.doc(9, "x", kAcmeDoc);
  const auto answering = [&](BlockId id) {
    w.docs.onRekey = [id](const Datablock &) { return id; };
    return refused(w, bytes, StatusCode::FailedPrecondition, "acme.Doc");
  };
  test_assert(answering(BlockId()));
  test_assert(answering(cid(9, kAcme)));  /* in use */
  test_assert(answering(cid(10)));        /* another client's */
  test_assert(answering(BlockId{kAcme, LocalId()}));
  w.docs.onRekey = [&](const Datablock &) {
    w.rt.setStickyError(Status::error(StatusCode::Internal, "rekey threw"));
    return BlockId();
  };
  test_assert(refused(w, bytes, StatusCode::Internal, "rekey threw"));
  /* The library is busy to a rekey. */
  w.docs.onRekey = [&](const Datablock &b) {
    test_assert(w.append(bytes).status().code() == StatusCode::Busy);
    test_assert(w.lib.create(kDoc, "x", nullptr).status().code() == StatusCode::Busy);
    return b.id();
  };
  test_assert(w.append(bytes).ok() && w.at(2, kAcme));
  w.docs.onRekey = nullptr;

  /* Every type that can't is listed, once. */
  {
    IoWorld from;
    for (uint64_t n = 1; n <= 3; n++) {
      from.root(from.doc(n, "plain", kAcmePlain));
    }
    const BlockId own = cid(4, kAcme);
    PlaceholderInfo info;
    info.typeName = "acme.Lost";
    test_assert(from.lib.addPlaceholder(own, std::move(info)).ok());
    PlaceholderInfo bare;
    bare.reason = PlaceholderReason::MissingBlock;
    bare.name = "named";
    Result<BlockHandle> h = from.lib.addPlaceholder(cid(5, kAcme), std::move(bare));
    test_assert(h.ok());
    from.root(*h);
    IoWorld into;
    test_assert(refused(into, from.save(), StatusCode::FailedPrecondition,
                        "here: (no type), acme.Lost, acme.Plain"));
  }
  /* Two answers that are the same. */
  {
    IoWorld from;
    from.root(from.doc(1, "one", kAcmeDoc));
    from.root(from.doc(2, "two", kAcmeDoc));
    IoWorld into;
    into.docs.onRekey = [](const Datablock &) { return cid(1, kAcme); };
    test_assert(refused(into, from.save(), StatusCode::FailedPrecondition, "acme.Doc"));
    into.docs.onRekey = nullptr;
  }
  /* Damage, and the states a load is refused in. */
  Bytes damaged = bytes;
  damaged[60] ^= 1;
  test_assert(refused(w, damaged, StatusCode::InvalidArgument, "checksum"));
  {
    EvaluationScope scope(w.rt);
    test_assert(w.append(bytes).status().code() == StatusCode::Busy);
  }
  w.rt.setStickyError(Status::error(StatusCode::Internal, "before"));
  test_assert(w.append(bytes).status().code() == StatusCode::FailedPrecondition);
  w.rt.takeStickyError().ignore();
  IoWorld clash;
  test_assert(clash.lib.fileNames().debugAddAs(IdDomain::Type, TypeId::of("x.Type").value, "other"));
  RawFile f;
  f.records.push_back(RawRecord());
  test_assert(refused(clash, f.file(), StatusCode::InvalidArgument, "has the id of 'other'"));
  /* A GUID source that repeats itself. */
  IoWorld w2;
  w2.doc(1, "x");
  w2.rt.setGuidSource([]() { return LocalId{7, 1}; });
  test_assert(refused(w2, f.file(), StatusCode::Internal, "GUID source"));
  w2.rt.setGuidSource([]() { return LocalId{7, 2}; });
  RawFile two = f;
  two.records.push_back(RawRecord());
  two.records[1].local.lo = 5;
  test_assert(refused(w2, two.file(), StatusCode::Internal, "GUID source"));
  w2.rt.setGuidSource({});
}

static void test_aliases()
{
  /* A file from before a rename, and one block of each name with one LocalId. */
  RawFile f;
  f.names = {"c.old", "x.Type", "coredcc"};
  for (uint32_t c : {1u, 3u}) {
    RawRecord r;
    r.client = c;
    r.slots = [c](BytesWriter &out) {
      out.u32(1);
      out.u32(2);
      out.i64(0);
      RawRecord::row(out, detail::libio::kRowStrong, LocalId{7, 1}, c);
    };
    f.records.push_back(std::move(r));
  }
  IoWorld w;
  const char *aliases[] = {"c.old"};
  test_assert(w.rt.ext<ClientRegistry>().registerClient(kCoredccClient, aliases).ok());
  Result<AppendResult> r = w.append(f.file());
  test_assert(r.ok() && w.lib.size() == 2 && r->ids.size() == 2);
  for (int i = 0; i < 2; i++) {
    const BlockHandle h = r->report.blocks[i];
    test_assert(w.lib.slot(h, TypeId::of("x.Type"), 0)->target == w.id(h));
  }
  /* A reference by the current name finds a record stored under the old one. */
  f.records.pop_back();
  f.records[0].slots = [](BytesWriter &out) {
    out.u32(1);
    out.u32(2);
    out.i64(0);
    RawRecord::row(out, detail::libio::kRowStrong, LocalId{7, 1}, 3);
  };
  r = w.append(f.file());
  test_assert(r.ok() && r->report.blocks.size() == 1);
  const BlockHandle h = r->report.blocks[0];
  test_assert(w.lib.slot(h, TypeId::of("x.Type"), 0)->target == w.id(h));
  /* And the other way around. */
  f.records[0].client = 3;
  f.records[0].slots = [](BytesWriter &out) {
    out.u32(1);
    out.u32(2);
    out.i64(0);
    RawRecord::row(out, detail::libio::kRowStrong, LocalId{7, 1}, 1);
  };
  r = w.append(f.file());
  test_assert(r.ok() && r->report.blocks.size() == 1);
  const BlockHandle g = r->report.blocks[0];
  test_assert(w.lib.slot(g, TypeId::of("x.Type"), 0)->target == w.id(g));
}

/* Whatever a damaged file holds: all or nothing, and the result saves and loads. */
static void test_fuzz()
{
  const Bytes good = sample();
  LoadOptions options;
  options.limits.maxBytes = uint64_t(4) << 20;
  const auto t0 = std::chrono::steady_clock::now();
  SplitMix64 rng(5);
  int appended = 0, refusals = 0;
  for (int i = 8; i < int(good.size()) - 8; i++) {
    for (int k = 0; k < 3; k++) {
      Bytes b = good;
      b[i] = k == 0 ? uint8_t(b[i] ^ (1 << (i % 8))) : k == 1 ? 0xff : uint8_t(rng.next());
      fixUp(b);
      IoWorld w;
      w.docs.onRekey = [&](const Datablock &block) {
        return cid(1000 + uint64_t(w.lib.size()) + rng.next() % 100000, block.id().client);
      };
      const BlockHandle own = w.doc(1, "mine"), kept = w.doc(2, "kept");
      w.root(own);
      w.link(own, kept);
      const size_t start = litestl::alloc::getMemorySize();
      const std::string before = snapshotOf(w);
      Result<AppendResult> r = w.append(b, options);
      test_assert(litestl::alloc::getMemorySize() - start < size_t(8) << 20);
      if (!r.ok()) {
        refusals++;
        test_assert(snapshotOf(w) == before && !w.rt.hasStickyError());
        continue;
      }
      appended++;
      test_assert(w.lib.validate().ok() && w.data(own)->mesh.target == w.id(kept));
      test_assert(w.lib.get(own)->state() == BlockState::Live &&
                  w.lib.get(own)->hasFlag(BlockFlags::Root));
      Result<Bytes> saved = saveLibrary(w.lib);
      test_assert(saved.ok());
      if (saved.ok()) {
        IoWorld again;
        test_assert(detail::loadLibrary(again.lib, view(*saved), {}).ok());
      }
      w.docs.onRekey = nullptr;
      w.lib.setAuditRelations(false);
    }
  }
  test_assert(appended > 50 && refusals > 50);
  test_assert(std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 60);
}

int main()
{
  test_fuzz();
  test_twice();
  test_outside_refs();
  test_placeholders();
  test_refused();
  test_aliases();
  return test_end();
}
