/* Values as text in a scene's units, and the units by datapath. */
#include "scene_doc.h"

#include "coredcc/api/format.h"

#include <numbers>

test_init;

using namespace scene_test;

namespace {

Status write(Doc &d, const char *path, const PropValue &v)
{
  return setValue(*d.ctx, path, v);
}

std::string text(Doc &d, const char *path)
{
  const Result<std::string> r = formatValue(*d.ctx, path);
  return r.ok() ? *r : std::string("! ") + r.status().message();
}

/* Writes what `parseValue` gives, and formats it. */
std::string typed(Doc &d, const char *path, const char *in)
{
  const Result<PropValue> v = parseValue(*d.ctx, path, in);
  if (!v.ok()) {
    return std::string("! ") + v.status().message();
  }
  const Status s = setValue(*d.ctx, path, *v);
  return s.ok() ? text(d, path) : std::string("! ") + s.message();
}

StatusCode refused(Doc &d, const char *path, const char *in)
{
  return parseValue(*d.ctx, path, in).status().code();
}

/* A scene of a host's own type, whose struct inherits `units`. */
struct HostScene {
  UnitSettings units;
};

void addHostScene(Doc &d)
{
  ApiStruct base("test.HostSceneBase");
  addUnitSettings(base, [](const PointerRef &p) -> UnitSettings * {
    return &static_cast<HostScene *>(p.data)->units;
  });
  must(d.rt.ext<ApiRegistry>().add(std::move(base)).status());
  ApiStruct st("test.HostScene");
  st.parent(StructId::of("test.HostSceneBase"));
  must(d.rt.ext<ApiRegistry>().add(std::move(st)).status());
  d.addSlotType("test.HostScene");
}

void testSettings()
{
  Doc d;
  must(registerUnitSettings(d.rt)); /* idempotent */
  must(write(d, "scene.units.lengthUnit", PropValue::ofString("centimeter")));
  must(write(d, "scene.units.timeUnit", PropValue::ofString("frame")));
  must(write(d, "scene.units.massUnit", PropValue::ofString("pound")));
  must(write(d, "scene.units.angleUnit", PropValue::ofString("radian")));
  must(write(d, "scene.units.factorUnit", PropValue::ofString("percent")));
  must(write(d, "scene.units.startFrame", PropValue::ofFloat(1001.0)));
  must(write(d, "scene.units.fps", PropValue::ofFloat(29.97)));
  const UnitSettings s = d.ctx->units();
  test_assert(s == d.sc().units && s.startFrame == 1001.0 &&
              s.rate == *FrameRate::of(30000, 1001));
  test_assert(!strcmp(s.displayUnit(UnitFamily::Length).name, "centimeter"));
  test_assert(!strcmp(s.displayUnit(UnitFamily::Angle).name, "radian"));
  test_assert(!strcmp(s.displayUnit(UnitFamily::Time).name, "frame"));
  test_assert(!strcmp(s.displayUnit(UnitFamily::Mass).name, "pound"));
  test_assert(!strcmp(s.displayUnit(UnitFamily::Factor).name, "percent"));
  /* Each reads what it was set to, so that undo is exact. */
  for (const char *m : {"lengthUnit",
                        "angleUnit",
                        "timeUnit",
                        "massUnit",
                        "factorUnit",
                        "fps",
                        "startFrame"})
  {
    const std::string path = std::string("scene.units.") + m;
    const Result<PropValue> v = getValue(*d.ctx, path);
    must(v.status());
    must(setValue(*d.ctx, path, *v));
    test_assert(d.ctx->units() == s);
    const Result<PropertyDef> def = getMeta(*d.ctx, path);
    must(def.status());
    must(def->validate());
    /* Its default is that of the settings. */
    must(setValue(*d.ctx, path, def->defaultValue()));
    test_assert(!(d.ctx->units() == s));
    must(setValue(*d.ctx, path, *v));
  }
  test_assert(write(d, "scene.units.lengthUnit", PropValue::ofString("furlong")).code() !=
              StatusCode::Ok);
  must(write(d, "scene.units.fps", PropValue::ofFloat(0.0)));
  test_assert(d.sc().units.rate == *FrameRate::of(1, 1000));

  for (const char *m : {"lengthUnit",
                        "angleUnit",
                        "timeUnit",
                        "massUnit",
                        "factorUnit",
                        "fps",
                        "startFrame"})
  {
    const std::string path = std::string("scene.units.") + m;
    must(setValue(*d.ctx, path, getMeta(*d.ctx, path)->defaultValue()));
  }
  test_assert(d.sc().units == UnitSettings());
}

void testActiveScene()
{
  Doc d;
  addHostScene(d);
  d.sc().units.startFrame = 5.0;
  test_assert(d.ctx->units().startFrame == 5.0);
  const BlockHandle other = d.addScene("other");
  d.ctx->setScene(other);
  test_assert(d.ctx->units() == UnitSettings());
  const BlockHandle host = d.addSlot<HostScene>("test.HostScene", "host");
  static_cast<HostScene *>(d.slot(host).data)->units.startFrame = 6.0;
  d.ctx->setScene(host);
  test_assert(d.ctx->units().startFrame == 6.0);
  /* A block without units, without data, deleted, none, and no library. */
  d.ctx->setScene(d.object("a"));
  test_assert(d.ctx->units() == UnitSettings());
  d.ctx->setScene(host);
  void *data = d.slot(host).data;
  d.slot(host).data = nullptr;
  test_assert(d.ctx->units() == UnitSettings());
  d.slot(host).data = data;
  test_assert(d.ctx->units().startFrame == 6.0);
  must(d.lib->markDeleted(host));
  test_assert(d.ctx->units() == UnitSettings());
  d.ctx->setScene(BlockHandle());
  test_assert(d.ctx->units() == UnitSettings());
  Context bare(&d.rt, nullptr, PointerRef());
  bare.setScene(d.scene);
  test_assert(bare.units() == UnitSettings());
}

/* A member `units` that isn't one of these. */
void testOtherUnits()
{
  struct Plain {
    double units = 3.0;
  };
  Doc d;
  ApiStruct plain("test.PlainScene");
  plain.addField(PropertyDef::floatProp("units"), &Plain::units);
  must(d.rt.ext<ApiRegistry>().add(std::move(plain)).status());
  ApiStruct pointer("test.PointerScene");
  PointerAccessor a;
  a.target = StructId::of("test.PlainScene");
  a.get = [target = a.target](const PointerRef &p, Context &) {
    return PointerRef{p.data, target, OwnerKey()};
  };
  pointer.addPointer(PropertyDef::pointerProp("units", a.target), std::move(a));
  must(d.rt.ext<ApiRegistry>().add(std::move(pointer)).status());
  ApiStruct null("test.NullScene");
  addUnitSettings(null, [](const PointerRef &) -> UnitSettings * { return nullptr; });
  must(d.rt.ext<ApiRegistry>().add(std::move(null)).status());
  for (const char *type : {"test.PlainScene", "test.PointerScene", "test.NullScene"}) {
    d.addSlotType(type);
    d.ctx->setScene(d.addSlot<Plain>(type, type));
    test_assert(d.ctx->units() == UnitSettings());
  }
}

void testPaths()
{
  Doc d;
  const BlockHandle a = d.object("a", 1.5, -2.0, 0.25);
  put(d.obj(a).channels, trs::kRotationEuler, double3(std::numbers::pi / 2, 0.0, 0.0));
  const char *const at = "scene.objects[\"a\"].transform.location",
                    *const x = "scene.objects[\"a\"].transform.location[1]",
                    *const turn = "scene.objects[\"a\"].transform.rotationEuler";
  test_assert(text(d, at) == "1.5 m, -2 m, 0.25 m" && text(d, x) == "-2 m");
  test_assert(text(d, turn) == "90\xC2\xB0, 0\xC2\xB0, 0\xC2\xB0");
  must(d.sc().units.setDisplayUnit(UnitFamily::Length, "cm"));
  test_assert(text(d, at) == "150 cm, -200 cm, 25 cm" && text(d, x) == "-200 cm");
  test_assert(typed(d, at, " 1, 2 m ,\t3mm") == "1 cm, 200 cm, 0.3 cm");
  test_assert(typed(d, x, "7") == "7 cm" && text(d, at) == "1 cm, 7 cm, 0.3 cm");
  test_assert(parseValue(*d.ctx, x, "7")->type() == PropValue::Type::Float);
  test_assert(typed(d, at, "4 in") == "10.16 cm, 10.16 cm, 10.16 cm");
  for (const char *bad :
       {"1, 2", "1, 2, 3, 4", "1,,3", "1, 2, 3,", "", ",", "1 kg, 2, 3", "x"})
  {
    test_assert(refused(d, at, bad) == StatusCode::InvalidArgument);
  }
  test_assert(refused(d, x, "1, 2, 3") == StatusCode::InvalidArgument);
  test_assert(typed(d, turn, "180, 0, 1 rad") ==
              "180\xC2\xB0, 0\xC2\xB0, 57.296\xC2\xB0");

  test_assert(text(d, "scene.name") == "Scene" && text(d, "scene.objects.length") == "1");
  test_assert(text(d, "scene.objects[\"a\"].flags[HIDDEN]") == "false");
  test_assert(typed(d, "scene.objects[\"a\"].flags[HIDDEN]", "yes") == "true");
  test_assert(text(d, "scene.objects[\"a\"].flags") == "HIDDEN");
  test_assert(text(d, "scene.units.lengthUnit") == "Centimeter");
  test_assert(typed(d, "scene.units.lengthUnit", "Meter") == "Meter");
  test_assert(typed(d, "scene.units.lengthUnit", "inch") == "Inch");
  test_assert(typed(d, "scene.units.fps", "23.976") == "23.976");
  test_assert(d.sc().units.rate == *FrameRate::of(24000, 1001));
  test_assert(refused(d, "scene.objects", "x") == StatusCode::Unsupported);
  test_assert(refused(d, "scene.activeObject", "a") == StatusCode::Unsupported);
  test_assert(text(d, "scene.activeObject").substr(0, 1) != "!");
  for (const char *bad : {"scene.none", "scene.objects[0]", "none"}) {
    test_assert(!formatValue(*d.ctx, bad).ok() && !parseValue(*d.ctx, bad, "1").ok());
  }
  /* As setValue: the active item's member when there is none. */
  test_assert(refused(d, "scene.objects.active", "a") == StatusCode::Unsupported);
}

void testDefs()
{
  UnitSettings s;
  const auto parsed = [&s](const PropertyDef &def, const char *in) {
    const Result<PropValue> v = parseValue(def, in, s);
    return v.ok() ? *v : PropValue();
  };
  const auto code = [&s](const PropertyDef &def, const char *in) {
    return parseValue(def, in, s).status().code();
  };
  const PropertyDef flag = PropertyDef::boolProp("flag");
  test_assert(formatValue(flag, PropValue::ofBool(true), s) == "true");
  for (const char *in : {"true", " on", "yes ", "1"}) {
    test_assert(parsed(flag, in) == PropValue::ofBool(true));
  }
  for (const char *in : {"false", "off", "no", "0"}) {
    test_assert(parsed(flag, in) == PropValue::ofBool(false));
  }
  test_assert(code(flag, "2") == StatusCode::InvalidArgument);

  /* Ints are exact. */
  const PropertyDef i = PropertyDef::intProp("i");
  const int64_t big = (int64_t(1) << 53) + 1;
  test_assert(formatValue(i, PropValue::ofInt(big), s) == "9007199254740993");
  test_assert(parsed(i, "+9007199254740993") == PropValue::ofInt(big));
  test_assert(parsed(i, "-7") == PropValue::ofInt(-7) &&
              parsed(i, "2e3") == PropValue::ofInt(2000));
  test_assert(code(i, "1.5") == StatusCode::OutOfRange &&
              code(i, "+-1") != StatusCode::Ok);
  test_assert(code(i, "0.9999999999") == StatusCode::OutOfRange);
  const PropertyDef ms =
      PropertyDef::intProp("ms").unit(UnitFamily::Length).range(-10, 1000);
  test_assert(formatValue(ms, PropValue::ofInt(7), s) == "7 m");
  for (int n = -10; n <= 1000; n++) {
    for (const char *unit : {" cm", " mm", "in", " ft", " mi"}) {
      const units::Unit &u =
          *units::findUnit(UnitFamily::Length, unit[0] == ' ' ? unit + 1 : unit);
      const std::string in = units::format(n, UnitFamily::Length, u.symbol, 12);
      test_assert(parsed(ms, in.c_str()) == PropValue::ofInt(n));
    }
  }
  test_assert(code(ms, "3 cm") == StatusCode::OutOfRange);
  test_assert(code(ms, "0.001 mm") == StatusCode::OutOfRange);
  test_assert(parsed(ms, "2 km") == PropValue::ofInt(1000));

  const PropertyDef percent = PropertyDef::floatProp("p")
                                  .subtype(PropSubtype::Percentage)
                                  .unit(UnitFamily::Factor)
                                  .range(0, 1);
  const PropertyDef factor =
      PropertyDef::floatProp("f").subtype(PropSubtype::Factor).unit(UnitFamily::Factor);
  const PropertyDef number =
      PropertyDef::floatProp("n").subtype(PropSubtype::Percentage).precision(1);
  test_assert(formatValue(percent, PropValue::ofFloat(0.5), s) == "50%");
  test_assert(formatValue(factor, PropValue::ofFloat(0.5), s) == "0.5");
  test_assert(formatValue(number, PropValue::ofFloat(0.55), s) == "0.6" ||
              formatValue(number, PropValue::ofFloat(0.55), s) == "0.5");
  test_assert(parsed(percent, "25") == PropValue::ofFloat(0.25));
  test_assert(parsed(percent, "250") == PropValue::ofFloat(1.0));
  test_assert(parsed(factor, "25") == PropValue::ofFloat(25.0));
  test_assert(parsed(factor, "25%") == PropValue::ofFloat(0.25));
  must(s.setDisplayUnit(UnitFamily::Factor, "%"));
  test_assert(formatValue(factor, PropValue::ofFloat(0.5), s) == "50%");
  must(s.setDisplayUnit(UnitFamily::Factor, "factor"));

  const PropertyDef time =
      PropertyDef::floatProp("t").subtype(PropSubtype::Time).unit(UnitFamily::Time);
  must(s.setDisplayUnit(UnitFamily::Time, "f"));
  s.startFrame = 10.0;
  test_assert(formatValue(time, PropValue::ofFloat(0.5), s) == "12 f");
  test_assert(parsed(time, "36") == PropValue::ofFloat(1.5));

  const PropertyDef quat = PropertyDef::quatProp("q"), m4 = PropertyDef::mat4Prop("m");
  test_assert(formatValue(quat, PropValue::ofQuat(0, 0.5, 0, 1), s) == "0, 0.5, 0, 1");
  test_assert(parsed(quat, "0, 0.5, 0, 1") == PropValue::ofQuat(0, 0.5, 0, 1));
  test_assert(code(quat, "1") == StatusCode::InvalidArgument);
  const std::string identity = formatValue(m4, PropValue::identityMat4(), s);
  test_assert(parsed(m4, identity.c_str()) == PropValue::identityMat4());

  const PropertyDef e =
      PropertyDef::enumProp("e").item("ONE", 1, "The One").item("TWO", 2);
  test_assert(formatValue(e, PropValue::ofEnum(1), s) == "The One");
  test_assert(formatValue(e, PropValue::ofEnum(9), s) == "9");
  test_assert(parsed(e, " The One") == PropValue::ofEnum(1) &&
              parsed(e, "TWO") == PropValue::ofEnum(2));
  test_assert(code(e, "THREE") != StatusCode::Ok &&
              parseValue(e, "TWOO", s).status().suggestions().size() > 0);

  const PropertyDef f = PropertyDef::flagsProp("f")
                            .item("A", 1, "First")
                            .item("B", 2)
                            .item("C", int64_t(1) << 62);
  const uint64_t ac = (uint64_t(1) << 62) | 1;
  test_assert(formatValue(f, PropValue::ofFlags(0), s) == "");
  test_assert(formatValue(f, PropValue::ofFlags(ac), s) == "First, C");
  test_assert(parsed(f, "First, C") == PropValue::ofFlags(ac) &&
              parsed(f, "C|A") == PropValue::ofFlags(ac));
  test_assert(parsed(f, "B, C | A") == PropValue::ofFlags(ac | 2));
  test_assert(parsed(f, "") == PropValue::ofFlags(0) &&
              parsed(f, "B") == PropValue::ofFlags(2));
  for (const char *bad : {"A,", ",A", "A,,B", "D", "A B"}) {
    test_assert(code(f, bad) != StatusCode::Ok);
  }

  const PropertyDef str = PropertyDef::stringProp("s").maxLength(4);
  test_assert(parsed(str, " a ") == PropValue::ofString(" a ") &&
              code(str, "abcde") != StatusCode::Ok);
  test_assert(formatValue(str, PropValue::ofString("1 m"), s) == "1 m");
  test_assert(formatValue(str, PropValue(), s) == PropValue().toString());
}

/* parse(format(v)) is v, to half of the last digit shown. */
void testRoundTrip()
{
  Rand r(3);
  UnitSettings s;
  for (int n = 0; n < 3000; n++) {
    const UnitFamily family = UnitFamily(1 + r.below(5));
    must(s.setDisplayIndex(family,
                           int(r.below(uint32_t(UnitSettings::unitCount(family))))));
    s.rate = *FrameRate::of(1 + r.below(240), 1 + r.below(10));
    const int precision = int(r.below(8));
    const PropertyDef def =
        PropertyDef::vecProp("v", 3).unit(family).precision(precision);
    const double3 v = r.vec(std::exp(r.range(-5, 10)));
    const std::string shown = formatValue(def, PropValue::ofVec3(v[0], v[1], v[2]), s);
    const Result<PropValue> back = parseValue(def, shown, s);
    test_assert(back.ok());
    const double digit = std::pow(10.0, -precision) * s.displayUnit(family).scale;
    for (int i = 0; i < 3 && back.ok(); i++) {
      test_assert(std::fabs(back->component(i) - v[i]) <=
                  0.5 * digit * (1 + 1e-9) + 1e-12 * std::fabs(v[i]));
    }
  }
}

} // namespace

int main()
{
  testSettings();
  testActiveScene();
  testOtherUnits();
  testPaths();
  testDefs();
  testRoundTrip();
  return test_end();
}
