#pragma once

/** \file
 * Reading and watching many paths at once, for the panels of scripts. The rules of
 * facades.h hold here too.
 */

#include "coredcc/bind/facades.h"

#include <memory>

namespace coredcc::script {

/**
 * A list of paths, and their values when they were last read. Copies share it.
 *
 * A reader that watches is connected to the ChangeBus of the Runtime and holds its
 * Context: it must be unwatched, or its last copy go, before that Context does.
 */
class PathReader {
public:
  struct Core;
  PathReader();
  const std::shared_ptr<Core> &core() const
  {
    return core_;
  }
  static const litestl::binding::types::Struct<PathReader> *defineBindings();

private:
  std::shared_ptr<Core> core_;
};

PathReader newPathReader();
/** Adds a path, which isn't read yet; its index. An error for one that doesn't parse. */
IntResult readerAdd(const PathReader &reader, const string &path);
int32_t readerCount(const PathReader &reader);
/** Reads every path. The error of one is in its readerValue. */
Status readerRead(Context &ctx, const PathReader &reader);
/** The value of path `i` when it was last read, or why it has none: FailedPrecondition if
 * it was never read, OutOfRange if there is no such path. */
ValueResult readerValue(const PathReader &reader, int32_t i);

/** Reads every path, and from now on reads those that flushChanges finds changed. Paths
 * that are added later aren't watched until the next call. Not from a script's slot. */
Status readerWatch(Context &ctx, const PathReader &reader);
/** Stops watching. The values stay. */
void readerUnwatch(const PathReader &reader);
/** A path that flushChanges found changed since it was last returned here, the lowest
 * index first; -1 when there is none. */
int32_t readerTakeChanged(const PathReader &reader);
/** As above, for paths whose `pathMeta` may have changed. */
int32_t readerTakeChangedMeta(const PathReader &reader);
/**
 * Takes all of both at once, or if `all` is set, every path: a JSON array, by index, of
 * `[index, what, kind, value]` with
 * - `what`: 1 if the value changed, 2 if the meta may have, or their sum;
 * - `kind` and `value`: `"n"` (None) without a value; `"b"` a Bool; `"i"` an Int; `"f"` a
 * Float;
 *   `"s"` a String; `"p"` a pointer's text or null; `"v"` an array of components; `"e"`
 * an enum's number, then its key if an item has it; `"g"` flags' bits, then the array of
 * their keys if items have them all; or `"!"`, for a path without a value: the error's
 * code, message and text. A number that JSON can't hold is a string (JsonWriter).
 */
StringResult readerTake(Context &ctx, const PathReader &reader, bool all);

/** Delivers the changes since the last flush to all watchers, of scripts and the host's
 * own. How many fired. Not from a script's slot. */
IntResult flushChanges(Context &ctx);
/** How many paths are watched, for tests of leaks. */
int32_t watchedCount(Context &ctx);

} // namespace coredcc::script
