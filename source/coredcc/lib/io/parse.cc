#include "coredcc/lib/io/parse.h"
#include "coredcc/lib/type_names.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <tuple>
#include <utility>

namespace coredcc::detail::libio {

using litestl::util::Vector;

namespace {

Status damaged(const std::string &what)
{
  return Status::error(StatusCode::InvalidArgument, ("the library section: " + what).c_str());
}

Status tooLarge(const std::string &what)
{
  return Status::error(StatusCode::OutOfRange, ("the library section: " + what).c_str());
}

std::string tagText(uint32_t tag)
{
  std::string s = "'";
  for (int i = 0; i < 4; i++) {
    const char c = char(tag >> (8 * i) & 0xff);
    s += c >= ' ' && c <= '~' ? c : '?';
  }
  return s + "'";
}

struct Chunk {
  bool found = false;
  uint32_t flags = 0;
  uint64_t raw = 0;
  std::span<const uint8_t> stored;
};

} // namespace

void readExtension(BytesReader &in, bool &critical)
{
  uint32_t last = 0;
  for (bool any = false; in.ok() && !in.atEnd(); any = true) {
    const uint32_t tag = in.u32();
    in.blob();
    if (any && tag <= last) {
      in.fail(StatusCode::InvalidArgument, "extension fields out of order");
    }
    critical |= (tag & kFieldCritical) != 0;
    last = tag;
  }
}

bool hasCriticalField(std::span<const uint8_t> extension)
{
  BytesReader in(extension);
  bool critical = false;
  readExtension(in, critical);
  return critical;
}

namespace {

class Parser {
 public:
  Parser(std::span<const uint8_t> bytes, const LoadLimits &limits, IoIssues &issues)
      : bytes_(bytes), limits_(limits), issues_(issues), file_(std::make_unique<ParsedFile>())
  {
    file_->budget.left = limits.maxBytes;
  }

  Result<std::unique_ptr<ParsedFile>> run()
  {
    CDCC_TRY(header());
    CDCC_TRY(chunks());
    std::span<const uint8_t> names, blocks;
    CDCC_TRY(open(name_, "NAME", file_->nameBytes, names));
    CDCC_TRY(open(blocks_, "BLCK", file_->blockBytes, blocks));
    if (data_.flags & kChunkLz4) {
      return damaged("DATA is never compressed as a whole");
    }
    CDCC_TRY(section("NAME", names, [this](BytesReader &in) { return readNames(in); }));
    CDCC_TRY(section("BLCK", blocks, [this](BytesReader &in) { return readBlocks(in); }));
    CDCC_TRY(section("DATA", data_.stored, [this](BytesReader &in) { return readData(in); }));
    return std::move(file_);
  }

 private:
  /* ---- the container ---- */

  Status header()
  {
    if (bytes_.size() < kHeaderBytes + 8) {
      return damaged("too short");
    }
    if (memcmp(bytes_.data(), kMagic, sizeof(kMagic)) != 0) {
      return damaged("it doesn't start as one does");
    }
    BytesReader in(bytes_.subspan(sizeof(kMagic)));
    version_ = in.u32();
    const uint32_t minReader = in.u32();
    const uint64_t total = in.u64();
    chunkCount_ = in.u32();
    if (minReader > kVersion) {
      return Status::error(StatusCode::Unsupported,
                           ("the library section needs a newer build (format " +
                            std::to_string(minReader) + ")")
                               .c_str());
    }
    if (version_ == 0 || minReader == 0 || minReader > version_) {
      return damaged("impossible version numbers");
    }
    if (total != bytes_.size()) {
      return damaged("it is " + std::to_string(bytes_.size()) + " bytes, but says " +
                     std::to_string(total));
    }
    const std::string_view all(reinterpret_cast<const char *>(bytes_.data()), bytes_.size() - 8);
    BytesReader tail(bytes_.subspan(bytes_.size() - 8));
    if (tail.u64() != fnv1a64(all)) {
      return damaged("the checksum doesn't match");
    }
    if (chunkCount_ > kMaxChunks) {
      return damaged("too many chunks");
    }
    return Status();
  }

  Status chunks()
  {
    BytesReader in(bytes_.subspan(kHeaderBytes, bytes_.size() - kHeaderBytes - 8));
    for (uint32_t i = 0; i < chunkCount_; i++) {
      const uint32_t tag = in.u32();
      Chunk c;
      c.found = true;
      c.flags = in.u32();
      c.raw = in.u64();
      const uint64_t stored = in.u64();
      if (stored > in.remaining()) {
        in.fail(StatusCode::InvalidArgument, "a chunk goes past the end");
      }
      c.stored = in.raw(in.ok() ? size_t(stored) : 0);
      if (!in.ok()) {
        break;
      }
      Chunk *to = tag == kTagName   ? &name_ :
                  tag == kTagBlocks ? &blocks_ :
                  tag == kTagData   ? &data_ :
                                      nullptr;
      if (!to) {
        if (c.flags & kChunkCritical) {
          return Status::error(StatusCode::Unsupported,
                               ("the library section has a chunk " + tagText(tag) +
                                " that this build doesn't know and can't do without")
                                   .c_str());
        }
        issues_.add(BlockId(), {},
                    Status::error(StatusCode::Unsupported,
                                  ("a chunk " + tagText(tag) + " this build doesn't know").c_str()));
        continue;
      }
      if (c.flags & ~(kChunkCritical | kChunkLz4)) {
        return Status::error(StatusCode::Unsupported,
                             ("the chunk " + tagText(tag) + " has flags this build doesn't know")
                                 .c_str());
      }
      if (to->found) {
        return damaged("two chunks " + tagText(tag));
      }
      *to = c;
    }
    CDCC_TRY(in.status());
    if (!in.atEnd()) {
      return damaged("bytes after the last chunk");
    }
    if (!name_.found || !blocks_.found || !data_.found) {
      return damaged("a chunk is missing");
    }
    return Status();
  }

  Status open(const Chunk &c, const char *what, Vector<uint8_t> &own, std::span<const uint8_t> &out)
  {
    const bool lz4 = (c.flags & kChunkLz4) != 0;
    if (!lz4) {
      if (c.raw != c.stored.size()) {
        return damaged(std::string(what) + " says the wrong size");
      }
      out = c.stored;
      return Status();
    }
    if (Status s = unpack(c.stored, true, c.raw, file_->budget, own); !s.ok()) {
      return Status::error(s.code(), (std::string(what) + ": " + s.message()).c_str());
    }
    out = std::span<const uint8_t>(own.data(), size_t(own.size()));
    return Status();
  }

  template <class Fn> Status section(const char *what, std::span<const uint8_t> bytes, Fn &&read)
  {
    BytesReader in(bytes);
    Status s = read(in);
    if (s.ok()) {
      s = in.status();
    }
    if (s.ok() && !in.atEnd()) {
      s = trailing(std::string("bytes after what ") + what + " should hold");
    }
    return s.ok() ? s :
                    Status::error(s.code(), (std::string(what) + ": " + s.message()).c_str());
  }

  /* A newer file may hold more than this build knows; ours may not. */
  Status trailing(const std::string &what)
  {
    if (version_ <= kVersion) {
      return damaged(what);
    }
    issues_.add(BlockId(), {}, Status::error(StatusCode::Unsupported, what.c_str()));
    return Status();
  }

  /* A code with a value that this build doesn't know. */
  Status unknown(const char *what) const
  {
    return version_ <= kVersion ?
               damaged(what) :
               Status::error(StatusCode::Unsupported,
                             (std::string("the library section: ") + what +
                              ", of a newer format than this build's")
                                 .c_str());
  }

  Status charge(uint64_t n)
  {
    return file_->budget.take(n) ? Status() : tooLarge("it needs more memory than allowed");
  }

  /* Reserves room for `n` more items, which have been charged. `room` is what the Vector has:
   * it doesn't say itself. */
  template <class T> Status reserve(Vector<T> &v, uint32_t n, uint64_t &room)
  {
    const uint64_t need = uint64_t(v.size()) + n;
    if (need > room) {
      const uint64_t grown = need + need / 2;
      CDCC_TRY(probeMemory(grown * 2 * sizeof(T)));
      v.ensure_capacity(size_t(grown));
      room = grown;
    }
    return Status();
  }
  template <class T> Status reserve(Vector<T> &v, uint32_t n)
  {
    CDCC_TRY(probeMemory(uint64_t(n) * 2 * sizeof(T)));
    v.ensure_capacity(n);
    return Status();
  }

  /* ---- NAME ---- */

  Status readNames(BytesReader &in)
  {
    const uint32_t n = in.count(kNameBytes);
    CDCC_TRY(charge(uint64_t(n) * kNameCost));
    CDCC_TRY(reserve(file_->names, n));
    for (uint32_t i = 0; i < n; i++) {
      FileName name;
      const uint8_t form = in.u8();
      if (form == uint8_t(NameForm::Text)) {
        name.text = in.str();
        if (name.text.empty()) {
          in.fail(StatusCode::InvalidArgument, "an empty name");
        }
        name.client = ClientId::of(name.text);
        name.type = typeNamed(name.text);
      }
      else if (form == uint8_t(NameForm::RawId)) {
        name.isRaw = true;
        name.client.value = name.type.value = in.u64();
        if (!name.client) {
          in.fail(StatusCode::InvalidArgument, "a null name");
        }
      }
      else {
        in.fail(unknown("an unknown kind of name"));
      }
      CDCC_TRY(in.status());
      file_->names.append(name);
    }
    return Status();
  }

  FileName *name(BytesReader &in)
  {
    const uint32_t i = in.u32();
    if (i > uint32_t(file_->names.size())) {
      in.fail(StatusCode::InvalidArgument, "an index outside NAME");
    }
    return i == 0 || !in.ok() ? nullptr : &file_->names[int(i - 1)];
  }
  ClientId clientName(BytesReader &in)
  {
    FileName *n = name(in);
    if (!n) {
      return ClientId();
    }
    n->asClient = true;
    return n->client;
  }
  /* A name that must be text; with `type`, a type's. */
  std::string_view text(BytesReader &in, TypeId *type)
  {
    FileName *n = name(in);
    if (!n) {
      return {};
    }
    if (n->isRaw) {
      in.fail(StatusCode::InvalidArgument, "an id where a name belongs");
    }
    if (type) {
      n->asType = true;
      *type = n->type;
    }
    return n->text;
  }

  /* ---- BLCK ---- */

  RefCell row(BytesReader &in)
  {
    RefCell cell;
    const uint8_t kind = in.u8();
    switch (kind) {
      case kRowStrong:
        cell.kind = RefKind::Strong;
        break;
      case kRowOptStrong:
        cell.kind = RefKind::OptStrong;
        break;
      case kRowWeak:
        cell.kind = RefKind::Weak;
        break;
      default:
        in.fail(unknown("an unknown kind of reference"));
    }
    cell.target.client = clientName(in);
    cell.target.local.hi = in.u64();
    cell.target.local.lo = in.u64();
    if (cell.isNull() && cell.target.client) {
      in.fail(StatusCode::InvalidArgument, "a null reference with a client");
    }
    return cell;
  }

  Status rows(BytesReader &in, Range &out)
  {
    out.begin = uint32_t(file_->rows.size());
    out.count = in.count(kRowBytes);
    CDCC_TRY(charge(uint64_t(out.count) * kRowCost));
    CDCC_TRY(reserve(file_->rows, out.count, rowRoom_));
    for (uint32_t i = 0; i < out.count && in.ok(); i++) {
      file_->rows.append(row(in));
    }
    return in.status();
  }

  Status slots(BytesReader &in, Range &out)
  {
    out.begin = uint32_t(file_->slots.size());
    out.count = in.count(kSlotBytes);
    CDCC_TRY(charge(uint64_t(out.count) * kSlotCost));
    CDCC_TRY(reserve(file_->slots, out.count, slotRoom_));
    for (uint32_t i = 0; i < out.count && in.ok(); i++) {
      ParsedSlot s;
      if (FileName *n = name(in)) {
        n->asType = true;
        s.ns = n->type;
      }
      s.key = in.i64();
      s.cell = row(in);
      file_->slots.append(s);
    }
    CDCC_TRY(in.status());
    Vector<std::pair<uint64_t, int64_t>> keys;
    for (uint32_t i = 0; i < out.count; i++) {
      const ParsedSlot &s = file_->slots[int(out.begin + i)];
      keys.append({s.ns.value, s.key});
    }
    std::sort(first(keys), last(keys));
    if (std::adjacent_find(first(keys), last(keys)) != last(keys)) {
      in.fail(StatusCode::InvalidArgument, "a block has one slot twice");
    }
    return in.status();
  }

  Status attachments(BytesReader &in, Range &out)
  {
    out.begin = uint32_t(file_->attachments.size());
    out.count = in.count(kAttachmentBytes);
    CDCC_TRY(charge(uint64_t(out.count) * kAttachmentCost));
    CDCC_TRY(reserve(file_->attachments, out.count, attachmentRoom_));
    for (uint32_t i = 0; i < out.count && in.ok(); i++) {
      ParsedAttachment a;
      a.type = text(in, &a.typeId);
      if (a.type.empty()) {
        in.fail(StatusCode::InvalidArgument, "an attachment without a type");
      }
      a.version = in.u32();
      a.blob = in.u32();
      CDCC_TRY(charge(a.type.size()));
      CDCC_TRY(rows(in, a.rows));
      file_->attachments.append(a);
    }
    CDCC_TRY(in.status());
    Vector<uint64_t> types;
    for (uint32_t i = 0; i < out.count; i++) {
      types.append(file_->attachments[int(out.begin + i)].typeId.value);
    }
    std::sort(first(types), last(types));
    if (std::adjacent_find(first(types), last(types)) != last(types)) {
      in.fail(StatusCode::InvalidArgument, "a block has one attachment twice");
    }
    return in.status();
  }

  Status record(BytesReader &in, ParsedRecord &r)
  {
    r.id.client = clientName(in);
    r.id.local.hi = in.u64();
    r.id.local.lo = in.u64();
    r.type = text(in, &r.typeId);
    r.name = in.str();
    r.flags = in.u32();
    const uint8_t as = in.u8();
    r.version = in.u32();
    r.source = text(in, nullptr);
    CDCC_TRY(in.status());
    if (r.id.isNull()) {
      return damaged("a block without an id");
    }
    if (as > uint8_t(StoredAs::NoPayload)) {
      return unknown("a block stored in an unknown way");
    }
    r.storedAs = StoredAs(as);
    if (r.type.empty() && r.storedAs != StoredAs::NoPayload) {
      return damaged("a payload without a type");
    }
    if (r.flags & ~(kFlagRoot | kFlagFakeUser)) {
      CDCC_TRY(trailing("a block has flags this build doesn't know"));
    }
    CDCC_TRY(charge(r.type.size() + r.name.size() + r.source.size()));
    CDCC_TRY(rows(in, r.rows));
    CDCC_TRY(slots(in, r.slots));
    CDCC_TRY(attachments(in, r.attachments));
    r.blob = in.u32();
    r.extension = in.blob();
    CDCC_TRY(in.status());
    CDCC_TRY(charge(r.extension.size()));
    BytesReader fields(r.extension);
    bool critical = false;
    readExtension(fields, critical);
    if (!fields.ok()) {
      return damaged(std::string("a block's extension: ") + fields.status().message());
    }
    if (r.blob != 0 && r.storedAs == StoredAs::NoPayload) {
      return damaged("a block without a payload has bytes");
    }
    if (!in.atEnd()) {
      CDCC_TRY(trailing("bytes after a block's record"));
    }
    return Status();
  }

  Status readBlocks(BytesReader &in)
  {
    const uint32_t n = in.count(kRecordBytes);
    if (n > limits_.maxBlocks) {
      return tooLarge("more than " + std::to_string(limits_.maxBlocks) + " blocks");
    }
    CDCC_TRY(charge(uint64_t(n) * kRecordCost));
    CDCC_TRY(reserve(file_->records, n));
    for (uint32_t i = 0; i < n; i++) {
      BytesReader body(in.blob());
      CDCC_TRY(in.status());
      ParsedRecord r;
      if (Status s = record(body, r); !s.ok()) {
        return Status::error(s.code(), ("block " + std::to_string(i) + " of the file, '" +
                                        std::string(r.name.substr(0, 64)) + "': " + s.message())
                                           .c_str());
      }
      file_->records.append(r);
    }
    return Status();
  }

  /* ---- DATA ---- */

  Status readData(BytesReader &in)
  {
    const uint32_t n = in.count(kBlobBytes);
    CDCC_TRY(charge(uint64_t(n) * kBlobCost));
    CDCC_TRY(reserve(file_->blobs, n));
    file_->blobs.resize(n);
    Vector<bool> used;
    used.resize(n);
    const auto use = [&](uint32_t index) -> Status {
      if (index == 0) {
        return Status();
      }
      if (index > n || used[int(index - 1)]) {
        return damaged("data that isn't there, or is used twice");
      }
      used[int(index - 1)] = true;
      return Status();
    };
    for (const ParsedRecord &r : file_->records) {
      CDCC_TRY(use(r.blob));
    }
    for (const ParsedAttachment &a : file_->attachments) {
      CDCC_TRY(use(a.blob));
    }
    for (uint32_t i = 0; i < n; i++) {
      const uint8_t codec = in.u8();
      if (codec > 1) {
        in.fail(unknown("data compressed in an unknown way"));
      }
      const bool lz4 = codec == 1;
      const uint64_t raw = in.u64();
      const uint64_t stored = in.u64();
      if (stored > in.remaining()) {
        in.fail(StatusCode::InvalidArgument, "data goes past the end");
      }
      const std::span<const uint8_t> bytes = in.raw(in.ok() ? size_t(stored) : 0);
      CDCC_TRY(in.status());
      if (!used[int(i)]) {
        CDCC_TRY(trailing("data that no block uses"));
        continue;
      }
      if (Status s = unpack(bytes, lz4, raw, file_->budget, file_->blobs[int(i)]); !s.ok()) {
        return Status::error(
            s.code(),
            ("data " + std::to_string(i) + ": " + s.message()).c_str());
      }
    }
    return Status();
  }

  std::span<const uint8_t> bytes_;
  const LoadLimits &limits_;
  IoIssues &issues_;
  std::unique_ptr<ParsedFile> file_;
  uint32_t version_ = 0;
  uint32_t chunkCount_ = 0;
  uint64_t rowRoom_ = 0, slotRoom_ = 0, attachmentRoom_ = 0;
  Chunk name_, blocks_, data_;
};

} // namespace

Result<std::unique_ptr<ParsedFile>> parseLibrary(std::span<const uint8_t> bytes,
                                                 const LoadLimits &limits,
                                                 IoIssues &issues)
{
  return Parser(bytes, limits, issues).run();
}

} // namespace coredcc::detail::libio
