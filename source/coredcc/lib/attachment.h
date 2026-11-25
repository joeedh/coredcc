#pragma once

/** \file
 * Attachment types: client data attached to blocks of other clients (architecture §8a).
 */

#include "coredcc/base/iface/owned.h"
#include "coredcc/base/runtime.h"
#include "coredcc/lib/block_io.h"

#include "litestl/util/map.h"
#include "litestl/util/string.h"
#include "litestl/util/vector.h"

namespace coredcc {

/**
 * What a client implements per attachment type. Only `destroy` is required.
 *
 * - `destroy` frees the data. It runs when the block is destroyed, under the same rules as
 *   BlockProvider::destroy.
 * - `create` makes empty data, for code that attaches by type name alone.
 * - `copy` makes a copy for a duplicated block (DuplicatePolicy::Copy needs it). References
 *   are copied as they are; the library remaps them.
 * - `serialize` and `deserialize` come together, and work as BlockProvider's do. A type
 *   without them isn't saved.
 * - `foreachRef` reports the data's references, as BlockProvider::foreachRef does.
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
  void *create()
  {
    return CDCC_CALL(create);
  }
  bool hasCopy() const
  {
    return CDCC_HAS(copy);
  }
  void *copy(const void *data)
  {
    return CDCC_CALL(copy, data);
  }
  bool hasSerialize() const
  {
    return CDCC_HAS(serialize);
  }
  Status serialize(const void *data, BlockWriter &out) const
  {
    return CDCC_CALL(serialize, data, out);
  }
  bool hasDeserialize() const
  {
    return CDCC_HAS(deserialize);
  }
  Status deserialize(BlockReader &in)
  {
    return CDCC_CALL(deserialize, in);
  }
  bool hasForeachRef() const
  {
    return CDCC_HAS(foreachRef);
  }
  void foreachRef(const void *data, RefVisitor &visitor) const
  {
    CDCC_CALL(foreachRef, data, visitor);
  }

  CDCC_INTERFACE(AttachmentProvider, "coredcc::AttachmentProvider")
  {
    BIND_STRUCT_METHOD(st, destroy, MARGS("data"));
    BIND_OPTIONAL_METHOD(st, create, MARGS());
    BIND_OPTIONAL_METHOD(st, copy, MARGS("data"));
    BIND_OPTIONAL_METHOD(st, serialize, MARGS("data", "out"));
    BIND_OPTIONAL_METHOD(st, deserialize, MARGS("in"));
    BIND_OPTIONAL_METHOD(st, foreachRef, MARGS("data", "visitor"));
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
  uint32_t version = 1;
  DuplicatePolicy onDuplicate = DuplicatePolicy::Drop;
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

  explicit AttachmentTypeRegistry(Runtime *rt) : rt_(*rt) {}
  ~AttachmentTypeRegistry();
  AttachmentTypeRegistry(const AttachmentTypeRegistry &) = delete;
  AttachmentTypeRegistry &operator=(const AttachmentTypeRegistry &) = delete;

  /** Registers a type. Takes the provider. Refuses one of serialize and deserialize without
   * the other, DuplicatePolicy::Copy without `copy`, and a block type's name. */
  Result<TypeId> add(AttachmentTypeInfo info, Owned<AttachmentProvider> provider);
  const Entry *find(TypeId type) const;

  Status validate() const;

 private:
  Runtime &rt_;
  litestl::util::Vector<Entry *> entries_;
  litestl::util::Map<uint64_t, Entry *> byId_;
};

} // namespace coredcc
