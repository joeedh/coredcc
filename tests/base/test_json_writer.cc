/* JsonWriter's text, and the UTF-8 checks it and the parsers share. */
#include "coredcc/base/json_writer.h"
#include "coredcc/base/utf8.h"

#include "litestl/tests/test_util.h"

#include <cmath>
#include <cstdio>
#include <string>

test_init;

using namespace coredcc;

static bool equals(const std::string &got, const std::string &want)
{
  if (got != want) {
    fprintf(stderr, "got  %s\nwant %s\n", got.c_str(), want.c_str());
  }
  return got == want;
}

static void test_json()
{
  JsonWriter w;
  w.beginObject();
  w.key("a").integer(1).key("b").beginArray();
  w.boolean(true).boolean(false).null().number(0.5).number(-2);
  w.beginObject().endObject().beginArray().endArray().string("x");
  w.endArray();
  w.key("c").beginObject().key("d").beginArray().integer(2).endArray().endObject();
  w.key("").string("");
  w.endObject();
  test_assert(equals(w.take(),
                     "{\"a\":1,\"b\":[true,false,null,0.5,-2,{},[],\"x\"],"
                     "\"c\":{\"d\":[2]},\"\":\"\"}"));
  /* The writer can be used again. */
  test_assert(equals(JsonWriter().integer(-7).take(), "-7"));
  w.string("again");
  test_assert(equals(w.take(), "\"again\""));
}

static void test_numbers()
{
  const int64_t edge = int64_t(1) << 53;
  JsonWriter w;
  w.beginArray();
  w.integer(edge).integer(edge + 1).integer(-edge).integer(-edge - 1);
  w.integer(INT64_MIN).integer(INT64_MAX);
  w.unsignedInteger(uint64_t(edge))
      .unsignedInteger(uint64_t(edge) + 1)
      .unsignedInteger(UINT64_MAX);
  w.number(NAN).number(INFINITY).number(-INFINITY).number(0.1).number(1e300);
  w.endArray();
  test_assert(
      equals(w.take(),
             "[9007199254740992,\"9007199254740993\",-9007199254740992,"
             "\"-9007199254740993\",\"-9223372036854775808\",\"9223372036854775807\","
             "9007199254740992,\"9007199254740993\",\"18446744073709551615\","
             "\"nan\",\"inf\",\"-inf\",0.1,1e+300]"));
}

static void test_strings()
{
  JsonWriter w;
  w.beginObject();
  w.key("k\"\n").string(std::string_view("a\"b\\c\n\t\x01\x1f/\x7f\0z", 13));
  w.endObject();
  test_assert(equals(w.take(),
                     "{\"k\\\"\\u000a\":\"a\\\"b\\\\c\\u000a\\u0009\\u0001\\u001f/\x7f"
                     "\\u0000z\"}"));
  test_assert(equals(JsonWriter().string("\xC3\xA9 \xE2\x82\xAC \xF0\x9F\x98\x80").take(),
                     "\"\xC3\xA9 \xE2\x82\xAC \xF0\x9F\x98\x80\""));
  /* Every byte that isn't part of a valid sequence becomes one U+FFFD. */
  test_assert(
      equals(JsonWriter().string("a\xFF\xC3z\xED\xA0\x80").take(),
             "\"a\xEF\xBF\xBD\xEF\xBF\xBDz\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD\""));
}

static void test_utf8()
{
  struct Case {
    const char *text;
    size_t length;
  };
  const Case cases[] = {
      {"a", 1},
      {"\x7f", 1},
      {"\xC2\x80", 2},
      {"\xDF\xBF", 2},
      {"\xE0\xA0\x80", 3},
      {"\xED\x9F\xBF", 3},
      {"\xEE\x80\x80", 3},
      {"\xEF\xBF\xBF", 3},
      {"\xF0\x90\x80\x80", 4},
      {"\xF4\x8F\xBF\xBF", 4},
      {"", 0},
      {"\x80", 0},     /* a stray continuation byte */
      {"\xC0\x80", 0}, /* overlong */
      {"\xC1\xBF", 0},
      {"\xE0\x9F\xBF", 0},
      {"\xF0\x8F\xBF\xBF", 0},
      {"\xED\xA0\x80", 0},     /* a surrogate */
      {"\xF4\x90\x80\x80", 0}, /* past U+10FFFF */
      {"\xF5\x80\x80\x80", 0},
      {"\xC3", 0}, /* cut off */
      {"\xE2\x82", 0},
      {"\xF0\x9F\x98", 0},
      {"\xC3\x28", 0}, /* no continuation */
      {"\xE2\x28\xA1", 0},
      {"\xE2\x82\x28", 0},
      {"\xF0\x9F\x98\x28", 0},
      {"\xF0\x9F\x28\x80", 0},
  };
  for (const Case &c : cases) {
    if (utf8SequenceLength(c.text, 0) != c.length) {
      fprintf(
          stderr, "case %d: %d\n", int(&c - cases), int(utf8SequenceLength(c.text, 0)));
      test_assert(false);
    }
  }
  test_assert(utf8SequenceLength("ab", 1) == 1 && utf8SequenceLength("ab", 2) == 0);
  test_assert(utf8SequenceLength("ab", 9) == 0);
  test_assert(utf8ValidPrefix("a\xC3\xA9\xFFz") == 3 && utf8ValidPrefix("abc") == 3);
  test_assert(utf8ValidPrefix("") == 0);
  test_assert(utf8ValidPrefix(std::string_view("a\0b", 3)) == 3);
}

int main()
{
  test_json();
  test_numbers();
  test_strings();
  test_utf8();
  return test_end();
}
