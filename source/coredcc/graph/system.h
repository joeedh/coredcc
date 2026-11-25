#pragma once

/** \file
 * GraphSystem: versions of the blocks kernels use.
 */

#include "coredcc/api/change_bus.h"
#include "coredcc/base/runtime.h"
#include "coredcc/lib/registry.h"

#include "litestl/util/map.h"

namespace coredcc {

/**
 * A version per block that some kernel used, moved by every ChangeBus notification of the
 * block (and of all blocks, by notifyAll). Those count as edits (Runtime::touch), so the
 * next evaluation looks at its dependencies again. A Runtime extension.
 */
class GraphSystem {
 public:
  static constexpr const char *kExtName = "coredcc.GraphSystem";

  explicit GraphSystem(Runtime *rt);

  /** The version of a block. From the first call on, it follows the block's changes. */
  uint64_t versionOf(BlockId id);
  /** Blocks followed. */
  int blockCount() const
  {
    return int(versions_.size());
  }

 private:
  void changed(const OwnerKey *owner);

  Runtime &rt_;
  ClientRegistry &clients_;
  /** By canonical id. An id that became an alias leaves its entry behind, never found again;
   * its block has another version from then on, so its users run. */
  litestl::util::Map<BlockId, uint64_t> versions_;
  ScopedConnection connection_;
};

} // namespace coredcc
