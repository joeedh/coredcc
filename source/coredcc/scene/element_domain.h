#pragma once

/** \file
 * ElementDomain: the kinds of elements that Spatial, ElementAccess, picking and nearest
 * queries name. Bound to scripts as an enum (scene/bindings.cc).
 */

#include "litestl/binding/binding.h"

#include <cstdint>

namespace coredcc {

/** Kinds of elements. None is an object's bounds, which aren't an element. A mask of
 * domains has bit d set for the domain d. */
enum class ElementDomain : int32_t { None = -1, Point = 0, Edge = 1, Face = 2 };

/** The bit of `d` in a mask of domains; 0 for None. */
inline uint32_t domainBit(ElementDomain d)
{
  const int32_t i = int32_t(d);
  return i < 0 || i >= 32 ? 0u : 1u << i;
}

} // namespace coredcc

namespace litestl::binding {
template <> struct Binder<coredcc::ElementDomain> {
  static const BindingBase *bind();
};
} // namespace litestl::binding
