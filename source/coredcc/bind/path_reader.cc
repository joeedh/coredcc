#include "coredcc/bind/path_reader.h"

#include "coredcc/api/change_bus.h"
#include "coredcc/api/resolve.h"
#include "coredcc/bind/detail.h"

#include "litestl/binding/binding.h"
#include "litestl/util/alloc.h"

#include <set>
#include <vector>

namespace coredcc::script {

using detail::error;
using detail::failed;
using detail::ok;
using detail::view;

struct PathReader::Core {
  struct Entry {
    Path path;
    PropValue value;
    Status status = Status::error(StatusCode::FailedPrecondition, "the path wasn't read yet");
    ScopedConnection watch;
  };
  /** By index: a callback finds its entry again, since adding moves them. */
  std::vector<Entry> entries;
  std::set<int> changed;

  void read(Context &ctx, int i)
  {
    Result<ResolvedPath> rp = resolve(ctx, entries[size_t(i)].path);
    Result<PropValue> v = rp.ok() ? coredcc::getValue(ctx, *rp) : rp.status();
    Entry &e = entries[size_t(i)];
    e.value = v.ok() ? *v : PropValue();
    e.status = v.status();
  }
  void unwatch()
  {
    for (Entry &e : entries) {
      e.watch = ScopedConnection();
    }
  }
};

PathReader::PathReader() : core_(std::make_shared<Core>()) {}

const litestl::binding::types::Struct<PathReader> *PathReader::defineBindings()
{
  static litestl::binding::types::Struct<PathReader> *st = nullptr;
  if (!st) {
    litestl::alloc::PermanentGuard guard;
    st = new litestl::binding::types::Struct<PathReader>("coredcc::PathReader",
                                                         sizeof(PathReader));
    BIND_STRUCT_COPY_CONSTRUCTOR(st);
  }
  return st;
}

PathReader newPathReader()
{
  return PathReader();
}

IntResult readerAdd(const PathReader &reader, const string &path)
{
  Result<Path> p = parsePath(view(path));
  if (!p.ok()) {
    return failed<IntResult>(p.status());
  }
  PathReader::Core &core = *reader.core();
  core.entries.emplace_back();
  core.entries.back().path = std::move(p).value();
  return ok<IntResult>(int64_t(core.entries.size()) - 1);
}

int32_t readerCount(const PathReader &reader)
{
  return int32_t(reader.core()->entries.size());
}

Status readerRead(Context &ctx, const PathReader &reader)
{
  CDCC_TRY(detail::kernelGate(ctx, "readerRead"));
  for (int i = 0; i < readerCount(reader); i++) {
    reader.core()->read(ctx, i);
  }
  return Status();
}

ValueResult readerValue(const PathReader &reader, int32_t i)
{
  if (i < 0 || i >= readerCount(reader)) {
    return failed<ValueResult>(error(StatusCode::OutOfRange,
                                     "path " + std::to_string(i) + " of a reader of " +
                                         std::to_string(readerCount(reader))));
  }
  const PathReader::Core::Entry &e = reader.core()->entries[size_t(i)];
  return e.status.ok() ? ok<ValueResult>(e.value) : failed<ValueResult>(e.status);
}

Status readerWatch(Context &ctx, const PathReader &reader)
{
  CDCC_TRY(detail::slotGate(ctx, "readerWatch", {}));
  PathReader::Core &core = *reader.core();
  core.unwatch();
  core.changed.clear();
  ChangeBus &bus = ctx.runtime().ext<ChangeBus>();
  for (int i = 0; i < readerCount(reader); i++) {
    core.read(ctx, i);
    std::weak_ptr<PathReader::Core> weak = reader.core();
    Result<ScopedConnection> c =
        bus.watch(ctx, core.entries[size_t(i)].path,
                  [weak, i](const PropValue &value, const Status &status) {
                    if (std::shared_ptr<PathReader::Core> core = weak.lock()) {
                      core->entries[size_t(i)].value = value;
                      core->entries[size_t(i)].status = status;
                      core->changed.insert(i);
                    }
                  });
    if (!c.ok()) {
      core.unwatch();
      return c.status();
    }
    core.entries[size_t(i)].watch = std::move(c).value();
  }
  return Status();
}

void readerUnwatch(const PathReader &reader)
{
  reader.core()->unwatch();
}

int32_t readerTakeChanged(const PathReader &reader)
{
  std::set<int> &changed = reader.core()->changed;
  if (changed.empty()) {
    return -1;
  }
  const int i = *changed.begin();
  changed.erase(changed.begin());
  return i;
}

IntResult flushChanges(Context &ctx)
{
  if (Status s = detail::slotGate(ctx, "flushChanges", {}); !s.ok()) {
    return failed<IntResult>(std::move(s));
  }
  return ok<IntResult>(int64_t(ctx.runtime().ext<ChangeBus>().flush()));
}

int32_t watchedCount(Context &ctx)
{
  return ctx.runtime().ext<ChangeBus>().watcherCount();
}

} // namespace coredcc::script
