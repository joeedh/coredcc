#include "coredcc/lib/io/fill.h"

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
        (std::to_string(bytes.remaining()) + " bytes of the payload were left over").c_str());
  }
  return data ? Status() :
                Status::error(StatusCode::Internal, "deserialize succeeded without calling setData");
}

Status newer(const char *what)
{
  return Status::error(StatusCode::Unsupported,
                       (std::string(what) + " was written by a newer version of its type").c_str());
}

} // namespace

/* ---- the scope ---- */

LibraryFiller::Scope::Scope(Library &lib) : lib_(lib)
{
  lib.loading_ = true;
}

LibraryFiller::Scope::~Scope()
{
  lib_.loading_ = false;
  lib_.runDeferred();
  lib_.releaseDoomed();
}

/* ---- records ---- */

Result<BlockHandle> LibraryFiller::addRecord(BlockId id, PlaceholderInfo info, BlockFlags flags)
{
  Result<BlockHandle> h = lib_.addPlaceholder(id, std::move(info));
  if (h.ok()) {
    lib_.rec(*h)->flags_ = flags;
  }
  return h;
}

void LibraryFiller::addSlot(BlockHandle h, TypeId ns, int64_t key, const RefCell &cell)
{
  lib_.rec(h)->slots_.append(Datablock::Slot{ns, key, cell});
}

void LibraryFiller::addOpaque(BlockHandle h, OpaqueAttachment attachment)
{
  Datablock &b = *lib_.rec(h);
  if (!b.opaque_) {
    b.opaque_ = std::make_unique<Vector<OpaqueAttachment, 1>>();
  }
  b.opaque_->append(std::move(attachment));
}

void LibraryFiller::addAttachment(BlockHandle h, TypeId type, void *data)
{
  lib_.rec(h)->attachments_.append(Datablock::Attachment{type, data});
  lib_.epoch_++;
}

Status LibraryFiller::ready(const Library &lib, const char *what)
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

void LibraryFiller::destroy(const Vector<BlockHandle> &blocks)
{
  lib_.destroyBlocks(blocks);
}

BlockId LibraryFiller::rekey(Library &lib, BlockId id, const PlaceholderInfo &info)
{
  const TypeId type = info.typeName.size() > 0 ? TypeId::of(info.typeName.c_str()) : TypeId();
  const BlockProvider p = lib.types_.provider(type);
  if (!p || !p.hasRekey()) {
    return BlockId();
  }
  Datablock b;
  b.id_ = id;
  b.type_ = type;
  b.name_ = info.name;
  b.state_ = BlockState::Placeholder;
  b.placeholderReason_ = info.reason;
  b.placeholderType_ = info.typeName;
  b.sourceName_ = info.sourceName;
  Scope scope(lib);
  return p.rekey(b);
}

void LibraryFiller::setSource(BlockHandle h, const string &source)
{
  Datablock &b = *lib_.rec(h);
  if (b.sourceName_.size() == 0) {
    b.sourceName_ = source;
  }
}

/* ---- data ---- */

Status LibraryFiller::read(Library &lib,
                           BlockHandle h,
                           const BlockTypeRegistry::Entry &type,
                           const BlockPayload &payload,
                           uint64_t *budget)
{
  BlockReader in(lib, payload);
  in.setBudget(budget);
  lib.beginNoCollect(); /* nothing keeps a half-made block */
  BlockProvider provider = type.provider.get();
  Status s = readOutcome(lib.runtime(), provider.deserialize(*lib.get(h), in));
  lib.endNoCollect();
  if (!s.ok()) {
    return s; /* the provider freed what it made */
  }
  s = readComplete(in.bytes(), in.data());
  if (in.data()) {
    lib.adoptData(h, in.data(), s.ok());
  }
  return s;
}

/* Why `b` can't be read, or ok. */
Status LibraryFiller::why(const Datablock &b) const
{
  const std::string type = "'" + std::string(b.placeholderType().c_str()) + "'";
  const BlockTypeRegistry::Entry *e = lib_.types_.find(b.type());
  if (!e) {
    return Status::error(StatusCode::NotFound, ("no block type " + type + " is registered").c_str());
  }
  if (e->info.client != b.id().client) {
    return Status::error(StatusCode::TypeMismatch,
                         ("its type " + type + " belongs to another client").c_str());
  }
  if (e->info.storage != BlockStorage::Payload) {
    return Status::error(StatusCode::Unsupported,
                         ("its type " + type + " isn't read from files").c_str());
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
  const TypeId type = TypeId::of(from.typeName.c_str());
  const AttachmentTypeRegistry::Entry *e = lib_.attachmentTypes_.find(type);
  const std::string what = "attachment '" + std::string(from.typeName.c_str()) + "'";
  if (!e) {
    return Status::error(StatusCode::NotFound, (what + ": the type isn't registered").c_str());
  }
  AttachmentProvider p = e->provider.get();
  if (!p.hasDeserialize()) {
    return Status::error(StatusCode::Unsupported, (what + ": the type isn't read from files").c_str());
  }
  if (from.payload.version > e->info.version) {
    return newer(what.c_str());
  }
  BlockReader in(lib_, from.payload);
  in.setBudget(&budget_);
  Status s = readOutcome(lib_.runtime(), p.deserialize(in));
  if (s.ok()) {
    s = readComplete(in.bytes(), in.data());
    if (s.ok()) {
      b.attachments_.append(Datablock::Attachment{type, in.data()});
      lib_.epoch_++;
      return s;
    }
    if (in.data()) {
      if (p.hasForeachRef()) {
        RefVisitor v([](void *, RefCell &cell) { sever(cell); }, nullptr);
        p.foreachRef(in.data(), v);
      }
      p.destroy(in.data());
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
  }
  else {
    b.opaque_.reset();
  }
  return filled;
}

/* ---- blocks that aren't there ---- */

void LibraryFiller::noteMissing(BlockHandle h)
{
  Status s = lib_.foreachRef(h, [this](RefCell &cell) {
    if (isStrong(cell.kind) && !cell.isNull() && !lib_.find(cell.target)) {
      missing_.append(cell.target);
    }
  });
  if (!s.ok()) {
    const Datablock &b = *lib_.rec(h);
    report_.issues.add(b.id_, b.name_.c_str(), lib_.rt_.takeStickyError());
  }
}

void LibraryFiller::makeMissing()
{
  for (const BlockId &id : missing_) {
    if (lib_.find(id)) {
      continue;
    }
    if (uint32_t(lib_.size()) >= maxBlocks_) {
      report_.issues.add(id, {},
                         Status::error(StatusCode::OutOfRange,
                                       "too many missing blocks: this one has no record"));
      continue;
    }
    PlaceholderInfo info;
    info.reason = PlaceholderReason::MissingBlock;
    Result<BlockHandle> h = lib_.addPlaceholder(id, std::move(info));
    if (h.ok()) {
      report_.blocks.append(*h);
      report_.missingBlock++;
    }
  }
  missing_.clear();
}

} // namespace coredcc::detail
