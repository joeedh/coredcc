#include "coredcc/lib/io/load.h"
#include "coredcc/lib/io/fill.h"
#include "coredcc/lib/io/parse.h"
#include "coredcc/lib/type_names.h"

#include <algorithm>
#include <string>
#include <utility>

namespace coredcc::detail {

using litestl::util::string;
using litestl::util::Vector;
using namespace libio;

namespace {

string own(std::string_view s)
{
  return string(std::string(s).c_str());
}

/* Renamed types are read by their current names (TypeAliases), so that is what the
 * library keeps and saves. */
void canonicalizeTypes(const Runtime &rt, ParsedFile &file)
{
  for (ParsedRecord &r : file.records) {
    r.type = canonicalTypeName(rt, TypeNameKind::Block, r.type);
    r.typeId = typeNamed(r.type);
  }
  for (ParsedAttachment &a : file.attachments) {
    a.type = canonicalTypeName(rt, TypeNameKind::Attachment, a.type);
    a.typeId = typeNamed(a.type);
  }
  for (ParsedSlot &s : file.slots) {
    if (!s.nsName.empty()) {
      s.nsName = canonicalTypeName(rt, TypeNameKind::Slot, s.nsName);
      s.ns = typeNamed(s.nsName);
    }
  }
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

PlaceholderReason reasonOf(const ParsedRecord &r)
{
  return r.storedAs == StoredAs::NoPayload ? PlaceholderReason::MissingBlock
                                           : PlaceholderReason::MissingType;
}

/* A load and an append. */
struct FillOptions {
  const LoadLimits &limits;
  const string &source;
  /* Records may fill MissingBlocks. */
  bool refill = false;
};

Status addRecord(LibraryFiller &filler,
                 ParsedFile &file,
                 const ParsedRecord &r,
                 bool refill,
                 LoadReport &report)
{
  PlaceholderInfo info;
  info.reason = reasonOf(r);
  info.typeName = own(r.type);
  info.name = own(r.name);
  info.sourceName = own(r.source);
  info.payload = payloadOf(file, r.version, r.blob, r.rows);
  Result<BlockHandle> h =
      filler.addRecord(r.id, std::move(info), flagsOf(r.flags), refill);
  if (!h.ok()) {
    return h.status();
  }
  filler.setExtension(*h, r.extension);
  for (uint32_t i = 0; i < r.slots.count; i++) {
    const ParsedSlot &s = file.slots[int(r.slots.begin + i)];
    filler.addSlot(*h, s.ns, s.key, s.cell);
    if (s.cell.kind == RefKind::Strong && s.cell.isNull()) {
      report.issues.add(r.id,
                        r.name,
                        Status::error(StatusCode::InvalidArgument,
                                      "a Strong slot that references nothing"));
    }
  }
  for (uint32_t i = 0; i < r.attachments.count; i++) {
    const ParsedAttachment &a = file.attachments[int(r.attachments.begin + i)];
    OpaqueAttachment o;
    o.typeName = own(a.type);
    o.payload = payloadOf(file, a.version, a.blob, a.rows);
    if (!filler.addOpaque(*h, std::move(o))) {
      report.issues.add(r.id,
                        r.name,
                        Status::error(StatusCode::AlreadyExists,
                                      ("the missing block it fills has an attachment '" +
                                       std::string(a.type) + "': the file's is dropped")
                                          .c_str()));
    }
  }
  report.blocks.append(*h);
  return Status();
}

void countPlaceholders(const Library &lib, LoadReport &report)
{
  for (BlockHandle h : report.blocks) {
    const Datablock *b = lib.get(h);
    if (b && b->state() == BlockState::Placeholder) {
      (b->placeholderReason() == PlaceholderReason::MissingType ? report.missingType
                                                                : report.missingBlock)++;
    }
  }
}

/* Every id is planned: from here on the load works. */
void fillFile(Library &lib,
              ParsedFile &file,
              const FillOptions &options,
              LoadReport &report)
{
  const uint64_t maxBlocks = uint64_t(options.limits.maxBlocks) + uint64_t(lib.size());
  LibraryFiller filler(
      lib, report, file.budget.left, uint32_t(std::min<uint64_t>(maxBlocks, UINT32_MAX)));
  for (const ParsedRecord &r : file.records) {
    if (Status s = addRecord(filler, file, r, options.refill, report); !s.ok()) {
      /* The plan gave every record its own id, so this doesn't happen. */
      report.issues.add(r.id, r.name, std::move(s));
    }
  }
  report.records = int64_t(report.blocks.size());
  for (int i = 0; i < int(report.records); i++) {
    filler.setSource(report.blocks[i],
                     options.source); /* cleared again when it is filled */
  }
  {
    LibraryFiller::Scope scope(lib);
    const int records = int(report.records);
    for (int i = 0; i < records; i++) {
      filler.fill(report.blocks[i]);
    }
    for (int i = 0; i < records; i++) {
      filler.fillAttachments(report.blocks[i]);
    }
    for (int i = 0; i < records; i++) {
      filler.noteMissing(report.blocks[i]);
    }
  }
  filler.makeMissing(options.source);
  report.missingBlock = 0;
  countPlaceholders(lib, report);
}

BlockId
rekeyRecord(Library &lib, const ParsedRecord &r, BlockId id, const RekeyContext &context)
{
  PlaceholderInfo info;
  info.reason = reasonOf(r);
  info.typeName = own(r.type);
  info.name = own(r.name);
  info.sourceName = own(r.source);
  return LibraryFiller::rekey(lib, id, info, context);
}

} // namespace

Result<LoadReport>
loadLibrary(Library &lib, std::span<const uint8_t> bytes, const LoadOptions &options)
{
  CDCC_TRY(LibraryFiller::ready(lib, "loadLibrary"));
  if (lib.size() > 0) {
    return Status::error(StatusCode::FailedPrecondition,
                         "loadLibrary: the library already has records");
  }
  LoadReport report;
  Result<std::unique_ptr<ParsedFile>> file =
      parseLibrary(bytes, options.limits, report.issues);
  if (!file.ok()) {
    return file.status();
  }
  canonicalizeTypes(lib.runtime(), **file);
  const auto rekey =
      [&lib](const ParsedRecord &r, BlockId id, const RekeyContext &context) {
        return rekeyRecord(lib, r, id, context);
      };
  CDCC_TRY(planIds(**file, lib, options.limits, rekey, report.issues));
  fillFile(lib, **file, FillOptions{options.limits, options.sourceName}, report);
  return report;
}

Result<AppendResult>
appendLibrary(Library &lib, std::span<const uint8_t> bytes, const AppendOptions &options)
{
  lib.find(BlockId()); /* takes in client aliases that are new */
  CDCC_TRY(LibraryFiller::ready(lib, "appendLibrary"));
  AppendResult result;
  Result<std::unique_ptr<ParsedFile>> file =
      parseLibrary(bytes, options.limits, result.report.issues);
  if (!file.ok()) {
    return file.status();
  }
  canonicalizeTypes(lib.runtime(), **file);
  const auto rekey =
      [&lib](const ParsedRecord &r, BlockId id, const RekeyContext &context) {
        return rekeyRecord(lib, r, id, context);
      };
  CDCC_TRY(
      planAppend(**file, lib, options.limits, rekey, result.ids, result.report.issues));
  for (ParsedRecord &r : (*file)->records) {
    /* What was kept for itself there is kept here, but this document's roots are its own.
     */
    r.flags = r.flags & kFlagRoot ? (r.flags & ~kFlagRoot) | kFlagFakeUser : r.flags;
  }
  fillFile(
      lib, **file, FillOptions{options.limits, options.sourceName, true}, result.report);
  return result;
}

Result<LoadReport> resolvePlaceholders(Library &lib)
{
  lib.find(BlockId()); /* takes in client aliases that are new */
  CDCC_TRY(LibraryFiller::ready(lib, "resolvePlaceholders"));
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
