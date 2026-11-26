/* The binary form of values and bags: golden bytes, round trips, hostile input. */
#include "coredcc/props/value_io.h"

#include "fuzz.h"
#include "litestl/tests/test_util.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <string>

test_init;

using namespace coredcc;
using coredcc_test::SplitMix64;
using litestl::util::Vector;

static std::span<const uint8_t> view(const BytesWriter &w)
{
  return std::span<const uint8_t>(w.data().data(), size_t(w.data().size()));
}

static bool hasMsg(const Status &s, const char *needle)
{
  if (strstr(s.message(), needle)) {
    return true;
  }
  fprintf(stderr, "message was: %s\n", s.message());
  return false;
}

static bool bytesAre(const BytesWriter &w, std::initializer_list<int> want)
{
  bool same = w.status().ok() && size_t(w.data().size()) == want.size();
  int i = 0;
  for (int b : want) {
    same = same && w.data()[i++] == uint8_t(b);
  }
  if (!same) {
    for (uint8_t b : w.data()) {
      fprintf(stderr, "%d, ", b);
    }
    fprintf(stderr, "\n");
  }
  return same;
}

static StableRef samplePointer()
{
  StableRef r;
  r.owner = OwnerKey::ofBlock(BlockId{ClientId::of("acme"), LocalId{1, 2}});
  r.key.append(KeySegment::member("mods"));
  r.key.append(KeySegment::stringKey("B v\"l"));
  r.key.append(KeySegment::ofHandle(7));
  r.type = StructId::of("acme.Mod");
  return r;
}

static Vector<PropValue> samples()
{
  Vector<PropValue> out;
  double m[16];
  for (int i = 0; i < 16; i++) {
    m[i] = i * 0.5 - 3;
  }
  out.append(PropValue());
  out.append(PropValue::ofBool(true));
  out.append(PropValue::ofBool(false));
  out.append(PropValue::ofInt(INT64_MIN));
  out.append(PropValue::ofInt(INT64_MAX));
  out.append(PropValue::ofFloat(0.1));
  out.append(PropValue::ofFloat(-0.0));
  out.append(PropValue::ofFloat(std::bit_cast<double>(0x7ff8000000001234ull)));
  out.append(PropValue::ofFloat(INFINITY));
  out.append(PropValue::ofString(""));
  out.append(PropValue::ofString("\xff\xfe latin-1 \x80")); /* not UTF-8 */
  out.append(PropValue::ofString(std::string(100000, 'n')));
  out.append(PropValue::ofVec2(1, 2));
  out.append(PropValue::ofVec3(1, NAN, 3));
  out.append(PropValue::ofVec4(1, 2, 3, 4));
  out.append(PropValue::ofComponents(PropValue::Type::Mat4, m));
  out.append(PropValue::ofQuat(0, 0, 0, 1));
  out.append(PropValue::ofEnum(-5));
  out.append(PropValue::ofFlags(UINT64_MAX));
  out.append(PropValue::nullPointer());
  out.append(PropValue::ofPointer(samplePointer()));
  StableRef rt;
  rt.owner = OwnerKey::ofRuntime(TypeId::of("owner:prefs").value);
  out.append(PropValue::ofPointer(rt));
  rt.owner = OwnerKey::ofRuntime(0x1234); /* no name anywhere */
  rt.type.value = 0x99;
  out.append(PropValue::ofPointer(rt));
  return out;
}

static void test_golden()
{
  Runtime rt;
  NameTable names;
  ValueCodec codec(rt, &names);
  const auto golden = [&](const PropValue &v, std::initializer_list<int> want) {
    BytesWriter w;
    writeValue(w, v, codec);
    return bytesAre(w, want);
  };
  test_assert(golden(PropValue(), {0}));
  test_assert(golden(PropValue::ofBool(true), {1, 1}));
  test_assert(golden(PropValue::ofInt(-2), {2, 254, 255, 255, 255, 255, 255, 255, 255}));
  test_assert(golden(PropValue::ofFloat(1.0), {3, 0, 0, 0, 0, 0, 0, 0xf0, 0x3f}));
  test_assert(golden(PropValue::ofString("ab"), {4, 2, 0, 0, 0, 'a', 'b'}));
  test_assert(golden(PropValue::ofVec2(1, -0.0),
                     {5, 0, 0, 0, 0, 0, 0, 0xf0, 0x3f, 0, 0, 0, 0, 0, 0, 0, 0x80}));
  test_assert(golden(PropValue::ofEnum(3), {10, 3, 0, 0, 0, 0, 0, 0, 0}));
  test_assert(golden(PropValue::ofFlags(6), {11, 6, 0, 0, 0, 0, 0, 0, 0}));
  test_assert(golden(PropValue::nullPointer(), {12, 0}));
  test_assert(names.add(IdDomain::Client, "acme") &&
              rt.registerId<IdDomain::Struct>("S").ok());
  StableRef r;
  r.owner = OwnerKey::ofBlock(BlockId{ClientId::of("acme"), LocalId{1, 2}});
  r.key.append(KeySegment::member("m"));
  r.key.append(KeySegment::stringKey("k"));
  r.key.append(KeySegment::ofHandle(7));
  r.type = StructId::of("S");
  test_assert(
      golden(PropValue::ofPointer(r),
             {12, 1,                              /* a block */
              1,  4, 0, 0, 0, 'a', 'c', 'm', 'e', /* its client's name */
              1,  0, 0, 0, 0, 0,   0,   0,   2,   0, 0, 0, 0, 0, 0, 0, /* LocalId */
              3,  0, 0, 0,                      /* three segments */
              0,  1, 0, 0, 0, 'm',              /* member */
              1,  1, 0, 0, 0, 'k',              /* string key */
              2,  7, 0, 0, 0, 0,   0,   0,   0, /* handle */
              1,  1, 0, 0, 0, 'S'}));           /* the struct's name */
  StableRef o;
  o.owner = OwnerKey::ofRuntime(0x0102);
  test_assert(golden(PropValue::ofPointer(o),
                     {12,
                      2,
                      2,
                      2,
                      1,
                      0,
                      0,
                      0,
                      0,
                      0,
                      0, /* a raw id */
                      0,
                      0,
                      0,
                      0,
                      0})); /* no segments, no struct */
  /* Every type's tag. */
  const ValueTag tags[] = {ValueTag::None,
                           ValueTag::Bool,
                           ValueTag::Int,
                           ValueTag::Float,
                           ValueTag::String,
                           ValueTag::Vec2,
                           ValueTag::Vec3,
                           ValueTag::Vec4,
                           ValueTag::Mat4,
                           ValueTag::Quat,
                           ValueTag::Enum,
                           ValueTag::Flags,
                           ValueTag::Pointer};
  for (int i = 0; i < 13; i++) {
    test_assert(int(tags[i]) == i && valueTag(PropValue::Type(i)) == tags[i]);
    test_assert(isKnownValueTag(uint8_t(i)));
  }
  test_assert(!isKnownValueTag(13) && !isKnownValueTag(255));
}

static void test_round_trip()
{
  Runtime rt;
  NameTable names;
  ValueCodec codec(rt, &names);
  names.add(IdDomain::Client, "acme");
  for (const PropValue &v : samples()) {
    BytesWriter w;
    writeValue(w, v, codec);
    test_assert(w.status().ok());
    BytesReader r(view(w));
    test_assert(readValue(r, codec) == v && r.ok() && r.atEnd()); /* == is bitwise */

    /* Every truncation fails, and leaves None. Long text: the interesting ends. */
    const size_t n = size_t(w.data().size());
    for (size_t cut = 0; cut < n; cut++) {
      if (cut > 300 && cut + 300 < n) {
        continue;
      }
      BytesReader t(view(w).subspan(0, cut));
      test_assert(readValue(t, codec).isNone() && !t.ok());
    }
  }
}

static void test_names()
{
  /* A name nobody registered comes back by id, and is written by name from the table. */
  Runtime a, b;
  test_assert(a.registerId<IdDomain::Struct>("acme.Mod").ok());
  test_assert(a.registerId<IdDomain::Client>("acme").ok());
  NameTable none, table;
  ValueCodec ca(a, &none), cb(b, &table), cnull(b, nullptr);
  const PropValue v = PropValue::ofPointer(samplePointer());
  BytesWriter w, again, noTable;
  writeValue(w, v, ca);
  BytesReader r(view(w)), r2(view(w));
  test_assert(readValue(r, cb) == v && r.ok());
  test_assert(strcmp(table.nameOf(IdDomain::Struct, StructId::of("acme.Mod").value),
                     "acme.Mod") == 0);
  test_assert(!b.nameOf(StructId::of("acme.Mod"))); /* never in the Runtime */
  writeValue(again, v, cb);
  test_assert(again.data().size() == w.data().size() &&
              memcmp(again.data().data(), w.data().data(), size_t(w.data().size())) == 0);
  test_assert(readValue(r2, cnull) == v && r2.ok());
  writeValue(noTable, v, cnull); /* as raw ids */
  BytesReader r3(view(noTable));
  test_assert(readValue(r3, cnull) == v && r3.ok() &&
              noTable.data().size() != w.data().size());

  /* A name whose id the Runtime has under another name: only this read fails. */
  Runtime c;
  test_assert(
      c.debugRegisterNameAs(IdDomain::Struct, StructId::of("acme.Mod").value, "other")
          .ok());
  NameTable tc;
  ValueCodec cc(c, &tc);
  BytesReader r4(view(w));
  test_assert(readValue(r4, cc).isNone() &&
              r4.status().code() == StatusCode::InvalidArgument);
  test_assert(hasMsg(r4.status(), "'other'") && !c.hasStickyError());
  /* The same in the table. */
  Runtime d;
  NameTable td;
  td.debugAddAs(IdDomain::Struct, StructId::of("acme.Mod").value, "other");
  ValueCodec cd(d, &td);
  BytesReader r5(view(w));
  test_assert(readValue(r5, cd).isNone() && hasMsg(r5.status(), "'other'"));
  /* Such a name can't stand for the id: it is written raw, and reads back. */
  BytesWriter w6;
  writeValue(w6, v, cc);
  BytesReader r6(view(w6));
  test_assert(readValue(r6, cc) == v && r6.ok());
  /* A full table keeps the id. */
  NameTable full;
  full.add(IdDomain::Type, std::string(NameTable::kMaxBytes - 100, 'f'));
  ValueCodec cf(d, &full);
  BytesReader r7(view(w));
  test_assert(readValue(r7, cf) == v && r7.ok());
}

static void test_bad_values()
{
  Runtime rt;
  NameTable names;
  ValueCodec codec(rt, &names);
  const auto fails = [&](std::initializer_list<int> bytes, const char *msg) {
    Vector<uint8_t> b;
    for (int x : bytes) {
      b.append(uint8_t(x));
    }
    BytesReader r(std::span<const uint8_t>(b.data(), size_t(b.size())));
    const bool none = readValue(r, codec).isNone();
    return none && !r.ok() && hasMsg(r.status(), msg);
  };
  test_assert(fails({13}, "unknown type"));
  test_assert(fails({255}, "unknown type"));
  test_assert(fails({12, 3}, "kind of owner"));
  test_assert(fails({12, 2, 3}, "form of name"));
  test_assert(fails({12, 2, 1, 0, 0, 0, 0}, "empty name"));
  test_assert(fails({12, 2, 2, 0, 0, 0, 0, 0, 0, 0, 0}, "null id"));
  test_assert(fails({12, 2, 0, 1, 0, 0, 0, 9, 0, 0, 0, 0}, "kind of key"));
  test_assert(fails({12, 2, 0, 1, 0, 0, 0, 2, 255, 255, 255, 255, 255, 255, 255, 255},
                    "negative"));
  test_assert(fails({12, 2, 0, 255, 255, 255, 255, 0, 0, 0, 0, 0}, "count"));
  test_assert(fails({4, 1, 0, 0, 0, 0}, "NUL"));
  test_assert(fails({1, 2}, "boolean"));

  /* The writer refuses what the reader would. */
  StableRef neg;
  neg.owner = OwnerKey::ofRuntime(1);
  KeySegment s;
  s.kind = KeySegment::Kind::Handle;
  s.handle = -1;
  neg.key.append(s);
  BytesWriter w;
  writeValue(w, PropValue::ofPointer(neg), codec);
  test_assert(w.status().code() == StatusCode::InvalidArgument);
}

/* ---- bags ---- */

static PropertyBag makeBag(bool withSize = true)
{
  PropertyBag bag;
  test_assert(
      bag.add(PropertyDef::enumProp("mode").item("EDIT", 1).item("OBJ", 2).defaultValue(
                  PropValue::ofEnum(2)))
          .ok());
  test_assert(bag.add(PropertyDef::flagsProp("bits").item("A", 1).item("B", 2)).ok());
  test_assert(bag.add(PropertyDef::floatProp("size").range(0, 10)).ok() || !withSize);
  test_assert(bag.add(PropertyDef::stringProp("label").maxLength(4)).ok());
  test_assert(bag.add(PropertyDef::pointerProp("target", StructId::of("acme.Mod"))).ok());
  test_assert(bag.add(PropertyDef::vecProp("pos", 3)).ok());
  return bag;
}

static void test_bag_golden()
{
  Runtime rt;
  ValueCodec codec(rt, nullptr);
  PropertyBag bag = makeBag();
  test_assert(bag.set("mode", PropValue::ofEnum(1)).ok());
  test_assert(bag.set("bits", PropValue::ofFlags(3)).ok());
  BytesWriter w;
  writeBag(w, bag, codec); /* keys, and only what is set */
  test_assert(
      bytesAre(w, {2,   0,   0,   0, 4, 0, 0,   0,   'm', 'o', 'd', 'e', 4,   8,   0,
                   0,   0,   4,   0, 0, 0, 'E', 'D', 'I', 'T', 4,   0,   0,   0,   'b',
                   'i', 't', 's', 4, 7, 0, 0,   0,   3,   0,   0,   0,   'A', '|', 'B'}));
}

static void test_bag_round_trip()
{
  Runtime rt;
  NameTable names;
  ValueCodec codec(rt, &names);
  PropertyBag bag = makeBag();
  test_assert(bag.set("mode", PropValue::ofEnum(1)).ok());
  test_assert(bag.set("bits", PropValue::ofFlags(0)).ok());
  test_assert(bag.set("size", PropValue::ofFloat(2.5)).ok());
  test_assert(bag.set("label",
                      PropValue::ofString("\xff"
                                          "b"))
                  .ok());
  test_assert(bag.set("target", PropValue::ofPointer(samplePointer())).ok());
  BytesWriter w;
  writeBag(w, bag, codec);
  PropertyBag back = makeBag();
  Vector<BagIssue> issues;
  BytesReader r(view(w));
  readBag(r, back, codec, issues);
  test_assert(r.ok() && r.atEnd() && issues.size() == 0);
  for (int i = 0; i < bag.size(); i++) {
    test_assert(back.value(i) == bag.value(i) && back.wasSet(i) == bag.wasSet(i));
  }
  for (size_t cut = 0; cut < size_t(w.data().size()); cut++) {
    PropertyBag part = makeBag();
    BytesReader t(view(w).subspan(0, cut));
    readBag(t, part, codec, issues);
    test_assert(!t.ok());
  }
}

static void test_bag_issues()
{
  Runtime rt;
  ValueCodec codec(rt, nullptr);
  /* Items the reading side doesn't have, an unknown name, an unknown tag, a value that
   * doesn't coerce: issues, and the rest is read. */
  PropertyBag bag;
  test_assert(bag.add(PropertyDef::enumProp("mode").item("NEW", 9)).ok());
  test_assert(bag.add(PropertyDef::floatProp("gone")).ok());
  test_assert(bag.add(PropertyDef::stringProp("label")).ok());
  test_assert(bag.add(PropertyDef::floatProp("size")).ok());
  test_assert(bag.set("mode", PropValue::ofEnum(9)).ok() &&
              bag.set("gone", PropValue::ofFloat(1)).ok() &&
              bag.set("label", PropValue::ofString("too long")).ok() &&
              bag.set("size", PropValue::ofFloat(99)).ok());
  BytesWriter w;
  writeBag(w, bag, codec);
  PropertyBag back = makeBag();
  Vector<BagIssue> issues;
  BytesReader r(view(w));
  readBag(r, back, codec, issues);
  test_assert(r.ok() && r.atEnd() && issues.size() == 3);
  test_assert(strcmp(issues[0].name.c_str(), "mode") == 0 && !issues[0].why.ok());
  test_assert(issues[1].why.code() == StatusCode::NotFound);
  test_assert(strcmp(issues[2].name.c_str(), "label") == 0);
  test_assert(!back.isSet("mode") &&
              *back.get("size") == PropValue::ofFloat(10)); /* clamped */

  /* A value with no item is written as a number, which coerce rejects on read. */
  PropertyBag dyn;
  bool wide = true;
  test_assert(dyn.add(PropertyDef::enumProp("mode").item("EDIT", 1).dynamic(
                          [&wide](PropertyDef &d, const void *) {
                            if (wide) {
                              d.item("LATE", 5);
                            }
                          }))
                  .ok());
  test_assert(dyn.set("mode", PropValue::ofEnum(5)).ok());
  test_assert(storedBagValue(dyn, 0) == PropValue::ofString("LATE"));
  wide = false;
  test_assert(storedBagValue(dyn, 0) == PropValue::ofEnum(5));
  BytesWriter w2;
  writeBag(w2, dyn, codec);
  issues.clear();
  BytesReader r2(view(w2));
  readBag(r2, dyn, codec, issues);
  test_assert(r2.ok() && issues.size() == 1);

  /* Flags with a bit no item names: a number, so the bit isn't lost. */
  PropertyBag flags;
  bool more = true;
  test_assert(flags
                  .add(PropertyDef::flagsProp("bits").item("A", 1).dynamic(
                      [&more](PropertyDef &d, const void *) {
                        if (more) {
                          d.item("B", 2);
                        }
                      }))
                  .ok());
  test_assert(flags.set("bits", PropValue::ofFlags(3)).ok());
  test_assert(storedBagValue(flags, 0) == PropValue::ofString("A|B"));
  more = false;
  test_assert(storedBagValue(flags, 0) == PropValue::ofFlags(3));

  /* An unknown tag is skipped by its length. */
  BytesWriter w3;
  w3.u32(2);
  w3.str("size");
  w3.u8(200);
  const uint8_t future[] = {1, 2, 3};
  w3.blob(future);
  w3.str("label");
  w3.u8(uint8_t(ValueTag::String));
  const size_t at = w3.beginSection();
  w3.str("ok");
  w3.endSection(at);
  PropertyBag b3 = makeBag();
  issues.clear();
  BytesReader r3(view(w3));
  readBag(r3, b3, codec, issues);
  test_assert(r3.ok() && issues.size() == 1 &&
              issues[0].why.code() == StatusCode::Unsupported);
  test_assert(*b3.get("label") == PropValue::ofString("ok") && !b3.isSet("size"));

  /* A body of the wrong length is damage, not an issue. */
  BytesWriter w4;
  w4.u32(1);
  w4.str("size");
  w4.u8(uint8_t(ValueTag::Bool));
  const uint8_t two[] = {1, 1};
  w4.blob(two);
  PropertyBag b4 = makeBag();
  BytesReader r4(view(w4));
  readBag(r4, b4, codec, issues);
  test_assert(hasMsg(r4.status(), "left over"));
}

/* ---- hostile input ---- */

static std::string uglyText(SplitMix64 &rng)
{
  std::string s;
  for (uint32_t n = rng.below(12); n > 0; n--) {
    s += char(1 + rng.below(255)); /* anything but NUL */
  }
  return s;
}

static PropValue uglyValue(SplitMix64 &rng)
{
  const auto bits = [&rng]() { return std::bit_cast<double>(rng.next()); };
  switch (rng.below(5)) {
  case 0:
    return PropValue::ofString(uglyText(rng));
  case 1:
    return PropValue::ofVec4(bits(), bits(), bits(), bits());
  case 2:
    return rng.below(2) ? PropValue::ofEnum(int64_t(rng.next()))
                        : PropValue::ofFlags(rng.next());
  default: {
    StableRef r;
    if (rng.below(2)) {
      r.owner = OwnerKey::ofBlock(
          BlockId{ClientId{rng.below(3)}, LocalId{rng.next(), rng.below(2)}});
    } else {
      r.owner = OwnerKey::ofRuntime(rng.below(3));
    }
    for (uint32_t n = rng.below(4); n > 0; n--) {
      r.key.append(rng.below(3) == 0 ? KeySegment::ofHandle(int64_t(rng.next() >> 1))
                   : rng.below(2)    ? KeySegment::member(uglyText(rng))
                                     : KeySegment::stringKey(uglyText(rng)));
    }
    r.type.value = rng.below(2) ? 0 : rng.next();
    return PropValue::ofPointer(r);
  }
  }
}

/* What is written always reads back. */
static void test_writer_never_emits_what_reader_rejects()
{
  Runtime rt;
  SplitMix64 rng(5);
  for (int i = 0; i < 5000 * coredcc_test::testScale(); i++) {
    NameTable names;
    const std::string name = uglyText(rng);
    if (!name.empty()) {
      test_assert(names.add(IdDomain::Type, name));
    }
    ValueCodec codec(rt, &names);
    PropValue v = uglyValue(rng);
    if (!name.empty() && rng.below(3) == 0) {
      StableRef r;
      r.owner = OwnerKey::ofRuntime(hashId(IdDomain::Type, name));
      v = PropValue::ofPointer(r);
    }
    BytesWriter w;
    writeValue(w, v, codec);
    BytesReader r(view(w));
    test_assert(w.status().ok() && readValue(r, codec) == v && r.ok() && r.atEnd());
  }
}

/* Mutated and random bytes: no crash, bounded time, and memory bounded by the input. */
static void test_fuzz()
{
  Runtime rt;
  SplitMix64 rng(77);
  Vector<PropValue> seeds = samples();
  const auto start = std::chrono::steady_clock::now();
  size_t worst = 0;
  for (int i = 0; i < 20000 * coredcc_test::testScale(); i++) {
    NameTable names;
    ValueCodec codec(rt, &names);
    BytesWriter w;
    const PropValue &seed = seeds[int(rng.below(uint32_t(seeds.size())))];
    if (seed.type() == PropValue::Type::String) {
      continue; /* the long one only slows this down */
    }
    const bool asBag = rng.below(2) != 0;
    if (asBag) {
      PropertyBag bag = makeBag();
      bag.set("target", seed).ignore();
      bag.set("label", PropValue::ofString("abc")).ignore();
      bag.set("bits", PropValue::ofFlags(3)).ignore();
      writeBag(w, bag, codec);
    } else {
      writeValue(w, seed, codec);
    }
    Vector<uint8_t> bytes = w.take();
    for (uint32_t e = rng.below(6); e > 0 && bytes.size() > 0; e--) {
      const int at = int(rng.below(uint32_t(bytes.size())));
      switch (rng.below(4)) {
      case 0:
        bytes[at] = uint8_t(rng.next());
        break;
      case 1:
        bytes[at] = rng.below(2) ? 0xff : uint8_t(rng.below(14));
        break;
      case 2:
        bytes.resize(size_t(at));
        break;
      case 3:
        bytes.append(uint8_t(rng.next()));
        break;
      }
    }
    const std::span<const uint8_t> in(bytes.data(), size_t(bytes.size()));
    const size_t before = litestl::alloc::getMemorySize();
    BytesReader r(in);
    PropertyBag back;
    Vector<BagIssue> issues;
    PropValue v;
    if (asBag) {
      back = makeBag();
      const size_t bagSize = litestl::alloc::getMemorySize() - before;
      readBag(r, back, codec, issues);
      worst = std::max(worst, litestl::alloc::getMemorySize() - before - bagSize);
    } else {
      v = readValue(r, codec);
      worst = std::max(worst, litestl::alloc::getMemorySize() - before);
      test_assert(r.ok() || v.isNone());
      if (r.ok()) {
        /* What was read can be written, and means the same. */
        BytesWriter out;
        writeValue(out, v, codec);
        BytesReader r2(view(out));
        test_assert(out.status().ok() && readValue(r2, codec) == v && r2.ok());
      }
    }
  }
  const double seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  test_assert(worst < 64 * 1024); /* inputs are under 1 KB */
  test_assert(seconds < 20.0 * coredcc_test::testScale());

  /* The densest inputs: what they allocate is a small multiple of their size. */
  NameTable names;
  ValueCodec codec(rt, &names);
  const size_t kItems = 200000;
  for (uint32_t n : {kMaxKeySegments, kMaxKeySegments + 1, 200000u}) {
    BytesWriter w;
    w.u8(uint8_t(ValueTag::Pointer));
    w.u8(2);
    w.u8(0);
    w.u32(n);
    for (uint32_t i = 0; i < n; i++) {
      w.u8(0);
      w.u32(0);
    }
    w.u8(0);
    const size_t before = litestl::alloc::getMemorySize();
    BytesReader r(view(w));
    const PropValue v = readValue(r, codec);
    test_assert(r.ok() == (n <= kMaxKeySegments));
    test_assert(r.ok() || (r.status().code() == StatusCode::OutOfRange &&
                           strstr(r.status().message(), "too long")));
    test_assert(litestl::alloc::getMemorySize() - before <
                60 * size_t(std::min(n, kMaxKeySegments)) * 5);
    StableRef ref;
    ref.owner = OwnerKey::ofRuntime(1);
    for (uint32_t i = 0; i < n; i++) {
      ref.key.append(KeySegment::member(""));
    }
    BytesWriter out;
    writeValue(out, PropValue::ofPointer(ref), codec);
    test_assert(out.status().ok() ? n <= kMaxKeySegments
                                  : out.status().code() == StatusCode::OutOfRange);
    test_assert(out.status().ok() == (n <= kMaxKeySegments));
  }
  {
    BytesWriter w;
    w.u32(uint32_t(kItems));
    for (size_t i = 0; i < kItems; i++) {
      w.str("");
      w.u8(200);
      w.blob({});
    }
    PropertyBag bag = makeBag();
    Vector<BagIssue> issues;
    BytesReader r(view(w));
    readBag(r, bag, codec, issues);
    test_assert(r.ok() && issues.size() == kMaxBagIssues);
  }
  {
    /* Distinct names fill the table and no more. */
    const size_t before = litestl::alloc::getMemorySize();
    for (int i = 0; i < 400000; i++) {
      BytesWriter w;
      w.u8(uint8_t(ValueTag::Pointer));
      w.u8(2);
      w.u8(1);
      w.str("n" + std::to_string(i));
      w.u32(0);
      w.u8(0);
      BytesReader r(view(w));
      readValue(r, codec);
      test_assert(r.ok());
    }
    test_assert(litestl::alloc::getMemorySize() - before < (size_t(64) << 20));
  }
}

int main()
{
  test_golden();
  test_round_trip();
  test_names();
  test_bad_values();
  test_bag_golden();
  test_bag_round_trip();
  test_bag_issues();
  test_writer_never_emits_what_reader_rejects();
  test_fuzz();
  return test_end();
}
