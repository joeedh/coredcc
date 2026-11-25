#pragma once

#include "coredcc/props/def.h"

#include "litestl/util/vector.h"

#include <string_view>

namespace coredcc {

/**
 * An ordered set of properties with values: a command's inputs or outputs, a tool's
 * settings. Each entry remembers whether it was set explicitly.
 *
 * An unset entry holds its default value, except an Optional one, which holds None
 * (setting None also unsets it). A dynamic def is resolved with the bag as its owner,
 * so one input's range can depend on another's value. Pointers from get() and value()
 * are invalidated by add().
 */
class PropertyBag {
public:
  /** Adds a property after validating its def. Names are unique. */
  Status add(PropertyDef def);

  int size() const
  {
    return int(entries_.size());
  }
  const PropertyDef &def(int i) const
  {
    return entries_[i].def;
  }
  const PropValue &value(int i) const
  {
    return entries_[i].value;
  }
  bool wasSet(int i) const
  {
    return entries_[i].wasSet;
  }

  /** The index of `name`, or -1. */
  int indexOf(std::string_view name) const;
  /** The def of entry `i` as it is now: a dynamic one is resolved with the bag. */
  PropertyDef resolvedDef(int i) const;
  /** The indices, with those of dynamic defs last: the order to set them in. */
  litestl::util::Vector<int> setOrder() const;
  /** The value of `name`, or null if there is no such property. */
  const PropValue *get(std::string_view name) const;
  bool isSet(std::string_view name) const;

  /** Coerces `value` to the property's kind and stores it. An unknown name is NotFound,
   * with suggestions. */
  Status set(std::string_view name, const PropValue &value);
  Status setAt(int index, const PropValue &value);
  /** Back to the unset value. */
  Status reset(std::string_view name);
  void resetAll();

  /** Bound opaque, with a copy constructor: scripts read bags through bind/'s
   * facades, and never write them (a recorded command's inputs are read again later). */
  static const litestl::binding::types::Struct<PropertyBag> *defineBindings();

private:
  struct Entry {
    PropertyDef def;
    PropValue value;
    bool wasSet = false;
  };
  static PropValue unsetValue(const PropertyDef &def);
  Status notFound(std::string_view name) const;

  litestl::util::Vector<Entry> entries_;
};

} // namespace coredcc
