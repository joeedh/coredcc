#pragma once

#include "coredcc/lib/block.h"
#include "coredcc/lib/block_io.h"

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
 * - `serialize` and `deserialize` (optional, together) write the block's data to a payload
 *   and make it again: what saving needs. `deserialize` makes new data and gives it to
 *   `in.setData`; on an error it has freed what it made. The record exists then (id, name,
 *   slots), but other blocks may not have their data yet.
 * - `duplicate` (optional) copies the data, references as they are; the library then remaps
 *   them through foreachRef.
 * - `rekey` (optional) gives an imported block an id the client can resolve (a UUID in host
 *   data). A null id refuses the import.
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
  bool hasSerialize() const
  {
    return CDCC_HAS(serialize);
  }
  Status serialize(const Datablock &block, BlockWriter &out) const
  {
    return CDCC_CALL(serialize, block, out);
  }
  bool hasDeserialize() const
  {
    return CDCC_HAS(deserialize);
  }
  Status deserialize(const Datablock &block, BlockReader &in)
  {
    return CDCC_CALL(deserialize, block, in);
  }
  bool hasDuplicate() const
  {
    return CDCC_HAS(duplicate);
  }
  Status duplicate(const Datablock &src, BlockCopy &out)
  {
    return CDCC_CALL(duplicate, src, out);
  }
  bool hasRekey() const
  {
    return CDCC_HAS(rekey);
  }
  BlockId rekey(const Datablock &imported) const
  {
    return CDCC_CALL(rekey, imported);
  }

  CDCC_INTERFACE(BlockProvider, "coredcc::BlockProvider")
  {
    BIND_STRUCT_METHOD(st, data, MARGS("block"));
    BIND_STRUCT_METHOD(st, foreachRef, MARGS("block", "visitor"));
    BIND_STRUCT_METHOD(st, destroy, MARGS("block"));
    BIND_OPTIONAL_METHOD(st, queryInterface, MARGS("block", "iface"));
    BIND_OPTIONAL_METHOD(st, onWeakTargetLost, MARGS("user", "lost"));
    BIND_OPTIONAL_METHOD(st, serialize, MARGS("block", "out"));
    BIND_OPTIONAL_METHOD(st, deserialize, MARGS("block", "in"));
    BIND_OPTIONAL_METHOD(st, duplicate, MARGS("src", "out"));
    BIND_OPTIONAL_METHOD(st, rekey, MARGS("imported"));
  }
};

} // namespace coredcc
