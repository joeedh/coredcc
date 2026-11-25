/* Layouts checked against structs and commands. */
#include "../command/command_fixture.h"

#include "coredcc/api/ref_text.h"
#include "coredcc/command/last_used.h"
#include "coredcc/layout/builder.h"
#include "coredcc/layout/validate.h"
#include "coredcc/layout/xml.h"

#include <chrono>
#include <string>

test_init;

using litestl::util::Vector;

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
  void undo(Context &) {}
  uint64_t memSize() const
  {
    return 0;
  }
  void discard() {}
  static lbt::Struct<Nothing> *defineBindings()
  {
    auto *st = new lbt::Struct<Nothing>("test::Nothing", sizeof(Nothing));
    BIND_COMMAND_CORE(st);
    return st;
  }
};

const StructId kRoot = StructId::of("LayoutRoot");

struct LayoutWorld {
  Setup s;
  Runtime &rt = s.w.rt;
  LayoutWorld()
  {
    test_assert(addNativeCommand<Nothing>(
                    rt, CommandDef("test.take")
                            .input(PropertyDef::pointerProp("any", StructId()))
                            .input(PropertyDef::pointerProp("base", kIdBase))
                            .input(PropertyDef::pointerProp("brush", kBrush))
                            .input(PropertyDef::intProp("amount"))
                            .input(PropertyDef::vecProp("offset", 3))
                            .input(PropertyDef::enumProp("mode").item("FAST", 0).item("SLOW", 1)))
                    .ok());
    /* What the fixture's scene has, the last-used inputs, and a pointer of no known type. */
    ApiStruct root("LayoutRoot");
    root.parent(kScene);
    rt.ext<LastUsedStore>().addCollectionTo(root);
    PointerAccessor gone;
    gone.get = [](const PointerRef &, Context &) { return PointerRef(); };
    gone.target = StructId::of("Gone");
    root.addPointer(PropertyDef::pointerProp("gone", StructId::of("Gone")), std::move(gone));
    /* A struct that 40 inherit from, each with a pointer to it. */
    for (int i = -1; i < 40; i++) {
      ApiStruct link(i < 0 ? std::string("Link") : "Link" + std::to_string(i));
      PointerAccessor next;
      next.get = [](const PointerRef &, Context &) { return PointerRef(); };
      next.target = StructId::of("Link");
      link.addPointer(PropertyDef::pointerProp(i < 0 ? "n" : "m", next.target), std::move(next));
      if (i >= 0) {
        link.parent(StructId::of("Link"));
      }
      test_assert(rt.ext<ApiRegistry>().add(std::move(link)).ok());
    }
    PointerAccessor first;
    first.get = [](const PointerRef &, Context &) { return PointerRef(); };
    first.target = StructId::of("Link");
    root.addPointer(PropertyDef::pointerProp("link", first.target), std::move(first));
    test_assert(rt.ext<ApiRegistry>().add(std::move(root)).ok());
  }
  Vector<LayoutIssue> issues(const std::string &xml)
  {
    Result<Layout> l = parseLayout(xml);
    if (!l.ok()) {
      fprintf(stderr, "%s\n", l.status().message());
    }
    test_assert(l.ok());
    return validateLayout(rt, kRoot, *l);
  }
  bool fine(const std::string &inner, const std::string &path = "")
  {
    const std::string xml =
        (path.empty() ? "<layout>" : "<layout path='" + path + "'>") + inner + "</layout>";
    const Vector<LayoutIssue> found = issues(xml);
    for (const LayoutIssue &i : found) {
      fprintf(stderr, "%s\n%s\n", xml.c_str(), i.status.message());
    }
    return found.size() == 0;
  }
  /* One issue, at `at` in the text. */
  bool one(const std::string &inner,
           const std::string &what,
           const std::string &at,
           LayoutSeverity severity = LayoutSeverity::Error,
           const std::string &path = "")
  {
    const std::string xml =
        (path.empty() ? "<layout>" : "<layout path='" + path + "'>") + inner + "</layout>";
    const Vector<LayoutIssue> found = issues(xml);
    const bool ok = found.size() == 1 && what == found[0].status.message() &&
                    xml.compare(found[0].offset, at.size(), at) == 0 &&
                    found[0].severity == severity &&
                    layoutIsValid(found) == (severity == LayoutSeverity::Warning);
    if (!ok) {
      fprintf(stderr, "%s\nwant %s\n", xml.c_str(), what.c_str());
      for (const LayoutIssue &i : found) {
        fprintf(stderr, "got  %s at %s\n", i.status.message(), xml.substr(i.offset, 20).c_str());
      }
    }
    return ok;
  }
  std::string suggestion(const std::string &inner)
  {
    const Vector<LayoutIssue> found = issues("<layout>" + inner + "</layout>");
    test_assert(found.size() == 1);
    return found.size() && found[0].status.suggestions().size() ?
               found[0].status.suggestions()[0].c_str() :
               "";
  }
};

const char *const kUnknown =
    "it can't be checked: it goes through a struct that isn't registered, or names a key that only "
    "a dynamic def may have";

} // namespace

static void test_props()
{
  LayoutWorld w;
  for (const char *path : {"tool.strength", "tool", "objects[0].location", "objects[0].location[2]",
                           "objects.active.flags[HIDE]", "objects.length", "objects[0].data",
                           "objects[0].anyData.radius", "/tool.strength",
                           "commandDefaults['test.add'].amount"})
  {
    test_assert(w.fine(std::string("<prop path=\"") + path + "\"/>"));
  }
  test_assert(w.one("<prop path='objects'/>", "<prop> path: 'objects' isn't a property", "path="));
  test_assert(w.one("<prop path='objects[0]'/>", "<prop> path: 'objects[0]' isn't a property",
                    "path="));
  test_assert(w.one("<prop id='a' path='tool.strenth'/>",
                    "<prop> path: no member 'strenth' in Tool (at offset 4)", "path="));
  test_assert(w.suggestion("<prop path='tool.strenth'/>") == "strength");
  test_assert(w.one("<prop path='gone.x'/>", std::string("<prop> path: ") + kUnknown, "path=",
                    LayoutSeverity::Warning));
  test_assert(w.one("<prop path='commandDefaults[\"test.ad\"].amount'/>",
                    "<prop> path: no command 'test.ad' (at offset 15)", "path="));
  test_assert(w.suggestion("<prop path='commandDefaults[\"test.ad\"].amount'/>") == "test.add");
  test_assert(w.one("<prop path='commandDefaults[\"test.add\"].other'/>",
                    "<prop> path: no member 'other' in CommandDefaults:test.add (at offset 27)",
                    "path="));
}

static void test_prefixes()
{
  LayoutWorld w;
  test_assert(w.fine("<prop path='location'/>", "objects.active"));
  test_assert(w.fine("<row path='data'><prop path='radius'/></row>", "objects.active"));
  test_assert(w.fine("<row path='/tool'><prop path='strength'/><row path='/'>"
                     "<prop path='tool'/></row></row>",
                     "objects.active"));
  test_assert(w.fine("<row><column><prop path='mode'/></column></row><prop path='/tool.locked'/>",
                     "objects.active"));
  /* A mistake in a prefix is one issue. */
  test_assert(w.one("<row path='nothing'><prop path='a'/><row path='b' visibleIf='c'>"
                    "<tool invocation=\"test.take(any='a')\"/></row></row>",
                    "<row> path: no member 'nothing' in LayoutRoot (at offset 0)", "path="));
  test_assert(w.one("<row path='tool.strength'><prop path='a'/></row>",
                    "<row> path: 'tool.strength' isn't a struct", "path="));
  test_assert(w.issues("<layout><row path='nothing'><prop path='/tool.nothing'/></row></layout>")
                  .size() == 2);
  /* Outside of it things go on. */
  test_assert(w.one("<row path='nothing'/><prop path='tool.strength'/>",
                    "<row> path: no member 'nothing' in LayoutRoot (at offset 0)", "path="));
  /* An unknown one is a warning, once. */
  test_assert(w.one("<row path='gone'><prop path='a'/><row path='b.c' enabledIf='d'>"
                    "<tool invocation=\"test.take(brush='a')\"/></row></row>",
                    std::string("<row> path: ") + kUnknown, "path=", LayoutSeverity::Warning));
  /* A custom element's path can be anything; what is in it isn't. */
  test_assert(w.fine("<custom tag='x' path='tool.strength'/><custom tag='x' path='objects'/>"
                     "<custom tag='x' path='tool'><prop path='strength'/></custom>"));
  test_assert(w.one("<custom tag='x' path='tool.nothing'/>",
                    "<custom> path: no member 'nothing' in Tool (at offset 4)", "path="));
  test_assert(w.fine("<custom tag='x' path='tool.strength'><label>a</label></custom>"));
}

static void test_conditions()
{
  LayoutWorld w;
  test_assert(w.fine("<row visibleIf='tool.locked' enabledIf='!objects.active.mode[EDIT]'/>"));
  test_assert(w.fine("<row path='data' visibleIf='!/tool.locked' enabledIf='flags[HIDE]'/>",
                     "objects.active"));
  for (const char *path : {"tool.strength", "objects", "objects[0]", "objects[0].location", "objects[0].location[0]"})
  {
    test_assert(w.one("<sep visibleIf='!" + std::string(path) + "'/>",
                      "<separator> visibleIf: '" + std::string(path) +
                          "' isn't a Bool, an enum's key, a pointer, a length or a String",
                      "visibleIf="));
  }
  test_assert(w.one("<sep enabledIf='tool.x'/>",
                    "<separator> enabledIf: no member 'x' in Tool (at offset 4)", "enabledIf="));
  test_assert(w.one("<sep enabledIf='gone.x'/>", std::string("<separator> enabledIf: ") + kUnknown,
                    "enabledIf=", LayoutSeverity::Warning));
}

static void test_tools()
{
  LayoutWorld w;
  const auto tool = [](const std::string &args) {
    return "<tool invocation=\"test.take(" + args + ")\"/>";
  };
  test_assert(w.fine("<tool invocation='test.add'/><toolOptions target='test.add'/>"));
  test_assert(w.fine(tool("amount=3 offset=(1,2,3) mode=FAST")));
  test_assert(w.one("<tool invocation='test.ad'/>",
                    "<tool> invocation: no command 'test.ad'", "invocation="));
  test_assert(w.suggestion("<tool invocation='test.ad'/>") == "test.add");
  /* A pointer's path from the root, whatever is around it. */
  test_assert(w.fine(tool("any='/objects[0]'"), "tool"));
  test_assert(w.one(tool("brush='/tool.brushes[0]'"), std::string("<layout> path: ") + kUnknown,
                    "path=", LayoutSeverity::Warning, "gone"));
  test_assert(w.one(tool("base='/objcts[0]'"), std::string("<tool> invocation: ") +
                    "no member 'objcts' in LayoutRoot (at offset 1)", "invocation=",
                    LayoutSeverity::Error, "tool"));
  /* Also where what is around it names nothing, which is another issue. */
  test_assert(w.issues("<layout><row path='nope'>" + tool("any='/objcts[0]'") + "</row></layout>").size() == 2);
  test_assert(w.issues("<layout><row path='nope'>" + tool("any='objcts[0]'") + "</row></layout>").size() == 1);
  /* A tool's own path is the prefix of its arguments. */
  test_assert(w.fine("<tool path='tool' invocation=\"test.take(brush='brushes[0]')\"/>"));
  test_assert(w.one("<tool path='tool' invocation=\"test.take(brush='objects[0]')\"/>",
                    "<tool> invocation: no member 'objects' in Tool (at offset 0)", "invocation="));
  test_assert(w.fine("<tool path='/' invocation=\"test.take(any='objects[0]')\"/>", "tool"));
  test_assert(w.one("<tool path='nope' invocation=\"test.take(brush='gone')\"/>",
                    "<tool> path: no member 'nope' in LayoutRoot (at offset 0)", "path="));
  test_assert(w.one("<tool path='tool.count' invocation='test.add'/>",
                    "<tool> path: 'tool.count' isn't a struct", "path="));
  /* Conditions. */
  test_assert(w.fine("<row visibleIf='objects.length'/><row visibleIf='!tool.activeBrush'/>"
                     "<row visibleIf='objects.active.name'/>"));
  const char *const want = "' isn't a Bool, an enum's key, a pointer, a length or a String";
  test_assert(w.one("<row visibleIf='tool.count'/>", std::string("<row> visibleIf: 'tool.count") + want,
                    "visibleIf="));
  test_assert(w.one("<row visibleIf='objects'/>", std::string("<row> visibleIf: 'objects") + want,
                    "visibleIf="));
  /* The active item is what a pointer says. */
  test_assert(w.one("<prop path='objects.active'/>",
                    "<prop> path: 'objects.active' isn't a property: use the pointer that holds the "
                    "active item", "path="));
  test_assert(w.fine("<prop path='tool.activeBrush'/><row path='objects.active'/>"));
  /* What was skipped. */
  {
    const std::string xml = "<layout version='2'><row x='1'/><new/><prop path='nope'/></layout>";
    const Vector<LayoutIssue> found = w.issues(xml);
    test_assert(found.size() == 3 && found[0].severity == LayoutSeverity::Warning && found[0].node == 1 &&
                xml.compare(found[0].offset, 2, "x=") == 0 && xml.compare(found[1].offset, 4, "<new") == 0);
    test_assert(strcmp(found[1].status.message(),
                       "<layout> skipped, since it is of a newer version: there is no element <new>") == 0);
    test_assert(found[2].severity == LayoutSeverity::Error && !layoutIsValid(found));
  }
  /* What isn't for a button that invokes it, or not for users. */
  test_assert(w.one("<tool invocation='test.drag'/>",
                    "<tool> invocation: 'test.drag' is interactive: it is begun with "
                    "beginInteractive, not invoked",
                    "invocation=", LayoutSeverity::Warning));
  test_assert(w.fine("<toolOptions target='test.drag'/>"));
  test_assert(addNativeCommand<Nothing>(w.rt, CommandDef("test.own").flags(CommandFlags::Private)).ok());
  const char *priv = "'test.own' is private: it isn't for users";
  test_assert(w.one("<tool invocation='test.own'/>", std::string("<tool> invocation: ") + priv,
                    "invocation=", LayoutSeverity::Warning));
  test_assert(w.one("<toolOptions target='test.own'/>", std::string("<toolOptions> target: ") + priv,
                    "target=", LayoutSeverity::Warning));
  /* A root without the last-used inputs. */
  const Vector<LayoutIssue> bare =
      validateLayout(w.rt, kScene, *parseLayout("<layout><toolOptions target='test.add'/></layout>"));
  test_assert(bare.size() == 1 && bare[0].severity == LayoutSeverity::Error &&
              strstr(bare[0].status.message(),
                     "the root needs LastUsedStore::addCollectionTo: no member 'commandDefaults'"));
  test_assert(w.one("<toolOptions target='test.ad'/>",
                    "<toolOptions> target: no command 'test.ad'", "target="));
  test_assert(w.one(tool("amout=3"), "<tool> invocation: test.take has no input 'amout' (at offset "
                                     "10)",
                    "invocation="));
  test_assert(w.suggestion(tool("amout=3")) == "amount");
  test_assert(w.one(tool("offset=(1,2)"),
                    "<tool> invocation: offset: expected a tuple of 3 numbers", "invocation="));
  test_assert(w.issues("<layout>" + tool("mode=OTHER") + "</layout>").size() == 1);
  test_assert(w.issues("<layout>" + tool("amount='x' mode=OTHER zzz=1") + "</layout>").size() == 3);
  /* Pointers: under the prefix, and of the input's struct. */
  test_assert(w.fine(tool("any='tool' brush='tool.activeBrush' base='objects[0].data'")));
  test_assert(w.fine(tool("brush='tool.brushes[0]' any='' base=' '")));
  test_assert(w.fine(tool("base='data' any='mod'"), "objects.active"));
  test_assert(w.one(tool("any='tool.strength'"),
                    "<tool> invocation: 'tool.strength' isn't a struct", "invocation="));
  test_assert(w.one(tool("brush='tool'"), "<tool> invocation: brush: 'tool' isn't a Brush",
                    "invocation="));
  test_assert(w.one(tool("any='data'"),
                    "<tool> invocation: no member 'data' in LayoutRoot (at offset 0)",
                    "invocation="));
  test_assert(w.one(tool("brush='gone'"), std::string("<tool> invocation: ") + kUnknown,
                    "invocation=", LayoutSeverity::Warning));
  test_assert(w.fine(tool("any='gone'")));
  StableRef ref;
  ref.owner = OwnerKey::ofBlock(w.s.w.objectId(0));
  ref.type = kMesh;
  std::string text;
  for (char c : formatRef(w.rt, ref)) {
    text += c == '"' ? "&quot;" : std::string(1, c);
  }
  const std::string quoted = "'" + text + "'";
  test_assert(w.fine(tool("any=" + quoted + " base=" + quoted)));
  test_assert(w.issues("<layout>" + tool("brush=" + quoted) + "</layout>").size() == 1);
  test_assert(w.issues("<layout>" + tool("any='\nnonsense'") + "</layout>").size() == 1);
  for (const char *value : {"3", "tool", "('tool','tool')", "('tool')", "true"}) {
    Vector<LayoutIssue> found =
        w.issues("<layout>" + tool(std::string("any=") + value) + "</layout>");
    test_assert(found.size() == 1 &&
                strstr(found[0].status.message(),
                       value[0] == '(' ? "any: expected a single value, not a tuple" :
                                         "any: expected a quoted reference or path"));
  }
  test_assert(w.issues("<layout>" + tool("any='a..b'") + "</layout>").size() == 1);
}

/* What the validator accepts runs. */
static void test_runs()
{
  LayoutWorld w;
  Context ctx = w.s.ctx;
  for (const char *args : {"any='tool' brush='tool.activeBrush' base='objects[0].data'",
                           "any='' base=' '", "amount=3 offset=(1,2,3) mode=FAST"})
  {
    Result<PropertyBag> r = invoke(ctx, "test.take(" + std::string(args) + ")");
    if (!r.ok()) {
      fprintf(stderr, "%s: %s\n", args, r.status().message());
    }
    test_assert(r.ok());
  }
  for (const char *args : {"any='tool.strength'", "brush='tool'", "any='data'", "amout=3",
                           "offset=(1,2)", "mode=OTHER", "any=3", "any='\nnonsense'", "any='a..b'"})
  {
    test_assert(!invoke(ctx, "test.take(" + std::string(args) + ")").ok());
  }
}

static void test_root()
{
  LayoutWorld w;
  Result<Layout> l = parseLayout("<layout><prop path='x'/></layout>");
  Vector<LayoutIssue> found = validateLayout(w.rt, StructId::of("Nowhere"), *l);
  test_assert(found.size() == 1 && !layoutIsValid(found) && found[0].node == 0 &&
              strcmp(found[0].status.message(),
                     "<layout> the root's struct isn't registered") == 0);
  test_assert(validateLayout(w.rt, kRoot, Layout()).size() == 0);
  test_assert(layoutIsValid(Vector<LayoutIssue>()));
  found = w.issues("<layout path='nothing'/>");
  test_assert(found.size() == 1 && found[0].node == 0 && found[0].offset == 8);
  /* A root's own subtypes don't count. */
  l = parseLayout("<layout><prop path='gone'/></layout>");
  test_assert(validateLayout(w.rt, kScene, *l).size() == 1);
}

static void test_many()
{
  LayoutWorld w;
  LayoutBuilder b;
  for (int i = 0; i < 1500; i++) {
    b.root().prop("nothing");
  }
  Vector<LayoutIssue> found = validateLayout(w.rt, kRoot, *b.finish());
  test_assert(int(found.size()) == kMaxLayoutIssues);
  test_assert(strcmp(found[kMaxLayoutIssues - 1].status.message(), "there are more issues") == 0);
  for (int i = 0; i < kMaxLayoutIssues; i++) {
    b.root().prop("nothing");
  }
  found = validateLayout(w.rt, kRoot, *b.finish());
  test_assert(int(found.size()) == kMaxLayoutIssues &&
              strstr(found[kMaxLayoutIssues - 1].status.message(), "nothing"));
  /* The largest layouts in time. */
  for (int i = 0; i < 16000; i++) {
    b.root().prop("objects.active.anyData.radius").visibleIf("objects[0].mode[EDIT]");
  }
  const Layout large = *b.finish();
  const auto start = std::chrono::steady_clock::now();
  test_assert(validateLayout(w.rt, kRoot, large).size() == 0);
  test_assert(std::chrono::steady_clock::now() - start < std::chrono::seconds(5));
}

/* The most text there can be. */
static void test_hostile()
{
  LayoutWorld w;
  const auto timed = [&](const std::string &xml) {
    test_assert(xml.size() <= LayoutLimits::kMaxBytes);
    const auto start = std::chrono::steady_clock::now();
    const int n = int(w.issues(xml).size());
    test_assert(std::chrono::steady_clock::now() - start < std::chrono::seconds(20));
    return n;
  };
  std::string path = "link";
  while (path.size() < 1000) {
    path += path.size() % 4 ? ".m" : ".n";
  }
  std::string xml = "<layout>", nested = "<layout>", bad = "<layout>";
  while (xml.size() < 1000 * 1000) {
    xml += "<prop path='" + path + "'/>";
    bad += "<prop path='" + path + ".x'/>";
  }
  test_assert(timed(xml + "</layout>") == 0);
  test_assert(timed(bad + "</layout>") > 900);
  for (int i = 0; i < 60; i++) {
    nested += "<row path='" + std::string(i ? "m.n.m" : "link") + "'>";
  }
  while (nested.size() < 200 * 1000) {
    nested += "<sep visibleIf='m.m.m.m.m.m.m.m.m.m.m.m.x'/>";
  }
  for (int i = 0; i < 60; i++) {
    nested += "</row>";
  }
  test_assert(timed(nested + "</layout>") == kMaxLayoutIssues);
  std::string tool = "<layout><tool invocation=\"test.take(";
  for (int i = 0; tool.size() < 1000 * 1000; i++) {
    tool += "a" + std::to_string(i) + "=1 ";
  }
  test_assert(timed(tool + ")\"/></layout>") == kMaxLayoutIssues);
}

int main()
{
  test_props();
  test_prefixes();
  test_conditions();
  test_tools();
  test_runs();
  test_root();
  test_many();
  test_hostile();
  return test_end();
}
