/* Duplicate and append as undo steps. */
#include "coredcc/api/change_bus.h"
#include "coredcc/command/block_commands.h"
#include "coredcc/command/executor.h"
#include "coredcc/command/library_commands.h"

#include "../lib/io_fixture.h"

test_init;

using namespace coredcc;
using namespace coredcc_test;

namespace {

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
  Result<PropertyBag> duplicate(BlockHandle h)
  {
    PropertyBag in =
        w.rt.ext<CommandRegistry>().find(kDuplicateBlockCommand)->newInputs();
    StableRef r;
    r.owner = OwnerKey::ofBlock(h ? w.id(h) : BlockId{kAcme, LocalId{1, 1}});
    test_assert(in.set("block", PropValue::ofPointer(r)).ok());
    return ex.run(ctx, kDuplicateBlockCommand, std::move(in));
  }
};

/* a, b and, by a strong ref, a block that isn't in the file. */
Bytes sample()
{
  IoWorld w;
  const BlockHandle a = w.doc(1, "a"), b = w.doc(2, "b");
  w.root(a);
  w.link(a, b);
  w.data(b)->mesh.target = BlockId{kAcme, LocalId{7, 50}};
  w.lib.noteRefsChanged();
  return w.save();
}

} // namespace

static void test_duplicate()
{
  CommandWorld c;
  IoWorld &w = c.w;
  const BlockHandle a = w.doc(1, "a");
  w.root(a);
  w.link(a, a);
  Result<PropertyBag> out = c.duplicate(a);
  test_assert(out.ok());
  const StableRef *ref = out->get("block")->tryPointer();
  test_assert(ref && ref->owner.kind == OwnerKey::Kind::Block);
  const BlockId id = ref->owner.block;
  const BlockHandle copy = w.lib.find(id);
  test_assert(copy && copy != a && w.data(copy)->mesh.target == id);
  test_assert(w.lib.get(copy)->pins(PinKind::History) == 1);
  test_assert(c.ex.undo(c.ctx).ok() && w.lib.get(copy)->state() == BlockState::InHistory);
  test_assert(c.ex.redo(c.ctx).ok() && w.lib.get(copy)->state() == BlockState::Live);
  test_assert(*w.lib.collect() == 0); /* the history's */
  c.history.clear();
  test_assert(w.lib.get(copy) && *w.lib.collect() == 1);

  /* Undone and dropped: destroyed. A failed or refused one leaves nothing. */
  test_assert(c.duplicate(a).ok() && c.ex.undo(c.ctx).ok() && w.lib.size() == 2);
  c.history.clear();
  test_assert(w.lib.size() == 1);
  w.docs.fault = DocFault::Error;
  test_assert(hasMsg(c.duplicate(a).status(), "no copies today"));
  w.docs.fault = DocFault::None;
  test_assert(c.duplicate(BlockHandle()).status().code() ==
              StatusCode::FailedPrecondition);
  test_assert(w.lib.size() == 1 && c.history.size() == 0);
}

static void test_append()
{
  const Bytes bytes = sample();
  CommandWorld c;
  IoWorld &w = c.w;
  Result<AppendResult> r = appendUndo(c.ctx, view(bytes));
  test_assert(r.ok() && r->report.records == 2 && r->report.blocks.size() == 3 &&
              r->ids.size() == 2);
  test_assert(c.history.size() == 1 && c.history.memoryUsed() >= bytes.size());
  const BlockHandle a = r->report.blocks[0], b = r->report.blocks[1],
                    missing = r->report.blocks[2];
  test_assert(w.lib.size() == 3 && w.lib.get(missing)->pins(PinKind::History) == 1);
  test_assert(c.ex.undo(c.ctx).ok());
  test_assert(w.lib.get(a)->state() == BlockState::InHistory &&
              w.lib.get(b)->state() == BlockState::InHistory);
  /* The missing block stays, so a redo finds it. A purge keeps it too. */
  test_assert(w.lib.get(missing)->state() == BlockState::Placeholder &&
              *w.lib.collect() == 0);
  test_assert(c.ex.redo(c.ctx).ok() && w.lib.get(a)->state() == BlockState::Live);

  /* The host fills it, then the append is undone: the host's block stays. */
  const BlockId hostId = w.id(missing);
  Doc *d = litestl::alloc::New<Doc>("Doc");
  d->bag = makeDocBag(w.docs);
  test_assert(w.lib.createWithId(kAcmeDoc, hostId, "host's", d).ok());
  test_assert(c.ex.undo(c.ctx).ok() && w.lib.get(missing)->state() == BlockState::Live);
  c.history.clear();
  test_assert(w.lib.size() == 1 && w.lib.get(missing)->pins() == 0);

  /* Kept, when it isn't undone. */
  r = appendUndo(c.ctx, view(bytes));
  test_assert(r.ok() && r->report.blocks.size() == 2);
  c.history.clear();
  test_assert(w.lib.size() == 3 && w.lib.get(r->report.blocks[0])->pins() == 0);

  Bytes damaged = bytes;
  damaged[60] ^= 1;
  test_assert(appendUndo(c.ctx, view(damaged)).status().code() ==
              StatusCode::InvalidArgument);
  test_assert(c.history.size() == 0 && w.lib.size() == 3);
  AppendOptions options;
  options.limits.maxBlocks = 1;
  test_assert(appendUndo(c.ctx, view(bytes), options).status().code() ==
              StatusCode::OutOfRange);
  /* The history's budget sees the file's size. */
  {
    IoWorld big;
    std::string name; /* that lz4 makes little of */
    for (uint32_t i = 0, x = 1; i < 20000; i++) {
      x = x * 1664525u + 1013904223u;
      name += char('!' + (x >> 24) % 90);
    }
    const BlockHandle h = big.doc(1, name.c_str());
    big.root(h);
    const Bytes file = big.save();
    const uint64_t used = c.history.memoryUsed();
    test_assert(file.size() > 10000 && appendUndo(c.ctx, view(file)).ok());
    test_assert(c.history.memoryUsed() >= used + file.size());
    c.history.clear();
  }
  Context noEx(&w.rt, &w.lib, PointerRef());
  test_assert(appendUndo(noEx, view(bytes)).status().code() ==
              StatusCode::FailedPrecondition);
  Context noLib(&w.rt, nullptr, PointerRef());
  noLib.setExecutor(&c.ex);
  test_assert(hasMsg(appendUndo(noLib, view(bytes)).status(), "no Library"));
  test_assert(hasMsg(appendLibrary(noLib, view(bytes)).status(), "no Library"));
  /* The bare command has nothing to append. */
  test_assert(
      c.ex.run(c.ctx,
               kAppendLibraryCommand,
               w.rt.ext<CommandRegistry>().find(kAppendLibraryCommand)->newInputs())
          .status()
          .code() == StatusCode::FailedPrecondition);
  test_assert(!w.rt.hasStickyError());
}

/* Appending notifies each block it made. */
static void test_notifications()
{
  CommandWorld c;
  int heard = 0;
  ScopedConnection l = c.w.rt.ext<ChangeBus>().addListener([&](const OwnerKey *owner) {
    heard += owner && c.w.lib.find(owner->block) ? 1 : 0;
  });
  test_assert(appendLibrary(c.ctx, view(sample())).ok() && heard == 3);
}

/* The library goes before the history: nothing dangles. */
static void test_library_first()
{
  LinearHistory history;
  {
    IoWorld w;
    CommandExecutor ex(&w.rt, &history);
    Context ctx(&w.rt, &w.lib, PointerRef());
    ctx.setExecutor(&ex);
    test_assert(registerBlockCommands(w.rt).ok());
    Library *lib = litestl::alloc::New<Library>("Library", &w.rt);
    Context other(&w.rt, lib, PointerRef());
    other.setExecutor(&ex);
    test_assert(appendUndo(other, view(sample())).ok() && ex.undo(other).ok());
    litestl::alloc::Delete(lib);
    test_assert(ex.redo(other).code() == StatusCode::FailedPrecondition);
    history.clear();
  }
}

int main()
{
  test_duplicate();
  test_append();
  test_notifications();
  test_library_first();
  return test_end();
}
