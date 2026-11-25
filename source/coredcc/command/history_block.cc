#include "coredcc/command/history_block.h"

#include "coredcc/api/context.h"

namespace coredcc {

HistoryBlock::HistoryBlock(Library &lib, BlockHandle h)
{
  if (const Datablock *b = lib.get(h)) {
    pin_ = LibraryPin(lib, h, PinKind::History);
    id_ = b->id();
  }
}

BlockHandle HistoryBlock::get() const
{
  Library *lib = pin_.library();
  return lib ? lib->find(id_) : BlockHandle();
}

Result<Library *> detail::libraryOf(const Context &ctx, Library *madeIn)
{
  if (!madeIn) {
    return Status::error(StatusCode::FailedPrecondition, "the command's Library is gone");
  }
  if (ctx.library() != madeIn) {
    return Status::error(StatusCode::FailedPrecondition,
                         "the command was done in another Library than the Context's");
  }
  return madeIn;
}

} // namespace coredcc
