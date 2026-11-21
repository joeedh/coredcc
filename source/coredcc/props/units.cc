#include "coredcc/props/units.h"

#include "coredcc/base/text.h"

#include <charconv>
#include <cmath>
#include <cstring>

namespace coredcc {

const char *unitFamilyName(UnitFamily family)
{
  switch (family) {
    case UnitFamily::None:
      return "None";
    case UnitFamily::Length:
      return "Length";
    case UnitFamily::Angle:
      return "Angle";
    case UnitFamily::Time:
      return "Time";
    case UnitFamily::Mass:
      return "Mass";
    case UnitFamily::Factor:
      return "Factor";
  }
  return "?";
}

namespace units {
namespace {

const char *const kNoAliases[] = {nullptr};
const char *const kMicronAliases[] = {"um", "micrometer", "micrometers", nullptr};
const char *const kInchAliases[] = {"inch", "inches", nullptr};
const char *const kFootAliases[] = {"foot", "feet", nullptr};
const char *const kDegAliases[] = {"deg", "degree", "degrees", nullptr};
const char *const kSecondAliases[] = {"sec", nullptr};
const char *const kMinuteAliases[] = {"minute", "minutes", nullptr};
const char *const kHourAliases[] = {"hr", "hour", "hours", nullptr};
const char *const kPoundAliases[] = {"lbs", nullptr};
const char *const kPercentAliases[] = {"percent", nullptr};

const Unit kLength[] = {
    {"meter", "m", 1.0, true, kNoAliases},
    {"kilometer", "km", 1000.0, true, kNoAliases},
    {"centimeter", "cm", 0.01, true, kNoAliases},
    {"millimeter", "mm", 0.001, true, kNoAliases},
    {"micron", "\xC2\xB5m", 1e-6, true, kMicronAliases},
    {"inch", "in", 0.0254, true, kInchAliases},
    {"foot", "ft", 0.3048, true, kFootAliases},
    {"yard", "yd", 0.9144, true, kNoAliases},
    {"mile", "mi", 1609.344, true, kNoAliases},
};
const Unit kAngle[] = {
    {"radian", "rad", 1.0, true, kNoAliases},
    {"degree", "\xC2\xB0", 0.017453292519943295, false, kDegAliases},
};
const Unit kTime[] = {
    {"second", "s", 1.0, true, kSecondAliases},
    {"millisecond", "ms", 0.001, true, kNoAliases},
    {"minute", "min", 60.0, true, kMinuteAliases},
    {"hour", "h", 3600.0, true, kHourAliases},
};
const Unit kMass[] = {
    {"kilogram", "kg", 1.0, true, kNoAliases},
    {"gram", "g", 0.001, true, kNoAliases},
    {"tonne", "t", 1000.0, true, kNoAliases},
    {"pound", "lb", 0.45359237, true, kPoundAliases},
    {"ounce", "oz", 0.028349523125, true, kNoAliases},
};
const Unit kFactor[] = {
    {"factor", "", 1.0, false, kNoAliases},
    {"percent", "%", 0.01, false, kPercentAliases},
};

bool spelledAs(const Unit &u, std::string_view text)
{
  if (text == u.symbol || text == u.name) {
    return true;
  }
  const size_t n = strlen(u.name);
  if (text.size() == n + 1 && text.substr(0, n) == u.name && text[n] == 's') {
    return true;
  }
  for (const char *const *a = u.aliases; *a; a++) {
    if (text == *a) {
      return true;
    }
  }
  return false;
}

bool isDigit(char c)
{
  return c >= '0' && c <= '9';
}

bool isSpace(char c)
{
  return c == ' ' || c == '\t';
}

/* Length of the number at the start of `s`: [+-]? (d+ (.d*)? | .d+) ([eE][+-]?d+)?. */
size_t scanNumber(std::string_view s)
{
  size_t i = 0;
  if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
    i++;
  }
  size_t digits = 0;
  while (i < s.size() && isDigit(s[i])) {
    i++;
    digits++;
  }
  if (i < s.size() && s[i] == '.') {
    i++;
    while (i < s.size() && isDigit(s[i])) {
      i++;
      digits++;
    }
  }
  if (digits == 0) {
    return 0;
  }
  if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
    size_t j = i + 1;
    if (j < s.size() && (s[j] == '+' || s[j] == '-')) {
      j++;
    }
    if (j < s.size() && isDigit(s[j])) {
      while (j < s.size() && isDigit(s[j])) {
        j++;
      }
      i = j;
    }
  }
  return i;
}

std::string_view trim(std::string_view s)
{
  while (!s.empty() && isSpace(s.front())) {
    s.remove_prefix(1);
  }
  while (!s.empty() && isSpace(s.back())) {
    s.remove_suffix(1);
  }
  return s;
}

} // namespace

std::span<const Unit> unitsOf(UnitFamily family)
{
  switch (family) {
    case UnitFamily::None:
      return {};
    case UnitFamily::Length:
      return kLength;
    case UnitFamily::Angle:
      return kAngle;
    case UnitFamily::Time:
      return kTime;
    case UnitFamily::Mass:
      return kMass;
    case UnitFamily::Factor:
      return kFactor;
  }
  return {};
}

const Unit *findUnit(UnitFamily family, std::string_view text)
{
  for (const Unit &u : unitsOf(family)) {
    if (spelledAs(u, text)) {
      return &u;
    }
  }
  return nullptr;
}

std::string format(double baseValue, UnitFamily family, std::string_view unit, int precision)
{
  precision = precision < 0 ? 0 : (precision > 15 ? 15 : precision);
  const Unit *u = findUnit(family, unit);
  if (!u && !unitsOf(family).empty()) {
    u = &unitsOf(family)[0];
  }
  const double v = u ? baseValue / u->scale : baseValue;
  char buf[512];
  std::to_chars_result r =
      std::to_chars(buf, buf + sizeof(buf), v, std::chars_format::fixed, precision);
  std::string out(buf, r.ptr);
  if (out.find('.') != std::string::npos) {
    while (out.back() == '0') {
      out.pop_back();
    }
    if (out.back() == '.') {
      out.pop_back();
    }
  }
  if (out == "-0") {
    out = "0";
  }
  if (u && u->symbol[0]) {
    if (u->spaced) {
      out += ' ';
    }
    out += u->symbol;
  }
  return out;
}

Result<double> parse(std::string_view text, UnitFamily family, std::string_view displayUnit)
{
  const Unit *display = findUnit(family, displayUnit);
  if (!display && family != UnitFamily::None) {
    return Status::error(StatusCode::InvalidArgument,
                         (std::string("unknown display unit '") + std::string(displayUnit) +
                          "' for " + unitFamilyName(family))
                             .c_str());
  }
  const std::string_view s = trim(text);
  const size_t n = scanNumber(s);
  if (n == 0) {
    return Status::error(StatusCode::InvalidArgument,
                         (std::string("expected a number, got '") + std::string(text) + "'")
                             .c_str());
  }
  double v = 0.0;
  const char *first = s.data();
  if (*first == '+') {
    first++; /* from_chars doesn't take a leading '+' */
  }
  std::from_chars_result fr = std::from_chars(first, s.data() + n, v);
  if (fr.ec != std::errc() || fr.ptr != s.data() + n) {
    return Status::error(StatusCode::OutOfRange,
                         (std::string("number out of range: '") + std::string(s.substr(0, n)) +
                          "'")
                             .c_str());
  }
  const std::string_view rest = trim(s.substr(n));
  const Unit *u = display;
  if (!rest.empty()) {
    u = findUnit(family, rest);
    if (!u) {
      Status err = Status::error(StatusCode::InvalidArgument,
                                 (std::string("unknown ") + unitFamilyName(family) + " unit '" +
                                  std::string(rest) + "'")
                                     .c_str());
      Suggester sg(rest);
      for (const Unit &c : unitsOf(family)) {
        sg.consider(c.symbol);
        sg.consider(c.name);
      }
      sg.addTo(err);
      return err;
    }
  }
  const double base = u ? v * u->scale : v;
  if (!std::isfinite(base)) {
    return Status::error(StatusCode::OutOfRange,
                         (std::string("value out of range: '") + std::string(text) + "'").c_str());
  }
  return base;
}

} // namespace units
} // namespace coredcc
