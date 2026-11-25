#pragma once

/** \file
 * `graph.foreach`: evaluates a def for each element of a table.
 *
 * Its inputs are `elements`, a table, and a socket per interface input of its def. For each
 * element, an interface input has the value of the table's column of that name; without one,
 * every element has the node's own input. Columns no input is named after are ignored.
 * Its output `elements` is a table of the same keys, with a column per interface output.
 *
 * Elements run in table order, up to the first that fails, which fails the node: its
 * GraphError has the element's key and the node that failed inside.
 *
 * When the table changes, elements whose row is the same as in the last one that succeeded
 * don't run. When something else does, all do. The first `maxCached` elements of the table
 * have a state of their own, found by key, so that only what changed for an element runs
 * again; the others share one, and all of their kernels run. Results don't depend on which
 * is which, and NodeReport has the numbers. An element that isn't in the table any more keeps
 * its state until a run succeeds.
 */

#include "coredcc/graph/payload.h"
#include "coredcc/graph/value.h"
#include "coredcc/props/def.h"

#include "litestl/util/string.h"
#include "litestl/util/vector.h"

#include <cstdint>
#include <string_view>

namespace coredcc {

/** A table: a payload with the socket type `elements`. Null counts as an empty one. */
struct Elements {
  struct Column {
    litestl::util::string name;
    litestl::util::Vector<GraphValue, 1> values;
  };
  /** Unique. They identify elements when the table changes. */
  litestl::util::Vector<int64_t, 1> keys;
  litestl::util::Vector<Column, 1> columns;

  int size() const
  {
    return int(keys.size());
  }
  /** The column of that name, or null. */
  const Column *column(std::string_view name) const;
  /** InvalidArgument for a key or column name that is there twice, or a column with more or
   * fewer values than there are keys. `checkKeys` off: the keys are known to be unique. */
  Status validate(bool checkKeys = true) const;
  bool operator==(const Elements &b) const;
};

template <> struct PayloadTraits<Elements> {
  static constexpr const char *name = "elements";
  static bool equal(const Elements &a, const Elements &b)
  {
    return a == b;
  }
  static size_t bytes(const Elements &e);
};

/** The sockets of `graph.foreach` that hold tables. */
inline constexpr const char *kElementsSocket = "elements";
/** Its param: how many elements have a state of their own, if there is room for it (see
 * GraphInstance::setMaxCachedKernels). */
inline constexpr const char *kMaxCachedParam = "maxCached";
PropertyDef foreachMaxCachedParam();

} // namespace coredcc
