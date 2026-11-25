#include "coredcc/bind/path_reader.h"

#include "coredcc/api/change_bus.h"
#include "coredcc/api/ref_text.h"
#include "coredcc/api/resolve.h"
#include "coredcc/base/json_writer.h"
#include "coredcc/bind/detail.h"

#include "litestl/binding/binding.h"
#include "litestl/util/alloc.h"

#include <set>
#include <utility>
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
    Status status =
        Status::error(StatusCode::FailedPrecondition, "the path wasn't read yet");
    ScopedConnection watch;
  };
  /** By index: a callback finds its entry again, since adding moves them. */
  std::vector<Entry> entries;
  std::set<int> changed;
  std::set<int> changedMeta;

  static int take(std::set<int> &from)
  {
    if (from.empty()) {
      return -1;
    }
    const int i = *from.begin();
    from.erase(from.begin());
    return i;
  }
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

PathReader::PathReader() : core_(std::make_shared<Core>())
{
}

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
  core.changedMeta.clear();
  ChangeBus &bus = ctx.runtime().ext<ChangeBus>();
  for (int i = 0; i < readerCount(reader); i++) {
    ChangeBus::Start start;
    std::weak_ptr<PathReader::Core> weak = reader.core();
    Result<ScopedConnection> c = bus.watch(
        ctx,
        core.entries[size_t(i)].path,
        [weak, i](const PropValue &value, const Status &status) {
          if (std::shared_ptr<PathReader::Core> core = weak.lock()) {
            core->entries[size_t(i)].value = value;
            core->entries[size_t(i)].status = status;
            core->changed.insert(i);
          }
        },
        [weak, i]() {
          if (std::shared_ptr<PathReader::Core> core = weak.lock()) {
            core->changedMeta.insert(i);
          }
        },
        &start);
    if (!c.ok()) {
      core.unwatch();
      return c.status();
    }
    PathReader::Core::Entry &e = core.entries[size_t(i)];
    e.watch = std::move(c).value();
    e.value = std::move(start.value);
    e.status = std::move(start.status);
  }
  return Status();
}

void readerUnwatch(const PathReader &reader)
{
  reader.core()->unwatch();
}

int32_t readerTakeChanged(const PathReader &reader)
{
  return PathReader::Core::take(reader.core()->changed);
}

int32_t readerTakeChangedMeta(const PathReader &reader)
{
  return PathReader::Core::take(reader.core()->changedMeta);
}

/* An enum's or flags' keys, if items have all of it. Only they need the path read again.
 */
static void writeKeys(JsonWriter &out, Context &ctx, const Path &path, const PropValue &v)
{
  Result<ResolvedPath> rp = resolve(ctx, path);
  Result<PropertyDef> def = rp.ok() ? getMeta(ctx, *rp) : rp.status();
  Result<std::string> keys = def.ok() ? detail::keysOf(*def, v) : def.status();
  if (!keys.ok()) {
    return;
  }
  if (v.tryEnum()) {
    out.string(*keys);
    return;
  }
  out.beginArray();
  std::string_view rest = *keys;
  while (!rest.empty()) {
    const size_t at = rest.find('|');
    out.string(rest.substr(0, at));
    rest = at == rest.npos ? std::string_view() : rest.substr(at + 1);
  }
  out.endArray();
}

static void
writeEntry(JsonWriter &out, Context &ctx, const Path &path, const PropValue &v)
{
  switch (v.type()) {
  case PropValue::Type::None:
    out.string("n");
    return;
  case PropValue::Type::Bool:
    out.string("b").boolean(v.asBool());
    return;
  case PropValue::Type::Int:
    out.string("i").integer(v.asInt());
    return;
  case PropValue::Type::Float:
    out.string("f").number(v.asFloat());
    return;
  case PropValue::Type::String:
    out.string("s").string(v.asString().c_str());
    return;
  case PropValue::Type::Enum:
    out.string("e").integer(v.asEnum());
    writeKeys(out, ctx, path, v);
    return;
  case PropValue::Type::Flags:
    out.string("g").unsignedInteger(v.asFlags());
    writeKeys(out, ctx, path, v);
    return;
  case PropValue::Type::Pointer:
    out.string("p");
    if (v.asPointer().isNull()) {
      out.null();
    } else {
      out.string(formatRef(ctx.runtime(), v.asPointer()));
    }
    return;
  default:
    out.string("v").beginArray();
    for (int i = 0; i < v.componentCount(); i++) {
      out.number(v.component(i));
    }
    out.endArray();
  }
}

StringResult readerTake(Context &ctx, const PathReader &reader, bool all)
{
  if (Status s = detail::kernelGate(ctx, "readerTake"); !s.ok()) {
    return failed<StringResult>(std::move(s));
  }
  PathReader::Core &core = *reader.core();
  const std::set<int> changed = std::exchange(core.changed, {});
  const std::set<int> changedMeta = std::exchange(core.changedMeta, {});
  std::set<int> which = changed;
  which.insert(changedMeta.begin(), changedMeta.end());
  for (int i = 0; all && i < readerCount(reader); i++) {
    which.insert(i);
  }
  JsonWriter out;
  out.beginArray();
  for (const int i : which) {
    /* A copy: a dynamic hook's code may add paths. */
    const PathReader::Core::Entry &e = core.entries[size_t(i)];
    const PropValue value = e.value;
    const Path path = e.path;
    const Status status = e.status;
    out.beginArray().integer(i).integer(int(changed.contains(i)) +
                                        2 * int(changedMeta.contains(i)));
    if (status.ok()) {
      writeEntry(out, ctx, path, value);
    } else {
      out.string("!").integer(int(status.code())).string(status.message());
      out.string(status.toString().c_str());
    }
    out.endArray();
  }
  out.endArray();
  return ok<StringResult>(string(out.take().c_str()));
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
