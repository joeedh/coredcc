#include "coredcc/props/bag.h"
#include "coredcc/props/def.h"

#include "test_util.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

test_init;

using namespace coredcc;

static bool hasMsg(const Status &s, const char *needle)
{
  if (strstr(s.message(), needle)) {
    return true;
  }
  fprintf(stderr, "message was: %s\n", s.message());
  return false;
}

static PropertyDef modeDef()
{
  return PropertyDef::enumProp("mode")
      .item("SMOOTH", 0)
      .item("SHARP", 1, "Sharp Edges")
      .item("ROOT", 5);
}

static PropertyDef flagsDef()
{
  return PropertyDef::flagsProp("axes").item("X", 1).item("Y", 2).item("Z", 4);
}

static void test_ui_names()
{
  test_assert(uiNameFromApiName("edgeCrease") == "Edge Crease");
  test_assert(uiNameFromApiName("edge_crease") == "Edge Crease");
  test_assert(uiNameFromApiName("HTTPServer") == "HTTP Server");
  test_assert(uiNameFromApiName("uvMap") == "Uv Map");
  test_assert(uiNameFromApiName("layer2Name") == "Layer2 Name");
  test_assert(uiNameFromApiName("_private") == "Private");
  test_assert(uiNameFromApiName("x") == "X");
  test_assert(
      strcmp(PropertyDef::floatProp("edgeCrease").uiName().c_str(), "Edge Crease") == 0);
  test_assert(strcmp(modeDef().enumMeta()->items[1].uiName.c_str(), "Sharp Edges") == 0);
  test_assert(strcmp(modeDef().enumMeta()->items[2].uiName.c_str(), "ROOT") == 0);
}

static void test_defaults_and_meta()
{
  test_assert(PropertyDef::intProp("n").numeric()->precision == 0);
  test_assert(PropertyDef::floatProp("f").numeric() != nullptr);
  test_assert(PropertyDef::boolProp("b").numeric() == nullptr);
  test_assert(PropertyDef::stringProp("s").stringMeta() != nullptr);
  test_assert(PropertyDef::quatProp("q").defaultValue() == PropValue::ofQuat(0, 0, 0, 1));
  test_assert(PropertyDef::vecProp("v", 3).kind() == PropKind::Vec3);
  test_assert(PropertyDef::pointerProp("p", StructId::of("Mesh")).target()->target ==
              StructId::of("Mesh"));
  /* An Enum defaults to its first item. */
  test_assert(modeDef().defaultValue() == PropValue::ofEnum(0));
  test_assert(PropertyDef::floatProp("f")
                  .access(PropAccess::ReadOnly)
                  .hasAccess(PropAccess::ReadOnly));
}

/* Which unit a subtype can have. No unit always can be. */
static void test_units_agree()
{
  const UnitFamily none = UnitFamily::None, any = UnitFamily(99), bad = UnitFamily(98);
  struct Row {
    PropSubtype subtype;
    UnitFamily unit;
  };
  const Row rows[] = {{PropSubtype::None, any},
                      {PropSubtype::Color, bad},
                      {PropSubtype::ColorGamma, bad},
                      {PropSubtype::Angle, UnitFamily::Angle},
                      {PropSubtype::Distance, UnitFamily::Length},
                      {PropSubtype::Factor, UnitFamily::Factor},
                      {PropSubtype::Percentage, UnitFamily::Factor},
                      {PropSubtype::Time, UnitFamily::Time},
                      {PropSubtype::Direction, bad},
                      {PropSubtype::Euler, UnitFamily::Angle},
                      {PropSubtype::Xyz, any},
                      {PropSubtype::FilePath, bad},
                      {PropSubtype::Password, bad}};
  for (const Row &row : rows) {
    for (UnitFamily u : {none,
                         UnitFamily::Length,
                         UnitFamily::Angle,
                         UnitFamily::Time,
                         UnitFamily::Mass,
                         UnitFamily::Factor})
    {
      const PropertyDef def = PropertyDef::vecProp("v", 3).subtype(row.subtype).unit(u);
      test_assert(def.validate().ok() == (u == none || row.unit == any || row.unit == u));
    }
  }
  test_assert(hasMsg(PropertyDef::floatProp("a")
                         .subtype(PropSubtype::Angle)
                         .unit(UnitFamily::Mass)
                         .validate(),
                     "the subtype Angle and the unit Mass"));
}

static void test_validate()
{
  test_assert(modeDef().validate().ok());
  test_assert(flagsDef().validate().ok());
  test_assert(PropertyDef::floatProp("r").range(0, 10).softRange(0, 5).validate().ok());
  test_assert(hasMsg(PropertyDef::floatProp("bad name").validate(), "identifier"));
  test_assert(hasMsg(PropertyDef::floatProp("r").range(5, 1).validate(), "hard range"));
  test_assert(hasMsg(PropertyDef::floatProp("r").range(0, 1).softRange(0, 2).validate(),
                     "inside the hard range"));
  test_assert(
      hasMsg(PropertyDef::floatProp("r").unit(UnitFamily::Length, "kg").validate(),
             "'kg' is no unit of Length"));
  test_assert(PropertyDef::floatProp("r").unit(UnitFamily::Length, "mm").validate().ok());
  test_assert(hasMsg(PropertyDef::floatProp("r").precision(16).validate(), "precision"));
  test_assert(hasMsg(PropertyDef::enumProp("e").validate(), "at least one item"));
  test_assert(hasMsg(PropertyDef::enumProp("e").item("A", 0).item("A", 1).validate(),
                     "used twice"));
  test_assert(hasMsg(PropertyDef::enumProp("e").item("A", 0).item("B", 0).validate(),
                     "share a value"));
  test_assert(hasMsg(PropertyDef::enumProp("e").item("a-b", 0).validate(), "identifier"));
  test_assert(hasMsg(PropertyDef::flagsProp("f").item("A", 3).validate(), "single bit"));
  /* A default the clamp would change is out of range. */
  test_assert(hasMsg(PropertyDef::floatProp("r")
                         .range(0, 1)
                         .defaultValue(PropValue::ofFloat(2))
                         .validate(),
                     "out of range"));
  test_assert(
      hasMsg(PropertyDef::intProp("n").defaultValue(PropValue::ofString("x")).validate(),
             "is a String, not a Int"));
}

static void test_display()
{
  const auto shows = [](UnitFamily f, const char *unit, const char *want) {
    const PropertyDef d = PropertyDef::floatProp("r").unit(f, unit);
    return strcmp(d.numeric()->displaySymbol(), want) == 0;
  };
  test_assert(shows(UnitFamily::Length, "", "m") &&
              shows(UnitFamily::Length, "millimeters", "mm"));
  test_assert(shows(UnitFamily::None, "", "") && shows(UnitFamily::Length, "kg", "m"));
  for (const char *name : {"None", "Length", "Angle", "Time", "Mass", "Factor"}) {
    test_assert(unitFamilyOfName(name) &&
                strcmp(unitFamilyName(*unitFamilyOfName(name)), name) == 0);
  }
  test_assert(!unitFamilyOfName("length") && !unitFamilyOfName(""));

  const auto name = [](PropKind k, PropSubtype s, int i, const char *want) {
    const char *got = componentName(k, s, i);
    return want ? got && strcmp(got, want) == 0 : got == nullptr;
  };
  test_assert(name(PropKind::Vec2, PropSubtype::None, 0, "X") &&
              name(PropKind::Vec2, PropSubtype::None, 2, nullptr));
  test_assert(name(PropKind::Vec3, PropSubtype::Euler, 2, "Z") &&
              name(PropKind::Quat, PropSubtype::None, 3, "W"));
  test_assert(name(PropKind::Vec4, PropSubtype::Color, 3, "A") &&
              name(PropKind::Vec3, PropSubtype::ColorGamma, 0, "R"));
  test_assert(name(PropKind::Mat4, PropSubtype::None, 0, nullptr) &&
              name(PropKind::Float, PropSubtype::None, 0, nullptr));
  test_assert(name(PropKind::Vec3, PropSubtype::None, -1, nullptr));
}

static void test_coerce_numbers()
{
  const PropertyDef i = PropertyDef::intProp("n").range(-10, 10);
  test_assert(*coerce(i, PropValue::ofInt(3)) == PropValue::ofInt(3));
  test_assert(*coerce(i, PropValue::ofInt(99)) == PropValue::ofInt(10));
  test_assert(*coerce(i, PropValue::ofFloat(-4.0)) == PropValue::ofInt(-4));
  test_assert(!coerce(i, PropValue::ofFloat(1.5)).ok());
  test_assert(!coerce(i, PropValue::ofString("1")).ok());
  test_assert(coerce(i, PropValue::ofBool(true)).status().code() ==
              StatusCode::TypeMismatch);

  /* Float -> Int range: 2^63 itself overflows, the largest double below it doesn't. */
  const PropertyDef wide = PropertyDef::intProp("w");
  test_assert(!coerce(wide, PropValue::ofFloat(1e19)).ok());
  test_assert(!coerce(wide, PropValue::ofFloat(9223372036854775808.0)).ok());
  test_assert(*coerce(wide, PropValue::ofFloat(-9223372036854775808.0)) ==
              PropValue::ofInt(std::numeric_limits<int64_t>::min()));
  test_assert(!coerce(wide, PropValue::ofFloat(std::nan(""))).ok());
  test_assert(!coerce(wide, PropValue::ofFloat(INFINITY)).ok());

  /* A fractional hard range clamps to the integers inside it. */
  const PropertyDef frac = PropertyDef::intProp("f").range(0.5, 9.5);
  test_assert(*coerce(frac, PropValue::ofInt(0)) == PropValue::ofInt(1));
  test_assert(*coerce(frac, PropValue::ofInt(10)) == PropValue::ofInt(9));

  const PropertyDef f = PropertyDef::floatProp("r").range(0, 1);
  test_assert(*coerce(f, PropValue::ofInt(1)) == PropValue::ofFloat(1.0));
  test_assert(*coerce(f, PropValue::ofFloat(-3)) == PropValue::ofFloat(0.0));
  test_assert(!coerce(f, PropValue::ofVec2(0, 0)).ok());

  const PropertyDef b = PropertyDef::boolProp("b");
  test_assert(*coerce(b, PropValue::ofInt(1)) == PropValue::ofBool(true));
  test_assert(!coerce(b, PropValue::ofInt(2)).ok());
}

static void test_coerce_vectors()
{
  const PropertyDef v = PropertyDef::vecProp("loc", 3).range(-1, 1);
  test_assert(*coerce(v, PropValue::ofVec3(0.5, 2, -3)) == PropValue::ofVec3(0.5, 1, -1));
  test_assert(*coerce(v, PropValue::ofFloat(0.25)) ==
              PropValue::ofVec3(0.25, 0.25, 0.25));
  test_assert(*coerce(v, PropValue::ofInt(7)) == PropValue::ofVec3(1, 1, 1));
  test_assert(!coerce(v, PropValue::ofVec2(0, 0)).ok());

  const PropertyDef q = PropertyDef::quatProp("rot");
  test_assert(*coerce(q, PropValue::ofVec4(0, 0, 1, 0)) == PropValue::ofQuat(0, 0, 1, 0));
  test_assert(!coerce(q, PropValue::ofFloat(1)).ok()); /* no broadcast to a Quat */
  const PropertyDef m = PropertyDef::mat4Prop("xf");
  test_assert(*coerce(m, PropValue::identityMat4()) == PropValue::identityMat4());
  test_assert(!coerce(m, PropValue::ofFloat(1)).ok());
}

static void test_coerce_enums()
{
  const PropertyDef e = modeDef();
  test_assert(*coerce(e, PropValue::ofString("SHARP")) == PropValue::ofEnum(1));
  test_assert(*coerce(e, PropValue::ofInt(5)) == PropValue::ofEnum(5));
  test_assert(*coerce(e, PropValue::ofEnum(0)) == PropValue::ofEnum(0));
  test_assert(coerce(e, PropValue::ofInt(2)).status().code() == StatusCode::OutOfRange);
  Result<PropValue> miss = coerce(e, PropValue::ofString("SHRAP"));
  test_assert(miss.status().code() == StatusCode::NotFound);
  test_assert(miss.status().suggestions().size() == 1);
  test_assert(strcmp(miss.status().suggestions()[0].c_str(), "SHARP") == 0);

  const PropertyDef f = flagsDef();
  test_assert(*coerce(f, PropValue::ofString("X|Z")) == PropValue::ofFlags(5));
  test_assert(*coerce(f, PropValue::ofString("")) == PropValue::ofFlags(0));
  test_assert(*coerce(f, PropValue::ofInt(3)) == PropValue::ofFlags(3));
  test_assert(!coerce(f, PropValue::ofInt(8)).ok());
  test_assert(!coerce(f, PropValue::ofInt(-1)).ok());
  test_assert(!coerce(f, PropValue::ofFlags(16)).ok());
  Result<PropValue> badKey = coerce(f, PropValue::ofString("X|W"));
  test_assert(hasMsg(badKey.status(), "no flag 'W'"));
  test_assert(!coerce(f, PropValue::ofString("X|")).ok()); /* an empty key */
}

static void test_coerce_other()
{
  const PropertyDef s = PropertyDef::stringProp("name").maxLength(3);
  test_assert(coerce(s, PropValue::ofString("abc")).ok());
  test_assert(coerce(s, PropValue::ofString("abcd")).status().code() ==
              StatusCode::OutOfRange);
  test_assert(!coerce(s, PropValue::ofInt(1)).ok());

  const PropertyDef p = PropertyDef::pointerProp("target", StructId::of("Object"));
  test_assert(*coerce(p, PropValue()) == PropValue::nullPointer());
  test_assert(!coerce(p, PropValue::ofString("objects[0]")).ok());

  const PropertyDef c = PropertyDef::collectionProp("items", StructId::of("Item"));
  test_assert(coerce(c, PropValue::ofInt(1)).status().code() == StatusCode::Unsupported);
  test_assert(c.validate().ok());
}

struct Limits {
  double max = 4.0;
};

static void test_dynamic()
{
  const PropertyDef d = PropertyDef::floatProp("r").range(0, 100).dynamic(
      [](PropertyDef &def, const void *o) {
        def.range(0, static_cast<const Limits *>(o)->max);
      });
  test_assert(d.isDynamic());
  Limits lim;
  const PropertyDef r = d.resolved(&lim);
  test_assert(r.numeric()->hardMax == 4.0 && !r.isDynamic());
  test_assert(*coerce(r, PropValue::ofFloat(50)) == PropValue::ofFloat(4.0));
  test_assert(d.numeric()->hardMax == 100.0); /* the original is unchanged */
  /* Without a hook, resolved() is a plain copy. */
  test_assert(PropertyDef::floatProp("x").resolved(nullptr).numeric() != nullptr);
}

static void test_bag()
{
  PropertyBag bag;
  test_assert(bag.add(PropertyDef::floatProp("dist").range(0, 10).defaultValue(
                          PropValue::ofFloat(1)))
                  .ok());
  test_assert(bag.add(modeDef()).ok());
  test_assert(bag.add(PropertyDef::pointerProp("target", StructId::of("Object"))
                          .access(PropAccess::Optional))
                  .ok());
  test_assert(bag.add(PropertyDef::floatProp("dist")).code() ==
              StatusCode::AlreadyExists);
  test_assert(!bag.add(PropertyDef::floatProp("bad name")).ok());
  test_assert(bag.size() == 3);

  test_assert(*bag.get("dist") == PropValue::ofFloat(1));
  test_assert(bag.get("target")->isNone()); /* Optional and unset */
  test_assert(!bag.isSet("dist"));

  test_assert(bag.set("dist", PropValue::ofInt(20)).ok());
  test_assert(*bag.get("dist") == PropValue::ofFloat(10)); /* coerced and clamped */
  test_assert(bag.isSet("dist") && bag.wasSet(0));
  test_assert(bag.set("mode", PropValue::ofString("ROOT")).ok());
  test_assert(*bag.get("mode") == PropValue::ofEnum(5));
  test_assert(!bag.set("mode", PropValue::ofString("NOPE")).ok());
  test_assert(*bag.get("mode") ==
              PropValue::ofEnum(5)); /* a failed set changes nothing */

  Status miss = bag.set("dsit", PropValue::ofFloat(1));
  test_assert(miss.code() == StatusCode::NotFound);
  test_assert(strcmp(miss.suggestions()[0].c_str(), "dist") == 0);

  test_assert(bag.reset("dist").ok());
  test_assert(!bag.isSet("dist") && *bag.get("dist") == PropValue::ofFloat(1));
  bag.resetAll();
  test_assert(*bag.get("mode") == PropValue::ofEnum(0));
  test_assert(bag.indexOf("target") == 2 && bag.indexOf("nope") == -1);
}

static void test_review_fixes()
{
  const double nan = std::nan("");
  /* NaN never gets past a numeric coerce. */
  test_assert(coerce(PropertyDef::floatProp("r").range(0, 1), PropValue::ofFloat(nan))
                  .status()
                  .code() == StatusCode::OutOfRange);
  test_assert(
      !coerce(PropertyDef::vecProp("v", 3).range(0, 1), PropValue::ofVec3(nan, .5, 2))
           .ok());
  test_assert(!coerce(PropertyDef::intProp("n"), PropValue::ofFloat(nan)).ok());
  test_assert(
      !PropertyDef::floatProp("r").defaultValue(PropValue::ofFloat(nan)).validate().ok());
  /* Infinity clamps to a finite bound. */
  test_assert(*coerce(PropertyDef::floatProp("r").range(0, 1),
                      PropValue::ofFloat(INFINITY)) == PropValue::ofFloat(1));

  /* An explicit default survives later item() calls. */
  PropertyDef e = PropertyDef::enumProp("m")
                      .defaultValue(PropValue::ofEnum(5))
                      .item("A", 0)
                      .item("B", 5);
  test_assert(e.defaultValue() == PropValue::ofEnum(5) && e.validate().ok());

  /* Wrong-type defaults say so. */
  test_assert(
      hasMsg(PropertyDef::floatProp("f").defaultValue(PropValue::ofInt(1)).validate(),
             "is a Int, not a Float"));

  /* Int clamping compares in int64, above 2^53 too. */
  const PropertyDef big = PropertyDef::intProp("n").range(0, 9007199254740992.0);
  test_assert(*coerce(big, PropValue::ofInt(9007199254740993)) ==
              PropValue::ofInt(9007199254740992));
  test_assert(hasMsg(PropertyDef::intProp("n").range(0.2, 0.8).validate(), "no integer"));
  test_assert(hasMsg(PropertyDef::floatProp("s").step(-1).validate(), "step"));
  test_assert(hasMsg(PropertyDef::stringProp("s").maxLength(-1).validate(), "maxLength"));

  /* JS numbers arrive as Float: integral ones work for Enum, Flags and Bool. */
  test_assert(*coerce(modeDef(), PropValue::ofFloat(5.0)) == PropValue::ofEnum(5));
  test_assert(!coerce(modeDef(), PropValue::ofFloat(0.5)).ok());
  test_assert(*coerce(flagsDef(), PropValue::ofFloat(3.0)) == PropValue::ofFlags(3));
  test_assert(*coerce(PropertyDef::boolProp("b"), PropValue::ofFloat(1.0)) ==
              PropValue::ofBool(true));

  /* Flag bit 63. */
  const PropertyDef top =
      PropertyDef::flagsProp("f").item("TOP", INT64_MIN).item("LOW", 1);
  test_assert(top.validate().ok());
  test_assert(*coerce(top, PropValue::ofString("TOP|LOW")) ==
              PropValue::ofFlags(0x8000000000000001ull));

  /* Pointer targets are checked when both types are known. */
  const PropertyDef p = PropertyDef::pointerProp("obj", StructId::of("Object"));
  StableRef mesh;
  mesh.owner = OwnerKey::ofRuntime(1);
  mesh.type = StructId::of("Mesh");
  test_assert(coerce(p, PropValue::ofPointer(mesh)).status().code() ==
              StatusCode::TypeMismatch);
  mesh.type = StructId::of("Object");
  test_assert(coerce(p, PropValue::ofPointer(mesh)).ok());
  mesh.type = StructId();
  test_assert(coerce(p, PropValue::ofPointer(mesh)).ok()); /* unknown type: accepted */

  /* A dynamic def must be resolved before coerce. */
  const PropertyDef dyn = PropertyDef::floatProp("d").dynamic(
      [](PropertyDef &def, const void *) { def.range(0, 1); });
  test_assert(coerce(dyn, PropValue::ofFloat(5)).status().code() ==
              StatusCode::FailedPrecondition);
  test_assert(*coerce(dyn.resolved(nullptr), PropValue::ofFloat(5)) ==
              PropValue::ofFloat(1));

  /* uiName("") re-derives. */
  test_assert(
      strcmp(PropertyDef::floatProp("edgeLoop").uiName("X").uiName("").uiName().c_str(),
             "Edge Loop") == 0);
}

/* A bag resolves dynamic defs with itself as the owner. */
static void test_bag_review_fixes()
{
  PropertyBag bag;
  test_assert(
      bag.add(PropertyDef::intProp("count").defaultValue(PropValue::ofInt(4))).ok());
  test_assert(
      bag.add(PropertyDef::intProp("pick").dynamic([](PropertyDef &def, const void *o) {
           const PropertyBag *b = static_cast<const PropertyBag *>(o);
           def.range(0, double(b->get("count")->asInt() - 1));
         }))
          .ok());
  test_assert(bag.set("pick", PropValue::ofInt(10)).ok());
  test_assert(*bag.get("pick") == PropValue::ofInt(3));
  test_assert(bag.setAt(0, PropValue::ofInt(2)).ok());
  test_assert(bag.set("pick", PropValue::ofInt(10)).ok());
  test_assert(*bag.get("pick") == PropValue::ofInt(1));

  test_assert(bag.add(PropertyDef::pointerProp("t", StructId::of("Object"))
                          .access(PropAccess::Optional))
                  .ok());
  StableRef r;
  r.owner = OwnerKey::ofRuntime(1);
  test_assert(bag.set("t", PropValue::ofPointer(r)).ok() && bag.isSet("t"));
  test_assert(bag.set("t", PropValue()).ok()); /* None unsets an Optional */
  test_assert(!bag.isSet("t") && bag.get("t")->isNone());
  test_assert(bag.reset("nope").code() == StatusCode::NotFound);
}

int main()
{
  test_review_fixes();
  test_bag_review_fixes();
  test_ui_names();
  test_defaults_and_meta();
  test_validate();
  test_display();
  test_units_agree();
  test_coerce_numbers();
  test_coerce_vectors();
  test_coerce_enums();
  test_coerce_other();
  test_dynamic();
  test_bag();
  return test_end();
}
