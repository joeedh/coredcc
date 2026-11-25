#include "coredcc/api/library_io.h"

#include "coredcc/api/change_bus.h"
#include "coredcc/lib/io/load.h"
#include "coredcc/lib/library.h"

namespace coredcc {

namespace {

Status noLibrary(const char *what)
{
  return Status::error(StatusCode::FailedPrecondition,
                       (std::string(what) + ": the Context has no Library").c_str());
}

void notifyBlocks(Context &ctx, const LoadReport &report)
{
  ChangeBus &bus = ctx.runtime().ext<ChangeBus>();
  for (BlockHandle h : report.blocks) {
    if (const Datablock *b = ctx.library()->get(h)) {
      bus.notify(OwnerKey::ofBlock(b->id()));
    }
  }
}

Result<LoadReport> notified(Context &ctx, Result<LoadReport> report)
{
  if (report.ok()) {
    notifyBlocks(ctx, *report);
  }
  return report;
}

} // namespace

Result<LoadReport>
loadLibrary(Context &ctx, std::span<const uint8_t> bytes, const LoadOptions &options)
{
  if (!ctx.library()) {
    return noLibrary("loadLibrary");
  }
  return notified(ctx, detail::loadLibrary(*ctx.library(), bytes, options));
}

Result<AppendResult>
appendLibrary(Context &ctx, std::span<const uint8_t> bytes, const AppendOptions &options)
{
  if (!ctx.library()) {
    return noLibrary("appendLibrary");
  }
  Result<AppendResult> r = detail::appendLibrary(*ctx.library(), bytes, options);
  if (r.ok()) {
    notifyBlocks(ctx, r->report);
  }
  return r;
}

Result<LoadReport> resolvePlaceholders(Context &ctx)
{
  if (!ctx.library()) {
    return noLibrary("resolvePlaceholders");
  }
  return notified(ctx, detail::resolvePlaceholders(*ctx.library()));
}

} // namespace coredcc
