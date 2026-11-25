#include "coredcc/graph/system.h"

namespace coredcc {

GraphSystem::GraphSystem(Runtime *rt) : rt_(*rt), clients_(rt->ext<ClientRegistry>())
{
  connection_ = rt_.ext<ChangeBus>().addChangeListener(
      [this](const BusChange &c) { changed(c); });
}

uint64_t GraphSystem::versionOf(BlockId id, ChangeAspects aspects)
{
  const BlockId canonical = clients_.canonical(id);
  if (!versions_.lookup_ptr(canonical)) {
    versions_.add(canonical, {});
  }
  auto &list = *versions_.lookup_ptr(canonical);
  for (const Version &v : list) {
    if (v.aspects == aspects) {
      return v.version;
    }
  }
  list.append(Version{aspects, rt_.nextVersion()});
  return list.last().version;
}

void GraphSystem::changed(const BusChange &change)
{
  const OwnerKey *owner = change.owner;
  if (!owner) {
    touch(GraphEditNote());
    versions_.clear(); /* each gets a new version when it is next asked for */
  }
  else if (owner->kind == OwnerKey::Kind::Block) {
    GraphEditNote e;
    e.kind = GraphEditNote::Kind::Block;
    e.block = clients_.canonical(owner->block);
    e.aspects = change.aspects;
    uint64_t version = 0;
    if (auto *list = versions_.lookup_ptr(e.block)) {
      for (Version &v : *list) {
        if (v.aspects & e.aspects) {
          version = version ? version : touch(e);
          v.version = version;
        }
      }
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
