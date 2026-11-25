#include "coredcc/props/unit_settings.h"

#include "coredcc/props/unit_text.h"

#include <cmath>
#include <numeric>

namespace coredcc {

/* ---- FrameRate ---- */

namespace {

/* Replaces a rounded name of a broadcast rate. The fraction is in lowest terms. */
void broadcastRate(int64_t &num, int64_t &den)
{
  if (1000 % den != 0) {
    return;
  }
  const int64_t thousandths = num * (1000 / den);
  for (int64_t step : {24, 30}) {
    for (int64_t base = step; base <= 240; base += step) {
      /* base * 1000 / 1001, rounded to three decimals and to two. 1001 is odd, so no
       * ties. */
      const int64_t three = (base * 2000000 + 1001) / 2002,
                    two = (base * 200000 + 1001) / 2002 * 10;
      if (thousandths == three || thousandths == two) {
        num = base * 1000;
        den = 1001;
        return;
      }
    }
  }
}

void reduce(int64_t &num, int64_t &den)
{
  const int64_t g = std::gcd(num, den);
  num /= g;
  den /= g;
}

} // namespace

Result<FrameRate> FrameRate::of(int64_t num, int64_t den)
{
  if (num < 1 || den < 1) {
    return Status::error(StatusCode::InvalidArgument,
                         "a frame rate is a positive fraction");
  }
  reduce(num, den);
  if (num > kMax || den > kMax || num * 1000 < den) {
    return Status::error(StatusCode::OutOfRange, "the frame rate is out of range");
  }
  broadcastRate(num, den);
  reduce(num, den);
  FrameRate r;
  r.num_ = int32_t(num);
  r.den_ = int32_t(den);
  return r;
}

FrameRate FrameRate::nearest(double fps)
{
  if (std::isnan(fps)) {
    return FrameRate();
  }
  fps = fps < 0.001 ? 0.001 : (fps > kMax ? double(kMax) : fps);
  /*
   * fps is a / b exactly: 53 bits over at most 2^62. Euclid's algorithm gives the
   * fractions that are nearest to it for their size. A fraction within the limits whose
   * double is fps is one of them (Legendre's theorem), and the last one within the
   * limits: two of those differ by 1e-12 of their value or more.
   */
  int exponent = 0;
  int64_t a = int64_t(std::ldexp(std::frexp(fps, &exponent), 53));
  int64_t b = int64_t(1) << (53 - exponent);
  int64_t p0 = 0, q0 = 1, p = 1, q = 0;
  while (b != 0) {
    const int64_t whole = a / b, rest = a % b;
    const int64_t p1 = whole * p + p0, q1 = whole * q + q0;
    if (p1 > kMax || q1 > kMax) {
      break;
    }
    p0 = p;
    q0 = q;
    p = p1;
    q = q1;
    a = b;
    b = rest;
  }
  Result<FrameRate> r = of(p, q);
  return r.ok() ? *r : FrameRate();
}

/* ---- UnitSettings ---- */

namespace {

const char *const kNoAliases[] = {nullptr};
const char *const kFrameAliases[] = {"fr", nullptr};

} // namespace

bool UnitSettings::isFrame(UnitFamily family, int index)
{
  return family == UnitFamily::Time && index == int(units::unitsOf(family).size());
}

int UnitSettings::unitCount(UnitFamily family)
{
  return int(units::unitsOf(family).size()) + (family == UnitFamily::Time ? 1 : 0);
}

units::Unit UnitSettings::unitAt(UnitFamily family, int index) const
{
  if (isFrame(family, index)) {
    return {"frame", "f", double(rate.den()) / rate.num(), true, kFrameAliases};
  }
  const std::span<const units::Unit> all = units::unitsOf(family);
  if (all.empty()) {
    return {"", "", 1.0, false, kNoAliases};
  }
  return all[index >= 0 && index < int(all.size()) ? size_t(index) : 0];
}

int UnitSettings::findUnit(UnitFamily family, std::string_view text) const
{
  for (int i = 0; i < unitCount(family); i++) {
    if (units::detail::spelledAs(unitAt(family, i), text)) {
      return i;
    }
  }
  return -1;
}

Status UnitSettings::setDisplayIndex(UnitFamily family, int index)
{
  if (index < 0 || index >= unitCount(family)) {
    return Status::error(StatusCode::OutOfRange, "no such unit");
  }
  display_[int(family)] = uint8_t(index);
  return Status();
}

Status UnitSettings::setDisplayUnit(UnitFamily family, std::string_view text)
{
  const int i = findUnit(family, text);
  return i < 0 ? units::detail::unknownUnit(family, text, *this)
               : setDisplayIndex(family, i);
}

/* ---- text ---- */

namespace units {

std::string
format(double baseValue, UnitFamily family, const UnitSettings &settings, int precision)
{
  const Unit u = settings.displayUnit(family);
  /* By the fraction: a whole number of frames is one again. */
  const double v = UnitSettings::isFrame(family, settings.displayIndex(family))
                       ? baseValue * settings.rate.num() / settings.rate.den()
                       : baseValue / u.scale;
  return detail::formatNumber(v, u, precision);
}

Result<double>
parse(std::string_view text, UnitFamily family, const UnitSettings &settings)
{
  Result<detail::Typed> t = detail::parseNumber(text);
  if (!t.ok()) {
    return t.status();
  }
  int i = settings.displayIndex(family);
  if (!t->unit.empty()) {
    i = settings.findUnit(family, t->unit);
    if (i < 0) {
      return detail::unknownUnit(family, t->unit, settings);
    }
  }
  const double v = UnitSettings::isFrame(family, i)
                       ? t->number * settings.rate.den() / settings.rate.num()
                       : t->number * settings.unitAt(family, i).scale;
  return detail::finite(v, text);
}

} // namespace units
} // namespace coredcc
