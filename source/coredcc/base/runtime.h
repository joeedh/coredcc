#pragma once

#include "coredcc/base/id.h"
#include "coredcc/base/status.h"

#include "litestl/util/map.h"
#include "litestl/util/string.h"

#include <string_view>

namespace coredcc {

/**
 * The root of one coredcc instance. Every registry belongs to a Runtime; there is no
 * process-global one. Several Runtimes may coexist in a process (e.g. two hosts).
 *
 * In P0 it holds only the name registry, which turns hashed ids back into names and
 * rejects collisions. Later phases add the interface, struct, command and tool
 * registries.
 */
class Runtime {
 public:
  Runtime() = default;
  Runtime(const Runtime &) = delete;
  Runtime &operator=(const Runtime &) = delete;

  /**
   * Register `name` in `domain`, returning its id. Registering the same name twice is
   * fine and returns the same id; a different name that hashes to the same id is an
   * AlreadyExists error naming both.
   */
  Result<uint64_t> registerName(IdDomain domain, std::string_view name);

  template <IdDomain D> Result<Id<D>> registerId(std::string_view name)
  {
    Result<uint64_t> r = registerName(D, name);
    if (!r.ok()) {
      return r.status();
    }
    return Id<D>{*r};
  }

  /** Test hook: register `name` under a forced id, to exercise collision handling. */
  Result<uint64_t> debugRegisterNameAs(IdDomain domain, uint64_t id, std::string_view name);

  /** The registered name for an id, or nullptr. */
  const char *nameOf(IdDomain domain, uint64_t id) const;
  template <IdDomain D> const char *nameOf(Id<D> id) const
  {
    return nameOf(D, id.value);
  }

 private:
  litestl::util::Map<uint64_t, litestl::util::string> names_[3];
};

} // namespace coredcc
