#include "coredcc/parse/path.h"
#include "coredcc/parse/path_cache.h"

#include "test_util.h"

#include <cstdio>
#include <cstring>
#include <string>

test_init;

using namespace coredcc;

static Path ok(const char *text)
{
  Result<Path> r = parsePath(text);
  if (!r.ok()) {
    fprintf(stderr, "parsePath('%s'): %s\n", text, r.status().message());
    test_assert(false);
    return Path();
  }
  return std::move(r).value();
}

/* The error offset, checking the message names it too. */
static size_t failsAt(std::string_view text, const char *needle = nullptr)
{
  size_t off = 999;
  Result<Path> r = parsePath(text, &off);
  test_assert(!r.ok());
  if (r.ok()) {
    return 999;
  }
  test_assert(strstr(r.status().message(), ("at offset " + std::to_string(off)).c_str()));
  if (needle && !strstr(r.status().message(), needle)) {
    fprintf(stderr, "message was: %s\n", r.status().message());
    test_assert(false);
  }
  return off;
}

static void test_forms()
{
  Path p = ok("objects[\"Cube\"].modifiers[0].width");
  test_assert(p.size() == 5);
  test_assert(p[0].kind == SegmentKind::Member && strcmp(p[0].text.c_str(), "objects") == 0);
  test_assert(p[1].kind == SegmentKind::StringKey && strcmp(p[1].text.c_str(), "Cube") == 0);
  test_assert(p[3].kind == SegmentKind::Index && p[3].index == 0);
  test_assert(p[4].kind == SegmentKind::Member);

  p = ok("objects[#12].location[2]");
  test_assert(p[1].kind == SegmentKind::HandleKey && p[1].index == 12);
  test_assert(p[3].kind == SegmentKind::Index && p[3].index == 2);

  /* Segment offsets point at the '.' or '['. */
  p = ok("objects['k'].modifiers[0].width");
  test_assert(p[0].offset == 0 && p[1].offset == 7 && p[2].offset == 12);
  test_assert(p[3].offset == 22 && p[4].offset == 25);

  p = ok("tool.mode[SMOOTH]");
  test_assert(p[2].kind == SegmentKind::EnumKey && strcmp(p[2].text.c_str(), "SMOOTH") == 0);
}

static void test_keys_and_escapes()
{
  Path p = ok("a['x\\'y\\\"z\\\\w\\n\\t']");
  test_assert(strcmp(p[1].text.c_str(), "x'y\"z\\w\n\t") == 0);
  test_assert(p.toString() == "a[\"x'y\\\"z\\\\w\\n\\t\"]");
  test_assert(ok(p.toString().c_str()) == p);
  test_assert(ok("a[\"\"]")[1].text.size() == 0); /* the empty key */
  test_assert(ok("a[007]")[1].index == 7);
  test_assert(ok("a[007]").toString() == "a[7]");
  test_assert(ok("a[9223372036854775807]")[1].index == INT64_MAX);
  /* active and length are plain members to the parser. */
  test_assert(ok("bone.length")[1].kind == SegmentKind::Member);
  test_assert(ok("objects.active.name").size() == 3);
}

static void test_errors()
{
  test_assert(failsAt("", "empty path") == 0);
  test_assert(failsAt("a.", "after '.'") == 2);
  test_assert(failsAt("a[") == 2);
  test_assert(failsAt("a[1") == 3);
  test_assert(failsAt("1a") == 0);
  test_assert(failsAt(".a") == 0);
  test_assert(failsAt("a b") == 1);
  test_assert(failsAt("a..b") == 2);
  test_assert(failsAt("a[-1]") == 2);
  test_assert(failsAt("a[1 ]") == 3);
  test_assert(failsAt("a[#]") == 3);
  test_assert(failsAt("a[#x]") == 3);
  test_assert(failsAt("a[\"x]", "unterminated string") == 5);
  test_assert(failsAt("a[\"x\\q\"]", "unknown escape") == 4);
  test_assert(failsAt("a[\"x\\", "unterminated escape") == 5);
  test_assert(failsAt("a['it''s']") == 6);
  /* int64 overflow is an error, not a wrap. */
  test_assert(failsAt("a[9223372036854775808]", "int64") == 2);
  test_assert(failsAt("a[#99999999999999999999]", "int64") == 3);
  /* NUL, raw or escaped. */
  test_assert(failsAt(std::string_view("a\0b", 3), "NUL") == 1);
  test_assert(failsAt(std::string_view("a[\"x\0\"]", 7), "NUL") == 4);
  test_assert(failsAt("a[\"\\0\"]", "unknown escape") == 3);
  test_assert(failsAt("a\xC3\xA9", "byte 0xc3") == 1);
}

static void test_hash_and_equality()
{
  test_assert(ok("a.b") == ok("a.b"));
  test_assert(!(ok("a.b") == ok("a.c")));
  test_assert(!(ok("a[1]") == ok("a[#1]")));
  test_assert(!(ok("a[X]") == ok("a[\"X\"]")));
  test_assert(ok("a[\"k\"]") == ok("a['k']"));
  test_assert(ok("a.b").computeHash() == ok("a.b").computeHash());
  test_assert(ok("a[1]").computeHash() != ok("a[#1]").computeHash());
  /* A kind byte inside a key can't mimic a second segment. */
  test_assert(ok("a[\"b\x04c\"]").computeHash() != ok("a[\"b\"][c]").computeHash());
}

static void test_cache()
{
  Runtime rt;
  PathCache &cache = rt.ext<PathCache>();
  test_assert(&rt.ext<PathCache>() == &cache);
  Result<PathPtr> a = cache.get("objects.active.name");
  test_assert(a.ok() && (*a)->size() == 3);
  test_assert(a->useCount() == 2); /* the caller and the cache */
  Result<PathPtr> again = cache.get("objects.active.name");
  test_assert(again.ok() && &**again == &**a); /* the same node */
  test_assert(cache.hits() == 1 && cache.misses() == 1);

  size_t off = 0;
  test_assert(!cache.get("a..b", &off).ok() && off == 2);
  test_assert(cache.size() == 1); /* failures aren't cached */

  /* LRU: with capacity 2, touching x keeps it over y. */
  cache.setCapacity(2);
  PathPtr x = *cache.get("x");
  test_assert(cache.size() == 2);
  PathPtr y = *cache.get("y"); /* evicts objects.active.name, the oldest */
  test_assert(cache.size() == 2);
  test_assert(a->useCount() == 2); /* a and again, no longer the cache */
  test_assert(cache.get("x").ok());
  test_assert(cache.get("z").ok()); /* evicts y */
  test_assert(y.useCount() == 1 && x.useCount() == 2);
  test_assert(strcmp((*a)->segments[2].text.c_str(), "name") == 0); /* still valid */
  test_assert(rt.validateAll().ok());

  /* Many inserts: nothing a caller holds moves. */
  cache.setCapacity(PathCache::kDefaultCapacity);
  const Path *held = &*x;
  for (int i = 0; i < 3000; i++) {
    test_assert(cache.get("p" + std::to_string(i)).ok());
  }
  test_assert(held == &*x && x->toString() == "x");
  test_assert(cache.size() == PathCache::kDefaultCapacity);
  test_assert(rt.validateAll().ok());

  /* LRU order beyond the trivial case: with capacity 3, a, b, c, touch a, add d -> b
   * goes; touch c, add e -> a goes. */
  cache.clear();
  cache.setCapacity(3);
  for (const char *t : {"a", "b", "c"}) {
    test_assert(cache.get(t).ok());
  }
  test_assert(cache.get("a").ok() && cache.get("d").ok());
  const uint64_t missesBefore = cache.misses();
  test_assert(cache.get("a").ok() && cache.get("c").ok() && cache.get("d").ok());
  test_assert(cache.misses() == missesBefore); /* all still cached */
  test_assert(cache.get("b").ok() && cache.misses() == missesBefore + 1); /* b was evicted */
  test_assert(rt.validateAll().ok());
  cache.setCapacity(0); /* clamps to 1 */
  test_assert(cache.size() == 1 && cache.capacity() == 1 && rt.validateAll().ok());
  cache.clear();
  test_assert(cache.size() == 0 && x.useCount() == 1);
}

int main()
{
  test_forms();
  test_keys_and_escapes();
  test_errors();
  test_hash_and_equality();
  test_cache();
  return test_end();
}
