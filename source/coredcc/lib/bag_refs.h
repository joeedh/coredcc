#pragma once

#include "coredcc/lib/block.h"
#include "coredcc/props/bag.h"

#include "litestl/util/vector.h"

#include <string_view>

namespace coredcc {

/**
 * The RefCells of a PropertyBag's pointers into blocks. A provider keeps one beside each bag in
 * its data, and reports its cells from foreachRef.
 *
 * The cell is the truth for which block a pointer is into: remapping, unlinking and undo write
 * it, not the bag. The bag's value has the rest of the pointer, and a copy of the id that can be
 * out of date, so read and set Pointer properties through this class.
 */
class BagRefs {
 public:
  explicit BagRefs(RefKind kind = RefKind::OptStrong) : kind_(kind) {}

  /** What new cells are. */
  RefKind kind() const
  {
    return kind_;
  }
  /** PropertyBag::setAt, and the cell. */
  Status setAt(PropertyBag &bag, int index, const PropValue &value);
  /** PropertyBag::set, and the cell. */
  Status set(PropertyBag &bag, std::string_view name, const PropValue &value);
  /** PropertyBag::reset, and the cell. */
  Status reset(PropertyBag &bag, std::string_view name);

  /** The bag's value, with the block its cell names. A pointer whose cell is null is null. */
  PropValue value(const PropertyBag &bag, int index) const;
  /** Whether the bag's entry was set, or its cell has a target. */
  bool isSet(const PropertyBag &bag, int index) const;

  /** The cell of a Pointer property, for PointerAccessor::cell: made (null) if there is none. */
  RefCell &cell(std::string_view name);
  /** Null if there is none. */
  const RefCell *find(std::string_view name) const;
  /** Visits every cell. */
  void visit(RefVisitor &visitor);

 private:
  struct Entry {
    litestl::util::string name;
    RefCell cell;
  };

  RefKind kind_;
  litestl::util::Vector<Entry, 1> entries_;
};

} // namespace coredcc
