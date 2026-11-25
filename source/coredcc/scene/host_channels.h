#pragma once

#include "coredcc/lib/pin.h"
#include "coredcc/scene/transform_registry.h"

#include "litestl/util/vector.h"

namespace coredcc::detail {

/** Copies of the channels of blocks other than coredcc.Object, for paths through their `transform`: a
 * Transformable gives values, and a path needs an instance. Writing one writes the block's channels.
 * A Runtime extension. */
class HostChannels {
 public:
  static constexpr const char *kExtName = "coredcc.HostChannels";
  explicit HostChannels(Runtime *) {}
  ~HostChannels();
  HostChannels(const HostChannels &) = delete;
  HostChannels &operator=(const HostChannels &) = delete;

  /** The block's copy, with these values in it. The same one each time. */
  ChannelBlock *put(const Library &lib, const BlockId &id, const ChannelBlock &channels);
  /** Whether it is a copy. */
  bool isCopy(const ChannelBlock *channels) const
  {
    return copies_.lookup_ptr(channels) != nullptr;
  }

 private:
  struct Key {
    const Library *lib = nullptr;
    BlockId id;
    uint64_t computeHash() const
    {
      return id.computeHash() ^ uint64_t(uintptr_t(lib) >> 4) * 0x9E3779B97F4A7C15ull;
    }
    bool operator==(const Key &b) const = default;
  };
  litestl::util::Map<Key, ChannelBlock *> byBlock_;
  litestl::util::Map<const ChannelBlock *, char> copies_;
};

} // namespace coredcc::detail
