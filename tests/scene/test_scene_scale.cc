/* Scale deltas: mirrors, factors along other axes than the object's, and parents that are
 * nearly flat. What is expected comes from 3 by 3 matrices made here. */
#include "command_doc.h"

#include <cmath>

test_init;

using namespace scene_test;

namespace {

/* ---- independent maths ---- */

struct M3 {
  double a[3][3] = {}; /* row, column */
};

M3 mul(const M3 &x, const M3 &y)
{
  M3 r;
  for (int i = 0; i < 3; i++) {
    for (int j = 0; j < 3; j++) {
      for (int k = 0; k < 3; k++) {
        r.a[i][j] += x.a[i][k] * y.a[k][j];
      }
    }
  }
  return r;
}

M3 transposed(const M3 &x)
{
  M3 r;
  for (int i = 0; i < 9; i++) {
    r.a[i % 3][i / 3] = x.a[i / 3][i % 3];
  }
  return r;
}

M3 diagonal(const double3 &d)
{
  M3 r;
  for (int i = 0; i < 3; i++) {
    r.a[i][i] = d[i];
  }
  return r;
}

double det(const M3 &m)
{
  double d = 0.0;
  for (int i = 0; i < 3; i++) {
    const int j = (i + 1) % 3, k = (i + 2) % 3;
    d += m.a[0][i] * (m.a[1][j] * m.a[2][k] - m.a[1][k] * m.a[2][j]);
  }
  return d;
}

M3 inverse(const M3 &m)
{
  M3 r;
  const double d = det(m);
  for (int i = 0; i < 3; i++) {
    for (int j = 0; j < 3; j++) {
      const int i1 = (i + 1) % 3, i2 = (i + 2) % 3, j1 = (j + 1) % 3, j2 = (j + 2) % 3;
      r.a[j][i] = (m.a[i1][j1] * m.a[i2][j2] - m.a[i1][j2] * m.a[i2][j1]) / d;
    }
  }
  return r;
}

M3 linearOf(const mat4d &m)
{
  M3 r;
  const double *d = m;
  for (int i = 0; i < 9; i++) {
    r.a[i % 3][i / 3] = d[(i / 3) * 4 + i % 3];
  }
  return r;
}

/* From a unit quaternion's components. */
M3 rotationOf(const quatd &q)
{
  const double x = q[0], y = q[1], z = q[2], w = q[3];
  const double rows[9] = {1 - 2 * (y * y + z * z),
                          2 * (x * y - z * w),
                          2 * (x * z + y * w),
                          2 * (x * y + z * w),
                          1 - 2 * (x * x + z * z),
                          2 * (y * z - x * w),
                          2 * (x * z - y * w),
                          2 * (y * z + x * w),
                          1 - 2 * (x * x + y * y)};
  M3 r;
  for (int i = 0; i < 9; i++) {
    r.a[i / 3][i % 3] = rows[i];
  }
  return r;
}

double norm(const M3 &m)
{
  double s = 0.0;
  for (const auto &row : m.a) {
    for (double v : row) {
      s += v * v;
    }
  }
  return std::sqrt(s);
}

double distance(const M3 &x, const M3 &y)
{
  M3 d;
  for (int i = 0; i < 9; i++) {
    d.a[i / 3][i % 3] = x.a[i / 3][i % 3] - y.a[i / 3][i % 3];
  }
  return norm(d);
}

/* The eigenvectors of a symmetric matrix, as columns (Jacobi). */
M3 eigenvectors(M3 s)
{
  M3 v = diagonal(double3(1, 1, 1));
  for (int n = 0; n < 60; n++) {
    for (int p = 0; p < 2; p++) {
      for (int q = p + 1; q < 3; q++) {
        const double angle = 0.5 * std::atan2(2.0 * s.a[p][q], s.a[q][q] - s.a[p][p]);
        M3 r = diagonal(double3(1, 1, 1));
        r.a[p][p] = r.a[q][q] = std::cos(angle);
        r.a[p][q] = std::sin(angle);
        r.a[q][p] = -r.a[p][q];
        s = mul(transposed(r), mul(s, r));
        v = mul(v, r);
      }
    }
  }
  return v;
}

/* ---- the fixture ---- */

struct Case {
  const TransformType *type;
  int scale; /* its first scale channel */
  ChannelBlock base;
};

/* A TRS object for 0, and FBX ones with each inherit type. */
Case caseOf(Rand &r, const Types &t, int type, const double3 &scale)
{
  return type == 0 ? Case{t.trs, int(trs::kScale), trsChannels(r, *t.trs, scale, 5.0)}
                   : Case{t.fbx,
                          int(fbx::kScaling),
                          fbxChannels(r, *t.fbx, type - 1, scale, 2.0)};
}

TransformDelta scaleDelta(const quatd &basis, const double3 &vector)
{
  TransformDelta d;
  d.kind = DeltaKind::Scale;
  d.basis = basis;
  d.vector = vector;
  return d;
}

/* The world's linear part after the delta. */
M3 wanted(const TransformDelta &d, const M3 &world)
{
  const M3 b = rotationOf(d.basis.normalized());
  return mul(mul(b, mul(diagonal(d.vector), transposed(b))), world);
}

double3 signs(int bits)
{
  return double3(bits & 1 ? -1 : 1, bits & 2 ? -1 : 1, bits & 4 ? -1 : 1);
}

/* The whole matrix, translation and all. */
void expectExact(const Case &c,
                 const Result<ChannelBlock> &out,
                 const TransformDelta &d,
                 const TransformState &parent,
                 double limit)
{
  test_assert(out.ok());
  if (!out.ok()) {
    return;
  }
  const TransformState from = eval(*c.type, c.base, parent),
                       to = eval(*c.type, *out, parent);
  const M3 want = wanted(d, linearOf(from.world));
  test_assert(distance(linearOf(to.world), want) <= limit * norm(want));
  const double3 arm = solve::translationOf(from.world) - d.pivot;
  const M3 b = rotationOf(d.basis.normalized());
  const M3 m = mul(b, mul(diagonal(d.vector), transposed(b)));
  double3 place = d.pivot;
  for (int i = 0; i < 3; i++) {
    for (int k = 0; k < 3; k++) {
      place[i] += m.a[i][k] * arm[k];
    }
  }
  test_assert(lengthOf(solve::translationOf(to.world) - place) <=
              1e-11 * (50.0 + lengthOf(place)));
}

/* ---- mirrors ---- */

/* Along any axes, wherever no parent has shear. */
void testMirrors(const Types &t)
{
  Rand r(31);
  for (int i = 0; i < 4000 * coredcc_test::testScale(); i++) {
    const double size = std::exp(r.range(-2, 2));
    TransformState p;
    if (i % 4) { /* rotated, mirrored and scaled uniformly */
      ChannelBlock pc =
          trsChannels(r, *t.trs, double3(size, size, size) * signs(int(r.below(8))), 5.0);
      p = eval(*t.trs, pc, chain(r, t, i % 2, 1.0, true));
    }
    Case c = caseOf(r, t, int(r.below(4)), r.scale(3.0, true));
    /* Rrs divides by the parent's scale, and RrSs and Rrs go before it: all the same
     * here. */
    TransformDelta d =
        scaleDelta(r.rotation(), signs(int(r.below(8))) * r.range(0.2, 4.0));
    d.pivot = r.vec(10.0);
    const Result<ChannelBlock> out = solveDelta(*c.type, EvalCtx(c.base), c.base, d, p);
    expectExact(c, out, d, p, 1e-12);
    if (out.ok()) {
      /* The sizes of the scales are the base's, times the factor's. */
      const double k = std::fabs(d.vector[0]);
      for (int a = 0; a < 3; a++) {
        const double was = std::fabs(c.base.get(c.scale + a)),
                     is = std::fabs(out->get(c.scale + a));
        test_assert(std::fabs(is - was * k) <= 1e-12 * is);
      }
      /* Twice is nothing. */
      TransformDelta back = d;
      back.vector =
          signs(int((d.vector[0] < 0) + 2 * (d.vector[1] < 0) + 4 * (d.vector[2] < 0))) /
          k;
      const Result<ChannelBlock> again =
          solveDelta(*c.type, EvalCtx(*out), *out, back, p);
      test_assert(again.ok() && difference(eval(*c.type, *again, p).world,
                                           eval(*c.type, c.base, p).world) < 1e-10);
    }
  }
  /* A pure mirror leaves them, but for rounding. */
  for (int i = 0; i < 200; i++) {
    Case c = caseOf(r, t, 0, r.scale(3.0, true));
    const TransformDelta d = scaleDelta(r.rotation(), signs(int(r.below(8))));
    const ChannelBlock out =
        *solveDelta(*c.type, EvalCtx(c.base), c.base, d, TransformState());
    for (int a = 0; a < 3; a++) {
      const double was = std::fabs(c.base.get(c.scale + a)),
                   is = std::fabs(out.get(c.scale + a));
      test_assert(std::fabs(is - was) <= 4e-15 * was);
    }
  }
}

/* Along its own axes the rotation stays, whatever the signs. */
void testOwnAxes(const Types &t)
{
  Rand r(32);
  for (int i = 0; i < 2000; i++) {
    const TransformState p = chain(r, t, int(r.below(3)), 1.0, false);
    Case c = caseOf(r, t, int(r.below(4)), r.scale(3.0, true));
    const double3 factors = r.scale(3.0, true);
    const quatd own = eval(*c.type, c.base, p).rotation;
    const TransformDelta d = scaleDelta(own, factors);
    const Result<ChannelBlock> out = solveDelta(*c.type, EvalCtx(c.base), c.base, d, p);
    expectExact(c, out, d, p, 1e-11);
    for (int a = 0; out.ok() && a < 3; a++) {
      const double want = c.base.get(c.scale + a) * factors[a];
      test_assert(std::fabs(out->get(c.scale + a) - want) <= 1e-11 * std::fabs(want));
    }
    test_assert(out.ok() &&
                std::fabs(eval(*c.type, *out, p).rotation.dot(own)) > 1.0 - 1e-12);
  }
}

/* A drag across 0 and on: exact at the mirror, and the angles don't jump after it. */
void testDrag(const Types &t)
{
  for (double degrees : {10.0, 30.0, 45.0, 60.0}) {
    Case c{t.trs, int(trs::kScale), t.trs->defaults()};
    c.base.set(trs::kRotationEuler + 2, degrees * 3.14159265358979323846 / 180.0);
    ChannelBlock last = c.base;
    for (int step = 40; step >= -120; step--) {
      const double f = step / 40.0;
      const TransformDelta d = scaleDelta(quatd(), double3(f, 1, 1));
      const Result<ChannelBlock> out =
          solveDelta(*t.trs, EvalCtx(c.base), last, d, TransformState());
      test_assert(out.ok());
      if (step == -40) {
        expectExact(c, out, d, TransformState(), 1e-15);
      }
      for (int a = 0; a < 3; a++) {
        test_assert(step <= -1 || out->get(trs::kRotationEuler + a) ==
                                      c.base.get(trs::kRotationEuler + a));
        test_assert(step > 0 || step < -80 ||
                    std::fabs(out->get(trs::kScale + a)) <= 2.0);
        test_assert(step >= -1 || std::fabs(out->get(trs::kRotationEuler + a) -
                                            last.get(trs::kRotationEuler + a)) < 1e-9);
      }
      int negative = 0;
      for (int a = 0; a < 3; a++) {
        negative += out->get(trs::kScale + a) < 0.0;
      }
      test_assert(negative == (step < 0 ? 1 : 0));
      const double volume = out->get(trs::kScale) * out->get(trs::kScale + 1);
      test_assert(step == 0 || (volume < 0.0) == (f < 0.0));
      last = *out;
    }
  }
}

/* ---- what can't be ---- */

/* Factors along other axes: the sizes stay between the delta's, and the mirror is right.
 */
void testOtherAxes(const Types &t)
{
  Rand r(33);
  int exact = 0;
  for (int i = 0; i < 4000; i++) {
    const bool bent = i % 2 == 1;
    const TransformState p =
        chain(r, t, bent ? 2 : int(r.below(3)), bent ? 2.0 : 1.0, true);
    Case c = caseOf(r, t, int(r.below(4)), r.scale(2.0, true));
    TransformDelta d = scaleDelta(r.rotation(), r.scale(3.0, true));
    d.pivot = r.vec(5.0);
    const Result<ChannelBlock> out = solveDelta(*c.type, EvalCtx(c.base), c.base, d, p);
    test_assert(out.ok());
    if (!out.ok()) {
      continue;
    }
    const M3 from = linearOf(eval(*c.type, c.base, p).world), want = wanted(d, from);
    const M3 to = linearOf(eval(*c.type, *out, p).world);
    exact += distance(to, want) <= 1e-9 * norm(want);
    test_assert((det(to) < 0.0) == (det(want) < 0.0));
    for (int a = 0; !bent && a < 3; a++) {
      const double k = std::fabs(out->get(c.scale + a) / c.base.get(c.scale + a));
      test_assert(k > 0.3333 && k < 3.0001);
    }
  }
  test_assert(exact < 100);
}

/* FBX nodes without pivots are TRS ones, however far from exact the answer is. */
void testSameAnswer(const Types &t)
{
  Rand r(37);
  for (int i = 0; i < 1000; i++) {
    const int inherit = int(r.below(3));
    const TransformState p =
        inherit == fbx::kRSrs ? chain(r, t, 1, 2.0, true) : TransformState();
    Case a{t.trs, int(trs::kScale), t.trs->defaults()},
        b{t.fbx, int(fbx::kScaling), t.fbx->defaults()};
    const double3 euler = r.vec(3), scale = r.scale(2.0, true);
    put(a.base, trs::kRotationEuler, euler);
    put(b.base, fbx::kRotation, euler);
    put(a.base, trs::kScale, scale);
    put(b.base, fbx::kScaling, scale);
    b.base.set(fbx::kInheritType, inherit);
    const TransformDelta d = scaleDelta(r.rotation(), r.scale(3.0, true));
    const Result<ChannelBlock> x = solveDelta(*a.type, EvalCtx(a.base), a.base, d, p),
                               y = solveDelta(*b.type, EvalCtx(b.base), b.base, d, p);
    test_assert(x.ok() && y.ok() &&
                difference(eval(*a.type, *x, p).world, eval(*b.type, *y, p).world) <
                    1e-9);
  }
}

/* Two mirrors are equally near: the first, also with rounding on either side of it. */
void testTie(const Types &t)
{
  const double half = 0.785398163397448309616;
  double want = 0.0, solvedAngle = 0.0;
  for (int i = -20; i <= 20; i++) {
    const double turn = (half + i * 1e-14) / 2;
    const TransformDelta d =
        scaleDelta(quatd(0, 0, std::sin(turn), std::cos(turn)), double3(-1, 1, 1));
    const ChannelBlock base = t.trs->defaults();
    const Result<ChannelBlock> out =
        solveDelta(*t.trs, EvalCtx(base), base, d, TransformState());
    must(out.status());
    const double angle = out->get(trs::kRotationEuler + 2);
    test_assert(std::fabs(std::fabs(angle) - 2 * half) < 1e-9);
    want = i == -20 ? angle : want;
    test_assert(std::fabs(angle - want) < 1e-9);
    const Result<ChannelBlock> solved = solveWorld(
        *t.trs, EvalCtx(base), d.matrix(), TransformState(), SetWorldMode::Exact);
    must(solved.status());
    solvedAngle = i == -20 ? solved->get(trs::kRotationEuler + 2) : solvedAngle;
    test_assert(std::fabs(solved->get(trs::kRotationEuler + 2) - solvedAngle) < 1e-9);
  }
  test_assert(want > 0.0 && std::fabs(std::fabs(solvedAngle) - 2 * half) < 1e-9);
}

/* A model that scales its one axis, and answers setWorld as it is told to. */
struct OneAxis {
  Status answer;
  bool throws = false;
  Runtime *rt = nullptr;

  TransformState evaluate(const EvalCtx &c, const TransformState &p) const
  {
    TransformState s = p;
    s.world[0][0] *= c.channel(0);
    return s;
  }
  Status applyDelta(const EvalCtx &base,
                    const ChannelBlock &,
                    const TransformDelta &d,
                    const TransformState &,
                    ChannelBlock &out) const
  {
    out.set(0, base.channel(0) * d.vector[0]);
    return Status();
  }
  Status setWorld(const EvalCtx &,
                  const mat4d &,
                  const TransformState &,
                  SetWorldMode,
                  ChannelBlock &) const
  {
    if (throws) {
      rt->setStickyError(Status::error(StatusCode::Internal, "threw"));
      return Status();
    }
    return answer;
  }
  static lbt::Struct<OneAxis> *defineBindings()
  {
    auto *st = new lbt::Struct<OneAxis>("test::OneAxis", sizeof(OneAxis));
    BIND_STRUCT_METHOD(st, evaluate, MARGS("ctx", "parent"));
    BIND_STRUCT_METHOD(st, applyDelta, MARGS("base", "nearTo", "delta", "parent", "out"));
    BIND_STRUCT_METHOD(st, setWorld, MARGS("current", "target", "parent", "mode", "out"));
    return st;
  }
};

/* Only an exception of the second attempt is an error. */
void testSecondAttempt()
{
  CommandDoc d;
  const char *name = "test.oneAxis";
  must(registerImpl<TransformModel, OneAxis>(d.rt, name));
  OneAxis *m = litestl::alloc::New<OneAxis>("OneAxis");
  m->rt = &d.rt;
  Result<Owned<TransformModel>> owned = own<TransformModel>(d.rt, name, m);
  must(owned.status());
  TransformTypeInfo info;
  info.name = name;
  info.channels.append(PropertyDef::floatProp("x"));
  const Result<const TransformType *> type =
      d.rt.ext<TransformRegistry>().add(std::move(info), std::move(owned).value());
  must(type.status());
  ChannelBlock base(1);
  base.set(0, 3.0);
  const auto solve = [&] {
    return solveDelta(
        **type, EvalCtx(base), base, scaleDelta(quatd(), double3(2, 2, 1)), {});
  };
  for (StatusCode code : {StatusCode::FailedPrecondition, StatusCode::Unsupported}) {
    m->answer = Status::error(code, "no");
    const Result<ChannelBlock> r = solve();
    test_assert(r.ok() && r->get(0) == 6.0);
  }
  m->throws = true;
  test_assert(isError(solve().status(), StatusCode::Internal, "threw") &&
              !d.rt.hasStickyError());
}

/* Matrices hold anything. */
void testMatrix(const Types &t)
{
  Rand r(34);
  for (int i = 0; i < 500; i++) {
    const TransformState p = chain(r, t, 2, 3.0, true);
    Case c{t.matrix, 0, t.matrix->defaults()};
    for (int k = 0; k < 12; k++) {
      c.base.set(k, r.range(-3, 3));
    }
    TransformDelta d = scaleDelta(r.rotation(), r.scale(3.0, true));
    d.pivot = r.vec(5.0);
    expectExact(c, solveDelta(*c.type, EvalCtx(c.base), c.base, d, p), d, p, 1e-9);
  }
}

/* Under a sheared parent a scale can be exact with another rotation: where the node's
 * axes are the right singular vectors of what the delta is in the parent's space, and its
 * new ones the left. */
void testExactByRotation(const Types &t)
{
  Rand r(35);
  for (int i = 0; i < 500; i++) {
    const TransformState p = chain(r, t, 2, 2.0, false);
    TransformDelta d = scaleDelta(r.rotation(), r.scale(3.0, false));
    const M3 pm = linearOf(p.world);
    const M3 m = mul(inverse(pm), wanted(d, pm));
    M3 v = eigenvectors(mul(transposed(m), m));
    if (det(v) < 0.0) {
      for (auto &row : v.a) {
        row[0] = -row[0];
      }
    }
    const double3 s = r.scale(3.0, true);
    const M3 local = mul(v, diagonal(s));
    mat4d world;
    world.identity();
    double *w = world;
    const M3 target = mul(pm, local);
    for (int k = 0; k < 9; k++) {
      w[(k % 3) * 4 + k / 3] = target.a[k / 3][k % 3];
    }
    Case c{t.trs, int(trs::kScale), t.trs->defaults()};
    Result<ChannelBlock> base =
        solveWorld(*t.trs, EvalCtx(c.base), world, p, SetWorldMode::Exact);
    test_assert(base.ok());
    if (!base.ok()) {
      continue;
    }
    c.base = *base;
    d.pivot = solve::translationOf(world);
    expectExact(c, solveDelta(*c.type, EvalCtx(c.base), c.base, d, p), d, p, 1e-8);
  }
}

/* ---- parents ---- */

void testParents(const Types &t)
{
  Rand r(36);
  const auto refused = [&](const double3 &scale) {
    ChannelBlock pc = trsChannels(r, *t.trs, scale, 1.0);
    const TransformState p = eval(*t.trs, pc, TransformState());
    int bad = 0;
    for (const TransformType *type : {t.trs, t.fbx, t.matrix}) {
      const ChannelBlock c = type->defaults();
      const mat4d world = eval(*type, c, TransformState()).world;
      const Result<ChannelBlock> a =
          solveWorld(*type, EvalCtx(c), world, p, SetWorldMode::Closest);
      TransformDelta d;
      d.vector = double3(1, 0, 0);
      const Result<ChannelBlock> b = solveDelta(*type, EvalCtx(c), c, d, p);
      test_assert(a.ok() || a.status().code() == StatusCode::FailedPrecondition);
      test_assert(a.ok() || strstr(a.status().message(), "nearly 0"));
      test_assert(b.ok() || b.status().code() == StatusCode::FailedPrecondition);
      bad += !a.ok() + !b.ok();
    }
    test_assert(bad == 0 || bad == 6);
    return bad == 6;
  };
  for (double s : {1e-11, 1e-3, 1.0, 1e6, 1e20}) {
    test_assert(!refused(double3(s, s, s)) && !refused(double3(s, -s, s)));
  }
  test_assert(!refused(double3(1e-7, 1, 1)) && !refused(double3(1, 1e-11, 1)));
  test_assert(refused(double3(1e-13, 1, 1)) && refused(double3(2.2e-16, -2.2e-16, 1)));
  test_assert(refused(double3(1, 1, 1e14)) && refused(double3(1, 0, 1)));
  test_assert(refused(double3(1e-300, 1, 1)));
}

/* ---- by command ---- */

void testCommand()
{
  CommandDoc d;
  const BlockHandle a = d.object("a", 2.0), c = d.object("c", 1.0);
  d.obj(a).channels.set(trs::kRotationEuler + 2, 3.14159265358979323846 / 4);
  d.setParent(c, a);
  d.select({a});
  const mat4d was = d.stateOf(a).world, child = d.stateOf(c).world;
  const Inputs mirror = {{"kind", PropValue::ofEnum(2)},
                         {"vector", PropValue::ofVec3(-1, 1, 1)}};
  d.cycle(kApplyDeltaCommand, mirror);
  mat4d m;
  m.identity();
  m[0][0] = -1.0;
  test_assert(difference(d.stateOf(a).world, m * was) < 1e-14);
  test_assert(difference(d.stateOf(c).world, m * child) < 1e-14); /* it goes along */
  d.select({c});
  d.ok(kApplyDeltaCommand, {{"vector", PropValue::ofVec3(1, 0, 0)}});
  test_assert(std::fabs(d.obj(c).channels.get(trs::kLocation)) < 3.0);
  d.undo();
  d.select({a});
  d.ok(kApplyDeltaCommand, mirror);
  test_assert(difference(d.stateOf(a).world, was) < 1e-14);
}

} // namespace

int main()
{
  {
    Types t;
    testMirrors(t);
    testOwnAxes(t);
    testDrag(t);
    testOtherAxes(t);
    testSameAnswer(t);
    testTie(t);
    testMatrix(t);
    testExactByRotation(t);
    testParents(t);
  }
  testSecondAttempt();
  testCommand();
  return test_end();
}
