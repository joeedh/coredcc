#pragma once

/** \file
 * Payloads: refcounted, immutable heavy values shared along graph edges (a mesh, a point
 * set). See docs/plans/2026-11-24-p5-graph.md.
 */

#include "coredcc/lib/pin.h"

#include "litestl/util/alloc.h"

#include <cstddef>
#include <cstdint>
#include <span>
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
  /** An aliasing payload, of views into the data of the pinned blocks. */
  PayloadBox(TypeId type, litestl::util::Vector<LibraryPin, 1> pins);
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
  bool isAlias() const
  {
    return pins_.size() > 0;
  }
  /** Whether what it points into is still there. */
  bool alive() const
  {
    return !isAlias() || pinsAlive();
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
  bool pinsAlive() const;

  TypeId type_;
  int refs_ = 1;
  /** The creating thread, in debug builds. In every build, so the layout is the same. */
  size_t thread_;
  litestl::util::Vector<LibraryPin, 1> pins_;
};

template <class T> class PayloadBoxOf final : public PayloadBox {
 public:
  explicit PayloadBoxOf(T v) : PayloadBox(payloadTypeId<T>()), value(std::move(v)) {}
  PayloadBoxOf(T v, litestl::util::Vector<LibraryPin, 1> pins)
      : PayloadBox(payloadTypeId<T>(), std::move(pins)), value(std::move(v))
  {
  }
  ~PayloadBoxOf() override = default;

  bool equal(const PayloadBox &b) const override
  {
    if (this == &b) {
      return true;
    }
    if (isAlias() || b.isAlias()) {
      return false; /* both may read the same memory, edited in between */
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
      if (alive()) {
        return PayloadTraits<T>::bytes(value);
      }
    }
    return sizeof(T);
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
  /** The value, or null if this is null, holds another type, or is an aliasing payload
   * whose blocks' data is gone (see makeAlias). Keep the PayloadRef, not the pointer. */
  template <class T> const T *get() const
  {
    return box_ && box_->type() == payloadTypeId<T>() && box_->alive() ?
               &static_cast<detail::PayloadBoxOf<T> *>(box_)->value :
               nullptr;
  }
  /** The value for writing: in place if this is the only reference, else a copy, which
   * this then refers to. Null as for `get`, and for an aliasing payload, whose views are of
   * the user's original data. */
  template <class T> T *mutate()
  {
    const T *now = get<T>();
    if (!now || box_->isAlias()) {
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
  bool isAlias() const
  {
    return box_ && box_->isAlias();
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
  template <class T>
  friend Result<PayloadRef> makeAlias(T view, Library &lib, std::span<const BlockHandle> blocks);
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

namespace detail {
/** The Host pins of an aliasing payload; see makeAlias for the errors. */
Result<litestl::util::Vector<LibraryPin, 1>> pinAliased(Library &lib,
                                                        std::span<const BlockHandle> blocks);
} // namespace detail

/**
 * An aliasing payload: `view` points into the data of `blocks`, which get a Host pin each
 * until the payload's last reference goes, so deleting one, or dropping it from the history,
 * doesn't free the data. Where it is freed anyway (the Library is destroyed, the host frees
 * a Detached block's data), `get` is null from then on. A pin doesn't keep the owner from
 * editing the data: what reads it through the payload also needs the block as a dependency.
 * For the same reason two aliasing payloads are equal only if they are the same object.
 *
 * NotFound for a null handle and FailedPrecondition for a block that isn't Live;
 * Unsupported for a Transient block type. A block that keeps a payload aliasing itself (in
 * an instance it owns, say) is never collected.
 */
template <class T>
Result<PayloadRef> makeAlias(T view, Library &lib, std::span<const BlockHandle> blocks)
{
  static_assert(std::is_same_v<T, std::remove_cvref_t<T>>);
  auto pins = detail::pinAliased(lib, blocks);
  if (!pins.ok()) {
    return pins.status();
  }
  return PayloadRef(litestl::alloc::New<detail::PayloadBoxOf<T>>(
      "coredcc::Payload", std::move(view), std::move(pins).value()));
}
template <class T> Result<PayloadRef> makeAlias(T view, Library &lib, BlockHandle block)
{
  return makeAlias<T>(std::move(view), lib, std::span<const BlockHandle>(&block, 1));
}

} // namespace coredcc
