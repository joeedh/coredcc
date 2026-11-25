#pragma once

/** \file
 * GraphValue: what flows along graph edges.
 */

#include "coredcc/base/status.h"
#include "coredcc/graph/payload.h"
#include "coredcc/props/value.h"

#include "litestl/util/string.h"

#include <cstdint>
#include <string_view>

namespace coredcc {

/**
 * The value of a socket, 40 bytes. Numbers and vectors are inline. The components of a Mat4
 * are shared between its copies and never change, so copying one never allocates (copying a
 * Mat4 PropValue does), but making one does. Mat4 is column-major, Quat is xyzw.
 *
 * `==` compares floats bitwise, as PropValue does (NaN equals itself), and payloads by
 * PayloadRef's `==`. The `as*` accessors abort on the wrong type.
 */
class GraphValue {
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
    Payload,
  };

  GraphValue() {}
  GraphValue(const GraphValue &b);
  GraphValue(GraphValue &&b) noexcept;
  GraphValue &operator=(const GraphValue &b);
  GraphValue &operator=(GraphValue &&b) noexcept;
  ~GraphValue()
  {
    reset();
  }

  static GraphValue ofBool(bool v);
  static GraphValue ofInt(int64_t v);
  static GraphValue ofFloat(double v);
  static GraphValue ofString(std::string_view v);
  static GraphValue ofVec2(double x, double y);
  static GraphValue ofVec3(double x, double y, double z);
  static GraphValue ofVec4(double x, double y, double z, double w);
  static GraphValue ofQuat(double x, double y, double z, double w);
  /** Float, Vec2/3/4, Mat4 or Quat from its components. */
  static GraphValue ofComponents(Type type, const double *components);
  static GraphValue identityMat4();
  /** A payload. A null reference gives the null payload. */
  static GraphValue ofPayload(PayloadRef ref);

  /** From a PropValue. Enum, Flags and Pointer values are Unsupported: sockets don't
   * carry them. */
  static Result<GraphValue> fromProp(const PropValue &v);
  /** None for None and for a payload. */
  PropValue toProp() const;

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
  /** 1 for Float, 2-4 for vectors and Quat, 16 for Mat4, 0 otherwise. */
  int componentCount() const;
  const double *components() const;
  /** A payload value's reference, which may be null. */
  const PayloadRef &asPayload() const;
  /** What it has outside of itself, for memory reports: components shared with copies count
   * for each, a payload doesn't (PayloadRef::bytes). */
  size_t heapBytes() const;
  /** The payload's value, or null if this doesn't hold a T. */
  template <class T> const T *payload() const
  {
    return type_ == Type::Payload ? p_.get<T>() : nullptr;
  }

  bool operator==(const GraphValue &b) const;
  bool operator!=(const GraphValue &b) const
  {
    return !operator==(b);
  }

 private:
  struct Matrix;
  void reset();
  double *newMatrix();

  Type type_ = Type::None;
  union {
    bool b_;
    int64_t i_;
    litestl::util::string *s_;
    PayloadRef p_;
    double d_[4];
    Matrix *m_;
  };
};

const char *graphValueTypeName(GraphValue::Type type);
/** Component count of a type (see GraphValue::componentCount). */
int graphComponentCount(GraphValue::Type type);
/** The GraphValue type for a property of `kind`, or None if sockets can't carry it. */
GraphValue::Type graphTypeFor(PropKind kind);

} // namespace coredcc
