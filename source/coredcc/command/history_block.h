#pragma once

#include "coredcc/base/status.h"
#include "coredcc/lib/pin.h"

namespace coredcc {

class Context;

/**
 * A block as an undo record keeps it: pinned (PinKind::History), by handle and id. The id
 * finds the block again when its record was replaced by another one (a delete that left the
 * history, then a redo that recreated the block under its id). Move-only.
 */
class HistoryBlock {
 public:
  HistoryBlock() = default;
  HistoryBlock(Library &lib, BlockHandle h);

  /** The record with the id now: the pinned one, or the one that replaced it. Null when there
   * is none, or the Library is gone. */
  BlockHandle get() const;
  const BlockId &id() const
  {
    return id_;
  }
  /** The Library, or null once it is being destroyed. */
  Library *library() const
  {
    return pin_.library();
  }
  /** Unpins the handle it pinned (the id could name another record by now). */
  void release()
  {
    pin_.reset();
  }

 private:
  LibraryPin pin_;
  BlockId id_;
};

namespace detail {
/**
 * The Library an undo record was made in, for a slot that runs in `ctx`. FailedPrecondition when
 * it is gone, or `ctx` is of another: a host that undoes one document's command through another
 * document's Context must not change that one.
 */
Result<Library *> libraryOf(const Context &ctx, Library *madeIn);
inline Result<Library *> libraryOf(const Context &ctx, const LibraryRef &madeIn)
{
  return libraryOf(ctx, madeIn.get());
}
} // namespace detail

} // namespace coredcc
