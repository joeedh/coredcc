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
  test_assert(strcmp(PropertyDef::floatProp("edgeCrease").uiName().c_str(), "Edge Crease") ==
              0);
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
  test_assert(PropertyDef::floatProp("f").access(PropAccess::ReadOnly).hasAccess(
      PropAccess::ReadOnly));
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
  test_assert(hasMsg(PropertyDef::floatProp("r").precision(16).validate(), "precision"));
  test_assert(hasMsg(PropertyDef::enumProp("e").validate(), "at least one item"));
  test_assert(hasMsg(PropertyDef::enumProp("e").item("A", 0).item("A", 1).validate(),
                     "used twice"));
  test_assert(hasMsg(PropertyDef::enumProp("e").item("A", 0).item("B", 0).validate(),
                     "share a value"));
  test_assert(hasMsg(PropertyDef::enumProp("e").item("a-b", 0).validate(), "identifier"));
  test_assert(hasMsg(PropertyDef::flagsProp("f").item("A", 3).validate(), "single bit"));
  /* A default the clamp would change is out of range. */
  test_assert(hasMsg(
      PropertyDef::floatProp("r").range(0, 1).defaultValue(PropValue::ofFloat(2)).validate(),
      "out of range"));
  test_assert(hasMsg(
      PropertyDef::intProp("n").defaultValue(PropValue::ofString("x")).validate(),
      "bad default"));
}

static void test_coerce_numbers()
{
  const PropertyDef i = PropertyDef::intProp("n").range(-10, 10);
  test_assert(*coerce(i, PropValue::ofInt(3)) == PropValue::ofInt(3));
  test_assert(*coerce(i, PropValue::ofInt(99)) == PropValue::ofInt(10));
  test_assert(*coerce(i, PropValue::ofFloat(-4.0)) == PropValue::ofInt(-4));
  test_assert(!coerce(i, PropValue::ofFloat(1.5)).ok());
  test_assert(!coerce(i, PropValue::ofString("1")).ok());
  test_assert(coerce(i, PropValue::ofBool(true)).status().code() == StatusCode::TypeMismatch);

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
  test_assert(*coerce(v, PropValue::ofFloat(0.25)) == PropValue::ofVec3(0.25, 0.25, 0.25));
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
  const PropertyDef d =
      PropertyDef::floatProp("r").range(0, 100).dynamic([](PropertyDef &def, const void *o) {
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
  test_assert(bag.add(PropertyDef::floatProp("dist")).code() == StatusCode::AlreadyExists);
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
  test_assert(*bag.get("mode") == PropValue::ofEnum(5)); /* a failed set changes nothing */

  Status miss = bag.set("dsit", PropValue::ofFloat(1));
  test_assert(miss.code() == StatusCode::NotFound);
  test_assert(strcmp(miss.suggestions()[0].c_str(), "dist") == 0);

  bag.reset("dist");
  test_assert(!bag.isSet("dist") && *bag.get("dist") == PropValue::ofFloat(1));
  bag.resetAll();
  test_assert(*bag.get("mode") == PropValue::ofEnum(0));
  test_assert(bag.indexOf("target") == 2 && bag.indexOf("nope") == -1);
}

int main()
{
  test_ui_names();
  test_defaults_and_meta();
  test_validate();
  test_coerce_numbers();
  test_coerce_vectors();
  test_coerce_enums();
  test_coerce_other();
  test_dynamic();
  test_bag();
  return test_end();
}
