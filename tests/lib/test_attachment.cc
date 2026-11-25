/* Attachment types: the registry's rules, and attachments' references and destruction. */
#include "coredcc/lib/attachment.h"

#include "coredcc/lib/payload_access.h"

#include "thing_fixture.h"

test_init;

using namespace coredcc;
using namespace coredcc_test;

namespace {

struct Note {
  int value = 0;
  RefCell about{RefKind::Strong};
};

struct NoteHost {
  Runtime *rt = nullptr;
  int destroyed = 0;
  bool sawRef = false;
  bool failForeach = false;
};

struct NoteProvider {
  NoteHost *host = nullptr;
  void destroy(void *data)
  {
    host->destroyed++;
    Note *n = static_cast<Note *>(data);
    host->sawRef = !n->about.isNull();
    litestl::alloc::Delete(n);
  }
  void *create(const Datablock &)
  {
    return litestl::alloc::New<Note>("Note");
  }
  void *copy(const Datablock &, const void *data, const Datablock &)
  {
    return litestl::alloc::New<Note>("Note", *static_cast<const Note *>(data));
  }
  Status serialize(const Datablock &, const void *data, BlockWriter &out) const
  {
    const Note *n = static_cast<const Note *>(data);
    out.bytes().u32(uint32_t(n->value));
    out.ref(n->about);
    return Status();
  }
  Status deserialize(const Datablock &, BlockReader &in)
  {
    Note *n = litestl::alloc::New<Note>("Note");
    n->value = int(in.bytes().u32());
    in.ref(n->about);
    in.setData(n);
    return Status();
  }
  void foreachRef(const Datablock &, const void *data, RefVisitor &v) const
  {
    if (host->failForeach) {
      host->rt->setStickyError(
          Status::error(StatusCode::Internal, "note foreachRef threw"));
      return;
    }
    v.visit(const_cast<Note *>(static_cast<const Note *>(data))->about);
  }
  static lbt::Struct<NoteProvider> *defineBindings()
  {
    auto *st = new lbt::Struct<NoteProvider>("test::NoteProvider", sizeof(NoteProvider));
    BIND_STRUCT_METHOD(st, destroy, MARGS("data"));
    BIND_STRUCT_METHOD(st, create, MARGS("block"));
    BIND_STRUCT_METHOD(st, copy, MARGS("from", "data", "to"));
    BIND_STRUCT_METHOD(st, serialize, MARGS("block", "data", "out"));
    BIND_STRUCT_METHOD(st, deserialize, MARGS("block", "in"));
    BIND_STRUCT_METHOD(st, foreachRef, MARGS("block", "data", "visitor"));
    return st;
  }
};

/* The least a provider can be, or with one optional slot. */
enum class Part { None, Serialize, Deserialize, Copy };
template <Part P> struct PartProvider {
  void destroy(void *)
  {
  }
  Status serialize(const Datablock &, const void *, BlockWriter &) const
  {
    return Status();
  }
  Status deserialize(const Datablock &, BlockReader &)
  {
    return Status();
  }
  void *copy(const Datablock &, const void *, const Datablock &)
  {
    return nullptr;
  }
  static lbt::Struct<PartProvider> *defineBindings()
  {
    static const std::string name = "test::Part" + std::to_string(int(P));
    auto *st = new lbt::Struct<PartProvider>(name.c_str(), sizeof(PartProvider));
    BIND_STRUCT_METHOD(st, destroy, MARGS("data"));
    if constexpr (P == Part::Serialize) {
      BIND_STRUCT_METHOD(st, serialize, MARGS("block", "data", "out"));
    }
    if constexpr (P == Part::Deserialize) {
      BIND_STRUCT_METHOD(st, deserialize, MARGS("block", "in"));
    }
    if constexpr (P == Part::Copy) {
      BIND_STRUCT_METHOD(st, copy, MARGS("from", "data", "to"));
    }
    return st;
  }
};

const TypeId kNote = TypeId::of("test.Note");

template <class P>
Result<TypeId> addAttachment(Runtime &rt,
                             const char *name,
                             P *provider,
                             DuplicatePolicy policy = DuplicatePolicy::Drop)
{
  const std::string impl = std::string(name) + "#provider";
  test_assert((registerImpl<AttachmentProvider, P>(rt, impl.c_str()).ok()));
  Result<Owned<AttachmentProvider>> owned = own<AttachmentProvider>(rt, impl, provider);
  test_assert(owned.ok());
  AttachmentTypeInfo info;
  info.name = name;
  info.onDuplicate = policy;
  return rt.ext<AttachmentTypeRegistry>().add(info, std::move(owned).value());
}

struct World {
  ThingHost things;
  NoteHost notes;
  Runtime rt;
  Library lib{&rt};
  World()
  {
    registerThing(rt, things);
    notes.rt = &rt;
    NoteProvider *p = litestl::alloc::New<NoteProvider>("provider");
    p->host = &notes;
    Result<TypeId> id = addAttachment(rt, "test.Note", p, DuplicatePolicy::Copy);
    test_assert(id.ok() && *id == kNote);
  }
  AttachmentProvider provider()
  {
    return rt.ext<AttachmentTypeRegistry>().find(kNote)->provider.get();
  }
};

} // namespace

static void test_registry_rules()
{
  Runtime rt;
  const auto add =
      [&](auto *p, const char *name, DuplicatePolicy policy = DuplicatePolicy::Drop) {
        return addAttachment(rt, name, p, policy).status();
      };
  test_assert(hasMsg(add(litestl::alloc::New<PartProvider<Part::Serialize>>("p"), "t.1"),
                     "both or neither"));
  test_assert(
      hasMsg(add(litestl::alloc::New<PartProvider<Part::Deserialize>>("p"), "t.2"),
             "both or neither"));
  test_assert(hasMsg(add(litestl::alloc::New<PartProvider<Part::None>>("p"),
                         "t.3",
                         DuplicatePolicy::Copy),
                     "no copy"));
  test_assert(add(litestl::alloc::New<PartProvider<Part::Copy>>("p"),
                  "t.4",
                  DuplicatePolicy::Copy)
                  .ok());
  test_assert(add(litestl::alloc::New<PartProvider<Part::None>>("p"), "t.5").ok());
  AttachmentTypeRegistry &reg = rt.ext<AttachmentTypeRegistry>();
  AttachmentTypeInfo info;
  info.name = "t.6";
  test_assert(hasMsg(reg.add(info, Owned<AttachmentProvider>()).status(), "no provider"));
  info.name = "t.5";
  Result<Owned<AttachmentProvider>> p = own<AttachmentProvider>(
      rt, "t.5#provider", litestl::alloc::New<PartProvider<Part::None>>("p"));
  test_assert(p.ok() && reg.add(info, std::move(p).value()).status().code() ==
                            StatusCode::AlreadyExists);
  /* A block type's name, and the other way round. */
  ThingHost things;
  registerThing(rt, things);
  info.name = "test.Thing";
  Result<Owned<AttachmentProvider>> ap = own<AttachmentProvider>(
      rt, "t.5#provider", litestl::alloc::New<PartProvider<Part::None>>("p"));
  test_assert(ap.ok() && hasMsg(reg.add(info, std::move(ap).value()).status(),
                                "a block type has this name"));
  BlockTypeInfo block;
  block.name = "t.5";
  block.client = ClientId::of(kCoredccClient);
  Result<Owned<BlockProvider>> bp = own<BlockProvider>(
      rt, "test.Thing#provider", litestl::alloc::New<ThingProvider>("provider"));
  test_assert(
      bp.ok() &&
      hasMsg(rt.ext<BlockTypeRegistry>().add(block, std::move(bp).value()).status(),
             "an attachment type has this name"));
  test_assert(!reg.find(TypeId::of("t.1")) && reg.find(TypeId::of("t.5")) != nullptr);
  const AttachmentProvider part = reg.find(TypeId::of("t.5"))->provider.get();
  test_assert(!part.hasCreate() && !part.hasCopy() && !part.hasSerialize() &&
              !part.hasDeserialize() && !part.hasForeachRef());
  test_assert(rt.validateAll().ok());
}

static void test_attach()
{
  World w;
  const BlockHandle a = makeThing(w.lib, "a"), target = makeThing(w.lib, "target");
  AttachmentProvider p = w.provider();
  Note *n = static_cast<Note *>(p.create(*w.lib.get(a)));
  Status s = w.lib.attach(a, TypeId::of("test.Nope"), n);
  test_assert(s.code() == StatusCode::NotFound && hasMsg(s, "not a registered"));
  test_assert(w.lib.attach(BlockHandle(), kNote, n).code() == StatusCode::NotFound);
  test_assert(w.lib.attach(a, kNote, n).ok() && w.lib.attachment(a, kNote) == n);
  int listed = 0;
  w.lib.forEachAttachment(
      a, [&](TypeId t, void *d) { listed += t == kNote && d == n ? 1 : 10; });
  test_assert(listed == 1);
  test_assert(w.lib.attach(a, kNote, n).code() == StatusCode::AlreadyExists);

  /* Its references count: relations, marking, remapping through foreachRef. */
  test_assert(w.lib.writeRef(n->about, w.lib.get(target)->id()).ok());
  test_assert(w.lib.usersOf(target).size() == 1);
  int visited = 0, data = 0;
  test_assert(
      w.lib.foreachRef(a, [&](RefCell &c) { visited += &c == &n->about ? 1 : 0; }).ok());
  test_assert(w.lib.foreachDataRef(a, [&](RefCell &) { data++; }).ok());
  test_assert(visited == 1 && data == 3);
  w.lib.setFlag(a, BlockFlags::Root, true);
  test_assert(*w.lib.collect() == 0 && w.lib.get(target));

  /* A failing foreachRef aborts the collection. */
  w.notes.failForeach = true;
  w.lib.noteRefsChanged();
  test_assert(hasMsg(w.lib.collect().status(), "note foreachRef threw"));
  test_assert(!w.lib.foreachRef(a, [](RefCell &) {}).ok());
  w.rt.takeStickyError().ignore();
  w.notes.failForeach = false;

  /* attach and detach change what query() answers: the epoch moves. */
  uint64_t epoch = w.lib.epoch();
  Result<void *> d = w.lib.detach(a, kNote);
  test_assert(w.lib.epoch() > epoch);
  epoch = w.lib.epoch();
  test_assert(!w.lib.detach(a, kNote).ok() && w.lib.epoch() == epoch);
  test_assert(d.ok() && *d == n && !w.lib.attachment(a, kNote) && w.notes.destroyed == 0);
  test_assert(w.lib.usersOf(target).size() == 0);
  test_assert(w.lib.attach(a, kNote, n).ok() && w.lib.epoch() > epoch);
  epoch = w.lib.epoch();
  test_assert(!w.lib.attach(a, kNote, n).ok() && w.lib.epoch() == epoch);
  w.lib.setFlag(a, BlockFlags::Root, false);
  test_assert(*w.lib.collect() == 2 && w.notes.destroyed == 1);
  test_assert(!w.notes.sawRef); /* severed before anything was destroyed */
  test_assert(w.lib.validate().ok() && w.rt.validateAll().ok());

  /* ~Library destroys them too. */
  {
    Library other(&w.rt);
    const BlockHandle b = makeThing(other, "b");
    test_assert(other.attach(b, kNote, w.provider().create(*other.get(b))).ok());
  }
  test_assert(w.notes.destroyed == 2);
}

static void test_provider_slots()
{
  World w;
  AttachmentProvider p = w.provider();
  test_assert(p.hasCreate() && p.hasCopy() && p.hasSerialize() && p.hasDeserialize() &&
              p.hasForeachRef());
  Note n;
  n.value = 42;
  n.about.target = BlockId{ClientId::of(kCoredccClient), {1, 2}};
  const Datablock &block = *w.lib.get(makeThing(w.lib, "block"));
  Note *copy = static_cast<Note *>(p.copy(block, &n, block));
  test_assert(copy->value == 42 && copy->about.target == n.about.target);
  p.destroy(copy);
  BlockWriter out(w.lib);
  test_assert(p.serialize(block, &n, out).ok());
  Result<BlockPayload> payload = detail::PayloadAccess::take(out, 1);
  test_assert(payload.ok() && payload->refs.size() == 1);
  BlockReader in(w.lib, *payload);
  test_assert(p.deserialize(block, in).ok() && in.bytes().ok() && in.bytes().atEnd());
  Note *back = static_cast<Note *>(detail::PayloadAccess::data(in));
  test_assert(back && back->value == 42 && back->about.target == n.about.target);
  p.destroy(back);
  test_assert(w.notes.destroyed == 2);
}

/* The same through a foreign dispatcher. */
struct FakeHost : ForeignDispatcher {
  std::string log;
  Status call(const IfaceTable &table, void *, int slot, void **args, void *ret) override
  {
    const char *name = table.ifaceDesc->methods[slot]->name.c_str();
    log += std::string(name) + ";";
    const auto pointer = [&](int i) { return *static_cast<Note **>(args[i]); };
    if (strcmp(name, "create") == 0) {
      new (ret) void *(litestl::alloc::New<Note>("Note"));
    } else if (strcmp(name, "copy") == 0) {
      new (ret) void *(litestl::alloc::New<Note>("Note", *pointer(1)));
    } else if (strcmp(name, "destroy") == 0) {
      litestl::alloc::Delete(pointer(0));
    } else if (strcmp(name, "serialize") == 0) {
      static_cast<BlockWriter *>(args[2])->bytes().u32(uint32_t(pointer(1)->value));
      new (ret) Status();
    } else if (strcmp(name, "deserialize") == 0) {
      BlockReader &in = *static_cast<BlockReader *>(args[1]);
      Note *n = litestl::alloc::New<Note>("Note");
      n->value = int(in.bytes().u32());
      in.setData(n);
      new (ret) Status();
    } else if (strcmp(name, "onWeakTargetLost") == 0) {
      test_assert(pointer(1)->about.target ==
                  static_cast<const Datablock *>(args[2])->id());
      log += static_cast<const Datablock *>(args[0])->name().c_str();
    } else if (strcmp(name, "foreachRef") == 0) {
      static_cast<RefVisitor *>(args[2])->visit(pointer(1)->about);
    }
    return Status();
  }
};

static void test_foreign()
{
  ThingHost things;
  Runtime rt;
  registerThing(rt, things);
  FakeHost host;
  const char *slots[] = {"destroy",
                         "create",
                         "copy",
                         "serialize",
                         "deserialize",
                         "foreachRef",
                         "onWeakTargetLost"};
  test_assert(
      registerForeign<AttachmentProvider>(rt, "test.Js#provider", &host, nullptr, slots)
          .ok());
  int jsObject = 1;
  AttachmentProvider p =
      query<AttachmentProvider>(rt, TypeId::of("test.Js#provider"), &jsObject);
  AttachmentTypeInfo info;
  info.name = "test.Js";
  info.onDuplicate = DuplicatePolicy::Copy;
  Result<TypeId> type =
      rt.ext<AttachmentTypeRegistry>().add(info, Owned<AttachmentProvider>(p, nullptr));
  test_assert(type.ok());
  Library lib(&rt);
  const BlockHandle a = makeThing(lib, "a");
  Note *n = static_cast<Note *>(p.create(*lib.get(a)));
  n->value = 9;
  test_assert(lib.attach(a, *type, n).ok());
  n->about.target = lib.get(a)->id();
  lib.noteRefsChanged();
  test_assert(lib.usersOf(a).size() == 1);
  BlockWriter out(lib);
  test_assert(p.serialize(*lib.get(a), n, out).ok());
  Result<BlockPayload> payload = detail::PayloadAccess::take(out, 1);
  test_assert(payload.ok());
  BlockReader in(lib, *payload);
  test_assert(p.deserialize(*lib.get(a), in).ok() &&
              static_cast<Note *>(detail::PayloadAccess::data(in))->value == 9);
  p.destroy(detail::PayloadAccess::data(in));
  Note *copy = static_cast<Note *>(p.copy(*lib.get(a), n, *lib.get(a)));
  test_assert(copy && copy->value == 9);
  p.destroy(copy);
  n->about.kind = RefKind::Weak;
  n->about.target = lib.get(makeThing(lib, "lost"))->id();
  lib.noteRefsChanged();
  lib.setFlag(a, BlockFlags::Root, true);
  test_assert(*lib.collect() == 1 &&
              host.log.find("onWeakTargetLost;a") != std::string::npos);
  lib.setFlag(a, BlockFlags::Root, false);
  host.log.clear();
  test_assert(*lib.collect() == 1);
  test_assert(host.log.find("destroy;") != std::string::npos && !rt.hasStickyError());
}

int main()
{
  test_registry_rules();
  test_attach();
  test_provider_slots();
  test_foreign();
  return test_end();
}
