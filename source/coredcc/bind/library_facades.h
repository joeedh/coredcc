#pragma once

/** \file
 * Saving and loading for scripts (api/library_io.h, command/library_commands.h). The
 * rules of facades.h hold here too. None of it runs in a script's slot, but
 * `appendLibrary` in `exec`.
 *
 * Reports are JSON objects. A block in them is `{block, name}`, where `block` is its id's
 * text (api/ref_text.h's formatBlockId). A list is `{total, first}`, with the first
 * 10,000 in `first`, so a file can't make a report large. Every report has `issues`, a
 * list of
 * `{block, name, code, message, suggestions}`, where `block` is left out of those about
 * the file, and `code` is a StatusCode's name.
 */

#include "coredcc/bind/facades.h"

#include "litestl/util/vector.h"

#include <memory>
#include <span>

namespace coredcc::script {

/** Bytes that C++ made. Copies share them. */
class ScriptBytes {
public:
  ScriptBytes() = default;
  explicit ScriptBytes(litestl::util::Vector<uint8_t> bytes);
  std::span<const uint8_t> bytes() const;
  static const litestl::binding::types::Struct<ScriptBytes> *defineBindings();

private:
  std::shared_ptr<const litestl::util::Vector<uint8_t>> bytes_;
};

int64_t bytesByteSize(const ScriptBytes &bytes);
/** Fills `out` with those from `offset`. OutOfRange unless there are as many. */
Status bytesRead(const ScriptBytes &bytes, int64_t offset, std::span<uint8_t> out);

struct SaveResult {
  Status status;
  ScriptBytes value;
  /** `blocks`, `loadBytes` and `loadBlocks` (SaveReport's counts), and the lists
   * `transient`, `throughTransient`, `hostPinned` and `detached`. */
  string report;
  static const litestl::binding::types::Struct<SaveResult> *defineBindings();
};

/** saveLibrary. */
SaveResult saveLibrary(Context &ctx);

/**
 * loadLibrary: `ctx`'s library must have no blocks. `sourceName` and the limits are
 * LoadOptions'; a limit of 0 is the default one. The report has `records`, `missingType`
 * and `missingBlock` (LoadReport's counts), and the list `blocks`.
 */
StringResult loadLibrary(Context &ctx,
                         std::span<const uint8_t> bytes,
                         const string &sourceName,
                         int64_t maxBytes,
                         int64_t maxBlocks);

/**
 * appendUndo, with options as loadLibrary's. The report is loadLibrary's, with the list
 * `ids` of
 * `{from, to}`, the texts of the file's ids and of the new ones.
 */
StringResult appendLibrary(Context &ctx,
                           std::span<const uint8_t> bytes,
                           const string &sourceName,
                           int64_t maxBytes,
                           int64_t maxBlocks);

/** resolvePlaceholders. FailedPrecondition while there is something to undo or redo. The
 * report is loadLibrary's. */
StringResult resolvePlaceholders(Context &ctx);

/** Forgets what could be undone and redone. */
Status clearHistory(Context &ctx);

} // namespace coredcc::script
