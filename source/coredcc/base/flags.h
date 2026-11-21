#pragma once

#include <type_traits>

/** Bitwise operators and hasFlag() for a scoped flag enum `E`, at namespace scope. */
#define CDCC_FLAG_OPERATORS(E)                                                           \
  constexpr E operator|(E a, E b)                                                        \
  {                                                                                      \
    return E(std::underlying_type_t<E>(a) | std::underlying_type_t<E>(b));               \
  }                                                                                      \
  constexpr E operator&(E a, E b)                                                        \
  {                                                                                      \
    return E(std::underlying_type_t<E>(a) & std::underlying_type_t<E>(b));               \
  }                                                                                      \
  constexpr E operator~(E a)                                                             \
  {                                                                                      \
    return E(~std::underlying_type_t<E>(a));                                             \
  }                                                                                      \
  constexpr bool hasFlag(E set, E f)                                                     \
  {                                                                                      \
    return (std::underlying_type_t<E>(set) & std::underlying_type_t<E>(f)) ==            \
           std::underlying_type_t<E>(f);                                                 \
  }
