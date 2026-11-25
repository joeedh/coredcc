/* The commands that change objects' transforms and parents, and delete objects. */
#include "command_doc.h"

test_init;

using namespace scene_test;

namespace {

bool closeTo(const mat4d &a, const mat4d &b, double tolerance = 1e-9)
{
  const double e = difference(a, b);
  if (e > tolerance) {
    fprintf(stderr, "differs by %g\n", e);
  }
  return e <= tolerance;
}

void testSetParent()
{
  CommandDoc d;
  Rand r(3);
  const BlockHandle a = d.object("a"), b = d.object("b"), c = d.object("c"), p = d.object("p"),
                    q = d.object("q"), h = d.hostObject("h"), out = d.loose("out");
  for (BlockHandle x : {a, b, c, p, q}) {
    const double s = r.range(0.5, 2.0);
    d.obj(x).channels = trsChannels(r, *d.trs, double3(s, s, s), 5.0);
  }
  d.setParent(b, a);
  d.setParent(p, q);
  const mat4d wa = d.stateOf(a).world, wb = d.stateOf(b).world, wc = d.stateOf(c).world;

  /* [A, B] with A above B, and a third. */
  d.select({a, b, c});
  PropertyBag res = d.cycle(kSetParentCommand, {{"parent", d.ptr(p)}});
  test_assert(res.get("approximate")->asInt() == 0);
  for (BlockHandle x : {a, b, c}) {
    test_assert(d.lib->resolveRef(d.obj(x).parent.cell()) == p);
  }
  test_assert(closeTo(d.stateOf(a).world, wa) && closeTo(d.stateOf(b).world, wb) &&
              closeTo(d.stateOf(c).world, wc));
  /* Cleared, with the place kept, and without. */
  d.select({a});
  d.cycle(kSetParentCommand);
  test_assert(d.obj(a).parent.cell().isNull() && closeTo(d.stateOf(a).world, wa));
  d.select({b});
  const ChannelBlock channels = d.obj(b).channels;
  d.cycle(kSetParentCommand, {{"keepWorld", PropValue::ofBool(false)}});
  test_assert(d.obj(b).channels == channels && !closeTo(d.stateOf(b).world, wb));

  /* Refusals. */
  d.select({q});
  test_assert(isError(d.fails(kSetParentCommand, {{"parent", d.ptr(c)}}),
                      StatusCode::FailedPrecondition,
                      "'c': is below one of the objects"));
  test_assert(isError(d.fails(kSetParentCommand, {{"parent", d.ptr(q)}}),
                      StatusCode::FailedPrecondition,
                      "its own parent"));
  test_assert(isError(d.fails(kSetParentCommand, {{"parent", d.ptr(out)}}),
                      StatusCode::FailedPrecondition,
                      "'out': isn't in the scene"));
  d.select({});
  test_assert(isError(d.fails(kSetParentCommand, {{"parent", d.ptr(c)}}),
                      StatusCode::FailedPrecondition,
                      "no object is selected"));
  /* The last of three can't be: the first two are taken back. */
  d.select({a, b, h});
  test_assert(isError(
      d.fails(kSetParentCommand, {{"parent", d.ptr(p)}}), StatusCode::Unsupported, "'h'"));
  d.obj(b).setTransformType("test.missing");
  d.select({a, b});
  test_assert(isError(d.fails(kSetParentCommand, {{"parent", d.ptr(p)}}),
                      StatusCode::FailedPrecondition,
                      "'b': its transform type"));
  d.cycle(kSetParentCommand,
          {{"parent", d.ptr(p)}, {"keepWorld", PropValue::ofBool(false)}});
  d.obj(b).setTransformType(kTrsTransform);

  /* Shear can't be kept: as well as it goes, and counted. */
  put(d.obj(p).channels, trs::kScale, double3(1, 3, 1));
  const double3 at = solve::translationOf(d.stateOf(c).world);
  d.select({c});
  d.ok(kSetParentCommand);
  res = d.cycle(kSetParentCommand, {{"parent", d.ptr(p)}});
  test_assert(res.get("approximate")->asInt() == 1);
  const double3 now = solve::translationOf(d.stateOf(c).world);
  test_assert(lengthOf(now - at) < 1e-9);
  /* A singular parent can't. */
  put(d.obj(p).channels, trs::kScale, double3(1, 0, 1));
  d.select({q, a});
  d.setParent(q, BlockHandle());
  d.setParent(a, BlockHandle());
  d.setParent(p, BlockHandle());
  test_assert(isError(d.fails(kSetParentCommand, {{"parent", d.ptr(p)}}),
                      StatusCode::FailedPrecondition,
                      "'q'"));
}

/* A host that fails while undo and redo write. */
void testFailingWrites()
{
  CommandDoc d;
  const BlockHandle a = d.object("a", 1.0), h = d.hostObject("h", 2.0), b = d.object("b", 3.0);
  const PropValue move = PropValue::ofVec3(10, 0, 0);
  d.select({a, h, b});
  d.ok(kApplyDeltaCommand, {{"vector", move}});
  d.evaluate();
  test_assert(d.isAt(a, 11) && d.isAt(h, 12) && d.isAt(b, 13));
  d.host(h).onWrite = [] { return boom("no"); };
  test_assert(isError(d.ex->undo(*d.ctx), StatusCode::Internal, "no"));
  d.evaluate();
  test_assert(d.isAt(a, 1) && d.isAt(h, 12) && d.isAt(b, 3)); /* the rest is undone */
  test_assert(!d.ex->history()->canRedo());

  d.host(h).onWrite = nullptr;
  d.ok(kApplyDeltaCommand, {{"vector", move}});
  d.undo();
  const std::string before = d.describe();
  d.host(h).onWrite = [] { return boom("no"); };
  test_assert(isError(d.ex->redo(*d.ctx), StatusCode::Internal, "no"));
  test_assert(d.describe() == before);
  test_assert(isError(d.fails(kApplyDeltaCommand, {{"vector", move}}), StatusCode::Internal, "no"));
  d.host(h).onWrite = [&] {
    d.rt.setStickyError(boom("threw"));
    return Status();
  };
  test_assert(
      isError(d.fails(kApplyDeltaCommand, {{"vector", move}}), StatusCode::Internal, "threw"));
  d.host(h).onWrite = nullptr;

  /* One that its host took away in between, and that is written again when it is back. */
  d.ok(kApplyDeltaCommand, {{"vector", move}});
  must(d.lib->onHostDestroyed(d.idOf(b)));
  test_assert(!d.ex->undo(*d.ctx).ok());
  d.evaluate();
  test_assert(d.isAt(a, 1));
}

void testTransformType()
{
  CommandDoc d;
  Rand r(9);
  const BlockHandle p = d.object("p"), a = d.object("a"), b = d.object("b");
  d.setParent(a, p);
  for (BlockHandle x : {p, a, b}) {
    const double s = r.range(0.5, 2.0);
    d.obj(x).channels = trsChannels(r, *d.trs, double3(s, s, s), 5.0);
  }
  const mat4d wa = d.stateOf(a).world, wb = d.stateOf(b).world;
  d.select({a, b});
  for (const char *type : {kFbxTransform, kMatrixTransform, kTrsTransform}) {
    PropertyBag out = d.cycle(kSetTransformTypeCommand, {{"type", PropValue::ofString(type)}});
    test_assert(out.get("approximate")->asInt() == 0);
    test_assert(d.obj(a).transformTypeId() == TypeId::of(type));
    test_assert(closeTo(d.stateOf(a).world, wa) && closeTo(d.stateOf(b).world, wb));
  }
  d.cycle(kSetTransformTypeCommand,
          {{"type", PropValue::ofString(kFbxTransform)}, {"keepWorld", PropValue::ofBool(false)}});
  test_assert(d.obj(a).channels == d.fbx->defaults());
  test_assert(closeTo(d.stateOf(b).world, mat4d().identity()));
  d.undo();

  /* A shear that only the matrix type has. */
  put(d.obj(p).channels, trs::kScale, double3(1, 4, 1));
  d.obj(p).channels.set(trs::kRotationMode, 0);
  put(d.obj(p).channels, trs::kRotationEuler, double3(0, 0, 0));
  const mat4d sheared = d.stateOf(a).world;
  d.select({a});
  d.ok(kSetParentCommand, {{"keepWorld", PropValue::ofBool(true)}});
  d.undo();
  PropertyBag out =
      d.cycle(kSetTransformTypeCommand, {{"type", PropValue::ofString(kMatrixTransform)}});
  test_assert(out.get("approximate")->asInt() == 0 && closeTo(d.stateOf(a).world, sheared));
  d.ok(kSetParentCommand);
  test_assert(closeTo(d.stateOf(a).world, sheared));
  out = d.cycle(kSetTransformTypeCommand);
  test_assert(out.get("approximate")->asInt() == 1);

  test_assert(isError(d.fails(kSetTransformTypeCommand, {{"type", PropValue::ofString("x.y")}}),
                      StatusCode::NotFound,
                      "'x.y'"));
  /* From a type that isn't registered, and back. */
  d.obj(a).setTransformType("test.missing");
  d.cycle(kSetTransformTypeCommand);
  d.undo();
  test_assert(!strcmp(d.obj(a).transformType().c_str(), "test.missing"));
  d.obj(a).setTransformType(kTrsTransform);
  d.obj(a).channels = d.trs->defaults();
  /* One object that can't: the others keep theirs. */
  const BlockHandle h = d.hostObject("h");
  d.select({a, h});
  test_assert(isError(d.fails(kSetTransformTypeCommand), StatusCode::Unsupported, "'h'"));
}

void testRotationMode()
{
  CommandDoc d;
  Rand r(17);
  const BlockHandle a = d.object("a"), b = d.object("b");
  d.select({a, b});
  for (int i = 0; i < 100; i++) {
    for (BlockHandle x : {a, b}) {
      d.obj(x).channels = trsChannels(r, *d.trs, r.scale(2, true), 5.0);
    }
    if (i % 5 == 0) {
      put(d.obj(a).channels, trs::kScale, double3(0, 1, 0)); /* the rotation stays here too */
    }
    const ChannelBlock before = d.obj(a).channels;
    const TransformState sa = d.stateOf(a), sb = d.stateOf(b);
    const int mode = int(r.below(7));
    const PropValue m = PropValue::ofEnum(mode);
    d.cycle(kSetRotationModeCommand, {{"mode", m}});
    test_assert(d.obj(a).channels.get(trs::kRotationMode) == mode);
    test_assert(closeTo(d.stateOf(a).world, sa.world) && closeTo(d.stateOf(b).world, sb.world));
    const TransformState after = d.stateOf(a);
    double dot = 0.0;
    for (int k = 0; k < 4; k++) {
      dot += after.rotation[k] * sa.rotation[k];
    }
    test_assert(std::fabs(std::fabs(dot) - 1.0) < 1e-12);
    const double3 l0 = get(before, trs::kLocation), l1 = get(d.obj(a).channels, trs::kLocation);
    test_assert(l0[0] == l1[0] && l0[1] == l1[1] && l0[2] == l1[2]);
  }
  d.obj(b).setTransformType(kFbxTransform);
  d.obj(b).channels = d.fbx->defaults();
  test_assert(
      isError(d.fails(kSetRotationModeCommand), StatusCode::FailedPrecondition, "'b': has no"));
  d.obj(b).setTransformType("test.missing"); /* with as many channels */
  d.obj(b).channels = d.trs->defaults();
  test_assert(
      isError(d.fails(kSetRotationModeCommand), StatusCode::FailedPrecondition, "'b': has no"));
  d.obj(b).setTransformType(kTrsTransform);
  d.obj(b).channels = ChannelBlock(2);
  test_assert(
      isError(d.fails(kSetRotationModeCommand), StatusCode::FailedPrecondition, "'b': has no"));
}

void testApplyDelta()
{
  CommandDoc d;
  const BlockHandle top = d.object("top", 1.0), mid = d.object("mid", 10.0),
                    low = d.object("low", 100.0), other = d.object("other", 1000.0);
  d.setParent(mid, top);
  d.setParent(low, mid);
  /* Below a selected one through one that isn't. */
  d.select({low, top, other});
  d.cycle(kApplyDeltaCommand, {{"vector", PropValue::ofVec3(0, 5, 0)}});
  d.evaluate();
  test_assert(d.isAt(top, 1, 5) && d.isAt(mid, 11, 5) && d.isAt(low, 111, 5) &&
              d.isAt(other, 1000, 5));
  test_assert(d.obj(low).channels.get(trs::kLocation + 1) == 0.0);
  d.select({low});
  d.ok(kApplyDeltaCommand, {{"vector", PropValue::ofVec3(0, 1, 0)}});
  d.evaluate();
  test_assert(d.isAt(low, 111, 6));
  d.undo();
  d.undo();

  /* Around a pivot, in a basis: against the delta's matrix. */
  Rand r(23);
  for (int i = 0; i < 100; i++) {
    TransformDelta t;
    t.kind = DeltaKind(i % 3);
    t.basis = r.rotation();
    t.pivot = r.vec(5.0);
    const double factor = r.range(0.5, 2.0);
    t.vector = t.kind == DeltaKind::Scale ? double3(factor, factor, factor) : r.vec(3.0);
    t.axis = r.vec(2.0);
    t.angle = r.range(-7.0, 7.0);
    const mat4d before = d.stateOf(top).world;
    d.select({top});
    d.cycle(kApplyDeltaCommand,
            {{"kind", PropValue::ofEnum(i % 3)},
             {"basis", PropValue::ofQuat(t.basis[0], t.basis[1], t.basis[2], t.basis[3])},
             {"pivot", PropValue::ofVec3(t.pivot[0], t.pivot[1], t.pivot[2])},
             {"vector", PropValue::ofVec3(t.vector[0], t.vector[1], t.vector[2])},
             {"axis", PropValue::ofVec3(t.axis[0], t.axis[1], t.axis[2])},
             {"angle", PropValue::ofFloat(t.angle)}});
    const mat4d m = t.matrix();
    const double *x = m, *y = before;
    mat4d want;
    double *w = want;
    for (int c = 0; c < 4; c++) {
      for (int row = 0; row < 4; row++) {
        w[c * 4 + row] = 0.0;
        for (int k = 0; k < 4; k++) {
          w[c * 4 + row] += x[k * 4 + row] * y[c * 4 + k];
        }
      }
    }
    test_assert(closeTo(d.stateOf(top).world, want));
    d.undo();
  }

  /* Objects on a cycle have no parent: each of them moves. */
  d.setParent(top, low);
  d.select({top, mid, low});
  d.cycle(kApplyDeltaCommand, {{"vector", PropValue::ofVec3(0, 0, 2)}});
  d.evaluate();
  test_assert(d.isAt(top, 1, 0, 2) && d.isAt(mid, 10, 0, 2) && d.isAt(low, 100, 0, 2));
  d.undo();
  d.setParent(top, BlockHandle());

  d.obj(other).setTransformType("test.missing");
  d.select({top, other});
  test_assert(isError(d.fails(kApplyDeltaCommand), StatusCode::FailedPrecondition, "'other'"));
}

void testDrag()
{
  CommandDoc d;
  const BlockHandle a = d.object("a", 1.0), h = d.hostObject("h", 5.0), b = d.object("b", 2.0);
  d.setParent(b, a);
  const CommandDef *def = d.rt.ext<CommandRegistry>().find(kDragDeltaCommand);
  test_assert(!d.run(kDragDeltaCommand).ok()); /* interactive only */
  test_assert(d.ex->beginInteractive(*d.ctx, *def, def->newInputs()).code() ==
              StatusCode::FailedPrecondition);
  d.select({a, b, h});
  const std::string before = d.describe();
  must(d.ex->beginInteractive(*d.ctx, *def, def->newInputs()));
  ApplyDelta *drag = d.ex->interactiveCommand()->as<ApplyDelta>();
  test_assert(drag && d.describe() == before);

  /* Totals, not steps. Angles go round more than once. */
  TransformDelta t;
  t.kind = DeltaKind::Rotate;
  t.axis = double3(0, 0, 1);
  for (int i = 1; i <= 72; i++) {
    t.angle = i * 0.174532925199433;
    must(drag->update(*d.ctx, t));
  }
  test_assert(std::fabs(d.obj(a).channels.get(trs::kRotationEuler + 2) - 4 * 3.14159265358979) <
              1e-9);
  test_assert(d.obj(b).channels.get(trs::kRotationEuler + 2) == 0.0);
  t = TransformDelta();
  t.vector = double3(0, 7, 0);
  must(drag->update(*d.ctx, t));
  must(drag->update(*d.ctx, t));
  d.evaluate();
  test_assert(d.isAt(a, 1, 7) && d.isAt(h, 5, 7) && d.isAt(b, 3, 7));

  /* A failure puts all of them back to the last update. */
  const std::string last = d.describe();
  t.vector = double3(0, 8, 0);
  d.host(h).onWrite = [] { return boom("no"); };
  test_assert(isError(drag->update(*d.ctx, t), StatusCode::Internal, "'h': no"));
  test_assert(d.describe() == last);
  /* Also one that failed after it wrote. */
  bool once = true;
  d.host(h).onWrite = [&] {
    if (once) {
      once = false;
      d.rt.setStickyError(boom("threw"));
    }
    return Status();
  };
  test_assert(isError(drag->update(*d.ctx, t), StatusCode::Internal, "'h': threw"));
  test_assert(d.describe() == last);
  d.host(h).onWrite = nullptr;
  t.vector = double3(0, 9, 0);
  must(drag->update(*d.ctx, t));
  must(d.ex->commit(*d.ctx));
  const std::string after = d.describe();
  d.undo();
  test_assert(d.describe() == before);
  d.redo();
  test_assert(d.describe() == after);
  d.undo();

  /* Cancelled. */
  must(d.ex->beginInteractive(*d.ctx, *def, def->newInputs()));
  drag = d.ex->interactiveCommand()->as<ApplyDelta>();
  must(drag->update(*d.ctx, t));
  test_assert(d.describe() != before);
  must(d.ex->cancel(*d.ctx));
  test_assert(d.describe() == before && d.history->canRedo());
}

void testDelete()
{
  CommandDoc d;
  Rand r(31);
  const BlockHandle top = d.object("top"), mid = d.object("mid"), low = d.object("low"),
                    leaf = d.object("leaf"), side = d.object("side"), out = d.loose("out"),
                    h = d.hostObject("h", 1.0);
  d.lib->setFlag(out, BlockFlags::FakeUser, true);
  d.setParent(mid, top);
  d.setParent(low, mid);
  d.setParent(leaf, low);
  d.setParent(side, mid);
  d.setParent(out, mid);
  d.host(h).info.parent = d.idOf(mid);
  const auto scramble = [&] {
    for (BlockHandle x : {top, mid, low, leaf, side, out}) {
      const double s = r.range(0.5, 2.0);
      d.obj(x).channels = trsChannels(r, *d.trs, double3(s, s, s), 5.0);
    }
  };
  scramble();
  const auto parent = [&](BlockHandle x) { return d.lib->resolveRef(d.obj(x).parent.cell()); };
  const auto worlds = [&] {
    std::vector<mat4d> w;
    for (BlockHandle x : {leaf, side, out, low}) {
      w.push_back(d.stateOf(x).world);
    }
    return w;
  };

  /* One in the middle: children, also outside the scene, go to its parent. */
  std::vector<mat4d> w = worlds();
  d.select({mid});
  PropertyBag res = d.cycle(kDeleteObjectsCommand);
  test_assert(res.get("count")->asInt() == 1 && res.get("approximate")->asInt() == 0);
  test_assert(parent(low) == top && parent(side) == top && parent(out) == top);
  test_assert(parent(leaf) == low);
  std::vector<mat4d> now = worlds();
  for (int i = 0; i < 4; i++) {
    test_assert(closeTo(now[i], w[i]));
  }
  test_assert(!d.lib->data(mid) && d.sc().contains(*d.lib, mid));
  d.undo();
  test_assert(parent(low) == mid && d.sc().objects(*d.lib).size() == 6);

  /* A parent and its child, and the root of them. */
  d.select({low, top, mid});
  res = d.cycle(kDeleteObjectsCommand);
  test_assert(res.get("count")->asInt() == 3);
  test_assert(!parent(leaf) && d.obj(leaf).parent.cell().isNull() && !parent(side));
  test_assert(closeTo(d.stateOf(leaf).world, w[0]) && closeTo(d.stateOf(side).world, w[1]));
  d.undo();

  /* Every other one of a chain: a child's new parent is one that is moved itself, and that only
   * approximately. The deeper one comes first in the Library. */
  CommandDoc e;
  const BlockHandle e4 = e.object("e4"), e2 = e.object("e2"), e1 = e.object("e1"),
                    e3 = e.object("e3"), e0 = e.object("e0");
  e.setParent(e4, e3);
  e.setParent(e3, e2);
  e.setParent(e2, e1);
  e.setParent(e1, e0);
  for (BlockHandle x : {e4, e3, e2, e1, e0}) {
    e.obj(x).channels = trsChannels(r, *e.trs, r.scale(2, false), 5.0);
  }
  e.obj(e4).setTransformType(kMatrixTransform);
  e.obj(e4).channels = e.matrix->defaults();
  const mat4d w4 = e.stateOf(e4).world;
  const double3 at2 = solve::translationOf(e.stateOf(e2).world);
  e.select({e3, e1});
  res = e.cycle(kDeleteObjectsCommand);
  test_assert(res.get("count")->asInt() == 2 && res.get("approximate")->asInt() == 1);
  test_assert(e.lib->resolveRef(e.obj(e4).parent.cell()) == e2 &&
              e.lib->resolveRef(e.obj(e2).parent.cell()) == e0);
  test_assert(closeTo(e.stateOf(e4).world, w4, 1e-8));
  test_assert(lengthOf(solve::translationOf(e.stateOf(e2).world) - at2) < 1e-9);

  /* Without keeping: the children are left alone, and jump. */
  d.select({mid});
  const ChannelBlock lowChannels = d.obj(low).channels;
  res = d.cycle(kDeleteObjectsCommand, {{"keepChildrenWorld", PropValue::ofBool(false)}});
  test_assert(d.obj(low).channels == lowChannels && !parent(low));
  test_assert(!d.obj(low).parent.cell().isNull());
  d.undo();

  /* A child that can't be placed, and one without a type, don't stop it. */
  put(d.obj(top).channels, trs::kScale, double3(1, 0, 1));
  d.obj(side).setTransformType("test.missing");
  d.select({mid});
  res = d.cycle(kDeleteObjectsCommand);
  test_assert(parent(low) == top && parent(side) == top);
  test_assert(res.get("approximate")->asInt() == 2);
  d.undo();
  put(d.obj(top).channels, trs::kScale, double3(1, 1, 1));
  d.obj(side).setTransformType(kTrsTransform);

  /* A cycle, with one of it deleted. */
  d.setParent(top, low);
  d.select({mid});
  d.cycle(kDeleteObjectsCommand);
  d.undo();
  d.setParent(top, BlockHandle());

  /* A read that throws stops it before anything is done. */
  d.host(h).onRead = [&](ObjectInfo &) { d.rt.setStickyError(boom("read")); };
  d.host(h).info.parent = BlockId();
  d.setParent(mid, h);
  test_assert(isError(d.fails(kDeleteObjectsCommand), StatusCode::Internal, "read"));
  d.host(h).onRead = nullptr;
  d.cycle(kDeleteObjectsCommand);
  d.undo();

  /* A child that can't be written: the ones before it are taken back. */
  struct Parented : Host {
    Status setParentId(const BlockId &)
    {
      return boom("no");
    }
    static lbt::Struct<Parented> *defineBindings()
    {
      auto *st = new lbt::Struct<Parented>("test::Parented", sizeof(Parented));
      BIND_STRUCT_METHOD(st, read, MARGS("info", "channels"));
      BIND_STRUCT_METHOD(st, writeChannels, MARGS("channels"));
      BIND_STRUCT_METHOD(st, setParentId, MARGS("parent"));
      return st;
    }
  };
  must(registerImpl<Transformable, Parented>(d.rt, "test.Parented"));
  const InterfaceId iface = InterfaceId::of(Transformable::ifaceName);
  Parented refusing;
  refusing.info = d.host(h).info;
  refusing.info.parent = d.idOf(mid);
  refusing.channels = d.host(h).channels;
  d.setParent(mid, BlockHandle());
  must(d.lib->setOverride(
      h, iface, IfaceRef(d.rt.ifaces().find(TypeId::of("test.Parented"), iface), &refusing)));
  test_assert(isError(d.fails(kDeleteObjectsCommand), StatusCode::Internal, "'h': no"));
  d.lib->clearOverride(h, iface);

  /* A deletion that is refused takes the reparenting back: a listener left an error. */
  CommandDoc f;
  const BlockHandle up = f.object("up", 1.0), down = f.object("down", 2.0);
  f.setParent(down, up);
  f.select({up});
  bool armed = true;
  ScopedConnection l = f.rt.ext<ChangeBus>().addListener([&](const OwnerKey *k) {
    if (armed && *k == OwnerKey::ofBlock(f.idOf(down)) && f.obj(down).parent.cell().isNull() &&
        f.obj(down).channels.get(trs::kLocation) == 3.0)
    {
      armed = false;
      f.rt.setStickyError(boom("left behind"));
    }
  });
  test_assert(!f.fails(kDeleteObjectsCommand).ok());
  test_assert(!armed);
  f.ok(kDeleteObjectsCommand);
  f.evaluate();
  test_assert(f.isAt(down, 3));

  /* The second child fails, after the first one has been written. */
  for (int by = 0; by < 2; by++) {
    CommandDoc k;
    bool throws = false;
    k.addFnModel("test.fn", [&](const EvalCtx &, const TransformState &p) {
      if (throws) {
        k.rt.setStickyError(boom("model"));
      }
      return p;
    });
    const BlockHandle kh = k.hostObject("h", 1.0), gone = k.object("gone", 4.0),
                      one = k.object("one", 1.0), two = k.object("two", 2.0);
    k.setParent(gone, kh);
    k.setParent(one, gone);
    k.setParent(two, gone);
    if (by == 1) {
      k.obj(two).setTransformType("test.fn");
      k.obj(two).channels = ChannelBlock(1);
    }
    k.select({gone});
    ScopedConnection arm = k.rt.ext<ChangeBus>().addListener([&](const OwnerKey *key) {
      if (*key == OwnerKey::ofBlock(k.idOf(one))) {
        throws = by == 1;
        k.host(kh).onRead = [&k, by](ObjectInfo &) {
          if (by == 0) {
            k.rt.setStickyError(boom("read"));
          }
        };
      }
    });
    /* Undoing doesn't read `h`: not the parent of the one that is written. */
    const std::string was = k.describe();
    Result<PropertyBag> res2 = k.run(kDeleteObjectsCommand);
    test_assert(isError(res2.status(), StatusCode::Internal, by ? "model" : "read"));
    test_assert(k.describe() == was && k.history->size() == 0);
    throws = false;
    k.host(kh).onRead = nullptr;
    arm = ScopedConnection();
    /* A model that can't be placed at all isn't one of those failures. */
    PropertyBag done = k.cycle(kDeleteObjectsCommand);
    test_assert(done.get("approximate")->asInt() == by);
  }

  /* What a deletion takes along has children too. */
  CommandDoc m;
  const BlockHandle target = m.object("target", 1.0), user = m.object("user", 10.0),
                    child = m.object("child", 100.0);
  must(m.lib->setSlot(user, TypeId::of("test.Shape"), 1, m.idOf(target), RefKind::Strong));
  m.setParent(child, user);
  m.select({target});
  PropertyBag two = m.cycle(kDeleteObjectsCommand);
  test_assert(two.get("count")->asInt() == 2 && m.obj(child).parent.cell().isNull());
  m.evaluate();
  test_assert(m.isAt(child, 110));

  /* An error that was pending doesn't keep undo from writing, and stays. */
  ObjectChanges changes;
  const ChannelBlock old = m.obj(child).channels, defaults = m.trs->defaults();
  ObjectChanges::Values values;
  values.channels = &defaults;
  must(changes.write(*m.ctx, child, values));
  m.rt.setStickyError(boom("pending"));
  changes.undo(*m.ctx);
  test_assert(m.obj(child).channels == old);
  test_assert(isError(m.rt.takeStickyError(), StatusCode::Internal, "pending"));

  /* Many at once. */
  CommandDoc g;
  Vector<BlockId> ids;
  for (int i = 0; i < 2000; i++) {
    const BlockHandle o = g.object("o");
    if (i % 4 == 0) {
      ids.append(g.idOf(o));
    }
  }
  g.sc().setSelection(*g.lib, ids);
  g.ok(kDeleteObjectsCommand);
  test_assert(g.sc().objects(*g.lib).size() == 1500);
}

} // namespace

int main()
{
  testSetParent();
  testFailingWrites();
  testTransformType();
  testRotationMode();
  testApplyDelta();
  testDrag();
  testDelete();
  return test_end();
}
