#include "coredcc/props/units.h"

#include "test_util.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

test_init;

using namespace coredcc;

static bool near(double a, double b, double eps = 1e-12)
{
  return std::fabs(a - b) <= eps * (1.0 + std::fabs(b));
}

static double parsed(const char *text, UnitFamily f, const char *display)
{
  Result<double> r = units::parse(text, f, display);
  if (!r.ok()) {
    fprintf(stderr, "parse('%s') failed: %s\n", text, r.status().message());
    return NAN;
  }
  return *r;
}

static void test_format()
{
  test_assert(units::format(1.25, UnitFamily::Length, "m", 3) == "1.25 m");
  test_assert(units::format(0.015, UnitFamily::Length, "cm", 3) == "1.5 cm");
  test_assert(units::format(2.0, UnitFamily::Length, "m", 3) == "2 m");
  test_assert(units::format(1.5707963267948966, UnitFamily::Angle, "deg", 1) ==
              "90\xC2\xB0");
  test_assert(units::format(0.5, UnitFamily::Factor, "%", 0) == "50%");
  test_assert(units::format(0.5, UnitFamily::Factor, "", 2) == "0.5");
  test_assert(units::format(-0.0001, UnitFamily::Length, "m", 2) == "0 m"); /* no "-0" */
  test_assert(units::format(3.0, UnitFamily::None, "", 2) == "3");
  test_assert(units::format(1.0, UnitFamily::Length, "parsec", 1) == "1 m"); /* fallback */
  test_assert(units::format(123.456, UnitFamily::Length, "m", 0) == "123 m");
}

static void test_parse()
{
  test_assert(near(parsed("1.5", UnitFamily::Length, "cm"), 0.015));
  test_assert(near(parsed(" 1.5 cm ", UnitFamily::Length, "m"), 0.015));
  test_assert(near(parsed("-2e3mm", UnitFamily::Length, "m"), -2.0));
  test_assert(near(parsed("+.5 km", UnitFamily::Length, "m"), 500.0));
  test_assert(near(parsed("3 feet", UnitFamily::Length, "m"), 0.9144));
  test_assert(near(parsed("2 meters", UnitFamily::Length, "cm"), 2.0));
  test_assert(near(parsed("90deg", UnitFamily::Angle, "rad"), 1.5707963267948966));
  test_assert(near(parsed("90\xC2\xB0", UnitFamily::Angle, "rad"), 1.5707963267948966));
  test_assert(near(parsed("50%", UnitFamily::Factor, ""), 0.5));
  test_assert(near(parsed("2 min", UnitFamily::Time, "s"), 120.0));
  test_assert(near(parsed("4", UnitFamily::None, ""), 4.0));

  Result<double> bad = units::parse("3 kmm", UnitFamily::Length, "m");
  test_assert(!bad.ok() && strstr(bad.status().message(), "unknown Length unit 'kmm'"));
  test_assert(bad.status().suggestions().size() > 0);
  test_assert(!units::parse("", UnitFamily::Length, "m").ok());
  test_assert(!units::parse("m", UnitFamily::Length, "m").ok());
  test_assert(!units::parse(".", UnitFamily::Length, "m").ok());
  test_assert(!units::parse("1 2", UnitFamily::Length, "m").ok());
  test_assert(!units::parse("1+1", UnitFamily::Length, "m").ok()); /* no expressions */
  test_assert(!units::parse("inf", UnitFamily::Length, "m").ok());
  test_assert(!units::parse("1e999", UnitFamily::Length, "m").ok());
  test_assert(!units::parse("1e308 km", UnitFamily::Length, "m").ok()); /* overflows */
  test_assert(!units::parse("1", UnitFamily::Length, "parsec").ok());
  test_assert(!units::parse("1 m", UnitFamily::None, "").ok());
}

/* parse(format(x)) is within half a displayed digit, in base units. */
static void test_round_trip()
{
  uint64_t state = 0x1234;
  auto next = [&state]() {
    state += 0x9E3779B97F4A7C15ull;
    uint64_t z = state;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
  };
  const UnitFamily families[] = {
      UnitFamily::Length, UnitFamily::Angle, UnitFamily::Time, UnitFamily::Mass,
      UnitFamily::Factor};
  int checked = 0;
  for (UnitFamily f : families) {
    for (const units::Unit &u : units::unitsOf(f)) {
      for (int precision = 0; precision <= 6; precision++) {
        for (int i = 0; i < 50; i++) {
          const double mag = std::pow(10.0, double(int(next() % 19)) - 9.0);
          const double x = (double(next() >> 11) / 9007199254740992.0 - 0.5) * mag;
          const std::string text = units::format(x, f, u.symbol, precision);
          Result<double> back = units::parse(text, f, "");
          if (!back.ok()) {
            back = units::parse(text, f, u.symbol); /* Factor prints no symbol */
          }
          const double tol = std::pow(10.0, -precision) * u.scale + 1e-12 * std::fabs(x);
          if (!back.ok() || !(std::fabs(*back - x) <= tol)) {
            fprintf(stderr, "round trip: %.17g -> '%s'\n", x, text.c_str());
            test_assert(false);
            return;
          }
          checked++;
        }
      }
    }
  }
  test_assert(checked > 1000);
}

int main()
{
  test_format();
  test_parse();
  test_round_trip();
  return test_end();
}
