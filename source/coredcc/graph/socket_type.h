#pragma once

/** \file
 * Socket types, and the conversions between them.
 */

#include "coredcc/base/runtime.h"
#include "coredcc/graph/value.h"
#include "coredcc/props/def.h"

#include "litestl/binding/binding.h"
#include "litestl/util/map.h"
#include "litestl/util/string.h"
#include "litestl/util/vector.h"

#include <string_view>

namespace coredcc {

/** A socket type. Registered ones live as long as the Runtime, at a stable address, so
 * they are compared by address. */
struct SocketType {
  litestl::util::string name;
  /** Its hash, under which files and other types find it. */
  TypeId id;
  GraphValue::Type valueType = GraphValue::Type::None;
  /** The payload type, for valueType Payload. */
  TypeId payload;
  /** An unlinked socket's value when nothing else gives one: zero, the identity, "", or
   * the null payload. */
  GraphValue defaultValue;

  /** Whether `v` may be a value of this socket type. */
  bool holds(const GraphValue &v) const;
  /** False for payloads, which have no literal. */
  bool hasLiteral() const
  {
    return valueType != GraphValue::Type::Payload;
  }
  /** The PropertyDef of a literal of this type. hasLiteral() only. */
  PropertyDef literalDef(std::string_view apiName) const;
};

/** Converts `in`, a value of the source type, to the target type. */
using SocketConvertFn = Status (*)(const GraphValue &in, GraphValue &out);

/**
 * Socket types and conversions. A Runtime extension, created with the types `bool`,
 * `int`, `float`, `vec2`, `vec3`, `vec4`, `mat4`, `quat` and `string`, and conversions
 * between bool, int and float in every direction, and from float to the vectors. Float to
 * int truncates and saturates, and NaN becomes 0.
 *
 * What is registered can't be replaced or removed. Registering is Busy while a graph
 * evaluates.
 */
class SocketTypeRegistry {
 public:
  static constexpr const char *kExtName = "coredcc.SocketTypeRegistry";

  explicit SocketTypeRegistry(Runtime *rt);
  ~SocketTypeRegistry();
  SocketTypeRegistry(const SocketTypeRegistry &) = delete;
  SocketTypeRegistry &operator=(const SocketTypeRegistry &) = delete;

  /** Registers payload type T, as a socket type named PayloadTraits<T>::name. Registering
   * T again is fine; another C++ type under its name is AlreadyExists. */
  template <class T> Result<const SocketType *> addPayloadType()
  {
    return addPayload(PayloadTraits<T>::name, litestl::binding::types::typeKeyOf<T>());
  }
  /** Adds a conversion between two registered types. AlreadyExists if there is one. */
  Status addConversion(std::string_view from, std::string_view to, SocketConvertFn fn);

  /** The type, or null. */
  const SocketType *find(std::string_view name) const;
  /** The NotFound error for `name`, with suggestions. */
  Status unknown(std::string_view name) const;
  /** The built-in type of a value type; null for None and Payload. */
  const SocketType *builtin(GraphValue::Type type) const;

  /** Whether a socket of `from` can feed one of `to`: the same type, or a conversion. */
  bool canConvert(const SocketType &from, const SocketType &to) const;
  /** `in`, a value of `from`, as a value of `to`. TypeMismatch without a conversion. A
   * conversion that gives a value of another type is Internal. */
  Status convert(const SocketType &from,
                 const SocketType &to,
                 const GraphValue &in,
                 GraphValue &out) const;

  Status validate() const;

 private:
  struct Entry {
    SocketType type;
    /** The payload's C++ type (litestl's typeKeyOf). */
    const void *typeKey = nullptr;
  };
  struct PairKey {
    uint64_t from = 0, to = 0;
    uint64_t computeHash() const
    {
      return from * 0x9E3779B97F4A7C15ull ^ to;
    }
    bool operator==(const PairKey &b) const = default;
  };

  Result<const SocketType *> addPayload(const char *name, const void *typeKey);
  Result<Entry *> addEntry(SocketType type);
  Entry *entry(std::string_view name) const;

  Runtime &rt_;
  litestl::util::Vector<Entry *> types_;
  litestl::util::Map<uint64_t, Entry *> byId_;
  litestl::util::Map<PairKey, SocketConvertFn> conversions_;
};

} // namespace coredcc
