/* Layouts from XML across versions: what a newer version's text skips, the reserved
 * names, and the renamed elements. */
#include "xml_check.h"

#include "coredcc/layout/layout.h"
#include "coredcc/layout/xml.h"

#include "litestl/tests/test_util.h"

#include <cstring>
#include <string>

test_init;

using namespace coredcc;
using namespace xmlcheck;

static void test_versions()
{
  test_assert(Layout().version() == 1 && parsed("<layout/>").version() == 1);
  test_assert(parsed("<layout version='1'/>").version() == 1);
  test_assert(parsed("<layout version='999999999'/>").version() == 999999999);
  test_assert(json("<layout version='7' id='a'/>") == json("<layout id='a'/>"));
  for (const char *bad : {"", "0", "01", "-1", "1.0", "x", " 1", "1234567890"}) {
    test_assert(fails("<layout version='" + std::string(bad) + "'/>",
                      "version: it isn't a number from 1",
                      1,
                      18));
  }
  test_assert(failsIn("<row version='1'/>", "a <row> has no 'version'", 6));
  /* Unknown things, in this version and in a newer one. */
  const std::string body =
      " foo='1'><row bar='2' id='r'><gone a='&lt;' x='1'>some text<row><prop/></row>"
      "<!-- c --></gone><prop path='a'/></row><tabs><nope/><tab id='t'/></tabs></layout>";
  test_assert(fails("<layout" + body, "there is no attribute 'foo'", 1, 9));
  test_assert(fails("<layout version='1'" + body, "there is no attribute 'foo'", 1, 21));
  test_assert(fails("<layout foo='1' bar='2'/>", "there is no attribute 'foo'", 1, 9));
  for (const std::string &head : {"<layout version='2'", "<layout foo='1' version='2'"}) {
    const std::string rest = head.size() > 19 ? body.substr(8) : body;
    Result<Layout> r = parseLayout(head + rest);
    test_assert(r.ok());
    if (!r.ok()) {
      continue;
    }
    test_assert(*r ==
                parsed("<layout version='2'><row id='r'><prop path='a'/></row><tabs>"
                       "<tab id='t'/></tabs></layout>"));
    const auto &skipped = r->skipped();
    test_assert(skipped.size() == 4);
    const char *const what[] = {"there is no attribute 'foo'",
                                "there is no attribute 'bar'",
                                "there is no element <gone>",
                                "there is no element <nope>"};
    const char *const at[] = {"foo=", "bar=", "<gone", "<nope"};
    const int nodes[] = {0, 1, 1, 3};
    for (int i = 0; i < int(skipped.size()) && i < 4; i++) {
      test_assert(strcmp(skipped[i].what.c_str(), what[i]) == 0 &&
                  skipped[i].node == nodes[i]);
      test_assert((head + rest).compare(skipped[i].offset, strlen(at[i]), at[i]) == 0);
    }
  }
  /* What is skipped is still XML, and what is known is checked. */
  const auto newer = [](const std::string &inner) {
    return "<layout version='2'>" + inner + "</layout>";
  };
  test_assert(fails(newer("<x><y></x>"), "</x> can't close <y>", 1, 27));
  test_assert(fails(newer("<x a=1/>"), "expected a quote, not '1'", 1, 26));
  test_assert(
      fails(newer("<x a='1'b='2'/>"), "expected '>' or an attribute, not 'b'", 1, 29));
  test_assert(fails(
      newer("<x>&y;</x>"), "'&' must start one of &lt; &gt; &amp; &quot; &apos;", 1, 24));
  test_assert(fails("<layout version='2'><x>", "the <x> in line 1 isn't closed", 1, 24));
  test_assert(fails(
      newer("text<x/>"), "a <layout> can't hold text: write it in a <label>", 1, 21));
  test_assert(!parseLayout(newer("<row><tab/></row>")).ok());
  test_assert(!parseLayout(newer("<row closed='maybe' expand='maybe'/>")).ok());
  test_assert(!parseLayout("<other version='2'/>").ok());
  /* An attribute of another kind's is one it doesn't know here. */
  test_assert(parseLayout(newer("<row closed='maybe'/>"))->skipped().size() == 1);
  /* Reserved names, which a Custom can't hold either. */
  for (const std::string name : {"list", "menu", "include", "slot"}) {
    const std::string e = "<" + name + " a='1'/>",
                      what = "<" + name + "> is reserved for a later version";
    test_assert(fails("<layout>" + e + "</layout>", what, 1, 9));
    test_assert(
        fails("<layout><custom tag='a'>" + e + "</custom></layout>", what, 1, 25));
    const Result<Layout> r =
        parseLayout(newer("<custom tag='a'><b>" + e + "</b></custom>"));
    test_assert(r.ok() && r->nodeCount() == 3 && r->skipped().size() == 1 &&
                r->skipped()[0].node == 2);
    test_assert(what == r->skipped()[0].what.c_str());
    test_assert(parsed("<layout><custom tag='" + name + "'/></layout>").nodeCount() == 2);
  }
  /* Not more than that many. */
  std::string many;
  for (int i = 0; i < LayoutLimits::kMaxSkipped + 5; i++) {
    many += "<x/>";
  }
  test_assert(parseLayout(newer(many))->skipped().size() == LayoutLimits::kMaxSkipped);
  test_assert(fails(newer("<row" +
                          [] {
                            std::string attrs;
                            for (int i = 0; i < 65; i++) {
                              attrs += " x='1'";
                            }
                            return attrs;
                          }() +
                          "/>"),
                    "an element has more than 64 attributes",
                    1,
                    26 + 64 * 6,
                    StatusCode::OutOfRange));
}

/* The old names of renamed elements are errors that name the new one, in the text of any
 * version and in a Custom, where another name would be skipped or be a Custom. */
static void test_renamed()
{
  const char *renamed[][2] = {{"tool", "command"}, {"toolOptions", "commandOptions"}};
  for (const auto &r : renamed) {
    const std::string e = "<" + std::string(r[0]) + " a='1'/>",
                      what = "<" + std::string(r[0]) + "> is now <" + r[1] + ">";
    test_assert(fails("<layout>" + e + "</layout>", what, 1, 9));
    test_assert(fails("<layout version='2'>" + e + "</layout>", what, 1, 21));
    test_assert(
        fails("<layout><custom tag='a'>" + e + "</custom></layout>", what, 1, 25));
    test_assert(
        fails("<layout version='2'><custom tag='a'><b>" + e + "</b></custom></layout>",
              what,
              1,
              40));
    /* As a Custom's tag, it is only a name. */
    test_assert(parsed("<layout><custom tag='" + std::string(r[0]) + "'/></layout>")
                    .nodeCount() == 2);
  }
}

int main()
{
  test_versions();
  test_renamed();
  return test_end();
}
