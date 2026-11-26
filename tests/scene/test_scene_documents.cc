/* Several Libraries with one executor: the scene's commands, undone or redone through
 * another one's Context, or when their own is gone, are refused and change nothing. */
#include "command_doc.h"

test_init;

using namespace scene_test;

namespace {

struct Documents : CommandDoc {
  std::unique_ptr<Library> two;
  std::unique_ptr<Context> in2;
  BlockHandle top, mid, low;

  Documents()
  {
    top = object("top", 1.0);
    mid = object("mid", 2.0);
    low = object("low", 3.0);
    setParent(mid, top);
    setParent(low, mid);
    select({mid});
    /* The same ids, as if the same file had been opened twice. */
    two = std::make_unique<Library>(&rt);
    for (BlockHandle h : {top, mid, low}) {
      Object *o = litestl::alloc::New<Object>("coredcc::Object", &rt);
      o->setTransformType(kTrsTransform);
      o->channels = trs->defaults();
      must(two->createWithId(TypeId::of(kObjectType), idOf(h), "other", o).status());
    }
    Result<BlockHandle> s =
        two->createWithId(TypeId::of(kSceneType),
                          idOf(scene),
                          "other",
                          litestl::alloc::New<Scene>("coredcc::Scene"));
    must(s.status());
    two->setFlag(*s, BlockFlags::Root, true);
    for (BlockHandle h : {top, mid}) {
      must(Scene::of(*two, *s)->add(*two, two->find(idOf(h))));
    }
    in2 = std::make_unique<Context>(&rt, two.get(), ctx->root());
    in2->setScene(*s);
    in2->setExecutor(ex.get());
  }
  ~Documents()
  {
    history->clear();
    in2.reset();
    two.reset();
  }
  std::string other()
  {
    std::swap(lib, two);
    std::string out = describe();
    std::swap(lib, two);
    return out;
  }
  void drag()
  {
    const CommandDef *def = rt.ext<CommandRegistry>().find(kDragObjectDeltaCommand);
    must(ex->beginInteractive(*ctx, *def, def->newInputs()));
    must(ex->interactiveCommand()->as<ApplyDelta>()->update(*ctx, moved()));
    must(ex->commit(*ctx));
  }
  static TransformDelta moved()
  {
    TransformDelta d;
    d.vector = double3(1.0, 2.0, 3.0);
    return d;
  }
};

struct Case {
  const char *name;
  std::function<void(Documents &)> run;
};

std::vector<Case> cases()
{
  return {
      {kNewSceneCommand, [](Documents &d) { d.ok(kNewSceneCommand); }},
      {kLinkCommand,
       [](Documents &d) {
         Result<BlockHandle> h = newObject(*d.lib, "loose", kTrsTransform);
         must(h.status());
         must(d.two
                  ->createWithId(TypeId::of(kObjectType),
                                 d.idOf(*h),
                                 "other",
                                 litestl::alloc::New<Object>("coredcc::Object", &d.rt))
                  .status());
         d.ok(kLinkCommand, {{"object", d.ptr(*h)}});
       }},
      {kUnlinkCommand, [](Documents &d) { d.ok(kUnlinkCommand); }},
      {kSetActiveCommand,
       [](Documents &d) { d.ok(kSetActiveCommand, {{"object", d.ptr(d.top)}}); }},
      {kSelectCommand,
       [](Documents &d) {
         d.ok(kSelectCommand, {{"mode", PropValue::ofString("NONE")}});
       }},
      {kAddObjectCommand, [](Documents &d) { d.ok(kAddObjectCommand); }},
      {kSetParentCommand,
       [](Documents &d) {
         d.ok(kSetParentCommand, {{"parent", PropValue::nullPointer()}});
       }},
      {kSetTransformTypeCommand,
       [](Documents &d) {
         d.ok(kSetTransformTypeCommand,
              {{"type", PropValue::ofString(kMatrixTransform)}});
       }},
      {kSetRotationModeCommand,
       [](Documents &d) {
         d.ok(kSetRotationModeCommand, {{"mode", PropValue::ofString("QUATERNION")}});
       }},
      {kApplyObjectDeltaCommand,
       [](Documents &d) {
         d.ok(kApplyObjectDeltaCommand,
              {{"vector",
                PropValue::ofComponents(
                    PropValue::Type::Vec3,
                    std::initializer_list<double>{1.0, 2.0, 3.0}.begin())}});
       }},
      {kDragObjectDeltaCommand, [](Documents &d) { d.drag(); }},
      {kDeleteObjectsCommand, [](Documents &d) { d.ok(kDeleteObjectsCommand); }},
  };
}

bool says(const Status &s, const char *what)
{
  return isError(s, StatusCode::FailedPrecondition, what) ||
         (fprintf(stderr, "  %s\n", s.message()), false);
}

void testOther()
{
  for (const Case &c : cases()) {
    for (bool redo : {false, true}) {
      Documents d;
      c.run(d);
      if (redo) {
        d.undo();
      }
      const std::string own = d.describe(), other = d.other();
      const uint64_t epoch = d.two->epoch();
      const Status s = redo ? d.ex->redo(*d.in2) : d.ex->undo(*d.in2);
      if (!says(s, "in another Library")) {
        fprintf(stderr, "%s, redo %d\n", c.name, int(redo));
        test_assert(false);
      }
      /* The command leaves the history, so blocks that were only there are gone. */
      test_assert(CommandDoc::liveOnly(d.describe()) == CommandDoc::liveOnly(own));
      test_assert(d.other() == other && d.two->epoch() == epoch);
      must(d.two->validate());
      must(d.lib->validate());
      test_assert(!d.rt.hasStickyError());
    }
  }
}

void testGone()
{
  for (const Case &c : cases()) {
    for (int when = 0; when < 3; when++) {
      Documents d;
      c.run(d);
      if (when == 1) {
        d.undo();
      }
      const std::string other = d.other();
      d.lib.reset();
      Context gone(&d.rt, nullptr, d.ctx->root());
      gone.setExecutor(d.ex.get());
      if (when < 2) {
        const Status s = when ? d.ex->redo(gone) : d.ex->undo(gone);
        if (!says(s, "Library is gone")) {
          fprintf(stderr, "%s, %d\n", c.name, when);
          test_assert(false);
        }
      }
      d.history->clear();
      test_assert(d.other() == other && !d.rt.hasStickyError());
    }
  }
}

/* A drag that goes on through another Context. */
void testDrag()
{
  Documents d;
  const CommandDef *def = d.rt.ext<CommandRegistry>().find(kDragObjectDeltaCommand);
  must(d.ex->beginInteractive(*d.ctx, *def, def->newInputs()));
  ApplyDelta *drag = d.ex->interactiveCommand()->as<ApplyDelta>();
  const std::string own = d.describe(), other = d.other();
  test_assert(says(drag->update(*d.in2, Documents::moved()), "in another Library"));
  test_assert(d.describe() == own && d.other() == other);
  must(drag->update(*d.ctx, Documents::moved()));
  test_assert(says(drag->update(*d.in2, Documents::moved()), "in another Library"));
  must(d.ex->commit(*d.ctx));

  /* And the values themselves. */
  ObjectChanges changes;
  const ChannelBlock defaults = d.trs->defaults();
  ObjectChanges::Values v;
  v.channels = &defaults;
  must(changes.write(*d.ctx, d.top, v));
  const std::string now = d.describe();
  test_assert(
      says(changes.write(*d.in2, d.two->find(d.idOf(d.mid)), v), "in another Library"));
  test_assert(says(changes.rewrite(*d.in2, 0, defaults), "in another Library"));
  test_assert(says(changes.redo(*d.in2), "in another Library"));
  changes.undo(*d.in2);
  test_assert(says(d.rt.takeStickyError(), "in another Library"));
  test_assert(d.describe() == now && d.other() == other && changes.size() == 1);
  /* Nothing was written: nothing to undo, anywhere. */
  ObjectChanges none;
  none.undo(*d.in2);
  must(none.redo(*d.in2));
  test_assert(!d.rt.hasStickyError());
}

/* The size that the history counts grows while blocks are undone. */
void testSize()
{
  for (const char *command : {kNewSceneCommand, kAddObjectCommand}) {
    Documents d;
    d.ok(command);
    const uint64_t before = d.history->memoryUsed();
    d.undo();
    test_assert(d.history->memoryUsed() > before);
  }
}

} // namespace

int main()
{
  testOther();
  testGone();
  testDrag();
  testSize();
  return test_end();
}
