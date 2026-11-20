#include "coredcc/base/assert.h"

#include <cstdio>
#include <cstdlib>

namespace coredcc::detail {

void assertFailed(const char *expr, const char *msg, const char *file, int line)
{
  fprintf(stderr, "%s:%d: coredcc assertion failed: %s (%s)\n", file, line, expr, msg);
  fflush(stderr);
  abort();
}

} // namespace coredcc::detail
