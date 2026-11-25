#pragma once

#include "coredcc/lib/block_io.h"

#include "litestl/util/string.h"
#include "litestl/util/vector.h"

namespace coredcc {

/**
 * The channels of one object as a file has them, for TransformModel::upgrade. Values are by the
 * names that the old version's channels had, and an Enum is its item's key, a String.
 */
class ChannelEntries {
 public:
  explicit ChannelEntries(litestl::util::Vector<BlockEntry> &entries) : entries_(entries) {}

  /** The value, or None if there isn't one, or it is of a type that this build doesn't know. */
  PropValue get(const litestl::util::string &name) const;
  /** Sets it, or adds it. */
  void set(const litestl::util::string &name, const PropValue &value);
  /** False if there isn't one. */
  bool remove(const litestl::util::string &name);
  /** Replaces what is named `to`. False if there is no `from`. */
  bool rename(const litestl::util::string &from, const litestl::util::string &to);

  static litestl::binding::types::Struct<ChannelEntries> *defineBindings();

 private:
  litestl::util::Vector<BlockEntry> &entries_;
};

} // namespace coredcc
