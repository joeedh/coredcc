#pragma once

/** \file
 * What a datapath can name, without an instance: for checking layouts and the like.
 */

#include "coredcc/api/resolve.h"

#include "litestl/util/vector.h"

#include <map>
#include <string_view>

namespace coredcc {

/** The structs something may be. */
struct StaticStructs {
  /** Registered ones, distinct. */
  litestl::util::Vector<StructId> structs;
  /** It may also be one that isn't registered, whose members aren't known. */
  bool unknown = false;
};

/** One thing that a path can name. */
struct StaticTarget {
  ResolvedPath::Kind kind = ResolvedPath::Kind::Struct;
  /** The struct itself (Struct), or the one that has the member. */
  StructId type;
  /** As in ResolvedPath. */
  const ApiMember *member = nullptr;
};

struct StaticResolution {
  litestl::util::Vector<StaticTarget> targets;
  /** It may also go through a struct that isn't registered. There may be no targets then.
   */
  bool unknown = false;
};

/**
 * Walks paths over every struct that a pointer or a collection's item may be: the
 * declared ones and those that inherit from them. A path is accepted if it fits one of
 * them, so it can still fail to resolve in a given document. Errors are resolve()'s; one
 * about a member that none of several structs has names them, and has suggestions from
 * all of them.
 *
 * Only static defs count: an enum key that a member's dynamic hook adds isn't found.
 * Structs that are registered later are found by later calls.
 */
class StaticResolver {
public:
  explicit StaticResolver(Runtime &rt) : reg_(rt.ext<ApiRegistry>())
  {
  }

  /** What `path` can name, from any of `from`. An empty path names `from`. */
  Result<StaticResolution> resolve(const StaticStructs &from, const Path &path);
  Result<StaticResolution> resolve(StructId from, std::string_view path);
  /** The structs that `path` can name, as a Context's prefix does: of a Pointer member,
   * those of its targets. InvalidArgument if it can't name one. */
  Result<StaticStructs> resolveStructs(const StaticStructs &from, const Path &path);
  /** `type` alone. Unknown if it isn't registered. */
  StaticStructs only(StructId type) const;
  /** `type` and the structs that inherit from it. Unknown if it isn't registered. */
  const StaticStructs &withSubtypes(StructId type);

private:
  struct Walk;

  ApiRegistry &reg_;
  /** Of a registry with `known_` structs. */
  std::map<uint64_t, StaticStructs> subtypes_;
  int known_ = 0;
};

} // namespace coredcc
