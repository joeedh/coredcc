#pragma once

#include "coredcc/base/id.h"

#include <string_view>

namespace coredcc::detail {

/** The id of a block type, attachment type or slot namespace by a name that was stored, or that
 * a caller gave as text; null for an empty name. Every such lookup goes through here, so that
 * aliases for renamed types have one place to go. */
inline TypeId typeNamed(std::string_view name)
{
  return name.empty() ? TypeId() : TypeId::of(name);
}

} // namespace coredcc::detail
