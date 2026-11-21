#include "coredcc/props/def.h"

#include "coredcc/base/text.h"

#include <cmath>
#include <cstring>
#include <string>

namespace coredcc {

using litestl::util::string;

namespace {

string toLs(std::string_view s)
{
  return string(std::string(s).c_str());
}

bool isUpper(char c)
{
  return c >= 'A' && c <= 'Z';
}

bool isLower(char c)
{
  return c >= 'a' && c <= 'z';
}

bool isDigit(char c)
{
  return c >= '0' && c <= '9';
}

char toUpper(char c)
{
  return isLower(c) ? char(c - 'a' + 'A') : c;
}

bool isPowerOfTwo(int64_t v)
{
  return v > 0 && (v & (v - 1)) == 0;
}

} // namespace

const char *propKindName(PropKind kind)
{
  switch (kind) {
    case PropKind::Bool:
      return "Bool";
    case PropKind::Int:
      return "Int";
    case PropKind::Float:
      return "Float";
    case PropKind::String:
      return "String";
    case PropKind::Enum:
      return "Enum";
    case PropKind::Flags:
      return "Flags";
    case PropKind::Vec2:
      return "Vec2";
    case PropKind::Vec3:
      return "Vec3";
    case PropKind::Vec4:
      return "Vec4";
    case PropKind::Mat4:
      return "Mat4";
    case PropKind::Quat:
      return "Quat";
    case PropKind::Pointer:
      return "Pointer";
    case PropKind::Collection:
      return "Collection";
  }
  return "?";
}

const char *propSubtypeName(PropSubtype subtype)
{
  switch (subtype) {
    case PropSubtype::None:
      return "None";
    case PropSubtype::Color:
      return "Color";
    case PropSubtype::ColorGamma:
      return "ColorGamma";
    case PropSubtype::Angle:
      return "Angle";
    case PropSubtype::Distance:
      return "Distance";
    case PropSubtype::Factor:
      return "Factor";
    case PropSubtype::Percentage:
      return "Percentage";
    case PropSubtype::Time:
      return "Time";
    case PropSubtype::Direction:
      return "Direction";
    case PropSubtype::Euler:
      return "Euler";
    case PropSubtype::Xyz:
      return "Xyz";
    case PropSubtype::FilePath:
      return "FilePath";
    case PropSubtype::Password:
      return "Password";
  }
  return "?";
}

std::string uiNameFromApiName(std::string_view s)
{
  std::string out;
  bool wordStart = true;
  for (size_t i = 0; i < s.size(); i++) {
    const char c = s[i];
    if (c == '_') {
      wordStart = true;
      continue;
    }
    if (i > 0 && !wordStart) {
      const char prev = s[i - 1];
      /* "edgeCrease" and "HTTPServer" (the S starts a word: it's followed by lowercase). */
      const bool lowerToUpper = (isLower(prev) || isDigit(prev)) && isUpper(c);
      const bool acronymEnd = isUpper(prev) && isUpper(c) && i + 1 < s.size() &&
                              isLower(s[i + 1]);
      wordStart = lowerToUpper || acronymEnd;
    }
    if (wordStart && !out.empty()) {
      out += ' ';
    }
    out += wordStart ? toUpper(c) : c;
    wordStart = false;
  }
  return out;
}

const EnumItem *EnumMeta::findKey(std::string_view key) const
{
  for (const EnumItem &it : items) {
    if (key == it.key.c_str()) {
      return &it;
    }
  }
  return nullptr;
}

const EnumItem *EnumMeta::findValue(int64_t value) const
{
  for (const EnumItem &it : items) {
    if (it.value == value) {
      return &it;
    }
  }
  return nullptr;
}

PropertyDef::PropertyDef(PropKind kind, std::string_view apiName)
    : kind_(kind), apiName_(toLs(apiName)), uiName_(toLs(uiNameFromApiName(apiName)))
{
  switch (kind) {
    case PropKind::Bool:
      default_ = PropValue::ofBool(false);
      break;
    case PropKind::Int:
      meta_ = NumericMeta{};
      std::get<NumericMeta>(meta_).precision = 0;
      default_ = PropValue::ofInt(0);
      break;
    case PropKind::Float:
      meta_ = NumericMeta{};
      default_ = PropValue::ofFloat(0.0);
      break;
    case PropKind::Vec2:
    case PropKind::Vec3:
    case PropKind::Vec4: {
      meta_ = NumericMeta{};
      const double zero[4] = {};
      default_ = PropValue::ofComponents(valueTypeFor(kind), zero);
      break;
    }
    case PropKind::Quat:
      meta_ = NumericMeta{};
      default_ = PropValue::ofQuat(0, 0, 0, 1);
      break;
    case PropKind::Mat4:
      meta_ = NumericMeta{};
      default_ = PropValue::identityMat4();
      break;
    case PropKind::String:
      meta_ = StringMeta{};
      default_ = PropValue::ofString("");
      break;
    case PropKind::Enum:
      meta_ = EnumMeta{};
      default_ = PropValue::ofEnum(0);
      break;
    case PropKind::Flags:
      meta_ = EnumMeta{};
      default_ = PropValue::ofFlags(0);
      break;
    case PropKind::Pointer:
      meta_ = TargetMeta{};
      default_ = PropValue::nullPointer();
      break;
    case PropKind::Collection:
      meta_ = TargetMeta{};
      break;
  }
}

PropertyDef PropertyDef::vecProp(std::string_view name, int components)
{
  CDCC_CHECK(components >= 2 && components <= 4, "vecProp takes 2, 3 or 4 components");
  const PropKind k = components == 2 ? PropKind::Vec2 :
                     components == 3 ? PropKind::Vec3 :
                                       PropKind::Vec4;
  return {k, name};
}

PropertyDef PropertyDef::pointerProp(std::string_view name, StructId target)
{
  PropertyDef d(PropKind::Pointer, name);
  std::get<TargetMeta>(d.meta_).target = target;
  return d;
}

PropertyDef PropertyDef::collectionProp(std::string_view name, StructId element)
{
  PropertyDef d(PropKind::Collection, name);
  std::get<TargetMeta>(d.meta_).target = element;
  return d;
}

NumericMeta &PropertyDef::numericMut()
{
  NumericMeta *m = std::get_if<NumericMeta>(&meta_);
  CDCC_CHECK(m != nullptr, "numeric metadata on a non-numeric property");
  return *m;
}

EnumMeta &PropertyDef::enumMut()
{
  EnumMeta *m = std::get_if<EnumMeta>(&meta_);
  CDCC_CHECK(m != nullptr, "enum items on a property that isn't Enum or Flags");
  return *m;
}

PropertyDef &PropertyDef::uiName(std::string_view s)
{
  uiName_ = toLs(s);
  return *this;
}

PropertyDef &PropertyDef::description(std::string_view s)
{
  description_ = toLs(s);
  return *this;
}

PropertyDef &PropertyDef::icon(std::string_view s)
{
  icon_ = toLs(s);
  return *this;
}

PropertyDef &PropertyDef::subtype(PropSubtype s)
{
  subtype_ = s;
  return *this;
}

PropertyDef &PropertyDef::access(PropAccess a)
{
  access_ = access_ | a;
  return *this;
}

PropertyDef &PropertyDef::ui(PropUi u)
{
  ui_ = u;
  return *this;
}

PropertyDef &PropertyDef::defaultValue(PropValue v)
{
  default_ = std::move(v);
  return *this;
}

PropertyDef &PropertyDef::dynamic(PropDynamicFn fn)
{
  dynamic_ = std::move(fn);
  return *this;
}

PropertyDef &PropertyDef::range(double hardMin, double hardMax)
{
  NumericMeta &m = numericMut();
  m.hardMin = hardMin;
  m.hardMax = hardMax;
  return *this;
}

PropertyDef &PropertyDef::softRange(double softMin, double softMax)
{
  NumericMeta &m = numericMut();
  m.softMin = softMin;
  m.softMax = softMax;
  return *this;
}

PropertyDef &PropertyDef::step(double s)
{
  numericMut().step = s;
  return *this;
}

PropertyDef &PropertyDef::precision(int p)
{
  numericMut().precision = p;
  return *this;
}

PropertyDef &PropertyDef::unit(UnitFamily u)
{
  numericMut().unit = u;
  return *this;
}

PropertyDef &PropertyDef::item(std::string_view key,
                               int64_t value,
                               std::string_view uiName,
                               std::string_view description,
                               std::string_view icon)
{
  EnumMeta &m = enumMut();
  EnumItem it;
  it.key = toLs(key);
  it.value = value;
  it.uiName = toLs(uiName.empty() ? uiNameFromApiName(key) : std::string(uiName));
  it.description = toLs(description);
  it.icon = toLs(icon);
  m.items.append(std::move(it));
  if (m.items.size() == 1 && kind_ == PropKind::Enum) {
    default_ = PropValue::ofEnum(value);
  }
  return *this;
}

PropertyDef &PropertyDef::maxLength(int64_t n)
{
  StringMeta *m = std::get_if<StringMeta>(&meta_);
  CDCC_CHECK(m != nullptr, "maxLength on a non-String property");
  m->maxLength = n;
  return *this;
}

PropertyDef PropertyDef::resolved(const void *owner) const
{
  PropertyDef copy = *this;
  if (dynamic_) {
    copy.dynamic_ = nullptr;
    dynamic_(copy, owner);
  }
  return copy;
}

Status PropertyDef::validate() const
{
  const std::string name = apiName_.c_str();
  auto fail = [&name](const std::string &why) {
    return Status::error(StatusCode::InvalidArgument,
                         ("property '" + name + "': " + why).c_str());
  };
  if (!isIdentifier(name)) {
    return fail("the API name must be an identifier");
  }
  if (const NumericMeta *m = numeric()) {
    if (std::isnan(m->hardMin) || std::isnan(m->hardMax) || m->hardMin > m->hardMax) {
      return fail("the hard range is empty");
    }
    if (std::isnan(m->softMin) || std::isnan(m->softMax) || m->softMin > m->softMax) {
      return fail("the soft range is empty");
    }
    if ((std::isfinite(m->softMin) && m->softMin < m->hardMin) ||
        (std::isfinite(m->softMax) && m->softMax > m->hardMax)) {
      return fail("the soft range must lie inside the hard range");
    }
    if (m->precision < 0 || m->precision > 15) {
      return fail("precision must be 0 to 15");
    }
  }
  if (const EnumMeta *m = enumMeta()) {
    if (kind_ == PropKind::Enum && m->items.size() == 0) {
      return fail("an Enum needs at least one item");
    }
    for (int i = 0; i < int(m->items.size()); i++) {
      const EnumItem &it = m->items[i];
      if (!isIdentifier(it.key.c_str())) {
        return fail(std::string("item key '") + it.key.c_str() + "' is not an identifier");
      }
      if (kind_ == PropKind::Flags && !isPowerOfTwo(it.value)) {
        return fail(std::string("flag '") + it.key.c_str() + "' must be a single bit");
      }
      for (int j = 0; j < i; j++) {
        if (strcmp(m->items[j].key.c_str(), it.key.c_str()) == 0) {
          return fail(std::string("item key '") + it.key.c_str() + "' is used twice");
        }
        if (m->items[j].value == it.value) {
          return fail(std::string("items '") + m->items[j].key.c_str() + "' and '" +
                      it.key.c_str() + "' share a value");
        }
      }
    }
  }
  if (kind_ == PropKind::Collection) {
    return default_.isNone() ? Status() : fail("a Collection has no default value");
  }
  Result<PropValue> d = coerce(*this, default_);
  if (!d.ok()) {
    return fail(std::string("bad default value: ") + d.status().message());
  }
  if (*d != default_) {
    return fail("the default value " + default_.toString() + " is out of range");
  }
  return Status();
}

/* ---- coerce ---- */

namespace {

Status mismatch(const PropertyDef &def, const PropValue &v)
{
  return Status::error(StatusCode::TypeMismatch,
                       (std::string(def.apiName().c_str()) + ": expected " +
                        propKindName(def.kind()) + ", got " + propValueTypeName(v.type()))
                           .c_str());
}

/* 2^63 as a double; every double below it in magnitude converts to int64 exactly. */
constexpr double kTwo63 = 9223372036854775808.0;

bool integralInt64(double d, int64_t &out)
{
  if (!(d >= -kTwo63 && d < kTwo63) || std::trunc(d) != d) {
    return false;
  }
  out = int64_t(d);
  return true;
}

double clampD(double v, const NumericMeta &m)
{
  return v < m.hardMin ? m.hardMin : (v > m.hardMax ? m.hardMax : v);
}

int64_t clampI(int64_t v, const NumericMeta &m)
{
  if (double(v) < m.hardMin && m.hardMin > -kTwo63) {
    return int64_t(std::ceil(m.hardMin));
  }
  if (double(v) > m.hardMax && m.hardMax < kTwo63) {
    return int64_t(std::floor(m.hardMax));
  }
  return v;
}

Result<PropValue> coerceEnum(const PropertyDef &def, const PropValue &v)
{
  const EnumMeta &m = *def.enumMeta();
  std::optional<int64_t> value = v.tryEnum();
  if (!value) {
    value = v.tryInt();
  }
  if (value) {
    if (!m.findValue(*value)) {
      return Status::error(StatusCode::OutOfRange,
                           (std::string(def.apiName().c_str()) + ": " +
                            std::to_string(*value) + " is not one of its values")
                               .c_str());
    }
    return PropValue::ofEnum(*value);
  }
  if (const string *key = v.tryString()) {
    if (const EnumItem *it = m.findKey(key->c_str())) {
      return PropValue::ofEnum(it->value);
    }
    Status err = Status::error(StatusCode::NotFound,
                               (std::string(def.apiName().c_str()) + ": no item '" +
                                key->c_str() + "'")
                                   .c_str());
    Suggester sg(key->c_str());
    for (const EnumItem &it : m.items) {
      sg.consider(it.key.c_str());
    }
    sg.addTo(err);
    return err;
  }
  return mismatch(def, v);
}

Result<PropValue> coerceFlags(const PropertyDef &def, const PropValue &v)
{
  const EnumMeta &m = *def.enumMeta();
  uint64_t known = 0;
  for (const EnumItem &it : m.items) {
    known |= uint64_t(it.value);
  }
  std::optional<uint64_t> bits = v.tryFlags();
  if (!bits) {
    if (std::optional<int64_t> i = v.tryInt(); i && *i >= 0) {
      bits = uint64_t(*i);
    }
  }
  if (bits) {
    if (*bits & ~known) {
      return Status::error(StatusCode::OutOfRange,
                           (std::string(def.apiName().c_str()) + ": " + v.toString() +
                            " sets bits that are not flags")
                               .c_str());
    }
    return PropValue::ofFlags(*bits);
  }
  if (const string *keys = v.tryString()) {
    uint64_t out = 0;
    std::string_view rest = keys->c_str();
    /* "" is no flags; otherwise every '|'-separated piece, including an empty one, must be
     * a key. */
    for (bool more = !rest.empty(); more;) {
      const size_t bar = rest.find('|');
      more = bar != std::string_view::npos;
      const std::string_view key = rest.substr(0, bar);
      const EnumItem *it = m.findKey(key);
      if (!it) {
        Status err = Status::error(StatusCode::NotFound,
                                   (std::string(def.apiName().c_str()) + ": no flag '" +
                                    std::string(key) + "'")
                                       .c_str());
        Suggester sg(key);
        for (const EnumItem &c : m.items) {
          sg.consider(c.key.c_str());
        }
        sg.addTo(err);
        return err;
      }
      out |= uint64_t(it->value);
      rest = more ? rest.substr(bar + 1) : std::string_view();
    }
    return PropValue::ofFlags(out);
  }
  return mismatch(def, v);
}

} // namespace

Result<PropValue> coerce(const PropertyDef &def, const PropValue &v)
{
  const NumericMeta *num = def.numeric();
  switch (def.kind()) {
    case PropKind::Bool:
      if (v.type() == PropValue::Type::Bool) {
        return v;
      }
      if (std::optional<int64_t> i = v.tryInt(); i && (*i == 0 || *i == 1)) {
        return PropValue::ofBool(*i == 1);
      }
      return mismatch(def, v);
    case PropKind::Int: {
      int64_t i = 0;
      if (std::optional<int64_t> vi = v.tryInt()) {
        i = *vi;
      }
      else if (std::optional<double> vf = v.tryFloat()) {
        if (!integralInt64(*vf, i)) {
          return Status::error(StatusCode::OutOfRange,
                               (std::string(def.apiName().c_str()) + ": " + v.toString() +
                                " is not an integer in the int64 range")
                                   .c_str());
        }
      }
      else {
        return mismatch(def, v);
      }
      return PropValue::ofInt(clampI(i, *num));
    }
    case PropKind::Float: {
      double d = 0.0;
      if (std::optional<double> vf = v.tryFloat()) {
        d = *vf;
      }
      else if (std::optional<int64_t> vi = v.tryInt()) {
        d = double(*vi);
      }
      else {
        return mismatch(def, v);
      }
      return PropValue::ofFloat(clampD(d, *num));
    }
    case PropKind::Vec2:
    case PropKind::Vec3:
    case PropKind::Vec4:
    case PropKind::Quat:
    case PropKind::Mat4: {
      const PropValue::Type want = valueTypeFor(def.kind());
      const int n = componentCount(want);
      double c[16];
      if (v.type() == want ||
          (want == PropValue::Type::Quat && v.type() == PropValue::Type::Vec4)) {
        for (int i = 0; i < n; i++) {
          c[i] = v.component(i);
        }
      }
      else if (isVector(def.kind()) &&
               (v.type() == PropValue::Type::Float || v.type() == PropValue::Type::Int)) {
        const double s = v.type() == PropValue::Type::Float ? v.asFloat() : double(v.asInt());
        for (int i = 0; i < n; i++) {
          c[i] = s;
        }
      }
      else {
        return mismatch(def, v);
      }
      for (int i = 0; i < n; i++) {
        c[i] = clampD(c[i], *num);
      }
      return PropValue::ofComponents(want, c);
    }
    case PropKind::String: {
      const string *s = v.tryString();
      if (!s) {
        return mismatch(def, v);
      }
      const int64_t max = def.stringMeta()->maxLength;
      if (max > 0 && int64_t(strlen(s->c_str())) > max) {
        return Status::error(StatusCode::OutOfRange,
                             (std::string(def.apiName().c_str()) + ": longer than " +
                              std::to_string(max) + " bytes")
                                 .c_str());
      }
      return v;
    }
    case PropKind::Enum:
      return coerceEnum(def, v);
    case PropKind::Flags:
      return coerceFlags(def, v);
    case PropKind::Pointer:
      if (v.type() == PropValue::Type::Pointer) {
        return v;
      }
      if (v.isNone()) {
        return PropValue::nullPointer();
      }
      return mismatch(def, v);
    case PropKind::Collection:
      return Status::error(StatusCode::Unsupported,
                           (std::string(def.apiName().c_str()) +
                            ": a Collection can't be set as a value")
                               .c_str());
  }
  return mismatch(def, v);
}

} // namespace coredcc
