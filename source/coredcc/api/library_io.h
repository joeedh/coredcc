#pragma once

/** \file
 * Loading the library section (coredcc/lib/io/library_io.h has saving, limits and reports).
 * Each call posts a change of every block it touched.
 */

#include "coredcc/api/context.h"
#include "coredcc/lib/io/library_io.h"

namespace coredcc {

/**
 * Loads `bytes` into the Context's library, which must have no records yet.
 *
 * - A damaged file, or one past the limits, fails the load and changes nothing:
 *   InvalidArgument, OutOfRange, or Unsupported for a file that needs a newer build.
 * - Past that point the load always works. A block whose type is missing, is another
 *   client's, is older than the payload, or fails to read it, becomes a MissingType
 *   placeholder that a save writes back unchanged; an attachment becomes opaque in the same
 *   way. Strong references to blocks that aren't in the file get MissingBlock records. The
 *   report lists all of it.
 * - Ids are kept, except where a client alias makes one id of two: the block under the old
 *   name gets a new one.
 * - Not undoable. Busy during a sweep, a load or an evaluation.
 */
Result<LoadReport> loadLibrary(Context &ctx,
                               std::span<const uint8_t> bytes,
                               const LoadOptions &options = {});

/**
 * Reads the MissingType placeholders and opaque attachments whose types have been registered
 * since the load. The report's blocks are those that changed; its issues say why the others
 * didn't.
 */
Result<LoadReport> resolvePlaceholders(Context &ctx);

} // namespace coredcc
