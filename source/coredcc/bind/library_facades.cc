#include "coredcc/bind/library_facades.h"

#include "coredcc/api/library_io.h"
#include "coredcc/api/ref_text.h"
#include "coredcc/base/json_writer.h"
#include "coredcc/bind/detail.h"
#include "coredcc/command/executor.h"
#include "coredcc/command/library_commands.h"
#include "coredcc/lib/library.h"

#include "litestl/binding/binding.h"
#include "litestl/util/alloc.h"

#include <algorithm>
#include <limits>

namespace coredcc::script {

using detail::error;
using detail::failed;
using detail::ok;
using litestl::util::Vector;

ScriptBytes::ScriptBytes(Vector<uint8_t> bytes)
    : bytes_(std::make_shared<const Vector<uint8_t>>(std::move(bytes)))
{
}

std::span<const uint8_t> ScriptBytes::bytes() const
{
  return bytes_ ? std::span<const uint8_t>(bytes_->data(), size_t(bytes_->size())) :
                  std::span<const uint8_t>();
}

const litestl::binding::types::Struct<ScriptBytes> *ScriptBytes::defineBindings()
{
  static litestl::binding::types::Struct<ScriptBytes> *st = nullptr;
  if (!st) {
    litestl::alloc::PermanentGuard guard;
    st = new litestl::binding::types::Struct<ScriptBytes>("coredcc::ScriptBytes",
                                                          sizeof(ScriptBytes));
    BIND_STRUCT_COPY_CONSTRUCTOR(st);
  }
  return st;
}

const litestl::binding::types::Struct<SaveResult> *SaveResult::defineBindings()
{
  static litestl::binding::types::Struct<SaveResult> *st = nullptr;
  if (!st) {
    litestl::alloc::PermanentGuard guard;
    st = new litestl::binding::types::Struct<SaveResult>("coredcc::SaveResult",
                                                         sizeof(SaveResult));
    BIND_STRUCT_COPY_CONSTRUCTOR(st);
    BIND_STRUCT_MEMBER(st, status);
    BIND_STRUCT_MEMBER(st, value);
    BIND_STRUCT_MEMBER(st, report);
  }
  return st;
}

int64_t bytesSize(const ScriptBytes &bytes)
{
  return int64_t(bytes.bytes().size());
}

Status bytesRead(const ScriptBytes &bytes, int64_t offset, std::span<uint8_t> out)
{
  const std::span<const uint8_t> all = bytes.bytes();
  if (offset < 0 || uint64_t(offset) > all.size() || out.size() > all.size() - size_t(offset)) {
    return error(StatusCode::OutOfRange,
                 std::to_string(out.size()) + " bytes at " + std::to_string(offset) + " of " +
                     std::to_string(all.size()));
  }
  std::copy_n(all.begin() + std::ptrdiff_t(offset), out.size(), out.begin());
  return Status();
}

namespace {

/* Of a list in a report. */
constexpr int64_t kMaxListed = 10000;

class Report {
 public:
  Report(const Runtime &rt, const Library &lib) : rt_(rt), lib_(lib)
  {
    out_.beginObject();
  }

  void count(const char *key, uint64_t n)
  {
    out_.key(key).unsignedInteger(n);
  }
  void list(const char *key, const Vector<BlockHandle> &handles)
  {
    begin(key, int64_t(handles.size()));
    for (int64_t i = 0; i < std::min<int64_t>(int64_t(handles.size()), kMaxListed); i++) {
      const Datablock *b = lib_.get(handles[int(i)]);
      out_.beginObject();
      block(b ? b->id() : BlockId(), b ? b->name().c_str() : "");
      out_.endObject();
    }
    end();
  }
  void ids(const IdMap &ids)
  {
    begin("ids", ids.size());
    int64_t n = 0;
    ids.forEach([&](const BlockId &from, const BlockId &to) {
      if (n++ < kMaxListed) {
        out_.beginObject();
        out_.key("from").string(formatBlockId(rt_, from));
        out_.key("to").string(formatBlockId(rt_, to));
        out_.endObject();
      }
    });
    end();
  }
  void issues(const IoIssues &issues)
  {
    begin("issues", issues.total);
    for (const IoIssue &i : issues.first) {
      out_.beginObject();
      if (!i.block.isNull()) {
        block(i.block, i.blockName.c_str());
      }
      out_.key("code").string(statusCodeName(i.why.code()));
      out_.key("message").string(i.why.message());
      out_.key("suggestions").beginArray();
      for (const string &s : i.why.suggestions()) {
        out_.string(s.c_str());
      }
      out_.endArray();
      out_.endObject();
    }
    end();
  }
  void load(const LoadReport &r)
  {
    count("records", uint64_t(r.records));
    count("missingType", uint64_t(r.missingType));
    count("missingBlock", uint64_t(r.missingBlock));
    list("blocks", r.blocks);
    issues(r.issues);
  }
  string take()
  {
    out_.endObject();
    return string(out_.take().c_str());
  }

 private:
  void begin(const char *key, int64_t total)
  {
    out_.key(key).beginObject();
    out_.key("total").integer(total);
    out_.key("first").beginArray();
  }
  void end()
  {
    out_.endArray().endObject();
  }
  void block(const BlockId &id, const char *name)
  {
    out_.key("block").string(formatBlockId(rt_, id));
    out_.key("name").string(name);
  }

  const Runtime &rt_;
  const Library &lib_;
  JsonWriter out_;
};

/* Of what the facades here have in common. */
Status canRun(Context &ctx, const char *what, std::initializer_list<const char *> slots = {})
{
  CDCC_TRY(detail::slotGate(ctx, what, slots));
  return ctx.library() ? Status() :
                         error(StatusCode::FailedPrecondition,
                               std::string(what) + ": the Context has no library");
}

/* LoadOptions or AppendOptions. */
template <class Options>
Result<Options> options(const char *what,
                        const string &sourceName,
                        int64_t maxBytes,
                        int64_t maxBlocks)
{
  Options out;
  out.sourceName = sourceName;
  if (maxBytes < 0 || maxBlocks < 0 || maxBlocks > std::numeric_limits<uint32_t>::max()) {
    return error(StatusCode::InvalidArgument, std::string(what) + ": a limit is out of range");
  }
  out.limits.maxBytes = maxBytes ? uint64_t(maxBytes) : out.limits.maxBytes;
  out.limits.maxBlocks = maxBlocks ? uint32_t(maxBlocks) : out.limits.maxBlocks;
  return out;
}

} // namespace

SaveResult librarySave(Context &ctx)
{
  SaveResult out;
  out.status = canRun(ctx, "librarySave");
  if (!out.status.ok()) {
    return out;
  }
  SaveReport report;
  Result<Vector<uint8_t>> bytes = saveLibrary(*ctx.library(), &report);
  if (!bytes.ok()) {
    out.status = bytes.status();
    return out;
  }
  out.value = ScriptBytes(std::move(bytes).value());
  Report r(ctx.runtime(), *ctx.library());
  r.count("blocks", uint64_t(report.blocks));
  r.count("loadBytes", report.loadBytes);
  r.count("loadBlocks", uint64_t(report.loadBlocks));
  r.list("transient", report.transient);
  r.list("throughTransient", report.throughTransient);
  r.list("hostPinned", report.hostPinned);
  r.list("detached", report.detached);
  r.issues(report.issues);
  out.report = r.take();
  return out;
}

StringResult libraryLoad(Context &ctx,
                         std::span<const uint8_t> bytes,
                         const string &sourceName,
                         int64_t maxBytes,
                         int64_t maxBlocks)
{
  if (Status s = canRun(ctx, "libraryLoad"); !s.ok()) {
    return failed<StringResult>(std::move(s));
  }
  Result<LoadOptions> opts =
      options<LoadOptions>("libraryLoad", sourceName, maxBytes, maxBlocks);
  if (!opts.ok()) {
    return failed<StringResult>(opts.status());
  }
  Result<LoadReport> r = loadLibrary(ctx, bytes, *opts);
  if (!r.ok()) {
    return failed<StringResult>(r.status());
  }
  Report out(ctx.runtime(), *ctx.library());
  out.load(*r);
  return ok<StringResult>(out.take());
}

StringResult libraryAppend(Context &ctx,
                           std::span<const uint8_t> bytes,
                           const string &sourceName,
                           int64_t maxBytes,
                           int64_t maxBlocks)
{
  if (Status s = canRun(ctx, "libraryAppend", {"exec"}); !s.ok()) {
    return failed<StringResult>(std::move(s));
  }
  Result<AppendOptions> opts =
      options<AppendOptions>("libraryAppend", sourceName, maxBytes, maxBlocks);
  if (!opts.ok()) {
    return failed<StringResult>(opts.status());
  }
  Result<AppendResult> r = appendUndo(ctx, bytes, *opts);
  if (!r.ok()) {
    return failed<StringResult>(r.status());
  }
  Report out(ctx.runtime(), *ctx.library());
  out.ids(r->ids);
  out.load(r->report);
  return ok<StringResult>(out.take());
}

StringResult libraryResolve(Context &ctx)
{
  if (Status s = canRun(ctx, "libraryResolve"); !s.ok()) {
    return failed<StringResult>(std::move(s));
  }
  const UndoHistory *history = ctx.executor() ? ctx.executor()->history() : nullptr;
  if (history && (history->canUndo() || history->canRedo())) {
    return failed<StringResult>(error(StatusCode::FailedPrecondition,
                                      "libraryResolve: clear the history first"));
  }
  Result<LoadReport> r = resolvePlaceholders(ctx);
  if (!r.ok()) {
    return failed<StringResult>(r.status());
  }
  Report out(ctx.runtime(), *ctx.library());
  out.load(*r);
  return ok<StringResult>(out.take());
}

Status clearHistory(Context &ctx)
{
  CDCC_TRY(detail::slotGate(ctx, "clearHistory", {}));
  CommandExecutor *ex = ctx.executor();
  if (!ex || !ex->history()) {
    return error(StatusCode::FailedPrecondition, "clearHistory: there is no history");
  }
  if (ex->running()) {
    return error(StatusCode::Busy, "clearHistory: a command is running");
  }
  ex->history()->clear();
  Runtime &rt = ctx.runtime(); /* a script's discard may have thrown */
  return rt.hasStickyError() ? rt.takeStickyError() : Status();
}

} // namespace coredcc::script
