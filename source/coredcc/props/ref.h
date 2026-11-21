#pragma once

/** \file
 * Stable references to a property: an owner plus a key path that survives reordering
 * (architecture §5). Undo steps and Pointer values store these, never raw paths.
 */

#include "coredcc/base/block_id.h"
#include "coredcc/base/id.h"

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

  /** No owner, or a block owner with a null id. */
  bool isNull() const
  {
    return kind == Kind::None || (kind == Kind::Block && block.isNull());
  }
  bool operator==(const OwnerKey &b) const;
  uint64_t computeHash() const;
};

/** One step of a stable key: a member name, a string key, or a non-negative int64
 * handle. Never a positional index: those are turned into keys by canonicalize().
 * Names and keys must not contain NUL. */
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
 * An address that survives reordering: an owner, and key segments from the owner down.
 * Undo records address a property this way; component (`location[2]`) and flag-key
 * accesses refer to the whole member. A Pointer value addresses a struct instance, and
 * `type` names its struct, so a Pointer property can check its target. A null
 * StableRef (no owner) is the null Pointer value.
 */
struct StableRef {
  OwnerKey owner;
  litestl::util::Vector<KeySegment, 2> key;
  /** The addressed struct's type (Pointer values); null when unknown. */
  StructId type;

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
