#include "coredcc/lib/block_io.h"

#include "coredcc/lib/io/fill.h"
#include "coredcc/lib/library.h"

#include <string>
#include <utility>

namespace coredcc {

using litestl::util::Map;
using litestl::util::Vector;

namespace {

/* A name, a tag, a ref count and an empty body. */
constexpr size_t kMinEntryBytes = 13;
constexpr size_t kRefIndexBytes = 4;

std::span<const uint8_t> view(const Vector<uint8_t> &v)
{
  return std::span<const uint8_t>(v.data(), size_t(v.size()));
}

bool isBlockPointer(const PropValue &v)
{
  const StableRef *ref = v.tryPointer();
  return ref && !ref->isNull() && ref->owner.kind == OwnerKey::Kind::Block;
}

/* Writing only looks names up, so a const Library's table is safe here. */
NameTable *tableOf(const Library &lib)
{
  return &const_cast<Library &>(lib).fileNames();
}

} // namespace

/* ---- entries ---- */

void detail::EntryCodec::writeBlockId(BytesWriter &out, const BlockId &)
{
  if (!entry || entry->refs.size() == 0) {
    out.fail(Status::error(StatusCode::Internal,
                           "a payload entry points into a block, but has no ref for it"));
  }
  out.u32(0); /* the entry's first ref */
}

void detail::EntryCodec::readBlockId(BytesReader &in, BlockId &id)
{
  const uint32_t i = in.u32();
  if (!entry || i >= uint32_t(entry->refs.size())) {
    in.fail(StatusCode::InvalidArgument, "a payload entry points at a ref it doesn't have");
    return;
  }
  id = entry->refs[int(i)].target;
}

/* ---- BlockWriter ---- */

BlockWriter::BlockWriter(const Library &lib) : codec_(lib.runtime(), tableOf(lib)) {}

void BlockWriter::ref(const RefCell &cell)
{
  bytes_.u32(uint32_t(refs_.size()));
  RefCell row(cell.kind);
  row.target = cell.target;
  refs_.append(row);
}

void BlockWriter::writeEntry(const BlockEntry &e)
{
  if (e.unknownTag != 0 && isKnownValueTag(e.unknownTag)) {
    bytes_.fail(Status::error(StatusCode::Internal,
                              "a payload entry with raw bytes has the tag of a known type"));
    return;
  }
  bytes_.str(e.name.c_str());
  bytes_.u8(e.unknownTag ? e.unknownTag : uint8_t(valueTag(e.value.type())));
  bytes_.u32(uint32_t(e.refs.size()));
  for (const RefCell &cell : e.refs) {
    ref(cell);
  }
  if (e.unknownTag) {
    bytes_.blob(view(e.raw));
    return;
  }
  const size_t body = bytes_.beginSection();
  codec_.entry = &e;
  writeValueBody(bytes_, e.value, codec_);
  codec_.entry = nullptr;
  bytes_.endSection(body);
}

void BlockWriter::entries(std::span<const BlockEntry> all)
{
  bytes_.u32(uint32_t(all.size()));
  for (const BlockEntry &e : all) {
    writeEntry(e);
  }
}

void BlockWriter::bag(const PropertyBag &bag,
                      RefKind pointerKind,
                      std::span<const BlockEntry> unread)
{
  uint32_t n = uint32_t(unread.size());
  for (int i = 0; i < bag.size(); i++) {
    n += bag.wasSet(i) ? 1 : 0;
  }
  bytes_.u32(n);
  /* Unread ones first: if the bag has since set the same name, that one wins on read. */
  for (const BlockEntry &e : unread) {
    writeEntry(e);
  }
  for (int i = 0; i < bag.size(); i++) {
    if (!bag.wasSet(i)) {
      continue;
    }
    BlockEntry e;
    e.name = bag.def(i).apiName();
    e.value = storedBagValue(bag, i);
    if (isBlockPointer(e.value)) {
      RefCell cell(pointerKind);
      cell.target = e.value.asPointer().owner.block;
      e.refs.append(cell);
    }
    writeEntry(e);
  }
}

Result<BlockPayload> BlockWriter::take(uint32_t version)
{
  if (!bytes_.status().ok()) {
    return bytes_.status();
  }
  BlockPayload p;
  p.version = version;
  p.bytes = bytes_.take();
  p.refs = std::move(refs_);
  refs_ = Vector<RefCell>();
  return p;
}

/* ---- BlockReader ---- */

BlockReader::BlockReader(Library &lib, const BlockPayload &payload)
    : lib_(lib), payload_(payload), bytes_(view(payload.bytes)),
      codec_(lib.runtime(), &lib.fileNames())
{
}

const RefCell *BlockReader::row()
{
  const uint32_t i = bytes_.u32();
  if (!bytes_.ok()) {
    return nullptr;
  }
  if (i >= uint32_t(payload_.refs.size())) {
    bytes_.fail(StatusCode::InvalidArgument, "an index outside the block's ref table");
    return nullptr;
  }
  return &payload_.refs[int(i)];
}

void BlockReader::ref(RefCell &cell)
{
  const RefCell *from = row();
  cell.target = from ? from->target : BlockId();
  cell.cache = BlockHandle();
}

uint32_t BlockReader::entryCount()
{
  const uint32_t n = bytes_.count(kMinEntryBytes);
  return charge(uint64_t(n) * sizeof(BlockEntry) * 4) ? n : 0;
}

/* What an entry allocates is a multiple of its bytes: hundreds for a small one. */
bool BlockReader::charge(uint64_t bytes)
{
  if (budget_ && bytes > *budget_) {
    bytes_.fail(StatusCode::OutOfRange, "the entries need more memory than allowed");
    return false;
  }
  if (budget_) {
    *budget_ -= bytes;
  }
  return bytes_.ok();
}

bool BlockReader::readEntry(BlockEntry &e)
{
  e.name = std::string(bytes_.str()).c_str();
  const uint8_t tag = bytes_.u8();
  const uint32_t refs = bytes_.count(kRefIndexBytes);
  if (!charge(uint64_t(refs) * sizeof(RefCell) * 4)) {
    return false;
  }
  for (uint32_t i = 0; i < refs; i++) {
    const RefCell *from = row();
    if (!from) {
      return false;
    }
    RefCell cell(from->kind);
    cell.target = from->target;
    e.refs.append(cell);
  }
  const std::span<const uint8_t> raw = bytes_.blob();
  if (!bytes_.ok()) {
    return false;
  }
  if (!isKnownValueTag(tag)) {
    e.unknownTag = tag;
    for (uint8_t b : raw) {
      e.raw.append(b);
    }
    return true;
  }
  BytesReader body(raw);
  codec_.entry = &e;
  e.value = readValueBody(body, ValueTag(tag), codec_);
  codec_.entry = nullptr;
  if (body.ok() && !body.atEnd()) {
    body.fail(StatusCode::InvalidArgument, "a value with bytes left over");
  }
  bytes_.fail(body.status());
  return bytes_.ok();
}

void BlockReader::entries(Vector<BlockEntry> &out)
{
  const uint32_t n = entryCount();
  for (uint32_t i = 0; i < n; i++) {
    BlockEntry e;
    if (!readEntry(e)) {
      return;
    }
    out.append(std::move(e));
  }
}

void BlockReader::bag(PropertyBag &bag, Vector<BlockEntry> &unread)
{
  const uint32_t n = entryCount();
  for (uint32_t i = 0; i < n; i++) {
    BlockEntry e;
    if (!readEntry(e)) {
      return;
    }
    const int at = e.unknownTag ? -1 : bag.indexOf(e.name.c_str());
    if (at < 0 || !bag.setAt(at, e.value).ok()) {
      unread.append(std::move(e));
    }
  }
}

/* ---- whole payloads ---- */

namespace {

Status readyFor(const Library &lib, const char *what)
{
  if (lib.busy()) {
    return Status::error(StatusCode::Busy, (std::string(what) + ": the library is busy").c_str());
  }
  if (lib.runtime().hasStickyError()) {
    return Status::error(
        StatusCode::FailedPrecondition,
        (std::string(what) + ": a sticky error is pending; take it first").c_str());
  }
  return Status();
}

/* The block's type, if its data goes through payloads. */
Result<const BlockTypeRegistry::Entry *> payloadType(const Library &lib,
                                                     const Datablock &b,
                                                     const char *what)
{
  const BlockTypeRegistry::Entry *e = lib.runtime().ext<BlockTypeRegistry>().find(b.type());
  if (!e || e->info.storage != BlockStorage::Payload) {
    return Status::error(StatusCode::Unsupported,
                         (std::string(what) + ": the type of '" + b.name().c_str() +
                          "' has no serialize and deserialize")
                             .c_str());
  }
  return e;
}

Status notFound(const char *what)
{
  return Status::error(StatusCode::NotFound, (std::string(what) + ": no such block").c_str());
}

Status badState(const char *what, const Datablock &b, const char *want)
{
  return Status::error(StatusCode::FailedPrecondition,
                       (std::string(what) + ": block '" + b.name().c_str() + "' is " +
                        blockStateName(b.state()) + ", not " + want)
                           .c_str());
}

} // namespace

Result<BlockPayload> writePayload(const Library &lib, BlockHandle h)
{
  CDCC_TRY(readyFor(lib, "writePayload"));
  const Datablock *b = lib.get(h);
  if (!b) {
    return notFound("writePayload");
  }
  if (b->dataState() != BlockState::Live || b->state() == BlockState::Doomed) {
    return badState("writePayload", *b, "Live or InHistory");
  }
  Result<const BlockTypeRegistry::Entry *> type = payloadType(lib, *b, "writePayload");
  if (!type.ok()) {
    return type.status();
  }
  BlockWriter out(lib);
  Status s = (*type)->provider.get().serialize(*b, out);
  if (lib.runtime().hasStickyError()) {
    s = lib.runtime().takeStickyError(); /* a foreign serialize threw */
  }
  if (!s.ok()) {
    return s;
  }
  Result<BlockPayload> p = out.take((*type)->info.payloadVersion);
  if (p.ok() && lib.auditsRelations()) {
    CDCC_TRY(checkPayloadRefs(lib, h, *p));
  }
  return p;
}

Status checkPayloadRefs(const Library &lib, BlockHandle h, const BlockPayload &payload)
{
  /* Bit 0: strong, bit 1: weak. */
  const ClientRegistry &clients = lib.runtime().ext<ClientRegistry>();
  const BlockIdHasher hasher(lib.runtime().newLocalId());
  const auto note = [&](Map<BlockIdKey, int> &to, const RefCell &cell) {
    if (!cell.target.isNull()) {
      const BlockIdKey id = hasher.key(clients.canonical(cell.target));
      const int *have = to.lookup_ptr(id);
      to.add_overwrite(id, (have ? *have : 0) | (isStrong(cell.kind) ? 1 : 2));
    }
  };
  Map<BlockIdKey, int> rows, refs;
  for (const RefCell &cell : payload.refs) {
    note(rows, cell);
  }
  CDCC_TRY(lib.foreachDataRef(h, [&](RefCell &cell) { note(refs, cell); }));
  const auto differs = [](const Map<BlockIdKey, int> &a, const Map<BlockIdKey, int> &b) {
    for (const auto &pair : a) {
      const int *other = b.lookup_ptr(pair.key);
      if (!other || (pair.value & ~*other)) {
        return true;
      }
    }
    return false;
  };
  const Datablock *b = lib.get(h);
  const std::string name = b ? b->name().c_str() : "?";
  if (differs(refs, rows)) {
    return Status::error(StatusCode::Internal,
                         ("'" + name + "': foreachRef reports a reference that serialize "
                                       "didn't write to the ref table (or of another strength)")
                             .c_str());
  }
  if (differs(rows, refs)) {
    return Status::error(StatusCode::Internal,
                         ("'" + name + "': serialize wrote a reference that foreachRef doesn't "
                                       "report (or of another strength)")
                             .c_str());
  }
  return Status();
}

Status readPayload(Library &lib, BlockHandle h, const BlockPayload &payload)
{
  CDCC_TRY(readyFor(lib, "readPayload"));
  const Datablock *b = lib.get(h);
  if (!b) {
    return notFound("readPayload");
  }
  if (b->state() != BlockState::Live) {
    return badState("readPayload", *b, "Live");
  }
  if (b->clientData()) {
    return Status::error(
        StatusCode::FailedPrecondition,
        ("readPayload: block '" + std::string(b->name().c_str()) + "' has data").c_str());
  }
  Result<const BlockTypeRegistry::Entry *> type = payloadType(lib, *b, "readPayload");
  if (!type.ok()) {
    return type.status();
  }
  if (payload.version > (*type)->info.payloadVersion) {
    return Status::error(StatusCode::Unsupported,
                         ("readPayload: '" + std::string(b->name().c_str()) +
                          "' was written by a newer version of its type")
                             .c_str());
  }
  return detail::LibraryFiller::read(lib, h, **type, payload);
}

} // namespace coredcc
