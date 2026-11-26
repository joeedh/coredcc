/* The layout parser on random and hostile text: no crash, positions in the text, accepted
 * layouts survive being written and built again, and time and size stay within bounds. */
#include "coredcc/base/utf8.h"
#include "coredcc/layout/builder.h"
#include "coredcc/layout/xml.h"

#include "fuzz.h"
#include "litestl/tests/test_util.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

test_init;

using namespace coredcc;
using coredcc_test::SplitMix64;
using Id = LayoutAttrId;

namespace {

struct Watch {
  std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
  double seconds() const
  {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
        .count();
  }
};

std::string escaped(std::string_view s)
{
  std::string out;
  for (char c : s) {
    out += c == '<'    ? "&lt;"
           : c == '>'  ? "&gt;"
           : c == '&'  ? "&amp;"
           : c == '"'  ? "&quot;"
           : c == '\'' ? "&apos;"
                       : std::string(1, c);
  }
  return out;
}

/* The layout as XML. */
void writeXml(const Layout &l, int i, std::string &out)
{
  const LayoutNode &n = l.node(i);
  std::string text;
  out += std::string("<") + layoutKindName(n.kind);
  for (uint32_t j = 0; j < n.attrCount; j++) {
    const LayoutAttr &a = l.attrs()[n.attrBegin + j];
    if (a.id == Id::Text || (a.id == Id::Label && n.kind == LayoutKind::Tool && i % 2)) {
      text = a.value.c_str();
      continue;
    }
    out += std::string(" ") + (a.id == Id::Free ? a.name.c_str() : layoutAttrName(a.id)) +
           "=\"" + escaped(a.value.c_str()) + "\"";
  }
  out += ">" + escaped(text);
  for (int c = n.firstChild; c >= 0; c = l.node(c).nextSibling) {
    writeXml(l, c, out);
  }
  out += std::string("</") + layoutKindName(n.kind) + ">";
}

Layout rebuilt(const Layout &l)
{
  LayoutBuilder b;
  std::vector<LayoutScope> scopes;
  for (int i = 0; i < l.nodeCount(); i++) {
    const LayoutNode &n = l.node(i);
    scopes.push_back(i == 0 ? b.root() : scopes[size_t(n.parent)].add(n.kind));
    for (uint32_t j = 0; j < n.attrCount; j++) {
      const LayoutAttr &a = l.attrs()[n.attrBegin + j];
      if (a.id == Id::Free) {
        scopes.back().set(a.name.c_str(), a.value.c_str());
      } else {
        scopes.back().set(a.id, a.value.c_str());
      }
    }
  }
  Result<Layout> r = b.finish();
  test_assert(r.ok());
  return r.ok() ? std::move(*r) : Layout();
}

/* Checks that `s` is a JSON value, without recursion. */
bool isJson(const std::string &s)
{
  std::string open;
  bool value = false; /* one just ended */
  for (size_t i = 0; i < s.size(); i++) {
    const char c = s[i];
    if (c == '"') {
      for (i++; i < s.size() && s[i] != '"'; i++) {
        if (uint8_t(s[i]) < 0x20) {
          return false;
        }
        i += s[i] == '\\' ? 1 : 0;
      }
      if (i >= s.size()) {
        return false;
      }
      value = true;
    } else if (c == '{' || c == '[') {
      open += c;
      value = false;
    } else if (c == '}' || c == ']') {
      if (open.empty() || open.back() != (c == '}' ? '{' : '[')) {
        return false;
      }
      open.pop_back();
      value = true;
    } else if (c == ',' || c == ':') {
      if (!value) {
        return false;
      }
      value = false;
    } else if (c >= '0' && c <= '9') {
      value = true;
    } else if (s.compare(i, 4, "true") == 0) {
      i += 3;
      value = true;
    } else if (s.compare(i, 5, "false") == 0) {
      i += 4;
      value = true;
    } else {
      return false;
    }
  }
  return open.empty() && value && utf8ValidPrefix(s) == s.size();
}

/* What holds for any text. True if it is a layout. */
bool check(std::string_view xml)
{
  LayoutPosition p;
  p.line = -1;
  Result<Layout> r = parseLayout(xml, &p);
  if (!r.ok()) {
    test_assert(p.offset <= xml.size() && p.line >= 1 && p.column >= 1);
    test_assert(strncmp(r.status().message(), "layout ", 7) == 0);
    return false;
  }
  const Layout &l = *r;
  test_assert(l.nodeCount() <= LayoutLimits::kMaxNodes);
  test_assert(rebuilt(l) == l);
  std::string again;
  writeXml(l, 0, again);
  if (again.size() <= LayoutLimits::kMaxBytes) {
    Result<Layout> second = parseLayout(again);
    test_assert(second.ok() && *second == l);
    if (!second.ok()) {
      fprintf(stderr, "%s\n%s\n", second.status().message(), again.c_str());
    }
  }
  const std::string json = layoutToJson(l);
  test_assert(isJson(json));
  for (int i = 0; i < l.nodeCount(); i++) {
    LayoutCondition c;
    test_assert(l.effectivePath(i).toString().size() <= LayoutLimits::kMaxPathBytes);
    l.condition(i, Id::VisibleIf, c);
  }
  return true;
}

const char *const kCorpus[] = {
    R"x(<?xml version="1.0"?><layout path="a.b" id="r"><panel label="x" closed="true"><prop path="c"
 visibleIf="!d" widget="w"/><row><tool invocation="x.y(z='W')">T &amp; U</tool><sep/></row></panel>
<tabs path="/q"><tab label="1"><label>t</label></tab></tabs></layout>)x",
    R"(<layout><custom tag="t" a="1"><b><c x='y'/> </b><d> text </d></custom><!-- c --></layout>)",
    R"(<layout><toolOptions target="a.b" enabledIf="e[F]"/><column expand="false"/></layout>)",
};

const char *const kPieces[] = {
    "<layout>",  "</layout>", "<row>",   "</row>",   "<custom tag='x'>",
    "</custom>", "<sep/>",    "<!--",    "-->",      "<![CDATA[",
    "<?xml",     "?>",        "&amp;",   "&#1;",     "path='a.b'",
    "id=\"i\"",  " ",         "'",       "\"",       "<",
    ">",         "/>",        "]]>",     "text",     "\xC3\xA9",
    "\r\n",      "xmlns='a'", "a:b='c'", "label=''", "<!DOCTYPE"};

} // namespace

static void test_mutations()
{
  SplitMix64 rng(7);
  const Watch watch;
  int accepted = 0;
  const int rounds = 20000 * coredcc_test::testScale();
  for (int round = 0; round < rounds; round++) {
    std::string s = kCorpus[rng.below(3)];
    const int edits = rng.below(3) ? 1 : 2 + int(rng.below(2));
    for (int e = 0; e < edits; e++) {
      const size_t at = rng.below(uint32_t(s.size()));
      switch (rng.below(6)) {
      case 0:
        s.erase(at, 1 + rng.below(8));
        break;
      case 1:
        s.insert(at, kPieces[rng.below(sizeof(kPieces) / sizeof(kPieces[0]))]);
        break;
      case 2:
        s[at] = char(rng.below(256));
        break;
      case 3:
        s.insert(at, s.substr(rng.below(uint32_t(s.size())), rng.below(40)));
        break;
      case 4:
        s.resize(at);
        break;
      default:
        s[at] = "<>&'\"/=! \n"[rng.below(10)];
        break;
      }
      if (s.empty()) {
        break;
      }
    }
    accepted += check(s) ? 1 : 0;
  }
  if (accepted < rounds / 50) {
    fprintf(stderr, "only %d of %d were layouts\n", accepted, rounds);
  }
  test_assert(accepted >= rounds / 50 && accepted < rounds);
  test_assert(watch.seconds() < 60 * coredcc_test::testScale());
}

static void test_prefixes_of_valid()
{
  for (const char *text : kCorpus) {
    const std::string s = text;
    test_assert(check(s));
    for (size_t n = 0; n < s.size(); n++) {
      test_assert(!check(s.substr(0, n)));
    }
  }
}

static void test_pieces()
{
  SplitMix64 rng(11);
  for (int round = 0; round < 5000 * coredcc_test::testScale(); round++) {
    std::string s = rng.below(2) ? "<layout>" : "";
    for (uint32_t n = rng.below(30); n > 0; n--) {
      s += kPieces[rng.below(sizeof(kPieces) / sizeof(kPieces[0]))];
    }
    check(s);
  }
}

/* A random layout, written, read and built again. */
static void test_random_layouts()
{
  SplitMix64 rng(3);
  const char *const paths[] = {"a", "a.b[0]", "/", "/x.y", "k[\"<'&>\\\"\"]", "e[F]"};
  const char *const texts[] = {
      "x", "a b", "<&>\"'", "\xF0\x9F\x98\x80", "  in  ", "-->", "]]>"};
  int most = 0;
  for (int round = 0; round < 500 * coredcc_test::testScale(); round++) {
    LayoutBuilder b;
    std::vector<LayoutScope> in = {b.root()};
    const int count = 1 + int(rng.below(40));
    const auto text = [&]() { return texts[rng.below(7)]; };
    const auto path = [&]() { return paths[rng.below(6)]; };
    for (int i = 0; i < count; i++) {
      LayoutScope s = in[rng.below(uint32_t(in.size()))];
      switch (rng.below(9)) {
      case 0:
        s = s.panel(text()).path(path());
        in.push_back(s);
        break;
      case 1:
        s = s.row();
        in.push_back(s);
        break;
      case 2:
        s = s.tabs().tab(text());
        in.push_back(s);
        break;
      case 3:
        s = s.prop(paths[rng.below(2)]);
        break;
      case 4:
        s = s.tool("a.b(c='<&>')", rng.below(2) ? "x &<" : "");
        break;
      case 5:
        s = s.custom("t").set("free", text()).set("z.z-z", text());
        in.push_back(s);
        break;
      case 6:
        s = s.custom("t").set(Id::Text, "a\n <&>");
        break;
      case 7:
        s = s.label("l").set(Id::Class, text());
        break;
      default:
        s = s.separator();
        break;
      }
      if (rng.below(3) == 0) {
        s.visibleIf(rng.below(2) ? "!a.b" : "/c");
      }
      if (rng.below(3) == 0) {
        s.id("id" + std::to_string(i));
      }
    }
    Result<Layout> r = b.finish();
    if (!r.ok()) {
      fprintf(stderr, "%s\n", r.status().message());
    }
    test_assert(r.ok());
    std::string xml;
    writeXml(*r, 0, xml);
    Result<Layout> back = parseLayout(xml);
    test_assert(back.ok() && *back == *r && check(xml));
    most = most > r->nodeCount() ? most : r->nodeCount();
  }
  test_assert(most > 30);
}

/* The worst that 1 MiB of text can do. */
static void test_bounds()
{
  const auto bounded =
      [](const char *what, const std::string &xml, bool ok, double seconds) {
        test_assert(xml.size() <= LayoutLimits::kMaxBytes);
        const Watch watch;
        Result<Layout> r = parseLayout(xml);
        size_t json = 0;
        if (r.ok()) {
          json = layoutToJson(*r).size();
        }
        const double took = watch.seconds();
        if (r.ok() != ok || took > seconds || json > (8u << 20)) {
          fprintf(stderr,
                  "%s: %s, %f s, %zu bytes of JSON\n",
                  what,
                  r.ok() ? "ok" : r.status().message(),
                  took,
                  json);
        }
        test_assert(r.ok() == ok && took <= seconds && json <= (8u << 20));
      };
  const auto repeated = [](const std::string &s, size_t bytes) {
    std::string out;
    while (out.size() + s.size() <= bytes) {
      out += s;
    }
    return out;
  };
  const size_t room = LayoutLimits::kMaxBytes - 2100;
  const auto in = [](const std::string &inner, const std::string &path = "a") {
    return "<layout path='" + path + "'>" + inner + "</layout>";
  };
  std::string segments = "a";
  while (segments.size() < 1010) {
    segments += ".a";
  }
  std::string rows, ends;
  for (int i = 0; i < 62; i++) {
    rows += "<row>";
    ends += "</row>";
  }
  std::string deep;
  for (int i = 0; i < 16000; i++) {
    deep += "<prop path='b' visibleIf='c' enabledIf='d'/>";
  }
  bounded("long prefixes", in(deep, segments), false, 10);
  bounded("long prefixes that fit",
          in(deep.substr(0, deep.size() / 16000 * 680), segments),
          true,
          10);
  bounded("long prefixes of tools",
          in(repeated("<tool invocation='a'/>", 1 << 18), segments),
          false,
          10);
  bounded("skipped", "<layout version='2'>" + repeated("<x a='1'>", room / 2), false, 5);
  bounded("skipped attributes",
          "<layout version='2'><x" + repeated(" a='1'", room / 2) + "/></layout>",
          true,
          5);
  bounded("long prefixes, deep", in(rows + deep + ends, segments), false, 10);
  std::string ids;
  for (int i = 0; i < 16000; i++) {
    ids += "<sep id='" + std::to_string(i) + "'/>";
  }
  bounded("ids", in(ids), true, 5);
  std::string attrs;
  for (int i = 0; i < 2000; i++) {
    attrs += "<custom tag='t'";
    for (int j = 0; j < 63; j++) {
      attrs += " a" + std::to_string(j) + "=''";
    }
    attrs += "/>";
  }
  bounded("attributes", in(attrs), true, 5);
  std::string many;
  for (int i = 0; many.size() < room - 20; i++) {
    many += " a" + std::to_string(i) + "=''";
  }
  bounded("one element's attributes", in("<custom tag='t'" + many + "/>"), false, 5);
  bounded("elements", in(repeated("<sep/>", room)), false, 5);
  bounded("nested elements", in(repeated("<row>", room)), false, 5);
  bounded("text", in("<label>" + repeated("a &amp;\n", room) + "</label>"), true, 5);
  bounded("blanks", in("<label>a" + repeated(" ", room) + "b</label>"), true, 5);
  bounded("a long value", in("<label label='" + repeated("&lt;", room) + "'/>"), true, 5);
  bounded("a long path", in("<prop path='" + repeated("a.", room) + "a'/>"), false, 5);
  bounded("a long invocation",
          in("<tool invocation='a(" + repeated("b=1 ", room) + ")'/>"),
          false,
          5);
  bounded("a long name", in("<" + repeated("panel", room) + "/>"), false, 5);
  bounded("a long attribute", in("<row " + repeated("path", room) + "='a'/>"), false, 5);
  bounded("comments", in(repeated("<!---->", room)), true, 5);
  bounded(
      "pieces of text", in("<label>" + repeated("a<!---->", room) + "</label>"), true, 5);
  bounded("open elements", repeated("<layout>", room), false, 5);
  bounded("lines", repeated("\n", room), false, 5);
  bounded("quotes", "<layout path='" + repeated("\"", room), false, 5);
}

int main()
{
  test_prefixes_of_valid();
  test_mutations();
  test_pieces();
  test_random_layouts();
  test_bounds();
  return test_end();
}
