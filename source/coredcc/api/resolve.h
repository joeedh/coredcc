#pragma once

/** \file
 * Resolving datapaths and StableRefs, reading and writing through them, and
 * canonicalizing them for undo (architecture §5, "Paths").
 */

#include "coredcc/api/struct.h"
#include "coredcc/parse/path.h"
#include "coredcc/props/value.h"

#include "litestl/util/vector.h"

#include <span>
#include <string_view>

namespace coredcc {

/** One (owner, member) a resolution passed through: what the ChangeBus matches. */
struct PathTouch {
  OwnerKey owner;
  litestl::util::string member;
};

/** A resolved path. C++ only and transient: it holds raw pointers into live data. */
struct ResolvedPath {
  enum class Kind : uint8_t {
    /** The path names a struct instance (`objects["A"]`). */
    Struct,
    /** A member of `ptr`. */
    Member,
    /** One component of a vector member (`location[2]`). */
    Component,
    /** One key of an Enum or Flags member (`flags[X]`), read as a Bool. */
    EnumKey,
    /** A collection's `length` pseudo-member, a read-only Int. */
    Length,
  };

  Kind kind = Kind::Struct;
  /** The struct itself (Struct), or the struct holding the member. */
  PointerRef ptr;
  /** The member (null for Struct). For Length, the collection. */
  const ApiMember *member = nullptr;
  int component = -1;
  litestl::util::string enumKey;
  /** The key's value (EnumKey), as the member's def is now. */
  int64_t enumValue = 0;
  /**
   * The canonical address: rooted at the innermost owner, with positions and `active`
   * replaced by keys, and component or enum-key accesses widened to the whole member.
   * For Struct, the struct's own address and type.
   */
  StableRef canonical;
  litestl::util::Vector<PathTouch, 4> touched;
};

/** Resolves `path` from the context root, or from its prefix unless `path` starts with `/`.
 * Errors name the failing segment's offset and suggest members. */
Result<ResolvedPath> resolve(Context &ctx, const Path &path);
Result<ResolvedPath> resolve(Context &ctx, std::string_view path);
/** As resolve, for writing: a path that ends in `.active` of a collection with an
 * `activeMember` resolves to that member, also when there is no active item. */
Result<ResolvedPath> resolveForWrite(Context &ctx, const Path &path);
Result<ResolvedPath> resolveForWrite(Context &ctx, std::string_view path);
/** Resolves a StableRef from its owner. A target that is gone (a deleted block, a
 * missing key) fails with NotFound, never a crash. */
Result<ResolvedPath> resolve(Context &ctx, const StableRef &ref);

/** The canonical StableRef of `path` (see ResolvedPath::canonical). */
Result<StableRef> canonicalize(Context &ctx, const Path &path);

/** The value at a resolved Member, Component, EnumKey or Length; a Pointer member reads
 * as a Pointer value addressing its target. */
Result<PropValue> getValue(Context &ctx, const ResolvedPath &rp);
Result<PropValue> getValue(Context &ctx, std::string_view path);

/** The def a resolved path reads and writes with: dynamic hooks applied. Component and
 * enum-key paths give the whole member's def; Length gives a read-only Int. */
Result<PropertyDef> getMeta(Context &ctx, const ResolvedPath &rp);
Result<PropertyDef> getMeta(Context &ctx, std::string_view path);
/** As getMeta, but a component's or an enum key's is the whole member's, and a struct's or a
 * length's is an error. */
Result<PropertyDef> getMemberMeta(Context &ctx, const ResolvedPath &rp);

/**
 * The single write path:
 * 1. reject read-only members;
 * 2. coerce (and clamp) to the member's def;
 * 3. call the accessor;
 * 4. notify the ChangeBus, whose listeners run now;
 * 5. call the member's update hook (after 4, so a hook that evaluates a graph sees the
 *    new value).
 * Busy, with nothing written, while a graph evaluates (Runtime::evaluating).
 * A Pointer member takes a Pointer value (resolved to its target) and bumps the
 * library's reference epoch. Path strings are resolved with resolveForWrite.
 */
Status setValue(Context &ctx, const ResolvedPath &rp, const PropValue &value);
Status setValue(Context &ctx, std::string_view path, const PropValue &value);

namespace detail {
/** `s` with the segment's offset appended, keeping its suggestions. */
Status atSegment(const PathSegment &seg, const Status &s);
} // namespace detail

/** Many reads in one call, for UIs and bindings. `values[i]` is None where
 * `errors[i]` holds the failure. Paths go through the Runtime's PathCache. */
void getValues(Context &ctx,
               std::span<const std::string_view> paths,
               litestl::util::Vector<PropValue> &values,
               litestl::util::Vector<Status> &errors);
/** Of parsed paths, such as a Layout gives. None may be null. */
void getValues(Context &ctx,
               std::span<const Path *const> paths,
               litestl::util::Vector<PropValue> &values,
               litestl::util::Vector<Status> &errors);

} // namespace coredcc
