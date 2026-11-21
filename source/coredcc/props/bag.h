#pragma once

#include "coredcc/props/def.h"

#include "litestl/util/vector.h"

#include <string_view>

namespace coredcc {

/**
 * An ordered set of properties with values: a command's inputs or outputs, a tool's
 * settings. Each entry remembers whether it was set explicitly.
 *
 * An unset entry holds its default value, except an Optional one, which holds None.
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
  /** The value of `name`, or null if there is no such property. */
  const PropValue *get(std::string_view name) const;
  bool isSet(std::string_view name) const;

  /** Coerces `value` to the property's kind and stores it. An unknown name is NotFound,
   * with suggestions. */
  Status set(std::string_view name, const PropValue &value);
  /** Back to the unset value. */
  void reset(std::string_view name);
  void resetAll();

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
