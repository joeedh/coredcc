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
    touch(GraphEditNote());
    versions_.clear(); /* each gets a new version when it is next asked for */
  }
  else if (owner->kind == OwnerKey::Kind::Block) {
    GraphEditNote e;
    e.kind = GraphEditNote::Kind::Block;
    e.block = clients_.canonical(owner->block);
    if (uint64_t *v = versions_.lookup_ptr(e.block)) {
      *v = touch(e);
    }
  }
}

uint64_t GraphSystem::touch(const GraphEditNote &edit)
{
  if (!journalCurrent()) {
    note(GraphEditNote());
  }
  note(edit);
  noted_ = rt_.touch();
  return noted_;
}

void GraphSystem::note(const GraphEditNote &edit)
{
  if (journal_.size() < kJournal) {
    journal_.append(edit);
  }
  else {
    journal_[int(end_ % kJournal)] = edit;
  }
  end_++;
}

} // namespace coredcc
