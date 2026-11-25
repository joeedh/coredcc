#pragma once

#include <cstddef>
#include <string_view>

namespace coredcc {

/** The length of the UTF-8 sequence at `at`, 1 to 4; 0 if it isn't valid there (a stray
 * or missing continuation byte, an overlong form, a surrogate, past U+10FFFF, or the
 * end). */
size_t utf8SequenceLength(std::string_view s, size_t at);

/** The offset of the first byte that isn't valid UTF-8, or s.size(). */
size_t utf8ValidPrefix(std::string_view s);

} // namespace coredcc
