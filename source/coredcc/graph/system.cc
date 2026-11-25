#include "coredcc/graph/system.h"

#include <algorithm>

namespace coredcc {

using litestl::util::Map;

GraphSystem::GraphSystem(Runtime *rt) : rt_(*rt), clients_(rt->ext<ClientRegistry>())
{
  connection_ =
      rt_.ext<ChangeBus>().addChangeListener([this](const BusChange &c) { changed(c); });
}

BlockVersionPtr GraphSystem::follow(BlockId id, ChangeAspects aspects)
{
  const BlockId canonical = clients_.canonical(id);
  if (!versions_.lookup_ptr(canonical)) {
    if (size_t(versions_.size()) >= pruneAt_) {
      prune();
    }
    versions_.add(canonical, Versions());
  }
  Versions &list = *versions_.lookup_ptr(canonical);
  for (const auto &v : list) {
    if (v->aspects == aspects) {
      return v;
    }
  }
  list.append(std::make_shared<BlockVersion>(BlockVersion{aspects, rt_.nextVersion()}));
  return list.last();
}

void GraphSystem::prune()
{
  Map<BlockId, Versions> held;
  for (auto &pair : versions_) {
    Versions list;
    for (const auto &v : pair.value) {
      if (v.use_count() > 1) {
        list.append(v);
      }
    }
    if (list.size() > 0) {
      held.add(pair.key, std::move(list));
    }
  }
  versions_ = std::move(held);
  pruneAt_ = std::max(kPrune, 2 * size_t(versions_.size()));
}

void GraphSystem::changed(const BusChange &change)
{
  const OwnerKey *owner = change.owner;
  if (!owner) {
    touch(GraphEditNote());
    versions_.clear(); /* each gets a new version when it is next asked for */
    pruneAt_ = kPrune;
  } else if (owner->kind == OwnerKey::Kind::Block) {
    GraphEditNote e;
    e.kind = GraphEditNote::Kind::Block;
    e.block = clients_.canonical(owner->block);
    e.aspects = change.aspects;
    uint64_t version = 0;
    if (Versions *list = versions_.lookup_ptr(e.block)) {
      for (const auto &v : *list) {
        if (v->aspects & e.aspects) {
          version = version ? version : touch(e);
          v->version = version;
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
  } else {
    journal_[int(end_ % kJournal)] = edit;
  }
  end_++;
}

} // namespace coredcc
