/* rename and unlinkRef, deleting placeholders, and pointer undo through PointerAccessor::cell. */
#include "command_fixture.h"

#include "coredcc/api/library_io.h"

#include "coredcc/command/block_edits.h"
#include "coredcc/command/history_block.h"

test_init;

namespace {

PropValue pointerTo(BlockId id)
{
  StableRef r;
  r.owner = OwnerKey::ofBlock(id);
  return PropValue::ofPointer(r);
}

PropertyBag blockInputs(Setup &s, const char *command, BlockId id)
{
  PropertyBag in = s.w.rt.ext<CommandRegistry>().find(command)->newInputs();
  test_assert(in.set("block", pointerTo(id)).ok());
  return in;
}

BlockHandle addPlaceholder(Setup &s, BlockId ref, RefKind kind = RefKind::Strong)
{
  PlaceholderInfo info;
  info.typeName = "gone.Widget";
  info.name = "ph";
  RefCell cell(kind);
  cell.target = ref;
  info.payload.refs.append(RefCell(RefKind::Weak));
  info.payload.refs.append(cell);
  Result<BlockHandle> h =
      s.w.lib.addPlaceholder(BlockId{ClientId::of("gone"), s.w.rt.newLocalId()}, info);
  test_assert(h.ok());
  return h.ok() ? *h : BlockHandle();
}

std::string nameOf(Setup &s, BlockHandle h)
{
  return s.w.lib.get(h)->name().c_str();
}

Status renameTo(Setup &s, BlockHandle h, const char *name)
{
  PropertyBag in = blockInputs(s, kRenameBlockCommand, s.w.lib.get(h)->id());
  test_assert(in.set("name", PropValue::ofString(name)).ok());
  return s.ex.run(s.ctx, kRenameBlockCommand, std::move(in)).status();
}

} // namespace

static void test_rename()
{
  Setup s;
  const BlockHandle ob = s.w.scene.objects[0];
  ChangeBus &bus = s.w.rt.ext<ChangeBus>();
  int fired = 0;
  std::string seen;
  Result<ScopedConnection> c = bus.watch(s.ctx, "objects[0].name", [&](const PropValue &v, const Status &) {
    fired++;
    seen = v.isNone() ? "none" : v.asString().c_str();
  });
  int heard = 0;
  ScopedConnection l = bus.addListener([&](const OwnerKey *owner) {
    heard += owner && *owner == OwnerKey::ofBlock(s.w.lib.get(ob)->id()) ? 1 : 0;
  });
  test_assert(c.ok());
  test_assert(renameTo(s, ob, "Renamed").ok() && nameOf(s, ob) == "Renamed");
  test_assert(heard == 1 && bus.flush() == 1 && seen == "Renamed");
  test_assert(s.w.lib.get(ob)->pins(PinKind::History) == 1);
  test_assert(s.ex.undo(s.ctx).ok() && nameOf(s, ob) == "Cube" && heard == 2);
  test_assert(s.ex.redo(s.ctx).ok() && nameOf(s, ob) == "Renamed" && heard == 3);
  s.history.clear();
  test_assert(s.w.lib.get(ob)->pins() == 0);

  /* Placeholders too; no such block; no Library. */
  const BlockHandle ph = addPlaceholder(s, BlockId());
  test_assert(renameTo(s, ph, "p2").ok() && nameOf(s, ph) == "p2");
  test_assert(s.ex.undo(s.ctx).ok() && nameOf(s, ph) == "ph");
  PropertyBag in = blockInputs(s, kRenameBlockCommand, BlockId{ClientId::of("x"), {1, 1}});
  test_assert(s.ex.run(s.ctx, kRenameBlockCommand, std::move(in)).status().code() ==
              StatusCode::FailedPrecondition);
  /* Its block gone for good: undo's error is sticky, not a crash. */
  test_assert(renameTo(s, ph, "p3").ok());
  test_assert(s.w.lib.markDeleted(ph).ok() && s.w.lib.destroyFromHistory(ph).ok());
  test_assert(!s.ex.undo(s.ctx).ok());
  s.w.rt.takeStickyError().ignore();
  test_assert(s.w.lib.validate().ok());
}

static void test_unlink_ref()
{
  Setup s;
  const BlockId mesh = s.w.meshId(0);
  const BlockHandle ph = addPlaceholder(s, mesh);
  const auto row = [&]() { return s.w.lib.get(ph)->placeholderData().refs[1].target; };
  const auto unlink = [&](int64_t index, BlockHandle h) {
    PropertyBag in = blockInputs(s, kUnlinkRefCommand, s.w.lib.get(h)->id());
    test_assert(in.set("index", PropValue::ofInt(index)).ok());
    return s.ex.run(s.ctx, kUnlinkRefCommand, std::move(in)).status();
  };
  test_assert(unlink(2, ph).code() == StatusCode::OutOfRange &&
              unlink(-1, ph).code() == StatusCode::OutOfRange);
  test_assert(unlink(0, s.w.meshes[1]).code() == StatusCode::OutOfRange); /* no table */
  test_assert(unlink(1, ph).ok() && row().isNull());
  test_assert(s.w.lib.get(s.w.meshes[0])->pins(PinKind::History) == 1);
  test_assert(s.ex.undo(s.ctx).ok() && row() == mesh);
  test_assert(s.ex.redo(s.ctx).ok() && row().isNull());
  test_assert(s.ex.undo(s.ctx).ok());

  /* Deleting the target takes the placeholder with it, unless it is unlinked. */
  litestl::util::Vector<BlockHandle> preview = deletePreview(s.w.lib, s.w.meshes[0]);
  test_assert(preview.size() == 2 && preview[1] == ph);
  test_assert(s.ex.run(s.ctx, kDeleteBlockCommand, blockInputs(s, kDeleteBlockCommand, mesh)).ok());
  test_assert(s.w.lib.get(ph)->state() == BlockState::InHistory);
  test_assert(s.ex.undo(s.ctx).ok());
  test_assert(s.w.lib.get(ph)->state() == BlockState::Placeholder && row() == mesh);
  test_assert(unlink(1, ph).ok() && deletePreview(s.w.lib, s.w.meshes[0]).size() == 1);
  s.history.clear();
  test_assert(s.w.lib.get(ph)->pins() == 0 && s.w.lib.get(s.w.meshes[0])->pins() == 0);

  /* Detached blocks can be deleted too. */
  const BlockHandle m1 = s.w.meshes[1];
  test_assert(s.w.lib.onHostDestroyed(s.w.meshId(1)).ok());
  test_assert(s.ex.run(s.ctx, kDeleteBlockCommand,
                       blockInputs(s, kDeleteBlockCommand, s.w.meshId(1)))
                  .ok());
  test_assert(s.w.lib.get(m1)->state() == BlockState::InHistory);
  test_assert(s.ex.undo(s.ctx).ok() && s.w.lib.get(m1)->state() == BlockState::Detached);
  test_assert(s.w.lib.validate().ok() && !s.w.rt.hasStickyError());
}

/* The references of an attachment this build can't read can be cut too. */
static void test_unlink_opaque()
{
  Setup s;
  const BlockId mesh = s.w.meshId(0);
  /* Loading is how an opaque attachment comes about. */
  BytesWriter names, blocks, data;
  names.u32(2);
  for (const char *name : {kCoredccClient, "gone.Notes"}) {
    names.u8(0);
    names.str(name);
  }
  blocks.u32(1);
  const size_t record = blocks.beginSection();
  blocks.u32(1);
  blocks.u64(5);
  blocks.u64(5);
  blocks.u32(0);
  blocks.str("owner");
  blocks.u32(0);
  blocks.u8(1);
  blocks.u32(0);
  blocks.u32(0);
  blocks.u32(0);
  blocks.u32(0);
  blocks.u32(1);
  blocks.u32(2);
  blocks.u32(1);
  blocks.u32(0);
  blocks.u32(1);
  blocks.u8(0);
  blocks.u32(1);
  blocks.u64(mesh.local.hi);
  blocks.u64(mesh.local.lo);
  blocks.u32(0); /* no blob */
  blocks.u32(0); /* no extension */
  blocks.endSection(record);
  data.u32(0);
  BytesWriter file;
  const char magic[] = "CDCCLIB\n";
  for (int i = 0; i < 8; i++) {
    file.u8(uint8_t(magic[i]));
  }
  const uint64_t total = 28 + 3 * 24 + names.data().size() + blocks.data().size() + 4 + 8;
  file.u32(1);
  file.u32(1);
  file.u64(total);
  file.u32(3);
  const char *tags[] = {"NAME", "BLCK", "DATA"};
  const BytesWriter *parts[] = {&names, &blocks, &data};
  for (int i = 0; i < 3; i++) {
    for (int k = 0; k < 4; k++) {
      file.u8(uint8_t(tags[i][k]));
    }
    file.u32(1);
    file.u64(parts[i]->data().size());
    file.u64(parts[i]->data().size());
    file.raw(std::span(parts[i]->data().data(), size_t(parts[i]->data().size())));
  }
  file.u64(detail::fnv1a64(std::string_view(
      reinterpret_cast<const char *>(file.data().data()), size_t(file.data().size()))));
  Result<AppendResult> r =
      appendLibrary(s.ctx, std::span(file.data().data(), size_t(file.data().size())));
  test_assert(r.ok());
  if (!r.ok()) {
    fprintf(stderr, "%s\n", r.status().message());
    return;
  }
  Library &lib = s.w.lib;
  const BlockHandle owner = r->report.blocks[0];
  const TypeId type = TypeId::of("gone.Notes");
  const auto row = [&]() { return (*lib.refTable(owner, type))[0].target; };
  test_assert(lib.refTable(owner) && lib.refTable(owner)->size() == 0 && row() == mesh);
  test_assert(!lib.refTable(s.w.meshes[0]) && !lib.refTable(owner, TypeId::of("other")));
  test_assert(!lib.refTable(BlockHandle()) && !lib.refTable(BlockHandle(), type));
  test_assert(deletePreview(lib, s.w.meshes[0]).size() == 2);
  const auto unlink = [&](int64_t index, const char *attachment) {
    PropertyBag in = blockInputs(s, kUnlinkRefCommand, lib.get(owner)->id());
    test_assert(in.set("index", PropValue::ofInt(index)).ok());
    test_assert(in.set("attachment", PropValue::ofString(attachment)).ok());
    return s.ex.run(s.ctx, kUnlinkRefCommand, std::move(in)).status();
  };
  test_assert(unlink(0, "").code() == StatusCode::OutOfRange);
  test_assert(unlink(0, "other").code() == StatusCode::OutOfRange);
  test_assert(unlink(1, "gone.Notes").code() == StatusCode::OutOfRange);
  test_assert(unlink(0, "gone.Notes").ok() && row().isNull());
  test_assert(deletePreview(lib, s.w.meshes[0]).size() == 1);
  test_assert(lib.get(s.w.meshes[0])->pins(PinKind::History) == 1);
  test_assert(s.ex.undo(s.ctx).ok() && row() == mesh);
  test_assert(s.ex.redo(s.ctx).ok() && row().isNull());
  test_assert(s.ex.undo(s.ctx).ok());
  test_assert(lib.forceUnlinkRef(owner, 0, TypeId::of("other")).code() == StatusCode::NotFound);
  test_assert(lib.forceUnlinkRef(owner, 1, type).code() == StatusCode::OutOfRange);
  const uint64_t epoch = lib.epoch();
  test_assert(lib.forceUnlinkRef(owner, 0, type).ok() && lib.epoch() > epoch);
}

/* Setting a pointer away from a block that isn't Live, and undoing, restores the link. */
static void test_pointer_cell()
{
  for (const char *path : {"objects[0].data", "objects[0].plainData"}) {
    const bool hasCell = strstr(path, "plain") == nullptr;
    Setup s;
    ChangeBus &bus = s.w.rt.ext<ChangeBus>();
    const BlockId lostId{ClientId::of(kCoredccClient), s.w.rt.newLocalId()};
    PlaceholderInfo info;
    info.reason = PlaceholderReason::MissingBlock;
    Result<BlockHandle> lost = s.w.lib.addPlaceholder(lostId, info);
    test_assert(lost.ok());
    s.w.object(0).mesh.target = lostId;
    s.w.lib.noteRefsChanged();
    for (BlockHandle ob : s.w.scene.objects) {
      s.w.lib.setFlag(ob, BlockFlags::FakeUser, true);
    }
    test_assert(*getValue(s.ctx, path) == PropValue::nullPointer());
    Result<PropValue> sphere = getValue(s.ctx, "objects[1].data");
    int updates = g_dataUpdates;
    test_assert(setValueUndo(s.ctx, path, *sphere).ok());
    test_assert(g_dataUpdates == updates + 1);
    test_assert(s.w.object(0).mesh.target == s.w.meshId(1));
    test_assert(s.w.lib.get(*lost)->pins(PinKind::History) == (hasCell ? 1 : 0));
    test_assert(s.w.lib.collect().ok());
    test_assert((s.w.lib.get(*lost) != nullptr) == hasCell);
    bus.flush();
    int heard = 0;
    ScopedConnection l = bus.addListener([&](const OwnerKey *owner) {
      heard += owner && *owner == OwnerKey::ofBlock(s.w.objectId(0)) ? 1 : 0;
    });
    const uint64_t epoch = s.w.lib.epoch();
    updates = g_dataUpdates;
    test_assert(s.ex.undo(s.ctx).ok() && heard == 1 && s.w.lib.epoch() > epoch);
    test_assert(g_dataUpdates == updates + 1); /* the update hook, either way */
    test_assert(s.w.object(0).mesh.target == (hasCell ? lostId : BlockId()));
    test_assert(s.ex.redo(s.ctx).ok() && s.w.object(0).mesh.target == s.w.meshId(1));
    if (hasCell) {
      /* Filled in meanwhile: undo links it, and it resolves. */
      test_assert(s.w.lib.createWithId(kMeshType, lostId, "m", litestl::alloc::New<Mesh>("Mesh")).ok());
      test_assert(s.ex.undo(s.ctx).ok() && s.w.lib.resolveRef(s.w.object(0).mesh) == *lost);
      test_assert(s.ex.redo(s.ctx).ok());
    }
    s.history.clear();
    test_assert(s.w.lib.validate().ok() && !s.w.rt.hasStickyError());
  }
  {
    /* The other way round: redo restores a link to a block deleted since. */
    Setup s;
    Result<PropValue> sphere = getValue(s.ctx, "objects[1].data");
    test_assert(setValueUndo(s.ctx, "objects[0].data", *sphere).ok());
    test_assert(s.ex.undo(s.ctx).ok());
    test_assert(s.w.lib.markDeleted(s.w.meshes[1]).ok());
    test_assert(s.ex.redo(s.ctx).ok() && s.w.object(0).mesh.target == s.w.meshId(1));
    test_assert(*getValue(s.ctx, "objects[0].data") == PropValue::nullPointer());
    test_assert(s.w.lib.restoreFromHistory(s.w.meshes[1]).ok());
    test_assert(*getValue(s.ctx, "objects[0].data") == *sphere);
  }
  {
    /* Gestures fold: undo goes to the first link, redo to the last. */
    Setup s;
    const BlockId first = s.w.meshId(0);
    Result<PropValue> sphere = getValue(s.ctx, "objects[1].data");
    test_assert(setValueUndo(s.ctx, "objects[0].data", *sphere, 9).ok());
    test_assert(setValueUndo(s.ctx, "objects[0].data", PropValue::nullPointer(), 9).ok());
    test_assert(s.history.size() == 1 && s.w.object(0).mesh.isNull());
    test_assert(s.ex.undo(s.ctx).ok() && s.w.object(0).mesh.target == first);
    test_assert(s.ex.redo(s.ctx).ok() && s.w.object(0).mesh.isNull());
  }
}

static void test_history_block()
{
  World w;
  HistoryBlock none;
  test_assert(!none.get() && none.id().isNull() && !none.library());
  none = HistoryBlock(w.lib, BlockHandle());
  test_assert(!none.get());
  const BlockHandle m = w.meshes[0];
  const BlockId id = w.meshId(0);
  HistoryBlock hb(w.lib, m);
  test_assert(hb.get() == m && hb.id() == id && w.lib.get(m)->pins(PinKind::History) == 1);
  HistoryBlock moved = std::move(hb);
  test_assert(moved.get() == m && w.lib.get(m)->pins() == 1);
  /* The record is replaced under its id: found, and the new one isn't unpinned. */
  w.object(0).mesh.target = BlockId();
  test_assert(w.lib.markDeleted(m).ok() && w.lib.destroyFromHistory(m).ok() && !moved.get());
  Result<BlockHandle> again = w.lib.reviveAs(kMeshType, id, "m", litestl::alloc::New<Mesh>("Mesh"));
  test_assert(again.ok() && moved.get() == *again && *again != m);
  w.lib.pin(*again, PinKind::History);
  moved.release();
  test_assert(w.lib.get(*again)->pins(PinKind::History) == 1 && !moved.library());
}

int main()
{
  test_rename();
  test_unlink_ref();
  test_unlink_opaque();
  test_pointer_cell();
  test_history_block();
  return test_end();
}
