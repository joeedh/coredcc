#include "coredcc/base/assert.h"

#include <cstdio>
#include <cstdlib>

namespace coredcc::detail {

/** Whether `path` starts with `prefix`, either separator standing for `/`. */
static bool startsWithPath(const char *path, const char *prefix)
{
  for (; *prefix; path++, prefix++) {
    if ((*path == '\\' ? '/' : *path) != *prefix) {
      return false;
    }
  }
  return true;
}

const char *pathInCheckout(const char *file)
{
  const char *found = file;
  for (const char *at = file; *at; at++) {
    bool starts = at == file || at[-1] == '/' || at[-1] == '\\';
    if (starts && (startsWithPath(at, "source/coredcc/") || startsWithPath(at, "tests/")))
    {
      found = at;
    }
  }
  return found;
}

void assertFailed(const char *expr, const char *msg, const char *file, int line)
{
  fprintf(stderr,
          "%s:%d: coredcc assertion failed: %s (%s)\n",
          pathInCheckout(file),
          line,
          expr,
          msg);
  fflush(stderr);
  abort();
}

} // namespace coredcc::detail
