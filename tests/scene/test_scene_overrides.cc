/* Channels that are driven from elsewhere (OverrideProvider): evaluation, computeState and the commands
 * against documents that store the driven values. Drags at a time that isn't 0. */
#include "command_doc.h"

#include "coredcc/api/ref_text.h"
#include "coredcc/scene/overrides.h"
#include "coredcc/scene/state_cache.h"

#include <cmath>
#include <limits>

test_init;

using namespace scene_test;

namespace {

/* value = base + slope * time. */
struct Curve {
  BlockHandle object;
  int32_t channel = 0;
  double base = 0.0;
  double slope = 0.0;

  double at(double time) const
  {
    return base + slope * time;
  }
};

struct Curves : OverrideProvider {
  Vector<Curve> curves;
  std::function<Status(BlockHandle)> before;
  int calls = 0;

  Status overridesOf(const Library &,
                     BlockHandle object,
                     const TransformType &,
                     double time,
                     ChannelOverrides &out) override
  {
    calls++;
    test_assert(!out.any());
    if (before) {
      CDCC_TRY(before(object));
    }
    for (const Curve &c : curves) {
      if (c.object == object) {
        out.set(c.channel, c.at(time));
      }
    }
    return Status();
  }
};

TransformDelta translate(double x, double y, double z)
{
  TransformDelta t;
  t.vector = double3(x, y, z);
  return t;
}

/* A random document: TRS, FBX and matrix objects, a type that depends on the time, hosts, and cycles. */
struct Random : CommandDoc {
  Vector<BlockHandle> all;

  explicit Random(uint64_t seed)
  {
    Rand r(seed);
    addFnModel("test.slide", [](const EvalCtx &c, const TransformState &p) {
      TransformState s = p;
      double *m = s.world;
      m[12] += c.channel(0) * c.time();
      return s;
    });
    Types types;
    for (int i = 0; i < 12; i++) {
      const std::string name = "o" + std::to_string(i);
      const int kind = int(r.below(5));
      if (kind == 4) {
        all.append(hostObject(name.c_str(), r.range(-3, 3)));
        host(all[i]).info.parent =
            i > 0 && r.coin() ? idOf(all[int(r.below(uint32_t(i)))]) : BlockId();
        continue;
      }
      const BlockHandle h = object(name.c_str());
      all.append(h);
      const char *const names[] = {kTrsTransform, kFbxTransform, kMatrixTransform, "test.slide"};
      obj(h).setTransformType(names[kind]);
      const TransformType &t = *rt.ext<TransformRegistry>().find(TypeId::of(names[kind]));
      obj(h).channels = kind == 0 ? trsChannels(r, t, r.scale(2, true), 3.0) :
                        kind == 1 ? fbxChannels(r, t, int(r.below(3)), r.scale(2, false), 1.0) :
                                    t.defaults();
      for (int k = matrix_transform::kTranslation; kind == 2 && k < obj(h).channels.size(); k++) {
        obj(h).channels.set(k, r.range(-3, 3));
      }
      if (kind == 3) {
        obj(h).channels.set(0, r.range(-3, 3));
      }
      if (i > 0 && r.below(4)) {
        setParent(h, all[int(r.below(uint32_t(i)))]);
      }
    }
    if (r.below(3) == 0 && lib->get(all[0])->type() == TypeId::of(kObjectType)) {
      setParent(all[0], all[int(r.below(12))]); /* a cycle, if that is below it */
    }
    sc().frame = std::floor(r.range(0, 100));
  }
  ChannelBlock &channels(BlockHandle h)
  {
    return lib->get(h)->type() == TypeId::of(kObjectType) ? obj(h).channels : host(h).channels;
  }
  const TransformType &typeOf(BlockHandle h)
  {
    return *readObject(*lib, h)->type;
  }
  void randomCurves(Rand &r, Curves &out)
  {
    for (BlockHandle h : all) {
      const bool matrixType = channels(h).size() == matrix->channelCount();
      for (int k = int(r.below(4)); k > 0 && r.coin(); k--) {
        Curve c;
        c.object = h;
        /* Of a matrix only the translation, so that it keeps an inverse. */
        c.channel = matrixType ? matrix_transform::kTranslation + int(r.below(3)) : int(r.below(uint32_t(channels(h).size())));
        c.base = r.range(0.5, 2.0);
        c.slope = r.range(-0.01, 0.01);
        out.curves.append(c);
      }
    }
  }
  void bake(const Curves &curves)
  {
    for (const Curve &c : curves.curves) {
      channels(c.object).set(c.channel, c.at(sc().time()));
    }
  }
};

void testEvaluation()
{
  int driven = 0;
  for (int k = 0; k < 60 * coredcc_test::testScale(); k++) {
    Random d(100 + k), baked(100 + k);
    Rand r(k);
    Curves curves;
    d.randomCurves(r, curves);
    driven += int(curves.curves.size());
    baked.bake(curves);
    must(setOverrideProvider(*d.lib, curves));
    const std::string before = d.describe();
    d.evaluate();
    baked.evaluate();
    const double time = d.sc().time();
    test_assert(time == baked.sc().time());
    detail::StateCache states(*d.lib, time);
    for (int i = 0; i < int(d.all.size()); i++) {
      const BlockHandle h = d.all[i], b = baked.all[i];
      const ObjectEval e = d.result(h), f = baked.result(b);
      test_assert(e.status == f.status && sameBits(e.state, f.state));
      const Result<TransformState> s = computeState(*d.lib, h, time);
      const Result<TransformState> t = computeState(*baked.lib, b, time);
      test_assert(s.ok() && t.ok() && sameBits(*s, *t) && sameBits(*s, e.state));
      const Result<TransformState> cached = states.state(h);
      test_assert(cached.ok() && sameBits(*cached, *s));
      const Result<TransformState> p = computeParentState(*d.lib, h, time);
      const Result<TransformState> q = computeParentState(*baked.lib, b, time);
      test_assert(p.ok() && q.ok() && sameBits(*p, *q));
    }
    test_assert(d.describe() == before); /* what is stored stays */
    clearOverrideProvider(*d.lib, curves);
  }
  test_assert(driven > 200);
}

void testProvider()
{
  CommandDoc d;
  d.object("first", 7);
  const BlockHandle a = d.object("a", 1), b = d.object("b", 2);
  d.setParent(b, a);
  Curves curves, other;
  Curve c;
  c.object = a;
  c.channel = trs::kLocation;
  c.base = 10;
  c.slope = 4;
  curves.curves.append(c);
  d.sc().frame = 12; /* half a second */
  d.evaluate();
  test_assert(d.isAt(a, 1) && d.isAt(b, 3) && curves.calls == 0);

  must(setOverrideProvider(*d.lib, curves));
  test_assert(setOverrideProvider(*d.lib, other).code() == StatusCode::AlreadyExists);
  clearOverrideProvider(*d.lib, other); /* not its */
  d.evaluate();
  test_assert(d.isAt(a, 12) && d.isAt(b, 14));
  d.sc().frame = 24;
  d.evaluate();
  test_assert(d.isAt(a, 14) && d.isAt(b, 16));
  /* At the time that is asked for. */
  Result<TransformState> s = computeState(*d.lib, b, 0.0);
  test_assert(s.ok() && solve::translationOf(s->world)[0] == 12.0);

  /* Another Library has none. */
  {
    CommandDoc e;
    const BlockHandle x = e.object("x", 5);
    e.evaluate();
    test_assert(e.isAt(x, 5));
  }

  /* Errors. `b` is a child: it is evaluated where its parent is. */
  curves.before = [&](BlockHandle h) {
    return h == a ? Status::error(StatusCode::FailedPrecondition, "no value") : Status();
  };
  test_assert(isError(evaluateScene(*d.ctx), StatusCode::FailedPrecondition, "no value"));
  test_assert(d.statusOf(a) == EvalStatus::ModelFailed && d.statusOf(b) == EvalStatus::Ok &&
              d.isAt(a, 0) && d.isAt(b, 2));
  test_assert(isError(computeState(*d.lib, b, 0.0).status(), StatusCode::FailedPrecondition));
  test_assert(isError(d.fails(kApplyDeltaCommand,
                              {{"objects", PropValue::ofString(
                                               formatBlockId(d.rt, d.idOf(a)).c_str())}}),
                      StatusCode::FailedPrecondition,
                      "no value"));
  /* A script's exception. */
  curves.before = [&](BlockHandle h) {
    if (h == a) {
      d.rt.setStickyError(boom("thrown"));
    }
    return Status();
  };
  test_assert(isError(evaluateScene(*d.ctx), StatusCode::Internal, "thrown"));
  test_assert(!d.rt.hasStickyError() && d.statusOf(a) == EvalStatus::ModelFailed);
  test_assert(isError(computeState(*d.lib, b, 0.0).status(), StatusCode::Internal, "thrown"));
  test_assert(isError(readObjectAt(*d.lib, a, 0.0).status(), StatusCode::Internal, "thrown"));
  test_assert(!d.rt.hasStickyError());
  /* Nothing is edited while a scene evaluates, and no provider comes or goes. */
  Status busy, edit;
  curves.before = [&](BlockHandle h) {
    if (h == a) {
      busy = setOverrideProvider(*d.lib, other);
      edit = writeFlags(*d.ctx, a, ObjectFlags::Hidden);
    }
    return Status();
  };
  d.evaluate();
  test_assert(busy.code() == StatusCode::Busy && edit.code() == StatusCode::Busy);
  curves.before = nullptr;

  /* Values that aren't finite, and channels that aren't there. */
  curves.curves[0].base = std::numeric_limits<double>::quiet_NaN();
  c.channel = 99;
  curves.curves.append(c);
  c.channel = -1;
  curves.curves.append(c);
  d.evaluate();
  test_assert(d.statusOf(a) == EvalStatus::Invalid);

  clearOverrideProvider(*d.lib, curves);
  d.evaluate();
  test_assert(d.isAt(a, 1) && d.isAt(b, 3));
  must(setOverrideProvider(*d.lib, other));
  d.evaluate();
  test_assert(other.calls == 3);
  clearOverrideProvider(*d.lib, other);
}

/* Who waits for a change hears of a provider's coming and going. */
void testNotice()
{
  CommandDoc d;
  Curves curves;
  ChangeBus &bus = d.rt.ext<ChangeBus>();
  const uint64_t e0 = bus.epoch();
  must(setOverrideProvider(*d.lib, curves));
  const uint64_t e1 = bus.epoch();
  clearOverrideProvider(*d.lib, curves);
  test_assert(e0 != e1 && e1 != bus.epoch());
}

bool storedStay(Random &d, const Curves &curves, const Vector<double> &was)
{
  bool same = true;
  for (int i = 0; i < int(curves.curves.size()); i++) {
    const Curve &c = curves.curves[i];
    same = same && was[i] == d.channels(c.object).get(c.channel);
  }
  return same;
}

/* Drags against the document with the driven values in it: the driven channels are what they were, and
 * the others are what they are there. An object that has none of its own moves as far. */
void testDrag()
{
  int full = 0, partly = 0, refused = 0;
  for (int k = 0; k < 80 * coredcc_test::testScale(); k++) {
    Random d(500 + k), baked(500 + k);
    Rand r(k);
    Curves curves;
    d.randomCurves(r, curves);
    baked.bake(curves);
    Vector<double> was;
    for (const Curve &c : curves.curves) {
      was.append(d.channels(c.object).get(c.channel));
    }
    must(setOverrideProvider(*d.lib, curves));
    const int i = int(r.below(uint32_t(d.all.size())));
    const BlockHandle h = d.all[i], b = baked.all[i];
    TransformDelta t = translate(r.range(-2, 2), r.range(-2, 2), r.range(-2, 2));
    if (r.coin()) {
      t = TransformDelta();
      t.kind = DeltaKind::Rotate;
      t.axis = r.vec(1.0);
      t.angle = r.range(-1, 1);
      t.pivot = r.vec(2.0);
    }
    const double time = d.sc().time();
    const Result<TransformState> from = computeState(*d.lib, h, time);
    const auto drag = [&](Random &doc, BlockHandle target, int &driven) {
      const CommandDef *def = doc.rt.ext<CommandRegistry>().find(kDragDeltaCommand);
      doc.select({target});
      const Status s = doc.ex->beginInteractive(*doc.ctx, *def, def->newInputs());
      if (s.ok()) {
        ApplyDelta *cmd = doc.ex->interactiveCommand()->as<ApplyDelta>();
        const Status u = cmd->update(*doc.ctx, t);
        driven = cmd->driven();
        must(doc.ex->commit(*doc.ctx));
        return u;
      }
      return s;
    };
    int driven = 0, none = 0;
    const Status s = drag(d, h, driven), u = drag(baked, b, none);
    test_assert(none == 0 && s.code() == u.code());
    if (!s.ok()) {
      clearOverrideProvider(*d.lib, curves);
      refused++;
      continue;
    }
    test_assert(storedStay(d, curves, was));
    bool mine = false;
    const auto isDriven = [&](int channel) {
      bool yes = false;
      for (const Curve &c : curves.curves) {
        yes = yes || (c.object == h && c.channel == channel);
        mine = mine || c.object == h;
      }
      return yes;
    };
    bool changed = false;
    for (int c = 0; c < d.channels(h).size(); c++) {
      if (isDriven(c)) {
        const double want = baked.channels(b).get(c);
        Curve curve;
        for (const Curve &x : curves.curves) {
          curve = x.object == h && x.channel == c ? x : curve;
        }
        changed = changed || std::fabs(want - curve.at(time)) > 1e-6;
      }
      else {
        test_assert(d.channels(h).get(c) == baked.channels(b).get(c));
      }
    }
    test_assert(!changed || driven == 1);
    test_assert(driven == 0 || mine);
    if (driven == 0) {
      /* The object is where the delta puts it, also when those above it are driven. */
      const Result<TransformState> to = computeState(*d.lib, h, time);
      test_assert(from.ok() && to.ok() &&
                  (t.kind != DeltaKind::Translate ||
                   difference(to->world, t.matrix() * from->world) < 1e-6));
      full++;
    }
    else {
      partly++;
    }
    clearOverrideProvider(*d.lib, curves);
  }
  printf("drags: %d all the way, %d of the rest, %d refused\n", full, partly, refused);
  test_assert(full > 10 && partly > 5);
}

/* A drag's overrides are those of its start, also when the drivers move meanwhile; a lone scale too. */
void testDragSteps()
{
  CommandDoc d;
  const BlockHandle a = d.object("a", 1, 2, 3);
  Curves curves;
  Curve c;
  c.object = a;
  c.channel = trs::kLocation + 1;
  c.base = 50;
  curves.curves.append(c);
  must(setOverrideProvider(*d.lib, curves));
  d.select({a});
  const CommandDef *def = d.rt.ext<CommandRegistry>().find(kDragDeltaCommand);
  must(d.ex->beginInteractive(*d.ctx, *def, def->newInputs()));
  ApplyDelta *drag = d.ex->interactiveCommand()->as<ApplyDelta>();
  for (int i = 1; i < 5; i++) {
    must(drag->update(*d.ctx, translate(i, i, i)));
    test_assert(drag->driven() == 1 && drag->locked() == 0 && drag->skipped() == 0);
    test_assert(lengthOf(get(d.obj(a).channels, trs::kLocation) - double3(1 + i, 2, 3 + i)) == 0.0);
    d.evaluate();
    test_assert(d.isAt(a, 1 + i, 50, 3 + i));
  }
  must(drag->update(*d.ctx, translate(3, 0, 1)));
  test_assert(drag->driven() == 0);
  must(d.ex->commit(*d.ctx));
  PropertyBag out = d.ok(kApplyDeltaCommand, {{"vector", PropValue::ofVec3(0, 4, 0)}});
  test_assert(out.get("driven")->asInt() == 1 && out.get("skipped")->asInt() == 0);
  test_assert(lengthOf(get(d.obj(a).channels, trs::kLocation) - double3(4, 2, 4)) == 0.0);
  /* About the place where it is. */
  out = d.ok(kApplyDeltaCommand,
             {{"kind", PropValue::ofEnum(int64_t(DeltaKind::Scale))},
              {"vector", PropValue::ofVec3(2, 2, 2)},
              {"pivot", PropValue::ofVec3(4, 50, 4)}});
  test_assert(out.get("driven")->asInt() == 0);
  test_assert(lengthOf(get(d.obj(a).channels, trs::kScale) - double3(2, 2, 2)) == 0.0);
  clearOverrideProvider(*d.lib, curves);
}

/* What keeps its place under parents that are driven, when it has no drivers itself. */
void testKeepWorld()
{
  int kept = 0, below = 0;
  for (int k = 0; k < 60 * coredcc_test::testScale(); k++) {
    Random d(900 + k);
    Rand r(k);
    Curves curves;
    d.randomCurves(r, curves);
    must(setOverrideProvider(*d.lib, curves));
    Vector<double> was;
    for (const Curve &c : curves.curves) {
      was.append(d.channels(c.object).get(c.channel));
    }
    const BlockHandle h = d.all[int(r.below(12))], p = d.all[int(r.below(12))];
    const double time = d.sc().time();
    const Result<TransformState> from = computeState(*d.lib, h, time);
    d.select({h});
    Result<PropertyBag> out = d.run(kSetParentCommand, {{"parent", d.ptr(p)}});
    if (out.ok() && out->get("approximate")->asInt() == 0) {
      const Result<TransformState> to = computeState(*d.lib, h, time);
      test_assert(from.ok() && to.ok() && difference(from->world, to->world) < 1e-6);
      kept++;
      for (const Curve &c : curves.curves) {
        below += c.object == p ? 1 : 0;
      }
    }
    test_assert(storedStay(d, curves, was));
    clearOverrideProvider(*d.lib, curves);
  }
  test_assert(kept > 8 && below > 2);

  /* Its own driver stands in the way: counted. Children stay when their parent goes. */
  CommandDoc d;
  const BlockHandle a = d.object("a", 1), b = d.object("b", 2), p = d.object("p", 100);
  d.setParent(b, a);
  Curves curves;
  Curve c;
  c.object = a;
  c.channel = trs::kLocation;
  c.base = 10;
  curves.curves.append(c);
  must(setOverrideProvider(*d.lib, curves));
  d.select({a});
  PropertyBag out = d.ok(kSetParentCommand, {{"parent", d.ptr(p)}});
  test_assert(out.get("approximate")->asInt() == 2); /* and b */
  d.undo();
  /* Only the rotation would have to change. */
  put(d.obj(p).channels, trs::kRotationEuler, double3(0, 0, 1));
  put(d.obj(p).channels, trs::kLocation, double3(0, 0, 0));
  curves.curves[0].base = 0.0;
  curves.curves[0].channel = trs::kRotationEuler + 2;
  out = d.ok(kSetParentCommand, {{"parent", d.ptr(p)}});
  test_assert(out.get("approximate")->asInt() >= 1);
  d.undo();
  curves.curves[0].base = 10;
  curves.curves[0].channel = trs::kLocation;
  d.evaluate();
  const ObjectEval e = d.result(b);
  d.ok(kDeleteObjectsCommand);
  d.evaluate();
  test_assert(difference(d.result(b).state.world, e.state.world) < 1e-12 && d.isAt(b, 12));
  d.undo();
  /* A driven child. */
  c.object = b;
  c.channel = trs::kLocation + 1;
  c.base = 3;
  curves.curves.append(c);
  d.evaluate();
  test_assert(d.isAt(b, 12, 3));
  out = d.ok(kDeleteObjectsCommand);
  d.evaluate();
  test_assert(out.get("approximate")->asInt() == 0 && d.isAt(b, 12, 3) &&
              d.obj(b).channels.get(trs::kLocation + 1) == 0.0);
  d.undo();
  /* A conversion takes the driven values. */
  out = d.ok(kSetTransformTypeCommand, {{"type", PropValue::ofString(kMatrixTransform)}});
  test_assert(d.obj(a).channels.get(matrix_transform::kTranslation) == 10.0);
  clearOverrideProvider(*d.lib, curves);
}

/* At a time that isn't 0, under a parent, and of a type, that depends on it. */
void testTime()
{
  CommandDoc d;
  d.addFnModel("test.spin", [](const EvalCtx &ctx, const TransformState &p) {
    /* A quarter turn each second about z, and a step along x. */
    TransformState s = p;
    const double angle = ctx.time() * 1.5707963267948966;
    mat4d m;
    m.identity();
    double *e = m;
    e[0] = e[5] = std::cos(angle);
    e[1] = std::sin(angle);
    e[4] = -e[1];
    e[12] = ctx.time() * ctx.channel(0);
    s.world = p.world * m;
    return s;
  });
  const BlockHandle top = d.object("top"), child = d.object("child", 1, 2, 3);
  d.obj(top).setTransformType("test.spin");
  d.obj(top).channels = ChannelBlock(1);
  d.obj(top).channels.set(0, 5.0);
  d.setParent(child, top);
  d.sc().frame = 72; /* 3 seconds */
  d.evaluate();
  const double3 at = solve::translationOf(d.result(child).state.world);
  test_assert(lengthOf(at - double3(15 + 2, -1, 3)) < 1e-12);
  d.select({child});
  d.ok(kApplyDeltaCommand, {{"vector", PropValue::ofVec3(10, 20, 30)}});
  d.evaluate();
  test_assert(lengthOf(solve::translationOf(d.result(child).state.world) - double3(27, 19, 33)) <
              1e-12);
  d.undo();

  const CommandDef *def = d.rt.ext<CommandRegistry>().find(kDragDeltaCommand);
  must(d.ex->beginInteractive(*d.ctx, *def, def->newInputs()));
  ApplyDelta *drag = d.ex->interactiveCommand()->as<ApplyDelta>();
  for (int i = 1; i < 4; i++) {
    must(drag->update(*d.ctx, translate(i, 2 * i, 0)));
    d.evaluate();
    const double3 p = solve::translationOf(d.result(child).state.world);
    test_assert(lengthOf(p - (at + double3(i, 2 * i, 0))) < 1e-12);
  }
  TransformDelta turn;
  turn.kind = DeltaKind::Rotate;
  turn.axis = double3(0, 0, 1);
  turn.angle = 1.0;
  turn.pivot = at;
  must(drag->update(*d.ctx, turn));
  d.evaluate();
  test_assert(lengthOf(solve::translationOf(d.result(child).state.world) - at) < 1e-12);
  must(d.ex->commit(*d.ctx));

  /* The object's own model is asked at that time too: one that has solvers of its own. */
  const std::string impl = "test.timed#impl";
  struct Timed {
    TransformState evaluate(const EvalCtx &ctx, const TransformState &p) const
    {
      TransformState s = p;
      double *m = s.world;
      m[12] += ctx.channel(0) * ctx.time();
      return s;
    }
    Status setWorld(const EvalCtx &ctx,
                    const mat4d &target,
                    const TransformState &p,
                    SetWorldMode,
                    ChannelBlock &c) const
    {
      const double *m = target, *q = p.world;
      c.set(0, (m[12] - q[12]) / ctx.time());
      return Status();
    }
    static lbt::Struct<Timed> *defineBindings()
    {
      auto *st = new lbt::Struct<Timed>("test::Timed", sizeof(Timed));
      BIND_STRUCT_METHOD(st, evaluate, MARGS("ctx", "parent"));
      BIND_STRUCT_METHOD(st, setWorld, MARGS("current", "target", "parent", "mode", "out"));
      return st;
    }
  };
  must(registerImpl<TransformModel, Timed>(d.rt, impl));
  Result<Owned<TransformModel>> owned =
      own<TransformModel>(d.rt, impl, litestl::alloc::New<Timed>("Timed"));
  must(owned.status());
  TransformTypeInfo info;
  info.name = "test.timed";
  info.channels.append(PropertyDef::floatProp("speed"));
  must(d.rt.ext<TransformRegistry>().add(std::move(info), std::move(owned).value()).status());
  const BlockHandle timed = d.object("timed");
  d.obj(timed).setTransformType("test.timed");
  d.obj(timed).channels = ChannelBlock(1);
  d.obj(timed).channels.set(0, 2.0);
  d.select({timed});
  must(d.ex->beginInteractive(*d.ctx, *def, def->newInputs()));
  drag = d.ex->interactiveCommand()->as<ApplyDelta>();
  must(drag->update(*d.ctx, translate(3, 0, 0)));
  test_assert(std::fabs(d.obj(timed).channels.get(0) - 3.0) < 1e-12);
  must(drag->update(*d.ctx, translate(9, 0, 0)));
  test_assert(std::fabs(d.obj(timed).channels.get(0) - 5.0) < 1e-12);
  must(d.ex->commit(*d.ctx));
  d.evaluate();
  test_assert(d.isAt(timed, 15));
}

} // namespace

int main()
{
  testEvaluation();
  testProvider();
  testNotice();
  testDrag();
  testDragSteps();
  testKeepWorld();
  testTime();
  return test_end();
}
