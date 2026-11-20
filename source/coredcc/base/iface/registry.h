#pragma once

#include "coredcc/base/iface/iface.h"
#include "coredcc/base/runtime.h"
#include "coredcc/base/status.h"

#include "litestl/util/map.h"
#include "litestl/util/vector.h"

#include <span>
#include <string_view>

namespace coredcc {

/**
 * The interface registry of one Runtime: interface descriptors by InterfaceId, and
 * IfaceTables by (implementation TypeId, InterfaceId). It owns the tables. Use the
 * templates below (registerImpl, query, ...) rather than calling it directly.
 */
class IfaceRegistry {
 public:
  explicit IfaceRegistry(Runtime *rt) : rt_(*rt) {}
  ~IfaceRegistry();
  IfaceRegistry(const IfaceRegistry &) = delete;
  IfaceRegistry &operator=(const IfaceRegistry &) = delete;

  /** Registers an interface, checking the declaration rules. Idempotent. */
  Status addInterface(const lbt::_StructBase *iface);
  const lbt::_StructBase *findInterface(InterfaceId id) const;

  /**
   * Matches `impl` against `iface` slot by slot and stores the table. `upcastIfaces`
   * are the interface's extra bases: each gets its own table, linked for upcast<>().
   */
  Status addImpl(const lbt::_StructBase *iface,
                 const lbt::_StructBase *impl,
                 std::string_view implName,
                 std::span<const lbt::_StructBase *const> upcastIfaces = {});

  /**
   * A foreign implementation: `presentSlots` names the methods the foreign class has.
   * Every required slot must be present; absent optional slots get null thunks.
   */
  Status addForeign(InterfaceId iface,
                    std::string_view implName,
                    ForeignDispatcher *dispatcher,
                    void *foreignClass,
                    std::span<const char *const> presentSlots);

  const IfaceTable *find(TypeId implType, InterfaceId iface) const;
  int tableCount() const
  {
    return int(tables_.size());
  }

  /** Re-checks every table against its descriptors. Run at startup in debug builds. */
  Status validateAll() const;

  /** The cached implementation descriptor registered under `implType`, or null. */
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

  Result<IfaceTable *> buildNative(const lbt::_StructBase *iface,
                                   const lbt::_StructBase *impl,
                                   std::string_view implName);
  Status store(IfaceTable *table);

  Runtime &rt_;
  litestl::util::Vector<IfaceTable *> tables_;
  litestl::util::Map<TableKey, IfaceTable *> byKey_;
  litestl::util::Map<uint64_t, const lbt::_StructBase *> interfaces_;
  litestl::util::Map<uint64_t, const lbt::_StructBase *> implDescs_;
};

namespace detail {

template <class I> struct UpcastsOf {
  static void collect(litestl::util::Vector<const lbt::_StructBase *> &) {}
};
template <class I>
  requires requires { typename I::CdccUpcasts; }
struct UpcastsOf<I> {
  template <class... U> static void add(IfaceList<U...>, litestl::util::Vector<const lbt::_StructBase *> &out)
  {
    (out.append(ifaceDesc<U>()), ...);
  }
  static void collect(litestl::util::Vector<const lbt::_StructBase *> &out)
  {
    add(typename I::CdccUpcasts{}, out);
  }
};

template <class I> Status registerInterfaceAndUpcasts(Runtime &rt,
                                                      litestl::util::Vector<const lbt::_StructBase *> &upcasts)
{
  UpcastsOf<I>::collect(upcasts);
  for (const lbt::_StructBase *u : upcasts) {
    CDCC_TRY(rt.ifaces().addInterface(u));
  }
  return rt.ifaces().addInterface(ifaceDesc<I>());
}

} // namespace detail

/** Registers interface I (and its CDCC_UPCASTS bases). registerImpl does this too. */
template <class I> Status registerInterface(Runtime &rt)
{
  litestl::util::Vector<const lbt::_StructBase *> upcasts;
  return detail::registerInterfaceAndUpcasts<I>(rt, upcasts);
}

/**
 * Registers bound class Impl as an implementation of I under `implName` (always
 * explicit: compiler-derived names differ between clang and MSVC). Fails with a Status
 * naming every slot that doesn't match.
 */
template <class I, class Impl> Status registerImpl(Runtime &rt, std::string_view implName)
{
  litestl::util::Vector<const lbt::_StructBase *> upcasts;
  CDCC_TRY(detail::registerInterfaceAndUpcasts<I>(rt, upcasts));
  Result<TypeId> type = rt.registerId<IdDomain::Type>(implName);
  if (!type.ok()) {
    return type.status();
  }
  const lbt::_StructBase *impl = rt.ifaces().cachedImplDesc(*type);
  if (!impl) {
    litestl::alloc::PermanentGuard guard;
    impl = static_cast<const lbt::_StructBase *>(Impl::defineBindings());
    rt.ifaces().cacheImplDesc(*type, impl);
  }
  return rt.ifaces().addImpl(detail::ifaceDesc<I>(),
                            impl,
                            implName,
                            std::span<const lbt::_StructBase *const>(upcasts.data(), upcasts.size()));
}

/**
 * Registers an adaptor: implementation methods bound onto a type we don't own.
 * `bindFn(litestl::binding::types::Struct<Adapted> *st)` adds them with
 * BIND_ADAPTOR_METHOD. The descriptor is never registered with a BindingManager,
 * because two adaptors of one type share its name.
 */
template <class I, class Adapted, class BindFn>
Status registerAdaptor(Runtime &rt, std::string_view implName, BindFn bindFn)
{
  litestl::util::Vector<const lbt::_StructBase *> upcasts;
  CDCC_TRY(detail::registerInterfaceAndUpcasts<I>(rt, upcasts));
  Result<TypeId> type = rt.registerId<IdDomain::Type>(implName);
  if (!type.ok()) {
    return type.status();
  }
  const lbt::_StructBase *impl = rt.ifaces().cachedImplDesc(*type);
  if (!impl) {
    litestl::alloc::PermanentGuard guard;
    litestl::util::string name(std::string(implName).c_str());
    auto *st = new lbt::Struct<Adapted>(name, sizeof(Adapted));
    bindFn(st);
    impl = st;
    rt.ifaces().cacheImplDesc(*type, impl);
  }
  return rt.ifaces().addImpl(detail::ifaceDesc<I>(),
                            impl,
                            implName,
                            std::span<const lbt::_StructBase *const>(upcasts.data(), upcasts.size()));
}

/** A foreign implementation of I (see IfaceRegistry::addForeign). */
template <class I>
Status implementForeign(Runtime &rt,
                        std::string_view implName,
                        ForeignDispatcher *dispatcher,
                        void *foreignClass,
                        std::span<const char *const> presentSlots)
{
  CDCC_TRY(registerInterface<I>(rt));
  Result<TypeId> type = rt.registerId<IdDomain::Type>(implName);
  if (!type.ok()) {
    return type.status();
  }
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

} // namespace coredcc
