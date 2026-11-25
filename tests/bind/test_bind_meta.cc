/* pathMeta's JSON, path kinds, and the facades for names, pointers and gestures. */
#include "../command/command_fixture.h"

#include "coredcc/api/ref_text.h"
#include "coredcc/bind/facades.h"
#include "coredcc/bind/meta.h"

#include <cstring>
#include <string>

test_init;

namespace sc = coredcc::script;

namespace {

/* A struct with one member of each kind and every piece of metadata. */
struct Rich {
  double f = 0;
  int64_t i = 0;
  bool b = false;
  litestl::util::string s;
  double v[2] = {0, 0};
  int e = 0;
  uint32_t flags = 0;
  double secret = 0;
  int cam = 0;
};

const StructId kRich = StructId::of("Rich");

void registerRich(Runtime &rt, Rich &rich)
{
  ApiRegistry &reg = rt.ext<ApiRegistry>();
  ApiStruct base("RichBase");
  base.addField(PropertyDef::boolProp("b").description("from the \"base\""), &Rich::b);
  base.addField(PropertyDef::floatProp("secret").access(PropAccess::Private), &Rich::secret);
  base.addField(PropertyDef::floatProp("f"), &Rich::f);
  test_assert(reg.add(std::move(base)).ok());
  ApiStruct s("Rich");
  s.parent(StructId::of("RichBase")).uiName("Rich Thing").description("All of it");
  s.addField(PropertyDef::enumProp("cam").item("NONE", 0).dynamic([](PropertyDef &d, const void *) {
               d.item("Cam1", 1, "Camera 1");
             }),
             &Rich::cam);
  s.addField(PropertyDef::floatProp("f")
                 .uiName("Factor")
                 .description("how much")
                 .icon("drop")
                 .range(-1, 100)
                 .softRange(0, 10)
                 .step(0.25)
                 .precision(2)
                 .subtype(PropSubtype::Distance)
                 .unit(UnitFamily::Length)
                 .access(PropAccess::Animatable | PropAccess::NoUndo)
                 .ui(PropUi{false, true, SliderStyle::Slider, false})
                 .defaultValue(PropValue::ofFloat(1.5)),
             &Rich::f);
  s.addField(PropertyDef::intProp("i").range(0, INFINITY).defaultValue(
                 PropValue::ofInt((int64_t(1) << 53) + 1)),
             &Rich::i);
  s.addField(PropertyDef::stringProp("s").maxLength(8).ui(
                 PropUi{true, false, SliderStyle::Number, true}),
             &Rich::s);
  s.addField(PropertyDef::vecProp("v", 2).range(0, 1).defaultValue(PropValue::ofVec2(0, 0.5)),
             &Rich::v);
  s.addField(PropertyDef::enumProp("e")
                 .item("A", 0, "First", "the first", "one")
                 .item("B", 5)
                 .defaultValue(PropValue::ofEnum(5)),
             &Rich::e);
  s.addField(PropertyDef::flagsProp("flags").item("X", 1).item("Z", 2).item("Y", 4).defaultValue(
                 PropValue::ofFlags(5)),
             &Rich::flags);
  test_assert(reg.add(std::move(s)).ok());
  test_assert(reg.addRuntimeOwner("rich", kRich, [&rich]() { return (void *)&rich; }).ok());
}

struct MetaWorld {
  Rich rich;
  Setup s;
  Context ctx;
  MetaWorld()
      : ctx(&s.w.rt, &s.w.lib,
            (registerRich(s.w.rt, rich), s.w.rt.ext<ApiRegistry>().runtimeOwner("rich")))
  {
    ctx.setExecutor(&s.ex);
  }
};

bool metaIs(Context &ctx, const char *path, const std::string &want)
{
  Result<std::string> got = pathMeta(ctx, path);
  if (!got.ok() || *got != want) {
    fprintf(stderr, "%s:\ngot  %s\nwant %s\n", path,
            got.ok() ? got->c_str() : got.status().message(), want.c_str());
    return false;
  }
  return true;
}

bool hasField(Context &ctx, const char *path, const char *field)
{
  Result<std::string> got = pathMeta(ctx, path);
  if (!got.ok() || got->find(field) == std::string::npos) {
    fprintf(stderr, "%s: no %s in %s\n", path, field,
            got.ok() ? got->c_str() : got.status().message());
    return false;
  }
  return true;
}

} // namespace

static void test_dynamic()
{
  MetaWorld m;
  test_assert(metaIs(m.ctx, "cam",
                     "{\"kind\":\"Enum\",\"canonical\":\"@owner(rich).cam\",\"struct\":\"Rich\","
                     "\"readOnly\":false,\"dynamic\":true,\"name\":\"cam\",\"uiName\":\"Cam\","
                     "\"access\":[],\"ui\":{},\"items\":[{\"key\":\"NONE\",\"value\":0,\"uiName\":"
                     "\"NONE\"},{\"key\":\"Cam1\",\"value\":1,\"uiName\":\"Camera 1\"}],"
                     "\"default\":\"NONE\"}"));
  test_assert(hasField(m.ctx, "cam[Cam1]", "\"uiName\":\"Camera 1\""));
  test_assert(hasField(m.ctx, "cam[Cam1]", "\"dynamic\":true"));
}

static void test_members()
{
  MetaWorld m;
  const std::string at = "\"canonical\":\"@owner(rich).";
  test_assert(metaIs(m.ctx, "f",
                     "{\"kind\":\"Float\"," + at + "f\",\"struct\":\"Rich\",\"readOnly\":false,"
                     "\"name\":\"f\",\"uiName\":\"Factor\",\"description\":\"how much\","
                     "\"icon\":\"drop\",\"subtype\":\"Distance\",\"access\":[\"NoUndo\","
                     "\"Animatable\"],\"ui\":{\"expand\":true,\"slider\":\"Slider\"},"
                     "\"hardMin\":-1,\"hardMax\":100,\"softMin\":0,\"softMax\":10,\"step\":0.25,"
                     "\"precision\":2,\"unit\":\"Length\",\"default\":1.5}"));
  test_assert(metaIs(m.ctx, "i",
                     "{\"kind\":\"Int\"," + at + "i\",\"struct\":\"Rich\",\"readOnly\":false,"
                     "\"name\":\"i\",\"uiName\":\"I\",\"access\":[],\"ui\":{},\"hardMin\":0,"
                     "\"softMin\":0,\"precision\":0,\"default\":\"9007199254740993\"}"));
  test_assert(metaIs(m.ctx, "s",
                     "{\"kind\":\"String\"," + at + "s\",\"struct\":\"Rich\",\"readOnly\":false,"
                     "\"name\":\"s\",\"uiName\":\"S\",\"access\":[],\"ui\":{\"iconsOnly\":true,"
                     "\"slider\":\"Number\",\"multiline\":true},\"maxLength\":8,"
                     "\"default\":\"\"}"));
  test_assert(metaIs(m.ctx, "b",
                     "{\"kind\":\"Bool\"," + at + "b\",\"struct\":\"Rich\",\"readOnly\":false,"
                     "\"name\":\"b\",\"uiName\":\"B\",\"description\":\"from the \\\"base\\\"\","
                     "\"access\":[],\"ui\":{},\"default\":false}"));
  test_assert(metaIs(m.ctx, "e",
                     "{\"kind\":\"Enum\"," + at + "e\",\"struct\":\"Rich\",\"readOnly\":false,"
                     "\"name\":\"e\",\"uiName\":\"E\",\"access\":[],\"ui\":{},\"items\":["
                     "{\"key\":\"A\",\"value\":0,\"uiName\":\"First\",\"description\":"
                     "\"the first\",\"icon\":\"one\"},{\"key\":\"B\",\"value\":5,"
                     "\"uiName\":\"B\"}],\"default\":\"B\"}"));
  test_assert(hasField(m.ctx, "flags", "\"default\":[\"X\",\"Y\"]}"));
  test_assert(hasField(m.ctx, "v", "\"kind\":\"Vec2\"") &&
              hasField(m.ctx, "v", "\"default\":[0,0.5]}"));
  /* The soft range without one is the hard range. */
  test_assert(hasField(m.ctx, "v", "\"softMin\":0,\"softMax\":1"));
}

static void test_other_kinds()
{
  MetaWorld m;
  Context scene = m.s.w.context();
  test_assert(metaIs(m.ctx, "v[1]",
                     "{\"kind\":\"Component\",\"canonical\":\"@owner(rich).v\","
                     "\"struct\":\"Rich\",\"component\":1,\"of\":\"Vec2\",\"readOnly\":false,"
                     "\"name\":\"v\",\"uiName\":\"V\",\"access\":[],\"ui\":{},\"hardMin\":0,"
                     "\"hardMax\":1,\"softMin\":0,\"softMax\":1,\"precision\":3,"
                     "\"default\":[0,0.5]}"));
  test_assert(metaIs(m.ctx, "e[A]",
                     "{\"kind\":\"EnumKey\",\"canonical\":\"@owner(rich).e\","
                     "\"struct\":\"Rich\",\"key\":\"A\",\"of\":\"Enum\",\"readOnly\":false,"
                     "\"name\":\"A\",\"uiName\":\"First\",\"description\":\"the first\","
                     "\"icon\":\"one\",\"access\":[],\"ui\":{},\"default\":false}"));
  test_assert(hasField(m.ctx, "flags[Y]", "\"of\":\"Flags\""));
  test_assert(metaIs(scene, "objects.length",
                     "{\"kind\":\"Length\",\"canonical\":\"@owner(scene).objects\","
                     "\"struct\":\"Scene\",\"target\":\"Object\",\"readOnly\":true,"
                     "\"name\":\"length\","
                     "\"uiName\":\"Length\",\"access\":[\"ReadOnly\"],\"ui\":{},\"precision\":0,"
                     "\"default\":0}"));
  test_assert(hasField(scene, "objects", "\"kind\":\"Collection\"") &&
              hasField(scene, "objects", "\"target\":\"Object\""));
  test_assert(hasField(scene, "objects[0].data", "\"kind\":\"Pointer\"") &&
              hasField(scene, "objects[0].data", "\"target\":\"Mesh\""));
  test_assert(hasField(scene, "objects[0].data", "\"readOnly\":false") &&
              hasField(scene, "objects[0].data", "\"default\":null}"));
  test_assert(hasField(scene, "objects[0].mod", "\"readOnly\":true"));
  test_assert(pathMeta(scene, "objects[0].name")->find("maxLength") == std::string::npos);
  test_assert(hasField(scene, "tool.version", "\"readOnly\":true"));
  /* Dynamic hooks count. */
  test_assert(hasField(scene, "tool.pick", "\"hardMax\":3"));
  m.s.w.tool.locked = true;
  test_assert(hasField(scene, "tool.guarded", "\"readOnly\":true"));

  /* Structs: own members, then the parents', without hidden and Private ones. */
  ApiRegistry &reg = m.s.w.rt.ext<ApiRegistry>();
  ApiStruct holder("Holder");
  PointerAccessor p;
  p.get = [&m](const PointerRef &, Context &) {
    return m.s.w.rt.ext<ApiRegistry>().runtimeOwner("rich");
  };
  p.target = kRich;
  holder.addPointer(PropertyDef::pointerProp("rich", kRich), std::move(p));
  test_assert(reg.add(std::move(holder)).ok());
  static int dummy = 0;
  test_assert(reg.addRuntimeOwner("holder", StructId::of("Holder"), []() {
                   return (void *)&dummy;
                 }).ok());
  Context hc(&m.s.w.rt, &m.s.w.lib, reg.runtimeOwner("holder"));
  test_assert(hasField(hc, "rich", "\"kind\":\"Pointer\""));
  hc.setPrefix(*parsePath("rich"));
  test_assert(hasField(hc, "f", "\"kind\":\"Float\""));
  test_assert(metaIs(scene, "objects[0].data.radius",
                     *pathMeta(scene, "objects[\"Cube\"].data.radius")));
  Result<std::string> cube = pathMeta(scene, "objects[0]");
  const std::string ref = formatRef(m.s.w.rt, canonicalize(scene, *parsePath("objects[0]")).value());
  test_assert(cube.ok() &&
              cube->find("{\"kind\":\"Struct\",\"canonical\":\"" + ref +
                         "\",\"struct\":\"Object\",\"uiName\":\"Object\",\"members\":[\"name\","
                         "\"location\",\"mode\",\"flags\",\"data\"") == 0);
  test_assert(pathMeta(scene, "objcts").status().code() == StatusCode::NotFound);
}

static void test_struct_members()
{
  MetaWorld m;
  ApiRegistry &reg = m.s.w.rt.ext<ApiRegistry>();
  /* A collection of one Rich: its item is a Struct path. */
  ApiStruct list("List");
  CollectionAccessor c;
  c.length = [](const PointerRef &, Context &) { return int64_t(1); };
  c.at = [&m](const PointerRef &, Context &, int64_t) {
    return PointerRef{&m.rich, kRich, OwnerKey()};
  };
  c.lookup = [&m](const PointerRef &, Context &, const KeySegment &) {
    return PointerRef{&m.rich, kRich, OwnerKey()};
  };
  c.keyOf = [](const PointerRef &, Context &, const PointerRef &) {
    return KeySegment::ofHandle(7);
  };
  c.itemStruct = kRich;
  list.addCollection(PropertyDef::collectionProp("all", kRich), std::move(c));
  test_assert(reg.add(std::move(list)).ok());
  test_assert(
      reg.addRuntimeOwner("list", StructId::of("List"), [&m]() { return (void *)&m; }).ok());
  Context ctx(&m.s.w.rt, &m.s.w.lib, reg.runtimeOwner("list"));
  test_assert(metaIs(ctx, "all[0]",
                     "{\"kind\":\"Struct\",\"canonical\":\"@owner(list).all[#7] : Rich\","
                     "\"struct\":\"Rich\",\"uiName\":\"Rich Thing\",\"description\":\"All of it\","
                     "\"members\":[\"cam\",\"f\",\"i\",\"s\",\"v\",\"e\",\"flags\",\"b\"]}"));
  test_assert(hasField(ctx, "all.length", "\"target\":\"Rich\""));
}

static void test_facades()
{
  MetaWorld m;
  Context &ctx = m.ctx;
  const auto kind = [&](Context &c, const char *path) {
    return std::string(sc::resolvePath(c, path).value.c_str());
  };
  Context scene = m.s.w.context();
  test_assert(kind(ctx, "e") == "Enum" && kind(ctx, "e[B]") == "EnumKey");
  test_assert(kind(ctx, "v[0]") == "Component" && kind(scene, "objects") == "Collection");
  test_assert(kind(scene, "objects.length") == "Length" && kind(scene, "objects[0]") == "Struct");
  test_assert(sc::pathMeta(ctx, "e").status.ok() &&
              std::string(sc::pathMeta(ctx, "e").value.c_str()) == *pathMeta(ctx, "e"));
  test_assert(sc::pathMeta(ctx, "nope").status.code() == StatusCode::NotFound);

  const auto keys = [&](const char *path, const PropValue &v) {
    return sc::valueKeys(ctx, path, v);
  };
  test_assert(strcmp(keys("e", PropValue::ofEnum(5)).value.c_str(), "B") == 0);
  test_assert(keys("e", PropValue::ofEnum(6)).status.code() == StatusCode::NotFound);
  test_assert(strcmp(keys("flags", PropValue::ofFlags(5)).value.c_str(), "X|Y") == 0);
  test_assert(strcmp(keys("flags", PropValue::ofFlags(4)).value.c_str(), "Y") == 0);
  test_assert(keys("flags", PropValue::ofFlags(0)).status.ok() &&
              keys("flags", PropValue::ofFlags(0)).value.size() == 0);
  test_assert(keys("flags", PropValue::ofFlags(9)).status.code() == StatusCode::NotFound);
  test_assert(hasMsg(keys("e", PropValue::ofFloat(1)).status, "Enum or Flags"));
  test_assert(keys("f", PropValue::ofFloat(1)).status.code() == StatusCode::InvalidArgument);
  test_assert(keys("f", PropValue::ofEnum(1)).status.code() == StatusCode::InvalidArgument);
  test_assert(keys("nope", PropValue::ofEnum(1)).status.code() == StatusCode::NotFound);

  /* Pointers as text. */
  test_assert(sc::makeNullPointer() == PropValue::nullPointer());
  test_assert(sc::makeFlags(-1) == PropValue::ofFlags(UINT64_MAX));
  sc::ValueResult p = sc::makePointer(ctx, "@owner(tool).brushes['A'] : Brush");
  test_assert(p.status.ok() && p.value.asPointer().key.size() == 2);
  test_assert(strcmp(sc::pointerText(ctx, p.value).value.c_str(),
                     "@owner(tool).brushes[\"A\"] : Brush") == 0);
  test_assert(sc::pointerText(ctx, sc::makeNullPointer()).value.size() == 0);
  test_assert(sc::pointerText(ctx, sc::makeInt(1)).status.code() == StatusCode::InvalidArgument);
  test_assert(sc::makePointer(ctx, "nope").status.code() == StatusCode::InvalidArgument);
  test_assert(sc::makePointer(ctx, "").value == PropValue::nullPointer());

  /* A gesture folds; another one, and none, don't. */
  const int64_t drag = sc::beginGesture(ctx).value;
  test_assert(drag > 0 && sc::beginGesture(ctx).value == drag + 1);
  const auto size = [&]() { return m.s.history.size(); };
  test_assert(sc::setValueGesture(ctx, "i", sc::makeInt(1), drag).ok());
  test_assert(sc::setValueGesture(ctx, "i", sc::makeInt(2), drag).ok() && size() == 1);
  test_assert(sc::setValueGesture(ctx, "i", sc::makeInt(3), drag + 1).ok() && size() == 2);
  test_assert(sc::setValue(ctx, "i", sc::makeInt(4)).ok() && size() == 3);
  test_assert(sc::setValue(ctx, "i", sc::makeInt(5)).ok() && size() == 4);
  for (int n = 0; n < 4; n++) {
    test_assert(sc::undo(ctx).ok());
  }
  test_assert(m.rich.i == 0);
  for (int64_t gesture : {int64_t(0), int64_t(-1), drag + 2}) {
    test_assert(hasMsg(sc::setValueGesture(ctx, "i", sc::makeInt(1), gesture), "isn't from beginGesture"));
  }
  ctx.setExecutor(nullptr);
  test_assert(hasMsg(sc::setValueGesture(ctx, "i", sc::makeInt(1), 1), "setValueGesture: the"));
  test_assert(hasMsg(sc::setValue(ctx, "i", sc::makeInt(1)), "setValue: the context has no"));
}

int main()
{
  test_members();
  test_dynamic();
  test_other_kinds();
  test_struct_members();
  test_facades();
  return test_end();
}
