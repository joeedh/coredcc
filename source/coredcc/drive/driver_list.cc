#include "coredcc/drive/driver_list.h"

#include "coredcc/api/struct.h"
#include "coredcc/lib/provider.h"
#include "coredcc/lib/registry.h"
#include "coredcc/scene/object.h"

#include "litestl/binding/binding.h"
#include "litestl/util/alloc.h"
#include "litestl/util/map.h"

#include <algorithm>
#include <string>

namespace coredcc {

using litestl::util::Vector;
namespace lbt = litestl::binding::types;

namespace {

constexpr uint32_t kDriverListPayload = 1;
constexpr const char *kGraphs = "graphs";

} // namespace

/* ---- the list ---- */

DriverList *DriverList::of(const Library &lib, BlockHandle h)
{
  const Datablock *b = lib.get(h);
  return b && b->type() == TypeId::of(kDriverListType)
             ? static_cast<DriverList *>(lib.data(h))
             : nullptr;
}

BlockHandle DriverList::find(const Library &lib)
{
  /* Not by the Library's order, which slot reuse and loading change: a Root list (the
   * document's own) before others (appended, duplicated), then the least id. */
  const ClientRegistry &clients = lib.runtime().ext<ClientRegistry>();
  BlockHandle found;
  bool foundRoot = false;
  BlockId foundId;
  for (BlockHandle h : allLists(lib)) {
    const Datablock &b = *lib.get(h);
    if (b.state() != BlockState::Live || !lib.data(h)) {
      continue;
    }
    const bool root = b.hasFlag(BlockFlags::Root);
    const BlockId id = clients.canonical(b.id());
    if (found.isNull() || (root && !foundRoot) || (root == foundRoot && id < foundId)) {
      found = h;
      foundRoot = root;
      foundId = id;
    }
  }
  return found;
}

Vector<BlockHandle> DriverList::allLists(const Library &lib)
{
  const TypeId type = TypeId::of(kDriverListType);
  const ClientRegistry &clients = lib.runtime().ext<ClientRegistry>();
  Vector<BlockHandle> out;
  lib.forEach([&](BlockHandle h, const Datablock &b) {
    if (b.type() == type) {
      out.append(h);
    }
  });
  std::sort(out.begin(), out.end(), [&](BlockHandle x, BlockHandle y) {
    return clients.canonical(lib.get(x)->id()) < clients.canonical(lib.get(y)->id());
  });
  return out;
}

const Vector<BlockHandle> &DriverList::graphs(const Library &lib) const
{
  const uint64_t clients = lib.runtime().ext<ClientRegistry>().generation();
  if (liveEpoch_ != lib.epoch() || liveGeneration_ != clients) {
    live_.clear();
    litestl::util::Map<BlockHandle, char> seen;
    for (const RefCell &c : cells_) {
      const BlockHandle h = lib.resolveRef(c);
      const Datablock *b = lib.get(h);
      if (b && b->state() == BlockState::Live && seen.add(h, 0)) {
        live_.append(h);
      }
    }
    liveEpoch_ = lib.epoch();
    liveGeneration_ = clients;
  }
  return live_;
}

Status DriverList::insert(Library &lib, const BlockId &id, int index)
{
  const ClientRegistry &clients = lib.runtime().ext<ClientRegistry>();
  for (const RefCell &c : cells_) {
    if (clients.canonical(c.target) == clients.canonical(id)) {
      return Status::error(StatusCode::AlreadyExists,
                           "the graph is in the list of drivers");
    }
  }
  return insertCell(lib, id, index);
}

Status DriverList::insertCell(Library &lib, const BlockId &id, int index)
{
  if (lib.find(id).isNull()) {
    return Status::error(StatusCode::NotFound, "DriverList::insert: no such block");
  }
  RefCell cell(RefKind::OptStrong);
  CDCC_TRY(lib.writeRef(cell, id));
  const int n = int(cells_.size());
  cells_.append(cell);
  if (index >= 0 && index < n) {
    for (int i = n; i > index; i--) {
      cells_[i] = cells_[i - 1];
    }
    cells_[index] = cell;
  }
  lib.noteRefsChanged();
  return Status();
}

Result<int> DriverList::remove(Library &lib, const BlockId &id)
{
  const ClientRegistry &clients = lib.runtime().ext<ClientRegistry>();
  int first = -1;
  for (int i = int(cells_.size()) - 1; i >= 0; i--) {
    if (clients.canonical(cells_[i].target) == clients.canonical(id)) {
      cells_.remove_at(i);
      first = i;
    }
  }
  if (first < 0) {
    return Status::error(StatusCode::NotFound, "the graph isn't in the list of drivers");
  }
  lib.noteRefsChanged();
  return first;
}

/* ---- files ---- */

void DriverList::write(BlockWriter &out) const
{
  Vector<BlockEntry> fields = fields_;
  Vector<RefCell> some;
  for (const RefCell &c : cells_) {
    if (!c.isNull()) {
      some.append(c);
    }
  }
  fields.append(detail::refField(
      kGraphs, std::span<const RefCell>(some.data(), size_t(some.size()))));
  out.entries(std::span<const BlockEntry>(fields.data(), size_t(fields.size())));
}

void DriverList::readFrom(BlockReader &in)
{
  const Library &lib = in.library();
  const ClientRegistry &clients = lib.runtime().ext<ClientRegistry>();
  detail::readFields(in, fields_, [&](const BlockEntry &e) {
    if (!detail::isRefField(e, kGraphs)) {
      return false;
    }
    /* Those that aren't null, once each: a save writes null for a deleted block. */
    litestl::util::Map<BlockId, int> seen;
    cells_.clear();
    for (const RefCell &c : e.refs) {
      if (!c.isNull() && seen.add(clients.canonical(c.target), 0)) {
        RefCell cell(RefKind::OptStrong);
        cell.target = c.target;
        cells_.append(cell);
      }
    }
    return true;
  });
}

DriverList *DriverList::duplicate() const
{
  /* Empty: a copy that listed the graphs too would drive their objects twice. */
  DriverList *d = litestl::alloc::New<DriverList>("coredcc::DriverList");
  d->fields_ = fields_;
  return d;
}

uint64_t DriverList::memSize() const
{
  return sizeof(DriverList) + uint64_t(cells_.size()) * sizeof(RefCell) +
         detail::fieldsMemSize(fields_);
}

namespace {

struct DriverListProvider {
  void *data(const Datablock &b) const
  {
    return b.clientData();
  }
  void foreachRef(const Datablock &b, RefVisitor &v) const
  {
    if (auto *d = static_cast<DriverList *>(b.clientData())) {
      d->foreachCell([&v](RefCell &cell) { v.visit(cell); });
    }
  }
  void destroy(const Datablock &b)
  {
    litestl::alloc::Delete(static_cast<DriverList *>(b.clientData()));
  }
  Status serialize(const Datablock &b, BlockWriter &out) const
  {
    if (!b.clientData()) {
      return Status::error(StatusCode::FailedPrecondition,
                           "a list of drivers without data");
    }
    static_cast<DriverList *>(b.clientData())->write(out);
    return Status();
  }
  Status deserialize(const Datablock &, BlockReader &in)
  {
    DriverList *d = litestl::alloc::New<DriverList>("coredcc::DriverList");
    d->readFrom(in);
    in.setData(d);
    return Status();
  }
  Status duplicate(const Datablock &src, BlockCopy &out)
  {
    if (!src.clientData()) {
      return Status::error(StatusCode::FailedPrecondition,
                           "a list of drivers without data");
    }
    out.setData(static_cast<DriverList *>(src.clientData())->duplicate());
    return Status();
  }
  uint64_t memSize(const Datablock &b) const
  {
    const auto *d = static_cast<DriverList *>(b.clientData());
    return d ? d->memSize() : 0;
  }
  static lbt::Struct<DriverListProvider> *defineBindings()
  {
    auto *st = new lbt::Struct<DriverListProvider>("coredcc::DriverListProvider",
                                                   sizeof(DriverListProvider));
    BIND_STRUCT_METHOD(st, data, MARGS("block"));
    BIND_STRUCT_METHOD(st, foreachRef, MARGS("block", "visitor"));
    BIND_STRUCT_METHOD(st, destroy, MARGS("block"));
    BIND_STRUCT_METHOD(st, serialize, MARGS("block", "out"));
    BIND_STRUCT_METHOD(st, deserialize, MARGS("block", "in"));
    BIND_STRUCT_METHOD(st, duplicate, MARGS("src", "out"));
    BIND_STRUCT_METHOD(st, memSize, MARGS("block"));
    return st;
  }
};

/* ---- the datapath ---- */

const DriverList &self(const PointerRef &p)
{
  return *static_cast<const DriverList *>(p.data);
}

/* A handle as a collection key, as the scene's collections have it. */
int64_t packed(BlockHandle h)
{
  return (int64_t(h.generation & 0x7fffffffu) << 32) | int64_t(h.index);
}

/* The listed graphs, read-only: the commands change them. */
CollectionAccessor graphsOf()
{
  CollectionAccessor c;
  c.itemStruct = StructId::of("coredcc.GraphDef");
  c.length = [](const PointerRef &p, Context &ctx) {
    return ctx.library() ? int64_t(self(p).graphs(*ctx.library()).size()) : int64_t(0);
  };
  c.at = [](const PointerRef &p, Context &ctx, int64_t i) {
    if (!ctx.library()) {
      return PointerRef();
    }
    const Vector<BlockHandle> &l = self(p).graphs(*ctx.library());
    return i >= 0 && i < int64_t(l.size()) ? detail::blockRoot(ctx, l[int(i)])
                                           : PointerRef();
  };
  c.lookup = [](const PointerRef &p, Context &ctx, const KeySegment &key) {
    const Library *lib = ctx.library();
    if (!lib) {
      return PointerRef();
    }
    for (BlockHandle h : self(p).graphs(*lib)) {
      const bool match = key.kind == KeySegment::Kind::Handle
                             ? packed(h) == key.handle
                             : lib->get(h)->name() == key.text;
      if (match) {
        return detail::blockRoot(ctx, h);
      }
    }
    return PointerRef();
  };
  c.keyOf = [](const PointerRef &, Context &ctx, const PointerRef &item) {
    return KeySegment::ofHandle(
        ctx.library() ? packed(ctx.library()->find(item.owner.block)) : 0);
  };
  return c;
}

ApiStruct listStruct()
{
  ApiStruct st(kDriverListType);
  st.uiName("Drivers").description("The graphs that drive the document's objects.");
  st.addCollection(
      PropertyDef::collectionProp("graphs", StructId::of("coredcc.GraphDef")),
      graphsOf());
  return st;
}

} // namespace

Result<BlockHandle> newDriverList(Library &lib)
{
  DriverList *d = litestl::alloc::New<DriverList>("coredcc::DriverList");
  Result<BlockHandle> h = lib.create(TypeId::of(kDriverListType), "Drivers", d);
  if (!h.ok()) {
    litestl::alloc::Delete(d);
    return h;
  }
  lib.setFlag(*h, BlockFlags::Root, true);
  return h;
}

Vector<BlockHandle> DriverList::allGraphs(const Library &lib)
{
  Vector<BlockHandle> out;
  litestl::util::Map<BlockHandle, char> seen;
  for (BlockHandle h : allLists(lib)) {
    const DriverList *l =
        lib.get(h)->state() == BlockState::Live ? DriverList::of(lib, h) : nullptr;
    for (BlockHandle g : l ? l->graphs(lib) : Vector<BlockHandle>()) {
      if (seen.add(g, 0)) {
        out.append(g);
      }
    }
  }
  return out;
}

void addDriverGraphsMember(ApiStruct &root)
{
  CollectionAccessor c;
  c.itemStruct = StructId::of("coredcc.GraphDef");
  c.length = [](const PointerRef &, Context &ctx) {
    return ctx.library() ? int64_t(DriverList::allGraphs(*ctx.library()).size())
                         : int64_t(0);
  };
  c.at = [](const PointerRef &, Context &ctx, int64_t i) {
    if (!ctx.library()) {
      return PointerRef();
    }
    const Vector<BlockHandle> l = DriverList::allGraphs(*ctx.library());
    return i >= 0 && i < int64_t(l.size()) ? detail::blockRoot(ctx, l[int(i)])
                                           : PointerRef();
  };
  c.lookup = [](const PointerRef &, Context &ctx, const KeySegment &key) {
    const Library *lib = ctx.library();
    if (!lib) {
      return PointerRef();
    }
    for (BlockHandle h : DriverList::allGraphs(*lib)) {
      const bool match = key.kind == KeySegment::Kind::Handle
                             ? packed(h) == key.handle
                             : lib->get(h)->name() == key.text;
      if (match) {
        return detail::blockRoot(ctx, h);
      }
    }
    return PointerRef();
  };
  c.keyOf = [](const PointerRef &, Context &ctx, const PointerRef &item) {
    return KeySegment::ofHandle(
        ctx.library() ? packed(ctx.library()->find(item.owner.block)) : 0);
  };
  root.addCollection(
      PropertyDef::collectionProp("driverGraphs", StructId::of("coredcc.GraphDef")),
      std::move(c));
}

namespace detail {

Status registerDriverListType(Runtime &rt)
{
  if (rt.ext<BlockTypeRegistry>().find(TypeId::of(kDriverListType))) {
    return Status();
  }
  CDCC_TRY(rt.ext<ApiRegistry>().add(listStruct()).status());
  const std::string impl = std::string(kDriverListType) + "#provider";
  CDCC_TRY((registerImpl<BlockProvider, DriverListProvider>(rt, impl)));
  Result<Owned<BlockProvider>> provider = own<BlockProvider>(
      rt, impl, litestl::alloc::New<DriverListProvider>("coredcc::DriverListProvider"));
  if (!provider.ok()) {
    return provider.status();
  }
  BlockTypeInfo info;
  info.name = kDriverListType;
  info.client = ClientId::of(kCoredccClient);
  info.dataStruct = StructId::of(kDriverListType);
  info.payloadVersion = kDriverListPayload;
  info.storage = BlockStorage::Payload;
  return rt.ext<BlockTypeRegistry>().add(std::move(info), std::move(*provider)).status();
}

} // namespace detail

} // namespace coredcc
