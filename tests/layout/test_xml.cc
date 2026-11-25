/* Layouts from XML: what the subset takes, what it refuses and where, prefixes, and JSON. */
#include "coredcc/layout/layout.h"
#include "coredcc/layout/xml.h"

#include "test_util.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

test_init;

using namespace coredcc;

namespace {

bool equals(const std::string &got, const std::string &want)
{
  if (got != want) {
    fprintf(stderr, "got  %s\nwant %s\n", got.c_str(), want.c_str());
  }
  return got == want;
}

Layout parsed(std::string_view xml)
{
  Result<Layout> r = parseLayout(xml);
  if (!r.ok()) {
    fprintf(stderr, "%s\n", r.status().message());
  }
  test_assert(r.ok());
  return r.ok() ? std::move(*r) : Layout();
}

std::string json(std::string_view xml)
{
  return layoutToJson(parsed(xml));
}

/* The error is `what`, at `line` and `column`. */
bool fails(std::string_view xml,
           const std::string &what,
           int line,
           int column,
           StatusCode code = StatusCode::InvalidArgument)
{
  LayoutPosition p;
  Result<Layout> r = parseLayout(xml, &p);
  const std::string want =
      "layout " + std::to_string(line) + ":" + std::to_string(column) + ": " + what;
  const bool ok = !r.ok() && r.status().code() == code && want == r.status().message() &&
                  p.line == line && p.column == column && p.offset <= xml.size();
  if (!ok) {
    fprintf(stderr, "got  %s\nwant %s\n", r.ok() ? "a layout" : r.status().message(), want.c_str());
  }
  return ok;
}

/* ... in the root. */
bool failsIn(const std::string &inner, const std::string &what, int column)
{
  return fails("<layout>" + inner + "</layout>", what, 1, 8 + column);
}

std::string repeated(const char *s, int n)
{
  std::string out;
  for (int i = 0; i < n; i++) {
    out += s;
  }
  return out;
}

} // namespace

static const char *const kExample = R"x(<?xml version="1.0" encoding="utf-8"?>
<!-- Everything there is. -->
<layout path="objects.active" id="root" class="wide dark">
  <panel label="Transform" icon="move" description="Where it is" closed="true" path="transform">
    <prop path="location" label="Loc" widget="vector" expand="true" showLabel="false"/>
    <prop path="/scene.frame" visibleIf="!locked" enabledIf='mode[EDIT]' iconsOnly="false"
          icon="clock" description="d"/>
    <row expand="false"><column expand="true"/><sep/><separator id="s"/></row>
    <tool invocation="object.clear(what='LOC')" iconsOnly="true">Clear &lt;all&gt;</tool>
    <tool invocation="a.b" label="L" icon="i"/>
    <toolOptions target="mesh.extrude" label="Extrude"/>
  </panel>
  <tabs path="/" visibleIf="/a.b[2]">
    <tab label="One" path="x['a\'b']"><label>Some
      text &amp;   more</label></tab>
    <tab label="Two"/>
  </tabs>
  <custom tag="brush.preview" size="32" closed="yes" label="P" path="brush" text="t">
    <brush-tip a.b-c="1"><label icon="i">in it</label></brush-tip>
    <inner> keep
   this </inner>
  </custom>
</layout>
)x";

static void test_golden()
{
  test_assert(equals(
      json(kExample),
      R"x({"kind":"layout","version":1,"path":"objects.active","effectivePath":"/objects.active","id":"root",)x"
      R"x("class":"wide dark","children":[)x"
      R"x({"kind":"panel","path":"transform","effectivePath":"/objects.active.transform",)x"
      R"x("label":"Transform","icon":"move","description":"Where it is","closed":true,"children":[)x"
      R"x({"kind":"prop","path":"location","effectivePath":"/objects.active.transform.location",)x"
      R"x("label":"Loc","widget":"vector","expand":true,"showLabel":false},)x"
      R"x({"kind":"prop","path":"/scene.frame","effectivePath":"/scene.frame","icon":"clock",)x"
      R"x("description":"d","visibleIf":{"path":"/objects.active.transform.locked","not":true},)x"
      R"x("enabledIf":{"path":"/objects.active.transform.mode[EDIT]"},"iconsOnly":false},)x"
      R"x({"kind":"row","expand":false,"children":[{"kind":"column","expand":true},)x"
      R"x({"kind":"separator"},{"kind":"separator","id":"s"}]},)x"
      R"x({"kind":"tool","label":"Clear <all>","invocation":"object.clear(what='LOC')",)x"
      R"x("iconsOnly":true,"prefix":"/objects.active.transform"},)x"
      R"x({"kind":"tool","label":"L","icon":"i","invocation":"a.b",)x"
      R"x("prefix":"/objects.active.transform"},)x"
      R"x({"kind":"toolOptions","label":"Extrude","target":"mesh.extrude",)x"
      R"x("optionsPath":"/commandDefaults[\"mesh.extrude\"]",)x"
      R"x("prefix":"/objects.active.transform"}]},)x"
      R"x({"kind":"tabs","path":"/","effectivePath":"/","visibleIf":{"path":"/a.b[2]"},"children":[)x"
      R"x({"kind":"tab","path":"x['a\\'b']","effectivePath":"/x[\"a'b\"]","label":"One",)x"
      R"x("children":[{"kind":"label","label":"Some text & more"}]},{"kind":"tab","label":"Two"}]},)x"
      R"x({"kind":"custom","path":"brush","effectivePath":"/objects.active.brush","label":"P",)x"
      R"x("tag":"brush.preview","attrs":{"closed":"yes","size":"32","text":"t"},"children":[)x"
      R"x({"kind":"custom","tag":"brush-tip","attrs":{"a.b-c":"1"},"children":[)x"
      R"x({"kind":"label","label":"in it","icon":"i"}]},)x"
      R"x({"kind":"custom","tag":"inner","text":"keep\u000a   this"}]}]})x"));
  test_assert(equals(json("<layout/>"), R"({"kind":"layout","version":1})"));
  test_assert(equals(json("<layout><row path='a'/><tool invocation='x'/></layout>"),
                     R"({"kind":"layout","version":1,"children":[{"kind":"row","path":"a",)"
                     R"("effectivePath":"/a"},{"kind":"tool","invocation":"x"}]})"));
  test_assert(layoutToJson(Layout()) == R"({"kind":"layout","version":1})" &&
              Layout() == parsed("<layout></layout>"));
}

/* Nodes, attributes and offsets. */
static void test_nodes()
{
  const std::string xml = kExample;
  const Layout l = parsed(xml);
  test_assert(l.nodeCount() == 19);
  for (int i = 0; i < l.nodeCount(); i++) {
    const LayoutNode &n = l.node(i);
    test_assert(xml[n.offset] == '<' && (i == 0) == (n.parent < 0) && n.parent < i);
    const std::string_view rest = std::string_view(xml).substr(n.offset + 1);
    const std::string name = n.kind == LayoutKind::Custom ? "" : layoutKindName(n.kind);
    test_assert(rest.substr(0, name.size()) == name || rest.substr(0, 3) == "sep");
    for (uint32_t j = 0; j < n.attrCount; j++) {
      const LayoutAttr &a = l.attrs()[n.attrBegin + j];
      const std::string_view at = std::string_view(xml).substr(a.offset);
      const std::string attr = a.id == LayoutAttrId::Free ? a.name.c_str() : layoutAttrName(a.id);
      test_assert(at.substr(0, attr.size() + 1) == attr + "=" || n.kind == LayoutKind::Tool ||
                  n.kind == LayoutKind::Label || n.kind == LayoutKind::Custom);
      test_assert(j == 0 || l.attrs()[n.attrBegin + j - 1].id <= a.id);
    }
    int children = 0;
    for (int c = n.firstChild; c >= 0; c = l.node(c).nextSibling) {
      test_assert(l.node(c).parent == i);
      children++;
    }
    int want = 0;
    for (int c = 0; c < l.nodeCount(); c++) {
      want += l.node(c).parent == i ? 1 : 0;
    }
    test_assert(children == want);
  }
  const int panel = l.node(0).firstChild;
  test_assert(l.node(panel).kind == LayoutKind::Panel && l.flag(panel, LayoutAttrId::Closed, false));
  test_assert(!l.flag(0, LayoutAttrId::Closed, false) && l.flag(0, LayoutAttrId::ShowLabel, true));
  test_assert(!l.flag(l.node(panel).firstChild, LayoutAttrId::ShowLabel, true));
  test_assert(strcmp(l.text(panel, LayoutAttrId::Icon), "move") == 0);
  test_assert(strcmp(l.text(panel, LayoutAttrId::Widget), "") == 0);
  test_assert(!l.attr(panel, LayoutAttrId::Id) && l.attr(panel, LayoutAttrId::Path));
  test_assert(l.findId("s") > 0 && l.node(l.findId("s")).kind == LayoutKind::Separator);
  test_assert(l.findId("root") == 0 && l.findId("") == -1 && l.findId("sx") == -1);
  const int custom = l.node(l.node(panel).nextSibling).nextSibling;
  test_assert(l.freeAttr(custom, "size") && !l.freeAttr(custom, "tag") && !l.freeAttr(0, "size"));
}

static void test_prefixes()
{
  const Layout l = parsed(kExample);
  const auto find = [&](const char *path) {
    for (int i = 0; i < l.nodeCount(); i++) {
      if (strcmp(l.text(i, LayoutAttrId::Path), path) == 0) {
        return i;
      }
    }
    return -1;
  };
  const auto text = [](const Path &p) { return p.toString(); };
  test_assert(text(l.prefixOf(0)) == "/" && text(l.effectivePath(0)) == "/objects.active");
  const int loc = find("location"), frame = find("/scene.frame");
  test_assert(text(l.prefixOf(loc)) == "/objects.active.transform");
  test_assert(text(l.effectivePath(loc)) == "/objects.active.transform.location");
  test_assert(text(l.effectivePath(frame)) == "/scene.frame");
  LayoutCondition c;
  test_assert(l.condition(frame, LayoutAttrId::VisibleIf, c) && c.negated &&
              text(c.path) == "/objects.active.transform.locked");
  test_assert(l.condition(frame, LayoutAttrId::EnabledIf, c) && !c.negated &&
              text(c.path) == "/objects.active.transform.mode[EDIT]");
  test_assert(!l.condition(loc, LayoutAttrId::EnabledIf, c));
  const int tabs = find("/");
  test_assert(l.effectivePath(tabs).size() == 0);
  test_assert(l.condition(tabs, LayoutAttrId::VisibleIf, c) && text(c.path) == "/a.b[2]");
  const int tab = find("x['a\\'b']");
  /* A label has no path of its own. */
  test_assert(text(l.effectivePath(l.node(tab).firstChild)) == "/x[\"a'b\"]");
  size_t at = 0;
  Path prefix = *parsePath("a.b");
  test_assert(text(*joinLayoutPath(prefix, "c[1]")) == "a.b.c[1]");
  test_assert(text(*joinLayoutPath(prefix, "/c")) == "/c" &&
              joinLayoutPath(prefix, "/")->size() == 0);
  test_assert(!joinLayoutPath(prefix, "c.", &at).ok() && at == 2);
  test_assert(!joinLayoutPath(prefix, "/c.", &at).ok() && at == 3);
  test_assert(!joinLayoutPath(prefix, "", &at).ok() && !joinLayoutPath(prefix, "//c").ok());
  test_assert(!joinLayoutPath(prefix, "[0]").ok());
  /* A condition of a container is outside of its own prefix. */
  const Layout own = parsed(R"(<layout path="a"><row path="b" visibleIf="c"/></layout>)");
  test_assert(own.condition(1, LayoutAttrId::VisibleIf, c) && text(c.path) == "/a.c");
}

/* What is XML here, and isn't a mistake. */
static void test_accepts()
{
  const Layout empty;
  for (const char *xml : {"\xEF\xBB\xBF<layout/>", " \t\r\n<layout />\n", "<layout></layout  >",
                          "<!----><layout><!-- <x> & --></layout><!-->-->",
                          "<?xml version='1.0'?><layout/>",
                          "\xEF\xBB\xBF<?xml version = \"1.0\" encoding='UTF-8' standalone='yes' ?>"
                          "<layout\n/>"})
  {
    test_assert(parsed(xml) == empty);
  }
  const auto label = [](const std::string &inner) {
    const Layout l = parsed("<layout><label" + inner + "</label></layout>");
    return std::string(l.text(1, LayoutAttrId::Label));
  };
  test_assert(label(">a<!-- b -->c") == "ac");
  test_assert(label(">\r\n a\r\tb \n") == "a b");
  test_assert(label(">&lt;&gt;&amp;&quot;&apos;") == "<>&\"'");
  test_assert(label(">&amp;lt;") == "&lt;" && label(">]] >]>") == "]] >]>");
  test_assert(label(" label='x &amp; &apos;\"'> ") == "x & '\"");
  test_assert(label(" label='a\tb\r\nc\nd\re  f'>") == "a b c d e  f");
  test_assert(label(">\xC3\xA9\xF0\x9F\x98\x80") == "\xC3\xA9\xF0\x9F\x98\x80");
  test_assert(label(" label = \"a > b\">") == "a > b");
  const Layout c = parsed("<layout><custom tag='t'> \r\n a\r\n  b\rc\t</custom></layout>");
  test_assert(equals(c.text(1, LayoutAttrId::Text), "a\n  b\nc"));
  test_assert(!parsed("<layout><custom tag='t'> \n</custom></layout>").attr(1, LayoutAttrId::Text));
  /* Kinds keep their meaning in a custom element. */
  const Layout in = parsed("<layout><custom tag='t'><row/><x><y/></x></custom></layout>");
  test_assert(in.node(2).kind == LayoutKind::Row && in.node(3).kind == LayoutKind::Custom);
  test_assert(strcmp(in.text(4, LayoutAttrId::Tag), "y") == 0);
}

static void test_refuses()
{
  /* The document. */
  test_assert(fails("", "there is no <layout>", 1, 1));
  test_assert(fails(" <!-- x -->\n", "there is no <layout>", 2, 1));
  test_assert(fails("x<layout/>", "there is text outside of the <layout>", 1, 1));
  test_assert(fails("<layout/>\n x", "there is text outside of the <layout>", 2, 2));
  test_assert(fails("<layout/>&amp;", "there is text outside of the <layout>", 1, 10));
  test_assert(fails("<layout/><layout/>", "there is more than one root element", 1, 10));
  test_assert(fails("<row/>", "the root isn't a <layout>", 1, 1));
  test_assert(failsIn("<layout/>", "only the root is a <layout>", 1));
  test_assert(fails("\n<layout><row>\n", "the <row> in line 2 isn't closed", 3, 1));
  test_assert(failsIn("<row></rows>", "</rows> can't close <row>", 6));
  test_assert(failsIn("<row></Row></row>", "</Row> can't close <row>", 6));
  test_assert(fails("</layout>", "there is nothing that </layout> could close", 1, 1));
  test_assert(fails("<layout/></layout>", "there is nothing that </layout> could close", 1, 10));
  /* What isn't in the subset. */
  const char *const other = "DOCTYPE and other declarations aren't supported";
  test_assert(fails("<!DOCTYPE layout><layout/>", other, 1, 1));
  test_assert(fails("<!DOCTYPE l [<!ENTITY a \"b\">]><layout/>", other, 1, 1));
  test_assert(failsIn("<![CDATA[x]]>", "CDATA isn't supported", 1));
  test_assert(failsIn("<?php x ?>", "processing instructions aren't supported", 1));
  test_assert(failsIn("<?xml version='1.0'?>", "an <?xml?> declaration must come first", 1));
  test_assert(fails(" <?xml version='1.0'?><layout/>", "an <?xml?> declaration must come first", 1,
                    2));
  test_assert(failsIn("<a:b/>", "namespaces aren't supported", 3));
  test_assert(failsIn("<row a:b='c'/>", "namespaces aren't supported", 7));
  test_assert(failsIn("<row xmlns='c'/>", "namespaces aren't supported", 6));
  const std::string reference = "numeric references aren't supported: write the character itself";
  test_assert(failsIn("<label>a&#65;</label>", reference, 9));
  test_assert(failsIn("<label label='&#x41;'/>", reference, 15));
  const std::string entity = "'&' must start one of &lt; &gt; &amp; &quot; &apos;";
  test_assert(failsIn("<label>&nbsp;</label>", entity, 8));
  test_assert(failsIn("<label>& b</label>", entity, 8) && failsIn("<label>&lt</label>", entity, 8));
  test_assert(failsIn("<label label='a&b;'/>", entity, 16));
  test_assert(fails("<layout>&", "a <layout> can't hold text: write it in a <label>", 1, 9));
  /* Malformed. */
  test_assert(failsIn("<row a='1'b='2'/>", "there is no attribute 'a'", 6));
  test_assert(failsIn("<row id='1'class='2'/>", "expected '>' or an attribute, not 'c'", 12));
  test_assert(failsIn("<row id/>", "expected '=' after 'id', not '/'", 8));
  test_assert(failsIn("<row id=1/>", "expected a quote, not '1'", 9));
  test_assert(fails("<layout id=", "expected a quote, not the end of the text", 1, 12));
  test_assert(fails("<layout id='x></layout>", "'<' in a value is written &lt;", 1, 15));
  test_assert(fails("<layout id='x>", "the value isn't closed", 1, 12));
  test_assert(fails("<layout", "expected '>' or an attribute, not the end of the text", 1, 8));
  test_assert(fails("<layout /", "expected the name of an attribute, not '/'", 1, 9));
  test_assert(failsIn("< row/>", "expected the name of an element, not ' '", 2));
  test_assert(failsIn("<\xC3\xA9/>", "expected the name of an element, not '\xC3\xA9'", 2));
  test_assert(failsIn("<1/>", "expected the name of an element, not '1'", 2));
  test_assert(failsIn("<row></row x>", "expected '>', not 'x'", 12));
  test_assert(failsIn("<row></ row>", "expected the name of an element, not ' '", 8));
  test_assert(fails("<", "expected the name of an element, not the end of the text", 1, 2));
  test_assert(failsIn("<!-- a", "the comment isn't closed", 1));
  test_assert(failsIn("<!-->", "the comment isn't closed", 1));
  test_assert(failsIn("<!-- a -- b -->", "'--' can't be in a comment", 8));
  test_assert(failsIn("<!-- a --->", "'--' can't be in a comment", 8));
  test_assert(failsIn("<label>a]]>b</label>", "']]>' in text is written ]]&gt;", 9));
  test_assert(fails("<?xml?><layout/>", "the <?xml?> declaration has no version", 1, 6));
  test_assert(fails("<?xml encoding='utf-8'?><layout/>", "the <?xml?> declaration has no version",
                    1, 23));
  test_assert(fails("<?xml version='1.1'?><layout/>", "'version=1.1' isn't supported", 1, 7));
  test_assert(fails("<?xml version='1.0' encoding='latin1'?><layout/>",
                    "'encoding=latin1' isn't supported", 1, 21));
  test_assert(fails("<?xml version='1.0' x='y'?><layout/>", "'x=y' isn't supported", 1, 21));
  test_assert(fails("<?xml version='1.0'x='y'?><layout/>", "expected '?>', not 'x'", 1, 20));
  test_assert(fails("<?xml version='1.0'", "expected '?>', not the end of the text", 1, 20));
  /* Characters. */
  test_assert(fails(std::string_view("<layout>\0</layout>", 18), "a NUL character", 1, 9));
  test_assert(failsIn("\x01", "a control character", 1) && failsIn("\x1F", "a control character", 1));
  test_assert(failsIn("<label>\xFF</label>", "the text isn't valid UTF-8", 8));
  test_assert(failsIn("<!-- \xC3 -->", "the text isn't valid UTF-8", 6));
  test_assert(failsIn("<label>\xEF\xBF\xBF</label>", "U+FFFE and U+FFFF aren't characters", 8));
  test_assert(failsIn("<label>\xEF\xBF\xBE</label>", "U+FFFE and U+FFFF aren't characters", 8));
  test_assert(parsed("<layout><label>\xEF\xBF\xBD</label></layout>").nodeCount() == 2);
}

static void test_refuses_content()
{
  /* Elements and text. */
  test_assert(failsIn("<row>x</row>", "a <row> can't hold text: write it in a <label>", 6));
  test_assert(fails("<layout>\n <prop path='a'>\n &amp;</prop></layout>",
                    "a <prop> can't hold text: write it in a <label>", 3, 2));
  test_assert(failsIn("<prop path='a'><row/></prop>", "a <prop> can't hold elements", 16));
  test_assert(failsIn("<label label='a'><row/></label>", "a <label> can't hold elements", 18));
  test_assert(failsIn("<tool invocation='a'><row/></tool>", "a <tool> can't hold elements", 22));
  test_assert(failsIn("<toolOptions target='a'><row/></toolOptions>",
                      "a <toolOptions> can't hold elements", 25));
  test_assert(failsIn("<sep><row/></sep>", "a <separator> can't hold elements", 6));
  test_assert(failsIn("<tab/>", "a <tab> must be in a <tabs>", 1));
  test_assert(failsIn("<custom tag='x'><tab/></custom>", "a <tab> must be in a <tabs>", 17));
  test_assert(failsIn("<tabs><row/></tabs>", "a <tabs> holds only <tab> elements", 7));
  test_assert(failsIn("<custom tag='x'>a<row/></custom>", "text and elements can't be mixed", 18));
  test_assert(failsIn("<custom tag='x'><row/>a</custom>", "text and elements can't be mixed", 17));
  test_assert(failsIn("<label label=''>a</label>", "there is a label and text", 17));
  test_assert(failsIn("<tool invocation='a' label='b'>c d</tool>", "there is a label and text", 32));
  /* What an element needs. */
  test_assert(failsIn("<prop/>", "a <prop> needs 'path'", 1));
  test_assert(failsIn("<tool>a</tool>", "a <tool> needs 'invocation'", 1));
  test_assert(failsIn("<toolOptions/>", "a <toolOptions> needs 'target'", 1));
  test_assert(failsIn("<label> </label>", "a <label> needs text", 1));
  test_assert(failsIn("<label label=''/>", "a <label> needs text", 1));
  test_assert(failsIn("<custom/>", "a <custom> needs 'tag'", 1));
  /* Attributes. */
  test_assert(failsIn("<prop path='a' path='b'/>", "'path' is there twice", 16));
  test_assert(failsIn("<custom tag='x' a='1' a='1'/>", "'a' is there twice", 23));
  test_assert(failsIn("<custom tag='x'><y tag='z'/></custom>", "the element's name is its tag", 20));
  test_assert(failsIn("<row invocation='b'/>", "a <row> has no 'invocation'", 6));
  test_assert(failsIn("<row text='b'/>", "there is no attribute 'text'", 6));
  test_assert(fails("<layout visibleIf='a'/>", "a <layout> has no 'visibleIf'", 1, 9));
  test_assert(failsIn("<panel closed='yes'/>", "closed: it is neither 'true' nor 'false'", 16));
  test_assert(failsIn("<panel closed=''/>", "closed: it is neither 'true' nor 'false'", 16));
  test_assert(failsIn("<row id=''/>", "id: it is empty", 10));
  test_assert(failsIn("<prop path='a' widget=''/>", "widget: it is empty", 24));
  test_assert(failsIn("<row id='a'/><custom tag='t' id='a'/>", "the id 'a' is used twice", 30));
  /* Paths, at the place in them. */
  const auto offsetOf = [](const std::string &xml) {
    LayoutPosition p;
    test_assert(!parseLayout(xml, &p).ok());
    return p.offset;
  };
  test_assert(offsetOf("<layout path='a..b'/>") == 16);
  test_assert(offsetOf("<layout path='/a..b'/>") == 17);
  test_assert(offsetOf("<layout path=''/>") == 14);
  test_assert(offsetOf("<layout><prop path='a[&lt;]'/></layout>") == 20); /* just the value */
  test_assert(offsetOf("<layout><row visibleIf='!a.'/></layout>") == 27);
  test_assert(offsetOf("<layout><row visibleIf='!'/></layout>") == 25);
  test_assert(offsetOf("<layout><row enabledIf=''/></layout>") == 24);
  test_assert(offsetOf("<layout><row enabledIf='!!a'/></layout>") == 25);
  test_assert(offsetOf("<layout><tool invocation='a.b(c='/></layout>") == 32);
  test_assert(!parseLayout("<layout><custom tag='a' path='.'/></layout>").ok());
  Result<Layout> r = parseLayout("<layout path='a.'/>");
  test_assert(strstr(r.status().message(), "layout 1:17: path: expected a member name"));
}

static void test_suggestions()
{
  const auto first = [](const char *xml) {
    Result<Layout> r = parseLayout(xml);
    test_assert(!r.ok());
    return std::string(r.status().suggestions().size() ? r.status().suggestions()[0].c_str() : "");
  };
  test_assert(first("<layout><pannel/></layout>") == "panel");
  test_assert(first("<layout><zzzzz/></layout>").empty());
  test_assert(first("<layout><prop path='a' lable='b'/></layout>") == "label");
  /* Only what the element can have. */
  test_assert(first("<layout><row patth='b'/></layout>") == "path");
  test_assert(first("<layout><separator patth='b'/></layout>").empty());
}

static void test_limits()
{
  const StatusCode over = StatusCode::OutOfRange;
  const auto nested = [](int n) {
    return "<layout>" + repeated("<row>", n) + repeated("</row>", n) + "</layout>";
  };
  test_assert(parsed(nested(63)).nodeCount() == 64);
  test_assert(fails(nested(64), "the elements are nested more than 64 deep", 1, 9 + 63 * 5, over));
  const auto many = [](int n) { return "<layout>" + repeated("<sep/>", n) + "</layout>"; };
  test_assert(parsed(many(16383)).nodeCount() == 16384);
  test_assert(fails(many(16384), "there are more than 16384 elements", 1, 9 + 16383 * 6, over));
  const auto attrs = [](int n, const char *text = "") {
    std::string out = "<layout><custom tag='t'";
    for (int i = 1; i < n; i++) {
      out += " a" + std::to_string(i) + "=''";
    }
    return out + ">" + text + "</custom></layout>";
  };
  test_assert(parsed(attrs(64)).node(1).attrCount == 64);
  Result<Layout> r = parseLayout(attrs(65));
  test_assert(r.status().code() == over && strstr(r.status().message(), "more than 64 attributes"));
  r = parseLayout(attrs(64, "x")); /* text is one */
  test_assert(r.status().code() == over && strstr(r.status().message(), "more than 64 attributes"));
  std::string text = "<layout/>";
  text.resize(1 << 20, ' ');
  test_assert(parsed(text).nodeCount() == 1);
  test_assert(fails(text + " ", "the text is longer than 1 MiB", 1, (1 << 20) + 1, over));
  const auto path = [](int outer, int inner) {
    return "<layout path='" + std::string(size_t(outer), 'a') + "'><prop path='" +
           std::string(size_t(inner), 'b') + "'/></layout>";
  };
  test_assert(parsed(path(1000, 22)).nodeCount() == 2);
  test_assert(fails(path(1000, 23),
                    "path: it is longer than 1024 bytes, with the prefixes around it", 1, 1029,
                    over));
  test_assert(parseLayout(path(1025, 1)).status().code() == over);
  /* An element without a path passes the prefix on. */
  test_assert(parseLayout("<layout path='" + std::string(1000, 'a') + "'><row><prop path='" +
                          std::string(24, 'b') + "'/></row></layout>")
                  .status()
                  .code() == over);
  /* From the root, what is around doesn't count. */
  test_assert(parsed("<layout path='" + std::string(1000, 'a') + "'><prop path='/" +
                     std::string(1000, 'b') + "'/></layout>")
                  .nodeCount() == 2);
}

static void test_positions()
{
  const std::string_view text = "\xEF\xBB\xBF" "a\r\nb\rc\n\xC3\xA9x";
  const auto at = [&](size_t offset, int line, int column) {
    const LayoutPosition p = layoutPosition(text, offset);
    return p.line == line && p.column == column && p.offset == offset;
  };
  test_assert(at(3, 1, 1) && at(4, 1, 2) && at(6, 2, 1) && at(8, 3, 1) && at(10, 4, 1));
  test_assert(at(12, 4, 2) && at(13, 4, 3));
  test_assert(layoutPosition(text, 99).offset == text.size());
}

/* What a later version may have. */
static void test_versions()
{
  test_assert(Layout().version() == 1 && parsed("<layout/>").version() == 1);
  test_assert(parsed("<layout version='1'/>").version() == 1);
  test_assert(parsed("<layout version='999999999'/>").version() == 999999999);
  test_assert(json("<layout version='7' id='a'/>") == json("<layout id='a'/>"));
  for (const char *bad : {"", "0", "01", "-1", "1.0", "x", " 1", "1234567890"}) {
    test_assert(fails("<layout version='" + std::string(bad) + "'/>",
                      "version: it isn't a number from 1", 1, 18));
  }
  test_assert(failsIn("<row version='1'/>", "a <row> has no 'version'", 6));
  /* Unknown things, in this version and in a newer one. */
  const std::string body = " foo='1'><row bar='2' id='r'><gone a='&lt;' x='1'>some text<row><prop/></row>"
                           "<!-- c --></gone><prop path='a'/></row><tabs><nope/><tab id='t'/></tabs></layout>";
  test_assert(fails("<layout" + body, "there is no attribute 'foo'", 1, 9));
  test_assert(fails("<layout version='1'" + body, "there is no attribute 'foo'", 1, 21));
  for (const std::string &head : {"<layout version='2'", "<layout foo='1' version='2'"}) {
    const std::string rest = head.size() > 19 ? body.substr(8) : body;
    Result<Layout> r = parseLayout(head + rest);
    test_assert(r.ok());
    if (!r.ok()) {
      continue;
    }
    test_assert(*r == parsed("<layout version='2'><row id='r'><prop path='a'/></row><tabs>"
                             "<tab id='t'/></tabs></layout>"));
    const auto &skipped = r->skipped();
    test_assert(skipped.size() == 4);
    const char *const what[] = {"there is no attribute 'foo'", "there is no attribute 'bar'",
                                "there is no element <gone>", "there is no element <nope>"};
    const char *const at[] = {"foo=", "bar=", "<gone", "<nope"};
    const int nodes[] = {0, 1, 1, 3};
    for (int i = 0; i < int(skipped.size()) && i < 4; i++) {
      test_assert(strcmp(skipped[i].what.c_str(), what[i]) == 0 && skipped[i].node == nodes[i]);
      test_assert((head + rest).compare(skipped[i].offset, strlen(at[i]), at[i]) == 0);
    }
  }
  /* What is skipped is still XML, and what is known is checked. */
  const auto newer = [](const std::string &inner) { return "<layout version='2'>" + inner + "</layout>"; };
  test_assert(fails(newer("<x><y></x>"), "</x> can't close <y>", 1, 27));
  test_assert(fails(newer("<x a=1/>"), "expected a quote, not '1'", 1, 26));
  test_assert(fails(newer("<x a='1'b='2'/>"), "expected '>' or an attribute, not 'b'", 1, 29));
  test_assert(fails(newer("<x>&y;</x>"), "'&' must start one of &lt; &gt; &amp; &quot; &apos;", 1, 24));
  test_assert(fails("<layout version='2'><x>", "the <x> in line 1 isn't closed", 1, 24));
  test_assert(fails(newer("text<x/>"), "a <layout> can't hold text: write it in a <label>", 1, 21));
  test_assert(!parseLayout(newer("<row><tab/></row>")).ok());
  test_assert(!parseLayout(newer("<row closed='maybe' expand='maybe'/>")).ok());
  test_assert(!parseLayout("<other version='2'/>").ok());
  /* An attribute of another kind's is one it doesn't know here. */
  test_assert(parseLayout(newer("<row closed='maybe'/>"))->skipped().size() == 1);
  /* Reserved names, which a Custom can't hold either. */
  for (const std::string name : {"list", "menu", "include", "slot"}) {
    const std::string e = "<" + name + " a='1'/>", what = "<" + name + "> is reserved for a later version";
    test_assert(fails("<layout>" + e + "</layout>", what, 1, 9));
    test_assert(fails("<layout><custom tag='a'>" + e + "</custom></layout>", what, 1, 25));
    const Result<Layout> r = parseLayout(newer("<custom tag='a'><b>" + e + "</b></custom>"));
    test_assert(r.ok() && r->nodeCount() == 3 && r->skipped().size() == 1 && r->skipped()[0].node == 2);
    test_assert(what == r->skipped()[0].what.c_str());
    test_assert(parsed("<layout><custom tag='" + name + "'/></layout>").nodeCount() == 2);
  }
  /* Not more than that many. */
  std::string many;
  for (int i = 0; i < LayoutLimits::kMaxSkipped + 5; i++) {
    many += "<x/>";
  }
  test_assert(parseLayout(newer(many))->skipped().size() == LayoutLimits::kMaxSkipped);
  test_assert(fails(newer("<row" + [] {
    std::string attrs;
    for (int i = 0; i < 65; i++) {
      attrs += " x='1'";
    }
    return attrs;
  }() + "/>"), "an element has more than 64 attributes", 1, 26 + 64 * 6, StatusCode::OutOfRange));
}

/* `data-` attributes, on any node. */
static void test_data()
{
  const Layout l = parsed("<layout data-a='1'><row data-b='2' data-a='x'/><custom tag='t' c='3' data-d=''/></layout>");
  test_assert(strcmp(l.freeAttr(0, "data-a")->value.c_str(), "1") == 0);
  test_assert(strcmp(l.freeAttr(1, "data-b")->value.c_str(), "2") == 0 && !l.freeAttr(1, "data-d"));
  test_assert(equals(layoutToJson(l),
                     R"({"kind":"layout","version":1,"attrs":{"data-a":"1"},"children":[)"
                     R"({"kind":"row","attrs":{"data-a":"x","data-b":"2"}},)"
                     R"({"kind":"custom","tag":"t","attrs":{"c":"3","data-d":""}}]})"));
  test_assert(failsIn("<row data-='1'/>", "there is no attribute 'data-'", 6));
  test_assert(failsIn("<row data='1'/>", "there is no attribute 'data'", 6));
  test_assert(failsIn("<row data-a='1' data-a='1'/>", "'data-a' is there twice", 17));
}

/* A tool's own prefix, and the size of the JSON. */
static void test_tool_prefix()
{
  test_assert(equals(json("<layout path='a'><tool invocation='x' path='b' visibleIf='c'/>"
                          "<tool invocation='x' path='/'/><toolOptions target='x' path='/d'/></layout>"),
                     R"({"kind":"layout","version":1,"path":"a","effectivePath":"/a","children":[)"
                     R"({"kind":"tool","path":"b","effectivePath":"/a.b","visibleIf":{"path":"/a.c"},)"
                     R"("invocation":"x","prefix":"/a.b"},)"
                     R"({"kind":"tool","path":"/","effectivePath":"/","invocation":"x"},)"
                     R"({"kind":"toolOptions","path":"/d","effectivePath":"/d","target":"x",)"
                     R"("optionsPath":"/commandDefaults[\"x\"]","prefix":"/d"}]})"));
  const Layout l = parsed("<layout path='a'><row id='b'/><tool id='a' invocation='x' path='b'/><row id='c'/></layout>");
  test_assert(l.effectivePath(2).toString() == "/a.b" && l.prefixOf(2).toString() == "/a");
  test_assert(l.findId("a") == 2 && l.findId("b") == 1 && l.findId("c") == 3);
  test_assert(l.findId("") == -1 && l.findId("bb") == -1 && l.findId("d") == -1 && Layout().findId("a") == -1);
  /* 2 MiB of joined paths: of props, of conditions, of tools. */
  const std::string outer(1000, 'a');
  const auto size = [&](int n, const char *element) {
    std::string out = "<layout path='" + outer + "'>";
    for (int i = 0; i < n; i++) {
      out += element;
    }
    return parseLayout(out + "</layout>").status();
  };
  /* "/aaa.bbbbbb" is 1008 bytes; the root's own is 1001. */
  const int fits = int((LayoutLimits::kMaxJoinedBytes - 1001) / 1008);
  test_assert(size(fits, "<prop path='bbbbbb'/>").ok());
  const Status s = size(fits + 1, "<prop path='bbbbbb'/>");
  test_assert(s.code() == StatusCode::OutOfRange &&
              strstr(s.message(), "the paths, with the prefixes around them, are more than 2 MiB"));
  test_assert(size(fits, "<row visibleIf='bbbbbb'/>").ok() && !size(fits + 1, "<row visibleIf='bbbbbb'/>").ok());
  const int tools = int((LayoutLimits::kMaxJoinedBytes - 1001) / 1001);
  test_assert(size(tools, "<tool invocation='x'/>").ok() && !size(tools + 1, "<tool invocation='x'/>").ok());
  test_assert(size(tools, "<toolOptions target='x'/>").ok() && !size(tools + 1, "<toolOptions target='x'/>").ok());
}

int main()
{
  test_versions();
  test_data();
  test_tool_prefix();
  test_golden();
  test_nodes();
  test_prefixes();
  test_accepts();
  test_refuses();
  test_refuses_content();
  test_suggestions();
  test_limits();
  test_positions();
  return test_end();
}
