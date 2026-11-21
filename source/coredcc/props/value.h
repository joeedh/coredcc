#pragma once

/** \file
 * PropValue: one property value of any kind, as the datapath layer and commands pass
 * them around (architecture §4).
 */

#include "coredcc/props/kind.h"
#include "coredcc/props/ref.h"

#include "litestl/util/string.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace coredcc {

/**
 * An opaque tagged value. Vectors, matrices and quaternions are doubles; Mat4 is
 * column-major, Quat is xyzw.
 *
 * operator== and hash() compare floating-point components bitwise, so NaN equals
 * itself and -0.0 differs from 0.0. That keeps the two consistent with each other, and
 * a watcher of a NaN value doesn't fire on every flush. Use approxEqual() for
 * numeric closeness.
 *
 * The `as*` accessors abort on the wrong type (a programmer error); the `try*` ones
 * return nothing instead.
 */
class PropValue {
 public:
  enum class Type : uint8_t {
    None,
    Bool,
    Int,
    Float,
    String,
    Vec2,
    Vec3,
    Vec4,
    Mat4,
    Quat,
    Enum,
    Flags,
    Pointer,
  };

  PropValue() = default;

  static PropValue ofBool(bool v);
  static PropValue ofInt(int64_t v);
  static PropValue ofFloat(double v);
  static PropValue ofString(std::string_view v);
  static PropValue ofVec2(double x, double y);
  static PropValue ofVec3(double x, double y, double z);
  static PropValue ofVec4(double x, double y, double z, double w);
  /** Vec2/3/4, Mat4 or Quat from `componentCount(type)` doubles. */
  static PropValue ofComponents(Type type, const double *components);
  static PropValue ofQuat(double x, double y, double z, double w);
  static PropValue identityMat4();
  static PropValue ofEnum(int64_t v);
  static PropValue ofFlags(uint64_t v);
  static PropValue ofPointer(StableRef ref);
  static PropValue nullPointer()
  {
    return ofPointer(StableRef());
  }

  Type type() const
  {
    return type_;
  }
  bool isNone() const
  {
    return type_ == Type::None;
  }

  bool asBool() const;
  int64_t asInt() const;
  double asFloat() const;
  const litestl::util::string &asString() const;
  int64_t asEnum() const;
  uint64_t asFlags() const;
  const StableRef &asPointer() const;

  /** Component count of the stored type: 1 for Float, 2-4 for vectors and Quat, 16 for
   * Mat4, 0 otherwise. */
  int componentCount() const;
  /** Component `i` of a Float, vector, Mat4 or Quat. */
  double component(int i) const;

  std::optional<bool> tryBool() const;
  std::optional<int64_t> tryInt() const;
  std::optional<double> tryFloat() const;
  const litestl::util::string *tryString() const;
  std::optional<int64_t> tryEnum() const;
  std::optional<uint64_t> tryFlags() const;
  const StableRef *tryPointer() const;

  bool operator==(const PropValue &b) const;
  bool operator!=(const PropValue &b) const
  {
    return !operator==(b);
  }
  uint64_t hash() const;
  uint64_t computeHash() const
  {
    return hash();
  }

  /** Same type, and numeric components within `eps` (absolute). Non-numeric parts
   * compare with ==. */
  bool approxEqual(const PropValue &b, double eps) const;

  /** A readable form: `true`, `3`, `0.5`, `"text"`, `(1, 2, 3)`, `#4` for an enum,
   * `0x5` for flags, or a StableRef's debug form. */
  std::string toString() const;

 private:
  Type type_ = Type::None;
  union {
    bool b_;
    int64_t i_;
    uint64_t u_;
    double d_[16] = {};
  };
  litestl::util::string str_;
  StableRef ref_;
};

const char *propValueTypeName(PropValue::Type type);

/** The value type a property of `kind` stores (None for Collection). */
PropValue::Type valueTypeFor(PropKind kind);

/** Component count of a value type (see PropValue::componentCount). */
int componentCount(PropValue::Type type);

} // namespace coredcc
