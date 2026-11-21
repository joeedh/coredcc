#include "coredcc/base/text.h"

#include "test_util.h"

#include <cstring>

test_init;

using namespace coredcc;

int main()
{
  test_assert(editDistance("", "") == 0);
  test_assert(editDistance("kitten", "sitting") == 3);
  test_assert(editDistance("abc", "") == 3);
  test_assert(editDistance("location", "locaton") == 1);
  test_assert(editDistance("abcdefgh", "x", 2) == 3); /* capped */
  test_assert(editDistance("abcdef", "badcfe", 1) == 2);
  test_assert(editDistance("SHRAP", "SHARP") == 1); /* a swap counts once */
  test_assert(editDistance("ab", "ba") == 1);

  {
    Suggester s("locaton");
    s.consider("location");
    s.consider("rotation");
    s.consider("scale");
    s.consider("locator");
    s.consider("location"); /* duplicates are ignored */
    Status st = Status::error(StatusCode::NotFound, "no member 'locaton'");
    s.addTo(st);
    test_assert(st.suggestions().size() == 2);
    test_assert(strcmp(st.suggestions()[0].c_str(), "location") == 0);
    test_assert(strcmp(st.suggestions()[1].c_str(), "locator") == 0);
  }
  {
    Suggester s("ab");
    s.consider("zz");
    s.consider("ab"); /* the name itself is not a suggestion */
    test_assert(s.count() == 0);
  }
  {
    /* At most three, closest first. */
    Suggester s("aaaaaaaaa");
    s.consider("aaaaaaaab");
    s.consider("aaaaaaabb");
    s.consider("aaaaaaaa");
    s.consider("aaaaaabbb");
    s.consider("aaaaaaaaaa");
    test_assert(s.count() == 3);
    Status st = Status::error(StatusCode::NotFound, "x");
    s.addTo(st);
    test_assert(strcmp(st.suggestions()[2].c_str(), "aaaaaaaaaa") == 0);
  }
  test_assert(isIdentifier("_a1"));
  test_assert(!isIdentifier("1a"));
  test_assert(!isIdentifier(""));
  test_assert(!isIdentifier("a-b"));
  return test_end();
}
