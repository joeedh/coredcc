/* Attach and detach as undo steps, attachments on datapaths, and block undo by id. */
#include "command_fixture.h"

#include "coredcc/command/attachment_commands.h"
#include "coredcc/lib/attachment.h"

test_init;

namespace {

struct Notes {
  double x = 0;
  RefCell about{RefKind::Strong};
};

struct NotesHost {
  int alive = 0;
  bool noData = false;
  bool throws = false;
  Runtime *rt = nullptr;
};

template <bool Create> struct NotesProvider {
  NotesHost *host = nullptr;
  void destroy(void *data)
  {
    host->alive--;
    litestl::alloc::Delete(static_cast<Notes *>(data));
  }
  void *create(const Datablock &)
  {
    if (host->throws) {
      host->rt->setStickyError(Status::error(StatusCode::Internal, "create threw"));
    }
    if (host->noData) {
      return nullptr;
    }
    host->alive++;
    return litestl::alloc::New<Notes>("Notes");
  }
  void forEachRef(const Datablock &, const void *data, RefVisitor &v) const
  {
    if (host->throws) {
      host->rt->setStickyError(Status::error(StatusCode::Internal, "forEachRef threw"));
      return;
    }
    v.visit(const_cast<Notes *>(static_cast<const Notes *>(data))->about);
  }
  static lbt::Struct<NotesProvider> *defineBindings()
  {
    auto *st = new lbt::Struct<NotesProvider>(Create ? "test::Notes" : "test::Bare",
                                              sizeof(NotesProvider));
    BIND_STRUCT_METHOD(st, destroy, MARGS("data"));
    if constexpr (Create) {
      BIND_STRUCT_METHOD(st, create, MARGS("block"));
      BIND_STRUCT_METHOD(st, forEachRef, MARGS("block", "data", "visitor"));
    }
    return st;
  }
};

const StructId kNotesStruct = StructId::of("Notes");
const TypeId kNotes = TypeId::of("acme.notes");
const TypeId kBare = TypeId::of("acme.bare");

template <bool Create>
void addType(Runtime &rt, NotesHost &host, const char *name, StructId dataStruct)
{
  const std::string impl = std::string(name) + "#provider";
  using P = NotesProvider<Create>;
  test_assert((registerImpl<AttachmentProvider, P>(rt, impl).ok()));
  P *p = litestl::alloc::New<P>("provider");
  p->host = &host;
  Result<Owned<AttachmentProvider>> owned = own<AttachmentProvider>(rt, impl, p);
  AttachmentTypeInfo info;
  info.name = name;
  info.dataStruct = dataStruct;
  test_assert(owned.ok() &&
              rt.ext<AttachmentTypeRegistry>().add(info, std::move(owned).value()).ok());
}

struct AttSetup {
  NotesHost host;
  Setup s;
  AttSetup()
  {
    Runtime &rt = s.w.rt;
    host.rt = &rt;
    addType<true>(rt, host, "acme.notes", kNotesStruct);
    addType<false>(rt, host, "acme.bare", StructId());
    ApiStruct notes("Notes");
    notes.addField(PropertyDef::floatProp("x"), &Notes::x);
    test_assert(rt.ext<ApiRegistry>().add(std::move(notes)).ok());
  }
  Library &lib()
  {
    return s.w.lib;
  }
  BlockHandle block()
  {
    return s.w.scene.objects[0];
  }
  Notes *notes()
  {
    return static_cast<Notes *>(lib().attachment(block(), kNotes));
  }
  Status run(const char *command, const char *type = "acme.notes")
  {
    StableRef r;
    r.owner = OwnerKey::ofBlock(s.w.objectId(0));
    PropertyBag in = s.w.rt.ext<CommandRegistry>().find(command)->newInputs();
    test_assert(in.set("block", PropValue::ofPointer(r)).ok());
    test_assert(in.set("type", PropValue::ofString(type)).ok());
    return s.ex.run(s.ctx, command, std::move(in)).status();
  }
};

} // namespace

static void test_attach_command()
{
  AttSetup t;
  int heard = 0;
  ScopedConnection l = t.s.w.rt.ext<ChangeBus>().addListener([&](const OwnerKey *owner) {
    heard += owner && *owner == OwnerKey::ofBlock(t.s.w.objectId(0)) ? 1 : 0;
  });
  test_assert(t.run(kAttachBlockCommand).ok() && t.notes() && t.host.alive == 1 &&
              heard == 1);
  Notes *n = t.notes();
  test_assert(t.s.ex.undo(t.s.ctx).ok() && !t.notes() && t.host.alive == 1 && heard == 2);
  test_assert(t.s.ex.redo(t.s.ctx).ok() && t.notes() == n && heard == 3);
  test_assert(t.run(kAttachBlockCommand).code() == StatusCode::AlreadyExists &&
              t.host.alive == 1);

  /* Undone and dropped: the command's data goes. */
  test_assert(t.s.ex.undo(t.s.ctx).ok());
  t.s.history.clear();
  test_assert(t.host.alive == 0 && t.lib().get(t.block())->pins() == 0);

  test_assert(
      hasMsg(t.run(kAttachBlockCommand, "acme.nope"), "no attachment type 'acme.nope'"));
  test_assert(t.run(kAttachBlockCommand, "acme.bare").code() == StatusCode::Unsupported);
  t.host.noData = true;
  test_assert(hasMsg(t.run(kAttachBlockCommand), "gave no data"));
  t.host.noData = false;
  t.host.throws = true;
  test_assert(hasMsg(t.run(kAttachBlockCommand), "create threw"));
  t.host.throws = false;
  test_assert(!t.notes() && t.host.alive == 0 && !t.s.w.rt.hasStickyError());
  test_assert(t.lib().validate().ok());
}

static void test_detach_command()
{
  AttSetup t;
  test_assert(t.run(kDetachBlockCommand).code() == StatusCode::NotFound);
  test_assert(t.run(kAttachBlockCommand).ok());
  Notes *n = t.notes();
  /* Something only the attachment references. */
  Result<BlockHandle> only =
      t.lib().create(kMeshType, "only", litestl::alloc::New<Mesh>("Mesh"));
  test_assert(only.ok() && t.lib().writeRef(n->about, t.lib().get(*only)->id()).ok());
  for (BlockHandle ob : t.s.w.scene.objects) {
    t.lib().setFlag(ob, BlockFlags::FakeUser, true);
  }
  t.host.throws = true;
  test_assert(hasMsg(t.run(kDetachBlockCommand), "forEachRef threw") && t.notes() == n);
  t.host.throws = false;
  test_assert(t.run(kDetachBlockCommand).ok() && !t.notes() && t.host.alive == 1);
  test_assert(t.lib().get(*only)->pins(PinKind::History) == 1);
  test_assert(*t.lib().collect() == 0);
  test_assert(t.s.ex.undo(t.s.ctx).ok() && t.notes() == n);
  test_assert(t.lib().get(*only)->pins() == 0);
  test_assert(t.s.ex.redo(t.s.ctx).ok());
  t.s.history.clear();
  test_assert(t.host.alive == 0 && *t.lib().collect() == 1 && !t.lib().get(*only));
  test_assert(t.lib().validate().ok() && !t.s.w.rt.hasStickyError());
}

static void test_attach_undo()
{
  AttSetup t;
  const auto make = [&]() {
    t.host.alive++;
    return litestl::alloc::New<Notes>("Notes");
  };
  Notes *n = make();
  test_assert(attachUndo(t.s.ctx, t.block(), kNotes, n).ok() && t.notes() == n);
  test_assert(t.s.history.size() == 1);
  /* A second one is refused, and destroyed. */
  test_assert(attachUndo(t.s.ctx, t.block(), kNotes, make()).code() ==
              StatusCode::AlreadyExists);
  test_assert(t.host.alive == 1 && t.notes() == n);
  test_assert(attachUndo(t.s.ctx, BlockHandle(), kNotes, make()).code() ==
              StatusCode::NotFound);
  test_assert(t.host.alive == 1);
  Context noEx(&t.s.w.rt, &t.lib(), PointerRef());
  test_assert(attachUndo(noEx, t.block(), kNotes, make()).code() ==
              StatusCode::FailedPrecondition);
  test_assert(t.host.alive == 1);
  Notes stray;
  test_assert(attachUndo(t.s.ctx, t.block(), TypeId::of("nope"), &stray).code() ==
              StatusCode::InvalidArgument); /* not destroyed: nobody knows how */
  test_assert(attachUndo(t.s.ctx, t.block(), kNotes, nullptr).code() ==
              StatusCode::InvalidArgument);
  test_assert(t.s.ex.undo(t.s.ctx).ok() && !t.notes());
  test_assert(t.s.ex.redo(t.s.ctx).ok() && t.notes() == n);
  t.s.history.clear();
  test_assert(t.host.alive == 1 && t.notes() == n); /* attached: the block's */
  /* A block attached by hand in between: the undo's detach gets an error, not a crash. */
  test_assert(attachUndo(t.s.ctx, t.s.w.meshes[1], kNotes, make()).ok());
  Result<void *> d = t.lib().detach(t.s.w.meshes[1], kNotes);
  test_assert(d.ok() && !t.s.ex.undo(t.s.ctx).ok());
  t.s.w.rt.takeStickyError().ignore();
  AttachmentProvider p =
      t.s.w.rt.ext<AttachmentTypeRegistry>().find(kNotes)->provider.get();
  p.destroy(*d);
  t.s.history.clear();
  test_assert(t.host.alive == 1);
}

static void test_paths()
{
  AttSetup t;
  const auto path = [](const char *rest) {
    return std::string("objects[\"Cube\"].attachments") + rest;
  };
  Context &ctx = t.s.ctx;
  test_assert(*getValue(ctx, path(".length")) == PropValue::ofInt(0));
  test_assert(t.run(kAttachBlockCommand).ok());
  Notes *bare = litestl::alloc::New<Notes>("Notes"); /* no data struct: not listed */
  t.host.alive++;
  test_assert(t.lib().attach(t.block(), kBare, bare).ok());
  test_assert(*getValue(ctx, path(".length")) == PropValue::ofInt(1));
  const std::string x = path("[\"acme.notes\"].x");
  test_assert(setValueUndo(ctx, x, PropValue::ofFloat(2)).ok() && t.notes()->x == 2);
  Result<ResolvedPath> rp = resolve(ctx, path("[0].x"));
  test_assert(rp.ok() && rp->canonical.owner == OwnerKey::ofBlock(t.s.w.objectId(0)));
  test_assert(rp.ok() && rp->canonical.key.size() == 3 &&
              strcmp(rp->canonical.key[1].text.c_str(), "acme.notes") == 0);
  test_assert(!resolve(ctx, path("[1].x")).ok() &&
              !resolve(ctx, path("[\"acme.bare\"]")).ok());
  test_assert(!resolve(ctx, path("[\"nope\"]")).ok());
  test_assert(t.s.ex.undo(ctx).ok() && t.notes()->x == 0);
  test_assert(t.s.ex.redo(ctx).ok() && t.notes()->x == 2);
  /* Undone past the attach and back: the same data, and the value undoes again. */
  test_assert(t.s.ex.undo(ctx).ok() && t.s.ex.undo(ctx).ok());
  test_assert(!getValue(ctx, x).ok());
  test_assert(t.s.ex.redo(ctx).ok() && t.s.ex.redo(ctx).ok() && t.notes()->x == 2);
}

/* A block whose record was replaced under its id is still the one undone. */
static void test_blocks_by_id()
{
  {
    Setup s;
    Library &lib = s.w.lib;
    Result<PropertyBag> made = invoke(s.ctx, "test.createMesh");
    const LocalId local{0, uint64_t(made->get("block")->asInt())};
    BlockHandle first;
    lib.forEach([&](BlockHandle h, const Datablock &b) {
      first = b.id().local.lo == local.lo ? h : first;
    });
    const BlockId id = lib.get(first)->id();
    const auto replace = [&](BlockHandle h) {
      test_assert(lib.markDeleted(h).ok() && lib.destroyFromHistory(h).ok());
      Result<BlockHandle> r =
          lib.reviveAs(kMeshType, id, "made", litestl::alloc::New<Mesh>("Mesh"));
      test_assert(r.ok());
      return *r;
    };
    const BlockHandle second = replace(first);
    test_assert(second != first);
    lib.pin(second, PinKind::History); /* another record's pin */
    test_assert(s.ex.undo(s.ctx).ok() &&
                lib.get(second)->state() == BlockState::InHistory);
    test_assert(s.ex.redo(s.ctx).ok() && lib.get(second)->state() == BlockState::Live);
    test_assert(s.ex.undo(s.ctx).ok());
    test_assert(lib.restoreFromHistory(second).ok()); /* brought back behind its back */
    s.history.clear();
    test_assert(lib.get(second) && lib.get(second)->pins(PinKind::History) == 1);
    test_assert(!s.w.rt.hasStickyError());
  }
  {
    Setup s;
    Library &lib = s.w.lib;
    test_assert(invoke(s.ctx, "test.createMesh").ok());
    const int size = lib.size();
    test_assert(s.ex.undo(s.ctx).ok());
    BlockHandle deleted;
    lib.forEach([&](BlockHandle h, const Datablock &b) {
      deleted = b.state() == BlockState::InHistory ? h : deleted;
    });
    test_assert(lib.get(deleted)->pins(PinKind::History) == 1);
    s.history.clear();
    test_assert(!lib.get(deleted) && lib.size() == size - 1);
  }
  {
    Setup s;
    Library &lib = s.w.lib;
    const BlockHandle first = s.w.meshes[0];
    const BlockId id = s.w.meshId(0);
    StableRef r;
    r.owner = OwnerKey::ofBlock(id);
    PropertyBag in = s.w.rt.ext<CommandRegistry>().find(kDeleteBlockCommand)->newInputs();
    test_assert(in.set("block", PropValue::ofPointer(r)).ok());
    test_assert(s.ex.run(s.ctx, kDeleteBlockCommand, std::move(in)).ok());
    test_assert(lib.get(first)->pins(PinKind::History) == 1);
    test_assert(lib.destroyFromHistory(first).ok());
    Result<BlockHandle> second =
        lib.reviveAs(kMeshType, id, "m", litestl::alloc::New<Mesh>("Mesh"));
    test_assert(second.ok() && lib.markDeleted(*second).ok());
    test_assert(s.ex.undo(s.ctx).ok() && lib.get(*second)->state() == BlockState::Live);
    test_assert(s.ex.redo(s.ctx).ok() &&
                lib.get(*second)->state() == BlockState::InHistory);
    s.history.clear();
    test_assert(!lib.get(*second) && lib.validate().ok() && !s.w.rt.hasStickyError());
  }
}

/* Created, then deleted by a later step: dropping the older step keeps the block. */
static void test_drop_oldest()
{
  Setup s;
  Library &lib = s.w.lib;
  Result<PropertyBag> made = invoke(s.ctx, "test.createMesh");
  BlockHandle h;
  lib.forEach([&](BlockHandle b, const Datablock &d) {
    h = d.id().local.lo == uint64_t(made->get("block")->asInt()) ? b : h;
  });
  StableRef r;
  r.owner = OwnerKey::ofBlock(lib.get(h)->id());
  PropertyBag in = s.w.rt.ext<CommandRegistry>().find(kDeleteBlockCommand)->newInputs();
  test_assert(in.set("block", PropValue::ofPointer(r)).ok());
  test_assert(s.ex.run(s.ctx, kDeleteBlockCommand, std::move(in)).ok() &&
              s.history.size() == 2);
  s.history.setMemoryBudget(1);
  test_assert(s.history.size() == 1 && lib.get(h));
  test_assert(s.ex.undo(s.ctx).ok() && lib.get(h) &&
              lib.get(h)->state() == BlockState::Live);
  test_assert(s.ex.redo(s.ctx).ok());

  /* A destroy that is refused reports through the sticky error. */
  s.w.rt.takeStickyError().ignore();
  test_assert(
      (registerImpl<BlockProvider, RigProvider>(s.w.rt, "test.Rig#provider").ok()));
  Result<Owned<BlockProvider>> owned = own<BlockProvider>(
      s.w.rt, "test.Rig#provider", litestl::alloc::New<RigProvider>("provider"));
  BlockTypeInfo info;
  info.name = "test.Rig";
  info.client = ClientId::of(kCoredccClient);
  test_assert(owned.ok() &&
              s.w.rt.ext<BlockTypeRegistry>().add(info, std::move(owned).value()).ok());
  Rig *rig = litestl::alloc::New<Rig>("Rig");
  Result<BlockHandle> rigH = lib.create(kRigType, "rig", rig);
  test_assert(rigH.ok() && lib.writeRef(rig->target, lib.get(h)->id()).ok());
  s.history.clear();
  test_assert(hasMsg(s.w.rt.takeStickyError(), "still holds a Strong ref") && lib.get(h));
}

int main()
{
  test_drop_oldest();
  test_attach_command();
  test_detach_command();
  test_attach_undo();
  test_paths();
  test_blocks_by_id();
  return test_end();
}
