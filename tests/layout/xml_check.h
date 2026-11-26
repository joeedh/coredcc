#pragma once

/* Checks of parsed layouts, for the layout XML tests. */

#include "coredcc/layout/layout.h"
#include "coredcc/layout/xml.h"

#include "litestl/tests/test_util.h"

#include <cstdio>
#include <string>
#include <string_view>

namespace xmlcheck {

using namespace coredcc;

inline bool equals(const std::string &got, const std::string &want)
{
  if (got != want) {
    fprintf(stderr, "got  %s\nwant %s\n", got.c_str(), want.c_str());
  }
  return got == want;
}

inline Layout parsed(std::string_view xml)
{
  Result<Layout> r = parseLayout(xml);
  if (!r.ok()) {
    fprintf(stderr, "%s\n", r.status().message());
  }
  test_assert(r.ok());
  return r.ok() ? std::move(*r) : Layout();
}

inline std::string json(std::string_view xml)
{
  return layoutToJson(parsed(xml));
}

/* The error is `what`, at `line` and `column`. */
inline bool fails(std::string_view xml,
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
    fprintf(stderr,
            "got  %s\nwant %s\n",
            r.ok() ? "a layout" : r.status().message(),
            want.c_str());
  }
  return ok;
}

/* ... in the root. */
inline bool failsIn(const std::string &inner, const std::string &what, int column)
{
  return fails("<layout>" + inner + "</layout>", what, 1, 8 + column);
}

} // namespace xmlcheck
