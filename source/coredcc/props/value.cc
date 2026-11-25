#include "coredcc/props/value.h"

#include "coredcc/base/assert.h"
#include "coredcc/base/id.h"
#include "coredcc/base/text.h"

#include "litestl/util/alloc.h"

#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace coredcc {

using litestl::util::string;

const char *propValueTypeName(PropValue::Type type)
{
  switch (type) {
  case PropValue::Type::None:
    return "None";
  case PropValue::Type::Bool:
    return "Bool";
  case PropValue::Type::Int:
    return "Int";
  case PropValue::Type::Float:
    return "Float";
  case PropValue::Type::String:
    return "String";
  case PropValue::Type::Vec2:
    return "Vec2";
  case PropValue::Type::Vec3:
    return "Vec3";
  case PropValue::Type::Vec4:
    return "Vec4";
  case PropValue::Type::Mat4:
    return "Mat4";
  case PropValue::Type::Quat:
    return "Quat";
  case PropValue::Type::Enum:
    return "Enum";
  case PropValue::Type::Flags:
    return "Flags";
  case PropValue::Type::Pointer:
    return "Pointer";
  }
  return "?";
}

PropValue::Type valueTypeFor(PropKind kind)
{
  switch (kind) {
  case PropKind::Bool:
    return PropValue::Type::Bool;
  case PropKind::Int:
    return PropValue::Type::Int;
  case PropKind::Float:
    return PropValue::Type::Float;
  case PropKind::String:
    return PropValue::Type::String;
  case PropKind::Enum:
    return PropValue::Type::Enum;
  case PropKind::Flags:
    return PropValue::Type::Flags;
  case PropKind::Vec2:
    return PropValue::Type::Vec2;
  case PropKind::Vec3:
    return PropValue::Type::Vec3;
  case PropKind::Vec4:
    return PropValue::Type::Vec4;
  case PropKind::Mat4:
    return PropValue::Type::Mat4;
  case PropKind::Quat:
    return PropValue::Type::Quat;
  case PropKind::Pointer:
    return PropValue::Type::Pointer;
  case PropKind::Collection:
    return PropValue::Type::None;
  }
  return PropValue::Type::None;
}

int valueComponentCount(PropValue::Type type)
{
  switch (type) {
  case PropValue::Type::Float:
    return 1;
  case PropValue::Type::Vec2:
    return 2;
  case PropValue::Type::Vec3:
    return 3;
  case PropValue::Type::Vec4:
  case PropValue::Type::Quat:
    return 4;
  case PropValue::Type::Mat4:
    return 16;
  default:
    return 0;
  }
}

PropValue::PropValue(const PropValue &b) : type_(b.type_), heap_(nullptr)
{
  memcpy(d_, b.d_, sizeof(d_));
  if (b.heap_) {
    heap_ = litestl::alloc::New<Heap>("coredcc::PropValue::Heap", *b.heap_);
  }
}

PropValue::PropValue(PropValue &&b) noexcept : type_(b.type_), heap_(b.heap_)
{
  memcpy(d_, b.d_, sizeof(d_));
  b.type_ = Type::None;
  b.heap_ = nullptr;
}

PropValue &PropValue::operator=(const PropValue &b)
{
  if (this != &b) {
    PropValue copy(b);
    *this = std::move(copy);
  }
  return *this;
}

PropValue &PropValue::operator=(PropValue &&b) noexcept
{
  if (this != &b) {
    if (heap_) {
      litestl::alloc::Delete(heap_);
    }
    type_ = b.type_;
    memcpy(d_, b.d_, sizeof(d_));
    heap_ = b.heap_;
    b.type_ = Type::None;
    b.heap_ = nullptr;
  }
  return *this;
}

PropValue::~PropValue()
{
  if (heap_) {
    litestl::alloc::Delete(heap_);
  }
}

PropValue PropValue::ofBool(bool v)
{
  PropValue p;
  p.type_ = Type::Bool;
  p.b_ = v;
  return p;
}

PropValue PropValue::ofInt(int64_t v)
{
  PropValue p;
  p.type_ = Type::Int;
  p.i_ = v;
  return p;
}

PropValue PropValue::ofFloat(double v)
{
  PropValue p;
  p.type_ = Type::Float;
  p.d_[0] = v;
  return p;
}

PropValue PropValue::ofString(std::string_view v)
{
  PropValue p;
  p.type_ = Type::String;
  p.heap_ = litestl::alloc::New<Heap>("coredcc::PropValue::Heap");
  p.heap_->str = lsString(v);
  return p;
}

PropValue PropValue::ofVec2(double x, double y)
{
  const double c[2] = {x, y};
  return ofComponents(Type::Vec2, c);
}

PropValue PropValue::ofVec3(double x, double y, double z)
{
  const double c[3] = {x, y, z};
  return ofComponents(Type::Vec3, c);
}

PropValue PropValue::ofVec4(double x, double y, double z, double w)
{
  const double c[4] = {x, y, z, w};
  return ofComponents(Type::Vec4, c);
}

PropValue PropValue::ofQuat(double x, double y, double z, double w)
{
  const double c[4] = {x, y, z, w};
  return ofComponents(Type::Quat, c);
}

PropValue PropValue::ofComponents(Type type, const double *components)
{
  const int n = valueComponentCount(type);
  CDCC_CHECK(n > 1, "ofComponents takes a vector, Mat4 or Quat type");
  PropValue p;
  p.type_ = type;
  double *dst = p.d_;
  if (type == Type::Mat4) {
    p.heap_ = litestl::alloc::New<Heap>("coredcc::PropValue::Heap");
    dst = p.heap_->m;
  }
  for (int i = 0; i < n; i++) {
    dst[i] = components[i];
  }
  return p;
}

PropValue PropValue::identityMat4()
{
  double m[16] = {};
  m[0] = m[5] = m[10] = m[15] = 1.0;
  return ofComponents(Type::Mat4, m);
}

PropValue PropValue::ofEnum(int64_t v)
{
  PropValue p;
  p.type_ = Type::Enum;
  p.i_ = v;
  return p;
}

PropValue PropValue::ofFlags(uint64_t v)
{
  PropValue p;
  p.type_ = Type::Flags;
  p.u_ = v;
  return p;
}

PropValue PropValue::ofPointer(StableRef ref)
{
  PropValue p;
  p.type_ = Type::Pointer;
  p.heap_ = litestl::alloc::New<Heap>("coredcc::PropValue::Heap");
  /* One spelling of null, so every null Pointer compares equal. */
  if (!ref.isNull()) {
    p.heap_->ref = std::move(ref);
  }
  return p;
}

#define CDCC_PROPVALUE_EXPECT(t)                                                         \
  CDCC_CHECK(type_ == (t), "PropValue accessed as the wrong type")

bool PropValue::asBool() const
{
  CDCC_PROPVALUE_EXPECT(Type::Bool);
  return b_;
}

int64_t PropValue::asInt() const
{
  CDCC_PROPVALUE_EXPECT(Type::Int);
  return i_;
}

double PropValue::asFloat() const
{
  CDCC_PROPVALUE_EXPECT(Type::Float);
  return d_[0];
}

const string &PropValue::asString() const
{
  CDCC_PROPVALUE_EXPECT(Type::String);
  return heap_->str;
}

int64_t PropValue::asEnum() const
{
  CDCC_PROPVALUE_EXPECT(Type::Enum);
  return i_;
}

uint64_t PropValue::asFlags() const
{
  CDCC_PROPVALUE_EXPECT(Type::Flags);
  return u_;
}

const StableRef &PropValue::asPointer() const
{
  CDCC_PROPVALUE_EXPECT(Type::Pointer);
  return heap_->ref;
}

int PropValue::componentCount() const
{
  return valueComponentCount(type_);
}

double PropValue::component(int i) const
{
  CDCC_CHECK(i >= 0 && i < componentCount(), "PropValue component out of range");
  return comps()[i];
}

std::optional<bool> PropValue::tryBool() const
{
  return type_ == Type::Bool ? std::optional<bool>(b_) : std::nullopt;
}

std::optional<int64_t> PropValue::tryInt() const
{
  return type_ == Type::Int ? std::optional<int64_t>(i_) : std::nullopt;
}

std::optional<double> PropValue::tryFloat() const
{
  return type_ == Type::Float ? std::optional<double>(d_[0]) : std::nullopt;
}

const string *PropValue::tryString() const
{
  return type_ == Type::String ? &heap_->str : nullptr;
}

std::optional<int64_t> PropValue::tryEnum() const
{
  return type_ == Type::Enum ? std::optional<int64_t>(i_) : std::nullopt;
}

std::optional<uint64_t> PropValue::tryFlags() const
{
  return type_ == Type::Flags ? std::optional<uint64_t>(u_) : std::nullopt;
}

const StableRef *PropValue::tryPointer() const
{
  return type_ == Type::Pointer ? &heap_->ref : nullptr;
}

bool PropValue::operator==(const PropValue &b) const
{
  if (type_ != b.type_) {
    return false;
  }
  switch (type_) {
  case Type::None:
    return true;
  case Type::Bool:
    return b_ == b.b_;
  case Type::Int:
  case Type::Enum:
    return i_ == b.i_;
  case Type::Flags:
    return u_ == b.u_;
  case Type::String:
    return strcmp(heap_->str.c_str(), b.heap_->str.c_str()) == 0;
  case Type::Pointer:
    return heap_->ref == b.heap_->ref;
  default:
    return memcmp(comps(), b.comps(), sizeof(double) * componentCount()) == 0;
  }
}

uint64_t PropValue::computeHash() const
{
  const uint8_t t = uint8_t(type_);
  uint64_t h = detail::fnv1a64(std::string_view(reinterpret_cast<const char *>(&t), 1));
  auto bytes = [&h](const void *p, size_t n) {
    h = detail::fnv1a64(std::string_view(static_cast<const char *>(p), n), h);
  };
  switch (type_) {
  case Type::None:
    break;
  case Type::Bool:
    bytes(&b_, sizeof(b_));
    break;
  case Type::Int:
  case Type::Enum:
    bytes(&i_, sizeof(i_));
    break;
  case Type::Flags:
    bytes(&u_, sizeof(u_));
    break;
  case Type::String:
    bytes(heap_->str.c_str(), strlen(heap_->str.c_str()));
    break;
  case Type::Pointer: {
    const uint64_t r = heap_->ref.computeHash();
    bytes(&r, sizeof(r));
    break;
  }
  default:
    bytes(comps(), sizeof(double) * componentCount());
  }
  return h;
}

bool PropValue::approxEqual(const PropValue &b, double eps) const
{
  if (type_ != b.type_) {
    return false;
  }
  const int n = componentCount();
  if (n == 0) {
    return operator==(b);
  }
  for (int i = 0; i < n; i++) {
    const double x = comps()[i];
    const double y = b.comps()[i];
    if (!(std::fabs(x - y) <= eps) && !(x == y) && !(std::isnan(x) && std::isnan(y))) {
      return false;
    }
  }
  return true;
}

std::string PropValue::toString() const
{
  std::string out;
  switch (type_) {
  case Type::None:
    return "None";
  case Type::Bool:
    return b_ ? "true" : "false";
  case Type::Int:
    return std::to_string(i_);
  case Type::Enum:
    return "#" + std::to_string(i_);
  case Type::Flags: {
    char buf[32];
    snprintf(buf, sizeof(buf), "0x%" PRIx64, u_);
    return buf;
  }
  case Type::String:
    appendQuoted(out, heap_->str.c_str());
    return out;
  case Type::Pointer:
    return heap_->ref.toString();
  case Type::Float:
    appendDouble(out, d_[0]);
    return out;
  default:
    out += '(';
    for (int i = 0; i < componentCount(); i++) {
      if (i) {
        out += ", ";
      }
      appendDouble(out, comps()[i]);
    }
    out += ')';
    return out;
  }
}

} // namespace coredcc
