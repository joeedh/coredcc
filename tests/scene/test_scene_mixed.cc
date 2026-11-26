/* Commands on objects of which some can't be changed, aren't there, or have others below
 * them that don't follow; and cells that were written behind a scene's back. */
#include "command_doc.h"

#include "coredcc/api/ref_text.h"
#include "coredcc/scene/kept_worlds.h"
#include "coredcc/scene/pending_writes.h"

#include <cmath>

test_init;

using namespace scene_test;

namespace {

const PropValue kStep = PropValue::ofVec3(10, 0, 0);

int waiting(CommandDoc &d)
{
  return d.rt.ext<detail::PendingWrites>().count(*d.lib);
}

double xOf(const CommandDoc &d, BlockHandle h)
{
  return solve::translationOf(computeState(*d.lib, h, 0.0)->world)[0];
}

/* ---- objects that aren't there ---- */

void testDetached()
{
  CommandDoc d;
  const BlockHandle a = d.object("a", 1.0), h = d.hostObject("h", 5.0);
  const BlockId id = d.idOf(h);
  const auto hostX = [&] { return d.hostRaw(h).channels.get(0); };
  int revived = 0;
  d.lib->addReviveListener(
      [&](Library &l, BlockHandle b) { revived += &l == d.lib.get() && b == h; });
  d.select({a, h});
  d.ok(kApplyObjectDeltaCommand, {{"vector", kStep}});
  must(d.lib->onHostDestroyed(id));
  d.undo();
  test_assert(d.obj(a).channels.get(0) == 1.0 && waiting(d) == 1 && d.history->canRedo());
  must(d.lib->onHostRestored(id));
  test_assert(hostX() == 5.0 && waiting(d) == 0);

  /* The last value counts, and what isn't a change of it doesn't matter. */
  must(d.lib->onHostDestroyed(id));
  d.redo();
  d.undo();
  d.redo();
  test_assert(d.obj(a).channels.get(0) == 11.0 && waiting(d) == 1);
  int heard = 0;
  const ScopedConnection listening = d.rt.ext<ChangeBus>().addListener(
      [&](const OwnerKey *owner) { heard += owner && *owner == OwnerKey::ofBlock(id); });
  must(d.lib->onHostRestored(id));
  test_assert(hostX() == 15.0 && waiting(d) == 0 && heard == 1);
  must(d.lib->onHostDestroyed(id));
  must(d.lib->onHostRestored(id));
  test_assert(hostX() == 15.0);

  /* Deleted while it is away, and back. */
  must(d.lib->onHostDestroyed(id));
  d.undo();
  must(d.lib->markDeleted(h));
  test_assert(waiting(d) == 1 && revived == 3);
  must(d.lib->onHostRestored(id)); /* it is still deleted */
  d.evaluate();
  test_assert(waiting(d) == 1 && revived == 3);
  must(d.lib->restoreFromHistory(h));
  test_assert(waiting(d) == 0 && hostX() == 5.0 && revived == 4);
  d.redo();
  test_assert(hostX() == 15.0);

  /* It refuses. */
  must(d.lib->onHostDestroyed(id));
  d.undo();
  d.host(h).onWrite = [] { return Status::error(StatusCode::Internal, "no"); };
  must(d.lib->onHostRestored(id));
  test_assert(waiting(d) == 0 && strcmp(d.rt.takeStickyError().message(), "no") == 0);
  d.host(h).onWrite = nullptr;
  d.redo();

  /* Not now: an error is pending. Then with the next evaluation, or the next undo. */
  for (bool evaluate : {true, false}) {
    must(d.lib->onHostDestroyed(id));
    d.undo();
    d.rt.setStickyError(Status::error(StatusCode::Internal, "before"));
    must(d.lib->onHostRestored(id));
    test_assert(waiting(d) == 1 &&
                strcmp(d.rt.takeStickyError().message(), "before") == 0);
    test_assert(hostX() == 15.0);
    if (evaluate) {
      d.evaluate();
      test_assert(hostX() == 5.0 && d.isAt(h, 5.0));
    }
    d.redo();
    test_assert(waiting(d) == 0 && hostX() == 15.0);
  }
  /* While the scene is evaluated. */
  const BlockHandle asks = d.hostObject("asks", 1.0);
  must(d.lib->onHostDestroyed(id));
  d.undo();
  d.host(asks).onRead = [&](ObjectInfo &) {
    if (d.lib->get(h)->state() != BlockState::Live) {
      must(d.lib->onHostRestored(id));
    }
  };
  must(evaluateScene(*d.ctx));
  d.host(asks).onRead = nullptr;
  test_assert(waiting(d) == 1 && hostX() == 15.0 && !d.rt.hasStickyError());
  d.evaluate();
  test_assert(waiting(d) == 0 && hostX() == 5.0);
  d.redo();
  /* Gone for good. */
  d.ok(kUnlinkSceneCommand, {{"objects", PropValue()}});
  d.undo();
  must(d.lib->onHostDestroyed(id));
  d.undo();
  must(d.lib->markDeleted(h));
  must(d.sc().remove(*d.lib, id).status());
  d.history->clear();
  must(d.lib->destroyFromHistory(h));
  test_assert(!d.lib->get(h) && waiting(d) == 1);
  d.evaluate();
  test_assert(waiting(d) == 0);
}

/* A drag goes on without it, and so does cancelling. */
void testDetachedDrag()
{
  for (int outcome = 0; outcome < 4; outcome++) {
    CommandDoc d;
    const BlockHandle a = d.object("a", 1.0), h = d.hostObject("h", 5.0);
    const BlockId id = d.idOf(h);
    d.select({a, h});
    const CommandDef *def = d.rt.ext<CommandRegistry>().find(kDragObjectDeltaCommand);
    must(d.ex->beginInteractive(*d.ctx, *def, def->newInputs()));
    ApplyDelta *drag = d.ex->interactiveCommand()->as<ApplyDelta>();
    TransformDelta t;
    t.vector = double3(1, 0, 0);
    must(drag->update(*d.ctx, t));
    must(d.lib->onHostDestroyed(id));
    t.vector = double3(2, 0, 0);
    must(drag->update(*d.ctx, t));
    test_assert(d.obj(a).channels.get(0) == 3.0 && drag->skipped() == 0);
    if (outcome & 1) {
      must(d.lib->onHostRestored(id));
      test_assert(d.host(h).channels.get(0) == 7.0);
    }
    must(outcome & 2 ? d.ex->cancel(*d.ctx) : d.ex->commit(*d.ctx));
    if (!(outcome & 1)) {
      must(d.lib->onHostRestored(id));
    }
    test_assert(d.obj(a).channels.get(0) == (outcome & 2 ? 1.0 : 3.0));
    test_assert(d.host(h).channels.get(0) == (outcome & 2 ? 5.0 : 7.0) &&
                waiting(d) == 0);
    if (!(outcome & 2)) {
      d.undo();
      test_assert(d.obj(a).channels.get(0) == 1.0 && d.host(h).channels.get(0) == 5.0);
    }
  }
}

/* Parents and types wait as well, and each field by itself. The types are coredcc's own
 * here: deleted behind the history's back. */
void testDetachedFields()
{
  CommandDoc d;
  const BlockHandle p = d.object("p", 100.0), a = d.object("a", 1.0);
  d.select({a});
  d.ok(kSetObjectParentCommand, {{"parent", d.ptr(p)}});
  d.ok(kSetObjectTransformTypeCommand, {{"type", PropValue::ofString(kMatrixTransform)}});
  d.ok(kApplyObjectDeltaCommand, {{"vector", kStep}});
  test_assert(xOf(d, a) == 11.0);
  must(d.lib->markDeleted(a));
  d.undo();
  d.undo();
  d.undo();
  test_assert(waiting(d) == 1 && d.obj(a).transformTypeId() == d.matrix->id());
  must(d.lib->restoreFromHistory(a));
  test_assert(waiting(d) == 0 && d.obj(a).transformTypeId() == d.trs->id());
  test_assert(d.obj(a).parent.target().isNull() && xOf(d, a) == 1.0);
  must(d.lib->markDeleted(a));
  d.redo();
  d.redo();
  must(d.lib->restoreFromHistory(a));
  test_assert(d.obj(a).transformTypeId() == d.matrix->id() &&
              d.obj(a).parent.target() == d.idOf(p));
  test_assert(xOf(d, a) == 1.0);
  d.redo();
  test_assert(xOf(d, a) == 11.0);
  /* Two objects, and one at a time. */
  const BlockHandle b = d.object("b", 2.0);
  d.select({a, b});
  d.ok(kApplyObjectDeltaCommand, {{"vector", kStep}});
  must(d.lib->markDeleted(a));
  must(d.lib->markDeleted(b));
  d.undo();
  test_assert(waiting(d) == 2);
  must(d.lib->restoreFromHistory(b));
  test_assert(waiting(d) == 1 && xOf(d, b) == 2.0);
  must(d.lib->restoreFromHistory(a));
  test_assert(waiting(d) == 0 && xOf(d, a) == 11.0);
  /* A Library that goes takes what waits with it. */
  {
    Library other(&d.rt);
    detail::PendingWrites &w = d.rt.ext<detail::PendingWrites>();
    StoredFields f;
    f.channels = true;
    w.add(other, d.idOf(a), f, StoredObject());
    test_assert(w.count(other) == 1 && waiting(d) == 0);
  }
  must(d.lib->markDeleted(a));
  d.redo();
  test_assert(waiting(d) == 1);
  must(d.lib->restoreFromHistory(a));
  test_assert(xOf(d, a) == 21.0 && xOf(d, b) == 12.0);
}

/* ---- objects that can't be changed ---- */

void testSkips()
{
  CommandDoc d;
  const BlockHandle a = d.object("a", 1.0), flat = d.object("flat"),
                    under = d.object("under", 1.0), odd = d.object("odd", 2.0),
                    below = d.object("below", 3.0), shape = d.shapeBlock("shape", 1.0);
  must(d.sc().add(*d.lib, shape));
  d.setParent(under, flat);
  d.setParent(below, odd);
  d.obj(flat).channels.set(trs::kScale, 0.0);
  d.obj(odd).setTransformType("gone.type");
  d.select({a, under, odd, below, shape});
  PropertyBag out = d.cycle(kApplyObjectDeltaCommand, {{"vector", kStep}});
  /* `below` moves by itself: the one above it doesn't. */
  test_assert(out.get("skipped")->asInt() == 3 && xOf(d, a) == 11.0 &&
              xOf(d, below) == 13.0);
  for (BlockHandle h : {under, odd, shape}) {
    d.select({h});
    test_assert(d.fails(kApplyObjectDeltaCommand, {{"vector", kStep}}).code() ==
                StatusCode::FailedPrecondition);
  }
  d.select({shape});
  for (const char *command : {kSetObjectTransformTypeCommand,
                              kSetObjectRotationModeCommand,
                              kSetObjectParentCommand})
  {
    test_assert(d.fails(command).code() == StatusCode::FailedPrecondition);
  }
  d.select({shape, under, a});
  out = d.cycle(kSetObjectParentCommand);
  test_assert(out.get("skipped")->asInt() == 1 && d.obj(under).parent.target().isNull());
  d.undo();
  out = d.cycle(kSetObjectRotationModeCommand, {{"mode", PropValue::ofEnum(2)}});
  test_assert(out.get("skipped")->asInt() == 1 &&
              d.obj(a).channels.get(trs::kRotationMode) == 2.0);
  /* In a drag: what can't follow stays at its last place. */
  const BlockHandle far = d.object("far", 1.0);
  d.setParent(far, flat);
  for (int i = 0; i < 3; i++) {
    d.obj(flat).channels.set(trs::kScale + i, 1e-200);
  }
  d.select({a, far, odd});
  const double a0 = xOf(d, a);
  const CommandDef *def = d.rt.ext<CommandRegistry>().find(kDragObjectDeltaCommand);
  must(d.ex->beginInteractive(*d.ctx, *def, def->newInputs()));
  ApplyDelta *drag = d.ex->interactiveCommand()->as<ApplyDelta>();
  test_assert(drag->skipped() == 1);
  TransformDelta t;
  /* A bad delta is an error, not an object's fault. */
  t.vector = double3(NAN, 0, 0);
  test_assert(drag->update(*d.ctx, t).code() == StatusCode::InvalidArgument);
  t.vector = double3(1, 0, 0);
  must(drag->update(*d.ctx, t));
  const double place = d.obj(far).channels.get(0);
  test_assert(drag->skipped() == 1 && std::fabs(place - 1e200) < 1e190);
  t.vector = double3(1e150, 0, 0);
  must(drag->update(*d.ctx, t));
  test_assert(drag->skipped() == 2 && d.obj(far).channels.get(0) == place);
  test_assert(xOf(d, a) == a0 + 1e150);
  t.vector = double3(2, 0, 0);
  must(drag->update(*d.ctx, t));
  test_assert(drag->skipped() == 1 && xOf(d, a) == a0 + 2.0);
  test_assert(std::fabs(d.obj(far).channels.get(0) - 2e200) < 1e190);
  must(d.ex->cancel(*d.ctx));
  test_assert(xOf(d, a) == a0 && d.obj(far).channels.get(0) == 1.0);
  /* None of them. */
  d.select({far});
  must(d.ex->beginInteractive(*d.ctx, *def, def->newInputs()));
  drag = d.ex->interactiveCommand()->as<ApplyDelta>();
  t.vector = double3(1e150, 0, 0);
  test_assert(drag->update(*d.ctx, t).code() == StatusCode::FailedPrecondition);
  test_assert(d.obj(far).channels.get(0) == 1.0);
  must(d.ex->cancel(*d.ctx));

  /* A read that throws is an error. */
  const BlockHandle h = d.hostObject("h", 1.0);
  d.select({a, h});
  d.host(h).onRead = [&](ObjectInfo &) {
    d.rt.setStickyError(Status::error(StatusCode::Internal, "threw"));
  };
  test_assert(
      isError(d.run(kApplyObjectDeltaCommand).status(), StatusCode::Internal, "threw"));
  test_assert(
      isError(d.run(kSetObjectParentCommand).status(), StatusCode::Internal, "threw"));
  test_assert(isError(d.run(kSetObjectTransformTypeCommand,
                            {{"type", PropValue::ofString(kFbxTransform)}})
                          .status(),
                      StatusCode::Internal,
                      "threw"));
  test_assert(isError(
      d.run(kSetObjectRotationModeCommand).status(), StatusCode::Internal, "threw"));
  d.host(h).onRead = nullptr;
  test_assert(xOf(d, a) == a0);
}

/* What is hidden or unselectable by now stays selected, but isn't worked on. */
void testOutOfReach()
{
  CommandDoc d;
  const BlockHandle a = d.object("a", 1.0), b = d.object("b", 2.0),
                    c = d.object("c", 3.0), h = d.hostObject("h", 4.0);
  d.select({a, b, c, h});
  d.obj(a).flags = ObjectFlags::Hidden;
  d.obj(b).flags = ObjectFlags::Unselectable;
  d.host(h).info.flags = uint32_t(ObjectFlags::Hidden);
  d.ok(kApplyObjectDeltaCommand, {{"vector", kStep}});
  test_assert(xOf(d, a) == 1.0 && xOf(d, b) == 2.0 && xOf(d, c) == 13.0 &&
              xOf(d, h) == 4.0);
  test_assert(d.sc().selected(*d.lib).size() == 4);
  /* By name they are. */
  Vector<BlockId> ids;
  for (BlockHandle x : {a, b, h}) {
    ids.append(d.idOf(x));
  }
  const PropValue named =
      PropValue::ofString(formatBlockIds(d.rt, {ids.data(), size_t(3)}).c_str());
  d.ok(kApplyObjectDeltaCommand, {{"vector", kStep}, {"objects", named}});
  test_assert(xOf(d, a) == 11.0 && xOf(d, b) == 12.0 && xOf(d, c) == 13.0 &&
              xOf(d, h) == 14.0);
  d.ok(kDeleteObjectCommand);
  test_assert(d.sc().objects(*d.lib).size() == 3 && d.sc().selected(*d.lib).size() == 3);
  test_assert(
      isError(d.fails(kDeleteObjectCommand), StatusCode::FailedPrecondition, "hidden"));
  d.obj(a).flags = ObjectFlags::None;
  d.ok(kDeleteObjectCommand);
  test_assert(d.sc().objects(*d.lib).size() == 2);
}

/* ---- what is below ---- */

void testBelow()
{
  CommandDoc d;
  const auto fbxChild = [&](const char *name, BlockHandle parent, int inherit) {
    const BlockHandle h = d.object(name, 1.0);
    d.obj(h).setTransformType(kFbxTransform);
    d.obj(h).channels = d.fbx->defaults();
    d.obj(h).channels.set(0, 2.0);
    d.obj(h).channels.set(fbx::kInheritType, inherit);
    d.setParent(h, parent);
    return h;
  };
  const BlockHandle p = d.object("p"), g = d.object("g"),
                    rrs = fbxChild("rrs", p, fbx::kRrs), under = d.object("under", 1.0),
                    plain = fbxChild("plain", p, fbx::kRSrs);
  d.setParent(under, rrs);
  put(d.obj(p).channels, trs::kScale, double3(2, 2, 2));
  put(d.obj(g).channels, trs::kScale, double3(3, 3, 3));
  const mat4d wp = d.stateOf(p).world, wr = d.stateOf(rrs).world,
              wu = d.stateOf(under).world, wl = d.stateOf(plain).world;
  d.select({p});
  PropertyBag out = d.cycle(kSetObjectParentCommand, {{"parent", d.ptr(g)}});
  test_assert(difference(d.stateOf(p).world, wp) < 1e-12);
  test_assert(difference(d.stateOf(plain).world, wl) < 1e-12);
  test_assert(difference(d.stateOf(rrs).world, wr) > 0.1);
  test_assert(difference(d.stateOf(under).world, wu) > 0.1);
  test_assert(out.get("approximate")->asInt() == 2); /* rrs and under */
  d.undo();
  /* Without keepWorld they are meant to move. */
  out = d.ok(kSetObjectParentCommand,
             {{"parent", d.ptr(g)}, {"keepWorld", PropValue::ofBool(false)}});
  test_assert(out.get("approximate")->asInt() == 0);
  d.undo();
  /* A parent that isn't an object is the origin. */
  d.setParent(p, d.shapeBlock("shape", 1.0));
  out = d.ok(kSetObjectParentCommand, {{"parent", d.ptr(g)}});
  test_assert(out.get("approximate")->asInt() == 2);
  d.undo();
  d.setParent(p, BlockHandle());
  /* A place that only moves. */
  d.addFnModel("test.slide", [](const EvalContext &, const TransformState &above) {
    TransformState s;
    s.world[3][0] = above.localScale[0];
    return s;
  });
  const BlockHandle slide = d.object("slide");
  d.obj(slide).setTransformType("test.slide");
  d.obj(slide).channels = ChannelBlock(1);
  d.setParent(slide, p);
  out = d.ok(kSetObjectParentCommand, {{"parent", d.ptr(g)}});
  test_assert(out.get("approximate")->asInt() == 3);
  d.undo();
  d.setParent(slide, BlockHandle());
  /* One that is named, and only near its place, counts once. */
  const BlockHandle turned = d.object("turned", 1.0);
  put(d.obj(turned).channels, trs::kRotationEuler, double3(0.3, 0.4, 0.5));
  d.setParent(turned, p);
  put(d.obj(g).channels, trs::kScale, double3(1, 5, 1));
  d.setParent(rrs, BlockHandle());
  d.select({p, turned});
  out = d.ok(kSetObjectParentCommand, {{"parent", d.ptr(g)}});
  test_assert(out.get("approximate")->asInt() == 1);
  d.undo();
  d.setParent(rrs, p);
  d.setParent(turned, BlockHandle());
  put(d.obj(g).channels, trs::kScale, double3(3, 3, 3));
  /* Those that are named with it aren't below it. */
  d.select({p, rrs});
  out = d.ok(kSetObjectParentCommand, {{"parent", d.ptr(g)}});
  test_assert(out.get("approximate")->asInt() == 0 &&
              difference(d.stateOf(rrs).world, wr) < 1e-12);
  d.undo();

  /* Another type with the same matrix, but not the same rotation: a mirror is kept in the
   * scale or not. And one that keeps both. */
  d.select({p});
  out = d.cycle(kSetObjectTransformTypeCommand,
                {{"type", PropValue::ofString(kMatrixTransform)}});
  test_assert(out.get("approximate")->asInt() == 0);
  d.undo();
  put(d.obj(p).channels, trs::kScale, double3(2, -2, -2));
  out = d.cycle(kSetObjectTransformTypeCommand,
                {{"type", PropValue::ofString(kMatrixTransform)}});
  test_assert(out.get("approximate")->asInt() == 2);
  d.undo();

  /* A deleted parent's children. */
  d.setParent(p, g);
  d.select({g});
  out = d.cycle(kDeleteObjectCommand);
  test_assert(out.get("approximate")->asInt() == 2);
  d.undo();
  /* Those that go with it don't count. */
  d.select({g, rrs});
  out = d.cycle(kDeleteObjectCommand);
  test_assert(out.get("approximate")->asInt() ==
              0); /* `under` is kept: it is a child of `rrs` */
}

/* A type that holds only a place: its answer is near, with the rotation and scale that
 * were there. */
struct PlaceOnly {
  TransformState evaluate(const EvalContext &c, const TransformState &) const
  {
    TransformState s;
    s.world[3][0] = c.channel(0);
    return s;
  }
  Status setWorld(const EvalContext &,
                  const mat4d &target,
                  const TransformState &,
                  SetWorldMode,
                  ChannelBlock &out) const
  {
    out.set(0, solve::translationOf(target)[0]);
    return Status();
  }
  static lbt::Struct<PlaceOnly> *defineBindings()
  {
    auto *st = new lbt::Struct<PlaceOnly>("test::PlaceOnly", sizeof(PlaceOnly));
    BIND_STRUCT_METHOD(st, evaluate, MARGS("ctx", "parent"));
    BIND_STRUCT_METHOD(st, setWorld, MARGS("current", "target", "parent", "mode", "out"));
    return st;
  }
};

void testBelowNear()
{
  CommandDoc d;
  must(registerImpl<TransformModel, PlaceOnly>(d.rt, "test.placeOnly"));
  Result<Owned<TransformModel>> owned = own<TransformModel>(
      d.rt, "test.placeOnly", litestl::alloc::New<PlaceOnly>("PlaceOnly"));
  must(owned.status());
  TransformTypeInfo info;
  info.name = "test.placeOnly";
  info.channels.append(PropertyDef::floatProp("x"));
  must(d.rt.ext<TransformRegistry>()
           .add(std::move(info), std::move(owned).value())
           .status());
  d.addFnModel("test.sheared", [](const EvalContext &, const TransformState &p) {
    TransformState s = p;
    s.world[1][0] = 0.5;
    return s;
  });
  const BlockHandle p = d.object("p"), below = d.object("below", 1.0);
  d.obj(p).setTransformType("test.sheared");
  d.obj(p).channels = ChannelBlock(1);
  d.obj(below).channels.set(trs::kLocation + 1, 2.0);
  d.setParent(below, p);
  d.select({p});
  PropertyBag out = d.cycle(kSetObjectTransformTypeCommand,
                            {{"type", PropValue::ofString("test.placeOnly")}});
  test_assert(out.get("approximate")->asInt() == 2); /* `below` moved by 1 in x */
  test_assert(d.stateOf(below).world[3][0] == 1.0);
}

void (*g_onFree)() = nullptr;
CommandDoc *g_doc = nullptr;

/* A Library that destroys blocks isn't written to. */
void testPendingBusy()
{
  CommandDoc d;
  const BlockHandle a = d.object("a", 1.0), gone = d.hostObject("gone", 2.0);
  StoredFields fields;
  fields.channels = true;
  StoredObject v;
  v.channels = d.trs->defaults();
  v.channels.set(0, 9.0);
  detail::PendingWrites &pending = d.rt.ext<detail::PendingWrites>();
  pending.add(*d.lib, d.idOf(a), fields, v);
  must(d.sc().remove(*d.lib, d.idOf(gone)).status());
  g_doc = &d;
  g_onFree = [] {
    test_assert(g_doc->lib->busy());
    g_doc->rt.ext<detail::PendingWrites>().flush(*g_doc->lib);
    test_assert(waiting(*g_doc) == 1);
  };
  d.slot(gone).free = [](void *p) {
    g_onFree();
    delete static_cast<Host *>(p);
  };
  must(d.lib->markDeleted(gone));
  must(d.lib->destroyFromHistory(gone));
  test_assert(!d.lib->get(gone) && d.obj(a).channels.get(0) == 1.0 && waiting(d) == 1);
  d.evaluate();
  test_assert(d.obj(a).channels.get(0) == 9.0 && waiting(d) == 0);
}

void testInherits()
{
  Rand r(43);
  TransformState a;
  a.rotation = r.rotation();
  a.localScale = double3(1, -2, 0);
  TransformState b = a;
  test_assert(!detail::inheritsOtherwise(a, b));
  for (int i = 0; i < 4; i++) {
    b.rotation[i] = -a.rotation[i]; /* the same rotation */
  }
  test_assert(!detail::inheritsOtherwise(a, b));
  b.localScale[0] = NAN;
  test_assert(detail::inheritsOtherwise(a, b));
  b.localScale[0] = 1 + 1e-12;
  test_assert(!detail::inheritsOtherwise(a, b));
  b.localScale[2] = 1e-3;
  test_assert(detail::inheritsOtherwise(a, b));
  b = a;
  b.rotation = r.rotation();
  test_assert(detail::inheritsOtherwise(a, b));
}

/* The counts, against states before and after. */
void testBelowRandom()
{
  Rand r(41);
  int counted = 0;
  for (int k = 0; k < 200 * coredcc_test::testScale(); k++) {
    CommandDoc d;
    std::vector<BlockHandle> all;
    for (int i = 0; i < 14; i++) {
      const BlockHandle h = d.object(("o" + std::to_string(i)).c_str());
      Object &o = d.obj(h);
      const double s = std::exp(r.range(-1, 1));
      const double3 scale = r.coin() ? double3(s, s, s) : double3(s, -s, s);
      if (r.below(3) == 0) {
        o.channels = trsChannels(r, *d.trs, scale, 3.0);
      } else {
        o.setTransformType(kFbxTransform);
        o.channels = fbxChannels(r, *d.fbx, int(r.below(3)), scale, 1.0);
      }
      if (i > 0 && r.below(4)) {
        d.setParent(h, all[r.below(uint32_t(i))]);
      }
      all.push_back(h);
    }
    if (k % 10 == 0) {
      d.setParent(all[0], all[13]); /* perhaps a cycle, which the command doesn't break */
    }
    const auto worlds = [&] {
      std::vector<mat4d> w;
      for (BlockHandle h : all) {
        w.push_back(d.stateOf(h).world);
      }
      return w;
    };
    const std::vector<mat4d> before = worlds();
    const BlockHandle x = all[r.below(14)], y = all[r.below(14)];
    d.select({x});
    Result<PropertyBag> out =
        k % 2 ? d.run(kSetObjectParentCommand, {{"parent", d.ptr(y)}})
              : d.run(kSetObjectTransformTypeCommand,
                      {{"type", PropValue::ofString(kMatrixTransform)}});
    if (!out.ok()) {
      continue;
    }
    const std::vector<mat4d> after = worlds();
    int moved = 0;
    for (int i = 0; i < 14; i++) {
      moved += difference(before[size_t(i)], after[size_t(i)]) > 1e-7;
    }
    counted += moved;
    test_assert(out->get("approximate")->asInt() == moved);
  }
  printf("%d objects moved below others\n", counted);
  test_assert(counted > 20);
}

/* ---- cells ---- */

void testCells()
{
  CommandDoc d;
  const BlockHandle a = d.object("a"), b = d.object("b"), c = d.loose("c"),
                    e = d.loose("e");
  Scene &s = d.sc();
  const auto write = [&](int i, BlockHandle h) {
    must(d.lib->writeRef(const_cast<RefCell &>(s.cells()[i]), d.idOf(h)));
  };
  test_assert(s.contains(*d.lib, a) && !s.contains(*d.lib, c));
  write(0, c);
  test_assert(s.contains(*d.lib, c) && !s.contains(*d.lib, a) &&
              s.indexOf(*d.lib, c) == 0);
  test_assert(s.add(*d.lib, c).code() == StatusCode::AlreadyExists);
  /* A host's undo, that tells the Library. */
  const_cast<RefCell &>(s.cells()[0]).target = d.idOf(e);
  d.lib->noteRefsChanged();
  test_assert(s.contains(*d.lib, e) && !s.contains(*d.lib, c));
  /* Twice. */
  write(0, b);
  test_assert(s.objects(*d.lib).size() == 1 && s.objects(*d.lib)[0] == b);
  test_assert(s.indexOf(*d.lib, b) >= 0 && s.cells().size() == 2);
  d.evaluate();
  const int rebuilds = s.counters().rebuilds;
  d.loose("other"); /* moves the epoch */
  d.evaluate();
  test_assert(s.counters().rebuilds == rebuilds && s.evaluatedObjects().size() == 1);
  test_assert(s.prune(*d.lib) == 1 && s.cells().size() == 1 && s.contains(*d.lib, b));
  must(s.add(*d.lib, a));
  must(s.add(*d.lib, e));
  write(0, e);
  test_assert(*s.remove(*d.lib, d.idOf(e)) == 0 && s.cells().size() == 1 &&
              !s.contains(*d.lib, e));
  test_assert(s.indexOf(*d.lib, a) == 0);
  /* One that isn't known yet gets in twice, and counts once. */
  write(0, c);
  must(s.add(*d.lib, c));
  test_assert(s.objects(*d.lib).size() == 1 && s.contains(*d.lib, c));
  d.select({c});
  test_assert(s.selected(*d.lib).size() == 1);
  must(s.remove(*d.lib, d.idOf(c)).status());
  test_assert(s.cells().size() == 0 && s.objects(*d.lib).size() == 0);
  test_assert(s.remove(*d.lib, d.idOf(c)).status().code() == StatusCode::NotFound);
  /* Adding many doesn't look at all of them each time: the epoch moves with each one. */
  for (int i = 0; i < 20000; i++) {
    must(s.add(*d.lib, d.loose("n")));
  }
  test_assert(s.objects(*d.lib).size() == 20000);
}

/* ---- registering ---- */

void testRegister()
{
  for (const char *taken :
       {"coredcc.EvaluatedScene", kSceneType, kObjectDataStruct, kObjectType})
  {
    Runtime rt;
    ApiStruct st(taken);
    must(rt.ext<ApiRegistry>().add(std::move(st)).status());
    for (int i = 0; i < 3; i++) {
      test_assert(registerSceneTypes(rt).code() == StatusCode::AlreadyExists);
    }
  }
  Runtime rt;
  for (int i = 0; i < 3; i++) {
    must(registerSceneTypes(rt));
  }
  /* A type that comes changes what its objects answer. */
  CommandDoc d;
  int all = 0;
  const ScopedConnection listening = d.rt.ext<ChangeBus>().addListener(
      [&](const OwnerKey *owner) { all += owner == nullptr; });
  d.addFnModel("test.late",
               [](const EvalContext &, const TransformState &p) { return p; });
  test_assert(all == 1);
}

} // namespace

int main()
{
  testDetached();
  testDetachedDrag();
  testDetachedFields();
  testSkips();
  testOutOfReach();
  testBelow();
  testBelowNear();
  testPendingBusy();
  testInherits();
  testBelowRandom();
  testCells();
  testRegister();
  return test_end();
}
