#pragma once

#include "coredcc/base/flags.h"

#include <cstdint>

namespace coredcc {

/** What changed about a block's own data, for whoever keeps something made from it. */
enum class InvalidateKinds : uint32_t {
  None = 0,
  Topology = 1 << 0,
  Positions = 1 << 1,
  Attributes = 1 << 2,
  Selection = 1 << 3,
  Materials = 1 << 4,
};
CDCC_FLAG_OPERATORS(InvalidateKinds)

/** A ChangeBus aspect (api/change_bus.h), beside the kinds above: an object's driven
 * channels (OverrideProvider) have other values. Its stored data didn't change. */
inline constexpr uint32_t kDrivenAspect = 1u << 15;

} // namespace coredcc
