/* Snapshot undo: before and after, sub-commands, failures, folding, interactive commands, the
 * budget, and the blocks a blob references. */
#include "command_fixture.h"

#include "coredcc/command/snapshot_undo.h"

#include <set>

test_init;

using litestl::util::Vector;

namespace {

/* State only a snapshot can take back: a number, padding that compresses, and a ref. */
struct State {
  Runtime *rt = nullptr;
  int value = 4;
  int padding = 0;
  bool random = false;
  BlockId ref;
  int foreachs = 0, captures = 0, restores = 0, alive = 0;
  bool failRestore = false, throwRestore = false, throwCapture = false, throwForeach = false;
};

template <bool Refs> struct StateProvider {
  State *s = nullptr;
  ~StateProvider()
  {
    s->alive--;
  }
  Vector<uint8_t> capture() const
  {
    s->captures++;
    if (s->throwCapture) {
      s->rt->setStickyError(Status::error(StatusCode::Internal, "capture threw"));
      return {};
    }
    BytesWriter out;
    out.u32(uint32_t(s->value));
    out.u64(s->ref.client.value);
    out.u64(s->ref.local.hi);
    out.u64(s->ref.local.lo);
    uint64_t seed = 1;
    for (int i = 0; i < s->padding; i++) {
      seed = seed * 6364136223846793005ull + 1442695040888963407ull;
      out.u8(s->random ? uint8_t(seed >> 56) : 7);
    }
    return out.take();
  }
  Status restore(const Vector<uint8_t> &blob)
  {
    s->restores++;
    if (s->failRestore) {
      return Status::error(StatusCode::Unsupported, "can't restore");
    }
    if (s->throwRestore) {
      s->rt->setStickyError(Status::error(StatusCode::Internal, "restore threw"));
      return Status();
    }
    BytesReader in(std::span<const uint8_t>(blob.data(), size_t(blob.size())));
    s->value = int(in.u32());
    s->ref.client.value = in.u64();
    s->ref.local.hi = in.u64();
    s->ref.local.lo = in.u64();
    test_assert(int(in.remaining()) == s->padding);
    return in.status();
  }
  void foreachRef(const Vector<uint8_t> &blob, RefVisitor &v) const
  {
    s->foreachs++;
    if (s->throwForeach) {
      s->rt->setStickyError(Status::error(StatusCode::Internal, "foreachRef threw"));
      return;
    }
    BytesReader in(std::span<const uint8_t>(blob.data(), size_t(blob.size())));
    in.u32();
    RefCell c(RefKind::Strong);
    c.target.client.value = in.u64();
    c.target.local.hi = in.u64();
    c.target.local.lo = in.u64();
    v.visit(c);
    v.visit(c); /* pinned once */
  }
  static lbt::Struct<StateProvider> *defineBindings()
  {
    auto *st = new lbt::Struct<StateProvider>(Refs ? "test::StateProvider" : "test::PlainState",
                                              sizeof(StateProvider));
    BIND_STRUCT_METHOD(st, capture, MARGS());
    BIND_STRUCT_METHOD(st, restore, MARGS("blob"));
    if constexpr (Refs) {
      BIND_STRUCT_METHOD(st, foreachRef, MARGS("blob", "visitor"));
    }
    return st;
  }
};

State *g_state = nullptr; /* what the commands below edit */

/* exec: value = 100, then a sub-command adds 9. */
enum class Fail { None, BeforeSub, AfterSub, AtEnd };
Fail g_fail = Fail::None;
bool g_noEnd = false;
bool g_fold = false;

struct AddNine {
  int before = 0, after = 0;
  Status canRun(Context &) const
  {
    return Status();
  }
  Status exec(Context &)
  {
    before = g_state->value;
    after = g_state->value += 9;
    return Status();
  }
  void undo(Context &)
  {
    g_state->value = before;
  }
  Status redo(Context &)
  {
    g_state->value = after;
    return Status();
  }
  uint64_t memSize() const
  {
    return 8;
  }
  void discard() {}
  static lbt::Struct<AddNine> *defineBindings()
  {
    auto *st = new lbt::Struct<AddNine>("test::AddNine", sizeof(AddNine));
    BIND_COMMAND_CORE(st);
    BIND_STRUCT_METHOD(st, redo, MARGS("ctx"));
    return st;
  }
};

struct Opaque {
  SnapshotUndo snap{"state"};
  Status canRun(Context &) const
  {
    return Status();
  }
  Status exec(Context &ctx)
  {
    CDCC_TRY(snap.begin(ctx));
    g_state->value = 100;
    if (g_fail == Fail::BeforeSub) {
      CDCC_TRY(snap.rollback(ctx));
      return Status::error(StatusCode::Cancelled, "failed early");
    }
    const CommandDef *nine = ctx.runtime().ext<CommandRegistry>().find("test.addNine");
    Result<PropertyBag> sub = ctx.executor()->runSub(ctx, *nine, nine->newInputs());
    Status s = sub.status();
    if (g_fail == Fail::AfterSub) {
      s = Status::error(StatusCode::Cancelled, "failed late");
    }
    if (g_fail == Fail::AtEnd) {
      g_state->throwCapture = true;
    }
    if (s.ok() && !g_noEnd) {
      s = snap.end(ctx);
    }
    if (!s.ok()) {
      CDCC_TRY(snap.rollback(ctx));
    }
    return s;
  }
  void undo(Context &ctx)
  {
    snap.undo(ctx);
  }
  Status redo(Context &ctx)
  {
    return snap.redo(ctx);
  }
  uint64_t memSize() const
  {
    return snap.memSize();
  }
  void discard()
  {
    snap.discard();
  }
  uint64_t foldKey() const
  {
    return g_fold ? 77 : 0;
  }
  bool foldFrom(Command next)
  {
    Opaque *o = next.as<Opaque>();
    return o && snap.foldFrom(o->snap);
  }
  static lbt::Struct<Opaque> *defineBindings()
  {
    auto *st = new lbt::Struct<Opaque>("test::Opaque", sizeof(Opaque));
    BIND_COMMAND_CORE(st);
    BIND_STRUCT_METHOD(st, redo, MARGS("ctx"));
    BIND_STRUCT_METHOD(st, foldKey, MARGS());
    BIND_STRUCT_METHOD(st, foldFrom, MARGS("next"));
    return st;
  }
};

/* Interactive, and foldable when run: sets a value. */
struct SnapDrag {
  SnapshotUndo snap{"state"};
  Status canRun(Context &) const
  {
    return Status();
  }
  Status exec(Context &ctx)
  {
    CDCC_TRY(snap.begin(ctx));
    g_state->value++;
    return Status();
  }
  void undo(Context &ctx)
  {
    snap.undo(ctx);
  }
  Status redo(Context &ctx)
  {
    return snap.redo(ctx);
  }
  uint64_t memSize() const
  {
    return snap.memSize();
  }
  void discard()
  {
    snap.discard();
  }
  static lbt::Struct<SnapDrag> *defineBindings()
  {
    auto *st = new lbt::Struct<SnapDrag>("test::SnapDrag", sizeof(SnapDrag));
    BIND_COMMAND_CORE(st);
    BIND_STRUCT_METHOD(st, redo, MARGS("ctx"));
    return st;
  }
};

/* Foldable, without sub-commands. */
struct Step {
  SnapshotUndo snap{"state"};
  Status canRun(Context &) const
  {
    return Status();
  }
  Status exec(Context &ctx)
  {
    CDCC_TRY(snap.begin(ctx));
    g_state->value++;
    return g_noEnd ? Status() : snap.end(ctx);
  }
  void undo(Context &ctx)
  {
    snap.undo(ctx);
  }
  Status redo(Context &ctx)
  {
    return snap.redo(ctx);
  }
  uint64_t memSize() const
  {
    return snap.memSize();
  }
  void discard()
  {
    snap.discard();
  }
  uint64_t foldKey() const
  {
    return 78;
  }
  bool foldFrom(Command next)
  {
    return snap.foldFrom(next.as<Step>()->snap);
  }
  static lbt::Struct<Step> *defineBindings()
  {
    auto *st = new lbt::Struct<Step>("test::SnapStep", sizeof(Step));
    BIND_COMMAND_CORE(st);
    BIND_STRUCT_METHOD(st, redo, MARGS("ctx"));
    BIND_STRUCT_METHOD(st, foldKey, MARGS());
    BIND_STRUCT_METHOD(st, foldFrom, MARGS("next"));
    return st;
  }
};

template <bool Refs = true> Owned<SnapshotProvider> makeProvider(Runtime &rt, State &s)
{
  using P = StateProvider<Refs>;
  const char *impl = Refs ? "test.state" : "test.plainState";
  registerImpl<SnapshotProvider, P>(rt, impl).ignore(); /* again is fine */
  P *p = litestl::alloc::New<P>("provider");
  p->s = &s;
  s.alive++;
  s.rt = &rt;
  Result<Owned<SnapshotProvider>> owned = own<SnapshotProvider>(rt, impl, p);
  test_assert(owned.ok());
  return std::move(owned).value();
}

struct SnapWorld {
  State state;
  Setup s;
  SnapWorld()
  {
    g_state = &state;
    g_fail = Fail::None;
    g_noEnd = g_fold = false;
    Runtime &rt = s.w.rt;
    test_assert(rt.ext<SnapshotRegistry>().add("state", makeProvider(rt, state)).ok());
    test_assert(addNativeCommand<AddNine>(rt, CommandDef("test.addNine")).ok());
    test_assert(addNativeCommand<Opaque>(rt, CommandDef("test.opaque")).ok());
    test_assert(addNativeCommand<Step>(rt, CommandDef("test.step")).ok());
    CommandDef drag("test.snapDrag");
    drag.flags(CommandFlags::Interactive);
    test_assert(addNativeCommand<SnapDrag>(rt, std::move(drag)).ok());
  }
  Status run(const char *id)
  {
    const CommandDef *def = s.w.rt.ext<CommandRegistry>().find(id);
    return s.ex.run(s.ctx, *def, def->newInputs()).status();
  }
};

} // namespace

static void test_registry()
{
  Runtime rt;
  State s;
  {
    SnapshotRegistry &reg = rt.ext<SnapshotRegistry>();
    test_assert(reg.add("a", makeProvider(rt, s)).ok() && reg.add("b", makeProvider(rt, s)).ok());
    test_assert(reg.add("a", makeProvider(rt, s)).code() == StatusCode::AlreadyExists);
    test_assert(s.alive == 2);
    test_assert(reg.add("", makeProvider(rt, s)).code() == StatusCode::InvalidArgument);
    test_assert(reg.add("c", Owned<SnapshotProvider>()).code() == StatusCode::InvalidArgument);
    test_assert(reg.find("a") && reg.find("b") && !reg.find("c") && reg.find("a") != reg.find("b"));
    test_assert(reg.remove("a").ok() && s.alive == 1 && !reg.find("a") && reg.find("b"));
    test_assert(reg.remove("a").code() == StatusCode::NotFound);
  }
  g_state = &s;
}

static void test_undo()
{
  SnapWorld w;
  ChangeBus &bus = w.s.w.rt.ext<ChangeBus>();
  int all = 0;
  ScopedConnection l = bus.addListener([&](const OwnerKey *owner) { all += owner ? 0 : 1; });
  test_assert(w.run("test.opaque").ok() && w.state.value == 109 && w.state.captures == 2);
  test_assert(all == 0);
  const uint64_t libEpoch = w.s.w.lib.epoch();
  const uint64_t busEpoch = bus.epoch();
  test_assert(w.s.ex.undo(w.s.ctx).ok() && w.state.value == 4);
  test_assert(all == 1 && w.s.w.lib.epoch() > libEpoch && bus.epoch() > busEpoch);
  test_assert(w.s.ex.redo(w.s.ctx).ok() && w.state.value == 109 && all == 2);
  test_assert(w.s.ex.undo(w.s.ctx).ok() && w.state.value == 4);

  /* A failing exec: what ran before the failure is undone, sub-commands included. */
  for (Fail f : {Fail::BeforeSub, Fail::AfterSub}) {
    g_fail = f;
    const int before = all;
    test_assert(w.run("test.opaque").code() == StatusCode::Cancelled && w.state.value == 4);
    test_assert(all > before);
  }
  g_fail = Fail::AtEnd;
  test_assert(hasMsg(w.run("test.opaque"), "capture threw") && w.state.value == 4);
  w.state.throwCapture = false;
  g_fail = Fail::None;

  /* Without an end: undo works, redo doesn't. */
  g_noEnd = true;
  test_assert(w.run("test.opaque").ok());
  test_assert(w.s.ex.undo(w.s.ctx).ok() && w.state.value == 4);
  test_assert(hasMsg(w.s.ex.redo(w.s.ctx), "redo: no snapshot was taken"));
  test_assert(!w.s.history.canRedo());
}

static void test_failures()
{
  SnapWorld w;
  w.state.throwCapture = true;
  test_assert(hasMsg(w.run("test.opaque"), "capture threw") && w.state.value == 4);
  test_assert(w.state.foreachs == 0); /* no blob to look at */
  w.state.throwCapture = false;
  w.state.throwForeach = true;
  test_assert(hasMsg(w.run("test.opaque"), "foreachRef threw") && w.state.value == 4);
  w.state.throwForeach = false;

  int all = 0;
  ScopedConnection l = w.s.w.rt.ext<ChangeBus>().addListener([&](const OwnerKey *) { all++; });
  test_assert(w.run("test.opaque").ok());
  w.state.failRestore = true;
  test_assert(hasMsg(w.s.ex.undo(w.s.ctx), "can't restore") && all == 1); /* it may have changed */
  w.state.failRestore = false;
  test_assert(w.s.history.size() == 0);
  w.state.value = 4;
  test_assert(w.run("test.opaque").ok() && w.s.ex.undo(w.s.ctx).ok());
  w.state.throwRestore = true;
  test_assert(hasMsg(w.s.ex.redo(w.s.ctx), "restore threw"));
  w.state.throwRestore = false;
  test_assert(!w.s.w.rt.hasStickyError());

  /* A rollback that fails at the discard reports through the executor. */
  g_fail = Fail::AfterSub;
  w.state.value = 4;
  w.state.failRestore = true;
  test_assert(hasMsg(w.run("test.opaque"), "can't restore"));
  w.state.failRestore = false;
  g_fail = Fail::None;

  /* The provider went. */
  w.state.value = 4;
  test_assert(w.run("test.opaque").ok());
  test_assert(w.s.w.rt.ext<SnapshotRegistry>().remove("state").ok());
  test_assert(hasMsg(w.s.ex.undo(w.s.ctx), "no snapshot provider 'state' is registered"));
  test_assert(hasMsg(w.run("test.opaque"), "no snapshot provider 'state'"));

  /* Out of order. */
  SnapWorld w2;
  SnapshotUndo s("state");
  test_assert(hasMsg(s.end(w2.s.ctx), "end: no snapshot"));
  test_assert(hasMsg(s.redo(w2.s.ctx), "redo: no snapshot"));
  test_assert(hasMsg(s.rollback(w2.s.ctx), "rollback: no snapshot"));
  s.undo(w2.s.ctx);
  test_assert(hasMsg(w2.s.w.rt.takeStickyError(), "undo: no snapshot"));
  s.discard();
  /* begin again starts over. */
  test_assert(s.begin(w2.s.ctx).ok() && s.end(w2.s.ctx).ok());
  w2.state.value = 50;
  test_assert(s.begin(w2.s.ctx).ok());
  w2.state.value = 60;
  test_assert(s.redo(w2.s.ctx).code() == StatusCode::FailedPrecondition);
  s.undo(w2.s.ctx);
  test_assert(w2.state.value == 50);
  w2.state.value = 70;
  test_assert(s.end(w2.s.ctx).ok());
  w2.state.value = 80;
  test_assert(s.end(w2.s.ctx).ok()); /* the last one counts */
  w2.state.value = 90;
  test_assert(s.redo(w2.s.ctx).ok() && w2.state.value == 80);
  /* After a rollback and a discard, nothing happens again. */
  test_assert(s.rollback(w2.s.ctx).ok() && w2.state.value == 50);
  w2.state.value = 1;
  const uint64_t epoch = w2.s.w.lib.epoch();
  s.discard();
  test_assert(w2.state.value == 50 && w2.s.w.lib.epoch() > epoch);
  w2.state.value = 2;
  s.discard();
  test_assert(w2.state.value == 2);
  test_assert(s.begin(w2.s.ctx).ok() && s.rollback(w2.s.ctx).ok());
  w2.state.failRestore = true;
  s.discard();
  test_assert(hasMsg(w2.s.w.rt.takeStickyError(), "can't restore"));
  w2.state.failRestore = false;
  /* A begin forgets a rollback. */
  test_assert(s.begin(w2.s.ctx).ok() && s.rollback(w2.s.ctx).ok() && s.begin(w2.s.ctx).ok());
  w2.state.value = 3;
  s.discard();
  test_assert(w2.state.value == 3);
}

static void test_folding()
{
  SnapWorld w;
  for (int i = 0; i < 3; i++) {
    test_assert(w.run("test.step").ok());
  }
  test_assert(w.state.value == 7 && w.s.history.size() == 1);
  test_assert(w.s.ex.undo(w.s.ctx).ok() && w.state.value == 4);
  test_assert(w.s.ex.redo(w.s.ctx).ok() && w.state.value == 7);
  /* A step without an end isn't folded. */
  test_assert(w.s.ex.undo(w.s.ctx).ok());
  g_noEnd = true;
  test_assert(w.run("test.step").ok());
  g_noEnd = false;
  test_assert(w.run("test.step").ok() && w.s.history.size() == 2);
  g_noEnd = true;
  test_assert(w.run("test.step").ok() && w.s.history.size() == 3);
  g_noEnd = false;
  SnapshotUndo a("state"), b("other");
  test_assert(a.begin(w.s.ctx).ok() && a.end(w.s.ctx).ok());
  test_assert(w.s.w.rt.ext<SnapshotRegistry>().add("other", makeProvider(w.s.w.rt, w.state)).ok());
  test_assert(b.begin(w.s.ctx).ok() && b.end(w.s.ctx).ok());
  test_assert(!a.foldFrom(b) && b.redo(w.s.ctx).ok());
}

static void test_interactive()
{
  SnapWorld w;
  const CommandDef *def = w.s.w.rt.ext<CommandRegistry>().find("test.snapDrag");
  const auto begin = [&]() { return w.s.ex.beginInteractive(w.s.ctx, *def, def->newInputs()); };
  test_assert(begin().ok() && w.state.value == 5);
  w.state.value = 30; /* the drag */
  test_assert(w.s.ex.cancel(w.s.ctx).ok() && w.state.value == 4);
  test_assert(begin().ok());
  w.state.value = 31;
  SnapshotUndo &snap = w.s.ex.interactiveCommand()->as<SnapDrag>()->snap;
  test_assert(snap.end(w.s.ctx).ok());
  w.state.value = 32;
  test_assert(snap.end(w.s.ctx).ok() && w.s.ex.commit(w.s.ctx).ok());
  test_assert(w.s.ex.undo(w.s.ctx).ok() && w.state.value == 4);
  test_assert(w.s.ex.redo(w.s.ctx).ok() && w.state.value == 32);
}

static void test_budget()
{
  SnapWorld w;
  w.state.padding = 100000;
  test_assert(w.run("test.opaque").ok());
  const uint64_t small = w.s.history.memoryUsed();
  test_assert(small < 20000); /* compressed */
  test_assert(w.s.ex.undo(w.s.ctx).ok() && w.state.value == 4); /* and read back, 1 byte each */
  w.state.random = true;
  test_assert(w.run("test.opaque").ok());
  test_assert(w.s.history.memoryUsed() > 200000); /* both blobs, raw */
  test_assert(w.s.ex.undo(w.s.ctx).ok() && w.state.value == 4);
  test_assert(w.s.ex.redo(w.s.ctx).ok() && w.state.value == 109);
  w.s.history.setMemoryBudget(300000);
  test_assert(w.run("test.opaque").ok() && w.s.history.size() == 1);
  w.s.history.setMemoryBudget(uint64_t(1) << 30);
  /* An empty blob is a blob. */
  SnapshotUndo s("state");
  SnapshotUndo moved;
  test_assert(s.memSize() > 0 && s.begin(w.s.ctx).ok() && s.memSize() > 100000);
  moved = std::move(s);
  w.state.value = 3;
  moved.undo(w.s.ctx);
  test_assert(w.state.value == 109 && !w.s.w.rt.hasStickyError());
  moved.discard();
  test_assert(moved.memSize() < 1000);
}

static void test_targets()
{
  SnapWorld w;
  Library &lib = w.s.w.lib;
  for (BlockHandle ob : w.s.w.scene.objects) {
    lib.setFlag(ob, BlockFlags::FakeUser, true);
  }
  Result<BlockHandle> a = lib.create(kMeshType, "before", litestl::alloc::New<Mesh>("Mesh"));
  Result<BlockHandle> b = lib.create(kMeshType, "after", litestl::alloc::New<Mesh>("Mesh"));
  w.state.ref = lib.get(*a)->id();
  SnapshotUndo s("state");
  test_assert(s.begin(w.s.ctx).ok() && lib.get(*a)->pins(PinKind::History) == 1);
  w.state.ref = lib.get(*b)->id();
  test_assert(s.end(w.s.ctx).ok() && lib.get(*b)->pins(PinKind::History) == 1);
  const uint64_t size = s.memSize();
  {
    const BlockId ref = w.state.ref;
    SnapshotUndo without("state"), with("state");
    test_assert(with.begin(w.s.ctx).ok() && with.end(w.s.ctx).ok());
    w.state.ref = BlockId();
    test_assert(without.begin(w.s.ctx).ok() && without.end(w.s.ctx).ok());
    test_assert(with.memSize() == without.memSize() + 2 * sizeof(HistoryBlock));
    w.state.ref = ref;
  }
  w.state.ref = BlockId();
  test_assert(lib.collect().ok() && lib.get(*a) && lib.get(*b));
  s.undo(w.s.ctx);
  test_assert(w.state.ref == lib.get(*a)->id());

  /* Folding hands the pins over. */
  SnapshotUndo next("state");
  Result<BlockHandle> c = lib.create(kMeshType, "later", litestl::alloc::New<Mesh>("Mesh"));
  test_assert(next.begin(w.s.ctx).ok());
  w.state.ref = lib.get(*c)->id();
  test_assert(next.end(w.s.ctx).ok() && s.foldFrom(next));
  next.discard();
  test_assert(lib.get(*a)->pins() == 1 && lib.get(*b)->pins() == 0 && lib.get(*c)->pins() == 1);
  w.state.ref = BlockId();
  s.discard();
  test_assert(lib.get(*a)->pins() == 0 && lib.get(*c)->pins() == 0 && s.memSize() < size);
  test_assert(*lib.collect() == 3);

  /* A provider without foreachRef, and a Context without a Library. */
  test_assert(w.s.w.rt.ext<SnapshotRegistry>()
                  .add("plain", makeProvider<false>(w.s.w.rt, w.state))
                  .ok());
  SnapshotUndo plain("plain");
  test_assert(plain.begin(w.s.ctx).ok());
  Context noLib(&w.s.w.rt, nullptr, PointerRef());
  w.state.ref = BlockId{ClientId::of("x"), LocalId{1, 2}}; /* no record */
  SnapshotUndo alone("state");
  test_assert(alone.begin(noLib).ok() && alone.end(noLib).ok() && alone.redo(noLib).ok());
  test_assert(alone.rollback(noLib).ok());
  alone.discard();
  SnapshotUndo lost("state");
  test_assert(lost.begin(w.s.ctx).ok());
}

int main()
{
  test_registry();
  test_undo();
  test_failures();
  test_folding();
  test_interactive();
  test_budget();
  test_targets();
  return test_end();
}
