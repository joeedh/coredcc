#pragma once

/** \file
 * Checking a Layout against what is registered.
 */

#include "coredcc/api/static_resolve.h"
#include "coredcc/layout/layout.h"

#include "litestl/util/vector.h"

namespace coredcc {

enum class LayoutSeverity : uint8_t {
  /** It can't be checked: it goes through a struct that isn't registered (yet), or names
   * a key that a dynamic def may have. Or a Command's command needs something of the
   * frontend, or the text is of a newer version and has what this one skipped. */
  Warning,
  /** It can't work. */
  Error,
};

struct LayoutIssue {
  int node = 0;
  /** In the layout's text: of the attribute, or of the element. */
  uint32_t offset = 0;
  LayoutSeverity severity = LayoutSeverity::Error;
  /** "<prop> path: no member 'x' in Object (at offset 0)", with suggestions. */
  Status status;
};

/** At most this many issues are reported. If there are more, the last one says so. */
inline constexpr int kMaxLayoutIssues = 1000;

/**
 * Checks what `layout` names, in a Context whose root is a `root` struct, without one:
 * - a Prop's path is a value, a pointer, a component, an enum key or a length;
 * - a container's path is a struct, or a pointer (its target is meant). A Custom's path
 * may be anything;
 * - conditions are Bools, enum keys, pointers, lengths or Strings;
 * - a Command's command is registered, and has the inputs it gives, which take the
 * literals. A pointer's path is checked from the prefix, which is the Command's `path` if
 * it has one,
 * and for the input's struct. An Interactive command is a Warning: a frontend begins it
 * with beginInteractive. So is a Private one, which isn't meant for users;
 * - a CommandOptions' command is registered, and the root has the last-used inputs.
 *
 * Paths are checked by StaticResolver, so a path that fits one of the structs a pointer
 * can have is fine. Call it when all structs and commands are registered.
 */
litestl::util::Vector<LayoutIssue>
validateLayout(Runtime &rt, StructId root, const Layout &layout);

/** Whether none of `issues` is an Error. */
bool layoutIsValid(const litestl::util::Vector<LayoutIssue> &issues);

} // namespace coredcc
