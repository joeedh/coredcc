#include "coredcc/lib/io/format.h"
#include "coredcc/lib/io/library_io.h"

#include "coredcc/lib/attachment.h"
#include "coredcc/lib/library.h"
#include "coredcc/lib/payload_access.h"

#include "litestl/util/map.h"

#include <algorithm>
#include <map>
#include <string>
#include <tuple>
#include <utility>

namespace coredcc {

using litestl::util::Map;
using litestl::util::Vector;

namespace detail::libio {

namespace {

std::span<const uint8_t> view(const Vector<uint8_t> &v)
{
  return std::span<const uint8_t>(v.data(), size_t(v.size()));
}

template <class T> void put(BytesWriter &out, const Vector<T> &v)
{
  out.raw(view(v));
}

/* The NAME chunk's table. Indices are in order of first use, which is sorted order. */
class Names {
 public:
  explicit Names(const Library &lib) : rt_(lib.runtime()), files_(lib.fileNames()) {}

  uint32_t text(std::string_view s)
  {
    return s.empty() ? 0 : add(std::string(1, 't') + std::string(s));
  }
  uint32_t id(IdDomain domain, uint64_t value)
  {
    if (value == 0) {
      return 0;
    }
    const uint32_t *known = byId_[int(domain)].lookup_ptr(value);
    if (known) {
      return *known;
    }
    const char *name = nameOf(domain, value);
    const uint32_t index =
        name ? text(name) : add(std::string(1, 'i') + std::to_string(value));
    if (!name) {
      raw_.emplace(index, value);
    }
    byId_[int(domain)].add(value, index);
    return index;
  }
  /* Null when neither the Runtime nor a file named it. */
  const char *nameOf(IdDomain domain, uint64_t value) const
  {
    const char *name = rt_.nameOf(domain, value);
    return name ? name : files_.nameOf(domain, value);
  }
  void write(BytesWriter &out) const
  {
    out.u32(uint32_t(order_.size()));
    for (uint32_t i = 0; i < uint32_t(order_.size()); i++) {
      const auto raw = raw_.find(i + 1);
      if (raw != raw_.end()) {
        out.u8(uint8_t(NameForm::RawId));
        out.u64(raw->second);
      }
      else {
        out.u8(uint8_t(NameForm::Text));
        out.str(order_[int(i)]->substr(1));
      }
    }
  }

 private:
  uint32_t add(std::string key)
  {
    const auto [it, fresh] = index_.emplace(std::move(key), uint32_t(index_.size()) + 1);
    if (fresh) {
      order_.append(&it->first);
    }
    return it->second;
  }

  const Runtime &rt_;
  const NameTable &files_;
  Map<uint64_t, uint32_t> byId_[kIdDomainCount];
  /* Keys have a form letter in front, so an id never equals a text. */
  std::map<std::string, uint32_t> index_;
  std::map<uint32_t, uint64_t> raw_;
  Vector<const std::string *> order_;
};

struct Item {
  BlockHandle h;
  const Datablock *b = nullptr;
};

class Saver {
 public:
  Saver(const Library &lib, SaveReport &report)
      : lib_(lib), rt_(lib.runtime()), clients_(rt_.ext<ClientRegistry>()),
        types_(rt_.ext<BlockTypeRegistry>()), attachments_(rt_.ext<AttachmentTypeRegistry>()),
        report_(report), names_(lib)
  {
  }

  Result<Vector<uint8_t>> run()
  {
    CDCC_TRY(collectItems());
    BytesWriter blocks;
    blocks.u32(uint32_t(items_.size()));
    for (const Item &item : items_) {
      const size_t record = blocks.beginSection();
      if (Status s = writeRecord(blocks, item); !s.ok()) {
        return named(s, *item.b);
      }
      blocks.endSection(record);
    }
    CDCC_TRY(blocks.status());
    report_.blocks = int64_t(items_.size());
    BytesWriter names;
    names_.write(names);
    CDCC_TRY(names.status());
    return assemble(pack(view(names.data())), pack(view(blocks.data())));
  }

 private:
  static Status named(const Status &s, const Datablock &b)
  {
    return Status::error(s.code(),
                         ("saving '" + std::string(b.name().c_str()) + "': " + s.message()).c_str());
  }
  Status takeSticky(Status s)
  {
    return rt_.hasStickyError() ? rt_.takeStickyError() : s;
  }

  /* ---- what is saved ---- */

  bool isTransient(const Datablock &b) const
  {
    const BlockTypeRegistry::Entry *e = types_.find(b.type());
    return b.state() == BlockState::Live && e && e->info.storage == BlockStorage::Transient;
  }
  static bool wasRecord(const Datablock &b)
  {
    return b.placeholderReason() == PlaceholderReason::MissingType || b.type();
  }
  static bool isSaveRoot(const Datablock &b)
  {
    return b.hasFlag(BlockFlags::Root) || b.hasFlag(BlockFlags::FakeUser) ||
           b.pins(PinKind::Host) > 0 || b.state() == BlockState::Detached ||
           (b.state() == BlockState::Placeholder && wasRecord(b));
  }
  /* A bare MissingBlock is made again from the references to it. */
  bool isWritten(BlockHandle h, const Datablock &b) const
  {
    if (b.state() != BlockState::Placeholder || wasRecord(b)) {
      return true;
    }
    bool any = b.name().size() > 0 || b.hasFlag(BlockFlags::Root) ||
               b.hasFlag(BlockFlags::FakeUser);
    lib_.forEachSlot(h, [&](TypeId, int64_t, const RefCell &) { any = true; });
    lib_.forEachAttachment(h, [&](TypeId, void *) { any = true; });
    lib_.forEachOpaqueAttachment(h, [&](const OpaqueAttachment &) { any = true; });
    return any;
  }

  Status mark(Map<BlockHandle, bool> &marked, bool throughTransient)
  {
    Vector<BlockHandle> stack;
    const auto visit = [&](BlockHandle h, const Datablock &b) {
      if (!marked.lookup_ptr(h) && b.state() != BlockState::InHistory &&
          (throughTransient || !isTransient(b)))
      {
        marked.add(h, true);
        stack.append(h);
      }
    };
    lib_.forEach([&](BlockHandle h, const Datablock &b) {
      if (isSaveRoot(b)) {
        visit(h, b);
      }
    });
    while (stack.size() > 0) {
      const BlockHandle h = stack.pop_back();
      Status s = lib_.foreachRef(h, [&](RefCell &cell) {
        const BlockHandle t =
            isStrong(cell.kind) && !cell.target.isNull() ? lib_.find(cell.target) : BlockHandle();
        if (t) {
          visit(t, *lib_.get(t));
        }
      });
      s = takeSticky(std::move(s));
      if (!s.ok()) {
        return named(s, *lib_.get(h));
      }
    }
    return Status();
  }

  Status collectItems()
  {
    Map<BlockHandle, bool> marked, direct;
    CDCC_TRY(mark(marked, true));
    CDCC_TRY(mark(direct, false));
    Vector<std::string> unsaved;
    struct Keyed {
      std::tuple<int, std::string, uint64_t> client;
      LocalId local;
      Item item;
    };
    Vector<Keyed> sorted;
    lib_.forEach([&](BlockHandle h, const Datablock &b) {
      if (!marked.lookup_ptr(h) || !isWritten(h, b)) {
        return;
      }
      const uint64_t c = b.id().client.value;
      const char *name = names_.nameOf(IdDomain::Client, c);
      sorted.append(Keyed{{c == 0 ? 0 : name ? 1 : 2, name ? name : "", name ? 0 : c},
                          b.id().local,
                          Item{h, &b}});
      const BlockTypeRegistry::Entry *e = types_.find(b.type());
      if (b.state() == BlockState::Live && e && e->info.storage == BlockStorage::Unspecified) {
        unsaved.append(e->info.name.c_str());
      }
    });
    std::sort(first(sorted), last(sorted), [](const Keyed &a, const Keyed &b) {
      return std::tie(a.client, a.local) < std::tie(b.client, b.local);
    });
    for (const Keyed &k : sorted) {
      if (isTransient(*k.item.b)) {
        report_.transient.append(k.item.h);
        continue;
      }
      if (k.item.b->state() == BlockState::Detached) {
        report_.detached.append(k.item.h);
      }
      if (!direct.lookup_ptr(k.item.h)) {
        report_.throughTransient.append(k.item.h);
      }
      items_.append(k.item);
    }
    if (unsaved.size() == 0) {
      return Status();
    }
    std::sort(first(unsaved), last(unsaved));
    std::string list;
    for (int i = 0; i < int(unsaved.size()); i++) {
      if (i == 0 || unsaved[i] != unsaved[i - 1]) {
        list += (list.empty() ? "" : ", ") + unsaved[i];
      }
    }
    return Status::error(StatusCode::FailedPrecondition,
                         ("these block types neither serialize nor are Transient: " + list).c_str());
  }

  /* ---- records ---- */

  uint32_t clientIndex(ClientId client)
  {
    return names_.id(IdDomain::Client, client.value);
  }
  void writeRow(BytesWriter &out, const RefCell &cell)
  {
    out.u8(cell.kind == RefKind::Strong    ? kRowStrong :
           cell.kind == RefKind::OptStrong ? kRowOptStrong :
                                             kRowWeak);
    BlockId id = cell.target.isNull() ? BlockId() : clients_.canonical(cell.target);
    if (cell.kind == RefKind::OptStrong && !id.isNull()) {
      const Datablock *t = lib_.get(lib_.find(id));
      if (!t || t->state() == BlockState::InHistory) {
        id = BlockId(); /* the user deleted it */
      }
    }
    out.u32(id.isNull() ? 0 : clientIndex(id.client));
    out.u64(id.local.hi);
    out.u64(id.local.lo);
  }
  void writeRows(BytesWriter &out, const Vector<RefCell> &rows)
  {
    out.u32(uint32_t(rows.size()));
    for (const RefCell &cell : rows) {
      writeRow(out, cell);
    }
  }
  Result<uint32_t> blob(const Vector<uint8_t> &bytes)
  {
    if (bytes.size() == 0) {
      return uint32_t(0);
    }
    if (uint64_t(bytes.size()) > kMaxRaw) {
      return Status::error(StatusCode::OutOfRange, "the payload is too large");
    }
    blobs_.append(pack(view(bytes)));
    return uint32_t(blobs_.size());
  }

  Status writeSlots(BytesWriter &out, const Item &item)
  {
    struct Slot {
      std::tuple<int, std::string, uint64_t> ns;
      int64_t key;
      TypeId type;
      RefCell cell;
    };
    Vector<Slot> slots;
    lib_.forEachSlot(item.h, [&](TypeId ns, int64_t key, const RefCell &cell) {
      const char *name = names_.nameOf(IdDomain::Type, ns.value);
      slots.append(
          Slot{{name ? 0 : 1, name ? name : "", name ? 0 : ns.value}, key, ns, cell});
    });
    std::sort(first(slots), last(slots), [](const Slot &a, const Slot &b) {
      return std::tie(a.ns, a.key) < std::tie(b.ns, b.key);
    });
    out.u32(uint32_t(slots.size()));
    for (const Slot &s : slots) {
      out.u32(names_.id(IdDomain::Type, s.type.value));
      out.i64(s.key);
      writeRow(out, s.cell);
    }
    return Status();
  }

  Status writeAttachments(BytesWriter &out, const Item &item)
  {
    struct Att {
      std::string name;
      const AttachmentTypeRegistry::Entry *type = nullptr;
      void *data = nullptr;
      const OpaqueAttachment *opaque = nullptr;
    };
    Vector<Att> all;
    lib_.forEachAttachment(item.h, [&](TypeId type, void *data) {
      const AttachmentTypeRegistry::Entry *e = attachments_.find(type);
      if (e->provider->hasSerialize()) {
        all.append(Att{e->info.name.c_str(), e, data, nullptr});
      }
      else {
        report_.skippedAttachments++;
      }
    });
    lib_.forEachOpaqueAttachment(item.h, [&](const OpaqueAttachment &o) {
      all.append(Att{o.typeName.c_str(), nullptr, nullptr, &o});
    });
    std::sort(first(all), last(all), [](const Att &a, const Att &b) { return a.name < b.name; });
    out.u32(uint32_t(all.size()));
    for (const Att &a : all) {
      BlockPayload own;
      if (a.type) {
        BlockWriter w(lib_);
        Status s = takeSticky(a.type->provider.get().serialize(*item.b, a.data, w));
        Result<BlockPayload> p = s.ok() ? PayloadAccess::take(w, a.type->info.payloadVersion) :
                                        Result<BlockPayload>(s);
        if (!p.ok()) {
          return Status::error(p.status().code(),
                               ("attachment '" + a.name + "': " + p.status().message()).c_str());
        }
        own = std::move(p).value();
      }
      const BlockPayload &payload = a.opaque ? a.opaque->payload : own;
      Result<uint32_t> data = blob(payload.bytes);
      if (!data.ok()) {
        return data.status();
      }
      out.u32(names_.text(a.name));
      out.u32(payload.version);
      out.u32(*data);
      writeRows(out, payload.refs);
    }
    return Status();
  }

  Status writeRecord(BytesWriter &out, const Item &item)
  {
    const Datablock &b = *item.b;
    StoredAs as = StoredAs::NoPayload;
    BlockPayload own;
    const BlockPayload *payload = &own;
    if (b.state() == BlockState::Live) {
      as = StoredAs::Payload;
      Result<BlockPayload> p = writePayload(lib_, item.h);
      if (!p.ok()) {
        return p.status();
      }
      own = std::move(p).value();
      if (!lib_.auditsRelations()) {
        if (Status s = checkPayloadRefs(lib_, item.h, own); !s.ok()) {
          report_.issues.add(b.id(), b.name().c_str(), takeSticky(std::move(s)));
        }
      }
    }
    else if (b.state() == BlockState::Placeholder) {
      payload = &b.placeholderData();
      if (b.placeholderReason() == PlaceholderReason::MissingType) {
        as = StoredAs::Payload;
      }
    }
    else { /* Detached */
      Status s = lib_.foreachDataRef(item.h, [&](RefCell &cell) { own.refs.append(cell); });
      CDCC_TRY(takeSticky(std::move(s)));
    }
    const bool named = b.state() == BlockState::Placeholder;
    out.u32(clientIndex(b.id().client));
    out.u64(b.id().local.hi);
    out.u64(b.id().local.lo);
    out.u32(named ? names_.text(b.placeholderType().c_str()) :
                    names_.id(IdDomain::Type, b.type().value));
    out.str(b.name().c_str());
    out.u32((b.hasFlag(BlockFlags::Root) ? kFlagRoot : 0) |
            (b.hasFlag(BlockFlags::FakeUser) ? kFlagFakeUser : 0));
    out.u8(uint8_t(as));
    out.u32(payload->version);
    out.u32(names_.text(b.sourceName().c_str()));
    writeRows(out, payload->refs);
    CDCC_TRY(writeSlots(out, item));
    CDCC_TRY(writeAttachments(out, item));
    Result<uint32_t> data =
        as == StoredAs::NoPayload ? Result<uint32_t>(uint32_t(0)) : blob(payload->bytes);
    if (!data.ok()) {
      return data.status();
    }
    out.u32(*data);
    out.blob(b.fileExtension());
    return Status();
  }

  /* ---- the file ---- */

  static void chunkHeader(BytesWriter &out, uint32_t tag, bool lz4, uint64_t raw, uint64_t stored)
  {
    out.u32(tag);
    out.u32(kChunkCritical | (lz4 ? kChunkLz4 : 0));
    out.u64(raw);
    out.u64(stored);
  }

  Result<Vector<uint8_t>> assemble(const Stored &names, const Stored &blocks)
  {
    constexpr uint64_t kChunkHeaderBytes = 24;
    uint64_t data = 4;
    for (const Stored &s : blobs_) {
      data += kBlobBytes + uint64_t(s.bytes.size());
    }
    const uint64_t total = kHeaderBytes + 3 * kChunkHeaderBytes + uint64_t(names.bytes.size()) +
                           uint64_t(blocks.bytes.size()) + data + 8;
    if (total > SIZE_MAX / 4) {
      return Status::error(StatusCode::OutOfRange, "the library is too large to save");
    }
    CDCC_TRY(probeMemory(total + total / 2));
    BytesWriter out(SIZE_MAX / 2);
    out.raw(kMagic);
    out.u32(kVersion);
    out.u32(kMinReader);
    out.u64(total);
    out.u32(3);
    chunkHeader(out, kTagName, names.lz4, names.raw, uint64_t(names.bytes.size()));
    put(out, names.bytes);
    chunkHeader(out, kTagBlocks, blocks.lz4, blocks.raw, uint64_t(blocks.bytes.size()));
    put(out, blocks.bytes);
    chunkHeader(out, kTagData, false, data, data);
    out.u32(uint32_t(blobs_.size()));
    for (Stored &s : blobs_) {
      out.u8(s.lz4 ? 1 : 0);
      out.u64(s.raw);
      out.u64(uint64_t(s.bytes.size()));
      put(out, s.bytes);
      s.bytes.clear_and_contract(); /* it is in the file now */
    }
    const std::string_view all(reinterpret_cast<const char *>(out.data().data()),
                               size_t(out.data().size()));
    out.u64(fnv1a64(all));
    CDCC_TRY(out.status());
    return out.take();
  }

  const Library &lib_;
  Runtime &rt_;
  const ClientRegistry &clients_;
  const BlockTypeRegistry &types_;
  const AttachmentTypeRegistry &attachments_;
  SaveReport &report_;
  Names names_;
  Vector<Item> items_;
  Vector<Stored> blobs_;
};

} // namespace

} // namespace detail::libio

Result<Vector<uint8_t>> saveLibrary(const Library &lib, SaveReport *report)
{
  if (lib.busy()) {
    return Status::error(StatusCode::Busy, "saveLibrary: the library is busy");
  }
  if (lib.runtime().hasStickyError()) {
    return Status::error(StatusCode::FailedPrecondition,
                         "saveLibrary: a sticky error is pending; take it first");
  }
  SaveReport own;
  SaveReport &to = report ? *report : own;
  to = SaveReport();
  return detail::libio::Saver(lib, to).run();
}

} // namespace coredcc
