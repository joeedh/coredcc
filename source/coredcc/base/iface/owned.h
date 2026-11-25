#pragma once

#include "coredcc/base/iface/registry.h"

#include "litestl/util/alloc.h"

#include <string>
#include <string_view>
#include <utility>

namespace coredcc {

/**
 * An interface value that owns its implementation and destroys it on reset or
 * destruction. Move-only. Never bound to TS or Python: scripts see plain interfaces.
 */
template <class I> class Owned {
public:
  /** Destroys `self`; `ctx` is whatever the creator passed (e.g. a foreign handle). */
  using Deleter = void (*)(void *self, void *ctx);

  Owned() = default;
  Owned(I ref, Deleter deleter, void *ctx = nullptr)
      : ref_(ref), deleter_(deleter), ctx_(ctx)
  {
  }
  Owned(Owned &&b) noexcept
      : ref_(std::exchange(b.ref_, I())), deleter_(std::exchange(b.deleter_, nullptr)),
        ctx_(std::exchange(b.ctx_, nullptr))
  {
  }
  Owned &operator=(Owned &&b) noexcept
  {
    if (this != &b) {
      reset();
      ref_ = std::exchange(b.ref_, I());
      deleter_ = std::exchange(b.deleter_, nullptr);
      ctx_ = std::exchange(b.ctx_, nullptr);
    }
    return *this;
  }
  Owned(const Owned &) = delete;
  Owned &operator=(const Owned &) = delete;
  ~Owned()
  {
    reset();
  }

  explicit operator bool() const
  {
    return bool(ref_);
  }
  /** Non-const as well: a slot's const-ness comes from its forwarder, so mutating slots
   * (Command::exec) need a non-const interface value. */
  I &get()
  {
    return ref_;
  }
  const I &get() const
  {
    return ref_;
  }
  I *operator->()
  {
    return &ref_;
  }
  const I *operator->() const
  {
    return &ref_;
  }

  void reset()
  {
    if (ref_ && deleter_) {
      deleter_(ref_.self(), ctx_);
    }
    ref_ = I();
    deleter_ = nullptr;
    ctx_ = nullptr;
  }

private:
  I ref_;
  Deleter deleter_ = nullptr;
  void *ctx_ = nullptr;
};

/**
 * Takes ownership of `obj` (allocated with litestl::alloc::New) as interface I, via the
 * implementation registered as `implName`. On error `obj` is destroyed.
 */
template <class I, class Impl>
Result<Owned<I>> own(const Runtime &rt, std::string_view implName, Impl *obj)
{
  I ref = query<I>(rt, TypeId::of(implName), static_cast<void *>(obj));
  if (!ref) {
    litestl::alloc::Delete(obj);
    return Status::error(
        StatusCode::NotFound,
        (std::string(implName) + " does not implement " + I::ifaceName).c_str());
  }
  if (ref.template as<Impl>() != obj) {
    litestl::alloc::Delete(obj);
    return Status::error(
        StatusCode::TypeMismatch,
        (std::string(implName) + " is not registered for this C++ type").c_str());
  }
  return Owned<I>(
      ref, [](void *self, void *) { litestl::alloc::Delete(static_cast<Impl *>(self)); });
}

} // namespace coredcc
