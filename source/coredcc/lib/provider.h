#pragma once

#include "coredcc/lib/block.h"
#include "coredcc/lib/block_io.h"

namespace coredcc {

/** What BlockProvider::rekey may ask about the append. */
class RekeyContext {
public:
  using IsFree = bool (*)(const void *self, const BlockId &id);
  RekeyContext(Library &lib, const void *self, IsFree isFree)
      : lib_(lib), self_(self), isFree_(isFree)
  {
  }

  Library &library() const
  {
    return lib_;
  }
  /** Whether the imported block may get `id`: it isn't null, no block of the append has
   * it, and none here, except a MissingBlock that the import can fill. */
  bool isFree(const BlockId &id) const
  {
    return isFree_(self_, id);
  }

  static const litestl::binding::types::Struct<RekeyContext> *defineBindings();

private:
  Library &lib_;
  const void *self_;
  IsFree isFree_;
};

/**
 * What a client implements per block type (architecture §8a). Native or foreign (TS),
 * like any interface (§2a). A provider serves every Library of its Runtime: the block
 * says which one it is in.
 *
 * - `data` resolves the block's data on every use; it may return null (a Detached
 *   block, a host object that went away).
 * - `forEachRef` is the single source of truth for the block's references. It is called
 * for Live, InHistory, Detached and Doomed blocks (a Detached block's targets must
 * survive GC until the host restores it, and a Doomed one's while something pins its
 * data); report nothing if the data is gone. Report every RefCell in the block's data,
 * strong and weak.
 * - `destroy` frees the data (for a host-owned block, only the wrapper). It runs during
 *   a sweep: library reads are allowed, void mutations are deferred, creates fail.
 * - `queryInterface` (optional) answers interfaces the block type doesn't implement
 *   itself, the last step of Library::query. Its answer may change only with a ChangeBus
 *   notification of the block or a move of the Library's epoch (see Library::query).
 * - `onWeakTargetLost` (optional) is told when a block that `user`'s data weakly
 * references is about to be destroyed, so a child can keep its world transform. `user`
 * has its data: it is Live, or InHistory from Live.
 *
 * - `serialize` and `deserialize` (optional, together) write the block's data to a
 * payload and make it again: what saving needs. `deserialize` makes new data and gives it
 * to `in.setData`; on an error it has freed what it made. The record exists then (id,
 * name, slots), but other blocks may not have their data yet.
 * - `duplicate` (optional) copies the data, references as they are; the library then
 * remaps them through forEachRef.
 * - `rekey` (optional) gives an imported block an id the client can resolve (a UUID in
 * host data), one that `context.isFree`. A null id refuses the import. `imported` isn't a
 * record yet; it has the id the file has, the type and the name. The id of a MissingBlock
 * is free: the import then fills it.
 * - `byteSize` (optional) is about what the block's data takes in memory, in bytes. The
 * undo history counts it for blocks that only the history holds.
 */
struct BlockProvider : IfaceRef {
  void *data(const Datablock &block) const
  {
    return CDCC_CALL(data, block);
  }
  void forEachRef(const Datablock &block, RefVisitor &visitor) const
  {
    CDCC_CALL(forEachRef, block, visitor);
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
  BlockId rekey(const Datablock &imported, const RekeyContext &context) const
  {
    return CDCC_CALL(rekey, imported, context);
  }
  bool hasByteSize() const
  {
    return CDCC_HAS(byteSize);
  }
  uint64_t byteSize(const Datablock &block) const
  {
    return CDCC_CALL(byteSize, block);
  }

  CDCC_INTERFACE(BlockProvider, "coredcc::BlockProvider")
  {
    BIND_STRUCT_METHOD(st, data, MARGS("block"));
    BIND_STRUCT_METHOD(st, forEachRef, MARGS("block", "visitor"));
    BIND_STRUCT_METHOD(st, destroy, MARGS("block"));
    BIND_OPTIONAL_METHOD(st, queryInterface, MARGS("block", "iface"));
    BIND_OPTIONAL_METHOD(st, onWeakTargetLost, MARGS("user", "lost"));
    BIND_OPTIONAL_METHOD(st, serialize, MARGS("block", "out"));
    BIND_OPTIONAL_METHOD(st, deserialize, MARGS("block", "in"));
    BIND_OPTIONAL_METHOD(st, duplicate, MARGS("src", "out"));
    BIND_OPTIONAL_METHOD(st, rekey, MARGS("imported", "context"));
    BIND_OPTIONAL_METHOD(st, byteSize, MARGS("block"));
  }
};

} // namespace coredcc
