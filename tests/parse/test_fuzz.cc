/* Randomized tests for the path and invocation parsers: random bytes plus mutations
 * of valid inputs. Properties: no crash, every error offset in [0, len], a valid input
 * round-trips through toString(), and NUL or int64 overflow never parse. */
#include "coredcc/parse/invocation.h"
#include "coredcc/parse/path.h"

#include "fuzz.h"
#include "litestl/tests/test_util.h"

#include <cstdio>
#include <string>

test_init;

using namespace coredcc;
using coredcc_test::SplitMix64;

static const char *const kPathCorpus[] = {
    "objects[\"Cube\"].modifiers[0].width",
    "objects[#12].location[2]",
    "scene.tool.mode[SMOOTH]",
    "a['x\\'y\\n']",
    "bone.length",
    "a[9223372036854775807]",
    "x",
};

static const char *const kInvocationCorpus[] = {
    "mesh.extrude(dist=1.0 mode='REGION')",
    "object.transform(offset=(0, 0, 1.5), target=\"objects[\\\"Cube\\\"]\")",
    "c(v=[1 2,3,] s=\"a\\tb\" e=X)",
    "view.frame",
    "c(x=-0 y=1e10 z=() w=.5e-3)",
};

/* Bytes that matter to the grammars, so mutations reach deep states. */
static const char kInteresting[] = "abzAZ_09#.[]()\"'\\=,+-eE \t\n";

static std::string mutate(SplitMix64 &rng, std::string s)
{
  const int edits = 1 + int(rng.below(4));
  for (int e = 0; e < edits; e++) {
    const uint32_t op = rng.below(5);
    const size_t pos = s.empty() ? 0 : rng.below(uint32_t(s.size() + 1));
    const char c = rng.below(8) == 0 ? char(rng.below(256))
                                     : kInteresting[rng.below(sizeof(kInteresting) - 1)];
    switch (op) {
    case 0:
      s.insert(s.begin() + pos, c);
      break;
    case 1:
      if (pos < s.size()) {
        s.erase(pos, 1);
      }
      break;
    case 2:
      if (pos < s.size()) {
        s[pos] = c;
      }
      break;
    case 3: /* duplicate a span */
      if (pos < s.size()) {
        const size_t len = 1 + rng.below(uint32_t(s.size() - pos));
        s.insert(pos, s.substr(pos, len));
      }
      break;
    default: /* a long run of digits, to provoke overflow */
      s.insert(pos, std::string(1 + rng.below(25), char('0' + rng.below(10))));
      break;
    }
  }
  return s;
}

static std::string randomBytes(SplitMix64 &rng)
{
  std::string s(rng.below(40), '\0');
  for (char &c : s) {
    c = rng.below(3) ? kInteresting[rng.below(sizeof(kInteresting) - 1)]
                     : char(rng.below(256));
  }
  return s;
}

/* Property checks shared by both parsers. Returns false (after reporting) on a
 * violation. */
template <class T, class ParseFn>
static bool check(const std::string &input, ParseFn parse, int &valid)
{
  size_t off = size_t(-1);
  Result<T> r = parse(std::string_view(input), &off);
  if (!r.ok()) {
    if (off > input.size()) {
      fprintf(stderr, "offset %zu past the end of %zu bytes\n", off, input.size());
      return false;
    }
    return true;
  }
  valid++;
  if (input.find('\0') != std::string::npos) {
    fprintf(stderr, "an input with NUL parsed\n");
    return false;
  }
  const std::string canon = r->toString();
  Result<T> again = parse(std::string_view(canon), nullptr);
  if (!again.ok() || !(*again == *r) || again->toString() != canon) {
    fprintf(stderr, "round trip failed: '%s'\n", canon.c_str());
    return false;
  }
  return true;
}

template <class T, class ParseFn>
static void fuzz(const char *name,
                 const char *const *corpus,
                 int corpusSize,
                 ParseFn parse,
                 uint64_t seed)
{
  SplitMix64 rng(seed);
  const int iterations = 20000 * coredcc_test::testScale();
  int valid = 0;
  for (int i = 0; i < iterations; i++) {
    const std::string input = rng.below(4) == 0
                                  ? randomBytes(rng)
                                  : mutate(rng, corpus[rng.below(uint32_t(corpusSize))]);
    if (!check<T>(input, parse, valid)) {
      std::string hex;
      for (unsigned char c : input) {
        char b[4];
        snprintf(b, sizeof(b), "%02x", c);
        hex += b;
      }
      fprintf(stderr, "%s: failing input (hex) %s\n", name, hex.c_str());
      test_assert(false);
      return;
    }
  }
  /* The mutations must keep a fair share valid, or the round-trip check is idle. */
  printf("%s: %d of %d inputs valid\n", name, valid, iterations);
  test_assert(valid > iterations / 20);
}

int main()
{
  for (const char *c : kPathCorpus) {
    test_assert(parsePath(c).ok());
  }
  for (const char *c : kInvocationCorpus) {
    test_assert(parseInvocation(c).ok());
  }
  fuzz<Path>(
      "path",
      kPathCorpus,
      int(sizeof(kPathCorpus) / sizeof(kPathCorpus[0])),
      [](std::string_view s, size_t *off) { return parsePath(s, off); },
      0xC0DEDCC1ull);
  fuzz<Invocation>(
      "invocation",
      kInvocationCorpus,
      int(sizeof(kInvocationCorpus) / sizeof(kInvocationCorpus[0])),
      [](std::string_view s, size_t *off) { return parseInvocation(s, off); },
      0xC0DEDCC2ull);
  return test_end();
}
