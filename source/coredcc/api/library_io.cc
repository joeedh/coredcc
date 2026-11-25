#include "coredcc/api/library_io.h"

#include "coredcc/api/change_bus.h"
#include "coredcc/lib/library.h"

namespace coredcc {

namespace {

Status noLibrary(const char *what)
{
  return Status::error(StatusCode::FailedPrecondition,
                       (std::string(what) + ": the Context has no Library").c_str());
}

Result<LoadReport> notifyAll(Context &ctx, Result<LoadReport> report)
{
  if (report.ok()) {
    ChangeBus &bus = ctx.runtime().ext<ChangeBus>();
    for (BlockHandle h : report->blocks) {
      if (const Datablock *b = ctx.library()->get(h)) {
        bus.notify(OwnerKey::ofBlock(b->id()));
      }
    }
  }
  return report;
}

} // namespace

Result<LoadReport> loadLibrary(Context &ctx,
                               std::span<const uint8_t> bytes,
                               const LoadOptions &options)
{
  if (!ctx.library()) {
    return noLibrary("loadLibrary");
  }
  return notifyAll(ctx, detail::loadLibrary(*ctx.library(), bytes, options));
}

Result<LoadReport> resolvePlaceholders(Context &ctx)
{
  if (!ctx.library()) {
    return noLibrary("resolvePlaceholders");
  }
  return notifyAll(ctx, detail::resolvePlaceholders(*ctx.library()));
}

} // namespace coredcc
