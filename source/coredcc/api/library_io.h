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
 * Adds the blocks of `bytes` to the Context's library.
 *
 * - Every block of the coredcc client gets a fresh id; other clients' blocks get what their
 *   type's `rekey` answers. A block whose type is missing, has no `rekey`, refuses, or answers
 *   an id that is in use or another client's, refuses the whole append (FailedPrecondition,
 *   naming the types).
 * - References within the file follow the new ids. Others find their blocks in the library, or
 *   get MissingBlock records. Blocks that were roots in the file become FakeUser here.
 * - Otherwise as loadLibrary: a refused append changes nothing, and one that isn't refused works.
 * - Not undoable by itself: see appendUndo (command/library_commands.h).
 */
Result<AppendResult> appendLibrary(Context &ctx,
                                   std::span<const uint8_t> bytes,
                                   const LoadOptions &options = {});

/**
 * Reads the MissingType placeholders and opaque attachments whose types have been registered
 * since the load. The report's blocks are those that changed; its issues say why the others
 * didn't.
 */
Result<LoadReport> resolvePlaceholders(Context &ctx);

} // namespace coredcc
