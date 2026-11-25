#pragma once

/* Helpers for randomized tests: a seeded splitmix64 generator, and the iteration scale
 * from COREDCC_TEST_SCALE (default 1), so CI or a soak run can raise it. */

#include <cstdint>
#include <cstdlib>

namespace coredcc_test {

struct SplitMix64 {
  uint64_t state;
  explicit SplitMix64(uint64_t seed) : state(seed)
  {
  }
  uint64_t next()
  {
    state += 0x9E3779B97F4A7C15ull;
    uint64_t z = state;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
  }
  /** Uniform in [0, n), n > 0. */
  uint32_t below(uint32_t n)
  {
    return uint32_t(next() % n);
  }
};

inline int testScale()
{
#ifdef _MSC_VER
#pragma warning(suppress : 4996) /* getenv */
#endif
  const char *s = getenv("COREDCC_TEST_SCALE");
  const int v = s ? atoi(s) : 1;
  return v < 1 ? 1 : v;
}

} // namespace coredcc_test
