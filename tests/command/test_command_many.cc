/* Commands on several blocks, what the history counts for them, and the undo of commands
 * that fill missing blocks or touch data this build can't read. */
#include "coredcc/api/change_bus.h"
#include "coredcc/api/library_io.h"
#include "coredcc/api/ref_text.h"
#include "coredcc/command/attachment_commands.h"
#include "coredcc/command/block_commands.h"
#include "coredcc/command/block_edits.h"
#include "coredcc/command/executor.h"
#include "coredcc/command/library_commands.h"

#include "../lib/io_fixture.h"
#include "fuzz.h"

#include <chrono>

test_init;

using namespace coredcc;
using namespace coredcc_test;

namespace {

BlockId cid(uint64_t n, ClientId client = ClientId::of(kCoredccClient))
{
  return BlockId{client, LocalId{7, n}};
}

PropValue pointer(BlockId id)
{
  StableRef r;
  r.owner = OwnerKey::ofBlock(id);
  return PropValue::ofPointer(r);
}

struct CommandWorld {
  IoWorld w;
  LinearHistory history;
  CommandExecutor ex{&w.rt, &history};
  Context ctx{&w.rt, &w.lib, PointerRef()};

  explicit CommandWorld(Knows k = {}) : w(k)
  {
    ctx.setExecutor(&ex);
    test_assert(registerBlockCommands(w.rt).ok());
  }
  /* `block` is null for none. */
  Result<PropertyBag>
  run(const char *command, BlockId block, const std::string &blocks, bool cascade = true)
  {
    PropertyBag in = w.rt.ext<CommandRegistry>().find(command)->newInputs();
    if (!block.isNull()) {
      test_assert(in.set("block", pointer(block)).ok());
    }
    if (!blocks.empty()) {
      test_assert(in.set("blocks", PropValue::ofString(blocks.c_str())).ok());
    }
    if (in.indexOf("cascade") >= 0) {
      test_assert(in.set("cascade", PropValue::ofBool(cascade)).ok());
    }
    return ex.run(ctx, command, std::move(in));
  }
  std::string list(std::initializer_list<uint64_t> ns) const
  {
    Vector<BlockId> ids;
    for (uint64_t n : ns) {
      ids.append(cid(n));
    }
    return formatBlockIds(w.rt, std::span<const BlockId>(ids.data(), size_t(ids.size())));
  }
  bool isLive(uint64_t n) const
  {
    const Datablock *b = w.lib.get(w.at(n));
    return b && b->state() == BlockState::Live;
  }
};

bool refusedWith(const Result<PropertyBag> &r, const char *what)
{
  return !r.ok() && r.status().code() == StatusCode::FailedPrecondition &&
         hasMsg(r.status(), what);
}

} // namespace

static void test_delete()
{
  CommandWorld c;
  IoWorld &w = c.w;
  for (uint64_t n = 1; n <= 6; n++) {
    w.doc(n, ("d" + std::to_string(n)).c_str());
  }
  /* 4 needs 1 and 2; 5 needs 4; 6 is free. */
  w.link(w.at(4), w.at(1));
  test_assert(w.lib.setSlot(w.at(4), kNote, 0, cid(2), RefKind::Strong).ok());
  w.link(w.at(5), w.at(4));

  const BlockHandle set[] = {w.at(2), BlockHandle(), w.at(1), w.at(2)};
  const Vector<BlockHandle> preview = deletePreview(w.lib, set);
  test_assert(preview.size() == 4 && preview[0] == w.at(2) && preview[1] == w.at(1) &&
              preview[2] == w.at(4) && preview[3] == w.at(5));
  test_assert(deletePreview(w.lib, std::span<const BlockHandle>()).size() == 0);
  test_assert(deletePreview(w.lib, w.at(6)).size() == 1);

  test_assert(refusedWith(c.run(kDeleteBlockCommand, BlockId(), c.list({1, 2}), false),
                          "would also delete d4, d5"));
  test_assert(c.isLive(1) && c.isLive(2) && c.history.size() == 0);
  Result<PropertyBag> r = c.run(kDeleteBlockCommand, cid(1), c.list({2, 1, 2}));
  test_assert(r.ok() && r->get("count")->asInt() == 4 && c.history.size() == 1);
  test_assert(!c.isLive(1) && !c.isLive(2) && !c.isLive(4) && !c.isLive(5) &&
              c.isLive(6));
  test_assert(w.lib.get(w.at(4))->pins(PinKind::History) == 1);
  test_assert(c.ex.undo(c.ctx).ok());
  test_assert(c.isLive(1) && c.isLive(2) && c.isLive(4) && c.isLive(5));
  /* A redo takes what needs them by then: with any of the targets. */
  w.link(w.at(6), w.at(2));
  test_assert(c.ex.redo(c.ctx).ok() && !c.isLive(6) && !c.isLive(5));
  test_assert(c.ex.undo(c.ctx).ok() && c.isLive(6) && c.isLive(5));
  /* Together they are a whole: nothing cascades. */
  r = c.run(kDeleteBlockCommand, BlockId(), c.list({1, 2, 4, 5, 6}), false);
  test_assert(r.ok() && r->get("count")->asInt() == 5);
  const int destroyed = w.docs.destroyed;
  c.history.clear();
  test_assert(w.docs.destroyed == destroyed + 5 && w.lib.size() == 1 && w.valid());

  /* Twice in one set is once. */
  w.doc(9, "d9");
  test_assert(w.lib.markDeleted(w.at(9)).ok());
  const BlockHandle twice[] = {w.at(9), w.at(9)};
  test_assert(w.lib.destroyFromHistory(twice).ok() && w.docs.destroyed == destroyed + 6);
  test_assert(w.lib.size() == 1 && w.valid());

  /* What isn't a block, or the text of one. */
  w.doc(7, "d7");
  test_assert(refusedWith(c.run(kDeleteBlockCommand, BlockId(), ""), "no such block"));
  test_assert(
      refusedWith(c.run(kDeleteBlockCommand, cid(99), c.list({7})), "no such block"));
  test_assert(refusedWith(c.run(kDeleteBlockCommand, BlockId(), c.list({7, 99})),
                          "no such block coredcc,00000000000000070000000000000063"));
  r = c.run(kDeleteBlockCommand, BlockId(), "nonsense");
  test_assert(r.status().code() == StatusCode::InvalidArgument &&
              hasMsg(r.status(), "blocks: "));
  test_assert(refusedWith(c.run(kDeleteBlockCommand, BlockId(), ";"), "") ||
              !c.run(kDeleteBlockCommand, BlockId(), ";").ok());
  test_assert(
      refusedWith(c.run(kDeleteBlockCommand, BlockId(), " "), "no block is named"));
  Context noLib(&w.rt, nullptr, PointerRef());
  noLib.setExecutor(&c.ex);
  PropertyBag in = w.rt.ext<CommandRegistry>().find(kDeleteBlockCommand)->newInputs();
  test_assert(in.set("blocks", PropValue::ofString(c.list({7}).c_str())).ok());
  test_assert(
      refusedWith(c.ex.run(noLib, kDeleteBlockCommand, std::move(in)), "no Library"));
  test_assert(c.isLive(7) && c.history.size() == 0);

  /* One that can't be deleted: the others come back. */
  w.doc(8, "d8");
  test_assert(w.lib.markDeleted(w.at(8)).ok());
  test_assert(
      refusedWith(c.run(kDeleteBlockCommand, BlockId(), c.list({7, 8})), "is InHistory"));
  test_assert(c.isLive(7) && w.lib.get(w.at(8))->state() == BlockState::InHistory);
}

/* Against looking at each user again for each target. */
static void test_preview_random()
{
  SplitMix64 r(17);
  int taken = 0;
  for (int round = 0; round < 60 * testScale(); round++) {
    IoWorld w;
    for (uint64_t n = 1; n <= 16; n++) {
      w.doc(n, "d");
    }
    for (uint64_t n = 1; n <= 16; n++) {
      const auto any = [&]() { return w.at(1 + r.below(16)); };
      w.link(w.at(n),
             r.below(3) ? any() : BlockHandle(),
             r.below(3) ? any() : BlockHandle());
      for (int i = int(r.below(3)); i > 0; i--) {
        const RefKind kind = r.below(2) ? RefKind::Strong : RefKind::OptStrong;
        test_assert(w.lib.setSlot(w.at(n), kNote, i, w.id(any()), kind).ok());
      }
    }
    const BlockHandle targets[] = {w.at(1 + r.below(16)), w.at(1 + r.below(16))};
    Vector<BlockHandle> want;
    for (BlockHandle h : targets) {
      if (!want.contains(h)) {
        want.append(h);
      }
    }
    for (int i = 0; i < int(want.size()); i++) {
      for (BlockHandle user : w.lib.usersOf(want[i])) {
        bool breaks = false;
        test_assert(w.lib
                        .forEachRef(user,
                                    [&](RefCell &c) {
                                      breaks |= c.kind == RefKind::Strong &&
                                                c.target == w.id(want[i]);
                                    })
                        .ok());
        if (breaks && !want.contains(user)) {
          want.append(user);
        }
      }
    }
    const Vector<BlockHandle> got = deletePreview(w.lib, targets);
    test_assert(got.size() == want.size());
    for (int i = 0; i < int(got.size()); i++) {
      test_assert(got[i] == want[i]);
    }
    taken += int(got.size());
  }
  test_assert(taken > 200);
}

/* A cascade of many is named in part. */
static void test_delete_names()
{
  CommandWorld c;
  c.w.doc(1, "target");
  for (uint64_t n = 2; n <= 31; n++) {
    c.w.link(c.w.doc(n, "user"), c.w.at(1));
  }
  test_assert(
      refusedWith(c.run(kDeleteBlockCommand, cid(1), "", false), "user and 10 more"));
}

/* Deleting, undoing, redoing and dropping many blocks takes time by their number.
 * Everything here was quadratic: at this size it took minutes. */
static void test_delete_time()
{
#ifdef NDEBUG
  constexpr uint64_t kBlocks = 40000, kDeleted = 20000;
#else
  constexpr uint64_t kBlocks = 10000, kDeleted = 5000;
#endif
  CommandWorld c;
  IoWorld &w = c.w;
  w.lib.setAuditRelations(false);
  Vector<BlockId> ids;
  for (uint64_t n = 1; n <= kBlocks; n++) {
    const BlockHandle h = w.doc(n, "d");
    w.data(h)->parent.target = cid(1 + n / 2);
    w.data(h)->mesh.target = n % 4 == 3 ? cid(n - 1) : BlockId();
    if (n <= kDeleted) {
      ids.append(cid(n));
    }
  }
  w.lib.noteRefsChanged();
  const std::string text =
      formatBlockIds(w.rt, std::span<const BlockId>(ids.data(), size_t(ids.size())));
  const auto start = std::chrono::steady_clock::now();
  Result<PropertyBag> r = c.run(kDeleteBlockCommand, BlockId(), text);
  test_assert(r.ok() && r->get("count")->asInt() == int64_t(kDeleted));
  test_assert(c.ex.undo(c.ctx).ok() && c.ex.redo(c.ctx).ok());
  c.history.clear();
  test_assert(uint64_t(w.lib.size()) == kBlocks - kDeleted);
  const double s =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  if (s >= 5) {
    fprintf(stderr, "it took %f s\n", s);
  }
  test_assert(s < 5);
}

static void test_duplicate()
{
  CommandWorld c;
  IoWorld &w = c.w;
  for (uint64_t n = 1; n <= 3; n++) {
    w.doc(n, "d");
  }
  w.link(w.at(1), w.at(2), w.at(3));
  w.link(w.at(2), w.at(1));
  Result<PropertyBag> r = c.run(kDuplicateBlockCommand, cid(2), c.list({1, 2}));
  test_assert(r.ok() && w.lib.size() == 5 && c.history.size() == 1);
  Result<Vector<BlockId>> copies =
      parseBlockIds(w.rt, r->get("blocks")->asString().c_str());
  test_assert(copies.ok() && copies->size() == 2);
  test_assert(r->get("block")->asPointer().owner.block == (*copies)[0]);
  /* In the order they were given, and they point at each other. */
  const BlockHandle two = w.lib.find((*copies)[0]), one = w.lib.find((*copies)[1]);
  test_assert(w.data(two)->number == 20 && w.data(one)->number == 10);
  test_assert(w.data(one)->mesh.target == (*copies)[0] &&
              w.data(two)->mesh.target == (*copies)[1]);
  test_assert(w.data(one)->parent.target == cid(3));
  test_assert(c.ex.undo(c.ctx).ok() && w.lib.get(one)->state() == BlockState::InHistory &&
              w.lib.get(two)->state() == BlockState::InHistory);
  test_assert(c.ex.redo(c.ctx).ok() && w.lib.get(one)->state() == BlockState::Live);
  /* One, as before. */
  r = c.run(kDuplicateBlockCommand, cid(3), "");
  test_assert(r.ok() && formatBlockId(w.rt, r->get("block")->asPointer().owner.block) ==
                            r->get("blocks")->asString().c_str());
  /* All or none. */
  w.lib.onHostDestroyed(cid(3)).ignore();
  const int size = w.lib.size();
  test_assert(!c.run(kDuplicateBlockCommand, BlockId(), c.list({1, 3})).ok() &&
              w.lib.size() == size);
}

/* ---- 27: what the history holds ---- */

static void test_memory()
{
  CommandWorld c;
  IoWorld &w = c.w;
  w.docs.dataSize = 1000000;
  w.doc(1, "a");
  w.doc(2, "b");
  w.link(w.at(2), w.at(1));
  test_assert(w.lib.memSize(w.at(1)) > 1000000 && w.lib.memSize(BlockHandle()) == 0);
  const auto used = [&]() { return c.history.memoryUsed() / 1000000; };
  test_assert(c.run(kDeleteBlockCommand, cid(1), "").ok() && used() == 2);
  test_assert(c.ex.undo(c.ctx).ok() && used() == 0 && c.history.validate().ok());
  test_assert(c.ex.redo(c.ctx).ok() && used() == 2);
  test_assert(c.ex.undo(c.ctx).ok());
  test_assert(c.run(kDuplicateBlockCommand, BlockId(), c.list({1, 2})).ok() &&
              used() == 0);
  test_assert(c.ex.undo(c.ctx).ok() && used() == 2);
  test_assert(c.ex.redo(c.ctx).ok() && used() == 0 && c.history.validate().ok());
  /* Over the budget, the oldest go. */
  c.history.clear();
  c.history.setMemoryBudget(1500000);
  test_assert(c.run(kDuplicateBlockCommand, cid(1), "").ok() && c.history.size() == 1);
  test_assert(c.run(kDeleteBlockCommand, cid(1), "").ok() && c.history.size() == 1);
  /* A placeholder's payload counts too. */
  PlaceholderInfo info;
  info.typeName = "x.Gone";
  info.payload.bytes.resize(5000);
  info.payload.refs.append(RefCell(RefKind::Weak));
  Result<BlockHandle> h = w.lib.addPlaceholder(cid(50), info);
  test_assert(w.lib.memSize(*h) == sizeof(Datablock) + 5000 + sizeof(RefCell));
  /* A block without its data has none to count. */
  test_assert(w.lib.onHostDestroyed(cid(2)).code() == StatusCode::FailedPrecondition ||
              w.lib.memSize(w.at(2)) < 1000000);
  /* An attachment that couldn't be read is the detach command's. */
  OpaqueAttachment o;
  o.typeName = "x.Other";
  o.payload.bytes.resize(3000000);
  test_assert(w.lib.attachOpaque(*h, o).ok());
  c.history.clear();
  c.history.setMemoryBudget(1 << 30);
  PropertyBag in = w.rt.ext<CommandRegistry>().find(kDetachCommand)->newInputs();
  test_assert(in.set("block", pointer(cid(50))).ok() &&
              in.set("type", PropValue::ofString("x.Other")).ok());
  test_assert(c.ex.run(c.ctx, kDetachCommand, std::move(in)).ok() && used() == 3);
  test_assert(c.ex.undo(c.ctx).ok() && used() == 0);
}

/* ---- 3: commands that fill missing blocks ---- */

namespace {

/* Fills every missing acme.Doc and makes one more block; then fails, if told to. */
bool g_fail = false;
struct Adopt {
  CreatedBlocks created;
  Library *lib = nullptr;

  Status canRun(Context &) const
  {
    return Status();
  }
  Status exec(Context &ctx)
  {
    lib = ctx.library();
    Vector<BlockId> missing;
    lib->forEach([&](BlockHandle, const Datablock &b) {
      if (b.type() == kAcmeDoc && b.state() == BlockState::Placeholder) {
        missing.append(b.id());
      }
    });
    created.add(*lib, *lib->create(kDoc, "new", litestl::alloc::New<Doc>("Doc")));
    for (const BlockId &id : missing) {
      created.add(
          *lib,
          *lib->createWithId(kAcmeDoc, id, "mine", litestl::alloc::New<Doc>("Doc")));
    }
    if (g_fail) {
      created.undo(*lib);
      return Status::error(StatusCode::Internal, "adopt failed");
    }
    return Status();
  }
  void undo(Context &)
  {
    created.undo(*lib);
  }
  Status redo(Context &)
  {
    return created.redo(*lib);
  }
  uint64_t memSize() const
  {
    return created.memSize();
  }
  void discard()
  {
    created.discard(*lib);
  }
  static lbt::Struct<Adopt> *defineBindings()
  {
    auto *st = new lbt::Struct<Adopt>("test::Adopt", sizeof(Adopt));
    BIND_STRUCT_METHOD(st, canRun, MARGS("ctx"));
    BIND_STRUCT_METHOD(st, exec, MARGS("ctx"));
    BIND_STRUCT_METHOD(st, undo, MARGS("ctx"));
    BIND_STRUCT_METHOD(st, redo, MARGS("ctx"));
    BIND_STRUCT_METHOD(st, memSize, MARGS());
    BIND_STRUCT_METHOD(st, discard, MARGS());
    return st;
  }
};

/* A root with an OptStrong reference to a Detached block that has a note. */
Bytes hosted()
{
  IoWorld w;
  const BlockHandle a = w.doc(1, "a"), h = w.doc(5, "actor", kAcmeDoc);
  w.root(a);
  w.addOptEntry(a, w.id(h));
  w.note(h, 5, BlockId());
  test_assert(w.lib.onHostDestroyed(w.id(h)).ok());
  return w.save();
}

} // namespace

/* Commands that leave the history together, in two Libraries, one with a block that can't
 * go. */
static void test_dropped_together()
{
  CommandWorld c;
  IoWorld &w = c.w;
  test_assert(addNativeCommand<Adopt>(w.rt, CommandDef("test.adopt")).ok());
  Library other(&w.rt);
  Context there(&w.rt, &other, PointerRef());
  there.setExecutor(&c.ex);
  for (int i = 0; i < 6; i++) {
    test_assert(c.ex.run(i % 2 ? there : c.ctx, "test.adopt", PropertyBag()).ok());
  }
  for (int i = 5; i >= 0; i--) {
    test_assert(c.ex.undo(i % 2 ? there : c.ctx).ok());
  }
  test_assert(w.lib.size() == 3 && other.size() == 3);
  const uint64_t gone = w.lib.removedCount();
  const int destroyed = w.docs.destroyed;
  test_assert(c.ex.run(c.ctx, "test.adopt", PropertyBag()).ok());
  test_assert(w.lib.size() == 1 && other.size() == 0 && w.lib.removedCount() == gone + 3);
  test_assert(w.docs.destroyed == destroyed + 6 && c.history.size() == 1);

  /* By the budget. */
  for (int i = 0; i < 2; i++) {
    test_assert(c.ex.run(there, "test.adopt", PropertyBag()).ok());
  }
  c.history.clear();
  test_assert(w.lib.size() == 1 &&
              other.size() == 2); /* they were done: the documents' */
  const auto all = [](Library &lib) {
    Vector<BlockHandle> out;
    lib.forEach([&](BlockHandle h, const Datablock &) { out.append(h); });
    return out;
  };
  for (int i = 0; i < 2; i++) {
    const BlockId id = other.get(all(other)[i])->id();
    PropertyBag in = w.rt.ext<CommandRegistry>().find(kDeleteBlockCommand)->newInputs();
    test_assert(in.set("block", pointer(id)).ok());
    test_assert(c.ex.run(there, kDeleteBlockCommand, std::move(in)).ok());
  }
  c.history.setMemoryBudget(1);
  test_assert(other.size() == 1 && c.history.size() == 1);
  c.history.setMemoryBudget(uint64_t(1) << 40);
  {
    /* A history that is dropped while another one is. */
    const detail::HistoryDestroysScope outer(w.rt);
    c.history.clear();
    test_assert(other.size() == 1);
  }
  test_assert(other.size() == 0);

  /* The block of one of them is still held: the others go, and the command says so. */
  const BlockHandle before = all(w.lib)[0];
  for (int i = 0; i < 3; i++) {
    test_assert(c.ex.run(c.ctx, "test.adopt", PropertyBag()).ok());
  }
  Vector<BlockHandle> blocks;
  for (BlockHandle h : all(w.lib)) {
    if (h != before) {
      blocks.append(h);
    }
  }
  const BlockHandle user = w.doc(90, "user");
  for (int i = 0; i < 3; i++) {
    test_assert(c.ex.undo(c.ctx).ok());
  }
  w.link(user, blocks[1]);
  Result<PropertyBag> res = c.ex.run(c.ctx, "test.adopt", PropertyBag());
  test_assert(hasMsg(res.status(), "history's upkeep failed") &&
              hasMsg(res.status(), "'user' still holds a Strong ref"));
  int held = 0;
  for (BlockHandle h : blocks) {
    held += w.lib.get(h) != nullptr;
  }
  test_assert(held == 1 && w.lib.get(blocks[1]) && c.history.size() == 1);
  w.link(user, BlockHandle());
  test_assert(w.lib.destroyFromHistory(blocks[1]).ok());
}

static void test_adopt()
{
  const Bytes bytes = hosted();
  CommandWorld c;
  IoWorld &w = c.w;
  test_assert(addNativeCommand<Adopt>(w.rt, CommandDef("test.adopt")).ok());
  test_assert(w.load(bytes).ok());
  const BlockHandle h = w.at(5, kAcme);
  const auto run = [&]() { return c.ex.run(c.ctx, "test.adopt", PropertyBag()).ok(); };
  w.docs.dataSize = 1000000;
  test_assert(run() && w.lib.get(h)->state() == BlockState::Live && w.lib.size() == 3);
  const Bytes done = w.save();
  test_assert(c.ex.undo(c.ctx).ok());
  test_assert(sameBytes(w.save(), bytes) && w.lib.attachment(h, kNote) && w.valid());
  test_assert(c.history.memoryUsed() / 1000000 == 2);
  test_assert(*w.lib.collect() == 0);
  test_assert(c.ex.redo(c.ctx).ok() && sameBytes(w.save(), done));
  test_assert(c.ex.undo(c.ctx).ok());
  /* Dropped while undone: the data goes, the record stays. */
  int destroyed = w.docs.destroyed;
  c.history.clear();
  test_assert(w.docs.destroyed == destroyed + 2 && sameBytes(w.save(), bytes));
  test_assert(w.lib.find(cid(5, kAcme)) == h && w.lib.size() == 2);
  /* Dropped while done: it is the document's. */
  test_assert(run());
  destroyed = w.docs.destroyed;
  c.history.clear();
  test_assert(w.docs.destroyed == destroyed && sameBytes(w.save(), done) &&
              w.lib.get(h)->state() == BlockState::Live);
  /* A command that fails, after another was undone. */
  CommandWorld d;
  test_assert(addNativeCommand<Adopt>(d.w.rt, CommandDef("test.adopt")).ok() &&
              d.w.load(bytes).ok());
  test_assert(d.ex.run(d.ctx, "test.adopt", PropertyBag()).ok() && d.ex.undo(d.ctx).ok());
  g_fail = true;
  test_assert(!d.ex.run(d.ctx, "test.adopt", PropertyBag()).ok());
  g_fail = false;
  test_assert(sameBytes(d.w.save(), bytes) && d.ex.redo(d.ctx).ok());
  test_assert(d.w.lib.get(d.w.at(5, kAcme))->state() == BlockState::Live);
  /* Somebody else fills it while it is undone: the redo says so, and nothing is half
   * done. */
  test_assert(d.ex.undo(d.ctx).ok());
  const int size = d.w.lib.size();
  test_assert(d.w.lib
                  .createWithId(
                      kAcmeDoc, cid(5, kAcme), "host's", litestl::alloc::New<Doc>("Doc"))
                  .ok());
  test_assert(hasMsg(d.ex.redo(d.ctx), "isn't missing any more"));
  test_assert(strcmp(d.w.lib.get(d.w.at(5, kAcme))->name().c_str(), "host's") == 0);
  test_assert(d.w.lib.size() == size - 1 && d.w.valid());
}

static void test_append_fills()
{
  const Bytes bytes = hosted();
  Bytes library;
  {
    IoWorld w;
    const BlockHandle h = w.doc(20, "from the library", kAcmeDoc);
    w.root(h);
    w.link(h, w.doc(21, "other"));
    library = w.save();
  }
  CommandWorld c;
  IoWorld &w = c.w;
  test_assert(w.load(bytes).ok());
  w.docs.onRekey = [](const Datablock &, const RekeyContext &) { return cid(5, kAcme); };
  const BlockHandle h = w.at(5, kAcme);
  Result<AppendResult> r = appendUndo(c.ctx, view(library));
  test_assert(r.ok() && r->report.blocks[0] == h &&
              w.lib.get(h)->state() == BlockState::Live);
  const Bytes done = w.save();
  test_assert(c.ex.undo(c.ctx).ok() && sameBytes(w.save(), bytes) && w.valid());
  test_assert(c.ex.redo(c.ctx).ok() && sameBytes(w.save(), done));
  test_assert(c.ex.undo(c.ctx).ok());
  c.history.clear();
  test_assert(sameBytes(w.save(), bytes) && w.lib.size() == 2);
  w.docs.onRekey = nullptr;
}

/* ---- 14 and 11: data that this build can't read ---- */

static void test_detach_opaque()
{
  Bytes bytes;
  {
    IoWorld w;
    const BlockHandle a = w.doc(1, "a"), b = w.doc(2, "b");
    w.root(a);
    w.note(a, 1, w.id(b));
    bytes = w.save();
  }
  Knows k;
  k.note = false;
  CommandWorld c(k);
  IoWorld &w = c.w;
  test_assert(w.load(bytes).ok());
  const auto detach = [&](const char *type) {
    PropertyBag in = w.rt.ext<CommandRegistry>().find(kDetachCommand)->newInputs();
    test_assert(in.set("block", pointer(cid(1))).ok() &&
                in.set("type", PropValue::ofString(type)).ok());
    return c.ex.run(c.ctx, kDetachCommand, std::move(in)).status();
  };
  test_assert(detach("test.Other").code() == StatusCode::NotFound);
  test_assert(detach("").code() == StatusCode::NotFound);
  int heard = 0;
  ScopedConnection l = w.rt.ext<ChangeBus>().addListener([&](const OwnerKey *owner) {
    heard += owner && *owner == OwnerKey::ofBlock(cid(1)) ? 1 : 0;
  });
  test_assert(detach("test.Note").ok() && !w.lib.hasAttachment(w.at(1), kNote) &&
              heard == 1);
  test_assert(w.lib.get(w.at(2))->pins(PinKind::History) == 1 && *w.lib.collect() == 0);
  test_assert(detach("test.Note").code() == StatusCode::NotFound);
  test_assert(c.ex.undo(c.ctx).ok() && sameBytes(w.save(), bytes));
  test_assert(w.lib.get(w.at(2))->pins(PinKind::History) == 0);
  test_assert(c.ex.redo(c.ctx).ok() && !w.lib.hasAttachment(w.at(1), kNote));
  /* The type comes while it is detached, and an undo and a resolve bring it back. */
  registerNote(w.rt, w.notes);
  test_assert(c.ex.undo(c.ctx).ok() && !w.lib.attachment(w.at(1), kNote) &&
              w.lib.hasAttachment(w.at(1), kNote));
  test_assert(resolvePlaceholders(c.ctx).ok() && w.lib.attachment(w.at(1), kNote));
  /* The redo takes the real one. */
  test_assert(c.ex.redo(c.ctx).ok() && !w.lib.hasAttachment(w.at(1), kNote) &&
              w.notes.alive == 1);
  test_assert(c.ex.undo(c.ctx).ok() && w.lib.attachment(w.at(1), kNote));
  test_assert(sameBytes(w.save(), bytes));
  test_assert(c.ex.redo(c.ctx).ok());
  c.history.clear();
  test_assert(w.notes.alive == 0);
}

static void test_unlink_after_resolve()
{
  Bytes bytes;
  {
    IoWorld w;
    const BlockHandle a = w.doc(1, "a"), b = w.doc(2, "b");
    w.root(a);
    w.link(a, b);
    bytes = w.save();
  }
  Knows k;
  k.doc = false;
  CommandWorld c(k);
  IoWorld &w = c.w;
  test_assert(w.load(bytes).ok());
  PropertyBag in = w.rt.ext<CommandRegistry>().find(kUnlinkRefCommand)->newInputs();
  test_assert(in.set("block", pointer(cid(1))).ok() &&
              in.set("index", PropValue::ofInt(1)).ok());
  int heard = 0;
  ScopedConnection l = w.rt.ext<ChangeBus>().addListener([&](const OwnerKey *owner) {
    heard += owner && *owner == OwnerKey::ofBlock(cid(1)) ? 1 : 0;
  });
  test_assert(c.ex.run(c.ctx, kUnlinkRefCommand, std::move(in)).ok() && heard == 1);
  test_assert(c.ex.undo(c.ctx).ok() && heard == 2 && c.ex.redo(c.ctx).ok() && heard == 3);
  registerDoc(w.rt, w.docs);
  test_assert(resolvePlaceholders(c.ctx).ok() &&
              w.lib.get(w.at(1))->state() == BlockState::Live);
  Status s = c.ex.undo(c.ctx);
  test_assert(s.code() == StatusCode::FailedPrecondition &&
              hasMsg(s, "'a' has been read since its reference was cut"));
  test_assert(hasMsg(s, "clear the history when placeholders are resolved"));
}

int main()
{
  test_delete();
  test_preview_random();
  test_delete_names();
  test_delete_time();
  test_duplicate();
  test_memory();
  test_dropped_together();
  test_adopt();
  test_append_fills();
  test_detach_opaque();
  test_unlink_after_resolve();
  return test_end();
}
