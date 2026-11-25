#pragma once

#include "coredcc/base/block_id.h"

#include <map>

namespace coredcc {

/** Which block each source block became, in an append or a duplicate. */
class IdMap {
public:
  void add(BlockId from, BlockId to)
  {
    ids_.emplace(from, to);
  }
  /** What `from` became, or a null id. */
  BlockId find(BlockId from) const
  {
    const auto it = ids_.find(from);
    return it == ids_.end() ? BlockId() : it->second;
  }
  int size() const
  {
    return int(ids_.size());
  }
  /** Calls fn(BlockId from, BlockId to), in the order of `from`. */
  template <class Fn> void forEach(Fn &&fn) const
  {
    for (const auto &pair : ids_) {
      fn(pair.first, pair.second);
    }
  }

private:
  /* Not hashed: the ids come from files. */
  std::map<BlockId, BlockId> ids_;
};

} // namespace coredcc
