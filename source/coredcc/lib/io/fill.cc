#include "coredcc/lib/io/fill.h"

#include "coredcc/lib/io/format.h"
#include "coredcc/lib/payload_access.h"
#include "coredcc/lib/type_names.h"

#include <cstring>
#include <string>
#include <utility>

namespace coredcc::detail {

using litestl::util::string;
using litestl::util::Vector;

namespace {

void sever(RefCell &cell)
{
  cell.target = BlockId();
  cell.cache = BlockHandle();
}

/* What is wrong with a deserialize that came back as `s`. */
Status readOutcome(Runtime &rt, Status s)
{
  if (rt.hasStickyError()) {
    return rt.takeStickyError(); /* a foreign deserialize threw */
  }
  return s;
}

/* The checks on a deserialize that returned ok. */
Status readComplete(const BytesReader &bytes, const void *data)
{
  if (!bytes.ok()) {
    return bytes.status();
  }
  if (!bytes.atEnd()) {
    return Status::error(
        StatusCode::InvalidArgument,
        (std::to_string(bytes.remaining()) + " bytes of the payload were left over")
            .c_str());
  }
  return data ? Status()
              : Status::error(StatusCode::Internal,
                              "deserialize succeeded without calling setData");
}

Status newer(const char *what)
{
  return Status::error(
      StatusCode::Unsupported,
      (std::string(what) + " was written by a newer version of its type").c_str());
}

} // namespace

/* ---- the scope ---- */

LibraryFiller::Scope::Scope(const Library &lib)
    : lib_(const_cast<Library &>(lib)), before_(lib.loading_)
{
  lib_.loading_ = true;
}

LibraryFiller::Scope::~Scope()
{
  lib_.loading_ = before_;
  if (!before_) {
    lib_.runDeferred();
    lib_.releaseDoomed();
  }
}

/* ---- records ---- */

Result<BlockHandle>
LibraryFiller::addRecord(BlockId id, PlaceholderInfo info, BlockFlags flags, bool refill)
{
  if (const BlockHandle h = refill ? lib_.find(id) : BlockHandle()) {
    Datablock &b = *lib_.rec(h);
    lib_.keepMissing(b);
    b.type_ = typeNamed(info.typeName.c_str());
    b.name_ = std::move(info.name);
    b.placeholderReason_ = info.reason;
    b.placeholderType_ = std::move(info.typeName);
    b.sourceStored_ = info.sourceName.size() > 0;
    b.sourceName_ = std::move(info.sourceName);
    b.placeholderData_ = std::move(info.payload);
    b.flags_ = b.flags_ | flags;
    lib_.setState(b, BlockState::Placeholder);
    return h;
  }
  Result<BlockHandle> h = lib_.addPlaceholder(id, std::move(info));
  if (h.ok()) {
    lib_.rec(*h)->flags_ = flags;
  }
  return h;
}

void LibraryFiller::addSlot(BlockHandle h, TypeId ns, int64_t key, const RefCell &cell)
{
  Datablock &b = *lib_.rec(h);
  if (b.adoption_) { /* a refilled one may have the slot */
    for (Datablock::Slot &s : b.slots_) {
      if (s.ns == ns && s.key == key) {
        s.cell = cell;
        return;
      }
    }
  }
  b.slots_.append(Datablock::Slot{ns, key, cell});
}

bool LibraryFiller::addOpaque(BlockHandle h, OpaqueAttachment attachment)
{
  Datablock &b = *lib_.rec(h);
  if (b.adoption_ && lib_.hasAttachment(h, typeNamed(attachment.typeName.c_str()))) {
    return false;
  }
  b.addOpaque(std::move(attachment));
  return true;
}

void LibraryFiller::addAttachment(BlockHandle h, TypeId type, void *data)
{
  lib_.rec(h)->attachments_.append(Datablock::Attachment{type, data});
  lib_.epoch_++;
}

Status LibraryFiller::ready(const Library &lib, const char *what)
{
  if (lib.busy()) {
    return Status::error(StatusCode::Busy,
                         (std::string(what) + ": the library is busy").c_str());
  }
  CDCC_TRY(lib.runtime().busyIfEvaluating(what));
  if (lib.runtime().hasStickyError()) {
    return Status::error(
        StatusCode::FailedPrecondition,
        (std::string(what) + ": a sticky error is pending; take it first").c_str());
  }
  return Status();
}

void LibraryFiller::destroy(const Vector<BlockHandle> &blocks)
{
  lib_.destroyBlocks(blocks);
}

BlockId LibraryFiller::rekey(Library &lib,
                             BlockId id,
                             const PlaceholderInfo &info,
                             const RekeyContext &context)
{
  const TypeId type = typeNamed(info.typeName.c_str());
  const BlockProvider p = lib.types_.provider(type);
  if (!p || !p.hasRekey()) {
    return BlockId();
  }
  Datablock b;
  b.lib_ = &lib;
  b.id_ = id;
  b.type_ = type;
  b.name_ = info.name;
  b.state_ = BlockState::Placeholder;
  b.placeholderReason_ = info.reason;
  b.placeholderType_ = info.typeName;
  b.sourceName_ = info.sourceName;
  Scope scope(lib);
  return p.rekey(b, context);
}

void LibraryFiller::setExtension(BlockHandle h, std::span<const uint8_t> bytes)
{
  if (bytes.empty()) {
    return;
  }
  auto kept = std::make_unique<Vector<uint8_t>>();
  kept->resize<false>(bytes.size());
  memcpy(kept->data(), bytes.data(), bytes.size());
  lib_.rec(h)->fileExtension_ = std::move(kept);
}

void LibraryFiller::setSource(BlockHandle h, const string &source)
{
  Datablock &b = *lib_.rec(h);
  if (b.sourceName_.size() == 0) {
    b.sourceName_ = source;
  }
}

/* ---- data ---- */

Status LibraryFiller::setData(BlockHandle h, void *data, Status s)
{
  Datablock &b = *lib_.rec(h);
  lib_.setState(b, BlockState::Live);
  if (data) {
    lib_.adoptData(h, data, s.ok());
  }
  if (s.ok()) {
    b.placeholderType_ = string();
    b.sourceName_ = string();
    b.sourceStored_ = false;
  } else {
    lib_.setState(b, BlockState::Placeholder);
    lib_.rt_.takeStickyError().ignore(); /* `s` says what went wrong first */
  }
  return s;
}

Status LibraryFiller::read(Library &lib,
                           BlockHandle h,
                           const BlockTypeRegistry::Entry &type,
                           const BlockPayload &payload,
                           uint64_t *budget)
{
  BlockReader in(lib, payload);
  PayloadAccess::setBudget(in, budget);
  lib.beginNoCollect(); /* nothing keeps a half-made block */
  BlockProvider provider = type.provider.get();
  Status s = readOutcome(lib.runtime(), provider.deserialize(*lib.get(h), in));
  lib.endNoCollect();
  if (!s.ok()) {
    return s; /* the provider freed what it made */
  }
  void *data = PayloadAccess::data(in);
  s = readComplete(in.bytes(), data);
  if (data) {
    lib.adoptData(h, data, s.ok());
  }
  return s;
}

/* Why `b` can't be read, or ok. */
Status LibraryFiller::why(const Datablock &b) const
{
  const std::string type = "'" + std::string(b.placeholderType().c_str()) + "'";
  const BlockTypeRegistry::Entry *e = lib_.types_.find(b.type());
  if (!e) {
    return Status::error(StatusCode::NotFound,
                         ("no block type " + type + " is registered").c_str());
  }
  if (e->info.client != b.id().client) {
    return Status::error(StatusCode::TypeMismatch,
                         ("its type " + type + " belongs to another client").c_str());
  }
  if (e->info.storage != BlockStorage::Payload) {
    return Status::error(StatusCode::Unsupported,
                         ("its type " + type + " isn't read from files").c_str());
  }
  if (libio::hasCriticalField(b.fileExtension())) {
    return Status::error(StatusCode::Unsupported, "it needs a newer build");
  }
  return b.placeholderData().version > e->info.payloadVersion ? newer("it") : Status();
}

bool LibraryFiller::fill(BlockHandle h)
{
  Datablock &b = *lib_.rec(h);
  if (b.state_ != BlockState::Placeholder ||
      b.placeholderReason_ != PlaceholderReason::MissingType)
  {
    return true;
  }
  Status s = why(b);
  if (s.ok()) {
    BlockPayload payload = std::exchange(b.placeholderData_, BlockPayload());
    lib_.setState(b, BlockState::Live);
    s = read(lib_, h, *lib_.types_.find(b.type_), payload, &budget_);
    if (s.ok()) {
      b.placeholderType_ = string();
      b.sourceName_ = string();
      b.sourceStored_ = false;
      return true;
    }
    b.placeholderData_ = std::move(payload);
    lib_.setState(b, BlockState::Placeholder);
  }
  report_.issues.add(b.id_, b.name_.c_str(), std::move(s));
  return false;
}

Status LibraryFiller::readAttachment(Datablock &b, const OpaqueAttachment &from)
{
  const TypeId type = typeNamed(from.typeName.c_str());
  const AttachmentTypeRegistry::Entry *e = lib_.attachmentTypes_.find(type);
  const std::string what = "attachment '" + std::string(from.typeName.c_str()) + "'";
  if (!e) {
    return Status::error(StatusCode::NotFound,
                         (what + ": the type isn't registered").c_str());
  }
  AttachmentProvider p = e->provider.get();
  if (!p.hasDeserialize()) {
    return Status::error(StatusCode::Unsupported,
                         (what + ": the type isn't read from files").c_str());
  }
  if (from.payload.version > e->info.payloadVersion) {
    return newer(what.c_str());
  }
  BlockReader in(lib_, from.payload);
  PayloadAccess::setBudget(in, &budget_);
  Status s = readOutcome(lib_.runtime(), p.deserialize(b, in));
  if (s.ok()) {
    void *data = PayloadAccess::data(in);
    s = readComplete(in.bytes(), data);
    if (s.ok()) {
      b.attachments_.append(Datablock::Attachment{type, data});
      lib_.epoch_++;
      return s;
    }
    if (data) {
      if (p.hasForeachRef()) {
        RefVisitor v([](void *, RefCell &cell) { sever(cell); }, nullptr);
        p.forEachRef(b, data, v);
      }
      p.destroy(data);
      lib_.runtime().takeStickyError().ignore(); /* `s` says what went wrong first */
    }
  }
  return Status::error(s.code(), (what + ": " + s.message()).c_str());
}

int LibraryFiller::fillAttachments(BlockHandle h)
{
  Datablock &b = *lib_.rec(h);
  if (!b.opaque_) {
    return 0;
  }
  int filled = 0;
  Vector<OpaqueAttachment, 1> kept;
  for (OpaqueAttachment &o : *b.opaque_) {
    Status s = readAttachment(b, o);
    if (s.ok()) {
      filled++;
      continue;
    }
    report_.issues.add(b.id_, b.name_.c_str(), std::move(s));
    kept.append(std::move(o));
  }
  if (kept.size() > 0) {
    *b.opaque_ = std::move(kept);
  } else {
    b.opaque_.reset();
  }
  return filled;
}

/* ---- blocks that aren't there ---- */

void LibraryFiller::noteMissing(BlockHandle h)
{
  Status s = lib_.forEachRef(h, [this](RefCell &cell) {
    if (isStrong(cell.kind) && !cell.isNull() && !lib_.find(cell.target)) {
      missing_.append(cell.target);
    }
  });
  if (!s.ok()) {
    const Datablock &b = *lib_.rec(h);
    report_.issues.add(b.id_, b.name_.c_str(), lib_.rt_.takeStickyError());
  }
}

void LibraryFiller::makeMissing(const string &source)
{
  for (const BlockId &id : missing_) {
    if (lib_.find(id)) {
      continue;
    }
    if (uint32_t(lib_.size()) >= maxBlocks_) {
      report_.issues.add(
          id,
          {},
          Status::error(StatusCode::OutOfRange,
                        "too many missing blocks: this one has no record"));
      continue;
    }
    PlaceholderInfo info;
    info.reason = PlaceholderReason::MissingBlock;
    Result<BlockHandle> h = lib_.addPlaceholder(id, std::move(info));
    if (h.ok()) {
      setSource(*h, source);
      report_.blocks.append(*h);
      report_.missingBlock++;
    }
  }
  missing_.clear();
}

} // namespace coredcc::detail
