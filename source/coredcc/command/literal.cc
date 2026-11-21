#include "coredcc/command/literal.h"

#include "coredcc/base/text.h"

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace coredcc {

namespace {

Status bad(const PropertyDef &def, const std::string &why)
{
  return Status::error(StatusCode::InvalidArgument,
                       (std::string(def.apiName().c_str()) + ": " + why).c_str());
}

void appendNumber(std::string &out, double v)
{
  appendDouble(out, v); /* nan, inf and -inf read back as (signed) identifiers */
}

} // namespace

Result<std::string> formatLiteral(const PropertyDef &def, const PropValue &value)
{
  std::string out;
  switch (def.kind()) {
    case PropKind::Bool:
      return std::string(value.asBool() ? "true" : "false");
    case PropKind::Int:
      return std::to_string(value.asInt());
    case PropKind::Float:
      appendNumber(out, value.asFloat());
      return out;
    case PropKind::String:
      appendQuoted(out, value.asString().c_str());
      return out;
    case PropKind::Enum: {
      const EnumItem *item = def.enumMeta()->findValue(value.asEnum());
      if (!item) {
        return bad(def, "the value " + std::to_string(value.asEnum()) + " has no item");
      }
      return std::string(item->key.c_str());
    }
    case PropKind::Flags: {
      uint64_t rest = value.asFlags();
      if (rest == 0) {
        return std::string("0");
      }
      std::string keys;
      for (const EnumItem &item : def.enumMeta()->items) {
        const uint64_t bit = uint64_t(item.value);
        /* All of its bits, or a multi-bit item would add the ones that are clear. */
        if (bit != 0 && (rest & bit) == bit) {
          keys += keys.empty() ? "" : "|";
          keys += item.key.c_str();
          rest &= ~bit;
        }
      }
      if (rest != 0) {
        return bad(def, "a flag bit has no item");
      }
      appendQuoted(out, keys);
      return out;
    }
    case PropKind::Vec2:
    case PropKind::Vec3:
    case PropKind::Vec4:
    case PropKind::Quat:
    case PropKind::Mat4:
      out += '(';
      for (int i = 0; i < value.componentCount(); i++) {
        out += i ? ", " : "";
        appendNumber(out, value.component(i));
      }
      out += ')';
      return out;
    case PropKind::Pointer:
    case PropKind::Collection:
      break;
  }
  return Status::error(StatusCode::Unsupported,
                       (std::string(def.apiName().c_str()) + ": " + propKindName(def.kind()) +
                        " values have no literal form")
                           .c_str());
}

namespace {

/* An integer literal; out of int64's range is an error, not a clamp. */
Result<int64_t> integer(const PropertyDef &def, const char *text)
{
  errno = 0;
  const long long v = std::strtoll(text, nullptr, 10);
  if (errno == ERANGE) {
    return bad(def, std::string("'") + text + "' is out of range");
  }
  return int64_t(v);
}

/* A scalar as a number, for numeric kinds; nan and inf are identifiers. */
Result<double> number(const PropertyDef &def, const RawScalar &s)
{
  if (s.kind == RawScalar::Kind::Number) {
    const double v = std::strtod(s.text.c_str(), nullptr);
    if (std::isinf(v)) {
      return bad(def, std::string("'") + s.text.c_str() + "' is out of range (write inf)");
    }
    return v;
  }
  if (s.kind == RawScalar::Kind::Identifier) {
    const char *t = s.text.c_str();
    if (strcmp(t, "inf") == 0 || strcmp(t, "+inf") == 0) {
      return INFINITY;
    }
    if (strcmp(t, "-inf") == 0) {
      return -INFINITY;
    }
    if (strcmp(t, "nan") == 0) {
      return double(NAN);
    }
  }
  return bad(def, std::string("expected a number, got '") + s.text.c_str() + "'");
}

} // namespace

Result<PropValue> parseLiteral(const PropertyDef &def, const RawValue &raw)
{
  const int comps = componentCount(def.kind());
  if (comps > 1) {
    if (!raw.isTuple || int(raw.items.size()) != comps) {
      return bad(def, "expected a tuple of " + std::to_string(comps) + " numbers");
    }
    double c[16];
    for (int i = 0; i < comps; i++) {
      Result<double> d = number(def, raw.items[i]);
      if (!d.ok()) {
        return d.status();
      }
      c[i] = *d;
    }
    return coerce(def, PropValue::ofComponents(valueTypeFor(def.kind()), c));
  }
  if (raw.isTuple || raw.items.size() != 1) {
    return bad(def, "expected a single value, not a tuple");
  }
  const RawScalar &s = raw.items[0];
  const char *text = s.text.c_str();
  switch (def.kind()) {
    case PropKind::Bool:
      if (s.kind == RawScalar::Kind::Identifier &&
          (strcmp(text, "true") == 0 || strcmp(text, "false") == 0))
      {
        return PropValue::ofBool(text[0] == 't');
      }
      if (s.kind == RawScalar::Kind::Number && s.isInteger) {
        Result<int64_t> i = integer(def, text);
        return i.ok() ? coerce(def, PropValue::ofInt(*i)) : Result<PropValue>(i.status());
      }
      return bad(def, std::string("expected true or false, got '") + text + "'");
    case PropKind::Int:
      if (s.kind == RawScalar::Kind::Number && s.isInteger) {
        Result<int64_t> i = integer(def, text);
        return i.ok() ? coerce(def, PropValue::ofInt(*i)) : Result<PropValue>(i.status());
      }
      break;
    case PropKind::String:
      if (s.kind == RawScalar::Kind::String) {
        return coerce(def, PropValue::ofString(text));
      }
      return bad(def, "expected a quoted string");
    case PropKind::Enum:
    case PropKind::Flags:
      if (s.kind == RawScalar::Kind::Number && s.isInteger) {
        Result<int64_t> i = integer(def, text);
        if (!i.ok()) {
          return i.status();
        }
        return coerce(def, def.kind() == PropKind::Enum ? PropValue::ofEnum(*i) :
                                                          PropValue::ofFlags(uint64_t(*i)));
      }
      return coerce(def, PropValue::ofString(text)); /* a key, or "A|B" */
    case PropKind::Pointer:
    case PropKind::Collection:
      return Status::error(StatusCode::Unsupported,
                           (std::string(def.apiName().c_str()) + ": " +
                            propKindName(def.kind()) + " values have no literal form")
                               .c_str());
    default:
      break;
  }
  Result<double> d = number(def, s);
  if (!d.ok()) {
    return d.status();
  }
  return coerce(def, PropValue::ofFloat(*d));
}

} // namespace coredcc
