#pragma once

#include "coredcc/lib/block.h"

namespace coredcc {

/**
 * What a client implements per block type (architecture §8a). Native or foreign (TS),
 * like any interface (§2a).
 *
 * - `data` resolves the block's data on every use; it may return null (a Detached
 *   block, a host object that went away).
 * - `foreachRef` is the single source of truth for the block's references. It is called
 *   for Live, InHistory and Detached blocks (a Detached block's targets must survive
 *   GC until the host restores it); report nothing if the data is gone. Report
 *   every RefCell in the block's data, strong and weak.
 * - `destroy` frees the data (for a host-owned block, only the wrapper). It runs during
 *   a sweep: library reads are allowed, void mutations are deferred, creates fail.
 * - `queryInterface` (optional) answers interfaces the block type doesn't implement
 *   itself, the last step of Library::query.
 * - `onWeakTargetLost` (optional) is told when a block it weakly references is about
 *   to be destroyed, so a child can keep its world transform.
 *
 * serialize, deserialize, duplicate and rekey come with library IO (P2), as optional
 * slots, which existing providers don't need to implement.
 */
struct BlockProvider : IfaceRef {
  void *data(const Datablock &block) const
  {
    return CDCC_CALL(data, block);
  }
  void foreachRef(const Datablock &block, RefVisitor &visitor) const
  {
    CDCC_CALL(foreachRef, block, visitor);
  }
  void destroy(const Datablock &block)
  {
    CDCC_CALL(destroy, block);
  }
  bool hasQueryInterface() const
  {
    return CDCC_HAS(queryInterface);
  }
  QueryAnswer queryInterface(const Datablock &block, InterfaceId iface) const
  {
    return CDCC_CALL(queryInterface, block, iface);
  }
  bool hasOnWeakTargetLost() const
  {
    return CDCC_HAS(onWeakTargetLost);
  }
  void onWeakTargetLost(const Datablock &user, const Datablock &lost)
  {
    CDCC_CALL(onWeakTargetLost, user, lost);
  }

  CDCC_INTERFACE(BlockProvider, "coredcc::BlockProvider")
  {
    BIND_STRUCT_METHOD(st, data, MARGS("block"));
    BIND_STRUCT_METHOD(st, foreachRef, MARGS("block", "visitor"));
    BIND_STRUCT_METHOD(st, destroy, MARGS("block"));
    BIND_OPTIONAL_METHOD(st, queryInterface, MARGS("block", "iface"));
    BIND_OPTIONAL_METHOD(st, onWeakTargetLost, MARGS("user", "lost"));
  }
};

} // namespace coredcc
