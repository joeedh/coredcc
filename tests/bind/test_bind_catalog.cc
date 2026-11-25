/* apiCatalog's JSON. */
#include "../command/command_fixture.h"

#include "coredcc/bind/catalog.h"
#include "coredcc/command/last_used.h"

#include <cstring>
#include <string>

test_init;

namespace sc = coredcc::script;

namespace {

struct Nothing {
  Status canRun(Context &) const
  {
    return Status();
  }
  Status exec(Context &)
  {
    return Status();
  }
  void undo(Context &)
  {
  }
  uint64_t memSize() const
  {
    return 0;
  }
  void discard()
  {
  }
  static lbt::Struct<Nothing> *defineBindings()
  {
    auto *st = new lbt::Struct<Nothing>("test::Nothing", sizeof(Nothing));
    BIND_COMMAND_CORE(st);
    return st;
  }
};

bool shows(const std::string &json, const std::string &want)
{
  const bool found = json.find(want) != std::string::npos;
  if (!found) {
    fprintf(stderr, "no %s\nin %s\n", want.c_str(), json.c_str());
  }
  return found;
}

/* A catalog with few structs: the whole of it. */
void test_small()
{
  Runtime rt;
  ApiRegistry &reg = rt.ext<ApiRegistry>();
  const ValueAccessor none{[](const PointerRef &, Context &) { return PropValue(); }, {}};
  ApiStruct base("Base");
  base.description("a \"base\"");
  base.add(PropertyDef::intProp("n")
               .description("how many")
               .dynamic([](PropertyDef &, const void *) {}),
           none);
  base.add(PropertyDef::boolProp("d"), none)
      .dynamic([](PropertyDef &, const PointerRef &, Context &) {});
  base.add(PropertyDef::intProp("secret").access(PropAccess::Private), none);
  test_assert(reg.add(std::move(base)).ok());
  ApiStruct root("Root");
  root.uiName("The Root").parent(StructId::of("Base"));
  PointerAccessor p;
  p.get = [](const PointerRef &, Context &) { return PointerRef(); };
  p.refine = [](const PointerRef &) { return StructId::of("Base"); };
  p.candidates.append(StructId::of("Base"));
  p.candidates.append(StructId::of("Root"));
  root.addPointer(PropertyDef::pointerProp("any", StructId()), std::move(p));
  CollectionAccessor c;
  c.length = [](const PointerRef &, Context &) { return int64_t(0); };
  c.at = [](const PointerRef &, Context &, int64_t) { return PointerRef(); };
  c.lookup = [](const PointerRef &, Context &, const KeySegment &) {
    return PointerRef();
  };
  c.keyOf = [](const PointerRef &, Context &, const PointerRef &) {
    return KeySegment::ofHandle(1);
  };
  c.itemStruct = StructId::of("Base");
  root.addCollection(PropertyDef::collectionProp("list", StructId::of("Base")), c);
  c.active = [](const PointerRef &, Context &) { return PointerRef(); };
  root.addCollection(PropertyDef::collectionProp("list2", StructId::of("Base")), c);
  test_assert(reg.add(std::move(root)).ok());
  test_assert(addNativeCommand<Nothing>(
                  rt,
                  CommandDef("test.a")
                      .uiName("Do A")
                      .description("does A")
                      .icon("a")
                      .flags(CommandFlags::NoUndo | CommandFlags::UndoBarrier)
                      .input(PropertyDef::boolProp("on").dynamic(
                          [](PropertyDef &, const void *) {}))
                      .input(PropertyDef::boolProp("hidden").access(PropAccess::Private))
                      .output(PropertyDef::stringProp("out")))
                  .ok());
  test_assert(
      addNativeCommand<Nothing>(rt, CommandDef("test.b").flags(CommandFlags::Private))
          .ok());

  int data = 0;
  test_assert(
      reg.addRuntimeOwner("root", StructId::of("Root"), [&] { return (void *)&data; })
          .ok());
  Context ctx(&rt, nullptr, reg.runtimeOwner("root"));
  const std::string want =
      "{\"root\":\"Root\",\"structs\":["
      "{\"name\":\"Base\",\"uiName\":\"Base\",\"description\":\"a "
      "\\\"base\\\"\",\"members\":["
      "{\"kind\":\"Int\",\"readOnly\":true,\"dynamic\":true,\"name\":\"n\",\"uiName\":"
      "\"N\",\"description\":"
      "\"how many\",\"access\":[],\"ui\":{},\"precision\":0,\"default\":0},"
      "{\"kind\":\"Bool\",\"readOnly\":true,\"dynamic\":true,\"name\":\"d\",\"uiName\":"
      "\"D\","
      "\"access\":[],\"ui\":{},\"default\":false}]},"
      "{\"name\":\"Root\",\"uiName\":\"The Root\",\"parent\":\"Base\",\"members\":["
      "{\"kind\":\"Pointer\",\"readOnly\":true,\"targets\":[\"Base\",\"Root\"],\"name\":"
      "\"any\","
      "\"uiName\":\"Any\",\"access\":[],\"ui\":{},\"default\":null},"
      "{\"kind\":\"Collection\",\"readOnly\":true,\"active\":false,\"name\":\"list\","
      "\"uiName\":"
      "\"List\",\"access\":[],\"ui\":{},\"target\":\"Base\"},"
      "{\"kind\":\"Collection\",\"readOnly\":true,\"active\":true,\"name\":\"list2\","
      "\"uiName\":"
      "\"List2\",\"access\":[],\"ui\":{},\"target\":\"Base\"}]}],"
      "\"commands\":[{\"id\":\"test.a\",\"uiName\":\"Do A\",\"description\":\"does "
      "A\",\"icon\":"
      "\"a\",\"flags\":[\"NoUndo\",\"UndoBarrier\"],\"inputs\":[{\"kind\":\"Bool\","
      "\"dynamic\":true,\"name\":\"on\","
      "\"uiName\":\"On\",\"access\":[],\"ui\":{},\"default\":false}],\"outputs\":[{"
      "\"kind\":"
      "\"String\",\"name\":\"out\",\"uiName\":\"Out\",\"access\":[],\"ui\":{},"
      "\"default\":\"\"}]}]}";
  const std::string got = apiCatalog(ctx);
  if (got != want) {
    fprintf(stderr, "got  %s\nwant %s\n", got.c_str(), want.c_str());
  }
  test_assert(got == want);
  Context bare(&rt, nullptr, PointerRef());
  test_assert(apiCatalog(bare).rfind("{\"structs\":[", 0) == 0);
}

void test_scene()
{
  Setup s;
  sc::StringResult r = sc::apiCatalog(s.ctx);
  test_assert(r.status.ok());
  const std::string json = r.value.c_str();
  test_assert(shows(json, "{\"root\":\"Scene\",\"structs\":["));
  test_assert(shows(json, "\"id\":\"test.add\""));
  test_assert(shows(json, "\"name\":\"location\""));
}

/* The structs of commandDefaults are all there, whatever was used first. */
void test_defaults()
{
  const auto make = [](bool use) {
    Runtime rt;
    ApiRegistry &reg = rt.ext<ApiRegistry>();
    LastUsedStore &store = rt.ext<LastUsedStore>();
    for (const char *id : {"test.b", "test.a", "test.hidden"}) {
      CommandDef def(id);
      def.input(PropertyDef::intProp("n"));
      if (strcmp(id, "test.hidden") == 0) {
        def.flags(CommandFlags::Private);
      }
      test_assert(addNativeCommand<Nothing>(rt, std::move(def)).ok());
    }
    const CommandRegistry &commands = rt.ext<CommandRegistry>();
    test_assert(!store.itemStruct(*commands.find("test.a")));
    ApiStruct root("Root");
    store.addCollectionTo(root);
    test_assert(reg.add(std::move(root)).ok());
    if (use) {
      test_assert(bool(store.itemStruct(*commands.find("test.hidden"))));
      test_assert(bool(store.itemStruct(*commands.find("test.a"))));
      test_assert(reg.add(ApiStruct("Later")).ok());
    } else {
      test_assert(reg.add(ApiStruct("Later")).ok());
    }
    int data = 0;
    test_assert(
        reg.addRuntimeOwner("root", StructId::of("Root"), [&] { return (void *)&data; })
            .ok());
    Context ctx(&rt, nullptr, reg.runtimeOwner("root"));
    const std::string out = apiCatalog(ctx);
    test_assert(out == apiCatalog(ctx));
    return out;
  };
  const std::string json = make(false);
  test_assert(json == make(true));
  const size_t b = json.find("{\"name\":\"CommandDefaults:test.b\"");
  const size_t a = json.find("{\"name\":\"CommandDefaults:test.a\"");
  test_assert(b != json.npos && a != json.npos && b < a && json.find("\"Later\"") < b);
  test_assert(json.find("hidden") == json.npos &&
              shows(json, "\"name\":\"CommandDefaults\""));
}

} // namespace

int main()
{
  test_defaults();
  test_small();
  test_scene();
  return test_end();
}
