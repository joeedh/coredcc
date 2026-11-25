#include "coredcc/graph/system.h"

namespace coredcc {

GraphSystem::GraphSystem(Runtime *rt) : rt_(*rt), clients_(rt->ext<ClientRegistry>())
{
  connection_ = rt_.ext<ChangeBus>().addListener([this](const OwnerKey *o) { changed(o); });
}

uint64_t GraphSystem::versionOf(BlockId id)
{
  const BlockId canonical = clients_.canonical(id);
  if (const uint64_t *v = versions_.lookup_ptr(canonical)) {
    return *v;
  }
  const uint64_t v = rt_.nextVersion();
  versions_.add(canonical, v);
  return v;
}

void GraphSystem::changed(const OwnerKey *owner)
{
  if (!owner) {
    rt_.touch();
    versions_.clear(); /* each gets a new version when it is next asked for */
  }
  else if (owner->kind == OwnerKey::Kind::Block) {
    if (uint64_t *v = versions_.lookup_ptr(clients_.canonical(owner->block))) {
      *v = rt_.touch();
    }
  }
}

} // namespace coredcc
