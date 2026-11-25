#pragma once

/** \file
 * A socket's key, for hash maps.
 */

#include "coredcc/base/id.h"

#include "litestl/util/string.h"

#include <cstdint>
#include <cstring>

namespace coredcc::detail {

/** With FNV-1a as its hash: litestl's own of a string has 19 bits, and loses some of those to
 * even characters, so it gives the same for many keys that differ in their digits. */
struct SocketKey {
  litestl::util::string text;

  uint64_t computeHash() const
  {
    return fnv1a64(text.c_str());
  }
  bool operator==(const SocketKey &b) const
  {
    return strcmp(text.c_str(), b.text.c_str()) == 0;
  }
};

} // namespace coredcc::detail
