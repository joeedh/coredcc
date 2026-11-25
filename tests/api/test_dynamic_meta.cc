/* Members with dynamic defs: their keys in paths, and watching what `getMeta` gives. */
#include "scene_fixture.h"

#include "coredcc/api/change_bus.h"
#include "coredcc/api/resolve.h"
#include "coredcc/api/static_resolve.h"

#include <cstring>
#include <memory>
#include <string>
#include <vector>

test_init;

using namespace coredcc;

namespace {

/* `camera` has an item per camera, `bits` may have another flag, and `level` goes to the number
 * of cameras. */
struct Data {
  int cameras = 1;
  int camera = 0;
  uint32_t bits = 0;
  double level = 0;
  double fixed = 0;
  bool locked = false;
};

const char *const kCams[] = {"Cam0", "Cam1", "Cam2"};

struct DynWorld {
  Runtime rt;
  Library lib{&rt};
  Data data;
  Context ctx{&rt, &lib, define()};

  PointerRef define()
  {
    ApiRegistry &reg = rt.ext<ApiRegistry>();
    ApiStruct s("Dyn");
    s.addField(PropertyDef::intProp("cameras"), &Data::cameras);
    s.addField(PropertyDef::boolProp("locked"), &Data::locked);
    s.addField(PropertyDef::enumProp("camera").item("NONE", 0), &Data::camera)
        .dynamic([](PropertyDef &d, const PointerRef &self, Context &) {
          const Data &data = *static_cast<const Data *>(self.data);
          for (int i = 0; i < data.cameras; i++) {
            d.item(kCams[i], i + 1);
          }
        });
    s.addField(PropertyDef::flagsProp("bits").item("X", 1).dynamic([](PropertyDef &d, const void *o) {
                 if (static_cast<const Data *>(o)->cameras > 1) {
                   d.item("Y", 2);
                 }
               }),
               &Data::bits);
    s.addField(PropertyDef::floatProp("level").range(0, 1).dynamic([](PropertyDef &d, const void *o) {
                 d.range(0, static_cast<const Data *>(o)->cameras);
                 if (static_cast<const Data *>(o)->locked) {
                   d.access(PropAccess::ReadOnly);
                 }
               }),
               &Data::level);
    s.addField(PropertyDef::floatProp("fixed"), &Data::fixed);
    test_assert(reg.add(std::move(s)).ok());
    test_assert(reg.addRuntimeOwner("dyn", StructId::of("Dyn"), [this] { return (void *)&data; }).ok());
    return reg.runtimeOwner("dyn");
  }
  ChangeBus &bus()
  {
    return rt.ext<ChangeBus>();
  }
};

} // namespace

static void test_keys()
{
  DynWorld w;
  Context &ctx = w.ctx;
  test_assert(setValue(ctx, "camera", PropValue::ofString("Cam0")).ok());
  test_assert(getValue(ctx, "camera[Cam0]")->asBool() && !getValue(ctx, "camera[NONE]")->asBool());
  test_assert(getValue(ctx, "camera[Cam1]").status().code() == StatusCode::NotFound);
  w.data.cameras = 2;
  test_assert(!getValue(ctx, "camera[Cam1]")->asBool());
  test_assert(setValue(ctx, "camera[Cam1]", PropValue::ofBool(true)).ok() && w.data.camera == 2);
  test_assert(resolve(ctx, "camera[Cam1]")->enumValue == 2);
  test_assert(getMeta(ctx, "camera[Cam1]")->kind() == PropKind::Bool);
  test_assert(getMemberMeta(ctx, *resolve(ctx, "camera[Cam1]"))->enumMeta()->items.size() == 3);
  test_assert(!getMemberMeta(ctx, *resolve(ctx, Path())).ok());
  test_assert(setValue(ctx, "bits[Y]", PropValue::ofBool(true)).ok() && w.data.bits == 2);
  test_assert(setValue(ctx, "bits[X]", PropValue::ofBool(true)).ok() && w.data.bits == 3);
  test_assert(getValue(ctx, "bits[Y]")->asBool());
  test_assert(setValue(ctx, "bits[Y]", PropValue::ofBool(false)).ok() && w.data.bits == 1);

  /* Without an instance, a key that isn't known may be there. */
  StaticResolver statics(w.rt);
  const StaticStructs root = statics.only(StructId::of("Dyn"));
  Result<StaticResolution> any = statics.resolve(root, *parsePath("camera[Other]"));
  test_assert(any.ok() && any->unknown && any->targets.size() == 0);
  Result<StaticResolution> known = statics.resolve(root, *parsePath("camera[NONE]"));
  test_assert(known.ok() && !known->unknown && known->targets.size() == 1);
}

static void test_same()
{
  const auto def = [] {
    return PropertyDef::floatProp("f").range(0, 1).description("d").icon("i").step(1);
  };
  test_assert(def().sameAs(def()));
  test_assert(def().sameAs(def().dynamic([](PropertyDef &, const void *) {})));
  const std::vector<PropertyDef> others = {
      def().range(0, 2), def().range(-1, 1), def().softRange(0, 0.5), def().softRange(0.5, 1),
      def().step(2), def().precision(1), def().unit(UnitFamily::Length), def().uiName("g"),
      def().description("e"), def().icon("j"), def().subtype(PropSubtype::Angle),
      def().access(PropAccess::ReadOnly), def().defaultValue(PropValue::ofFloat(0.5)),
      def().ui(PropUi{true, false, SliderStyle::Default, false}),
      def().ui(PropUi{false, true, SliderStyle::Default, false}),
      def().ui(PropUi{false, false, SliderStyle::Slider, false}),
      def().ui(PropUi{false, false, SliderStyle::Default, true}),
      PropertyDef::floatProp("g").range(0, 1).description("d").icon("i").step(1),
      PropertyDef::vecProp("f", 2).range(0, 1).description("d").icon("i").step(1)};
  for (const PropertyDef &other : others) {
    test_assert(!def().sameAs(other) && other.sameAs(other));
  }
  const auto items = [](const char *key, const char *ui, const char *text, const char *icon) {
    return PropertyDef::enumProp("e").item(key, 0, ui, text, icon);
  };
  const PropertyDef a = items("A", "a", "b", "c");
  test_assert(a.sameAs(items("A", "a", "b", "c")));
  const std::vector<PropertyDef> enums = {
      items("B", "a", "b", "c"), items("A", "x", "b", "c"), items("A", "a", "x", "c"),
      items("A", "a", "b", "x"), items("A", "a", "b", "c").item("B", 1),
      PropertyDef::enumProp("e").item("A", 1, "a", "b", "c").defaultValue(a.defaultValue())};
  for (const PropertyDef &other : enums) {
    test_assert(!a.sameAs(other) && !other.sameAs(a));
  }
  test_assert(!PropertyDef::stringProp("s").sameAs(PropertyDef::stringProp("s").maxLength(1)));
  test_assert(!PropertyDef::pointerProp("p", StructId::of("A"))
                   .sameAs(PropertyDef::pointerProp("p", StructId::of("B"))));
}

static void test_watch()
{
  DynWorld w;
  Context &ctx = w.ctx;
  std::string log;
  std::vector<ScopedConnection> all;
  for (const char *path : {"camera", "level", "fixed", "camera[Cam1]", "camera[NONE]"}) {
    const std::string name = path;
    all.push_back(w.bus()
                      .watch(
                          ctx, path,
                          [&log, name](const PropValue &, const Status &) { log += name + ";"; },
                          [&log, name] { log += "meta " + name + ";"; })
                      .value());
  }
  const auto flush = [&] {
    log.clear();
    w.bus().flush();
    return log;
  };
  test_assert(flush() == "");
  /* Any change: what the hooks look at isn't known. */
  w.data.cameras = 2;
  test_assert(flush() == "");
  w.bus().notify(OwnerKey::ofRuntime(1), "something");
  test_assert(flush() == "meta camera;meta level;meta camera[Cam1];camera[Cam1];meta camera[NONE];");
  w.bus().notify(OwnerKey::ofRuntime(1), "something");
  test_assert(flush() == "");
  /* A value, and one that isn't dynamic. */
  test_assert(setValue(ctx, "camera", PropValue::ofString("Cam1")).ok());
  test_assert(setValue(ctx, "fixed", PropValue::ofFloat(1)).ok());
  test_assert(flush() == "camera;fixed;camera[Cam1];camera[NONE];");
  test_assert(setValue(ctx, "locked", PropValue::ofBool(true)).ok() && flush() == "meta level;");
  /* A key that goes. */
  w.data.cameras = 1;
  w.bus().bumpEpoch();
  test_assert(flush() == "meta camera;meta level;meta camera[Cam1];camera[Cam1];meta camera[NONE];");
  /* A callback of the def that disconnects the watcher. */
  all.clear();
  all.resize(2);
  int values = 0;
  all[0] = w.bus()
               .watch(
                   ctx, "camera[Cam1]", [&](const PropValue &, const Status &) { values++; },
                   [&] { all[0].disconnect(); })
               .value();
  w.data.cameras = 2;
  w.bus().bumpEpoch();
  test_assert(w.bus().flush() == 1 && values == 0 && w.bus().watcherCount() == 0);
  /* What it holds goes with it. */
  const auto ignore = [](const PropValue &, const Status &) {};
  auto held = std::make_shared<int>(1);
  all[0] = w.bus().watch(ctx, "level", ignore, [held] { (void)held; }).value();
  all[1] = w.bus().watch(ctx, "level", ignore).value();
  test_assert(held.use_count() == 2);
  all[0].disconnect();
  test_assert(held.use_count() == 1);
  /* And those without one. */
  w.data.cameras = 3;
  w.bus().bumpEpoch();
  test_assert(w.bus().flush() == 0);
}

/* A path to another member. */
static void test_repointed()
{
  fixture::World w;
  Context ctx = w.context();
  ChangeBus &bus = w.rt.ext<ChangeBus>();
  int meta = 0, value = 0;
  ScopedConnection c = bus.watch(
                              ctx, "objects.active.name",
                              [&](const PropValue &, const Status &) { value++; }, [&] { meta++; })
                           .value();
  w.scene.active = 1;
  bus.bumpEpoch();
  test_assert(bus.flush() == 1 && value == 1 && meta == 0); /* the same member */
  w.scene.active = -1;
  bus.bumpEpoch();
  test_assert(bus.flush() == 1 && value == 2 && meta == 1);
  w.scene.active = 0;
  bus.bumpEpoch();
  test_assert(bus.flush() == 1 && value == 3 && meta == 2);
}

int main()
{
  test_keys();
  test_same();
  test_watch();
  test_repointed();
  return test_end();
}
