#include "coredcc/props/def.h"

#include "coredcc/base/text.h"

#include <cmath>
#include <cstring>
#include <string>

namespace coredcc {

using litestl::util::string;

namespace {

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

/* Flag values are bit masks: compare as uint64 so bit 63 works. */
bool isSingleBit(int64_t v)
{
  const uint64_t u = uint64_t(v);
  return u != 0 && (u & (u - 1)) == 0;
}

} // namespace

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
    : kind_(kind), apiName_(lsString(apiName)), uiName_(lsString(uiNameFromApiName(apiName)))
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
  uiName_ = s.empty() ? lsString(uiNameFromApiName(apiName_.c_str())) : lsString(s);
  return *this;
}

PropertyDef &PropertyDef::description(std::string_view s)
{
  description_ = lsString(s);
  return *this;
}

PropertyDef &PropertyDef::icon(std::string_view s)
{
  icon_ = lsString(s);
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
  defaultSet_ = true;
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
  it.key = lsString(key);
  it.value = value;
  it.uiName = lsString(uiName.empty() ? uiNameFromApiName(key) : std::string(uiName));
  it.description = lsString(description);
  it.icon = lsString(icon);
  m.items.append(std::move(it));
  if (m.items.size() == 1 && kind_ == PropKind::Enum && !defaultSet_) {
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

namespace {

bool same(const NumericMeta &a, const NumericMeta &b)
{
  return a.hardMin == b.hardMin && a.hardMax == b.hardMax && a.softMin == b.softMin &&
         a.softMax == b.softMax && a.step == b.step && a.precision == b.precision &&
         a.unit == b.unit;
}

bool same(const EnumMeta &a, const EnumMeta &b)
{
  bool out = a.items.size() == b.items.size();
  for (int i = 0; out && i < int(a.items.size()); i++) {
    const EnumItem &x = a.items[i], &y = b.items[i];
    out = x.key == y.key && x.value == y.value && x.uiName == y.uiName &&
          x.description == y.description && x.icon == y.icon;
  }
  return out;
}

bool same(const StringMeta &a, const StringMeta &b)
{
  return a.maxLength == b.maxLength;
}

bool same(const TargetMeta &a, const TargetMeta &b)
{
  return a.target == b.target;
}

/* Both or neither, and the same. */
template <class T> bool sameOrNone(const T *a, const T *b)
{
  return a && b ? same(*a, *b) : a == b;
}

} // namespace

bool PropertyDef::sameAs(const PropertyDef &b) const
{
  return kind_ == b.kind_ && apiName_ == b.apiName_ && uiName_ == b.uiName_ &&
         description_ == b.description_ && icon_ == b.icon_ && subtype_ == b.subtype_ &&
         access_ == b.access_ && ui_.iconsOnly == b.ui_.iconsOnly && ui_.expand == b.ui_.expand &&
         ui_.slider == b.ui_.slider && ui_.multiline == b.ui_.multiline && default_ == b.default_ &&
         sameOrNone(numeric(), b.numeric()) && sameOrNone(enumMeta(), b.enumMeta()) &&
         sameOrNone(stringMeta(), b.stringMeta()) && sameOrNone(target(), b.target());
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
    if (!(m->step >= 0)) {
      return fail("step must be >= 0");
    }
    if (kind_ == PropKind::Int && !(std::ceil(m->hardMin) <= std::floor(m->hardMax))) {
      return fail("the hard range holds no integer");
    }
  }
  if (const StringMeta *m = stringMeta(); m && m->maxLength < 0) {
    return fail("maxLength must be >= 0");
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
      if (kind_ == PropKind::Flags && !isSingleBit(it.value)) {
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
  if (default_.type() != valueTypeFor(kind_)) {
    return fail(std::string("the default value is a ") + propValueTypeName(default_.type()) +
                ", not a " + propKindName(kind_));
  }
  /* The static def: a dynamic hook needs a real owner, which validation doesn't have. */
  PropertyDef plain = *this;
  plain.dynamic_ = nullptr;
  Result<PropValue> d = coerce(plain, default_);
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

/* The hard range as integers, compared in int64 (comparing in double rounds above
 * 2^53). Bounds outside int64 don't clamp. */
int64_t clampI(int64_t v, const NumericMeta &m)
{
  const double lo = std::ceil(m.hardMin);
  const double hi = std::floor(m.hardMax);
  if (lo > -kTwo63 && lo < kTwo63 && v < int64_t(lo)) {
    return int64_t(lo);
  }
  if (hi > -kTwo63 && hi < kTwo63 && v > int64_t(hi)) {
    return int64_t(hi);
  }
  return v;
}

Status nanError(const PropertyDef &def)
{
  return Status::error(StatusCode::OutOfRange,
                       (std::string(def.apiName().c_str()) + ": NaN is not a value").c_str());
}

/* An Int, or a Float holding an integer in int64 (JS numbers arrive as Float). */
std::optional<int64_t> integral(const PropValue &v)
{
  if (std::optional<int64_t> i = v.tryInt()) {
    return i;
  }
  int64_t out = 0;
  if (std::optional<double> f = v.tryFloat(); f && integralInt64(*f, out)) {
    return out;
  }
  return std::nullopt;
}

Result<PropValue> coerceEnum(const PropertyDef &def, const PropValue &v)
{
  const EnumMeta &m = *def.enumMeta();
  std::optional<int64_t> value = v.tryEnum();
  if (!value) {
    value = integral(v);
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
    if (std::optional<int64_t> i = integral(v); i && *i >= 0) {
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
  if (def.isDynamic()) {
    return Status::error(StatusCode::FailedPrecondition,
                         (std::string(def.apiName().c_str()) +
                          ": a dynamic def must be resolved for its owner first")
                             .c_str());
  }
  const NumericMeta *num = def.numeric();
  switch (def.kind()) {
    case PropKind::Bool:
      if (v.type() == PropValue::Type::Bool) {
        return v;
      }
      if (std::optional<int64_t> i = integral(v); i && (*i == 0 || *i == 1)) {
        return PropValue::ofBool(*i == 1);
      }
      return mismatch(def, v);
    case PropKind::Int: {
      int64_t i = 0;
      if (std::optional<int64_t> vi = v.tryInt()) {
        i = *vi;
      }
      else if (std::optional<double> vf = v.tryFloat()) {
        if (std::isnan(*vf)) {
          return nanError(def);
        }
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
      if (std::isnan(d)) {
        return nanError(def);
      }
      return PropValue::ofFloat(clampD(d, *num));
    }
    case PropKind::Vec2:
    case PropKind::Vec3:
    case PropKind::Vec4:
    case PropKind::Quat:
    case PropKind::Mat4: {
      const PropValue::Type want = valueTypeFor(def.kind());
      const int n = valueComponentCount(want);
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
        if (std::isnan(c[i])) {
          return nanError(def);
        }
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
      if (const StableRef *ref = v.tryPointer()) {
        const StructId want = def.target()->target;
        if (!ref->isNull() && want && ref->type && ref->type != want) {
          return Status::error(StatusCode::TypeMismatch,
                               (std::string(def.apiName().c_str()) +
                                ": the pointer's target is of another struct type")
                                   .c_str());
        }
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
