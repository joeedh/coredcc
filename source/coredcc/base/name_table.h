#pragma once

#include "coredcc/base/id.h"

#include "litestl/util/string.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <string_view>

namespace coredcc {

/**
 * Names of ids that only a file knows: a missing add-on's struct or client, a runtime
 * owner nobody registered. They never go into the Runtime's registry, where a file's name
 * could make a plug-in's registration fail.
 *
 * Bounded: a table holds at most kMaxBytes of names (counting kEntryBytes for each).
 */
class NameTable {
 public:
  static constexpr size_t kMaxBytes = size_t(16) << 20;
  static constexpr size_t kEntryBytes = 64;

  /** False when the table is full, the name is empty or holds a NUL, or its id has
   * another name. Adding a name twice is fine. */
  bool add(IdDomain domain, std::string_view name);
  /** The name of `id`, or null. */
  const char *nameOf(IdDomain domain, uint64_t id) const;

  /** Test hook: add() for a chosen id, to exercise collisions. */
  bool debugAddAs(IdDomain domain, uint64_t id, std::string_view name);

 private:
  /* Ordered: a file chooses the ids, and litestl's hash of a number is easy to invert. */
  std::map<uint64_t, litestl::util::string> names_[kIdDomainCount];
  size_t bytes_ = 0;
};

} // namespace coredcc
