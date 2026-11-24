#include "coredcc/base/assert.h"

#include "test_util.h"

#include <cstring>

test_init;

using coredcc::detail::pathInCheckout;

int main()
{
  auto shows = [](const char *file, const char *expected) {
    return strcmp(pathInCheckout(file), expected) == 0;
  };
  test_assert(shows("C:\\dev\\coredcc\\source\\coredcc\\base\\text.cc",
                    "source\\coredcc\\base\\text.cc"));
  /* clang-cl mixes the separators */
  test_assert(shows("C:\\dev\\coredcc-a\\source\\coredcc/base/status.h",
                    "source\\coredcc/base/status.h"));
  test_assert(shows("/home/a/coredcc/tests/base/x.cc", "tests/base/x.cc"));
  /* the last one counts */
  test_assert(shows("/tests/coredcc/source/coredcc/a.h", "source/coredcc/a.h"));
  test_assert(shows("source/coredcc/a.h", "source/coredcc/a.h"));
  test_assert(shows("/mytests/a.h", "/mytests/a.h"));
  test_assert(shows("/resource/coredcc/a.h", "/resource/coredcc/a.h"));
  test_assert(shows("", ""));
  return test_end();
}
