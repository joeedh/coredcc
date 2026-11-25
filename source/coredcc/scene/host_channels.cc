#include "coredcc/scene/host_channels.h"

#include "litestl/util/alloc.h"

namespace coredcc::detail {

HostChannels::~HostChannels()
{
  for (auto &pair : byBlock_) {
    litestl::alloc::Delete(pair.value);
  }
}

ChannelBlock *
HostChannels::put(const Library &lib, const BlockId &id, const ChannelBlock &channels)
{
  const Key key{&lib, id};
  ChannelBlock *const *known = byBlock_.lookup_ptr(key);
  ChannelBlock *copy = known ? *known : nullptr;
  if (!copy) {
    copy = litestl::alloc::New<ChannelBlock>("coredcc::HostChannels");
    byBlock_.add(key, copy);
    copies_.add(static_cast<const ChannelBlock *>(copy), char(0));
  }
  *copy = channels;
  return copy;
}

} // namespace coredcc::detail
