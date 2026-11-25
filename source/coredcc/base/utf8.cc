#include "coredcc/base/utf8.h"

#include <cstdint>

namespace coredcc {

size_t utf8SequenceLength(std::string_view s, size_t at)
{
  if (at >= s.size()) {
    return 0;
  }
  const uint8_t lead = uint8_t(s[at]);
  if (lead < 0x80) {
    return 1;
  }
  const size_t n = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : 2;
  if (lead < 0xC2 || lead > 0xF4 || s.size() - at < n) {
    return 0;
  }
  /* The second byte's range excludes overlong forms, surrogates and what is past
   * U+10FFFF. */
  const uint8_t second = uint8_t(s[at + 1]);
  const uint8_t low = lead == 0xE0 ? 0xA0 : lead == 0xF0 ? 0x90 : 0x80;
  const uint8_t high = lead == 0xED ? 0x9F : lead == 0xF4 ? 0x8F : 0xBF;
  if (second < low || second > high) {
    return 0;
  }
  for (size_t i = 2; i < n; i++) {
    if ((uint8_t(s[at + i]) & 0xC0) != 0x80) {
      return 0;
    }
  }
  return n;
}

size_t utf8ValidPrefix(std::string_view s)
{
  size_t at = 0;
  while (const size_t n = utf8SequenceLength(s, at)) {
    at += n;
  }
  return at;
}

} // namespace coredcc
