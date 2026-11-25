#include "coredcc/graph/value.h"

#include "coredcc/base/assert.h"
#include "coredcc/base/text.h"

#include "litestl/util/alloc.h"

#include <cstring>
#include <new>
#include <string>
#include <utility>

namespace coredcc {

using litestl::util::string;

const char *graphValueTypeName(GraphValue::Type type)
{
  switch (type) {
    case GraphValue::Type::None:
      return "None";
    case GraphValue::Type::Bool:
      return "Bool";
    case GraphValue::Type::Int:
      return "Int";
    case GraphValue::Type::Float:
      return "Float";
    case GraphValue::Type::String:
      return "String";
    case GraphValue::Type::Vec2:
      return "Vec2";
    case GraphValue::Type::Vec3:
      return "Vec3";
    case GraphValue::Type::Vec4:
      return "Vec4";
    case GraphValue::Type::Mat4:
      return "Mat4";
    case GraphValue::Type::Quat:
      return "Quat";
    case GraphValue::Type::Payload:
      return "Payload";
  }
  return "?";
}

int graphComponentCount(GraphValue::Type type)
{
  switch (type) {
    case GraphValue::Type::Float:
      return 1;
    case GraphValue::Type::Vec2:
      return 2;
    case GraphValue::Type::Vec3:
      return 3;
    case GraphValue::Type::Vec4:
    case GraphValue::Type::Quat:
      return 4;
    case GraphValue::Type::Mat4:
      return 16;
    default:
      return 0;
  }
}

GraphValue::Type graphTypeFor(PropKind kind)
{
  switch (kind) {
    case PropKind::Bool:
      return GraphValue::Type::Bool;
    case PropKind::Int:
      return GraphValue::Type::Int;
    case PropKind::Float:
      return GraphValue::Type::Float;
    case PropKind::String:
      return GraphValue::Type::String;
    case PropKind::Vec2:
      return GraphValue::Type::Vec2;
    case PropKind::Vec3:
      return GraphValue::Type::Vec3;
    case PropKind::Vec4:
      return GraphValue::Type::Vec4;
    case PropKind::Mat4:
      return GraphValue::Type::Mat4;
    case PropKind::Quat:
      return GraphValue::Type::Quat;
    default:
      return GraphValue::Type::None;
  }
}

/* ---- copying ---- */

void GraphValue::reset()
{
  if (type_ == Type::String) {
    litestl::alloc::Delete(s_);
  }
  else if (type_ == Type::Payload) {
    p_.~PayloadRef();
  }
  type_ = Type::None;
}

GraphValue::GraphValue(const GraphValue &b)
{
  if (b.type_ == Type::String) {
    s_ = litestl::alloc::New<string>("coredcc::GraphValue string", *b.s_);
  }
  else if (b.type_ == Type::Payload) {
    new (&p_) PayloadRef(b.p_);
  }
  else if (b.type_ == Type::Bool) {
    b_ = b.b_;
  }
  else if (b.type_ == Type::Int) {
    i_ = b.i_;
  }
  else {
    memcpy(d_, b.d_, sizeof(double) * size_t(graphComponentCount(b.type_)));
  }
  type_ = b.type_;
}

GraphValue::GraphValue(GraphValue &&b) noexcept
{
  operator=(std::move(b));
}

GraphValue &GraphValue::operator=(const GraphValue &b)
{
  /* Copied first: `b` may live in what this holds. */
  GraphValue copy(b);
  return operator=(std::move(copy));
}

GraphValue &GraphValue::operator=(GraphValue &&b) noexcept
{
  if (this == &b) {
    return *this;
  }
  if (b.type_ == Type::String) {
    string *s = b.s_;
    b.type_ = Type::None;
    reset();
    s_ = s;
    type_ = Type::String;
  }
  else if (b.type_ == Type::Payload) {
    PayloadRef p = std::move(b.p_);
    b.reset();
    reset();
    new (&p_) PayloadRef(std::move(p));
    type_ = Type::Payload;
  }
  else {
    reset();
    new (this) GraphValue(static_cast<const GraphValue &>(b));
  }
  return *this;
}

/* ---- making ---- */

GraphValue GraphValue::ofBool(bool v)
{
  GraphValue g;
  g.b_ = v;
  g.type_ = Type::Bool;
  return g;
}

GraphValue GraphValue::ofInt(int64_t v)
{
  GraphValue g;
  g.i_ = v;
  g.type_ = Type::Int;
  return g;
}

GraphValue GraphValue::ofFloat(double v)
{
  GraphValue g;
  g.d_[0] = v;
  g.type_ = Type::Float;
  return g;
}

GraphValue GraphValue::ofString(std::string_view v)
{
  GraphValue g;
  g.s_ = litestl::alloc::New<string>("coredcc::GraphValue string", lsString(v));
  g.type_ = Type::String;
  return g;
}

GraphValue GraphValue::ofComponents(Type type, const double *components)
{
  const int n = graphComponentCount(type);
  CDCC_CHECK(n > 0, "GraphValue::ofComponents: a type without components");
  GraphValue g;
  memcpy(g.d_, components, sizeof(double) * size_t(n));
  g.type_ = type;
  return g;
}

GraphValue GraphValue::ofVec2(double x, double y)
{
  const double c[] = {x, y};
  return ofComponents(Type::Vec2, c);
}

GraphValue GraphValue::ofVec3(double x, double y, double z)
{
  const double c[] = {x, y, z};
  return ofComponents(Type::Vec3, c);
}

GraphValue GraphValue::ofVec4(double x, double y, double z, double w)
{
  const double c[] = {x, y, z, w};
  return ofComponents(Type::Vec4, c);
}

GraphValue GraphValue::ofQuat(double x, double y, double z, double w)
{
  const double c[] = {x, y, z, w};
  return ofComponents(Type::Quat, c);
}

GraphValue GraphValue::identityMat4()
{
  const double c[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  return ofComponents(Type::Mat4, c);
}

GraphValue GraphValue::ofPayload(PayloadRef ref)
{
  GraphValue g;
  new (&g.p_) PayloadRef(std::move(ref));
  g.type_ = Type::Payload;
  return g;
}

/* ---- PropValue ---- */

namespace {

/* The types both have, in one table. */
struct TypePair {
  GraphValue::Type graph;
  PropValue::Type prop;
};
constexpr TypePair kComponentTypes[] = {
    {GraphValue::Type::Float, PropValue::Type::Float},
    {GraphValue::Type::Vec2, PropValue::Type::Vec2},
    {GraphValue::Type::Vec3, PropValue::Type::Vec3},
    {GraphValue::Type::Vec4, PropValue::Type::Vec4},
    {GraphValue::Type::Mat4, PropValue::Type::Mat4},
    {GraphValue::Type::Quat, PropValue::Type::Quat},
};

} // namespace

Result<GraphValue> GraphValue::fromProp(const PropValue &v)
{
  switch (v.type()) {
    case PropValue::Type::None:
      return GraphValue();
    case PropValue::Type::Bool:
      return ofBool(v.asBool());
    case PropValue::Type::Int:
      return ofInt(v.asInt());
    case PropValue::Type::String:
      return ofString(v.asString().c_str());
    default:
      break;
  }
  for (const TypePair &t : kComponentTypes) {
    if (t.prop == v.type()) {
      double c[16];
      for (int i = 0; i < v.componentCount(); i++) {
        c[i] = v.component(i);
      }
      return ofComponents(t.graph, c);
    }
  }
  return Status::error(StatusCode::Unsupported,
                       (std::string("a graph socket can't hold ") +
                        propValueTypeName(v.type()) + " values")
                           .c_str());
}

PropValue GraphValue::toProp() const
{
  switch (type_) {
    case Type::Bool:
      return PropValue::ofBool(b_);
    case Type::Int:
      return PropValue::ofInt(i_);
    case Type::Float:
      return PropValue::ofFloat(d_[0]);
    case Type::String:
      return PropValue::ofString(s_->c_str());
    default:
      break;
  }
  for (const TypePair &t : kComponentTypes) {
    if (t.graph == type_) {
      return PropValue::ofComponents(t.prop, d_);
    }
  }
  return PropValue();
}

/* ---- reading ---- */

bool GraphValue::asBool() const
{
  CDCC_CHECK(type_ == Type::Bool, "GraphValue::asBool on another type");
  return b_;
}

int64_t GraphValue::asInt() const
{
  CDCC_CHECK(type_ == Type::Int, "GraphValue::asInt on another type");
  return i_;
}

double GraphValue::asFloat() const
{
  CDCC_CHECK(type_ == Type::Float, "GraphValue::asFloat on another type");
  return d_[0];
}

const string &GraphValue::asString() const
{
  CDCC_CHECK(type_ == Type::String, "GraphValue::asString on another type");
  return *s_;
}

int GraphValue::componentCount() const
{
  return graphComponentCount(type_);
}

const double *GraphValue::components() const
{
  CDCC_CHECK(componentCount() > 0, "GraphValue::components on a type without any");
  return d_;
}

const PayloadRef &GraphValue::asPayload() const
{
  CDCC_CHECK(type_ == Type::Payload, "GraphValue::asPayload on another type");
  return p_;
}

bool GraphValue::operator==(const GraphValue &b) const
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
      return i_ == b.i_;
    case Type::String:
      return strcmp(s_->c_str(), b.s_->c_str()) == 0;
    case Type::Payload:
      return p_ == b.p_;
    default:
      return memcmp(d_, b.d_, sizeof(double) * size_t(graphComponentCount(type_))) == 0;
  }
}

} // namespace coredcc
