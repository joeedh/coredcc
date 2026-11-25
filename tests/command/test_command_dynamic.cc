/* Inputs with dynamic defs: in bags, invocations, last-used values, and the
 * commandDefaults. */
#include "command_fixture.h"

#include "coredcc/command/literal.h"
#include "coredcc/layout/validate.h"
#include "coredcc/layout/xml.h"
#include "coredcc/parse/invocation.h"

test_init;

namespace {

int g_hooks = 0;

/* What each hook is promised: the bag of the four inputs. */
bool manyOf(const void *owner)
{
  g_hooks++;
  const auto &bag = *static_cast<const PropertyBag *>(owner);
  test_assert(bag.size() == 4 && bag.indexOf("many") == 3);
  return bag.get("many")->asBool();
}

/* "amount=2 mode=B text=xx", as it ran. */
std::string g_ran;

struct Dyn {
  Status canRun(Context &) const
  {
    return Status();
  }
  Status exec(Context &ctx)
  {
    g_ran.clear();
    for (int i = 0; i < ctx.inputs()->size(); i++) {
      g_ran += ctx.inputs()->value(i).toString() + ";";
    }
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
  static lbt::Struct<Dyn> *defineBindings()
  {
    auto *st = new lbt::Struct<Dyn>("test::Dyn", sizeof(Dyn));
    BIND_COMMAND_CORE(st);
    return st;
  }
};

struct DynWorld {
  Setup s;
  const CommandDef *def;
  DynWorld()
  {
    CommandDef d("x.dyn");
    d.input(PropertyDef::floatProp("amount").range(0, 1).dynamic(
        [](PropertyDef &p, const void *o) { p.range(0, manyOf(o) ? 100 : 1); }));
    d.input(PropertyDef::enumProp("mode").item("NONE", 0).dynamic(
        [](PropertyDef &p, const void *o) {
          p.item("A", 1);
          if (manyOf(o)) {
            p.item("B", 2);
          }
        }));
    d.input(PropertyDef::stringProp("text").maxLength(1).dynamic(
        [](PropertyDef &p, const void *o) { p.maxLength(manyOf(o) ? 4 : 1); }));
    d.input(PropertyDef::boolProp("many"));
    def = *addNativeCommand<Dyn>(s.w.rt, std::move(d));
  }
  /* What it ran with, or the error. */
  std::string run(const char *invocation)
  {
    Result<PropertyBag> r = invoke(s.ctx, invocation);
    return r.ok() ? g_ran : std::string(r.status().message());
  }
};

} // namespace

static void test_invocations()
{
  DynWorld w;
  LastUsedStore &lu = w.s.w.rt.ext<LastUsedStore>();
  test_assert(w.run("x.dyn(mode=A)") == "0;#1;\"\";false;");
  test_assert(w.run("x.dyn(amount=2)") ==
              "1;#1;\"\";false;"); /* and the mode that was used */
  lu.clear();
  /* Whatever the order, they see the others. */
  test_assert(w.run("x.dyn(amount=2 mode=B text=\"xxxx\" many=true)") ==
              "2;#2;\"xxxx\";true;");
  lu.clear();
  test_assert(w.run("x.dyn(mode=2 many=true)") == "0;#2;\"\";true;");
  lu.clear();
  test_assert(strstr(w.run("x.dyn(mode=B)").c_str(), "B"));
  test_assert(strstr(w.run("x.dyn(text=\"xxxx\")").c_str(), "text"));
  test_assert(strstr(w.run("x.dyn(mode=A nope=1)").c_str(), "no input 'nope'"));
  /* A caller's bag is copied in an order that works, too. */
  PropertyBag in = w.def->newInputs();
  for (int i : in.setOrder()) {
    test_assert(in.set(in.def(i).apiName().c_str(),
                       i == 0   ? PropValue::ofFloat(50)
                       : i == 1 ? PropValue::ofString("B")
                       : i == 2 ? PropValue::ofString("xx")
                                : PropValue::ofBool(true))
                    .ok());
  }
  test_assert(w.s.ex.run(w.s.ctx, *w.def, in).ok() && g_ran == "50;#2;\"xx\";true;");
  test_assert(in.resolvedDef(1).enumMeta()->items.size() == 3 &&
              in.resolvedDef(3).kind() == PropKind::Bool);
}

static void test_last_used()
{
  DynWorld w;
  LastUsedStore &lu = w.s.w.rt.ext<LastUsedStore>();
  test_assert(w.run("x.dyn(amount=2 mode=B text=\"xxxx\" many=true)") ==
              "2;#2;\"xxxx\";true;");
  test_assert(*lu.get(*w.def, "mode") == PropValue::ofEnum(2));
  test_assert(w.run("x.dyn") == "2;#2;\"xxxx\";true;");
  const std::string text = lu.serialize();
  test_assert(text == "x.dyn(amount=2 mode=B text=\"xxxx\" many=true)\n");
  lu.clear();
  test_assert(lu.load(text).ok() && lu.serialize() == text);
  /* A last-used value that another input no longer allows isn't used. */
  test_assert(w.run("x.dyn(many=false text=\"\" mode=NONE)") == "1;#0;\"\";false;");
}

static void test_defaults()
{
  DynWorld w;
  Runtime &rt = w.s.w.rt;
  int data = 0;
  ApiStruct root("Prefs");
  rt.ext<LastUsedStore>().addCollectionTo(root);
  test_assert(rt.ext<ApiRegistry>().add(std::move(root)).ok());
  test_assert(
      rt.ext<ApiRegistry>()
          .addRuntimeOwner("prefs", StructId::of("Prefs"), [&] { return (void *)&data; })
          .ok());
  Context pc(&rt, &w.s.w.lib, rt.ext<ApiRegistry>().runtimeOwner("prefs"));
  const std::string at = "commandDefaults[\"x.dyn\"].";
  /* A hook gets a bag here as well. */
  g_hooks = 0;
  test_assert(getMeta(pc, at + "mode")->enumMeta()->items.size() == 2 && g_hooks == 1);
  test_assert(getMeta(pc, at + "amount")->numeric()->hardMax == 1);
  test_assert(!setValue(pc, at + "mode", PropValue::ofString("B")).ok());
  test_assert(setValue(pc, at + "many", PropValue::ofBool(true)).ok());
  test_assert(getMeta(pc, at + "mode")->enumMeta()->items.size() == 3);
  test_assert(setValue(pc, at + "mode", PropValue::ofString("B")).ok());
  test_assert(setValue(pc, at + "amount", PropValue::ofFloat(500)).ok());
  test_assert(*getValue(pc, at + "amount") == PropValue::ofFloat(100));
  test_assert(*getValue(pc, at + "mode") == PropValue::ofEnum(2));
  test_assert(w.run("x.dyn") == "100;#2;\"\";true;");
  test_assert(g_hooks > 4);
  /* And by its index. */
  const CommandRegistry &all = rt.ext<CommandRegistry>();
  for (int i = 0; i < all.size(); i++) {
    if (&all.at(i) == w.def) {
      const std::string item = "commandDefaults[" + std::to_string(i) + "].";
      test_assert(getMeta(pc, item + "mode")->enumMeta()->items.size() == 3);
      test_assert(*getValue(pc, item + "amount") == PropValue::ofFloat(100));
    }
  }
  Result<StableRef> c = canonicalize(pc, *parsePath(at + "mode"));
  test_assert(c.ok() && resolve(pc, *c).ok()); /* keyOf */
}

static void test_layouts()
{
  DynWorld w;
  const auto issues = [&](const char *invocation) {
    const std::string xml =
        "<layout><tool invocation='" + std::string(invocation) + "'/></layout>";
    Result<Layout> layout = parseLayout(xml);
    test_assert(layout.ok());
    litestl::util::Vector<LayoutIssue> out =
        validateLayout(w.s.w.rt, w.s.ctx.root().type, *layout);
    return std::string(out.size() ? out[0].status.message() : "");
  };
  test_assert(issues("x.dyn(amount=2 mode=B text=\"xxxx\" many=true)") == "");
  test_assert(issues("x.dyn(mode=ANY)") == "" && issues("x.dyn(mode=7)") == "");
  /* The kind is checked. */
  test_assert(strstr(issues("x.dyn(amount=\"x\")").c_str(), "expected a number"));
  test_assert(strstr(issues("x.dyn(mode=1.5)").c_str(), "mode: "));
  test_assert(strstr(issues("x.dyn(text=x)").c_str(), "a quoted string"));
  test_assert(strstr(issues("x.dyn(many=x)").c_str(), "true or false"));
}

int main()
{
  test_invocations();
  test_last_used();
  test_defaults();
  test_layouts();
  return test_end();
}
