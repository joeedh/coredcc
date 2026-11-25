#include "coredcc/graph/trace.h"

#include "coredcc/graph/system.h"

namespace coredcc::graph {

int TypeChains::extend(int chain, const SocketType *type)
{
  const Key key{chain, type};
  if (const int *id = ids_.lookup_ptr(key)) {
    return *id;
  }
  const int id = int(ids_.size());
  ids_.add(key, id);
  return id;
}

void DepSet::clear()
{
  blocks_.clear();
  queries_.clear();
}

void DepSet::add(const Trace &from, Trace &to)
{
  if (!from.more) {
    return;
  }
  for (const BlockDep &d : from.more->blocks) {
    if (blocks_.add(d.id, true)) {
      to.blocks().blocks.append(d);
    }
  }
  for (const QueryDep &d : from.more->queries) {
    if (queries_.add(QueryKey{d.handle, d.iface.value}, true)) {
      to.blocks().queries.append(d);
    }
  }
}

BlockReader::BlockReader(Runtime &rt, LibraryRef lib)
    : rt_(rt), system_(rt.ext<GraphSystem>()), lib_(std::move(lib))
{
}

BlockDep BlockReader::read(BlockId id) const
{
  BlockDep d;
  d.id = id;
  d.version = system_.versionOf(id);
  const Library *lib = lib_.get();
  d.handle = lib ? lib->find(id) : BlockHandle();
  if (const Datablock *b = lib ? lib->get(d.handle) : nullptr) {
    d.state = b->state();
    d.stateChanges = b->stateChanges();
    d.renames = b->renames();
  }
  return d;
}

QueryDep BlockReader::read(BlockHandle h, InterfaceId iface) const
{
  const Library *lib = lib_.get();
  const IfaceRef r = lib ? lib->queryRaw(h, iface) : IfaceRef();
  return QueryDep{h, iface, r.table(), r.self()};
}

bool BlockReader::current(const Trace &t) const
{
  if (!t.more) {
    return true;
  }
  for (const BlockDep &d : t.more->blocks) {
    if (!(read(d.id) == d)) {
      return false;
    }
  }
  for (const QueryDep &d : t.more->queries) {
    if (!(read(d.handle, d.iface) == d)) {
      return false;
    }
    if (rt_.hasStickyError()) {
      rt_.takeStickyError().ignore(); /* the kernel runs again, and gets it itself */
      return false;
    }
  }
  return true;
}

BlockUse BlockRecorder::useBlock(BlockId id)
{
  if (!reader_.library()) {
    return BlockUse();
  }
  BlockDep d = reader_.read(id);
  const BlockUse use{d.handle, reader_.library()->data(d.handle)};
  trace_.blocks().blocks.append(d);
  return use;
}

IfaceRef BlockRecorder::queryBlock(BlockHandle h, InterfaceId iface)
{
  if (!reader_.library()) {
    return IfaceRef();
  }
  const QueryDep d = reader_.read(h, iface);
  trace_.blocks().queries.append(d);
  return IfaceRef(d.table, d.self);
}

} // namespace coredcc::graph
