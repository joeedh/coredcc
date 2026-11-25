/* FrameRate and UnitSettings: rates as fractions, frames as a unit, display units, files.
 */
#include "coredcc/props/unit_settings.h"

#include "fuzz.h"
#include "test_util.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <numeric>

test_init;

using namespace coredcc;

namespace {

constexpr UnitFamily kFamilies[] = {UnitFamily::Length,
                                    UnitFamily::Angle,
                                    UnitFamily::Time,
                                    UnitFamily::Mass,
                                    UnitFamily::Factor};

bool is(const Result<FrameRate> &r, int num, int den)
{
  return r.ok() && r->num() == num && r->den() == den;
}

bool is(const FrameRate &r, int num, int den)
{
  return r.num() == num && r.den() == den;
}

void testRates()
{
  test_assert(is(FrameRate(), 24, 1));
  test_assert(is(FrameRate::of(25), 25, 1) && is(FrameRate::of(50, 4), 25, 2));
  test_assert(is(FrameRate::of(60000, 2002), 30000, 1001));
  test_assert(is(FrameRate::of(1, 1000), 1, 1000) &&
              is(FrameRate::of(1000000), 1000000, 1));
  for (int64_t bad : {int64_t(0), int64_t(-24), std::numeric_limits<int64_t>::min()}) {
    test_assert(FrameRate::of(bad).status().code() == StatusCode::InvalidArgument);
    test_assert(FrameRate::of(24, bad).status().code() == StatusCode::InvalidArgument);
  }
  const int64_t huge = std::numeric_limits<int64_t>::max();
  test_assert(FrameRate::of(1000001).status().code() == StatusCode::OutOfRange);
  test_assert(FrameRate::of(huge).status().code() == StatusCode::OutOfRange);
  test_assert(FrameRate::of(huge, huge - 1).status().code() == StatusCode::OutOfRange);
  test_assert(FrameRate::of(1, 1001).status().code() == StatusCode::OutOfRange);
  test_assert(FrameRate::of(1, huge).status().code() == StatusCode::OutOfRange);
  test_assert(FrameRate::of(999983, 1000001).status().code() == StatusCode::OutOfRange);
  test_assert(is(FrameRate::of(huge, huge), 1, 1));

  /* What people call the broadcast rates. */
  struct Name {
    double fps;
    int num;
  };
  const Name names[] = {{23.976, 24000},
                        {23.98, 24000},
                        {29.97, 30000},
                        {47.952, 48000},
                        {47.95, 48000},
                        {59.94, 60000},
                        {89.91, 90000},
                        {95.904, 96000},
                        {95.9, 96000},
                        {119.88, 120000},
                        {143.856, 144000},
                        {239.76, 240000}};
  for (const Name &n : names) {
    if (!is(FrameRate::nearest(n.fps), n.num, 1001)) {
      fprintf(stderr,
              "%g is %d/%d\n",
              n.fps,
              FrameRate::nearest(n.fps).num(),
              FrameRate::nearest(n.fps).den());
      test_assert(false);
    }
  }
  test_assert(is(FrameRate::of(2997, 100), 30000, 1001) &&
              is(FrameRate::of(23976, 1000), 24000, 1001));
  test_assert(is(FrameRate::nearest(23.976023976023978), 24000, 1001));
  test_assert(is(FrameRate::nearest(167.83), 24000, 143)); /* 168000/1001 */
  /* And what they aren't. */
  test_assert(is(FrameRate::nearest(24.0), 24, 1) && is(FrameRate::nearest(30.0), 30, 1));
  test_assert(is(FrameRate::nearest(23.9), 239, 10) &&
              is(FrameRate::nearest(29.9), 299, 10));
  test_assert(is(FrameRate::nearest(29.96), 749, 25) &&
              is(FrameRate::nearest(29.98), 1499, 50));
  test_assert(is(FrameRate::nearest(23.977), 23977, 1000) &&
              is(FrameRate::nearest(12.5), 25, 2));
  test_assert(is(FrameRate::nearest(24.975), 999, 40) &&
              is(FrameRate::nearest(269.73), 26973, 100));
}

void testNearest()
{
  coredcc_test::SplitMix64 r(17);
  /* Every fraction comes back from its double. */
  int big = 0;
  for (int i = 0; i < 100000 * coredcc_test::testScale(); i++) {
    const uint32_t top = i % 3 ? uint32_t(FrameRate::kMax) : 2000;
    const Result<FrameRate> f = FrameRate::of(1 + r.below(top), 1 + r.below(top));
    if (f.ok()) {
      test_assert(std::gcd(f->num(), f->den()) == 1);
      test_assert(FrameRate::nearest(f->fps()) == *f);
      big += f->den() > 100000;
    }
  }
  test_assert(big > 10000);
  for (int num = FrameRate::kMax - 50; num <= FrameRate::kMax; num++) {
    for (int den : {1, 2, 3, 7, 999, 999999, 1000000}) {
      const Result<FrameRate> f = FrameRate::of(num, den);
      test_assert(f.ok() && FrameRate::nearest(f->fps()) == *f);
    }
  }
  /* Any other number is near its fraction. */
  for (int i = 0; i < 100000 * coredcc_test::testScale(); i++) {
    const double x = std::pow(10.0, double(r.next() >> 11) * 0x1p-53 * 9.0 - 3.0);
    const FrameRate f = FrameRate::nearest(x);
    if (!(std::fabs(f.fps() - x) <= 2e-6 * x)) {
      fprintf(stderr, "%.17g is %d/%d\n", x, f.num(), f.den());
      test_assert(false);
      break;
    }
  }
  const double inf = std::numeric_limits<double>::infinity();
  test_assert(is(FrameRate::nearest(0.0), 1, 1000) &&
              is(FrameRate::nearest(-inf), 1, 1000));
  test_assert(is(FrameRate::nearest(5e-324), 1, 1000) &&
              is(FrameRate::nearest(0.001), 1, 1000));
  test_assert(is(FrameRate::nearest(inf), 1000000, 1) &&
              is(FrameRate::nearest(1e300), 1000000, 1));
  test_assert(is(FrameRate::nearest(std::nan("")), 24, 1));
}

void testFrames()
{
  UnitSettings s;
  test_assert(s.timeOf(36.0) == 1.5 && s.frameAt(1.5) == 36.0);
  s.startFrame = 1001.0;
  test_assert(s.timeOf(1001.0) == 0.0 && s.timeOf(1025.0) == 1.0 &&
              s.frameAt(-1.0) == 977.0);
  s.rate = *FrameRate::of(30000, 1001);
  s.startFrame = 0.0;
  test_assert(s.timeOf(30000.0) == 1001.0 && s.frameAt(1001.0) == 30000.0);

  /* A length of time: not from the start frame. */
  s.startFrame = 100.0;
  const auto parsed = [&s](const char *text) {
    const Result<double> v = units::parse(text, UnitFamily::Time, s);
    test_assert(v.ok());
    return v.ok() ? *v : std::nan("");
  };
  test_assert(parsed("30000 f") == 1001.0 && parsed("30000frames") == 1001.0);
  test_assert(parsed("-1 frame") == -1001.0 / 30000.0 &&
              parsed("1fr") == 1001.0 / 30000.0);
  test_assert(parsed("2") == 2.0 && parsed("2 min") == 120.0);
  test_assert(units::format(1.0, UnitFamily::Time, s, 3) == "1 s");
  test_assert(s.setDisplayUnit(UnitFamily::Time, "frames").ok());
  test_assert(!strcmp(s.displayUnit(UnitFamily::Time).name, "frame"));
  test_assert(std::fabs(s.displayUnit(UnitFamily::Time).scale - 1001.0 / 30000.0) <
              1e-17);
  test_assert(units::format(1001.0, UnitFamily::Time, s, 3) == "30000 f");
  test_assert(units::format(1.0, UnitFamily::Time, s, 3) == "29.97 f");
  test_assert(parsed("15") == 15.0 * 1001.0 / 30000.0 && parsed("15 s") == 15.0);
  /* Whole frames stay whole, at every rate that is common. */
  for (int num : {12, 24, 25, 30, 48, 50, 60, 120, 24000, 30000, 60000, 120000}) {
    s.rate = *FrameRate::of(num, num > 1000 ? 1001 : 1);
    for (int f = -2000; f < 200000; f += 1 + f / 64 * (f > 0)) {
      const double t = s.timeOf(f + s.startFrame);
      test_assert(std::llround(s.frameAt(t) - s.startFrame) == f);
      test_assert(units::format(t, UnitFamily::Time, s, 6) == std::to_string(f) + " f");
      test_assert(units::format(t, UnitFamily::Time, s, 9) == std::to_string(f) + " f");
      test_assert(parsed((std::to_string(f) + " f").c_str()) == t);
    }
  }
  /* The other overloads know no frames. */
  test_assert(!units::parse("1 f", UnitFamily::Time, "s").ok());
  test_assert(units::findUnit(UnitFamily::Time, "f") == nullptr);
}

void testDisplay()
{
  UnitSettings s;
  const char *const defaults[] = {"meter", "degree", "second", "kilogram", "factor"};
  for (int i = 0; i < 5; i++) {
    const UnitFamily f = kFamilies[i];
    test_assert(!strcmp(s.displayUnit(f).name, defaults[i]));
    test_assert(UnitSettings::unitCount(f) ==
                int(units::unitsOf(f).size()) + (f == UnitFamily::Time));
    for (int u = 0; u < UnitSettings::unitCount(f); u++) {
      const units::Unit unit = s.unitAt(f, u);
      test_assert(s.findUnit(f, unit.symbol) == u && s.findUnit(f, unit.name) == u);
      test_assert(UnitSettings::isFrame(f, u) == !strcmp(unit.name, "frame"));
      test_assert(s.setDisplayIndex(f, u).ok() && s.displayIndex(f) == u);
      /* As the other overloads, for what they know. */
      if (!UnitSettings::isFrame(f, u)) {
        test_assert(units::format(1234.5678, f, s, 3) ==
                    units::format(1234.5678, f, unit.name, 3));
        for (const char *text :
             {"12.5", "12.5 cm", "90deg", "3 h", "2 lb", "50%", "x", ""})
        {
          const Result<double> a = units::parse(text, f, s),
                               b = units::parse(text, f, unit.name);
          test_assert(a.ok() == b.ok() && a.status().code() == b.status().code());
          test_assert(!a.ok() || *a == *b);
        }
      }
    }
    test_assert(s.setDisplayIndex(f, -1).code() == StatusCode::OutOfRange);
    test_assert(s.setDisplayIndex(f, UnitSettings::unitCount(f)).code() ==
                StatusCode::OutOfRange);
    test_assert(s.displayIndex(f) == UnitSettings::unitCount(f) - 1);
    test_assert(!strcmp(s.unitAt(f, 99).name, units::unitsOf(f)[0].name));
    test_assert(!strcmp(s.unitAt(f, -1).name, units::unitsOf(f)[0].name));
  }
  test_assert(s.setDisplayIndex(UnitFamily::None, 0).code() == StatusCode::OutOfRange);
  test_assert(UnitSettings::unitCount(UnitFamily::None) == 0);
  test_assert(units::format(2.5, UnitFamily::None, s, 3) == "2.5");
  test_assert(*units::parse("2.5", UnitFamily::None, s) == 2.5);
  test_assert(!units::parse("2.5 m", UnitFamily::None, s).ok());

  const Status bad = s.setDisplayUnit(UnitFamily::Time, "fram");
  test_assert(bad.code() == StatusCode::InvalidArgument && bad.suggestions().size() > 0);
  bool frame = false;
  for (const litestl::util::string &name : bad.suggestions()) {
    frame = frame || !strcmp(name.c_str(), "frame");
  }
  test_assert(frame);
  test_assert(!units::parse("1 fram", UnitFamily::Time, s).ok());
  test_assert(units::parse("1e308 h", UnitFamily::Time, s).status().code() ==
              StatusCode::OutOfRange);
  test_assert(s.setDisplayUnit(UnitFamily::Length, "cm").ok());
  test_assert(units::format(0.015, UnitFamily::Length, s, 3) == "1.5 cm");
  test_assert(*units::parse("1.5", UnitFamily::Length, s) == 0.015);
}

} // namespace

int main()
{
  testRates();
  testNearest();
  testFrames();
  testDisplay();
  return test_end();
}
