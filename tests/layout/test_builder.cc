/* Layouts built in C++: the same as parsed ones, and the same checks. */
#include "coredcc/layout/builder.h"
#include "coredcc/layout/xml.h"

#include "litestl/tests/test_util.h"

#include <cstdio>
#include <cstring>
#include <functional>
#include <string>

test_init;

using namespace coredcc;

using Id = LayoutAttrId;

static Layout parsed(std::string_view xml)
{
  Result<Layout> r = parseLayout(xml);
  test_assert(r.ok());
  return r.ok() ? std::move(*r) : Layout();
}

static bool fails(const std::function<void(LayoutBuilder &)> &build,
                  const std::string &what,
                  StatusCode code = StatusCode::InvalidArgument)
{
  LayoutBuilder b;
  build(b);
  Result<Layout> r = b.finish();
  const bool ok = !r.ok() && r.status().code() == code && what == r.status().message();
  if (!ok) {
    fprintf(stderr,
            "got  %s\nwant %s\n",
            r.ok() ? "a layout" : r.status().message(),
            what.c_str());
  }
  return ok;
}

static void test_same()
{
  LayoutBuilder b;
  LayoutScope root = b.root();
  root.path("objects.active").id("root");
  LayoutScope panel = root.panel("Transform").path("transform");
  panel.setFlag(Id::Closed, true).set("icon", "move");
  LayoutScope tabs = root.tabs();
  tabs.tab("One").label("text");
  panel.prop("location").visibleIf("!locked").enabledIf("a");
  LayoutScope row = panel.row();
  row.separator();
  row.column().setFlag(Id::Expand, false);
  panel.tool("a.b(c=1)", "Run").setFlag(Id::IconsOnly, true);
  panel.tool("a.b");
  panel.toolOptions("a.b");
  tabs.tab("Two");
  LayoutScope custom =
      root.custom("host").set("size", "3").set("b", "x").set("label", "L");
  custom.custom("inner").set(Id::Text, "text\n here");
  row.prop("last").set(Id::Widget, "slider");
  test_assert(root.index() == 0 && panel.index() == 1);
  Result<Layout> built = b.finish();
  test_assert(built.ok());
  const Layout same = parsed(R"x(<layout id="root" path="objects.active">
    <panel closed="true" label="Transform" path="transform" icon="move">
      <prop path="location" enabledIf="a" visibleIf="!locked"/>
      <row><sep/><column expand="false"/><prop path="last" widget="slider"/></row>
      <tool invocation="a.b(c=1)" iconsOnly="true">Run</tool><tool invocation="a.b"/>
      <toolOptions target="a.b"/>
    </panel>
    <tabs><tab label="One"><label>text</label></tab><tab label="Two"/></tabs>
    <custom b="x" tag="host" label="L" size="3"><inner>text
 here</inner></custom></layout>)x");
  if (!(*built == same)) {
    fprintf(stderr, "%s\n%s\n", layoutToJson(*built).c_str(), layoutToJson(same).c_str());
  }
  test_assert(*built == same && layoutToJson(*built) == layoutToJson(same));
  test_assert(built->node(1).offset == 0);
  /* The builder starts again. */
  test_assert(b.finish().ok() && b.finish()->nodeCount() == 1);
  /* An attribute that is set again. */
  b.root().set(Id::Path, "bad.").path("a").set("path", "b").id("i");
  test_assert(*b.finish() == parsed("<layout path='b' id='i'/>"));
}

/* Every difference counts. */
static void test_differs()
{
  const char *const xmls[] = {
      "<layout/>",
      "<layout path='a'/>",
      "<layout path='b'/>",
      "<layout id='a'/>",
      "<layout><row/></layout>",
      "<layout><column/></layout>",
      "<layout><row/><row/></layout>",
      "<layout><row><row/></row></layout>",
      "<layout><row id='a'/><row/></layout>",
      "<layout><row/><row id='a'/></layout>",
      "<layout><custom tag='a' b='c'/></layout>",
      "<layout><custom tag='a' d='c'/></layout>",
      "<layout><custom tag='a'>c</custom></layout>",
  };
  for (const char *a : xmls) {
    for (const char *b : xmls) {
      test_assert((parsed(a) == parsed(b)) == (a == b));
    }
  }
}

/* From a layout. */
static void test_edit()
{
  const char *const xml =
      R"(<layout version="3" path="a"><panel id="p" label="P" data-x="1"><prop path="b"/></panel>
    <custom tag="t" c="d">text</custom><label id="l">L</label></layout>)";
  const Layout from = parsed(xml);
  LayoutBuilder same(from);
  test_assert(*same.finish() == from);
  test_assert(*same.finish() == Layout()); /* it starts again, empty */
  LayoutBuilder b(from);
  test_assert(b.findId("p") == 1 && b.findId("l") == 4 && b.findId("x") == -1 &&
              b.findId("") == -1);
  b.at(1).prop("c").id("x");
  test_assert(b.findId("x") == 5);
  b.at(4).set(Id::Label, "M");
  b.at(1).set("data-y", "2");
  b.root().row();
  Result<Layout> out = b.finish();
  test_assert(out.ok() && *out == parsed(R"(<layout version="3" path="a">
    <panel id="p" label="P" data-x="1" data-y="2"><prop path="b"/><prop path="c" id="x"/></panel>
    <custom tag="t" c="d">text</custom><label id="l">M</label><row/></layout>)"));
  test_assert(from.node(4).kind == LayoutKind::Label && from == parsed(xml));
  /* Its bytes count. */
  LayoutBuilder big;
  big.root().label(std::string(LayoutLimits::kMaxBytes - 100, 'x'));
  LayoutBuilder more(*big.finish());
  more.root().label(std::string(50, 'x'));
  LayoutBuilder tooMuch(*more.finish());
  tooMuch.root().label(std::string(100, 'x'));
  test_assert(tooMuch.finish().status().code() == StatusCode::OutOfRange);
}

static void test_refuses()
{
  test_assert(fails([](LayoutBuilder &b) { b.root().row().set("x", "1"); },
                    "layout: node 1 (row): there is no attribute 'x'"));
  test_assert(fails([](LayoutBuilder &b) { b.root().set(Id::Version, "v"); },
                    "layout: node 0 (layout): version: it isn't a number from 1"));
  LayoutBuilder bad;
  bad.root().row().prop("a..b");
  Result<Layout> r = bad.finish();
  test_assert(
      strncmp(r.status().message(), "layout: node 2 (prop): path: expected", 37) == 0);
  test_assert(fails([](LayoutBuilder &b) { b.root().prop("a").row(); },
                    "layout: node 2 (row): a <prop> can't hold elements"));
  test_assert(fails([](LayoutBuilder &b) { b.root().add(LayoutKind::Layout); },
                    "layout: node 1 (layout): only the root is a <layout>"));
  test_assert(fails([](LayoutBuilder &b) { b.root().tab("a"); },
                    "layout: node 1 (tab): a <tab> must be in a <tabs>"));
  test_assert(fails([](LayoutBuilder &b) { b.root().label(""); },
                    "layout: node 1 (label): a <label> needs text"));
  test_assert(fails([](LayoutBuilder &b) { b.root().add(LayoutKind::Tool); },
                    "layout: node 1 (tool): a <tool> needs 'invocation'"));
  test_assert(fails([](LayoutBuilder &b) { b.root().row().setFlag(Id::Closed, true); },
                    "layout: node 1 (row): a <row> has no 'closed'"));
  test_assert(
      fails([](LayoutBuilder &b) { b.root().panel("a").set(Id::Closed, "maybe"); },
            "layout: node 1 (panel): closed: it is neither 'true' nor 'false'"));
  test_assert(fails([](LayoutBuilder &b) { b.root().set(Id::Free, "x"); },
                    "layout: node 0 (layout): a free attribute is set by its name"));
  test_assert(fails([](LayoutBuilder &b) { b.root().custom("a").set("b c", "x"); },
                    "layout: node 1 (custom): 'b c' isn't an attribute's name"));
  test_assert(fails([](LayoutBuilder &b) { b.root().custom("a").set("", "x"); },
                    "layout: node 1 (custom): '' isn't an attribute's name"));
  test_assert(
      fails([](LayoutBuilder &b) { b.root().custom("a").set(Id::Text, "x").row(); },
            "layout: node 2 (row): text and elements can't be mixed"));
  test_assert(fails([](LayoutBuilder &b) { b.root().label(std::string_view("a\0b", 3)); },
                    "layout: node 1 (label): label: a NUL character"));
  test_assert(fails([](LayoutBuilder &b) { b.root().custom("a").set("b", "\xFF"); },
                    "layout: node 1 (custom): b: the text isn't valid UTF-8"));
  /* Names, with suggestions; the first mistake counts. */
  LayoutBuilder b;
  b.root().row().set("pathh", "x").set("nothing", "x");
  r = b.finish();
  test_assert(strcmp(r.status().message(),
                     "layout: node 1 (row): there is no attribute 'pathh'") == 0);
  test_assert(r.status().suggestions().size() == 1 &&
              strcmp(r.status().suggestions()[0].c_str(), "path") == 0);
  test_assert(b.finish().ok());
  b.root().set(Id::Free, "x");
  b.root().row().set(Id::Free, "x").set("pathh", "x");
  r = b.finish();
  test_assert(strstr(r.status().message(), "layout: node 0 (layout): a free attribute"));
  test_assert(r.status().suggestions().size() == 0);
}

static void test_limits()
{
  const StatusCode over = StatusCode::OutOfRange;
  const auto rows = [](LayoutBuilder &b, int n) {
    LayoutScope s = b.root();
    for (int i = 0; i < n; i++) {
      s = s.row();
    }
  };
  LayoutBuilder b;
  rows(b, 63);
  test_assert(b.finish().ok());
  test_assert(fails([&](LayoutBuilder &c) { rows(c, 64); },
                    "layout: node 64 (row): the elements are nested more than 64 deep",
                    over));
  const auto many = [](LayoutBuilder &c, int n) {
    for (int i = 0; i < n; i++) {
      c.root().separator().set(Id::Class, "a");
    }
  };
  many(b, 16383);
  test_assert(b.finish()->nodeCount() == 16384);
  for (int n : {16384, 20000}) {
    test_assert(
        fails([&](LayoutBuilder &c) { many(c, n); },
              "layout: node 16384 (separator): there are more than 16384 elements",
              over));
  }
  const auto attrs = [](LayoutBuilder &c, int n) {
    LayoutScope s = c.root().custom("t");
    for (int i = 1; i < n; i++) {
      s.set("a" + std::to_string(i), "");
    }
  };
  attrs(b, 64);
  test_assert(b.finish()->node(1).attrCount == 64);
  for (int n : {65, 200}) {
    test_assert(fails([&](LayoutBuilder &c) { attrs(c, n); },
                      "layout: node 1 (custom): an element has more than 64 attributes",
                      over));
  }
  b.root().set(Id::Class, std::string(1 << 20, 'x'));
  test_assert(b.finish().ok());
  test_assert(fails(
      [&](LayoutBuilder &c) { c.root().set(Id::Class, std::string((1 << 20) + 1, 'x')); },
      "layout: node 0 (layout): the attributes are more than 1 MiB of text",
      over));
  test_assert(fails(
      [&](LayoutBuilder &c) {
        for (int i = 0; i < 3; i++) {
          c.root().custom("t").set("name", std::string(400000, 'x'));
        }
      },
      "layout: node 3 (custom): the attributes are more than 1 MiB of text",
      over));
}

int main()
{
  test_same();
  test_differs();
  test_edit();
  test_refuses();
  test_limits();
  return test_end();
}
