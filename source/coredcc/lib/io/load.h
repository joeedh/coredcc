#pragma once

/** \file
 * Loading without the notifications: what coredcc/api/library_io.h calls.
 */

#include "coredcc/lib/io/library_io.h"

namespace coredcc {

class Library;

namespace detail {

Result<LoadReport>
loadLibrary(Library &lib, std::span<const uint8_t> bytes, const LoadOptions &options);
Result<AppendResult>
appendLibrary(Library &lib, std::span<const uint8_t> bytes, const AppendOptions &options);
Result<LoadReport> resolvePlaceholders(Library &lib);

} // namespace detail

} // namespace coredcc
