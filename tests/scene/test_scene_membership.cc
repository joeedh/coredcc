/* The commands that make scenes and change what is in them, selected and active. */
#include "command_doc.h"

test_init;

using namespace scene_test;

namespace {

PropValue text(const char *s)
{
  return PropValue::ofString(s);
}

PropValue mode(int64_t m)
{
  return PropValue::ofEnum(m);
}

enum { kSet, kAdd, kRemove, kToggle, kAll, kNone };

std::string names(CommandDoc &d, const Vector<BlockHandle> &handles)
{
  std::string s;
  for (BlockHandle h : handles) {
    s += d.lib->get(h)->name().c_str();
  }
  return s;
}

std::string members(CommandDoc &d)
{
  return names(d, d.sc().objects(*d.lib));
}

std::string selected(CommandDoc &d)
{
  return names(d, d.sc().selected(*d.lib));
}

void testNew()
{
  CommandDoc d;
  const BlockHandle first = d.scene;
  PropertyBag out = d.cycle(kNewSceneCommand, {{"name", text("Second")}});
  const BlockHandle s = d.blockOut(out, "scene");
  test_assert(Scene::of(*d.lib, s) && d.ctx->scene() == first);
  test_assert(!strcmp(d.lib->get(s)->name().c_str(), "Second"));
  test_assert(d.lib->get(s)->hasFlag(BlockFlags::Root));
  /* Purging keeps it, with its members, and an undone one until the history lets go of it. */
  d.ctx->setScene(s);
  const BlockHandle o = d.object("o");
  d.ctx->setScene(first);
  Result<int> purged = d.lib->collect();
  test_assert(purged.ok() && d.lib->get(s));
  (void)o;
  d.undo();
  test_assert(!Scene::of(*d.lib, s));
  must(d.lib->collect().status());
  d.redo();
  test_assert(Scene::of(*d.lib, s));
  d.undo();
  d.history->clear();
  test_assert(!d.lib->get(s));
  Context none(&d.rt, nullptr, PointerRef());
  const CommandDef *def = d.rt.ext<CommandRegistry>().find(kNewSceneCommand);
  test_assert(!d.ex->run(none, *def, def->newInputs()).ok());
}

void testLink()
{
  CommandDoc d;
  d.object("a");
  const BlockHandle b = d.loose("b"), shape = d.shapeBlock("shape", 1.0);
  d.cycle(kLinkCommand, {{"object", d.ptr(b)}});
  test_assert(members(d) == "ab");
  test_assert(isError(
      d.fails(kLinkCommand, {{"object", d.ptr(b)}}), StatusCode::AlreadyExists, "'b'"));
  test_assert(
      isError(d.fails(kLinkCommand, {{"object", d.ptr(shape)}}), StatusCode::FailedPrecondition));
  test_assert(isError(d.fails(kLinkCommand), StatusCode::FailedPrecondition, "object"));
  must(d.lib->markDeleted(shape));
  test_assert(isError(d.fails(kLinkCommand, {{"object", d.ptr(shape)}}),
                      StatusCode::FailedPrecondition,
                      "no such block"));
  must(d.lib->restoreFromHistory(shape));

  /* Undone, only the history holds it. */
  d.undo();
  test_assert(members(d) == "a");
  must(d.lib->collect().status());
  d.redo();
  test_assert(members(d) == "ab" && d.lib->get(b));
  /* Linked a second time meanwhile. */
  d.undo();
  must(d.sc().add(*d.lib, b));
  test_assert(d.ex->redo(*d.ctx).code() == StatusCode::AlreadyExists);

  /* Without a scene. */
  d.ctx->setScene(BlockHandle());
  for (const char *c : {kLinkCommand, kUnlinkCommand, kSetActiveCommand, kSelectCommand,
                        kAddObjectCommand, kSetParentCommand, kSetTransformTypeCommand,
                        kSetRotationModeCommand, kApplyDeltaCommand, kDeleteObjectsCommand})
  {
    Result<PropertyBag> r = d.run(c);
    test_assert(isError(r.status(), StatusCode::FailedPrecondition, "no scene"));
  }
}

void testUnlink()
{
  CommandDoc d;
  const BlockHandle a = d.object("a"), b = d.object("b"), c = d.object("c"), e = d.object("e"),
                    f = d.object("f");
  test_assert(isError(d.fails(kUnlinkCommand), StatusCode::FailedPrecondition, "selected"));
  for (std::initializer_list<BlockHandle> set : {std::initializer_list<BlockHandle>{a},
                                                 {f},
                                                 {c},
                                                 {a, b},
                                                 {c, b},
                                                 {f, a, e},
                                                 {b, e, c},
                                                 {a, b, c, e, f}})
  {
    d.select(set);
    PropertyBag out = d.cycle(kUnlinkCommand);
    test_assert(out.get("count")->asInt() == int64_t(set.size()));
    test_assert(d.sc().objects(*d.lib).size() == 5 - set.size() && selected(d).empty());
    d.undo();
    test_assert(members(d) == "abcef");
  }

  /* Others moved meanwhile: behind the one it was behind, else the nearest place. */
  d.select({c});
  d.ok(kUnlinkCommand);
  must(d.sc().remove(*d.lib, d.idOf(a)).status());
  must(d.sc().add(*d.lib, a));
  test_assert(members(d) == "befa");
  d.undo();
  test_assert(members(d) == "bcefa");
  d.select({c});
  d.ok(kUnlinkCommand);
  must(d.sc().remove(*d.lib, d.idOf(b)).status());
  d.undo();
  test_assert(members(d) == "ecfa");

  /* The history keeps what the scene doesn't. */
  d.select({c});
  d.ok(kUnlinkCommand);
  must(d.lib->collect().status());
  test_assert(d.lib->get(c) && !d.lib->get(b));
  d.undo();
  test_assert(d.sc().contains(*d.lib, c));
  d.select({c});
  d.ok(kUnlinkCommand);
  d.history->clear();
  must(d.lib->collect().status());
  test_assert(!d.lib->get(c));

  /* An active object that is unlinked isn't active, and is again when it comes back. */
  must(d.lib->writeRef(d.sc().activeCell(), d.idOf(a)));
  d.select({a});
  d.ok(kUnlinkCommand);
  test_assert(!d.sc().active(*d.lib));
  d.undo();
  test_assert(d.sc().active(*d.lib) == a && selected(d) == "a");
}

/* Membership commands drop members without a record. */
void testPrune()
{
  CommandDoc d;
  const BlockHandle a = d.object("a"), b = d.object("b"), c = d.object("c");
  const auto lose = [&](BlockHandle h) {
    must(d.lib->markDeleted(h));
    must(d.lib->destroyFromHistory(h));
  };
  lose(a);
  d.evaluate();
  test_assert(d.sc().cells().size() == 3);
  const BlockHandle e = d.loose("e");
  d.ok(kLinkCommand, {{"object", d.ptr(e)}});
  test_assert(d.sc().cells().size() == 3 && members(d) == "bce");
  lose(b);
  d.ok(kAddObjectCommand, {{"name", text("f")}});
  test_assert(d.sc().cells().size() == 3 && members(d) == "cef");
  d.select({c});
  lose(e);
  d.ok(kUnlinkCommand);
  test_assert(d.sc().cells().size() == 1 && members(d) == "f");
  d.undo();
  test_assert(members(d) == "cf");
}

void testActive()
{
  CommandDoc d;
  const BlockHandle a = d.object("a"), b = d.object("b"), out = d.loose("out");
  d.cycle(kSetActiveCommand, {{"object", d.ptr(a)}});
  test_assert(d.sc().active(*d.lib) == a);
  d.cycle(kSetActiveCommand, {{"object", d.ptr(b)}});
  d.cycle(kSetActiveCommand);
  test_assert(!d.sc().active(*d.lib));
  test_assert(isError(d.fails(kSetActiveCommand, {{"object", d.ptr(out)}}),
                      StatusCode::FailedPrecondition,
                      "'out'"));
  d.undo();
  test_assert(d.sc().active(*d.lib) == b);

  /* Delete the active object, set another, undo both: by the command and by its path. */
  for (int path = 0; path < 2; path++) {
    must(d.lib->writeRef(d.sc().activeCell(), d.idOf(b)));
    d.ok(kDeleteBlockCommand, {{"block", d.ptr(b)}});
    test_assert(!d.sc().active(*d.lib));
    if (path) {
      must(setValueUndo(*d.ctx, "scene.objects.active", d.ptr(a)));
    }
    else {
      d.ok(kSetActiveCommand, {{"object", d.ptr(a)}});
    }
    test_assert(d.sc().active(*d.lib) == a);
    d.undo();
    test_assert(!d.sc().active(*d.lib));
    d.undo();
    test_assert(d.sc().active(*d.lib) == b);
    /* The same with one that its host took away. */
    must(d.lib->onHostDestroyed(d.idOf(b)));
    if (path) {
      must(setValueUndo(*d.ctx, "scene.activeObject", d.ptr(a)));
    }
    else {
      d.ok(kSetActiveCommand, {{"object", d.ptr(a)}});
    }
    d.undo();
    must(d.lib->onHostRestored(d.idOf(b)));
    test_assert(d.sc().active(*d.lib) == b);
  }

  /* Watchers. */
  ChangeBus &bus = d.rt.ext<ChangeBus>();
  int fired = 0;
  Result<ScopedConnection> w = bus.watch(
      *d.ctx, "scene.activeObject", [&](const PropValue &, const Status &) { fired++; });
  must(w.status());
  d.ok(kSetActiveCommand, {{"object", d.ptr(a)}});
  bus.flush();
  test_assert(fired == 1);
  d.undo();
  bus.flush();
  test_assert(fired == 2);
}

void testSelect()
{
  CommandDoc d;
  const BlockHandle a = d.object("a"), b = d.object("b"), c = d.object("c"), out = d.loose("out");
  const auto select = [&](int m, BlockHandle h = BlockHandle()) {
    d.cycle(kSelectCommand, {{"mode", mode(m)}, {"object", d.ptr(h)}});
    return selected(d);
  };
  test_assert(select(kSet, b) == "b");
  test_assert(select(kSet, c) == "c");
  test_assert(select(kAdd, a) == "ca" && select(kAdd, a) == "ca");
  test_assert(select(kToggle, c) == "a" && select(kToggle, c) == "ac");
  test_assert(select(kRemove, a) == "c" && select(kRemove, a) == "c");
  test_assert(select(kAll) == "abc" && select(kNone).empty());
  test_assert(select(kAll) == "abc" && select(kSet).empty());

  d.obj(a).flags = ObjectFlags::Hidden;
  d.obj(b).flags = ObjectFlags::Unselectable;
  test_assert(select(kAll) == "c" && select(kAdd, a) == "ca");
  for (int m : {int(kSet), int(kAdd), int(kToggle)}) {
    test_assert(isError(d.fails(kSelectCommand, {{"mode", mode(m)}, {"object", d.ptr(b)}}),
                        StatusCode::FailedPrecondition,
                        "'b': isn't selectable"));
    test_assert(isError(d.fails(kSelectCommand, {{"mode", mode(m)}, {"object", d.ptr(out)}}),
                        StatusCode::FailedPrecondition,
                        "'out': isn't in the scene"));
  }
  test_assert(select(kRemove, b) == "ca");
  test_assert(isError(d.fails(kSelectCommand, {{"mode", mode(kAdd)}}),
                      StatusCode::FailedPrecondition));

  /* What was selected and is deleted comes back with undo, also after another selection. */
  d.ok(kDeleteBlockCommand, {{"block", d.ptr(c)}});
  test_assert(selected(d) == "a");
  d.obj(b).flags = ObjectFlags::None;
  test_assert(select(kSet, b) == "b");
  d.undo();
  d.undo();
  test_assert(selected(d) == "ca");
}

void testAdd()
{
  CommandDoc d;
  PropertyBag out = d.cycle(kAddObjectCommand,
                            {{"name", text("a")}, {"location", PropValue::ofVec3(1, 2, 3)}});
  const BlockHandle a = d.blockOut(out, "object");
  d.evaluate();
  test_assert(members(d) == "a" && d.isAt(a, 1, 2, 3));
  test_assert(d.obj(a).transformTypeId() == d.trs->id());

  /* In the world, under a parent. */
  put(d.obj(a).channels, trs::kScale, double3(2, 4, 8));
  for (const char *type : {kTrsTransform, kFbxTransform, kMatrixTransform}) {
    out = d.cycle(kAddObjectCommand,
                  {{"type", text(type)},
                   {"name", text("b")},
                   {"location", PropValue::ofVec3(5, 6, 7)},
                   {"parent", d.ptr(a)}});
    const BlockHandle b = d.blockOut(out, "object");
    test_assert(d.lib->resolveRef(d.obj(b).parent.cell()) == a);
    d.evaluate();
    test_assert(d.isAt(b, 5, 6, 7));
  }
  test_assert(members(d) == "abbb");

  /* Refusals leave nothing behind. */
  const BlockHandle outside = d.loose("outside");
  test_assert(isError(d.fails(kAddObjectCommand, {{"type", text("test.unknown")}}),
                      StatusCode::NotFound,
                      "test.unknown"));
  test_assert(isError(d.fails(kAddObjectCommand, {{"parent", d.ptr(outside)}}),
                      StatusCode::FailedPrecondition,
                      "'outside'"));
  put(d.obj(a).channels, trs::kScale, double3(1, 0, 1));
  test_assert(isError(d.fails(kAddObjectCommand, {{"parent", d.ptr(a)}}),
                      StatusCode::FailedPrecondition));
  put(d.obj(a).channels, trs::kScale, double3(1, 1, 1));

  /* Make a parent and parent to it, and take both back; then discard them. */
  CommandDoc e;
  const BlockHandle child = e.object("child", 4.0);
  const BlockHandle p = e.blockOut(
      e.ok(kAddObjectCommand, {{"name", text("p")}, {"location", PropValue::ofVec3(1, 0, 0)}}),
      "object");
  e.select({child});
  e.ok(kSetParentCommand, {{"parent", e.ptr(p)}});
  e.evaluate();
  test_assert(e.isAt(child, 4) && e.obj(child).channels.get(trs::kLocation) == 3.0);
  e.undo();
  e.undo();
  e.evaluate();
  test_assert(e.isAt(child, 4) && e.obj(child).parent.cell().isNull());
  test_assert(members(e) == "child" && e.lib->get(p)->state() == BlockState::InHistory);
  must(e.lib->collect().status());
  e.redo();
  e.redo();
  e.evaluate();
  test_assert(e.isAt(child, 4) && e.lib->resolveRef(e.obj(child).parent.cell()) == p);
  e.undo();
  e.undo();
  e.history->clear();
  test_assert(!e.lib->get(p) && e.sc().cells().size() == 1);
}

} // namespace

int main()
{
  testNew();
  testLink();
  testUnlink();
  testPrune();
  testActive();
  testSelect();
  testAdd();
  return test_end();
}
