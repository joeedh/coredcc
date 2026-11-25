/* Damaged and hostile library sections: every rejection, versions, limits, bombs and
 * fuzzing, within time and memory bounds. */
#include "fuzz.h"
#include "io_fixture.h"

#include "litestl/io/compress.h"
#include "litestl/util/alloc.h"

#include <chrono>

test_init;

using namespace coredcc;
using namespace coredcc_test;
using namespace coredcc::detail::libio;

namespace {

constexpr StatusCode kBad = StatusCode::InvalidArgument;
constexpr StatusCode kRange = StatusCode::OutOfRange;
constexpr StatusCode kUnsupported = StatusCode::Unsupported;

/* Loading fails with `code` and a message that has `what`, and changes nothing. */
bool refused(const Bytes &file,
             StatusCode code,
             const char *what,
             const LoadOptions &options = {})
{
  Runtime rt;
  Library lib(&rt);
  const uint64_t epoch = lib.epoch();
  Result<LoadReport> r = detail::loadLibrary(lib, view(file), options);
  if (r.ok() || r.status().code() != code) {
    fprintf(stderr,
            "%s: %s, wanted %s\n",
            r.ok() ? "Ok" : statusCodeName(r.status().code()),
            r.ok() ? "" : r.status().message(),
            what);
    return false;
  }
  return hasMsg(r.status(), what) && lib.size() == 0 && lib.epoch() == epoch &&
         !rt.hasStickyError() &&
         !lib.fileNames().nameOf(IdDomain::Type, TypeId::of("x.Type").value) &&
         !lib.fileNames().nameOf(IdDomain::Client, ClientId::of("coredcc").value);
}

RawFile oneRecord(litestl::util::function<void(RawRecord &)> change = {})
{
  RawFile f;
  RawRecord r;
  if (change) {
    change(r);
  }
  f.records.push_back(std::move(r));
  return f;
}

Bytes sample()
{
  IoWorld w;
  const BlockHandle a = w.doc(1, "a"), b = w.doc(2, "bb", kAcmeDoc), c = w.doc(3, "c");
  w.root(a);
  w.link(a, b, c);
  test_assert(w.lib.setSlot(a, kDoc, 1, w.id(c), RefKind::Strong).ok());
  test_assert(w.lib.setSlot(a, TypeId{77}, -2, w.id(b), RefKind::OptStrong).ok());
  w.note(a, 5, w.id(c));
  w.addOptEntry(a, w.id(b));
  w.addOptEntry(
      a, w.id(b), "a name that is long and long and long and long and long and long");
  for (int i = 0; i < 12; i++) {
    w.data(c)->entries.append(entryOf("the same entry again", PropValue::ofInt(i)));
  }
  w.data(b)->parent.target = BlockId{kAcme, LocalId{7, 99}};
  w.lib.noteRefsChanged();
  return w.save();
}

double seconds(std::chrono::steady_clock::time_point from)
{
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - from).count();
}

} // namespace

static void test_container()
{
  const Bytes good = oneRecord().file();
  {
    Runtime rt;
    Library lib(&rt);
    test_assert(detail::loadLibrary(lib, view(good), {}).ok() && lib.size() == 1);
  }
  test_assert(refused(Bytes(), kBad, "too short"));
  Bytes b = good;
  b[0] = 'X';
  test_assert(refused(b, kBad, "doesn't start"));
  b = good;
  b[40] ^= 1;
  test_assert(refused(b, kBad, "checksum"));
  b = good;
  b.append(0);
  test_assert(refused(b, kBad, "but says"));
  for (int n = 0; n < int(good.size()); n++) {
    Bytes cut = good;
    cut.resize(n);
    test_assert(refused(cut, kBad, n < 36 ? "too short" : "but says"));
    fixUp(cut);
    Runtime rt;
    Library lib(&rt);
    test_assert(!detail::loadLibrary(lib, view(cut), {}).ok() && lib.size() == 0);
  }

  RawFile f = oneRecord();
  f.file();
  const std::vector<FileBuilder::Chunk> chunks = f.chunks;
  const auto with = [&](auto change) {
    f.chunks = chunks;
    f.version = f.minReader = 1;
    change();
    return f.build();
  };
  for (int i = 0; i < 3; i++) {
    test_assert(
        refused(with([&]() { f.chunks.erase(f.chunks.begin() + i); }), kBad, "missing"));
    test_assert(
        refused(with([&]() { f.chunks.push_back(chunks[i]); }), kBad, "two chunks '"));
    test_assert(
        refused(with([&]() { f.chunks[i].flags |= 4; }), kUnsupported, "has flags"));
    test_assert(refused(with([&]() {
                          f.chunks[i].bytes.append(0);
                          f.chunks[i].raw++;
                        }),
                        kBad,
                        i == 0   ? "NAME: "
                        : i == 1 ? "BLCK: "
                                 : "DATA: "));
  }
  test_assert(
      refused(with([&]() { f.chunks[0].raw++; }), kBad, "NAME says the wrong size"));
  test_assert(refused(with([&]() { f.chunks[2].flags |= 2; }), kBad, "DATA is never"));
  test_assert(refused(with([&]() { f.chunks[1].flags |= 2; }), kBad, "BLCK: "));
  test_assert(
      refused(with([&]() { f.chunks.resize(17, chunks[0]); }), kBad, "too many chunks"));
  test_assert(refused(with([&]() { f.version = 0; }), kBad, "version numbers"));
  test_assert(refused(with([&]() { f.minReader = 0; }), kBad, "version numbers"));
  test_assert(refused(with([&]() {
                        f.version = 1;
                        f.minReader = 2;
                      }),
                      kUnsupported,
                      "newer build"));
  /* A newer file says so even when the rest can't be checked. */
  b = with([&]() { f.version = 5, f.minReader = 5; });
  b[40] ^= 1;
  test_assert(refused(b, kUnsupported, "needs a newer build (format 5)"));
  b = good;
  b[24] = 2; /* chunkCount */
  fixUp(b);
  test_assert(refused(b, kBad, "after the last chunk"));
  b[24] = 4;
  fixUp(b);
  test_assert(refused(b, kBad, "ends too early"));
  b = with([&]() {});
  for (int i = 0; i < 8; i++) {
    b[28 + 16 + i] = 0xff; /* the first chunk's stored size */
  }
  fixUp(b);
  test_assert(refused(b, kBad, "past the end"));
}

static void test_versions()
{
  /* A newer file that this build can read: unknown chunks, record bytes, flags. */
  RawFile f = oneRecord([](RawRecord &r) {
    r.flags = 1 | 64;
    r.after = [](BytesWriter &out) { out.u32(123); };
  });
  f.version = 2;
  f.blobs.push_back(bytesOf({1}));
  f.file();
  for (FileBuilder::Chunk &c : f.chunks) {
    c.bytes.append(9);
    c.raw++;
  }
  f.chunk(chunkTag('M', 'O', 'R', 'E'), bytesOf({1, 2}), 0);
  f.chunk(chunkTag('M', 'O', 'R', 'E'), bytesOf({1, 2}), 64);
  IoWorld w;
  LoadReport report;
  test_assert(w.load(f.build(), &report).ok() && w.at(1));
  test_assert(w.lib.get(w.at(1))->flags() == BlockFlags::Root);
  const std::string issues = issueText(report.issues);
  for (const char *what : {"a chunk 'MORE'",
                           "bytes after a block's record",
                           "flags this build",
                           "what NAME should",
                           "what BLCK should",
                           "what DATA should",
                           "data that no block uses"})
  {
    test_assert(has(issues, what));
  }
  f.chunks.back().flags = 1;
  test_assert(refused(f.build(), kUnsupported, "chunk 'MORE' that this build"));
  f.chunks.back().tag = 1;
  test_assert(refused(f.build(), kUnsupported, "chunk '?"));
  /* In this version, they are damage (an unknown chunk is still only skipped). */
  const auto v1 = [](auto change) {
    RawFile g = oneRecord();
    change(g);
    return g.file();
  };
  test_assert(
      refused(v1([](RawFile &g) { g.records[0].flags = 64; }), kBad, "flags this build"));
  test_assert(refused(
      v1([](RawFile &g) { g.records[0].after = [](BytesWriter &out) { out.u8(0); }; }),
      kBad,
      "bytes after a block's record"));
  test_assert(refused(v1([](RawFile &g) { g.blobs.push_back(bytesOf({1})); }),
                      kBad,
                      "data that no block uses"));
}

static void test_records()
{
  const auto bad = [](const char *what,
                      litestl::util::function<void(RawRecord &)> change) {
    return refused(oneRecord(std::move(change)).file(), kBad, what);
  };
  test_assert(bad("block 0 of the file, '", [](RawRecord &r) { r.client = 3; }));
  test_assert(bad("outside NAME", [](RawRecord &r) { r.client = 3; }));
  test_assert(bad("outside NAME", [](RawRecord &r) { r.type = 99; }));
  test_assert(bad("outside NAME", [](RawRecord &r) { r.source = 3; }));
  test_assert(bad("without an id", [](RawRecord &r) { r.local = LocalId(); }));
  test_assert(bad("a payload without a type", [](RawRecord &r) { r.type = 0; }));
  test_assert(bad("in an unknown way", [](RawRecord &r) { r.as = 2; }));
  test_assert(bad("isn't there", [](RawRecord &r) { r.blob = 1; }));
  test_assert(bad("NUL", [](RawRecord &r) { r.name = std::string("a\0b", 3); }));
  test_assert(
      bad("kind of reference", [](RawRecord &r) { r.rows = {{3, LocalId{1, 1}}}; }));
  test_assert(bad("null reference with a client", [](RawRecord &r) {
    r.slots = [](BytesWriter &out) {
      out.u32(1);
      out.u32(1);
      out.i64(0);
      out.u8(0);
      out.u32(1);
      out.u64(0);
      out.u64(0);
    };
  }));
  const auto slots = [](int n, uint32_t ns2) {
    return [n, ns2](BytesWriter &out) {
      out.u32(uint32_t(n));
      for (int i = 0; i < n; i++) {
        out.u32(i == 0 ? 2 : ns2);
        out.i64(4);
        RawRecord::row(out, 0, LocalId{7, 1});
      }
    };
  };
  test_assert(bad("one slot twice", [&](RawRecord &r) { r.slots = slots(2, 2); }));
  test_assert(!bad("one slot twice", [&](RawRecord &r) { r.slots = slots(2, 1); }));
  const auto attachments = [](std::initializer_list<uint32_t> types, uint32_t blob = 0) {
    const std::vector<uint32_t> t = types;
    return [t, blob](BytesWriter &out) {
      out.u32(uint32_t(t.size()));
      for (uint32_t type : t) {
        out.u32(type);
        out.u32(1);
        out.u32(blob);
        out.u32(0);
      }
    };
  };
  test_assert(bad("one attachment twice",
                  [&](RawRecord &r) { r.attachments = attachments({2, 2}); }));
  test_assert(
      bad("without a type", [&](RawRecord &r) { r.attachments = attachments({0}); }));
  test_assert(
      bad("isn't there", [&](RawRecord &r) { r.attachments = attachments({2}, 1); }));
  test_assert(
      !bad("no damage", [&](RawRecord &r) { r.attachments = attachments({1, 2}); }));

  RawFile f = oneRecord();
  f.records.push_back(RawRecord());
  test_assert(refused(f.file(), kBad, "two blocks have one id"));
  f.records[1].local.lo = 2;
  f.blobs.push_back(bytesOf({1, 2}));
  f.records[0].blob = f.records[1].blob = 1;
  test_assert(refused(f.file(), kBad, "used twice"));
  f.records[1].blob = 0;
  f.records[1].attachments = attachments({2}, 1);
  test_assert(refused(f.file(), kBad, "used twice"));
  f.records[0].blob = 0;
  f.records[0].as = 1;
  test_assert(!refused(f.file(), kBad, "no damage"));
  f.records[0].blob = 1;
  test_assert(refused(f.file(), kBad, "without a payload has bytes"));

  /* Names. */
  const auto names = [](litestl::util::function<void(BytesWriter &)> write) {
    RawFile g = oneRecord();
    g.file();
    BytesWriter out;
    write(out);
    g.chunks[0].bytes = out.take();
    g.chunks[0].raw = g.chunks[0].bytes.size();
    return g.build();
  };
  const auto two = [](uint8_t form, litestl::util::function<void(BytesWriter &)> second) {
    return [form, second](BytesWriter &out) {
      out.u32(2);
      out.u8(0);
      out.str("coredcc");
      out.u8(form);
      second(out);
    };
  };
  test_assert(
      refused(names(two(0, [](BytesWriter &o) { o.str(""); })), kBad, "an empty name"));
  test_assert(
      refused(names(two(1, [](BytesWriter &o) { o.u64(0); })), kBad, "a null name"));
  test_assert(refused(
      names(two(2, [](BytesWriter &o) { o.u64(1); })), kBad, "unknown kind of name"));
  test_assert(refused(
      names(two(1, [](BytesWriter &o) { o.u64(5); })), kBad, "an id where a name"));
  test_assert(
      refused(names([](BytesWriter &o) { o.u32(0xffffffff); }), kBad, "a count of more"));

  /* Two names with one id: FNV-1a's collisions are easy to make. */
  RawFile clash = oneRecord();
  clash.names.push_back("x.Type");
  clash.records.push_back(RawRecord());
  clash.records[1].local.lo = 2;
  clash.records[1].type = 3;
  const std::string one = "L45quFHgMXI1IRpC", other = "K4Yk0FA8KNAMBsZE";
  test_assert(TypeId::of(one) == TypeId::of(other));
  {
    RawFile f2 = clash;
    f2.names[1] = one;
    f2.names[2] = other;
    test_assert(refused(f2.file(), kBad, "has the id of"));
    /* As attachments, and against a registered type. */
    f2.records.pop_back();
    f2.records[0].attachments = attachments({3});
    test_assert(refused(f2.file(), kBad, "has the id of"));
    f2.names[1] = "x.Type";
    IoWorld w;
    registerDoc(w.rt, w.docs, 1, one.c_str());
    Status s = w.load(f2.file());
    test_assert(s.code() == kBad && hasMsg(s, "has the id of 'L45") && w.lib.size() == 0);
  }
  IoWorld same;
  test_assert(same.load(clash.file()).ok() &&
              same.lib.size() == 2); /* the same text: fine */

  /* Blobs. */
  const auto data = [](uint8_t lz4, uint64_t raw, uint64_t stored, int have) {
    RawFile g = oneRecord([](RawRecord &r) { r.blob = 1; });
    g.blobs.push_back(Bytes());
    g.file();
    BytesWriter out;
    out.u32(1);
    out.u8(lz4);
    out.u64(raw);
    out.u64(stored);
    for (int i = 0; i < have; i++) {
      out.u8(0);
    }
    g.chunks[2].bytes = out.take();
    g.chunks[2].raw = g.chunks[2].bytes.size();
    return g.build();
  };
  test_assert(refused(data(0, 5, 4, 4), kBad, "says the wrong size"));
  test_assert(refused(data(0, 4, 5, 4), kBad, "past the end"));
  test_assert(refused(data(2, 4, 4, 4), kBad, "unknown way"));
  test_assert(refused(data(1, 4, 4, 4), kBad, "compressed data is damaged"));
  test_assert(refused(data(1, 0, 4, 4), kBad, "says the wrong size"));
  test_assert(refused(data(1, 4, 0, 0), kBad, "says the wrong size"));
  test_assert(refused(data(1, 4 * 255 + 65, 4, 4), kBad, "says the wrong size"));
  test_assert(refused(data(1, 4 * 255 + 64, 4, 4), kBad, "damaged"));
  test_assert(refused(data(1, kMaxRaw + 1, 4, 4), kRange, "too large"));
  test_assert(refused(data(0, UINT64_MAX, 4, 4), kRange, "too large"));
}

static void test_limits()
{
  LoadOptions options;
  /* A valid bomb: 4 MiB of zeros in 16 KB. */
  Bytes zeros, packed;
  zeros.resize(4 << 20);
  for (size_t i = 0; i < zeros.size(); i++) {
    zeros[int(i)] = 0;
  }
  test_assert(sculptcore::io::compressBlock(zeros.data(), zeros.size(), packed) > 0);
  RawFile f = oneRecord([](RawRecord &r) { r.blob = 1; });
  f.blobs.push_back(Bytes());
  f.file();
  BytesWriter out;
  out.u32(1);
  out.u8(1);
  out.u64(zeros.size());
  out.u64(packed.size());
  out.raw(view(packed));
  f.chunks[2].bytes = out.take();
  f.chunks[2].raw = f.chunks[2].bytes.size();
  const Bytes bomb = f.build();
  options.limits.maxBytes = uint64_t(6) << 20;
  test_assert(refused(bomb, kRange, "more memory than allowed", options));
  options.limits.maxBytes = uint64_t(7) << 20;
  {
    IoWorld w;
    test_assert(w.load(bomb, nullptr, options).ok());
    test_assert(w.lib.get(w.at(1))->placeholderPayload().size() == zeros.size());
  }
  /* A chunk as a bomb. */
  f = oneRecord();
  f.file();
  f.chunks[0].bytes = packed;
  f.chunks[0].raw = zeros.size();
  f.chunks[0].flags |= 2;
  options.limits.maxBytes = uint64_t(6) << 20;
  test_assert(refused(f.build(), kRange, "NAME: the data needs more", options));

  /* A huge size, within what lz4 could: refused by the limit, the memory probe or lz4. */
  f = oneRecord([](RawRecord &r) { r.blob = 1; });
  f.blobs.push_back(Bytes());
  f.file();
  BytesWriter big;
  big.u32(1);
  big.u8(1);
  big.u64(uint64_t(1) << 30);
  big.u64(zeros.size() + 400000);
  big.raw(view(zeros));
  for (int i = 0; i < 400000; i++) {
    big.u8(0);
  }
  f.chunks[2].bytes = big.take();
  f.chunks[2].raw = f.chunks[2].bytes.size();
  options.limits.maxBytes = UINT64_MAX;
  {
    Runtime rt;
    Library lib(&rt);
    Result<LoadReport> r = detail::loadLibrary(lib, view(f.build()), options);
    test_assert(!r.ok() && lib.size() == 0);
  }

  /* Each kind of item is charged. */
  const auto limited = [](RawFile &file, uint64_t bytes) {
    LoadOptions o;
    o.limits.maxBytes = bytes;
    return refused(file.file(), kRange, "more memory than allowed", o);
  };
  const uint64_t one = kRecordCost + 2 * kNameCost + 200;
  RawFile rows = oneRecord();
  test_assert(!limited(rows, one));
  rows.records[0].rows.resize(1000, {uint8_t(2), LocalId{1, 1}});
  test_assert(limited(rows, one + 999 * kRowCost) &&
              !limited(rows, one + 1000 * kRowCost));
  RawFile slots = oneRecord([](RawRecord &r) {
    r.slots = [](BytesWriter &o) {
      o.u32(1000);
      for (int i = 0; i < 1000; i++) {
        o.u32(2);
        o.i64(i);
        RawRecord::row(o, 2, LocalId{1, 1});
      }
    };
  });
  test_assert(limited(slots, one + 999 * kSlotCost) &&
              !limited(slots, one + 1000 * kSlotCost));
  RawFile names = oneRecord();
  names.names.resize(1000, "n");
  test_assert(limited(names, one + 900 * kNameCost) &&
              !limited(names, one + 1000 * kNameCost));
  RawFile atts = oneRecord([](RawRecord &r) {
    r.attachments = [](BytesWriter &o) {
      o.u32(1000);
      for (uint32_t i = 0; i < 1000; i++) {
        o.u32(i + 1);
        o.u32(0);
        o.u32(0);
        o.u32(0);
      }
    };
  });
  for (int i = 2; i < 1000; i++) {
    atts.names.push_back("type " + std::to_string(i));
  }
  const uint64_t named = one + 1000 * kNameCost + 10000;
  test_assert(limited(atts, named + 900 * kAttachmentCost) &&
              !limited(atts, named + 1000 * kAttachmentCost));
  RawFile missing = oneRecord();
  for (uint64_t i = 0; i < 1000; i++) {
    missing.records[0].rows.push_back({uint8_t(0), LocalId{9, i + 1}});
  }
  const uint64_t tables = one + 1000 * kRowCost;
  test_assert(limited(missing, tables + 999 * kRecordCost));
  test_assert(!limited(missing, tables + 1000 * kRecordCost));
  LoadOptions few;
  few.limits.maxBlocks = 1000;
  test_assert(refused(missing.file(), kRange, "more than 1000 records", few));
  few.limits.maxBlocks = 1001;
  {
    IoWorld w;
    test_assert(w.load(missing.file(), nullptr, few).ok() && w.lib.size() == 1001);
  }
  few.limits.maxBlocks = 0;
  test_assert(refused(oneRecord().file(), kRange, "more than 0 blocks", few));
  RawFile many;
  for (uint64_t i = 0; i < 1000; i++) {
    RawRecord r;
    r.local.lo = i + 1;
    many.records.push_back(std::move(r));
  }
  test_assert(limited(many, 900 * kRecordCost) &&
              !limited(many, 1000 * kRecordCost + 20000));
  /* Long names are charged for each record that has them. */
  many.names[1] = std::string(10000, 't');
  test_assert(limited(many, 1000 * kRecordCost + 9000000));
  test_assert(!limited(many, 1000 * kRecordCost + 10100000));

  /* Entries: a payload of tiny ones costs memory that its bytes don't show. */
  IoWorld from;
  const BlockHandle h = from.doc(1, "entries");
  from.root(h);
  for (int i = 0; i < 5000; i++) {
    from.data(h)->entries.append(entryOf("", PropValue::ofInt(0)));
  }
  const Bytes entries = from.save();
  options.limits.maxBytes = 5000 * sizeof(BlockEntry);
  IoWorld w;
  LoadReport report;
  test_assert(w.load(entries, &report, options).ok() && report.missingType == 1);
  test_assert(has(issueText(report.issues), "the entries need more memory"));
  options.limits.maxBytes = 5000 * sizeof(BlockEntry) * 5 + 200000;
  IoWorld w2;
  test_assert(w2.load(entries, &report, options).ok() && report.missingType == 0);
  /* Refs of entries too. */
  IoWorld from2;
  const BlockHandle h2 = from2.doc(1, "refs");
  from2.root(h2);
  from2.addOptEntry(h2, from2.id(h2));
  for (int i = 0; i < 5000; i++) {
    from2.data(h2)->entries[0].refs.append(from2.data(h2)->entries[0].refs[0]);
  }
  from2.lib.setAuditRelations(false);
  const Bytes refs = from2.save();
  /* Enough for the table's rows, but not for the entry's too. */
  options.limits.maxBytes = 5010 * kRowCost + 200000;
  IoWorld w3;
  test_assert(w3.load(refs, &report, options).ok() && report.missingType == 1);
  options.limits.maxBytes = 5010 * kRowCost + 5010 * sizeof(RefCell) * 4 + 200000;
  IoWorld w4;
  test_assert(w4.load(refs, &report, options).ok() && report.missingType == 0);
}

/* Large but allowed: linear time. */
static void test_large()
{
  const auto t0 = std::chrono::steady_clock::now();
  /* 100,000 rows to missing blocks, all ids colliding in BlockId's own hash. */
  RawFile f = oneRecord();
  for (uint64_t i = 1; i <= 100000; i++) {
    f.records[0].rows.push_back({uint8_t(0), LocalId{i, i * 0x9E3779B97F4A7C15ull}});
  }
  {
    IoWorld w;
    LoadReport report;
    LoadOptions options;
    options.limits.maxBytes = uint64_t(512) << 20; /* a 32-bit process allows less */
    test_assert(w.load(f.file(), &report, options).ok() && report.missingBlock == 100000);
  }
  /* A slot list and an alias conflict per record. */
  RawFile g;
  g.names = {"c.old", "x.Type", "c.now"};
  for (uint64_t i = 1; i <= 20000; i++) {
    for (uint32_t c : {1u, 3u}) {
      RawRecord r;
      r.client = c;
      r.local.lo = i;
      r.rows = {{uint8_t(0), LocalId{7, i}}};
      g.records.push_back(std::move(r));
    }
  }
  RawRecord slots;
  slots.local.lo = 900000;
  slots.slots = [](BytesWriter &out) {
    out.u32(50000);
    for (int i = 0; i < 50000; i++) {
      out.u32(2);
      out.i64(i);
      RawRecord::row(out, 0, LocalId{7, uint64_t(1 + i % 20000)});
    }
  };
  g.records.push_back(std::move(slots));
  IoWorld w;
  const char *aliases[] = {"c.old"};
  test_assert(w.rt.ext<ClientRegistry>().registerClient("c.now", aliases).ok());
  LoadReport report;
  test_assert(w.load(g.file(), &report).ok() && report.blocks.size() == 40001);
  test_assert(report.issues.total >= 20000 &&
              report.issues.first.size() == IoIssues::kMaxKept);
  test_assert(seconds(t0) < 60);
}

static void test_fuzz()
{
  const Bytes good = sample();
  const auto t0 = std::chrono::steady_clock::now();
  double slowest = 0;
  int loaded = 0;
  LoadOptions options;
  options.limits.maxBytes = uint64_t(4) << 20;
  const size_t before = litestl::alloc::getMemorySize();
  const auto attempt = [&](const Bytes &file) {
    const auto t = std::chrono::steady_clock::now();
    Bytes again;
    {
      Knows k;
      k.note = (loaded & 1) != 0;
      IoWorld w(k);
      const size_t start = litestl::alloc::getMemorySize();
      Result<LoadReport> r = detail::loadLibrary(w.lib, view(file), options);
      /* getMemorySize is 0 growth in release builds. The payload copy of a Doc is the
       * rest. */
      test_assert(litestl::alloc::getMemorySize() - start < size_t(8) << 20);
      if (r.ok()) {
        loaded++;
        test_assert(w.lib.validate().ok());
        Result<Bytes> saved = saveLibrary(w.lib);
        if (!saved.ok()) {
          fprintf(stderr, "saving what was loaded: %s\n", saved.status().message());
        }
        test_assert(saved.ok());
        again = std::move(saved).value();
      } else {
        test_assert(w.lib.size() == 0);
      }
      w.lib.setAuditRelations(false);
    }
    if (again.size() > 0) {
      IoWorld w;
      test_assert(detail::loadLibrary(w.lib, view(again), {}).ok());
    }
    slowest = std::max(slowest, seconds(t));
  };
  for (int i = 0; i < int(good.size()); i++) {
    Bytes b = good;
    b[i] ^= uint8_t(1 << (i % 8));
    Runtime rt;
    Library lib(&rt);
    test_assert(!detail::loadLibrary(lib, view(b), {}).ok() && lib.size() == 0);
    fixUp(b);
    attempt(b);
    b[i] = 0xff;
    fixUp(b);
    attempt(b);
  }
  SplitMix64 rng(77);
  for (int i = 0; i < 20000 * testScale(); i++) {
    Bytes b = good;
    for (uint32_t n = 1 + rng.below(3); n > 0; n--) {
      const int at = 28 + int(rng.below(uint32_t(b.size()) - 36));
      switch (rng.below(4)) {
      case 0:
        b[at] = uint8_t(rng.next());
        break;
      case 1:
        b[at] = uint8_t(b[at] + (rng.below(2) ? 1 : -1));
        break;
      case 2: { /* move a run */
        const int from = 28 + int(rng.below(uint32_t(b.size()) - 36));
        for (int k = 0; k < 12 && at + k < int(b.size()) && from + k < int(b.size()); k++)
        {
          b[at + k] = b[from + k];
        }
        break;
      }
      default: { /* a count */
        const uint32_t v = rng.below(3) ? rng.below(300) : uint32_t(rng.next());
        for (int k = 0; k < 4 && at + k < int(b.size()); k++) {
          b[at + k] = uint8_t(v >> (8 * k));
        }
      }
      }
    }
    fixUp(b);
    attempt(b);
  }
  test_assert(loaded > 1000); /* the fuzz reaches past the parser */
  test_assert(slowest < 5);
  test_assert(litestl::alloc::getMemorySize() <= before + 4096);
  fprintf(stderr,
          "fuzz: %d loaded, slowest %.3f s, %.1f s in all\n",
          loaded,
          slowest,
          seconds(t0));
}

int main()
{
  test_container();
  test_versions();
  test_records();
  test_limits();
  test_large();
  test_fuzz();
  return test_end();
}
