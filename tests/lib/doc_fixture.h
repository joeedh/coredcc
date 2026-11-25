#pragma once

/* A block type with payloads, for the IO tests: an integer, refs, a list of entries and a
 * bag with the entries it couldn't read. DocHost makes it misbehave. */

#include "thing_fixture.h"

namespace coredcc_test {

using litestl::util::Vector;

struct Doc {
  int64_t number = 0;
  RefCell parent{RefKind::Weak};
  RefCell mesh{RefKind::Strong};
  Vector<BlockEntry> entries;
  PropertyBag bag;
  /** The bag's `target` property; the bag's value holds a copy of the id. */
  RefCell bagTarget{RefKind::OptStrong};
  Vector<BlockEntry> unread;
};

enum class DocFault {
  None,
  Error,          /* returns an error, having freed what it made */
  Sticky,         /* a sticky error, as a throwing script leaves */
  SkipRef,        /* serialize: a reference foreachRef reports isn't written */
  ExtraRef,       /* serialize: one more than foreachRef reports */
  WrongStrength,  /* serialize: a strong ref written as weak */
  LeaveBytes,     /* deserialize: stops early */
  NoData,         /* deserialize: forgets setData */
};

struct DocHost {
  Runtime *rt = nullptr;
  DocFault fault = DocFault::None;
  bool bagHasExtra = true;
  int destroyed = 0;
  uint32_t lastVersion = 0;
  litestl::util::function<void(BlockReader &)> onDeserialize;
  litestl::util::function<void(const Datablock &)> onDestroy;
  /** What rekey answers; by default, another id of the same client. */
  litestl::util::function<BlockId(const Datablock &)> onRekey;
  int duplicated = 0;
  litestl::util::function<void()> onDuplicate;
};

inline PropertyBag makeDocBag(const DocHost &host)
{
  PropertyBag bag;
  test_assert(bag.add(PropertyDef::floatProp("size").range(0, 10)).ok());
  test_assert(bag.add(PropertyDef::pointerProp("target", StructId::of("acme.Mesh"))).ok());
  if (host.bagHasExtra) {
    test_assert(bag.add(PropertyDef::stringProp("extra")).ok());
    test_assert(bag.add(PropertyDef::pointerProp("other", StructId::of("acme.Mesh"))).ok());
  }
  return bag;
}

inline PropValue pointerTo(BlockId id, const char *key = "verts")
{
  StableRef r;
  r.owner = OwnerKey::ofBlock(id);
  r.key.append(KeySegment::member(key));
  r.type = StructId::of("acme.Mesh");
  return PropValue::ofPointer(r);
}

inline BlockEntry entryOf(const char *name, PropValue v, RefKind kind = RefKind::OptStrong)
{
  BlockEntry e;
  e.name = name;
  if (const StableRef *r = v.tryPointer(); r && r->owner.kind == OwnerKey::Kind::Block) {
    RefCell cell(kind);
    cell.target = r->owner.block;
    e.refs.append(cell);
  }
  e.value = std::move(v);
  return e;
}

/* Without Extras, a provider has no duplicate and no rekey. */
template <bool Extras> struct DocProviderT {
  DocHost *host = nullptr;

  void *data(const Datablock &b) const
  {
    return b.clientData();
  }
  void foreachRef(const Datablock &b, RefVisitor &v) const
  {
    Doc *d = static_cast<Doc *>(b.clientData());
    if (!d) {
      return;
    }
    v.visit(d->parent);
    v.visit(d->mesh);
    for (BlockEntry &e : d->entries) {
      for (RefCell &c : e.refs) {
        v.visit(c);
      }
    }
    v.visit(d->bagTarget);
    for (BlockEntry &e : d->unread) {
      for (RefCell &c : e.refs) {
        v.visit(c);
      }
    }
  }
  void destroy(const Datablock &b)
  {
    host->destroyed++;
    if (host->onDestroy) {
      host->onDestroy(b);
    }
    if (b.clientData()) {
      litestl::alloc::Delete(static_cast<Doc *>(b.clientData()));
    }
  }
  Status serialize(const Datablock &b, BlockWriter &out) const
  {
    const Doc *d = static_cast<const Doc *>(b.clientData());
    if (host->fault == DocFault::Error) {
      return Status::error(StatusCode::Unsupported, "not today");
    }
    if (host->fault == DocFault::Sticky) {
      host->rt->setStickyError(Status::error(StatusCode::Internal, "serialize threw"));
      return Status();
    }
    out.bytes().i64(d->number);
    RefCell parent = d->parent, mesh = d->mesh;
    if (host->fault == DocFault::SkipRef) {
      parent.target = BlockId();
    }
    if (host->fault == DocFault::WrongStrength) {
      mesh.kind = RefKind::Weak;
    }
    out.ref(parent);
    out.ref(mesh);
    if (host->fault == DocFault::ExtraRef) {
      RefCell extra(RefKind::Weak);
      extra.target = b.id();
      out.ref(extra);
    }
    out.entries(std::span<const BlockEntry>(d->entries.data(), size_t(d->entries.size())));
    out.bag(d->bag, RefKind::OptStrong,
            std::span<const BlockEntry>(d->unread.data(), size_t(d->unread.size())));
    return Status();
  }
  Status deserialize(const Datablock &, BlockReader &in)
  {
    host->lastVersion = in.version();
    if (host->onDeserialize) {
      host->onDeserialize(in);
    }
    if (host->fault == DocFault::Error) {
      return Status::error(StatusCode::Unsupported, "not today");
    }
    if (host->fault == DocFault::Sticky) {
      host->rt->setStickyError(Status::error(StatusCode::Internal, "deserialize threw"));
      return Status();
    }
    if (host->fault == DocFault::NoData) {
      while (!in.bytes().atEnd()) {
        in.bytes().u8();
      }
      return Status();
    }
    Doc *d = litestl::alloc::New<Doc>("Doc");
    d->number = in.bytes().i64();
    in.setData(d); /* failures after this are the library's to clean up */
    if (host->fault == DocFault::LeaveBytes) {
      return Status();
    }
    in.ref(d->parent);
    in.ref(d->mesh);
    in.entries(d->entries);
    d->bag = makeDocBag(*host);
    in.bag(d->bag, d->unread);
    if (const StableRef *r = d->bag.get("target")->tryPointer()) {
      d->bagTarget.target = r->owner.block;
    }
    return Status();
  }
  Status duplicate(const Datablock &src, BlockCopy &out)
  {
    const Doc *d = static_cast<const Doc *>(src.clientData());
    if (!d) {
      return Status::error(StatusCode::FailedPrecondition, "no data");
    }
    host->duplicated++;
    if (host->onDuplicate) {
      host->onDuplicate();
    }
    if (host->fault == DocFault::Error) {
      return Status::error(StatusCode::Unsupported, "no copies today");
    }
    if (host->fault == DocFault::Sticky) {
      host->rt->setStickyError(Status::error(StatusCode::Internal, "duplicate threw"));
      return Status();
    }
    if (host->fault == DocFault::NoData) {
      return Status();
    }
    Doc *copy = litestl::alloc::New<Doc>("Doc");
    *copy = *d;
    out.setData(copy);
    return Status();
  }
  BlockId rekey(const Datablock &imported) const
  {
    if (host->onRekey) {
      return host->onRekey(imported);
    }
    BlockId id = imported.id();
    id.local.hi ^= 1;
    return id;
  }
  static lbt::Struct<DocProviderT> *defineBindings()
  {
    auto *st = new lbt::Struct<DocProviderT>(
        Extras ? "test::DocProvider" : "test::PlainDocProvider", sizeof(DocProviderT));
    BIND_STRUCT_METHOD(st, data, MARGS("block"));
    BIND_STRUCT_METHOD(st, foreachRef, MARGS("block", "visitor"));
    BIND_STRUCT_METHOD(st, destroy, MARGS("block"));
    BIND_STRUCT_METHOD(st, serialize, MARGS("block", "out"));
    BIND_STRUCT_METHOD(st, deserialize, MARGS("block", "in"));
    if constexpr (Extras) {
      BIND_STRUCT_METHOD(st, duplicate, MARGS("src", "out"));
      BIND_STRUCT_METHOD(st, rekey, MARGS("imported"));
    }
    return st;
  }
};
using DocProvider = DocProviderT<true>;

inline const TypeId kDoc = TypeId::of("test.Doc");

/** Any number of types can share a host. */
template <class Provider = DocProvider>
inline void registerDoc(Runtime &rt,
                        DocHost &host,
                        uint32_t version = 3,
                        const char *name = "test.Doc",
                        ClientId client = ClientId::of(kCoredccClient))
{
  host.rt = &rt;
  const std::string impl = std::string(name) + "#provider";
  test_assert((registerImpl<BlockProvider, Provider>(rt, impl).ok()));
  Provider *p = litestl::alloc::New<Provider>("provider");
  p->host = &host;
  Result<Owned<BlockProvider>> owned = own<BlockProvider>(rt, impl, p);
  test_assert(owned.ok());
  BlockTypeInfo info;
  info.name = name;
  info.client = client;
  info.payloadVersion = version;
  test_assert(rt.ext<BlockTypeRegistry>().add(info, std::move(owned).value()).ok());
}

inline Doc *doc(const Library &lib, BlockHandle h)
{
  return static_cast<Doc *>(lib.get(h)->clientData());
}

inline BlockHandle makeDoc(Library &lib, const DocHost &host, const char *name)
{
  Doc *d = litestl::alloc::New<Doc>("Doc");
  d->bag = makeDocBag(host);
  Result<BlockHandle> h = lib.create(kDoc, name, d);
  test_assert(h.ok());
  return h.ok() ? *h : BlockHandle();
}

/** A Doc record with no data, for readPayload. */
inline BlockHandle emptyDoc(Library &lib, const char *name)
{
  Result<BlockHandle> h = lib.create(kDoc, name, nullptr);
  test_assert(h.ok());
  return h.ok() ? *h : BlockHandle();
}

} // namespace coredcc_test
