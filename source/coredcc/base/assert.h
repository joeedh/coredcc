#pragma once

/** \file
 * coredcc's internal assertions. `CDCC_ASSERT` is checked in debug builds only;
 * `CDCC_CHECK` is always checked. Both print the expression and location, then abort.
 * They are for programmer errors, never for conditions a caller can trigger: those
 * return a `Status`.
 */

namespace coredcc::detail {
[[noreturn]] void assertFailed(const char *expr, const char *msg, const char *file, int line);
} // namespace coredcc::detail

#define CDCC_CHECK(expr, msg)                                                            \
  ((expr) ? (void)0 : ::coredcc::detail::assertFailed(#expr, msg, __FILE__, __LINE__))

#ifdef NDEBUG
#define CDCC_ASSERT(expr, msg) ((void)0)
#else
#define CDCC_ASSERT(expr, msg) CDCC_CHECK(expr, msg)
#endif
