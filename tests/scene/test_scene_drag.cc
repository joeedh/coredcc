/* Drags with a delta for each object, pivots, nudges that are one step, and drags that
 * did nothing. */
#include "command_doc.h"

#include "coredcc/api/ref_text.h"

#include <cmath>

test_init;

using namespace scene_test;

namespace {

TransformDelta translate(double x, double y, double z)
{
  TransformDelta t;
  t.vector = double3(x, y, z);
  return t;
}

ApplyDelta *beginDrag(CommandDoc &d)
{
  const CommandDef *def = d.rt.ext<CommandRegistry>().find(kDragObjectDeltaCommand);
  must(d.ex->beginInteractive(*d.ctx, *def, def->newInputs()));
  return d.ex->interactiveCommand()->as<ApplyDelta>();
}

Status update(CommandDoc &d, const Vector<TransformDelta> &deltas)
{
  return d.ex->updateInteractive(*d.ctx, [&](Context &c, Command &cmd) {
    return cmd.as<ApplyDelta>()->update(c,
                                        std::span(deltas.data(), size_t(deltas.size())));
  });
}

/* ---- a delta for each ---- */

void testEach()
{
  for (int k = 0; k < 40 * coredcc_test::testScale(); k++) {
    CommandDoc d, one;
    Rand r(k);
    Vector<BlockHandle> all, others;
    for (CommandDoc *doc : {&d, &one}) {
      Rand same(1000 + k);
      for (int i = 0; i < 8; i++) {
        const BlockHandle h = doc->object(("o" + std::to_string(i)).c_str());
        const TransformType &t = i & 1 ? *doc->fbx : *doc->trs;
        if (i & 1) {
          doc->obj(h).setTransformType(kFbxTransform);
        }
        doc->obj(h).channels = i & 1 ? fbxChannels(same, t, 0, double3(1, 2, 1), 0.5)
                                     : trsChannels(same, t, double3(1, 1, 1), 3.0);
        (doc == &d ? all : others).append(h);
        if (i > 4) {
          doc->setParent(h, (doc == &d ? all : others)[i - 3]);
        }
      }
    }
    Vector<BlockId> ids;
    for (int i = 0; i < 8; i += 1 + int(r.below(2))) {
      ids.append(d.idOf(all[i]));
    }
    d.sc().setSelection(*d.lib, ids);
    const std::string before = d.describe();
    ApplyDelta *drag = beginDrag(d);
    test_assert(drag->count() > 0 && drag->count() <= int(ids.size()));
    Vector<TransformDelta> deltas;
    for (int step = 0; step < 3; step++) {
      deltas.clear();
      for (int i = 0; i < drag->count(); i++) {
        TransformDelta t = translate(r.range(-2, 2), r.range(-2, 2), r.range(-2, 2));
        if (r.coin()) {
          t = TransformDelta();
          t.kind = DeltaKind::Rotate;
          t.axis = r.vec(1.0);
          t.angle = r.range(-2, 2);
          const Result<double3> p = drag->startPivot(*d.ctx, i);
          test_assert(p.ok());
          t.pivot = *p;
        }
        deltas.append(t);
      }
      must(update(d, deltas));
    }
    Vector<TransformDelta> few = deltas;
    few.pop_back();
    test_assert(update(d, few).code() == StatusCode::InvalidArgument);
    few = deltas;
    few.append(TransformDelta());
    test_assert(update(d, few).code() == StatusCode::InvalidArgument);

    /* As if each had been moved by itself. */
    for (int i = 0; i < drag->count(); i++) {
      int at = 0;
      while (!(all[at] == drag->object(i))) {
        at++;
      }
      const Result<TransformState> start = drag->startState(*d.ctx, i);
      test_assert(start.ok() && sameBits(*start, one.stateOf(others[at])));
      const Result<TransformState> above =
          computeParentState(*one.lib, others[at], one.sc().time());
      test_assert(above.ok() && sameBits(*above, drag->startParent(i)));
    }
    for (int i = 0; i < drag->count(); i++) {
      int at = 0;
      while (!(all[at] == drag->object(i))) {
        at++;
      }
      const TransformDelta &t = deltas[i];
      one.select({others[at]});
      const BlockId id = one.idOf(others[at]);
      PropertyBag out =
          one.ok(kApplyObjectDeltaCommand,
                 {{"kind", PropValue::ofEnum(int64_t(t.kind))},
                  {"vector", PropValue::ofVec3(t.vector[0], t.vector[1], t.vector[2])},
                  {"axis", PropValue::ofVec3(t.axis[0], t.axis[1], t.axis[2])},
                  {"pivot", PropValue::ofVec3(t.pivot[0], t.pivot[1], t.pivot[2])},
                  {"angle", PropValue::ofFloat(t.angle)},
                  {"objects", PropValue::ofString(formatBlockId(one.rt, id).c_str())}});
      test_assert(out.get("skipped")->asInt() == 0);
      const double e =
          difference(d.stateOf(all[at]).world, one.stateOf(others[at]).world);
      test_assert(e < 1e-9);
    }
    must(d.ex->commit(*d.ctx));
    const std::string after = d.describe();
    d.undo();
    test_assert(d.describe() == before);
    d.redo();
    test_assert(d.describe() == after);
  }

  /* Individual origins. One that can't be moved stays, the others go. */
  CommandDoc d;
  const BlockHandle a = d.object("a", 1), b = d.object("b", 5);
  d.select({a, b});
  ApplyDelta *drag = beginDrag(d);
  test_assert(drag->count() == 2 && drag->object(0) == a && drag->object(1) == b);
  TransformDelta turn;
  turn.kind = DeltaKind::Rotate;
  turn.axis = double3(0, 0, 1);
  turn.angle = 0.5;
  Vector<TransformDelta> deltas;
  for (int i = 0; i < 2; i++) {
    turn.pivot = *drag->startPivot(*d.ctx, i);
    deltas.append(turn);
  }
  must(update(d, deltas));
  d.evaluate();
  test_assert(d.isAt(a, 1) && d.isAt(b, 5));
  test_assert(std::fabs(d.obj(b).channels.get(trs::kRotationEuler + 2) - 0.5) < 1e-12);
  deltas[0].angle = std::nan("");
  test_assert(update(d, deltas).code() == StatusCode::InvalidArgument);
  must(d.ex->cancel(*d.ctx));
}

/* ---- pivots ---- */

void testPivot()
{
  Types t;
  Rand r(9);
  for (int k = 0; k < 500 * coredcc_test::testScale(); k++) {
    const TransformState parent = chain(r, t, int(r.below(3)), 2.0, true);
    const ChannelBlock c = trsChannels(r, *t.trs, r.scale(2, true), 3.0);
    const Result<double3> p = transformPivot(*t.trs, EvalCtx(c), parent);
    test_assert(p.ok() && lengthOf(*p - solve::translationOf(
                                            eval(*t.trs, c, parent).world)) == 0.0);

    /* The point of an FBX node that stays where it is, whatever its rotation. */
    ChannelBlock f = fbxChannels(r, *t.fbx, fbx::kRSrs, r.scale(2, true), 1.0);
    const Result<double3> q = transformPivot(*t.fbx, EvalCtx(f), parent);
    const mat4d first = eval(*t.fbx, f, parent).world;
    put(f, fbx::kRotation, r.vec(3.0));
    const Result<double3> same = transformPivot(*t.fbx, EvalCtx(f), parent);
    test_assert(q.ok() && same.ok() && lengthOf(*q - *same) == 0.0);
    const mat4d second = eval(*t.fbx, f, parent).world;
    /* A point x of the object with first * x = pivot; then second * x is it too. */
    mat4d inverse = first;
    inverse.invert();
    const double3 x = litestl::math::transformPoint(inverse, *q);
    test_assert(lengthOf(litestl::math::transformPoint(second, x) - *q) < 1e-6);
  }

  CommandDoc d;
  const BlockHandle a = d.object("a", 1, 2, 3), b = d.object("b", 10);
  d.setParent(a, b);
  Result<double3> p = computePivot(*d.lib, a, 0.0);
  test_assert(p.ok() && lengthOf(*p - double3(11, 2, 3)) == 0.0);
  d.obj(a).setTransformType(kFbxTransform);
  d.obj(a).channels = d.fbx->defaults();
  put(d.obj(a).channels, fbx::kTranslation, double3(1, 2, 3));
  put(d.obj(a).channels, fbx::kRotationPivot, double3(0, 0, 4));
  put(d.obj(a).channels, fbx::kRotationOffset, double3(0, 5, 0));
  put(d.obj(a).channels, fbx::kScalingPivot, double3(7, 7, 7));
  p = computePivot(*d.lib, a, 0.0);
  test_assert(p.ok() && lengthOf(*p - double3(11, 7, 7)) == 0.0);
  d.obj(a).setTransformType("test.none");
  test_assert(computePivot(*d.lib, a, 0.0).status().code() ==
              StatusCode::FailedPrecondition);
  test_assert(!computePivot(*d.lib, d.shapeBlock("s", 1.0), 0.0).ok());

  /* A model's own, and its error. */
  struct Pivoted {
    TransformState evaluate(const EvalCtx &, const TransformState &p) const
    {
      return p;
    }
    Status setWorld(const EvalCtx &,
                    const mat4d &,
                    const TransformState &,
                    SetWorldMode,
                    ChannelBlock &) const
    {
      return Status();
    }
    double3 pivot(const EvalCtx &ctx, const TransformState &p) const
    {
      if (ctx.channel(0) < 0.0 && rt) {
        rt->setStickyError(boom("pivot"));
      }
      return solve::translationOf(p.world) + double3(ctx.channel(0), 0, ctx.time());
    }
    static lbt::Struct<Pivoted> *defineBindings()
    {
      auto *st = new lbt::Struct<Pivoted>("test::Pivoted", sizeof(Pivoted));
      BIND_STRUCT_METHOD(st, evaluate, MARGS("ctx", "parent"));
      BIND_STRUCT_METHOD(
          st, setWorld, MARGS("current", "target", "parent", "mode", "out"));
      BIND_STRUCT_METHOD(st, pivot, MARGS("ctx", "parent"));
      return st;
    }
    Runtime *rt = nullptr;
  };
  must(registerImpl<TransformModel, Pivoted>(d.rt, "test.pivoted#impl"));
  Pivoted *model = litestl::alloc::New<Pivoted>("Pivoted");
  model->rt = &d.rt;
  Result<Owned<TransformModel>> owned =
      own<TransformModel>(d.rt, "test.pivoted#impl", model);
  must(owned.status());
  TransformTypeInfo info;
  info.name = "test.pivoted";
  info.channels.append(PropertyDef::floatProp("x"));
  must(d.rt.ext<TransformRegistry>()
           .add(std::move(info), std::move(owned).value())
           .status());
  d.obj(a).setTransformType("test.pivoted");
  d.obj(a).channels = ChannelBlock(1);
  d.obj(a).channels.set(0, 3.0);
  p = computePivot(*d.lib, a, 2.0);
  test_assert(p.ok() && lengthOf(*p - double3(13, 0, 2)) == 0.0);
  d.obj(a).channels.set(0, -3.0);
  test_assert(
      isError(computePivot(*d.lib, a, 2.0).status(), StatusCode::Internal, "pivot"));
  test_assert(!d.rt.hasStickyError());
}

/* ---- nudges, and drags that did nothing ---- */

void testNudges()
{
  CommandDoc d;
  const BlockHandle a = d.object("a", 1), b = d.object("b", 2);
  d.select({a, b});
  const std::string start = d.describe();
  const auto nudge = [&](int64_t gesture, double x) {
    d.ok(
        kApplyObjectDeltaCommand,
        {{"vector", PropValue::ofVec3(x, 0, 0)}, {"gesture", PropValue::ofInt(gesture)}});
    return d.history->size();
  };
  const int64_t g = int64_t(beginGesture(d.rt)), g2 = int64_t(beginGesture(d.rt));
  const int n = d.history->size();
  test_assert(nudge(g, 1) == n + 1 && nudge(g, 1) == n + 1 && nudge(g, 1) == n + 1);
  test_assert(d.obj(a).channels.get(0) == 4.0);
  const std::string moved = d.describe();
  d.undo();
  test_assert(d.describe() == start);
  d.redo();
  test_assert(d.describe() == moved);
  test_assert(nudge(g, 1) == n + 2); /* not into one that was redone */
  test_assert(nudge(g2, 1) == n + 3);
  test_assert(nudge(0, 1) == n + 4 && nudge(0, 1) == n + 5);
  d.ok(kApplyObjectDeltaCommand, {{"vector", PropValue::ofVec3(1, 0, 0)}});
  test_assert(d.history->size() == n + 6);

  /* Other objects, or what isn't what it was, are another step. */
  test_assert(nudge(g, 1) == n + 7);
  d.select({a});
  test_assert(nudge(g, 1) == n + 8);
  d.obj(a).channels.set(1, 5.0);
  test_assert(nudge(g, 1) == n + 9 && nudge(g, 1) == n + 9);
  d.obj(a).flags = ObjectFlags::Unselectable;
  d.obj(a).flags = ObjectFlags::None;
  must(writeLocks(*d.ctx, a, 2));
  test_assert(nudge(g, 1) == n + 10);
  d.select({b, a});
  test_assert(nudge(g, 1) == n + 11);
  d.select({a, b});
  test_assert(nudge(g, 1) == n + 12);
  for (int i = 0; i < 12; i++) {
    d.undo();
  }
  test_assert(d.obj(a).channels.get(0) == 1.0 && d.obj(b).channels.get(0) == 2.0);

  /* A click. */
  ApplyDelta *drag = beginDrag(d);
  test_assert(!drag->changed());
  must(drag->update(*d.ctx, translate(1, 0, 0)));
  test_assert(drag->changed());
  must(drag->update(*d.ctx, translate(0, 0, 0)));
  test_assert(!drag->changed());
  must(d.ex->cancel(*d.ctx));
}

} // namespace

int main()
{
  testEach();
  testPivot();
  testNudges();
  return test_end();
}
