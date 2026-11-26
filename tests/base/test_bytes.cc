/* BytesWriter, BytesReader and NameTable. */
#include "coredcc/base/bytes.h"
#include "coredcc/base/name_table.h"
#include "coredcc/base/runtime.h"

#include "fuzz.h"
#include "litestl/tests/test_util.h"

#include <bit>
#include <cmath>
#include <cstring>
#include <string>

test_init;

using namespace coredcc;
using coredcc_test::SplitMix64;

static std::span<const uint8_t> view(const BytesWriter &w)
{
  return std::span<const uint8_t>(w.data().data(), size_t(w.data().size()));
}

static bool hasMsg(const Status &s, const char *needle)
{
  return strstr(s.message(), needle) != nullptr;
}

static void test_golden()
{
  BytesWriter w;
  w.u8(0xab);
  w.u16(0x1234);
  w.u32(0xdeadbeef);
  w.u64(0x0102030405060708ull);
  w.i64(-2);
  w.f64(-0.0);
  w.boolean(true);
  w.boolean(false);
  w.str("hi");
  w.str(std::string_view("cut\0off", 7)); /* stops at the NUL */
  const uint8_t raw[] = {0, 255};
  w.blob(raw);
  const uint8_t golden[] = {
      0xab, 0x34, 0x12, 0xef, 0xbe, 0xad, 0xde, 8,    7,    6,    5, 4,   3,
      2,    1,    0xfe, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0, 0,   0,
      0,    0,    0,    0,    0x80, 1,    0,    2,    0,    0,    0, 'h', 'i',
      3,    0,    0,    0,    'c',  'u',  't',  2,    0,    0,    0, 0,   255};
  test_assert(w.status().ok() && size_t(w.data().size()) == sizeof(golden));
  test_assert(memcmp(w.data().data(), golden, sizeof(golden)) == 0);

  BytesReader r(view(w));
  test_assert(r.u8() == 0xab && r.u16() == 0x1234 && r.u32() == 0xdeadbeef);
  test_assert(r.u64() == 0x0102030405060708ull && r.i64() == -2);
  test_assert(std::signbit(r.f64()) && r.boolean() && !r.boolean());
  test_assert(r.str() == "hi" && r.str() == "cut");
  const std::span<const uint8_t> b = r.blob();
  test_assert(b.size() == 2 && b[0] == 0 && b[1] == 255);
  test_assert(r.ok() && r.atEnd() && r.remaining() == 0 && r.offset() == sizeof(golden));

  const litestl::util::Vector<uint8_t> taken = w.take();
  test_assert(size_t(taken.size()) == sizeof(golden) && w.data().size() == 0);
}

static void test_floats_keep_bits()
{
  const uint64_t bits[] = {0x7ff8000000000001ull,
                           0xfff8dead0000beefull,
                           0x0000000000000001ull,
                           0x7ff0000000000000ull,
                           0x8000000000000000ull};
  for (uint64_t b : bits) {
    BytesWriter w;
    w.f64(std::bit_cast<double>(b));
    BytesReader r(view(w));
    test_assert(std::bit_cast<uint64_t>(r.f64()) == b);
  }
}

static void test_arrays()
{
  const float f[] = {1.0f, std::bit_cast<float>(0x7fc12345u), -0.0f};
  const uint32_t u[] = {0x01020304u, 7};
  const int16_t i[] = {-2, 0x0102};
  const uint64_t big[] = {0x0102030405060708ull};
  BytesWriter w;
  w.i32(-2);
  w.f32(f[1]);
  w.values(std::span<const float>(f));
  w.array(std::span<const uint32_t>(u));
  w.array(std::span<const int16_t>(i));
  w.values(std::span<const uint64_t>(big));
  w.array(std::span<const double>());
  const uint8_t golden[] = {
      0xfe, 0xff, 0xff, 0xff, 0x45, 0x23, 0xc1, 0x7f,                /* scalars */
      0,    0,    0x80, 0x3f, 0x45, 0x23, 0xc1, 0x7f, 0, 0, 0, 0x80, /* f, no count */
      2,    0,    0,    0,    4,    3,    2,    1,    7, 0, 0, 0,    /* u */
      2,    0,    0,    0,    0xfe, 0xff, 2,    1,                   /* i */
      8,    7,    6,    5,    4,    3,    2,    1,    0, 0, 0, 0};   /* big, none */
  test_assert(w.status().ok() && size_t(w.data().size()) == sizeof(golden));
  test_assert(memcmp(w.data().data(), golden, sizeof(golden)) == 0);

  BytesReader r(view(w));
  float back[3];
  litestl::util::Vector<uint32_t> bu;
  litestl::util::Vector<int16_t> bi;
  litestl::util::Vector<double> none;
  uint64_t bb[1];
  test_assert(r.i32() == -2 && std::bit_cast<uint32_t>(r.f32()) == 0x7fc12345u);
  r.values(std::span<float>(back));
  r.array(bu);
  r.array(bi);
  r.values(std::span<uint64_t>(bb));
  none.append(1.0);
  r.array(none);
  test_assert(r.ok() && r.atEnd() && memcmp(back, f, sizeof(f)) == 0 && bb[0] == big[0]);
  test_assert(bu.size() == 2 && bu[0] == u[0] && bu[1] == 7 && none.size() == 0);
  test_assert(bi.size() == 2 && bi[0] == -2 && bi[1] == 0x0102);

  /* Too short: zeros, and an empty array. */
  {
    BytesReader cut(view(w).first(15));
    cut.u64();
    back[0] = back[1] = 5;
    cut.values(std::span<float>(back).first(2));
    test_assert(!cut.ok() && back[0] == 0 && back[1] == 0 && back[2] != 5);
    cut.array(bu);
    test_assert(bu.size() == 0);
  }
  {
    const uint8_t lying[] = {3, 0, 0, 0, 1, 0, 0, 0, 2, 0, 0, 0};
    BytesReader cut{std::span<const uint8_t>(lying)};
    cut.array(bu);
    test_assert(!cut.ok() && bu.size() == 0);
  }
  {
    BytesWriter small(7);
    small.array(std::span<const uint32_t>(u));
    test_assert(small.status().code() == StatusCode::OutOfRange);
  }
  /* What a big-endian machine does. */
  uint8_t swapped[8];
  detail::copyNumbers(swapped, u, 2, 4, true);
  const uint8_t want[] = {1, 2, 3, 4, 0, 0, 0, 7};
  if constexpr (std::endian::native == std::endian::little) {
    test_assert(memcmp(swapped, want, 8) == 0);
  }
  detail::copyNumbers(swapped, u, 2, 4, false);
  test_assert(memcmp(swapped, u, 8) == 0);
  detail::copyNumbers(swapped, nullptr, 0, 4, true);
}

static void test_sections()
{
  BytesWriter w;
  const size_t outer = w.beginSection();
  w.u8(1);
  const size_t inner = w.beginSection();
  w.u16(2);
  w.endSection(inner);
  w.endSection(outer);
  const size_t empty = w.beginSection();
  w.endSection(empty);
  BytesReader r(view(w));
  const std::span<const uint8_t> o = r.blob();
  test_assert(o.size() == 7 && r.blob().empty() && r.ok() && r.atEnd());
  BytesReader ro(o);
  test_assert(ro.u8() == 1);
  BytesReader ri(ro.blob());
  test_assert(ri.u16() == 2 && ri.atEnd() && ro.atEnd() && ri.ok());
}

static void test_writer_failures()
{
  BytesWriter w(8);
  w.u32(1);
  test_assert(w.status().ok());
  w.u32(2); /* exactly full */
  test_assert(w.status().ok() && w.data().size() == 8);
  w.u8(3);
  test_assert(w.status().code() == StatusCode::OutOfRange && w.data().size() == 8);
  w.fail(Status::error(StatusCode::Internal, "later"));
  test_assert(w.status().code() == StatusCode::OutOfRange);
  w.take();
  w.u8(1);
  test_assert(w.status().ok() && w.data().size() == 1);
  w.fail(Status());
  test_assert(w.status().ok());
  w.fail(Status::error(StatusCode::Cancelled, "mine"));
  w.u8(1);
  w.str("x");
  test_assert(w.data().size() == 1 && w.status().code() == StatusCode::Cancelled);
  test_assert(hasMsg(w.status(), "mine"));

  /* A section that overflows doesn't write past its end. */
  BytesWriter s(2);
  const size_t at = s.beginSection();
  s.endSection(at);
  test_assert(s.status().code() == StatusCode::OutOfRange && s.data().size() == 0);

  BytesWriter b(8);
  const uint8_t five[5] = {};
  b.blob(five); /* 4 + 5 */
  test_assert(b.status().code() == StatusCode::OutOfRange);
  test_assert(BytesWriter::kMaxBytes >= (size_t(256) << 20));
}

static void test_reader_failures()
{
  {
    /* Short by one, for every width; later reads give zeros and keep the first error. */
    const uint8_t seven[7] = {1, 2, 3, 4, 5, 6, 7};
    BytesReader r(seven);
    test_assert(r.u8() == 1 && r.u64() == 0 && !r.ok());
    test_assert(r.status().code() == StatusCode::InvalidArgument &&
                hasMsg(r.status(), "at byte 1"));
    test_assert(r.u8() == 0 && r.str().empty() && r.blob().empty() && r.count(1) == 0);
    r.fail(StatusCode::Internal, "later");
    test_assert(r.status().code() == StatusCode::InvalidArgument && r.atEnd());
  }
  {
    const uint8_t two[] = {2};
    BytesReader r(two);
    test_assert(!r.boolean() && r.status().code() == StatusCode::InvalidArgument);
  }
  {
    BytesWriter w;
    const uint8_t nul[] = {'a', 0, 'b'};
    w.blob(nul);
    BytesReader r(view(w));
    test_assert(r.str().empty() && hasMsg(r.status(), "NUL"));
  }
  {
    BytesReader r({});
    r.fail(Status());
    test_assert(r.ok() && r.atEnd());
    r.fail(Status::error(StatusCode::Unsupported, "a newer file"));
    test_assert(r.status().code() == StatusCode::Unsupported &&
                hasMsg(r.status(), "a newer file (at byte 0)"));
  }
  /* Lengths and counts near 2^32. */
  for (uint32_t n : {uint32_t(5),
                     uint32_t(0x7fffffff),
                     uint32_t(0x80000000),
                     uint32_t(0xfffffffc),
                     uint32_t(0xffffffff)})
  {
    BytesWriter w;
    w.u32(n);
    w.u32(0);
    BytesReader a(view(w)), b(view(w)), c(view(w));
    test_assert(a.blob().empty() && !a.ok());
    test_assert(b.count(1) == 0 && !b.ok());
    test_assert(c.count(0x40000000) == 0 && !c.ok()); /* no 32-bit wraparound */
  }
  {
    BytesWriter w;
    w.u32(4);
    w.u32(0);
    BytesReader a(view(w)), b(view(w));
    test_assert(a.count(1) == 4 && a.ok());
    test_assert(b.count(2) == 0 && !b.ok());
  }
}

/* Random reads over random bytes: never out of bounds, never past a failure. */
static void test_fuzz_reader()
{
  SplitMix64 rng(11);
  for (int i = 0; i < 20000 * coredcc_test::testScale(); i++) {
    const uint32_t n = rng.below(40);
    /* An exact heap buffer, so a checked build sees a read past the end. */
    uint8_t *bytes = new uint8_t[n + 1];
    for (uint32_t k = 0; k < n; k++) {
      bytes[k] = rng.below(3) ? uint8_t(rng.below(6)) : uint8_t(rng.next());
    }
    BytesReader r(std::span<const uint8_t>(bytes, n));
    for (int k = 0; k < 12; k++) {
      const bool wasOk = r.ok();
      size_t got = 0;
      switch (rng.below(7)) {
      case 0:
        got = r.u8();
        break;
      case 1:
        got = r.u16();
        break;
      case 2:
        got = r.u32();
        break;
      case 3:
        got = size_t(r.u64());
        break;
      case 4:
        got = r.boolean() ? 1 : 0;
        break;
      case 5: {
        const std::string_view s = r.str();
        got = s.size();
        test_assert(s.find('\0') == std::string_view::npos);
        break;
      }
      case 6: {
        const std::span<const uint8_t> b = r.blob();
        got = b.size();
        test_assert(b.empty() || (b.data() >= bytes && b.data() + b.size() <= bytes + n));
        break;
      }
      }
      test_assert(wasOk || got == 0);
      test_assert(r.offset() + r.remaining() == n);
    }
    delete[] bytes;
  }
}

static void test_names()
{
  NameTable t;
  test_assert(t.add(IdDomain::Struct, "acme.Widget"));
  const uint64_t id = hashId(IdDomain::Struct, "acme.Widget");
  test_assert(strcmp(t.nameOf(IdDomain::Struct, id), "acme.Widget") == 0);
  test_assert(!t.nameOf(IdDomain::Client, id) && !t.nameOf(IdDomain::Struct, id + 1));
  test_assert(t.add(IdDomain::Struct, "acme.Widget"));
  test_assert(!t.add(IdDomain::Struct, "") &&
              !t.add(IdDomain::Struct, std::string_view("a\0b", 3)));
  test_assert(!t.debugAddAs(IdDomain::Struct, id, "other")); /* a collision */
  test_assert(strcmp(t.nameOf(IdDomain::Struct, id), "acme.Widget") == 0);

  /* Bounded by bytes, and by entries of no length to speak of. */
  const std::string big(NameTable::kMaxBytes / 4, 'x');
  int taken = 0;
  for (int i = 0; i < 6; i++) {
    taken += t.add(IdDomain::Type, big + char('a' + i)) ? 1 : 0;
  }
  test_assert(taken == 3);
  NameTable small;
  int n = 0;
  while (n < 1000000 && small.add(IdDomain::Type, "n" + std::to_string(n))) {
    n++;
  }
  test_assert(n > 100000 && n <= int(NameTable::kMaxBytes / NameTable::kEntryBytes));
  test_assert(small.add(IdDomain::Type, "n0")); /* a known name still "adds" */
}

int main()
{
  test_golden();
  test_floats_keep_bits();
  test_arrays();
  test_sections();
  test_writer_failures();
  test_reader_failures();
  test_fuzz_reader();
  test_names();
  return test_end();
}
