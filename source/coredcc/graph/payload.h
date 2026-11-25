#pragma once

/** \file
 * Payloads: refcounted, immutable heavy values shared along graph edges (a mesh, a point
 * set). See docs/plans/2026-11-24-p5-graph.md.
 */

#include "coredcc/base/id.h"

#include "litestl/util/alloc.h"

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>

namespace coredcc {

/**
 * What makes T a payload type. Specialize it:
 *
 *     template <> struct coredcc::PayloadTraits<MyMesh> {
 *       static constexpr const char *name = "my.Mesh";
 *       static bool equal(const MyMesh &a, const MyMesh &b);   // optional
 *       static size_t bytes(const MyMesh &m);                  // optional, for reports
 *     };
 *
 * `name` identifies T, and is the name of its socket type. Two C++ types must not share
 * one; SocketTypeRegistry::addPayloadType rejects the second. Without `equal`, two
 * payloads are equal only when they are the same object. T must be copyable.
 */
template <class T> struct PayloadTraits;

/** The id of payload type T. */
template <class T> constexpr TypeId payloadTypeId()
{
  return TypeId::of(PayloadTraits<T>::name);
}

namespace detail {

/** A payload's refcount and type. The count isn't atomic. */
class PayloadBox {
 public:
  explicit PayloadBox(TypeId type);
  PayloadBox(const PayloadBox &) = delete;
  PayloadBox &operator=(const PayloadBox &) = delete;

  TypeId type() const
  {
    return type_;
  }
  int refs() const
  {
    return refs_;
  }
  void ref();
  /** Drops one reference, destroying the payload with the last. */
  void unref();
  virtual bool equal(const PayloadBox &b) const = 0;
  virtual size_t bytes() const = 0;

 protected:
  virtual ~PayloadBox() = default;
  virtual void destroySelf() = 0;

 private:
  void checkThread() const;

  TypeId type_;
  int refs_ = 1;
  /** The creating thread, in debug builds. In every build, so the layout is the same. */
  size_t thread_;
};

template <class T> class PayloadBoxOf final : public PayloadBox {
 public:
  explicit PayloadBoxOf(T v) : PayloadBox(payloadTypeId<T>()), value(std::move(v)) {}
  ~PayloadBoxOf() override = default;

  bool equal(const PayloadBox &b) const override
  {
    if (this == &b) {
      return true;
    }
    if constexpr (requires(const T &x) { PayloadTraits<T>::equal(x, x); }) {
      return b.type() == type() &&
             PayloadTraits<T>::equal(value, static_cast<const PayloadBoxOf &>(b).value);
    }
    else {
      return false;
    }
  }
  size_t bytes() const override
  {
    if constexpr (requires(const T &x) { PayloadTraits<T>::bytes(x); }) {
      return PayloadTraits<T>::bytes(value);
    }
    else {
      return sizeof(T);
    }
  }

  T value;

 protected:
  void destroySelf() override
  {
    litestl::alloc::Delete(this);
  }
};

} // namespace detail

/**
 * A reference to a payload, or null. References are made, copied and dropped on the
 * thread the payload was made on (the main thread), which debug builds check. The body of
 * a parallel_for may read the value `get` returned, but must not copy references.
 */
class PayloadRef {
 public:
  PayloadRef() = default;
  PayloadRef(const PayloadRef &b) : box_(b.box_)
  {
    if (box_) {
      box_->ref();
    }
  }
  PayloadRef(PayloadRef &&b) noexcept : box_(std::exchange(b.box_, nullptr)) {}
  PayloadRef &operator=(PayloadRef b) noexcept
  {
    std::swap(box_, b.box_);
    return *this;
  }
  ~PayloadRef()
  {
    if (box_) {
      box_->unref();
    }
  }

  explicit operator bool() const
  {
    return box_ != nullptr;
  }
  /** The payload type; null for a null reference. */
  TypeId type() const
  {
    return box_ ? box_->type() : TypeId();
  }
  /** The value, or null if this is null or holds another type. */
  template <class T> const T *get() const
  {
    return box_ && box_->type() == payloadTypeId<T>() ?
               &static_cast<detail::PayloadBoxOf<T> *>(box_)->value :
               nullptr;
  }
  /** The value for writing: in place if this is the only reference, else a copy, which
   * this then refers to. Null as for `get`. */
  template <class T> T *mutate()
  {
    const T *now = get<T>();
    if (!now) {
      return nullptr;
    }
    if (box_->refs() > 1) {
      *this =
          PayloadRef(litestl::alloc::New<detail::PayloadBoxOf<T>>("coredcc::Payload", *now));
    }
    return &static_cast<detail::PayloadBoxOf<T> *>(box_)->value;
  }
  /** Its size, for memory reports. */
  size_t bytes() const
  {
    return box_ ? box_->bytes() : 0;
  }
  /** Whether both refer to the same object. */
  bool same(const PayloadRef &b) const
  {
    return box_ == b.box_;
  }
  /** The same object, or equal by the type's `equal`. */
  bool operator==(const PayloadRef &b) const
  {
    return box_ == b.box_ || (box_ && b.box_ && box_->equal(*b.box_));
  }

 private:
  template <class T> friend PayloadRef makePayload(T value);
  explicit PayloadRef(detail::PayloadBox *box) : box_(box) {}
  detail::PayloadBox *box_ = nullptr;
};

/** A new payload holding `value`. */
template <class T> PayloadRef makePayload(T value)
{
  static_assert(std::is_same_v<T, std::remove_cvref_t<T>>);
  return PayloadRef(
      litestl::alloc::New<detail::PayloadBoxOf<T>>("coredcc::Payload", std::move(value)));
}

} // namespace coredcc
