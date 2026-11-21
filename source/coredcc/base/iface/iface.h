#pragma once

/**
 * Interfaces (architecture §2a): a contract a client implements, built on litestl's
 * binding descriptors. An interface is a bound struct marked `isInterface`; its bound
 * methods are the slots. An implementation is any bound class (or adaptor, or foreign
 * object) whose methods match the slots by name and signature, checked at
 * registration (see registry.h).
 *
 *   struct Spatial : coredcc::IfaceRef {
 *     int count() const { return CDCC_CALL(count); }
 *     double cost() const { return CDCC_HAS(cost) ? CDCC_CALL(cost) : count() * 2.0; }
 *
 *     CDCC_INTERFACE(Spatial, "coredcc.Spatial") {
 *       BIND_STRUCT_METHOD(st, count, MARGS());
 *       BIND_OPTIONAL_METHOD(st, cost, MARGS());
 *     }
 *   };
 *
 * Declaration rules: no overloads, no static or template methods, no reference returns,
 * no function-pointer, `function_ref` or `Span` parameters, no primitive out-parameters,
 * and no slots named `table`, `self` or `as`. A derived interface must use
 * CDCC_INTERFACE_EXTENDS; debug builds catch a forgotten one on the first call.
 *
 * Every forwarder must have a bound slot: an unbound one aborts on its first call, so
 * tests should call every forwarder.
 *
 * Identity is per binary image: declare interfaces and register implementations inside
 * the image that links coredcc (docs/embedding.md).
 */

#include "coredcc/base/assert.h"
#include "coredcc/base/id.h"
#include "coredcc/base/runtime.h"
#include "coredcc/base/status.h"

#include "litestl/binding/binding.h"
#include "litestl/util/alloc.h"
#include "litestl/util/vector.h"

#include <cstdint>
#include <cstring>
#include <new>
#include <type_traits>
#include <utility>

#if !defined(LITESTL_HAS_IFACE_DESCRIPTORS)
#error "coredcc interfaces need litestl's interface descriptors (litestl PR #6)"
#endif

namespace coredcc {

namespace lbt = litestl::binding::types;

struct IfaceTable;

/**
 * Calls a slot of a foreign (JS) implementation. One per host: EM_JS under WASM,
 * napi_call_function under N-API, a fake in tests. It marshals `args` and `ret` with
 * the slot's descriptor, `table.ifaceDesc->methods[slot]`, using the thunk ABI.
 *
 * On success it must have constructed the return value in `ret` (for non-void slots).
 * On failure (a JS exception, a wrong return type) it returns an error and leaves `ret`
 * unconstructed; the forwarder then returns a value-initialized result and records the
 * error as the Runtime's sticky error.
 */
class ForeignDispatcher {
 public:
  virtual ~ForeignDispatcher() = default;
  virtual Status call(const IfaceTable &table, void *self, int slot, void **args, void *ret) = 0;
};

enum class IfaceFlags : uint32_t {
  None = 0,
  /** Must be called on the main thread. Always set for foreign tables; set it for native
   * implementations that touch host UI or engine state. */
  ThreadAffine = 1 << 0,
  Foreign = 1 << 1,
};
constexpr IfaceFlags operator|(IfaceFlags a, IfaceFlags b)
{
  return IfaceFlags(uint32_t(a) | uint32_t(b));
}
constexpr bool hasFlag(IfaceFlags set, IfaceFlags f)
{
  return (uint32_t(set) & uint32_t(f)) != 0;
}

/** Placeholder thunk marking a present slot of a foreign table (the dispatcher runs). */
void foreignSlotPresent(void *self, void **args, void *ret);

/**
 * One implementation of one interface: a thunk per slot. A null thunk is an absent
 * optional slot. Built by registration and owned by the Runtime's IfaceRegistry.
 */
struct IfaceTable {
  InterfaceId iface;
  TypeId implType;
  IfaceFlags flags = IfaceFlags::None;
  const lbt::_StructBase *ifaceDesc = nullptr;
  /** The implementation's descriptor (the adapted type's, for an adaptor); null for
   * foreign tables. */
  const lbt::_StructBase *implDesc = nullptr;
  litestl::util::Vector<lbt::MethodThunk> thunks;
  ForeignDispatcher *dispatcher = nullptr;
  /** The dispatcher's per-class context (e.g. the JS class). */
  void *foreignClass = nullptr;
  /** Receives foreign-call errors as its sticky error. */
  Runtime *runtime = nullptr;
  /** The same implementation's tables for every related interface: the concatenated
   * bases and the CDCC_UPCASTS bases, transitively. Used by upcast<>(). */
  litestl::util::Vector<const IfaceTable *> related;

  bool hasSlot(int slot) const
  {
    return thunks[slot] != nullptr;
  }
  Status call(void *self, int slot, void **args, void *ret) const
  {
    if (dispatcher) {
      return dispatcher->call(*this, self, slot, args, ret);
    }
    thunks[slot](self, args, ret);
    return Status();
  }
};

/**
 * A transient interface value: a table and an object. Interfaces derive from it and add
 * forwarders. Never store one; store the object's id and the InterfaceId instead.
 *
 * Const-ness is shallow: a copy of a `const Spatial &` is a mutable `Spatial`.
 */
struct IfaceRef {
  IfaceRef() = default;
  IfaceRef(const IfaceTable *table, void *self) : table_(table), self_(self) {}

  explicit operator bool() const
  {
    return table_ != nullptr;
  }
  const IfaceTable *table() const
  {
    return table_;
  }
  void *self() const
  {
    return self_;
  }
  /** The implementation object if it is a T (checked by litestl type key), else null. */
  template <class T> T *as() const
  {
    if (!table_ || !table_->implDesc || table_->implDesc->typeKey != lbt::typeKeyOf<T>()) {
      return nullptr;
    }
    return static_cast<T *>(self_);
  }

 private:
  const IfaceTable *table_ = nullptr;
  void *self_ = nullptr;
};

namespace detail {

/** The cached descriptor of interface I. */
template <class I> const lbt::_StructBase *ifaceDesc()
{
  return static_cast<const lbt::_StructBase *>(I::defineBindings());
}

/** A resolved slot: its index, and its descriptor for the debug check in FwdCall. */
struct SlotRef {
  int index;
  const lbt::Method *method;
};

/** Slot `name` of interface I; aborts if there is none. */
template <class I> SlotRef slotOf(const char *name)
{
  const lbt::_StructBase *d = ifaceDesc<I>();
  for (int i = 0; i < int(d->methods.size()); i++) {
    if (strcmp(d->methods[i]->name.c_str(), name) == 0) {
      return {i, d->methods[i]};
    }
  }
  CDCC_CHECK(false, "CDCC_CALL: interface has no slot with this name");
  return {-1, nullptr};
}

/* By-value parameters are passed through by const reference: args[i] only needs an
 * address, so a second copy would be wasted. */
template <class P> using FwdParam = std::conditional_t<std::is_reference_v<P>, P, const P &>;

/** Checks, in debug builds, that `slot` belongs to the table's interface. */
inline void checkSlot(const IfaceRef &ref, SlotRef slot)
{
  CDCC_ASSERT(ref.table() != nullptr, "calling a null interface");
  CDCC_ASSERT(slot.index >= 0 && slot.index < int(ref.table()->thunks.size()),
              "slot index out of range: a derived interface without CDCC_INTERFACE_EXTENDS?");
  CDCC_ASSERT(ref.table()->ifaceDesc->methods[slot.index] == slot.method,
              "slot belongs to another interface: a derived interface without "
              "CDCC_INTERFACE_EXTENDS?");
  CDCC_ASSERT(ref.table()->hasSlot(slot.index), "optional slot called without CDCC_HAS");
  (void)ref;
  (void)slot;
}

/** Records a failed foreign call on the table's Runtime. */
void reportCallFailure(const IfaceTable &table, Status status);

/**
 * Calls a slot with the forwarder's own parameter and return types, taken from its
 * member pointer: the same signature the descriptor was built from, so they can't
 * diverge.
 */
template <class R, class... P> struct FwdCall {
  static_assert(!std::is_reference_v<R>, "interface slots can't return references");

  static R call(const IfaceRef &ref, SlotRef slot, FwdParam<P>... p)
  {
    checkSlot(ref, slot);
    /* args[i] points at the value (or at the pointer, for pointer parameters). */
    void *args[sizeof...(P) + 1] = {const_cast<void *>(static_cast<const void *>(&p))...};
    if constexpr (std::is_void_v<R>) {
      Status s = ref.table()->call(ref.self(), slot.index, args, nullptr);
      if (!s.ok()) {
        reportCallFailure(*ref.table(), std::move(s));
      }
    } else {
      alignas(R) unsigned char buf[sizeof(R)];
      Status s = ref.table()->call(ref.self(), slot.index, args, buf);
      if (!s.ok()) {
        reportCallFailure(*ref.table(), std::move(s));
        if constexpr (std::is_default_constructible_v<R>) {
          return R();
        } else {
          CDCC_CHECK(false, "a foreign slot whose return type has no default failed");
        }
      }
      R *out = std::launder(reinterpret_cast<R *>(buf));
      R result(std::move(*out));
      out->~R();
      return result;
    }
  }
};

/* Matched on the member pointer's type: MSVC finds value-based partial specializations
 * over const and const noexcept ambiguous (C2752). */
template <class T> struct FwdOf;
template <class C, class R, class... P> struct FwdOf<R (C::*)(P...) const> {
  using type = FwdCall<R, P...>;
};
template <class C, class R, class... P> struct FwdOf<R (C::*)(P...)> {
  using type = FwdCall<R, P...>;
};
template <class C, class R, class... P> struct FwdOf<R (C::*)(P...) const noexcept> {
  using type = FwdCall<R, P...>;
};
template <class C, class R, class... P> struct FwdOf<R (C::*)(P...) noexcept> {
  using type = FwdCall<R, P...>;
};
template <auto M> struct Fwd : FwdOf<decltype(M)>::type {};

template <class I> lbt::Struct<I> *newInterfaceStruct(const char *name)
{
  static_assert(std::is_base_of_v<IfaceRef, I>, "an interface derives from coredcc::IfaceRef");
  static_assert(sizeof(I) == sizeof(IfaceRef), "an interface has no data members");
  auto *st = new lbt::Struct<I>(name, sizeof(I));
  st->isInterface = true;
  /* The TS runtime passes structs by value through their "copy" constructor. */
  ::litestl::binding::BIND_STRUCT_COPY_CONSTRUCTOR(st);
  return st;
}

/** Single-base inheritance: Base's slots come first, so slot indices coincide. */
template <class I, class Base> lbt::Struct<I> *newDerivedInterfaceStruct(const char *name)
{
  static_assert(std::is_base_of_v<Base, I>, "CDCC_INTERFACE_EXTENDS: I must derive from Base");
  lbt::Struct<I> *st = newInterfaceStruct<I>(name);
  for (lbt::Method *m : ifaceDesc<Base>()->methods) {
    st->addMethod(m);
  }
  return st;
}

} // namespace detail

/**
 * I's view of the same object, if I is related to `from`'s interface: the same
 * interface, a concatenated base, or a CDCC_UPCASTS base (transitively). Else null.
 */
template <class To> To upcast(const IfaceRef &from)
{
  To out;
  const IfaceTable *t = from.table();
  if (!t) {
    return out;
  }
  const InterfaceId want = InterfaceId::of(To::ifaceName);
  if (t->iface == want) {
    static_cast<IfaceRef &>(out) = from;
    return out;
  }
  for (const IfaceTable *r : t->related) {
    if (r->iface == want) {
      static_cast<IfaceRef &>(out) = IfaceRef(r, from.self());
      break;
    }
  }
  return out;
}

/** A list of interfaces, for CDCC_UPCASTS. */
template <class... I> struct IfaceList {};

} // namespace coredcc

/* Slot `mname` of the enclosing interface, resolved on first use and cached. */
#define CDCC_SLOT(mname)                                                                 \
  ([]() -> ::coredcc::detail::SlotRef {                                                  \
    static const ::coredcc::detail::SlotRef cdccSlot_ =                                  \
        ::coredcc::detail::slotOf<Self>(#mname);                                         \
    return cdccSlot_;                                                                    \
  }())

/** Call slot `mname` from its forwarder. A misspelled slot is a compile error. */
#define CDCC_CALL(mname, ...)                                                            \
  ::coredcc::detail::Fwd<&Self::mname>::call(                                            \
      *this, CDCC_SLOT(mname) __VA_OPT__(, ) __VA_ARGS__)

/** Whether the implementation provides optional slot `mname`. */
#define CDCC_HAS(mname)                                                                  \
  (static_cast<const ::coredcc::IfaceRef *>(this)->table()->hasSlot(CDCC_SLOT(mname).index))

#define CDCC_INTERFACE_IMPL_(T, NAME, BASE, NEW_STRUCT)                                  \
  using Self = T;                                                                        \
  /** The concatenated base interface, or void. */                                      \
  using CdccBase = BASE;                                                                 \
  static constexpr const char *ifaceName = NAME;                                         \
  static ::litestl::binding::types::Struct<T> *defineBindings()                          \
  {                                                                                      \
    /* Cached before binding: a slot that takes T itself recurses into this. */          \
    static ::litestl::binding::types::Struct<T> *cdccDesc_ = nullptr;                    \
    if (!cdccDesc_) {                                                                    \
      ::litestl::alloc::PermanentGuard cdccGuard_;                                       \
      cdccDesc_ = NEW_STRUCT;                                                            \
      cdccBindSlots(cdccDesc_);                                                          \
    }                                                                                    \
    return cdccDesc_;                                                                    \
  }                                                                                      \
  static void cdccBindSlots([[maybe_unused]] ::litestl::binding::types::Struct<T> *st)

/** Declares interface T named NAME; the following block binds its slots into `st`. */
#define CDCC_INTERFACE(T, NAME)                                                          \
  CDCC_INTERFACE_IMPL_(T, NAME, void, ::coredcc::detail::newInterfaceStruct<T>(NAME))

/** Like CDCC_INTERFACE, for T deriving from interface BASE (whose slots come first). */
#define CDCC_INTERFACE_EXTENDS(T, NAME, BASE)                                            \
  CDCC_INTERFACE_IMPL_(                                                                  \
      T, NAME, BASE, (::coredcc::detail::newDerivedInterfaceStruct<T, BASE>(NAME)))

/** Extra bases reached by upcast<>() rather than by slot concatenation. */
#define CDCC_UPCASTS(...) using CdccUpcasts = ::coredcc::IfaceList<__VA_ARGS__>
