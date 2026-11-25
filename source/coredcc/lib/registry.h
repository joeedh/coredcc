#pragma once

/** \file
 * The lib module's Runtime extensions: clients (with aliases) and block types.
 */

#include "coredcc/base/iface/owned.h"
#include "coredcc/base/runtime.h"
#include "coredcc/lib/provider.h"

#include "litestl/util/map.h"
#include "litestl/util/string.h"
#include "litestl/util/vector.h"

#include <span>
#include <string_view>

namespace coredcc {

/** The built-in client's name: blocks coredcc itself creates (Object, Scene, ...). */
inline constexpr const char *kCoredccClient = "coredcc";

/**
 * Clients by name, with aliases. An alias maps an old client name to its current one,
 * so blocks saved under the old name still resolve (architecture §8a). Created with
 * the built-in "coredcc" client registered.
 */
class ClientRegistry {
public:
  static constexpr const char *kExtName = "coredcc.ClientRegistry";

  explicit ClientRegistry(Runtime *rt);

  /** Registers a client and its aliases. Registering the same client again with the
   * same aliases is fine; a name that is already another client's alias is not. */
  Result<ClientId> registerClient(std::string_view name,
                                  std::span<const char *const> aliases = {});
  bool isRegistered(ClientId id) const;
  /** The canonical id for `id`: its client if `id` is an alias, else `id`. */
  ClientId canonical(ClientId id) const;
  /** `id` with its client made canonical. */
  BlockId canonical(BlockId id) const
  {
    return BlockId{canonical(id.client), id.local};
  }

  /** Bumped whenever an alias is added, so Libraries re-key records saved under a name
   * that has since become an alias (a placeholder loaded before its plugin). */
  uint64_t generation() const
  {
    return generation_;
  }

  Status validate() const;

private:
  Runtime &rt_;
  uint64_t generation_ = 0;
  litestl::util::Vector<ClientId> clients_;
  /** Alias id -> canonical id. */
  litestl::util::Map<uint64_t, uint64_t> aliases_;
};

/** How the data of a block type or an attachment type is saved. */
enum class BlockStorage : uint8_t {
  /** Not said: Payload if the provider has serialize and deserialize. Otherwise a save of
   * such data fails, naming the type. */
  Unspecified,
  /** Through the provider's serialize and deserialize. */
  Payload,
  /** Never saved, on purpose. */
  Transient,
};

/** A block type, as a client registers it. */
struct BlockTypeInfo {
  /** The full type name, e.g. "coredcc.Mesh"; its TypeId is the registry key. */
  litestl::util::string name;
  ClientId client;
  /** The bound struct of the block's data, for the datapath layer. */
  StructId dataStruct;
  /** The payload version serialize writes; deserialize reads it and older ones. */
  uint32_t payloadVersion = 1;
  BlockStorage storage = BlockStorage::Unspecified;
};

/**
 * Block types and their providers. A Runtime extension. A block type's name is also
 * the implementation name its data's capabilities are registered under in the
 * interface registry, so register a provider class under another name
 * (`<type>#provider`).
 */
class BlockTypeRegistry {
public:
  static constexpr const char *kExtName = "coredcc.BlockTypeRegistry";

  struct Entry {
    BlockTypeInfo info;
    TypeId id;
    Owned<BlockProvider> provider;
  };

  explicit BlockTypeRegistry(Runtime *rt) : rt_(*rt)
  {
  }
  ~BlockTypeRegistry();
  BlockTypeRegistry(const BlockTypeRegistry &) = delete;
  BlockTypeRegistry &operator=(const BlockTypeRegistry &) = delete;

  /** Registers a block type. Its client must be registered. Takes the provider. Refuses
   * one of serialize and deserialize without the other, and a `storage` that contradicts
   * them, and an attachment type's name (slots belong to either kind by TypeId). */
  Result<TypeId> add(BlockTypeInfo info, Owned<BlockProvider> provider);
  const Entry *find(TypeId type) const;
  /** The provider of `type`, or a null interface. */
  BlockProvider provider(TypeId type) const;

  Status validate() const;

private:
  Runtime &rt_;
  litestl::util::Vector<Entry *> entries_;
  litestl::util::Map<uint64_t, Entry *> byId_;
};

} // namespace coredcc
