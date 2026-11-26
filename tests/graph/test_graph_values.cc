#include "coredcc/graph/socket_type.h"

#include "litestl/tests/test_util.h"

#include <cmath>
#include <limits>

using namespace coredcc;
using Type = GraphValue::Type;

test_init;

namespace {

int g_meshes = 0;

struct Mesh {
  int verts = 0;
  Mesh(int n) : verts(n)
  {
    g_meshes++;
  }
  Mesh(const Mesh &b) : verts(b.verts)
  {
    g_meshes++;
  }
  ~Mesh()
  {
    g_meshes--;
  }
};
struct Points {
  int count = 0;
};
/* A second C++ type under Mesh's name. */
struct Impostor {
  int x = 0;
};

} // namespace

template <> struct coredcc::PayloadTraits<Mesh> {
  static constexpr const char *name = "test.Mesh";
};
template <> struct coredcc::PayloadTraits<Points> {
  static constexpr const char *name = "test.Points";
  static bool equal(const Points &a, const Points &b)
  {
    return a.count == b.count;
  }
  static size_t bytes(const Points &p)
  {
    return size_t(p.count) * 24;
  }
};
template <> struct coredcc::PayloadTraits<Impostor> {
  static constexpr const char *name = "test.Mesh";
};

namespace {

void testPayloads()
{
  {
    PayloadRef a = makePayload(Mesh(3));
    test_assert(a && a.get<Mesh>()->verts == 3 && g_meshes == 1);
    test_assert(a.type() == TypeId::of("test.Mesh"));
    test_assert(a.get<Points>() == nullptr && a.mutate<Points>() == nullptr);
    test_assert(a.bytes() == sizeof(Mesh));

    /* The only reference: in place. */
    const Mesh *at = a.get<Mesh>();
    test_assert(a.mutate<Mesh>() == at);
    a.mutate<Mesh>()->verts = 4;
    test_assert(a.get<Mesh>()->verts == 4 && g_meshes == 1);

    /* Shared: a copy, and the other reference keeps its value. */
    PayloadRef b = a;
    test_assert(b.same(a) && b == a && g_meshes == 1);
    b.mutate<Mesh>()->verts = 5;
    test_assert(g_meshes == 2 && !b.same(a) && a.get<Mesh>()->verts == 4);
    test_assert(b.get<Mesh>()->verts == 5 && a.get<Mesh>() == at);

    /* Without `equal`, the same content isn't equal. */
    test_assert(!(makePayload(Mesh(4)) == a));
    test_assert(g_meshes == 2);

    PayloadRef c = std::move(a);
    test_assert(!a && c.get<Mesh>() == at && g_meshes == 2);
    PayloadRef &self = c;
    c = self;
    test_assert(c.get<Mesh>() == at);
    c = PayloadRef();
    test_assert(g_meshes == 1 && !c && c.type().isNull() && c.get<Mesh>() == nullptr);
    test_assert(c == PayloadRef() && !(c == b) && !(b == c));
  }
  test_assert(g_meshes == 0);

  PayloadRef p = makePayload(Points{2});
  test_assert(p == makePayload(Points{2}) && !(p == makePayload(Points{3})));
  test_assert(!p.same(makePayload(Points{2})) && p.bytes() == 48);
  test_assert(!(p == makePayload(Mesh(2))) && !(makePayload(Mesh(2)) == p));
}

void testValues()
{
  test_assert(sizeof(GraphValue) == 40);
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const double m[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
  const GraphValue values[] = {
      GraphValue(),
      GraphValue::ofBool(true),
      GraphValue::ofInt(int64_t(1) << 62),
      GraphValue::ofFloat(0.5),
      GraphValue::ofFloat(nan),
      GraphValue::ofString("some text that is longer than any inline buffer"),
      GraphValue::ofVec2(1, 2),
      GraphValue::ofVec3(1, 2, 3),
      GraphValue::ofVec4(1, 2, 3, 4),
      GraphValue::ofQuat(1, 2, 3, 4),
      GraphValue::ofComponents(Type::Mat4, m),
      GraphValue::ofPayload(makePayload(Mesh(7))),
      GraphValue::ofPayload(PayloadRef()),
  };
  const int n = int(std::size(values));
  for (int i = 0; i < n; i++) {
    for (int j = 0; j < n; j++) {
      test_assert((values[i] == values[j]) == (i == j));
      /* Assignment over every other type, by copy and by move. */
      GraphValue v = values[j];
      v = values[i];
      test_assert(v == values[i]);
      GraphValue w = values[j];
      w = std::move(v);
      test_assert(w == values[i]);
      GraphValue x(std::move(w));
      test_assert(x == values[i]);
      GraphValue &self = x;
      x = self;
      test_assert(x == values[i]);
      x = std::move(self);
      test_assert(x == values[i]);
    }
  }
  test_assert(g_meshes == 1);

  /* Bitwise: -0 differs from 0, each component counts. */
  test_assert(GraphValue::ofFloat(0.0) != GraphValue::ofFloat(-0.0));
  test_assert(GraphValue::ofVec3(1, 2, 3) != GraphValue::ofVec3(1, 2, 4));
  test_assert(GraphValue::ofVec4(1, 2, 3, 4) != GraphValue::ofQuat(1, 2, 3, 4));
  double m2[16];
  for (int i = 0; i < 16; i++) {
    m2[i] = m[i];
  }
  m2[15] = 0;
  test_assert(GraphValue::ofComponents(Type::Mat4, m2) != values[10]);
  test_assert(GraphValue::ofString("a") != GraphValue::ofString("b"));
  test_assert(GraphValue::ofBool(true) != GraphValue::ofBool(false));
  test_assert(GraphValue::ofInt(1) != GraphValue::ofInt(2));

  test_assert(values[1].asBool() && values[2].asInt() == int64_t(1) << 62);
  test_assert(values[3].asFloat() == 0.5 && values[10].components()[15] == 16);
  test_assert(values[11].payload<Mesh>()->verts == 7 && !values[11].payload<Points>());
  test_assert(!values[12].payload<Mesh>() && !values[3].payload<Mesh>());
  test_assert(values[12].type() == Type::Payload && !values[12].asPayload());

  /* PropValue and back, for every type both have. */
  for (int i = 0; i < n; i++) {
    if (values[i].type() == Type::Payload) {
      test_assert(values[i].toProp().isNone());
      continue;
    }
    Result<GraphValue> back = GraphValue::fromProp(values[i].toProp());
    test_assert(back.ok() && *back == values[i]);
  }
  test_assert(values[9].toProp().type() == PropValue::Type::Quat);
  test_assert(GraphValue::fromProp(PropValue::ofEnum(1)).status().code() ==
              StatusCode::Unsupported);
  test_assert(!GraphValue::fromProp(PropValue::ofFlags(1)).ok());
  test_assert(!GraphValue::fromProp(PropValue::nullPointer()).ok());

  test_assert(graphTypeFor(PropKind::Quat) == Type::Quat);
  test_assert(graphTypeFor(PropKind::Enum) == Type::None);
  test_assert(graphTypeFor(PropKind::Collection) == Type::None);
}

GraphValue
convert(const SocketTypeRegistry &reg, const char *from, const char *to, GraphValue v)
{
  GraphValue out;
  const Status s = reg.convert(*reg.find(from), *reg.find(to), v, out);
  return s.ok() ? out : GraphValue();
}

Status mat4ToVec3(const GraphValue &in, GraphValue &out)
{
  const double *c = in.components();
  out = GraphValue::ofVec3(c[12], c[13], c[14]);
  return Status();
}

Status liar(const GraphValue &, GraphValue &out)
{
  out = GraphValue::ofInt(1);
  return Status();
}

Status refuses(const GraphValue &, GraphValue &)
{
  return Status::error(StatusCode::OutOfRange, "no");
}

void testSocketTypes()
{
  Runtime rt;
  SocketTypeRegistry &reg = rt.ext<SocketTypeRegistry>();
  test_assert(rt.validateAll().ok());
  for (const char *name :
       {"bool", "int", "float", "vec2", "vec3", "vec4", "mat4", "quat", "string"})
  {
    const SocketType *t = reg.find(name);
    test_assert(t && t->holds(t->defaultValue) && t->hasLiteral());
    test_assert(reg.builtin(t->valueType) == t);
    const PropertyDef def = t->literalDef("v");
    test_assert(def.validate().ok() && graphTypeFor(def.kind()) == t->valueType);
    test_assert(*GraphValue::fromProp(def.defaultValue()) == t->defaultValue);
  }
  test_assert(!reg.find("enum") && !reg.builtin(Type::Payload) &&
              !reg.builtin(Type::None));
  test_assert(!reg.find("float")->holds(GraphValue::ofInt(1)));

  /* Payload types, and the two C++ types under one name. */
  const uint64_t before = rt.lastEdit();
  Result<const SocketType *> mesh = reg.addPayloadType<Mesh>();
  test_assert(mesh.ok() && rt.lastEdit() > before && !(*mesh)->hasLiteral());
  const uint64_t after = rt.lastEdit();
  test_assert(*reg.addPayloadType<Mesh>() == *mesh && rt.lastEdit() == after);
  test_assert(reg.addPayloadType<Impostor>().status().code() ==
              StatusCode::AlreadyExists);
  test_assert(reg.addPayloadType<Points>().ok());
  test_assert((*mesh)->holds(GraphValue::ofPayload(makePayload(Mesh(1)))));
  test_assert((*mesh)->holds(GraphValue::ofPayload(PayloadRef())));
  test_assert(!(*mesh)->holds(GraphValue::ofPayload(makePayload(Points{1}))));
  test_assert(!(*mesh)->holds(GraphValue()) && !(*mesh)->holds(GraphValue::ofFloat(1)));
  test_assert((*mesh)->defaultValue == GraphValue::ofPayload(PayloadRef()));

  /* Conversions. */
  const double inf = std::numeric_limits<double>::infinity();
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const struct {
    double in;
    int64_t out;
  } ints[] = {{2.9, 2},
              {-2.9, -2},
              {0.0, 0},
              {nan, 0},
              {inf, INT64_MAX},
              {-inf, INT64_MIN},
              {9223372036854775808.0, INT64_MAX},
              {9223372036854774784.0, 9223372036854774784},
              {-9223372036854775808.0, INT64_MIN},
              {-1e30, INT64_MIN},
              {1e30, INT64_MAX}};
  for (const auto &c : ints) {
    test_assert(convert(reg, "float", "int", GraphValue::ofFloat(c.in)) ==
                GraphValue::ofInt(c.out));
  }
  test_assert(convert(reg, "float", "bool", GraphValue::ofFloat(0.25)) ==
              GraphValue::ofBool(true));
  test_assert(convert(reg, "float", "bool", GraphValue::ofFloat(0)) ==
              GraphValue::ofBool(false));
  test_assert(convert(reg, "int", "bool", GraphValue::ofInt(-1)) ==
              GraphValue::ofBool(true));
  test_assert(convert(reg, "int", "float", GraphValue::ofInt(-3)) ==
              GraphValue::ofFloat(-3));
  test_assert(convert(reg, "bool", "int", GraphValue::ofBool(true)) ==
              GraphValue::ofInt(1));
  test_assert(convert(reg, "bool", "float", GraphValue::ofBool(true)) ==
              GraphValue::ofFloat(1));
  test_assert(convert(reg, "float", "vec2", GraphValue::ofFloat(2)) ==
              GraphValue::ofVec2(2, 2));
  test_assert(convert(reg, "float", "vec3", GraphValue::ofFloat(2)) ==
              GraphValue::ofVec3(2, 2, 2));
  test_assert(convert(reg, "float", "vec4", GraphValue::ofFloat(2)) ==
              GraphValue::ofVec4(2, 2, 2, 2));
  test_assert(convert(reg, "quat", "quat", GraphValue::ofQuat(0, 0, 0, 1)) ==
              GraphValue::ofQuat(0, 0, 0, 1));

  GraphValue out = GraphValue::ofInt(9);
  const SocketType &mat4 = *reg.find("mat4");
  const SocketType &vec3 = *reg.find("vec3");
  const SocketType &text = *reg.find("string");
  test_assert(!reg.canConvert(mat4, vec3) && !reg.canConvert(vec3, *reg.find("float")));
  test_assert(reg.canConvert(mat4, mat4) &&
              reg.canConvert(*reg.find("bool"), *reg.find("int")));
  test_assert(reg.convert(mat4, vec3, GraphValue::identityMat4(), out).code() ==
              StatusCode::TypeMismatch);
  test_assert(out == GraphValue::ofInt(9));

  const uint64_t edit = rt.lastEdit();
  test_assert(reg.addConversion("mat4", "vec3", mat4ToVec3).ok() && rt.lastEdit() > edit);
  test_assert(reg.canConvert(mat4, vec3) && !reg.canConvert(vec3, mat4));
  test_assert(reg.convert(mat4, vec3, GraphValue::identityMat4(), out).ok());
  test_assert(out == GraphValue::ofVec3(0, 0, 0));
  test_assert(reg.addConversion("mat4", "vec3", refuses).code() ==
              StatusCode::AlreadyExists);
  test_assert(reg.convert(mat4, vec3, GraphValue::identityMat4(), out).ok());
  test_assert(reg.addConversion("float", "float", liar).code() ==
              StatusCode::InvalidArgument);
  test_assert(reg.addConversion("float", "quat", nullptr).code() ==
              StatusCode::InvalidArgument);
  const Status missing = reg.addConversion("flaot", "int", liar);
  test_assert(missing.code() == StatusCode::NotFound && missing.suggestions().size() > 0);
  test_assert(reg.addConversion("int", "nothing", liar).code() == StatusCode::NotFound);

  /* A conversion that fails, or gives another type, leaves `out` alone. */
  test_assert(reg.addConversion("string", "quat", liar).ok());
  test_assert(reg.addConversion("string", "vec3", refuses).ok());
  out = GraphValue::ofInt(9);
  test_assert(
      reg.convert(text, *reg.find("quat"), GraphValue::ofString(""), out).code() ==
      StatusCode::Internal);
  test_assert(reg.convert(text, vec3, GraphValue::ofString(""), out).code() ==
              StatusCode::OutOfRange);
  test_assert(out == GraphValue::ofInt(9));

  {
    EvaluationScope scope(rt);
    test_assert(reg.addConversion("int", "vec2", liar).code() == StatusCode::Busy);
    test_assert(reg.addPayloadType<Mesh>().status().code() == StatusCode::Busy);
  }
  test_assert(!reg.canConvert(*reg.find("int"), *reg.find("vec2")));
  test_assert(rt.validateAll().ok());
}

} // namespace

int main()
{
  testPayloads();
  testValues();
  testSocketTypes();
  test_assert(g_meshes == 0);
  return test_end();
}
