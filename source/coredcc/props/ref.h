#pragma once

/** \file
 * Stable references to a property: an owner plus a key path that survives reordering
 * (architecture §5). Undo steps and Pointer values store these, never raw paths.
 */

#include "coredcc/base/block_id.h"

#include "litestl/util/string.h"
#include "litestl/util/vector.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace coredcc {

/** Who owns a property: a datablock, or a named runtime owner (a preferences struct, a
 * tool's settings). */
struct OwnerKey {
  enum class Kind : uint8_t { None, Block, Runtime };

  Kind kind = Kind::None;
  /** Kind::Block. */
  BlockId block;
  /** Kind::Runtime: the hashed name the owner registered under. */
  uint64_t nameHash = 0;

  static OwnerKey ofBlock(BlockId id)
  {
    OwnerKey k;
    k.kind = Kind::Block;
    k.block = id;
    return k;
  }
  static OwnerKey ofRuntime(uint64_t nameHash)
  {
    OwnerKey k;
    k.kind = Kind::Runtime;
    k.nameHash = nameHash;
    return k;
  }

  bool isNull() const
  {
    return kind == Kind::None;
  }
  bool operator==(const OwnerKey &b) const;
  uint64_t computeHash() const;
};

/** One step of a stable key: a member name, a string key, or an int64 handle. Never a
 * positional index: those are turned into keys by canonicalize(). */
struct KeySegment {
  enum class Kind : uint8_t { Member, StringKey, Handle };

  Kind kind = Kind::Member;
  litestl::util::string text;
  int64_t handle = 0;

  static KeySegment member(std::string_view name);
  static KeySegment stringKey(std::string_view key);
  static KeySegment ofHandle(int64_t handle);

  bool operator==(const KeySegment &b) const;
};

/**
 * A property address that survives reordering: an owner, and key segments from the
 * owner down to one member. Component (`location[2]`) and flag-key accesses refer to
 * the whole member. A null StableRef (no owner) is the null Pointer value.
 */
struct StableRef {
  OwnerKey owner;
  litestl::util::Vector<KeySegment> key;

  bool isNull() const
  {
    return owner.isNull();
  }
  bool operator==(const StableRef &b) const;
  uint64_t computeHash() const;
  /** A debug form, e.g. `{block 1a2b..:..}.modifiers["Bevel"].width`. */
  std::string toString() const;
};

} // namespace coredcc
