/* The built-in transform models' evaluate, EvalContext and the registry. */
#include "scene_fixture.h"

#include "coredcc/api/resolve.h"
#include "coredcc/bind/foreign_command.h"

#include "../bind/fake_host.h"

test_init;

using namespace scene_test;
namespace lbt = litestl::binding::types;
namespace sc = coredcc::script;

namespace {

/* ---- channels ---- */

void testChannels()
{
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const double inf = std::numeric_limits<double>::infinity();
  ChannelBlock c(3);
  test_assert(c.size() == 3 && c.get(0) == 0.0);
  c.set(1, 2.5);
  c.set(3, 1.0);
  c.set(-1, 1.0);
  test_assert(c.get(1) == 2.5 && c.get(3) == 0.0 && c.get(-1) == 0.0);
  c.resize(5);
  test_assert(c.size() == 5 && c.get(1) == 2.5 && c.get(4) == 0.0);
  c.resize(1);
  c.resize(2);
  test_assert(c.get(1) == 0.0);
  c.resize(-1);
  test_assert(c.size() == 0 && c == ChannelBlock());

  ChannelBlock a(1), b(1);
  a.set(0, nan);
  test_assert(!(a == b) && !(a == ChannelBlock(2)));
  b.set(0, nan);
  test_assert(a == b);

  struct {
    double value;
    bool asBool;
    int64_t asInt;
    int32_t asEnum; /* of 3 */
  } cases[] = {
      {0.0, false, 0, 0},
      {0.4999, false, 0, 0},
      {0.5, true, 1, 1},
      {1.6, true, 2, 2},
      {2.5, true, 3, 0},
      {-0.5, false, -1, 0},
      {-3.2, false, -3, 0},
      {nan, false, 0, 0},
      {inf, true, int64_t(1) << 53, 0},
      {-inf, false, -(int64_t(1) << 53), 0},
      {1e300, true, int64_t(1) << 53, 0},
      {9007199254740992.0, true, int64_t(1) << 53, 0},
  };
  for (const auto &k : cases) {
    ChannelBlock one(1);
    one.set(0, k.value);
    const EvalContext ctx(one);
    test_assert(ctx.boolChannel(0) == k.asBool);
    test_assert(ctx.intChannel(0) == k.asInt);
    test_assert(ctx.enumChannel(0, 3) == k.asEnum);
  }

  /* Overrides. */
  ChannelBlock stored(3), over(3);
  litestl::util::Vector<uint8_t> set;
  for (int i = 0; i < 3; i++) {
    stored.set(i, i + 1.0);
    over.set(i, (i + 1) * 10.0);
    set.append(uint8_t(i == 1 ? 1 : 0));
  }
  const EvalContext ctx(stored, 2.5, over, set);
  test_assert(ctx.channel(0) == 1.0 && ctx.channel(1) == 20.0 && ctx.channel(2) == 3.0);
  test_assert(ctx.channel(-1) == 0.0 && ctx.channel(3) == 0.0);
  test_assert(ctx.time() == 2.5 && ctx.channelCount() == 3 && ctx.intChannel(1) == 20);
  const ChannelBlock snap = ctx.snapshot();
  test_assert(snap.get(0) == 1.0 && snap.get(1) == 20.0 && stored.get(1) == 2.0);
}

/* ---- evaluate ---- */

struct GoldenNode {
  bool root;
  double channels[fbx::kCount];
  double world[16];
};
const GoldenNode kGolden[] = {
#include "fbx_golden.inc"
};

void testFbxGolden(const Types &t)
{
  TransformState parent;
  int bad = 0, index = 0, types[3] = {};
  for (const GoldenNode &g : kGolden) {
    if (g.root) {
      parent = TransformState();
    }
    ChannelBlock c = t.fbx->defaults();
    for (int i = 0; i < fbx::kCount; i++) {
      c.set(i, g.channels[i]);
    }
    types[int(g.channels[fbx::kInheritType])]++;
    const TransformState s = eval(*t.fbx, c, parent);
    double size = 1.0, e = 0.0;
    const double *w = s.world;
    for (int i = 0; i < 16; i++) {
      size = std::fmax(size, std::fabs(g.world[i]));
      e = worst(e, std::fabs(w[i] - g.world[i]));
    }
    if (!(e < 1e-12 * size)) {
      bad++;
      fprintf(stderr, "golden node %d differs by %g\n", index, e);
    }
    parent = s;
    index++;
  }
  test_assert(bad == 0);
  test_assert(index > 120 && types[0] > 20 && types[1] > 20 && types[2] > 20);
}

/* Against matrices multiplied one by one. */
void testTrs(const Types &t)
{
  Rand r(3);
  for (int i = 0; i < 2000; i++) {
    TransformState parent;
    parent.world = solve::affine(Linear::of(r.rotation()).scaled(r.scale(4, true)) *
                                     Linear::of(r.rotation()),
                                 r.vec(4));
    parent.rotation = r.rotation();
    ChannelBlock c = trsChannels(r, *t.trs, r.scale(3, true), 3.0);
    const int mode = int(c.get(trs::kRotationMode));
    quatd q;
    if (mode < 6) {
      const int axes[6][3] = {
          {0, 1, 2}, {0, 2, 1}, {1, 0, 2}, {1, 2, 0}, {2, 0, 1}, {2, 1, 0}};
      for (int k = 0; k < 3; k++) {
        double3 axis;
        axis[axes[mode][k]] = 1.0;
        const double angle = c.get(trs::kRotationEuler + axes[mode][k]);
        q = quatd::fromAxisAngle(axis, angle) * q;
      }
    } else {
      for (int k = 0; k < 4; k++) {
        q[k] = c.get(trs::kRotationQuaternion + k);
      }
      q.normalize();
    }
    const mat4d want = parent.world * litestl::math::composeTrs(
                                          get(c, trs::kLocation), q, get(c, trs::kScale));
    const TransformState s = eval(*t.trs, c, parent);
    test_assert(difference(s.world, want) < 1e-12);
    test_assert(std::fabs(std::fabs(s.rotation.dot(parent.rotation * q)) - 1.0) < 1e-12);
    const double3 ls = s.localScale - get(c, trs::kScale);
    test_assert(ls.dot(ls) == 0.0);
  }
  /* Defaults are the identity, also under a parent. */
  for (const TransformType *type : {t.trs, t.fbx, t.matrix}) {
    Rand q(5);
    const TransformState p = chain(q, t, 2, 2.0, true);
    const TransformState s = eval(*type, type->defaults(), p);
    test_assert(difference(s.world, p.world) < 1e-14 * 100);
    test_assert(s.localScale[0] == 1.0 && s.localScale[1] == 1.0 &&
                s.localScale[2] == 1.0);
  }
  /* Quaternions without a direction. */
  ChannelBlock c = t.trs->defaults();
  c.set(trs::kRotationMode, trs::kQuaternionMode);
  for (double v : {0.0, std::numeric_limits<double>::quiet_NaN(), 1e-320}) {
    for (int i = 0; i < 4; i++) {
      c.set(trs::kRotationQuaternion + i, 0.0);
    }
    c.set(trs::kRotationQuaternion, v);
    const TransformState s = eval(*t.trs, c, TransformState());
    test_assert(s.finite() && (v == 1e-320 || s.rotation[3] == 1.0));
  }
  /* A mode that isn't one is the first. */
  put(c, trs::kRotationEuler, double3(0.1, 0.2, 0.3));
  for (double mode : {-1.0, 7.0, 1e9, std::numeric_limits<double>::quiet_NaN()}) {
    c.set(trs::kRotationMode, mode);
    ChannelBlock xyz = c;
    xyz.set(trs::kRotationMode, 0.0);
    test_assert(difference(eval(*t.trs, c, TransformState()).world,
                           eval(*t.trs, xyz, TransformState()).world) == 0.0);
  }
}

void testMatrix(const Types &t)
{
  Rand r(4);
  for (int i = 0; i < 500; i++) {
    const TransformState p = chain(r, t, 2, 3.0, true);
    Linear m;
    for (double3 &col : m.col) {
      col = r.vec(3);
    }
    const double3 at = r.vec(5);
    ChannelBlock c = t.matrix->defaults();
    for (int k = 0; k < 3; k++) {
      put(c, matrix_transform::kColumns + 3 * k, m.col[k]);
    }
    put(c, matrix_transform::kTranslation, at);
    test_assert(difference(eval(*t.matrix, c, p).world, p.world * solve::affine(m, at)) <
                1e-10);
  }
  ChannelBlock c = t.matrix->defaults();
  const quatd q = r.rotation();
  const Linear turned = Linear::of(q).scaled(double3(-2, 3, 4));
  for (int k = 0; k < 3; k++) {
    put(c, 3 * k, turned.col[k]);
  }
  const TransformState s = eval(*t.matrix, c, TransformState());
  test_assert(lengthOf(s.localScale - double3(-2, 3, 4)) < 1e-12);
  test_assert(std::fabs(std::fabs(s.rotation.dot(q)) - 1.0) < 1e-12);

  /* The state's rotation and scale are decomposeTrs's: at right angles, nearly, and not
   * at all; of ordinary, tiny and huge lengths. */
  for (int i = 0; i < 4000; i++) {
    const double lengths[] = {1.0, 1.0, 1e-90, 1e-161, 1e90, 1e160};
    Linear l = Linear::of(r.rotation()).scaled(r.vec(3) * lengths[r.below(6)]);
    if (i % 2) {
      const double shears[] = {0.0, 1e-16, 3e-15, 9e-15, 1.1e-14, 1e-12, 1e-6, 0.3};
      l.col[1] = l.col[1] + l.col[r.coin() ? 0 : 2] * shears[r.below(8)];
    }
    if (r.below(10) == 0) {
      l.col[r.below(3)] = double3(0, 0, 0);
    }
    for (int k = 0; k < 3; k++) {
      put(c, matrix_transform::kColumns + 3 * k, l.col[k]);
    }
    const TransformState got = eval(*t.matrix, c, TransformState());
    double3 at, scale;
    quatd rotation;
    litestl::math::decomposeTrs(solve::affine(l, double3(0, 0, 0)), at, rotation, scale);
    for (int k = 0; k < 3; k++) {
      test_assert(std::fabs(got.localScale[k] - scale[k]) <= 1e-13 * std::fabs(scale[k]));
    }
    test_assert(std::fabs(got.rotation.dot(rotation.normalized())) > 1.0 - 1e-13);
  }
}

/* Each rotation alone, about each axis: most of them are zero. */
void testFbxRotations(const Types &t)
{
  for (int group : {fbx::kRotation, fbx::kPreRotation, fbx::kPostRotation}) {
    for (int axis = 0; axis < 3; axis++) {
      ChannelBlock c = t.fbx->defaults();
      c.set(group + axis, 0.7);
      double3 unit(0, 0, 0);
      unit[axis] = 1.0;
      quatd want = quatd::fromAxisAngle(unit, 0.7);
      want = group == fbx::kPostRotation ? want.conjugated() : want;
      const TransformState s = eval(*t.fbx, c, TransformState());
      test_assert(std::fabs(s.rotation.dot(want)) > 1.0 - 1e-12);
      test_assert(difference(s.world, solve::affine(Linear::of(want), double3(0, 0, 0))) <
                  1e-12);
    }
  }
}

/* A long chain keeps a unit rotation, and any channel values are accepted. */
void testRobust(const Types &t)
{
  Rand r(6);
  TransformState p;
  for (int i = 0; i < 100000; i++) {
    ChannelBlock c = t.trs->defaults();
    put(c, trs::kRotationEuler, r.vec(0.1));
    p = eval(*t.trs, c, p);
  }
  test_assert(p.finite() && std::fabs(p.rotation.dot(p.rotation) - 1.0) < 1e-14);

  const double values[] = {0.0,
                           -0.0,
                           1.0,
                           1e-320,
                           1e308,
                           -1e200,
                           std::numeric_limits<double>::infinity(),
                           std::numeric_limits<double>::quiet_NaN()};
  int invalid = 0;
  for (const TransformType *type : {t.trs, t.fbx, t.matrix}) {
    for (int i = 0; i < 3000; i++) {
      ChannelBlock c = type->defaults();
      for (int n = int(r.below(6)); n >= 0; n--) {
        c.set(int(r.below(uint32_t(c.size()))), values[r.below(8)]);
      }
      const TransformState parent = eval(*type, c, TransformState());
      const TransformState s = eval(*type, c, parent);
      invalid += !s.finite();
      TransformDelta delta;
      delta.kind = DeltaKind(r.below(3));
      delta.vector = r.vec(2);
      delta.axis = r.vec(1);
      delta.angle = 1.0;
      const Result<ChannelBlock> moved =
          solveDelta(*type, EvalContext(c), c, delta, parent);
      const Result<ChannelBlock> solved = solveWorld(
          *type, EvalContext(c), TransformState().world, parent, SetWorldMode::Closest);
      for (const Result<ChannelBlock> *res : {&moved, &solved}) {
        test_assert(!res->ok() || eval(*type, **res, parent).finite());
      }
    }
  }
  test_assert(invalid > 500);
}

/* ---- the registry ---- */

struct Offset {
  TransformState evaluate(const EvalContext &ctx, const TransformState &parent) const
  {
    TransformState s = parent;
    s.world[3][0] += ctx.channel(0);
    return s;
  }
  Status setWorld(const EvalContext &,
                  const mat4d &,
                  const TransformState &,
                  SetWorldMode,
                  ChannelBlock &) const
  {
    return Status();
  }
  static lbt::Struct<Offset> *defineBindings()
  {
    static lbt::Struct<Offset> *st = nullptr;
    if (!st) {
      litestl::alloc::PermanentGuard guard;
      st = new lbt::Struct<Offset>("test::Offset", sizeof(Offset));
      BIND_STRUCT_METHOD(st, evaluate, MARGS("ctx", "parent"));
      BIND_STRUCT_METHOD(
          st, setWorld, MARGS("current", "target", "parent", "mode", "out"));
    }
    return st;
  }
};

Owned<TransformModel> offsetModel(Runtime &rt)
{
  if (!rt.ifaces().cachedImplDesc(TypeId::of("test.offset#impl"))) {
    const Status s = registerImpl<TransformModel, Offset>(rt, "test.offset#impl");
    test_assert(s.ok());
  }
  auto m =
      own<TransformModel>(rt, "test.offset#impl", litestl::alloc::New<Offset>("test"));
  return m.ok() ? std::move(*m) : Owned<TransformModel>();
}

TransformTypeInfo info(const char *name, std::initializer_list<PropertyDef> defs)
{
  TransformTypeInfo i;
  i.name = name;
  for (const PropertyDef &d : defs) {
    i.channels.append(d);
  }
  return i;
}

void testRegistry()
{
  Types t;
  TransformRegistry &reg = t.rt.ext<TransformRegistry>();
  ApiRegistry &api = t.rt.ext<ApiRegistry>();
  test_assert(registerTransformTypes(t.rt).ok() && reg.count() == 3);
  test_assert(t.trs->channelCount() == trs::kCount &&
              t.fbx->channelCount() == fbx::kCount);
  test_assert(t.matrix->channelCount() == matrix_transform::kCount);
  test_assert(reg.find("nope") == nullptr && reg.find(t.trs->id()) == t.trs);
  test_assert(&reg.at(1) == t.fbx && strcmp(t.trs->name().c_str(), kTrsTransform) == 0);
  test_assert(
      api.isA(t.trs->structId(), StructId::of(TransformRegistry::kChannelsStruct)));

  /* Distance channels. */
  for (int i = -1; i <= fbx::kCount; i++) {
    const bool want =
        (i >= 0 && i < 3) || (i >= fbx::kRotationOffset && i < fbx::kScaling);
    test_assert(t.fbx->isDistance(i) == want);
  }
  for (int i = 0; i < trs::kCount; i++) {
    test_assert(t.trs->isDistance(i) == (i < 3));
  }
  for (int i = 0; i < matrix_transform::kCount; i++) {
    test_assert(t.matrix->isDistance(i) == (i >= 9));
  }

  /* Every kind of channel, in order. */
  const double two53 = 9007199254740992.0;
  Result<const TransformType *> added = reg.add(
      info("test.kinds",
           {PropertyDef::floatProp("f").defaultValue(PropValue::ofFloat(1.5)),
            PropertyDef::boolProp("b").defaultValue(PropValue::ofBool(true)),
            PropertyDef::intProp("i").defaultValue(PropValue::ofInt(-7)),
            PropertyDef::enumProp("e").item("A", 3).item("B", -5).defaultValue(
                PropValue::ofEnum(-5)),
            PropertyDef::vecProp("v2", 2).defaultValue(PropValue::ofVec2(1, 2)),
            PropertyDef::vecProp("v4", 4).defaultValue(PropValue::ofVec4(1, 2, 3, 4)),
            PropertyDef::quatProp("q").defaultValue(PropValue::ofQuat(0, 0, 0, 1))}),
      offsetModel(t.rt));
  test_assert(added.ok());
  const TransformType &k = **added;
  const double defaults[] = {1.5, 1, -7, -5, 1, 2, 1, 2, 3, 4, 0, 0, 0, 1};
  test_assert(k.channelCount() == 14);
  for (int i = 0; i < 14; i++) {
    test_assert(k.defaults().get(i) == defaults[i]);
  }

  Context ctx(&t.rt, nullptr, PointerRef());
  ChannelBlock block = k.defaults();
  PointerRef self;
  self.data = &block;
  self.type = k.structId();
  const auto member = [&](const char *name) {
    return &api.findMember(k.structId(), name)->value();
  };
  test_assert(member("i")->get(self, ctx) == PropValue::ofInt(-7));
  test_assert(member("e")->get(self, ctx) == PropValue::ofEnum(-5));
  test_assert(member("b")->get(self, ctx) == PropValue::ofBool(true));
  test_assert(member("v4")->get(self, ctx) == PropValue::ofVec4(1, 2, 3, 4));
  test_assert(member("q")->get(self, ctx) == PropValue::ofQuat(0, 0, 0, 1));
  block.set(3, 4.0); /* not an item */
  block.set(1, 0.4);
  block.set(2, 2.6);
  test_assert(member("e")->get(self, ctx) == PropValue::ofEnum(3));
  test_assert(member("b")->get(self, ctx) == PropValue::ofBool(false));
  test_assert(member("i")->get(self, ctx) == PropValue::ofInt(3));
  test_assert(member("v2")->set(self, ctx, PropValue::ofVec2(8, 9)).ok());
  test_assert(block.get(4) == 8 && block.get(5) == 9 && block.get(6) == 1);
  test_assert(member("f")->set(self, ctx, PropValue::ofFloat(0.25)).ok() &&
              block.get(0) == 0.25);
  test_assert(member("b")->set(self, ctx, PropValue::ofBool(true)).ok() &&
              block.get(1) == 1.0);
  test_assert(member("e")->set(self, ctx, PropValue::ofEnum(-5)).ok() &&
              block.get(3) == -5.0);
  test_assert(member("q")->set(self, ctx, PropValue::ofQuat(1, 2, 3, 4)).ok());
  test_assert(block.get(13) == 4);
  for (int64_t big : {int64_t(two53), -int64_t(two53)}) {
    test_assert(member("i")->set(self, ctx, PropValue::ofInt(big)).ok());
    test_assert(member("i")->get(self, ctx) == PropValue::ofInt(big));
    const Status over =
        member("i")->set(self, ctx, PropValue::ofInt(big + (big < 0 ? -1 : 1)));
    test_assert(over.code() == StatusCode::OutOfRange);
    test_assert(member("i")->get(self, ctx) == PropValue::ofInt(big));
  }
  block.resize(13);
  test_assert(member("q")->set(self, ctx, PropValue::ofQuat(0, 0, 0, 1)).code() ==
              StatusCode::FailedPrecondition);
  test_assert(block.get(10) == 1.0);

  /* Refused, with nothing registered. */
  const int count = reg.count();
  const auto refused = [&](TransformTypeInfo i, StatusCode code) {
    const std::string name = i.name.c_str();
    const Status s = reg.add(std::move(i), offsetModel(t.rt)).status();
    return s.code() == code && reg.count() == count &&
           (code == StatusCode::AlreadyExists || !api.find(StructId::of(name)));
  };
  const PropertyDef f = PropertyDef::floatProp("f");
  test_assert(refused(info("test.kinds", {f}), StatusCode::AlreadyExists));
  test_assert(refused(info(kTrsTransform, {f}), StatusCode::AlreadyExists));
  test_assert(refused(info("test.s", {PropertyDef::stringProp("s")}),
                      StatusCode::InvalidArgument));
  test_assert(
      refused(info("test.m", {PropertyDef::mat4Prop("m")}), StatusCode::InvalidArgument));
  test_assert(refused(info("test.fl", {PropertyDef::flagsProp("m").item("A", 1)}),
                      StatusCode::InvalidArgument));
  test_assert(refused(info("test.i",
                           {PropertyDef::intProp("i").defaultValue(
                               PropValue::ofInt(int64_t(two53) + 1))}),
                      StatusCode::InvalidArgument));
  test_assert(refused(
      info("test.e",
           {PropertyDef::enumProp("e").item("A", 0).item("B", int64_t(two53) + 1)}),
      StatusCode::InvalidArgument));
  test_assert(refused(
      info("test.dyn",
           {PropertyDef::floatProp("f").dynamic([](PropertyDef &, const void *) {})}),
      StatusCode::InvalidArgument));
  test_assert(refused(info("test.twice", {f, f}), StatusCode::InvalidArgument));
  test_assert(refused(info("test.name", {PropertyDef::floatProp("a b")}),
                      StatusCode::InvalidArgument));
  test_assert(reg.add(info("test.none", {f}), Owned<TransformModel>()).status().code() ==
              StatusCode::InvalidArgument);
  /* A refused name can be registered later. */
  test_assert(reg.add(info("test.twice", {f}), offsetModel(t.rt)).ok());
  test_assert(t.rt.validateAll().ok());
}

/* ---- scripts ---- */

void testForeign()
{
  test_assert(sc::foreignImplementable(detail::ifaceDesc<TransformModel>()).ok());
  test_assert(ChannelBlock::defineBindings() == ChannelBlock::defineBindings());
  test_assert(EvalContext::defineBindings() && TransformDelta::defineBindings());

  fakehost::Host host;
  {
    Types t;
    sc::ForeignBindings &fb = t.rt.ext<sc::ForeignBindings>();
    const char *slots[] = {"evaluate", "setWorld"};
    test_assert(
        registerForeign<TransformModel>(t.rt, "js.model", fb.dispatcher(), nullptr, slots)
            .ok());
    std::string fail;
    int32_t mode = -1, setCalls = 0;
    double startedAt = 0.0;
    bool wrongSize = false, notFinite = false, refuse = false;
    double at = 0.0;
    fakehost::Obj obj;
    obj.methods["evaluate"] = [&](void **args, void *ret, litestl::util::string *err) {
      if (fail == "evaluate") {
        *err = "evaluate: Error: boom";
        return false;
      }
      const EvalContext &ctx = *static_cast<const EvalContext *>(args[0]);
      TransformState *s =
          new (ret) TransformState(*static_cast<TransformState *>(args[1]));
      s->world[3][0] += ctx.channel(0);
      return true;
    };
    obj.methods["setWorld"] = [&](void **args, void *ret, litestl::util::string *err) {
      setCalls++;
      if (fail == "setWorld") {
        *err = "setWorld: Error: boom";
        return false;
      }
      mode = *static_cast<int32_t *>(args[3]);
      startedAt = static_cast<const EvalContext *>(args[0])->channel(0);
      ChannelBlock &out = *static_cast<ChannelBlock *>(args[4]);
      out.set(0, notFinite ? std::numeric_limits<double>::infinity() : at);
      if (wrongSize) {
        out.resize(2);
      }
      new (ret) Status(refuse ? Status::error(StatusCode::Unsupported, "no") : Status());
      return true;
    };
    const fakehost::lf::Handle h = host.add(std::move(obj));
    const TransformModel js = query<TransformModel>(
        t.rt, TypeId::of("js.model"), reinterpret_cast<void *>(uintptr_t(h)));
    Result<const TransformType *> added = t.rt.ext<TransformRegistry>().add(
        info("test.js", {PropertyDef::floatProp("x").subtype(PropSubtype::Distance)}),
        Owned<TransformModel>(js, nullptr));
    test_assert(added.ok() && !js.hasApplyDelta());
    const TransformType &type = **added;
    ChannelBlock c = type.defaults();
    c.set(0, 2.0);
    const TransformState root;
    test_assert(eval(type, c, root).world[3][0] == 2.0);

    const mat4d target = solve::affine(Linear::identity(), double3(7, 0, 0));
    const auto solved = [&](SetWorldMode m) {
      return solveWorld(type, EvalContext(c), target, root, m);
    };
    at = 7.0;
    Result<ChannelBlock> r = solved(SetWorldMode::Closest);
    test_assert(r.ok() && r->get(0) == 7.0 && mode == 1 && setCalls == 1);
    test_assert(solved(SetWorldMode::Exact).ok() && mode == 0 && setCalls == 2);
    at = 7.001;
    test_assert(solved(SetWorldMode::Closest).status().code() ==
                StatusCode::FailedPrecondition);
    at = 7.0;
    const auto failsWith = [&](bool &flag, StatusCode code) {
      flag = true;
      const Status s = solved(SetWorldMode::Closest).status();
      flag = false;
      return s.code() == code && !t.rt.hasStickyError();
    };
    test_assert(failsWith(wrongSize, StatusCode::FailedPrecondition));
    test_assert(failsWith(notFinite, StatusCode::FailedPrecondition));
    test_assert(failsWith(refuse, StatusCode::Unsupported));
    /* The fallback for models without applyDelta. */
    TransformDelta delta;
    delta.vector = double3(5, 0, 0);
    ChannelBlock nearTo = c;
    nearTo.set(0, 6.5);
    r = solveDelta(type, EvalContext(c), nearTo, delta, root);
    test_assert(r.ok() && r->get(0) == 7.0 && startedAt == 6.5);

    /* Models aren't asked about parents without an inverse. */
    TransformState flat;
    flat.world[1][1] = 0.0;
    const int32_t before = setCalls;
    test_assert(solveWorld(type, EvalContext(c), target, flat, SetWorldMode::Closest)
                    .status()
                    .code() == StatusCode::FailedPrecondition);
    test_assert(setCalls == before);

    for (const char *slot : {"evaluate", "setWorld"}) {
      fail = slot;
      const Status s = solved(SetWorldMode::Closest).status();
      test_assert(s.code() == StatusCode::Internal && strstr(s.message(), "boom"));
      test_assert(!t.rt.hasStickyError());
    }
    fail = "evaluate";
    test_assert(
        strstr(evaluateTransform(type, EvalContext(c), root).status().message(), "boom"));
    test_assert(strstr(
        solveDelta(type, EvalContext(c), c, delta, root).status().message(), "boom"));
    test_assert(!t.rt.hasStickyError());
    fail.clear();

    t.rt.setStickyError(Status::error(StatusCode::Internal, "earlier"));
    for (const TransformType *any : {&type, t.trs}) {
      const ChannelBlock &d = any->defaults();
      const StatusCode pending = StatusCode::FailedPrecondition;
      test_assert(evaluateTransform(*any, EvalContext(d), root).status().code() ==
                  pending);
      test_assert(solveDelta(*any, EvalContext(d), d, delta, root).status().code() ==
                  pending);
      test_assert(solveWorld(*any, EvalContext(d), target, root, SetWorldMode::Exact)
                      .status()
                      .code() == pending);
    }
    test_assert(strcmp(t.rt.takeStickyError().message(), "earlier") == 0);
    fakehost::lf::release(h);
  }
  test_assert(host.live() == 0 && host.allReleasedOnce());
}

} // namespace

int main()
{
  {
    Types t;
    testChannels();
    testFbxGolden(t);
    testTrs(t);
    testFbxRotations(t);
    testMatrix(t);
    testRobust(t);
  }
  testRegistry();
  testForeign();
  return test_end();
}
