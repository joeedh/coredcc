#include "coredcc/command/history_block.h"

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

} // namespace coredcc
