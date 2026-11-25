#include "coredcc/graph/socket_type.h"

#include "coredcc/base/assert.h"
#include "coredcc/base/text.h"

#include <cmath>
#include <string>

namespace coredcc {

using Type = GraphValue::Type;

bool SocketType::holds(const GraphValue &v) const
{
  if (v.type() != valueType) {
    return false;
  }
  return valueType != Type::Payload || !v.asPayload() || v.asPayload().type() == payload;
}

PropertyDef SocketType::literalDef(std::string_view apiName) const
{
  CDCC_CHECK(hasLiteral(), "literalDef: a payload socket has no literal");
  switch (valueType) {
  case Type::Bool:
    return PropertyDef::boolProp(apiName);
  case Type::Int:
    return PropertyDef::intProp(apiName);
  case Type::String:
    return PropertyDef::stringProp(apiName);
  case Type::Vec2:
    return PropertyDef::vecProp(apiName, 2);
  case Type::Vec3:
    return PropertyDef::vecProp(apiName, 3);
  case Type::Vec4:
    return PropertyDef::vecProp(apiName, 4);
  case Type::Mat4:
    return PropertyDef::mat4Prop(apiName);
  case Type::Quat:
    return PropertyDef::quatProp(apiName);
  default:
    return PropertyDef::floatProp(apiName);
  }
}

/* ---- built-in conversions ---- */

namespace {

double asNumber(const GraphValue &v)
{
  if (v.type() == Type::Bool) {
    return v.asBool() ? 1.0 : 0.0;
  }
  return v.type() == Type::Int ? double(v.asInt()) : v.asFloat();
}

Status toBool(const GraphValue &in, GraphValue &out)
{
  out = GraphValue::ofBool(asNumber(in) != 0.0);
  return Status();
}

Status toInt(const GraphValue &in, GraphValue &out)
{
  const double x = asNumber(in);
  constexpr double k63 = 9223372036854775808.0;
  int64_t i = 0;
  if (x >= k63) {
    i = INT64_MAX;
  } else if (x < -k63) {
    i = INT64_MIN;
  } else if (!std::isnan(x)) {
    i = int64_t(x);
  }
  out = GraphValue::ofInt(i);
  return Status();
}

Status toFloat(const GraphValue &in, GraphValue &out)
{
  out = GraphValue::ofFloat(asNumber(in));
  return Status();
}

template <Type T> Status splat(const GraphValue &in, GraphValue &out)
{
  const double x = in.asFloat();
  const double c[4] = {x, x, x, x};
  out = GraphValue::ofComponents(T, c);
  return Status();
}

} // namespace

/* ---- registry ---- */

SocketTypeRegistry::SocketTypeRegistry(Runtime *rt) : rt_(*rt)
{
  struct Builtin {
    const char *name;
    GraphValue value;
  };
  const Builtin builtins[] = {
      {"bool", GraphValue::ofBool(false)},
      {"int", GraphValue::ofInt(0)},
      {"float", GraphValue::ofFloat(0)},
      {"vec2", GraphValue::ofVec2(0, 0)},
      {"vec3", GraphValue::ofVec3(0, 0, 0)},
      {"vec4", GraphValue::ofVec4(0, 0, 0, 0)},
      {"mat4", GraphValue::identityMat4()},
      {"quat", GraphValue::ofQuat(0, 0, 0, 1)},
      {"string", GraphValue::ofString("")},
  };
  for (const Builtin &b : builtins) {
    SocketType t;
    t.name = b.name;
    t.valueType = b.value.type();
    t.defaultValue = b.value;
    const bool ok = addEntry(std::move(t)).ok();
    CDCC_CHECK(ok, "registering a built-in socket type");
  }
  struct Conversion {
    const char *from, *to;
    SocketConvertFn fn;
  };
  const Conversion conversions[] = {
      {"bool", "int", toInt},
      {"bool", "float", toFloat},
      {"int", "bool", toBool},
      {"int", "float", toFloat},
      {"float", "bool", toBool},
      {"float", "int", toInt},
      {"float", "vec2", splat<Type::Vec2>},
      {"float", "vec3", splat<Type::Vec3>},
      {"float", "vec4", splat<Type::Vec4>},
  };
  for (const Conversion &c : conversions) {
    const PairKey key{entry(c.from)->type.id.value, entry(c.to)->type.id.value};
    conversions_.add(key, c.fn);
  }
}

SocketTypeRegistry::~SocketTypeRegistry()
{
  for (Entry *e : types_) {
    litestl::alloc::Delete(e);
  }
}

static std::string idName(std::string_view name)
{
  return "socket:" + std::string(name);
}

Result<SocketTypeRegistry::Entry *> SocketTypeRegistry::addEntry(SocketType type)
{
  Result<TypeId> id = rt_.registerId<IdDomain::Type>(idName(type.name.c_str()));
  if (!id.ok()) {
    return id.status();
  }
  type.id = *id;
  Entry *e = litestl::alloc::New<Entry>("coredcc::SocketType");
  e->type = std::move(type);
  types_.append(e);
  byId_.add(e->type.id.value, e);
  stamp_ = rt_.touch();
  return e;
}

SocketTypeRegistry::Entry *SocketTypeRegistry::entry(std::string_view name) const
{
  Entry *const *e = byId_.lookup_ptr(TypeId::of(idName(name)).value);
  return e && name == (*e)->type.name.c_str() ? *e
                                              : nullptr; /* a file's can have its id */
}

Result<const SocketType *> SocketTypeRegistry::addPayload(const char *name,
                                                          const void *typeKey)
{
  CDCC_TRY(rt_.busyIfEvaluating("addPayloadType"));
  if (const Entry *found = entry(name)) {
    if (found->typeKey != typeKey) {
      return Status::error(
          StatusCode::AlreadyExists,
          (std::string("socket type '") + name + "' is registered for another type")
              .c_str());
    }
    return &found->type;
  }
  SocketType t;
  t.name = name;
  t.valueType = Type::Payload;
  t.payload = TypeId::of(name);
  t.defaultValue = GraphValue::ofPayload(PayloadRef());
  Result<Entry *> e = addEntry(std::move(t));
  if (!e.ok()) {
    return e.status();
  }
  (*e)->typeKey = typeKey;
  return static_cast<const SocketType *>(&(*e)->type);
}

Status SocketTypeRegistry::addConversion(std::string_view from,
                                         std::string_view to,
                                         SocketConvertFn fn)
{
  CDCC_TRY(rt_.busyIfEvaluating("addConversion"));
  const Entry *a = entry(from);
  const Entry *b = entry(to);
  if (!a || !b) {
    return unknown(a ? to : from);
  }
  if (a == b || !fn) {
    return Status::error(StatusCode::InvalidArgument,
                         "addConversion: it needs two types and a function");
  }
  if (!conversions_.add(PairKey{a->type.id.value, b->type.id.value}, fn)) {
    return Status::error(StatusCode::AlreadyExists,
                         ("there is a conversion from " + std::string(from) + " to " +
                          std::string(to) + " already")
                             .c_str());
  }
  stamp_ = rt_.touch();
  return Status();
}

const SocketType *SocketTypeRegistry::find(std::string_view name) const
{
  const Entry *e = entry(name);
  return e ? &e->type : nullptr;
}

Status SocketTypeRegistry::unknown(std::string_view name) const
{
  Status s = Status::error(StatusCode::NotFound,
                           ("no socket type '" + std::string(name) + "'").c_str());
  Suggester sg(name);
  for (const Entry *e : types_) {
    sg.consider(e->type.name.c_str());
  }
  sg.addTo(s);
  return s;
}

const SocketType *SocketTypeRegistry::builtin(Type type) const
{
  for (const Entry *e : types_) {
    if (e->type.valueType == type && type != Type::Payload) {
      return &e->type;
    }
  }
  return nullptr;
}

const SocketType *SocketTypeRegistry::typeOf(const GraphValue &v) const
{
  if (v.type() != Type::Payload) {
    return builtin(v.type());
  }
  for (const Entry *e : types_) {
    if (v.asPayload() && e->type.payload == v.asPayload().type()) {
      return &e->type;
    }
  }
  return nullptr;
}

bool SocketTypeRegistry::canConvert(const SocketType &from, const SocketType &to) const
{
  return &from == &to || conversions_.lookup_ptr(PairKey{from.id.value, to.id.value});
}

Status SocketTypeRegistry::convert(const SocketType &from,
                                   const SocketType &to,
                                   const GraphValue &in,
                                   GraphValue &out) const
{
  if (&from == &to) {
    out = in;
    return Status();
  }
  const SocketConvertFn *fn =
      conversions_.lookup_ptr(PairKey{from.id.value, to.id.value});
  if (!fn) {
    return Status::error(StatusCode::TypeMismatch,
                         (std::string("there is no conversion from ") +
                          from.name.c_str() + " to " + to.name.c_str())
                             .c_str());
  }
  GraphValue v;
  CDCC_TRY((*fn)(in, v));
  if (!to.holds(v)) {
    return Status::error(StatusCode::Internal,
                         (std::string("the conversion from ") + from.name.c_str() +
                          " to " + to.name.c_str() + " gave a " +
                          graphValueTypeName(v.type()))
                             .c_str());
  }
  out = std::move(v);
  return Status();
}

Status SocketTypeRegistry::validate() const
{
  for (const Entry *e : types_) {
    if (entry(e->type.name.c_str()) != e || !e->type.holds(e->type.defaultValue)) {
      return Status::error(StatusCode::Internal,
                           (std::string("socket type '") + e->type.name.c_str() +
                            "' isn't registered consistently")
                               .c_str());
    }
  }
  return Status();
}

} // namespace coredcc
