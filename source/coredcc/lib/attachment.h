#pragma once

/** \file
 * Attachment types: client data attached to blocks of other clients (architecture §8a).
 */

#include "coredcc/base/iface/owned.h"
#include "coredcc/base/runtime.h"
#include "coredcc/lib/block_io.h"
#include "coredcc/lib/registry.h"

#include "litestl/util/map.h"
#include "litestl/util/string.h"
#include "litestl/util/vector.h"

namespace coredcc {

/**
 * What a client implements per attachment type. Only `destroy` is required. `block` is
 * the block the data is (or will be) attached to.
 *
 * - `destroy` frees the data. It runs when the block is destroyed, under the same rules
 * as BlockProvider::destroy. It gets no block: the history can hold detached data for
 * longer than the block, and the Library, exist.
 * - `create` makes empty data, for code that attaches by type name alone.
 * - `copy` makes a copy for a duplicate of `from` (DuplicatePolicy::Copy needs it).
 * References are copied as they are; the library remaps them.
 * - `serialize` and `deserialize` come together, and work as BlockProvider's do. A type
 * without them says in its info that it is Transient.
 * - `forEachRef` reports the data's references, as BlockProvider::forEachRef does. A save
 * checks them against what `serialize` wrote.
 * - `onWeakTargetLost` is told when a block that the data weakly references (`forEachRef`
 * says) is about to be destroyed. The block may be in any state.
 */
struct AttachmentProvider : IfaceRef {
  void destroy(void *data)
  {
    CDCC_CALL(destroy, data);
  }
  bool hasCreate() const
  {
    return CDCC_HAS(create);
  }
  void *create(const Datablock &block)
  {
    return CDCC_CALL(create, block);
  }
  bool hasCopy() const
  {
    return CDCC_HAS(copy);
  }
  void *copy(const Datablock &from, const void *data, const Datablock &to)
  {
    return CDCC_CALL(copy, from, data, to);
  }
  bool hasSerialize() const
  {
    return CDCC_HAS(serialize);
  }
  Status serialize(const Datablock &block, const void *data, BlockWriter &out) const
  {
    return CDCC_CALL(serialize, block, data, out);
  }
  bool hasDeserialize() const
  {
    return CDCC_HAS(deserialize);
  }
  Status deserialize(const Datablock &block, BlockReader &in)
  {
    return CDCC_CALL(deserialize, block, in);
  }
  bool hasForeachRef() const
  {
    return CDCC_HAS(forEachRef);
  }
  void forEachRef(const Datablock &block, const void *data, RefVisitor &visitor) const
  {
    CDCC_CALL(forEachRef, block, data, visitor);
  }
  bool hasOnWeakTargetLost() const
  {
    return CDCC_HAS(onWeakTargetLost);
  }
  void onWeakTargetLost(const Datablock &block, void *data, const Datablock &lost)
  {
    CDCC_CALL(onWeakTargetLost, block, data, lost);
  }

  CDCC_INTERFACE(AttachmentProvider, "coredcc::AttachmentProvider")
  {
    BIND_STRUCT_METHOD(st, destroy, MARGS("data"));
    BIND_OPTIONAL_METHOD(st, create, MARGS("block"));
    BIND_OPTIONAL_METHOD(st, copy, MARGS("from", "data", "to"));
    BIND_OPTIONAL_METHOD(st, serialize, MARGS("block", "data", "out"));
    BIND_OPTIONAL_METHOD(st, deserialize, MARGS("block", "in"));
    BIND_OPTIONAL_METHOD(st, forEachRef, MARGS("block", "data", "visitor"));
    BIND_OPTIONAL_METHOD(st, onWeakTargetLost, MARGS("block", "data", "lost"));
  }
};

/** What happens to an attachment when its block is duplicated. */
enum class DuplicatePolicy : uint8_t { Copy, Drop };

/** An attachment type, as a client registers it. */
struct AttachmentTypeInfo {
  /** The full type name; its TypeId is the registry key. */
  litestl::util::string name;
  /** The bound struct of the data, for the datapath layer. */
  StructId dataStruct;
  /** The payload version `serialize` writes. */
  uint32_t payloadVersion = 1;
  DuplicatePolicy onDuplicate = DuplicatePolicy::Drop;
  BlockStorage storage = BlockStorage::Unspecified;
};

/** Attachment types and their providers. A Runtime extension. */
class AttachmentTypeRegistry {
public:
  static constexpr const char *kExtName = "coredcc.AttachmentTypeRegistry";

  struct Entry {
    AttachmentTypeInfo info;
    TypeId id;
    Owned<AttachmentProvider> provider;
  };

  explicit AttachmentTypeRegistry(Runtime *rt) : rt_(*rt)
  {
  }
  ~AttachmentTypeRegistry();
  AttachmentTypeRegistry(const AttachmentTypeRegistry &) = delete;
  AttachmentTypeRegistry &operator=(const AttachmentTypeRegistry &) = delete;

  /** Registers a type. Takes the provider. Refuses one of serialize and deserialize
   * without the other, a `storage` that contradicts them, DuplicatePolicy::Copy without
   * `copy`, and a block type's name. */
  Result<TypeId> add(AttachmentTypeInfo info, Owned<AttachmentProvider> provider);
  const Entry *find(TypeId type) const;

  Status validate() const;

private:
  Runtime &rt_;
  litestl::util::Vector<Entry *> entries_;
  litestl::util::Map<uint64_t, Entry *> byId_;
};

} // namespace coredcc
