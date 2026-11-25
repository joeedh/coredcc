#include "coredcc/lib/io/fill.h"
#include "coredcc/lib/io/parse.h"

#include <string>
#include <utility>

namespace coredcc::detail {

using litestl::util::string;
using litestl::util::Vector;
using namespace libio;

namespace {

Status readyToFill(const Library &lib, const char *what)
{
  if (lib.busy()) {
    return Status::error(StatusCode::Busy, (std::string(what) + ": the library is busy").c_str());
  }
  CDCC_TRY(lib.runtime().busyIfEvaluating(what));
  if (lib.runtime().hasStickyError()) {
    return Status::error(
        StatusCode::FailedPrecondition,
        (std::string(what) + ": a sticky error is pending; take it first").c_str());
  }
  return Status();
}

string own(std::string_view s)
{
  return string(std::string(s).c_str());
}

/* Takes the blob out of the file. */
BlockPayload payloadOf(ParsedFile &file, uint32_t version, uint32_t blob, Range rows)
{
  BlockPayload p;
  p.version = version;
  if (blob) {
    p.bytes = std::move(file.blobs[int(blob - 1)]);
  }
  p.refs.ensure_capacity(rows.count);
  for (uint32_t i = 0; i < rows.count; i++) {
    p.refs.append(file.rows[int(rows.begin + i)]);
  }
  return p;
}

BlockFlags flagsOf(uint32_t stored)
{
  BlockFlags f = BlockFlags::None;
  if (stored & kFlagRoot) {
    f = f | BlockFlags::Root;
  }
  if (stored & kFlagFakeUser) {
    f = f | BlockFlags::FakeUser;
  }
  return f;
}

Status addRecord(LibraryFiller &filler, ParsedFile &file, const ParsedRecord &r, LoadReport &report)
{
  PlaceholderInfo info;
  info.reason = r.storedAs == StoredAs::NoPayload ? PlaceholderReason::MissingBlock :
                                                    PlaceholderReason::MissingType;
  info.typeName = own(r.type);
  info.name = own(r.name);
  info.sourceName = own(r.source);
  info.payload = payloadOf(file, r.version, r.blob, r.rows);
  Result<BlockHandle> h = filler.addRecord(r.id, std::move(info), flagsOf(r.flags));
  if (!h.ok()) {
    return h.status();
  }
  for (uint32_t i = 0; i < r.slots.count; i++) {
    const ParsedSlot &s = file.slots[int(r.slots.begin + i)];
    filler.addSlot(*h, s.ns, s.key, s.cell);
    if (s.cell.kind == RefKind::Strong && s.cell.isNull()) {
      report.issues.add(r.id, r.name,
                        Status::error(StatusCode::InvalidArgument,
                                      "a Strong slot that references nothing"));
    }
  }
  for (uint32_t i = 0; i < r.attachments.count; i++) {
    const ParsedAttachment &a = file.attachments[int(r.attachments.begin + i)];
    OpaqueAttachment o;
    o.typeName = own(a.type);
    o.payload = payloadOf(file, a.version, a.blob, a.rows);
    filler.addOpaque(*h, std::move(o));
  }
  report.blocks.append(*h);
  return Status();
}

void countPlaceholders(const Library &lib, LoadReport &report)
{
  for (BlockHandle h : report.blocks) {
    const Datablock *b = lib.get(h);
    if (b && b->state() == BlockState::Placeholder) {
      (b->placeholderReason() == PlaceholderReason::MissingType ? report.missingType :
                                                                  report.missingBlock)++;
    }
  }
}

} // namespace

Result<LoadReport> loadLibrary(Library &lib,
                               std::span<const uint8_t> bytes,
                               const LoadOptions &options)
{
  CDCC_TRY(readyToFill(lib, "loadLibrary"));
  if (lib.size() > 0) {
    return Status::error(StatusCode::FailedPrecondition,
                         "loadLibrary: the library already has records");
  }
  LoadReport report;
  Result<std::unique_ptr<ParsedFile>> parsed = parseLibrary(bytes, options.limits, report.issues);
  if (!parsed.ok()) {
    return parsed.status();
  }
  ParsedFile &file = **parsed;
  CDCC_TRY(planIds(file, lib, options.limits, report.issues));

  LibraryFiller filler(lib, report, file.budget.left, options.limits.maxBlocks);
  for (const ParsedRecord &r : file.records) {
    if (Status s = addRecord(filler, file, r, report); !s.ok()) {
      /* planIds gave every record its own id, so this doesn't happen. */
      report.issues.add(r.id, r.name, std::move(s));
    }
  }
  {
    LibraryFiller::Scope scope(lib);
    const int records = int(report.blocks.size());
    for (int i = 0; i < records; i++) {
      if (!filler.fill(report.blocks[i])) {
        filler.setSource(report.blocks[i], options.sourceName);
      }
    }
    for (int i = 0; i < records; i++) {
      filler.fillAttachments(report.blocks[i]);
    }
    for (int i = 0; i < records; i++) {
      filler.noteMissing(report.blocks[i]);
    }
  }
  filler.makeMissing();
  report.missingBlock = 0;
  countPlaceholders(lib, report);
  return report;
}

Result<LoadReport> resolvePlaceholders(Library &lib)
{
  lib.find(BlockId()); /* takes in client aliases that are new */
  CDCC_TRY(readyToFill(lib, "resolvePlaceholders"));
  LoadReport report;
  const LoadLimits limits;
  LibraryFiller filler(lib, report, limits.maxBytes, UINT32_MAX);
  Vector<BlockHandle> candidates;
  lib.forEach([&](BlockHandle h, const Datablock &b) {
    bool any = b.state() == BlockState::Placeholder &&
               b.placeholderReason() == PlaceholderReason::MissingType;
    lib.forEachOpaqueAttachment(h, [&](const OpaqueAttachment &) { any = true; });
    if (any) {
      candidates.append(h);
    }
  });
  {
    LibraryFiller::Scope scope(lib);
    Vector<bool> changed;
    for (BlockHandle h : candidates) {
      const bool wasPlaceholder = lib.get(h)->state() == BlockState::Placeholder;
      const bool filled = filler.fill(h);
      changed.append(wasPlaceholder && filled && lib.get(h)->state() == BlockState::Live);
    }
    for (int i = 0; i < int(candidates.size()); i++) {
      const bool attached = filler.fillAttachments(candidates[i]) > 0;
      if (changed[i] || attached) {
        report.blocks.append(candidates[i]);
        filler.noteMissing(candidates[i]);
      }
      const Datablock &b = *lib.get(candidates[i]);
      if (b.state() == BlockState::Placeholder &&
          b.placeholderReason() == PlaceholderReason::MissingType)
      {
        report.missingType++;
      }
    }
  }
  filler.makeMissing();
  return report;
}

} // namespace coredcc::detail
