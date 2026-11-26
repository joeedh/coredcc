/* Block commands (delete, purge, CreatedBlocks) and the undo strategies. */
#include "command_fixture.h"

#include <cmath>
#include <string>

test_init;

static void test_block_commands()
{
  Setup s;
  Library &lib = s.w.lib;
  Runtime &rt = s.w.rt;
  const int before = lib.size();

  /* Create, undo, redo: the block keeps its id; a discard while undone destroys it. */
  Result<PropertyBag> made = invoke(s.ctx, "test.createMesh");
  test_assert(made.ok() && lib.size() == before + 1);
  const uint64_t lo = uint64_t(made->get("block")->asInt());
  BlockHandle h;
  lib.forEach([&](BlockHandle bh, const Datablock &b) {
    if (b.id().local.lo == lo) {
      h = bh;
    }
  });
  const BlockId id = lib.get(h)->id();
  test_assert(s.ex.undo(s.ctx).ok() && lib.get(h)->state() == BlockState::InHistory);
  test_assert(lib.data(h) == nullptr);
  test_assert(s.ex.redo(s.ctx).ok() && lib.get(h)->state() == BlockState::Live);
  test_assert(lib.find(id) == h);
  test_assert(s.ex.undo(s.ctx).ok());
  test_assert(s.runAdd(1).ok()); /* truncates the undone create */
  test_assert(!lib.get(h) && lib.size() == before);

  /* Delete through an OptStrong ref: no cascade, and the object's `data` reads null. */
  Result<PropValue> mesh = getValue(s.ctx, "objects[0].data");
  PropertyBag in = rt.ext<CommandRegistry>().find(kDeleteBlockCommand)->newInputs();
  test_assert(in.set("block", *mesh).ok());
  Result<PropertyBag> del = s.ex.run(s.ctx, kDeleteBlockCommand, std::move(in));
  test_assert(del.ok() && *del->get("count") == PropValue::ofInt(1));
  test_assert(*getValue(s.ctx, "objects[0].data") == PropValue::nullPointer());
  test_assert(s.ex.undo(s.ctx).ok());
  test_assert(getValue(s.ctx, "objects[0].data.radius").ok());

  /* A strong Ref cascades: preview, refuse without cascade, delete both, undo both. */
  test_assert((registerImpl<BlockProvider, RigProvider>(rt, "test.Rig#provider").ok()));
  Result<Owned<BlockProvider>> p = own<BlockProvider>(
      rt, "test.Rig#provider", litestl::alloc::New<RigProvider>("provider"));
  BlockTypeInfo info;
  info.name = "test.Rig";
  info.client = ClientId::of(kCoredccClient);
  test_assert(p.ok() && rt.ext<BlockTypeRegistry>().add(info, std::move(p).value()).ok());
  Rig *rig = litestl::alloc::New<Rig>("Rig");
  Result<BlockHandle> rigH = lib.create(kRigType, "rig", rig);
  test_assert(rigH.ok() && lib.writeRef(rig->target, s.w.meshId(0)).ok());
  Rig *rig2 = litestl::alloc::New<Rig>("Rig");
  Result<BlockHandle> rig2H = lib.create(kRigType, "rig2", rig2);
  test_assert(rig2H.ok() && lib.writeRef(rig2->target, lib.get(*rigH)->id()).ok());
  litestl::util::Vector<BlockHandle> preview = deletePreview(lib, s.w.meshes[0]);
  test_assert(preview.size() == 3 && preview[0] == s.w.meshes[0]);

  in = rt.ext<CommandRegistry>().find(kDeleteBlockCommand)->newInputs();
  test_assert(in.set("block", *mesh).ok() &&
              in.set("cascade", PropValue::ofBool(false)).ok());
  Result<PropertyBag> refused = s.ex.run(s.ctx, kDeleteBlockCommand, std::move(in));
  test_assert(hasMsg(refused.status(), "would also delete rig, rig2"));
  test_assert(lib.get(*rigH)->state() == BlockState::Live);

  in = rt.ext<CommandRegistry>().find(kDeleteBlockCommand)->newInputs();
  test_assert(in.set("block", *mesh).ok());
  del = s.ex.run(s.ctx, kDeleteBlockCommand, std::move(in));
  test_assert(del.ok() && *del->get("count") == PropValue::ofInt(3));
  test_assert(lib.get(*rig2H)->state() == BlockState::InHistory);
  test_assert(s.ex.undo(s.ctx).ok());
  test_assert(lib.get(*rig2H)->state() == BlockState::Live &&
              lib.resolveRef(rig2->target) == *rigH);
  test_assert(s.ex.redo(s.ctx).ok() && lib.get(*rigH)->state() == BlockState::InHistory);

  /* Purge: history-held blocks are roots, so nothing goes until the history drops them.
   * The scene's objects are rooted by hand (the fixture's scene is a runtime owner). */
  for (BlockHandle ob : s.w.scene.objects) {
    lib.setFlag(ob, BlockFlags::FakeUser, true);
  }
  Result<PropertyBag> purged = invoke(s.ctx, "core.block.purge");
  test_assert(purged.ok() && *purged->get("count") == PropValue::ofInt(0));
  s.history.clear(); /* discards the applied delete: the three are destroyed */
  test_assert(!lib.get(*rigH) && !lib.get(*rig2H) && !lib.get(s.w.meshes[0]));
  test_assert(lib.validate().ok() && rt.validateAll().ok());
  test_assert(s.ex.run(s.ctx,
                       kDeleteBlockCommand,
                       rt.ext<CommandRegistry>().find(kDeleteBlockCommand)->newInputs())
                  .status()
                  .code() == StatusCode::FailedPrecondition);
}

/* A snapshot provider over one int. */
struct IntSnapshot {
  int *value = nullptr;
  Status capture(Context &, litestl::util::Vector<uint8_t> &out) const
  {
    for (int i = 0; i < 4; i++) {
      out.append(uint8_t((uint32_t(*value) >> (i * 8)) & 0xff));
    }
    return Status();
  }
  Status restore(Context &, const litestl::util::Vector<uint8_t> &blob)
  {
    if (blob.size() != 4) {
      return Status::error(StatusCode::InvalidArgument, "bad snapshot");
    }
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
      v |= uint32_t(blob[i]) << (i * 8);
    }
    *value = int(v);
    return Status();
  }
  static lbt::Struct<IntSnapshot> *defineBindings()
  {
    auto *st = new lbt::Struct<IntSnapshot>("test::IntSnapshot", sizeof(IntSnapshot));
    BIND_STRUCT_METHOD(st, capture, MARGS("ctx", "blob"));
    BIND_STRUCT_METHOD(st, restore, MARGS("ctx", "blob"));
    return st;
  }
};

static void test_undo_strategies()
{
  std::string log;
  {
    DelegatedUndo d([&log]() { log += "u"; },
                    [&log]() {
                      log += "r";
                      return Status();
                    },
                    [&log]() { log += "x"; },
                    64);
    d.undo();
    test_assert(d.redo().ok() && d.byteSize() == 64);
    DelegatedUndo moved = std::move(d);
    moved.discard();
    moved.discard(); /* once */
  }
  test_assert(log == "urx");

  Runtime rt;
  test_assert((registerImpl<SnapshotProvider, IntSnapshot>(rt, "test.IntSnapshot").ok()));
  int value = 1234567;
  IntSnapshot impl;
  impl.value = &value;
  SnapshotProvider snap =
      query<SnapshotProvider>(rt, TypeId::of("test.IntSnapshot"), &impl);
  test_assert(bool(snap));
  Context ctx(&rt, nullptr, PointerRef());
  litestl::util::Vector<uint8_t> blob;
  test_assert(snap.capture(ctx, blob).ok());
  value = 0;
  test_assert(snap.restore(ctx, blob).ok() && value == 1234567);
  test_assert(!snap.restore(ctx, litestl::util::Vector<uint8_t>()).ok());
}

int main()
{
  test_block_commands();
  test_undo_strategies();
  return test_end();
}
