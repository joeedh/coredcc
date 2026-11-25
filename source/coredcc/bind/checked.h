#pragma once

/** \file
 * The check at the entry of strings from scripts: a NUL is refused here, since the strings
 * behind the facades can't hold one.
 */

#include "coredcc/bind/detail.h"

#include "litestl/binding/foreign.h"

#include <string>
#include <tuple>
#include <type_traits>

namespace coredcc::script::detail {

/** Which of `args` is the first string with a NUL, from 1; 0 for none. */
template <class... A> int nulArgument(const A &...args)
{
  int i = 0, found = 0;
  const auto check = [&](const auto &a) {
    i++;
    if constexpr (std::is_same_v<std::decay_t<decltype(a)>, string>) {
      if (!found && view(a).find('\0') != std::string_view::npos) {
        found = i;
      }
    }
  };
  (check(args), ...);
  return found;
}

/** `F`, a facade, refusing strings with a NUL: what is bound for scripts. If it owns a foreign
 * handle, argument `Owned` (from 1), a refusal releases that, as `F` would. */
template <auto F, int Owned = 0> struct Checked;

template <class R, class... A, R (*F)(A...), int Owned> struct Checked<F, Owned> {
  static void release(const A &...args)
  {
    if constexpr (Owned > 0) {
      const auto handle = std::get<size_t(Owned) - 1>(std::tie(args...));
      static_assert(std::is_same_v<decltype(handle), const litestl::binding::foreign::Handle>);
      if (handle != 0) {
        litestl::binding::foreign::release(handle);
      }
    }
  }
  static R call(A... args)
  {
    if (const int i = nulArgument(args...)) {
      release(args...);
      Status s = error(StatusCode::InvalidArgument,
                       "argument " + std::to_string(i) + ": a string can't hold a NUL");
      if constexpr (std::is_same_v<R, Status>) {
        return s;
      }
      else if constexpr (std::is_same_v<R, string>) {
        return s.toString();
      }
      else if constexpr (requires { R().status; }) {
        return failed<R>(std::move(s));
      }
      else {
        /* With a string, it has to return one of those. */
        static_assert(!(std::is_same_v<std::decay_t<A>, string> || ...));
      }
    }
    return F(args...);
  }
};

/** As BIND_FUNCTION, for facades. */
#define BIND_FACADE(mgr, qualifiedName, fn, ARGNAMES)                                    \
  (mgr)                                                                                  \
      ->addFunction(::litestl::binding::types::makeFunction<                             \
                    &::coredcc::script::detail::Checked<&fn>::call>(qualifiedName))      \
      ->setArgNames(ARGNAMES)

/** BIND_FACADE of a facade that takes over a foreign handle: argument `owned`, from 1. */
#define BIND_OWNING_FACADE(mgr, qualifiedName, fn, owned, ARGNAMES)                        (mgr)                                                                                        ->addFunction(::litestl::binding::types::makeFunction<                                                 &::coredcc::script::detail::Checked<&fn, owned>::call>(                        qualifiedName))                                                                      ->setArgNames(ARGNAMES)

} // namespace coredcc::script::detail
