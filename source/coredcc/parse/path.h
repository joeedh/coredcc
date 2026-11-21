#pragma once

/** \file
 * Datapath strings (architecture §5):
 *
 *     objects["Cube"].modifiers[0].width
 *     objects[#12].location[2]
 *     scene.tool.mode[SMOOTH]
 *
 * Grammar, with no whitespace anywhere:
 *
 *     path    := member ( '.' member | '[' sub ']' )*
 *     member  := [A-Za-z_][A-Za-z0-9_]*
 *     sub     := integer | quoted | '#' integer | member
 *     quoted  := '"' ... '"' | '\'' ... '\''   escapes: \\ \" \' \n \t
 *
 * Integers are non-negative and must fit int64. NUL is never valid, raw or escaped.
 * `active` and `length` are ordinary members here; the resolver gives them meaning on
 * collections.
 */

#include "coredcc/base/status.h"

#include "litestl/util/string.h"
#include "litestl/util/vector.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace coredcc {

enum class SegmentKind : uint8_t {
  /** `.name` (or the leading name). */
  Member,
  /** `[3]`: a position in a collection, or a vector component. */
  Index,
  /** `["key"]`. */
  StringKey,
  /** `[#123]`: a stable handle. */
  HandleKey,
  /** `[KEY]`: an enum or flag key. */
  EnumKey,
};

struct PathSegment {
  SegmentKind kind = SegmentKind::Member;
  /** Member name, string key or enum key. */
  litestl::util::string text;
  /** Index or handle. */
  int64_t index = 0;
  /** Where the segment starts in the parsed text (its '.' or '['), for errors that
   * point at it. Not compared. */
  uint32_t offset = 0;

  bool operator==(const PathSegment &b) const;
};

/** A parsed path. */
struct Path {
  litestl::util::Vector<PathSegment> segments;

  int size() const
  {
    return int(segments.size());
  }
  const PathSegment &operator[](int i) const
  {
    return segments[i];
  }
  bool operator==(const Path &b) const;
  uint64_t computeHash() const;
  /** The canonical spelling: string keys double-quoted, integers without leading zeros.
   * parsePath(toString()) gives back an equal Path. */
  std::string toString() const;
};

/**
 * Parses a path. On error the Status message names the offset, and `errorOffset` (if
 * given) receives it: 0 <= offset <= text.size(), where text.size() means the input
 * ended too early ("a.", "a[").
 */
Result<Path> parsePath(std::string_view text, size_t *errorOffset = nullptr);

/** The longest path text parsePath accepts (offsets are 32-bit). */
inline constexpr size_t kMaxPathLength = 1u << 20;

} // namespace coredcc
