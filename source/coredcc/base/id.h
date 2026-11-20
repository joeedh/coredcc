#pragma once

/** \file
 * Hashed ids for named things: types, interfaces, clients.
 *
 * An id is FNV-1a 64 over a domain tag followed by the stable name, e.g.
 * `"type:" "coredcc.Object"`. The algorithm and the tags are part of the file format
 * contract and never change. Collisions are detected when names are registered with a
 * Runtime; files always store names, and ids are rebuilt from them on load.
 */

#include <compare>
#include <cstdint>
#include <string_view>

namespace coredcc {

namespace detail {
inline constexpr uint64_t kFnvOffset = 0xcbf29ce484222325ull;
inline constexpr uint64_t kFnvPrime = 0x100000001b3ull;

constexpr uint64_t fnv1a64(std::string_view s, uint64_t h = kFnvOffset)
{
  for (char c : s) {
    h ^= static_cast<uint8_t>(c);
    h *= kFnvPrime;
  }
  return h;
}
} // namespace detail

/** The domains ids are hashed in. The tag string is hashed before the name. */
enum class IdDomain : uint8_t { Type, Interface, Client };

constexpr std::string_view idDomainTag(IdDomain d)
{
  switch (d) {
    case IdDomain::Type:
      return "type:";
    case IdDomain::Interface:
      return "iface:";
    case IdDomain::Client:
      return "client:";
  }
  return "?:";
}

constexpr uint64_t hashId(IdDomain domain, std::string_view name)
{
  return detail::fnv1a64(name, detail::fnv1a64(idDomainTag(domain)));
}

/** A strongly typed 64-bit id in one domain. Zero is the null id. */
template <IdDomain D> struct Id {
  static constexpr IdDomain domain = D;
  uint64_t value = 0;

  static constexpr Id of(std::string_view name)
  {
    return Id{hashId(D, name)};
  }
  constexpr bool isNull() const
  {
    return value == 0;
  }
  constexpr explicit operator bool() const
  {
    return value != 0;
  }
  constexpr auto operator<=>(const Id &) const = default;
  uint64_t computeHash() const
  {
    return value;
  }
};

using TypeId = Id<IdDomain::Type>;
using InterfaceId = Id<IdDomain::Interface>;
using ClientId = Id<IdDomain::Client>;

} // namespace coredcc
