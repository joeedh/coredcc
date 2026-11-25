#pragma once

#include "coredcc/base/iface/iface.h"
#include "coredcc/base/runtime.h"
#include "coredcc/base/status.h"

#include "litestl/util/map.h"
#include "litestl/util/vector.h"

#include <span>
#include <string>
#include <string_view>

namespace coredcc {

/**
 * The interface registry of one Runtime: interface descriptors by InterfaceId, and
 * IfaceTables by (implementation TypeId, InterfaceId). It owns the tables.
 *
 * The templates below (registerImpl, registerForeign, query, ...) are the usual entry
 * points. The add* methods are the untyped layer under them, for hosts that only have
 * names and descriptors (N-API, WASM).
 */
class IfaceRegistry {
public:
  explicit IfaceRegistry(Runtime *rt) : rt_(*rt)
  {
  }
  ~IfaceRegistry();
  IfaceRegistry(const IfaceRegistry &) = delete;
  IfaceRegistry &operator=(const IfaceRegistry &) = delete;

  /**
   * Registers an interface, checking the declaration rules. `related` lists every
   * interface an implementation of it also implements: its concatenated bases and its
   * CDCC_UPCASTS bases, transitively. They must be registered too. Idempotent.
   */
  Status addInterface(const lbt::_StructBase *iface,
                      std::span<const lbt::_StructBase *const> related = {});
  const lbt::_StructBase *findInterface(InterfaceId id) const;

  /**
   * Matches `impl` against `iface` and each related interface, slot by slot, and stores
   * one table per interface. All or nothing: on error nothing is stored.
   */
  Status addImpl(const lbt::_StructBase *iface,
                 const lbt::_StructBase *impl,
                 std::string_view implName,
                 IfaceFlags flags = IfaceFlags::None);

  /**
   * A foreign implementation: `presentSlots` names the methods the foreign class has.
   * Every required slot of the interface and its related interfaces must be present;
   * absent optional slots get null thunks.
   */
  Status addForeign(InterfaceId iface,
                    std::string_view implName,
                    ForeignDispatcher *dispatcher,
                    void *foreignClass,
                    std::span<const char *const> presentSlots);

  const IfaceTable *find(TypeId implType, InterfaceId iface) const;
  int tableCount() const
  {
    return int(byKey_.size());
  }

  /** Checks the registry's invariants. Run at startup in debug builds. */
  Status validateAll() const;

  /** The implementation descriptor cached under `implType`, or null. */
  const lbt::_StructBase *cachedImplDesc(TypeId implType) const;
  void cacheImplDesc(TypeId implType, const lbt::_StructBase *desc);

private:
  struct TableKey {
    uint64_t type = 0;
    uint64_t iface = 0;
    uint64_t computeHash() const
    {
      return type * 0x9E3779B97F4A7C15ull ^ iface;
    }
    bool operator==(const TableKey &b) const
    {
      return type == b.type && iface == b.iface;
    }
  };
  struct InterfaceEntry {
    const lbt::_StructBase *desc = nullptr;
    litestl::util::Vector<const lbt::_StructBase *> related;
  };

  /* Stores family[0] (the requested interface's table) and the related tables after
   * it; a related table that already exists for this implementation is reused. */
  Status storeFamily(litestl::util::Vector<IfaceTable *> &family);

  Runtime &rt_;
  litestl::util::Vector<IfaceTable *> tables_;
  litestl::util::Map<TableKey, IfaceTable *> byKey_;
  litestl::util::Map<uint64_t, InterfaceEntry> interfaces_;
  litestl::util::Map<uint64_t, const lbt::_StructBase *> implDescs_;
};

namespace detail {

template <class T> struct BaseOf {
  using type = void;
};
template <class T>
  requires requires { typename T::CdccBase; }
struct BaseOf<T> {
  using type = typename T::CdccBase;
};

template <class I> struct UpcastsOf {
  template <class F> static void each(F &&)
  {
  }
};
template <class I>
  requires requires { typename I::CdccUpcasts; }
struct UpcastsOf<I> {
  template <class F, class... U> static void eachIn(IfaceList<U...>, F &f)
  {
    (f.template operator()<U>(), ...);
  }
  template <class F> static void each(F &&f)
  {
    eachIn(typename I::CdccUpcasts{}, f);
  }
};

/** Calls `f.template operator()<R>()` for each interface directly related to I. */
template <class I, class F> void forEachRelated(F &&f)
{
  using Base = typename BaseOf<I>::type;
  if constexpr (!std::is_void_v<Base>) {
    f.template operator()<Base>();
  }
  UpcastsOf<I>::each(f);
}

/** Every interface related to I, transitively, without duplicates or I itself. */
template <class I>
void collectRelated(const lbt::_StructBase *root,
                    litestl::util::Vector<const lbt::_StructBase *> &out)
{
  forEachRelated<I>([root, &out]<class R>() {
    const lbt::_StructBase *d = ifaceDesc<R>();
    if (d == root || out.contains(d)) {
      return;
    }
    out.append(d);
    collectRelated<R>(root, out);
  });
}

inline std::span<const lbt::_StructBase *const>
spanOf(const litestl::util::Vector<const lbt::_StructBase *> &v)
{
  return {v.data(), size_t(v.size())};
}

} // namespace detail

/** Registers interface I and every interface related to it. registerImpl does this. */
template <class I> Status registerInterface(Runtime &rt)
{
  if (rt.ifaces().findInterface(InterfaceId::of(I::ifaceName)) == detail::ifaceDesc<I>())
  {
    return Status();
  }
  litestl::util::Vector<const lbt::_StructBase *> related;
  detail::collectRelated<I>(detail::ifaceDesc<I>(), related);
  /* I first, so a cycle of upcasts stops when it comes back to I. */
  CDCC_TRY(rt.ifaces().addInterface(detail::ifaceDesc<I>(), detail::spanOf(related)));
  Status status;
  detail::forEachRelated<I>([&rt, &status]<class R>() {
    if (status.ok()) {
      status = registerInterface<R>(rt);
    }
  });
  return status;
}

/**
 * Registers bound class Impl as an implementation of I, and of every interface related
 * to I, under `implName` (always explicit: compiler-derived names differ between clang
 * and MSVC). Fails with a Status naming every slot that doesn't match.
 */
template <class I, class Impl>
Status
registerImpl(Runtime &rt, std::string_view implName, IfaceFlags flags = IfaceFlags::None)
{
  CDCC_TRY(registerInterface<I>(rt));
  const TypeId type = TypeId::of(implName);
  const lbt::_StructBase *impl = rt.ifaces().cachedImplDesc(type);
  if (impl && impl->typeKey != lbt::typeKeyOf<Impl>()) {
    return Status::error(
        StatusCode::AlreadyExists,
        (std::string(implName) + " is already bound to another C++ type").c_str());
  }
  const bool cached = impl != nullptr;
  if (!cached) {
    litestl::alloc::PermanentGuard guard;
    impl = static_cast<const lbt::_StructBase *>(Impl::defineBindings());
  }
  CDCC_TRY(rt.ifaces().addImpl(detail::ifaceDesc<I>(), impl, implName, flags));
  if (!cached) {
    rt.ifaces().cacheImplDesc(type, impl);
  }
  return Status();
}

/**
 * Registers an adaptor: implementation methods bound onto a type we don't own.
 * `bindFn(litestl::binding::types::Struct<Adapted> *st)` adds them with
 * BIND_ADAPTOR_METHOD. One adaptor name has one descriptor: registering the same name
 * for another interface reuses it without calling `bindFn` again, so the first `bindFn`
 * must bind every slot of every interface the adaptor implements. The descriptor is
 * never registered with a BindingManager, because two adaptors of one type share its
 * name.
 */
template <class I, class Adapted, class BindFn>
Status registerAdaptor(Runtime &rt,
                       std::string_view implName,
                       BindFn bindFn,
                       IfaceFlags flags = IfaceFlags::None)
{
  CDCC_TRY(registerInterface<I>(rt));
  const TypeId type = TypeId::of(implName);
  const lbt::_StructBase *impl = rt.ifaces().cachedImplDesc(type);
  if (impl && impl->typeKey != lbt::typeKeyOf<Adapted>()) {
    return Status::error(
        StatusCode::AlreadyExists,
        (std::string(implName) + " is already bound to another C++ type").c_str());
  }
  const bool cached = impl != nullptr;
  if (!cached) {
    litestl::alloc::PermanentGuard guard;
    litestl::util::string name(std::string(implName).c_str());
    auto *st = new lbt::Struct<Adapted>(name, sizeof(Adapted));
    bindFn(st);
    impl = st;
  }
  CDCC_TRY(rt.ifaces().addImpl(detail::ifaceDesc<I>(), impl, implName, flags));
  if (!cached) {
    rt.ifaces().cacheImplDesc(type, impl);
  }
  return Status();
}

/** A foreign implementation of I and its related interfaces (see addForeign). */
template <class I>
Status registerForeign(Runtime &rt,
                       std::string_view implName,
                       ForeignDispatcher *dispatcher,
                       void *foreignClass,
                       std::span<const char *const> presentSlots)
{
  CDCC_TRY(registerInterface<I>(rt));
  return rt.ifaces().addForeign(
      InterfaceId::of(I::ifaceName), implName, dispatcher, foreignClass, presentSlots);
}

/** Interface I of `self`, an object of registered type `implType`; null if none. */
template <class I> I query(const Runtime &rt, TypeId implType, void *self)
{
  I out;
  if (const IfaceTable *t = rt.ifaces().find(implType, InterfaceId::of(I::ifaceName))) {
    static_cast<IfaceRef &>(out) = IfaceRef(t, self);
  }
  return out;
}

/** query() for a typed object: debug builds check that T is the registered type. */
template <class I, class T>
  requires(!std::is_void_v<T>)
I query(const Runtime &rt, TypeId implType, T *self)
{
  I out = query<I>(rt, implType, static_cast<void *>(self));
  CDCC_ASSERT(!out || !out.table()->implDesc || out.template as<T>() == self,
              "query: the object is not of the registered implementation type");
  return out;
}

} // namespace coredcc
