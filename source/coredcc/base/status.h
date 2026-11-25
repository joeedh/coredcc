#pragma once

#include "coredcc/base/assert.h"

#include "litestl/util/string.h"
#include "litestl/util/vector.h"

#include <cstdint>
#include <optional>
#include <type_traits>
#include <utility>

namespace litestl::binding {
struct BindingBase;
template <typename T> struct Binder;
namespace types {
template <typename T> struct Struct;
}
} // namespace litestl::binding

namespace coredcc {

enum class StatusCode : uint8_t {
  Ok,
  InvalidArgument,
  NotFound,
  AlreadyExists,
  TypeMismatch,
  OutOfRange,
  FailedPrecondition,
  Busy,
  Cancelled,
  Unsupported,
  Internal,
};

/** Stable name of a code, e.g. "NotFound". */
const char *statusCodeName(StatusCode code);

/**
 * The result of an operation that can fail. coredcc builds without exceptions, so every
 * fallible API returns a Status (or a Result<T>).
 *
 * An ok Status is a single null pointer, so returning one is free. An error carries a
 * code, a message and optional suggestions ("did you mean ..."), heap-allocated.
 */
class [[nodiscard]] Status {
public:
  Status() = default;
  Status(const Status &b);
  Status(Status &&b) noexcept : rep_(std::exchange(b.rep_, nullptr))
  {
  }
  Status &operator=(const Status &b);
  Status &operator=(Status &&b) noexcept;
  ~Status();

  static Status error(StatusCode code, litestl::util::stringref message);

  bool ok() const
  {
    return rep_ == nullptr;
  }
  StatusCode code() const;
  /** The message, or "" for an ok Status. */
  const char *message() const;
  const litestl::util::Vector<litestl::util::string> &suggestions() const;

  /** Add a suggestion. Has no effect on an ok Status. */
  Status &suggest(litestl::util::stringref suggestion) &;
  Status &&suggest(litestl::util::stringref suggestion) &&;

  /** "Code: message (did you mean: a, b?)", or "Ok". */
  litestl::util::string toString() const;

  /** Explicitly discard a Status whose failure doesn't matter here. */
  void ignore() const
  {
  }

  /** Bound so interface slots can return a Status (Command::exec). */
  static const litestl::binding::types::Struct<Status> *defineBindings();

private:
  struct Rep;
  Rep *rep_ = nullptr;
};

/** A value, or the Status explaining why there isn't one. */
template <typename T> class [[nodiscard]] Result {
  static_assert(!std::is_same_v<std::remove_cvref_t<T>, Status>, "use Status directly");

public:
  Result(T value) : value_(std::move(value))
  {
  }
  Result(Status status) : status_(std::move(status))
  {
    CDCC_CHECK(!status_.ok(), "a Result built from a Status must hold an error");
  }

  bool ok() const
  {
    return value_.has_value();
  }
  /** The error, or an ok Status when there is a value. */
  const Status &status() const
  {
    return status_;
  }

  T &value() &
  {
    CDCC_CHECK(ok(), "Result::value() on an error");
    return *value_;
  }
  const T &value() const &
  {
    CDCC_CHECK(ok(), "Result::value() on an error");
    return *value_;
  }
  T &&value() &&
  {
    CDCC_CHECK(ok(), "Result::value() on an error");
    return std::move(*value_);
  }
  T valueOr(T fallback) &&
  {
    return ok() ? std::move(*value_) : std::move(fallback);
  }

  T *operator->()
  {
    return &value();
  }
  const T *operator->() const
  {
    return &value();
  }
  T &operator*() &
  {
    return value();
  }
  const T &operator*() const &
  {
    return value();
  }

private:
  std::optional<T> value_;
  Status status_;
};

/** Return early from the enclosing function if `expr` yields an error Status. */
#define CDCC_TRY(expr)                                                                   \
  do {                                                                                   \
    ::coredcc::Status cdcc_try_status_ = (expr);                                         \
    if (!cdcc_try_status_.ok()) {                                                        \
      return cdcc_try_status_;                                                           \
    }                                                                                    \
  } while (0)

} // namespace coredcc

namespace litestl::binding {
/** StatusCode, bound as an enum (base/bindings.cc). */
template <> struct Binder<coredcc::StatusCode> {
  static const BindingBase *bind();
};
} // namespace litestl::binding
