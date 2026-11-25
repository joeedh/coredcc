/* solveWorld and solveDelta. */
#include "scene_fixture.h"

#include <string>

test_init;

using namespace scene_test;

namespace {

Result<ChannelBlock> solved(const TransformType &t,
                            const ChannelBlock &from,
                            const mat4d &target,
                            const TransformState &parent,
                            SetWorldMode mode = SetWorldMode::Exact)
{
  return solveWorld(t, EvalCtx(from), target, parent, mode);
}

/* The largest relative column error of two matrices' linear parts. */
double columnError(const mat4d &a, const mat4d &b)
{
  const Linear x = Linear::of(a), y = Linear::of(b);
  double e = 0.0;
  for (int i = 0; i < 3; i++) {
    const double len = lengthOf(y.col[i]);
    e = worst(e, lengthOf(x.col[i] - y.col[i]) / (len > 0.0 ? len : 1.0));
  }
  return e;
}

/* An object's own world gives its own channels, whatever they are. */
void testOwnWorld(const Types &t)
{
  Rand r(21);
  int refused = 0, changed = 0;
  const int reps = 300 * coredcc_test::testScale();
  for (double span : {3.0, 1000.0}) {
    for (double pivots : {0.0, 100.0, 1e6}) {
      for (int i = 0; i < reps; i++) {
        const TransformState p = chain(r, t, 1 + int(r.below(3)), span, true);
        double3 s = r.scale(span, true);
        if (i % 5 == 0) {
          s[int(r.below(3))] = i % 2 ? 0.0 : 1e-13;
        }
        for (int type = 0; type < 5; type++) {
          const TransformType &k = type == 0 ? *t.trs : type == 1 ? *t.matrix : *t.fbx;
          ChannelBlock c = k.defaults();
          if (type == 0) {
            c = trsChannels(r, k, s, pivots + 1.0);
          }
          else if (type == 1) {
            for (int n = 0; n < c.size(); n++) {
              c.set(n, r.range(-span, span));
            }
          }
          else {
            c = fbxChannels(r, k, type - 2, s, pivots);
          }
          for (SetWorldMode mode : {SetWorldMode::Exact, SetWorldMode::Closest}) {
            const Result<ChannelBlock> out = solved(k, c, eval(k, c, p).world, p, mode);
            refused += !out.ok();
            changed += out.ok() && !(*out == c);
          }
        }
      }
    }
  }
  test_assert(refused == 0 && changed == 0);

  /* Also under a singular parent, where nothing else is found. */
  TransformState flat;
  flat.world[1][1] = 0.0;
  for (const TransformType *k : {t.trs, t.fbx, t.matrix}) {
    ChannelBlock c = k->defaults();
    const mat4d own = eval(*k, c, flat).world;
    test_assert(solved(*k, c, own, flat).ok());
    mat4d moved = own;
    moved[3][0] += 1.0;
    for (SetWorldMode mode : {SetWorldMode::Exact, SetWorldMode::Closest}) {
      test_assert(solved(*k, c, moved, flat, mode).status().code() ==
                  StatusCode::FailedPrecondition);
    }
  }
}

/* Another node's world, from unrelated channels with the same pivots. */
void testRepresentable(const Types &t)
{
  Rand r(22);
  const int reps = 1500 * coredcc_test::testScale();
  int refused = 0, wrong = 0, scale = 0;
  for (int i = 0; i < reps; i++) {
    const bool signs = i % 2 == 0;
    const TransformState p = chain(r, t, int(r.below(3)), 3.0, signs);
    for (int type = 0; type < 4; type++) {
      const TransformType &k = type == 0 ? *t.trs : *t.fbx;
      const double3 s = r.scale(3.0, signs);
      const double pivots = i % 3 == 0 ? 10.0 : 0.0;
      const ChannelBlock goal = type == 0 ? trsChannels(r, k, s, 3.0) :
                                            fbxChannels(r, k, type - 1, s, pivots);
      ChannelBlock from = goal;
      const int at = type == 0 ? int(trs::kScale) : int(fbx::kScaling);
      put(from, at, r.scale(3.0, signs));
      put(from, 0, r.vec(5));
      if (type == 0) {
        from.set(trs::kRotationMode, double(r.below(7)));
      }
      put(from, type == 0 ? int(trs::kRotationEuler) : int(fbx::kRotation), r.vec(3));
      const mat4d target = eval(k, goal, p).world;
      const Result<ChannelBlock> out = solved(k, from, target, p);
      if (!out.ok()) {
        refused++;
        continue;
      }
      wrong += columnError(eval(k, *out, p).world, target) > 1e-9;
      /* The scale's magnitude is unique; only its signs aren't. */
      for (int a = 0; a < 3; a++) {
        scale += std::fabs(std::fabs(out->get(at + a)) - std::fabs(s[a])) > 1e-9 * std::fabs(s[a]);
      }
      /* Pivots and orders are kept. */
      if (type > 0) {
        for (int n = fbx::kRotationOrder; n < fbx::kScaling; n++) {
          wrong += out->get(n) != goal.get(n);
        }
      }
    }
  }
  test_assert(refused == 0 && wrong == 0 && scale == 0);

  /* Any matrix, for the Matrix type. */
  for (int i = 0; i < reps; i++) {
    const TransformState p = chain(r, t, int(r.below(3)), 3.0, true);
    const mat4d target =
        solve::affine({{r.vec(2), r.vec(2), i % 7 ? r.vec(2) : double3()}}, r.vec(10));
    const Result<ChannelBlock> out = solved(*t.matrix, t.matrix->defaults(), target, p);
    test_assert(out.ok());
    test_assert(out.ok() && difference(eval(*t.matrix, *out, p).world, target) < 1e-11);
  }
}

/* Signs: those of the current channels, where the target allows them. */
void testSigns(const Types &t)
{
  Rand r(23);
  int bad = 0;
  for (int i = 0; i < 2000; i++) {
    const double3 s = r.scale(3.0, true);
    ChannelBlock c = trsChannels(r, *t.trs, s, 3.0);
    const TransformState before = eval(*t.trs, c, TransformState());
    const TransformState p = chain(r, t, 1, 1.0, false); /* a rotation */
    const Result<ChannelBlock> out = solved(*t.trs, c, before.world, p);
    test_assert(out.ok());
    for (int a = 0; out.ok() && a < 3; a++) {
      bad += std::fabs(out->get(trs::kScale + a) - s[a]) > 1e-9 * std::fabs(s[a]);
    }
  }
  test_assert(bad == 0);

  /* A mirror along x, from positive channels: one sign changes, and the nearest rotation is the
   * current one. */
  for (int axis = 0; axis < 3; axis++) {
    ChannelBlock c = t.trs->defaults();
    double3 s(1, 2, 3);
    put(c, trs::kScale, s);
    put(c, trs::kRotationEuler, double3(0.3, -0.4, 0.5));
    const TransformState before = eval(*t.trs, c, TransformState());
    mat4d target = before.world;
    for (int row = 0; row < 3; row++) {
      target[axis][row] = -target[axis][row];
    }
    const Result<ChannelBlock> out = solved(*t.trs, c, target, TransformState());
    test_assert(out.ok());
    s[axis] = -s[axis];
    test_assert(out.ok() && lengthOf(get(*out, trs::kScale) - s) < 1e-12);
    test_assert(out.ok() &&
                lengthOf(get(*out, trs::kRotationEuler) - double3(0.3, -0.4, 0.5)) < 1e-12);
  }
}

/* Zero scales in targets, and the rotation they leave free. */
void testZeroColumns(const Types &t)
{
  Rand r(24);
  for (int i = 0; i < 3000; i++) {
    double3 s = r.scale(3.0, true);
    const int zeros = 1 + int(r.below(7));
    for (int a = 0; a < 3; a++) {
      s[a] = (zeros >> a) & 1 ? (i % 2 ? 0.0 : -0.0) : s[a];
    }
    const int type = int(r.below(4));
    const TransformType &k = type == 0 ? *t.trs : *t.fbx;
    const TransformState p = chain(r, t, int(r.below(3)), 2.0, true);
    const ChannelBlock goal = type == 0 ? trsChannels(r, k, s, 3.0) :
                                          fbxChannels(r, k, type - 1, s, 0.0);
    const TransformState want = eval(k, goal, p);
    ChannelBlock from = goal;
    const int at = type == 0 ? int(trs::kScale) : int(fbx::kScaling);
    put(from, at, double3(1, 1, 1));
    /* Only the scale differs, so the rotation is already right. */
    Result<ChannelBlock> out = solved(k, from, want.world, p);
    test_assert(out.ok());
    if (out.ok()) {
      const TransformState got = eval(k, *out, p);
      test_assert(columnError(got.world, want.world) < 1e-9);
      for (int a = 0; a < 3; a++) {
        test_assert(s[a] != 0.0 || out->get(at + a) == 0.0);
      }
      const bool positive = s[0] >= 0.0 && s[1] >= 0.0 && s[2] >= 0.0;
      test_assert(!positive || std::fabs(got.rotation.dot(want.rotation)) > 1.0 - 1e-12);
    }
    /* From an unrelated rotation. */
    put(from, type == 0 ? int(trs::kRotationEuler) : int(fbx::kRotation), r.vec(3));
    out = solved(k, from, want.world, p);
    test_assert(out.ok() && columnError(eval(k, *out, p).world, want.world) < 1e-9);
  }
  /* A column that only rounding left counts as zero. */
  for (int i = 0; i < 300; i++) {
    double3 s = r.scale(3.0, false);
    const int flat = int(r.below(3));
    s[flat] = 0.0;
    const ChannelBlock goal = trsChannels(r, *t.trs, s, 3.0);
    mat4d target = eval(*t.trs, goal, TransformState()).world;
    const double3 noise = r.vec(1e-14);
    for (int row = 0; row < 3; row++) {
      target[flat][row] = noise[row];
    }
    const Result<ChannelBlock> out = solved(*t.trs, t.trs->defaults(), target, TransformState());
    test_assert(out.ok() && out->get(trs::kScale + flat) == 0.0);
  }
  /* One column, opposite to where it is now. */
  for (int a = 0; a < 3; a++) {
    ChannelBlock c = t.trs->defaults();
    mat4d target;
    target.zero();
    target[3][3] = 1.0;
    target[a][a] = -2.0;
    const Result<ChannelBlock> out = solved(*t.trs, c, target, TransformState());
    test_assert(out.ok() && difference(eval(*t.trs, *out, TransformState()).world, target) < 1e-12);
    test_assert(out.ok() && out->get(trs::kScale + a) == 2.0);
  }
}

/* Shear: Exact refuses it, Closest keeps the translation, the lengths and the mirror. */
void testShear(const Types &t)
{
  Rand r(25);
  const mat4d at = solve::affine(Linear::identity(), double3(4, 5, 6));
  for (double e : {1e-11, 1e-7, 1e-3, 0.5}) {
    mat4d shear = at;
    shear[1][0] = e;
    const Result<ChannelBlock> out = solved(*t.trs, t.trs->defaults(), shear, TransformState());
    test_assert(out.ok() == (e < 1e-9));
    test_assert(out.ok() || out.status().code() == StatusCode::FailedPrecondition);
    test_assert(
        solved(*t.trs, t.trs->defaults(), shear, TransformState(), SetWorldMode::Closest).ok());
  }
  int lost = 0, refused = 0, accepted = 0;
  for (int i = 0; i < 2000; i++) {
    const TransformState p = chain(r, t, 2, 3.0, true);
    const double shear = i % 4 ? r.range(0.0, 1.0) : 0.0;
    Linear skew = Linear::identity();
    skew.col[1][0] = shear;
    const bool mirror = r.coin();
    const Linear target = Linear::of(p.world) * Linear::of(r.rotation()) * skew;
    const mat4d world =
        solve::affine(target.scaled(double3(mirror ? -1.0 : 1.0, 1.0, 2.0)), r.vec(9));
    for (int type = 0; type < 4; type++) {
      const TransformType &k = type == 0 ? *t.trs : *t.fbx;
      ChannelBlock c = k.defaults();
      if (type > 0) {
        c.set(fbx::kInheritType, type - 1);
      }
      const Result<ChannelBlock> out = solved(k, c, world, p, SetWorldMode::Closest);
      refused += !out.ok();
      const bool exactModel = type == 0 || type == 2;
      accepted += shear > 0.01 && exactModel && solved(k, c, world, p).ok();
      if (out.ok()) {
        const double3 moved =
            solve::translationOf(eval(k, *out, p).world) - solve::translationOf(world);
        lost += lengthOf(moved) > 1e-12 * 50;
        if (exactModel) {
          const double3 s = get(*out, type == 0 ? int(trs::kScale) : int(fbx::kScaling));
          lost += (s[0] * s[1] * s[2] < 0.0) != mirror;
        }
      }
    }
  }
  test_assert(lost == 0 && refused == 0 && accepted == 0);

  /* Arbitrary targets under a sheared parent aren't representable. */
  int wrongly = 0;
  for (int i = 0; i < 2000; i++) {
    const TransformState p = chain(r, t, 3, 3.0, false);
    ChannelBlock c = t.fbx->defaults();
    c.set(fbx::kInheritType, double(i % 3));
    const mat4d target =
        solve::affine(Linear::of(r.rotation()).scaled(r.scale(3, false)), r.vec(3));
    double3 place, scale;
    quatd q;
    const bool sheared = litestl::math::decomposeTrs(p.world, place, q, scale) > 0.01;
    wrongly += sheared && solved(*t.fbx, c, target, p).ok();
  }
  test_assert(wrongly == 0);
}

/* Float matrices are Closest's. */
void testFloatTargets(const Types &t)
{
  Rand r(26);
  int exact = 0, closest = 0;
  for (int i = 0; i < 500; i++) {
    const ChannelBlock c = trsChannels(r, *t.trs, r.scale(3, false), 3.0);
    mat4d m = eval(*t.trs, c, TransformState()).world;
    double *d = m;
    for (int k = 0; k < 16; k++) {
      d[k] = float(d[k]);
    }
    exact += solved(*t.trs, t.trs->defaults(), m, TransformState()).ok();
    closest += solved(*t.trs, t.trs->defaults(), m, TransformState(), SetWorldMode::Closest).ok();
  }
  test_assert(exact < 5 && closest == 500);
}

void testTranslations(const Types &t)
{
  Rand r(27);
  /* Large numbers that cancel: pivots, and a parent far away. */
  for (int i = 0; i < 1000; i++) {
    ChannelBlock c = t.fbx->defaults();
    TransformState p;
    ChannelBlock goal = c;
    const double3 turn = r.vec(3), at = r.vec(1);
    put(goal, fbx::kTranslation, at);
    put(goal, fbx::kRotation, turn);
    if (i % 3 == 0) {
      const double3 arm = r.vec(1e6);
      const double3 back = litestl::math::eulerToQuat(turn, EulerOrder::XYZ).rotate(arm) * -1.0;
      for (ChannelBlock *b : {&c, &goal}) {
        put(*b, fbx::kScalingOffset, arm);
        put(*b, fbx::kRotationOffset, back);
      }
    }
    else if (i % 3 == 1) {
      p.world = solve::affine(Linear::of(r.rotation()), r.vec(1e10));
    }
    else {
      const Linear big = Linear::of(r.rotation()).scaled(double3(1e6, 2e6, 3e6));
      p.world = solve::affine(big, big.apply(at) * -1.0);
    }
    const mat4d target = eval(*t.fbx, goal, p).world;
    for (SetWorldMode mode : {SetWorldMode::Exact, SetWorldMode::Closest}) {
      test_assert(solved(*t.fbx, c, target, p, mode).ok());
    }
  }
  /* Ill-conditioned parents: a pure move stays within a few roundings. */
  for (double cond : {1e2, 1e4, 1e6, 1e8}) {
    for (int i = 0; i < 300; i++) {
      TransformState p;
      const double3 s(1, 1 / std::sqrt(cond), 1 / cond);
      p.world = solve::affine(
          Linear::of(r.rotation()).scaled(s) * Linear::of(r.rotation()), r.vec(3));
      ChannelBlock c = t.trs->defaults();
      put(c, trs::kLocation, r.vec(2));
      mat4d target = eval(*t.trs, c, p).world;
      const double3 to = r.vec(3);
      for (int a = 0; a < 3; a++) {
        target[3][a] = to[a];
      }
      const Result<ChannelBlock> out = solved(*t.trs, c, target, p, SetWorldMode::Closest);
      test_assert(out.ok());
      if (out.ok()) {
        const double e = lengthOf(solve::translationOf(eval(*t.trs, *out, p).world) - to);
        const double3 l = get(*out, trs::kLocation);
        test_assert(e <= 8 * 2.3e-16 * (Linear::of(p.world).norm() * lengthOf(l) + 6.0));
      }
    }
  }
  /* The last bit of a translation far away doesn't count. */
  TransformState distant;
  distant.world[3][0] = 1e10;
  mat4d bit = distant.world;
  bit[3][0] = std::nextafter(1e10, 2e10);
  const Result<ChannelBlock> same = solved(*t.trs, t.trs->defaults(), bit, distant);
  test_assert(same.ok() && *same == t.trs->defaults());
  /* Far away, translations are compared as exactly, and the linear part isn't hidden by them. */
  ChannelBlock c = t.trs->defaults();
  put(c, trs::kLocation, double3(1e10, -2e10, 3e10));
  mat4d target = eval(*t.trs, c, TransformState()).world;
  target[3][0] += 1.0;
  Result<ChannelBlock> out = solved(*t.trs, c, target, TransformState());
  test_assert(out.ok() && out->get(0) == 1e10 + 1.0);
  target[1][0] = 1e-6; /* a shear */
  test_assert(!solved(*t.trs, c, target, TransformState()).ok());
  target = eval(*t.trs, c, TransformState()).world;
  target[0][0] = target[1][1] = std::cos(1e-6);
  target[0][1] = std::sin(1e-6);
  target[1][0] = -std::sin(1e-6);
  out = solved(*t.trs, c, target, TransformState()); /* not the current channels */
  test_assert(out.ok() && std::fabs(out->get(trs::kRotationEuler + 2) - 1e-6) < 1e-12);
}

void testBadInput(const Types &t)
{
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const TransformState root;
  for (const TransformType *k : {t.trs, t.fbx, t.matrix}) {
    const ChannelBlock c = k->defaults();
    for (SetWorldMode mode : {SetWorldMode::Exact, SetWorldMode::Closest}) {
      mat4d m = root.world;
      m[1][2] = nan;
      test_assert(solved(*k, c, m, root, mode).status().code() == StatusCode::InvalidArgument);
      m = root.world;
      m[1][3] = 0.1;
      test_assert(solved(*k, c, m, root, mode).status().code() == StatusCode::InvalidArgument);
      m = root.world;
      m[3][3] = 2.0;
      test_assert(solved(*k, c, m, root, mode).status().code() == StatusCode::InvalidArgument);
      TransformState p;
      p.world[0][0] = nan;
      m = root.world;
      test_assert(solved(*k, c, m, p, mode).status().code() == StatusCode::FailedPrecondition);
      /* Too large for the channels. */
      p = TransformState();
      p.world[0][0] = 1e-200;
      m[0][0] = 1e200;
      test_assert(solved(*k, c, m, p, mode).status().code() == StatusCode::FailedPrecondition);
    }
    const ChannelBlock few(k->channelCount() - 1);
    TransformDelta d;
    test_assert(!solved(*k, few, root.world, root).ok());
    test_assert(!evaluateTransform(*k, EvalCtx(few), root).ok());
    test_assert(!solveDelta(*k, EvalCtx(c), few, d, root).ok());
    test_assert(!solveDelta(*k, EvalCtx(few), c, d, root).ok());
    TransformState flat;
    flat.world[2][2] = 0.0;
    d.vector = double3(1, 0, 0);
    test_assert(solveDelta(*k, EvalCtx(c), c, d, flat).status().code() ==
                StatusCode::FailedPrecondition);
    for (int bad = 0; bad < 6; bad++) {
      TransformDelta b;
      b.setKind(bad == 0 ? 3 : 0);
      b.setKind(bad == 1 ? -1 : b.kindValue());
      b.angle = bad == 2 ? nan : 0.0;
      b.vector[0] = bad == 3 ? nan : 0.0;
      b.pivot[1] = bad == 4 ? nan : 0.0;
      b.basis[2] = bad == 5 ? nan : 0.0;
      test_assert(solveDelta(*k, EvalCtx(c), c, b, root).status().code() ==
                  StatusCode::InvalidArgument);
    }
  }
  /* Current channels that aren't finite are only a hint. */
  for (const TransformType *k : {t.trs, t.fbx, t.matrix}) {
    ChannelBlock c = k->defaults();
    for (int i = 0; i < 3; i++) {
      c.set(i + (k == t.trs ? int(trs::kRotationEuler) : 3), nan);
    }
    const quatd turn = quatd::fromAxisAngle(double3(1, 2, 3), 0.7);
    const mat4d m = solve::affine(Linear::of(turn), double3(1, 2, 3));
    Result<ChannelBlock> out = solved(*k, c, m, root);
    test_assert(out.ok() && difference(eval(*k, *out, root).world, m) < 1e-12);
    const mat4d none = solve::affine(Linear(), double3(1, 2, 3));
    out = solved(*k, c, none, root);
    test_assert(out.ok() && difference(eval(*k, *out, root).world, none) == 0.0);
  }
  /* A channel that doesn't count doesn't matter. */
  ChannelBlock q = t.trs->defaults();
  q.set(trs::kRotationMode, trs::kQuaternionMode);
  q.set(trs::kRotationEuler, nan);
  q.set(trs::kRotationQuaternion + 3, -0.01);
  const quatd turn = quatd::fromAxisAngle(double3(1, 2, 3), 0.1);
  const mat4d m = solve::affine(Linear::of(turn), double3(4, 0, 0));
  const Result<ChannelBlock> out = solved(*t.trs, q, m, root);
  test_assert(out.ok() && difference(eval(*t.trs, *out, root).world, m) < 1e-14);
  test_assert(out.ok() && out->get(trs::kRotationQuaternion + 3) < -0.9); /* the same side */
}

/* ---- deltas ---- */

TransformDelta deltaOf(DeltaKind kind)
{
  TransformDelta d;
  d.kind = kind;
  return d;
}

void testDeltaMatrix()
{
  Rand r(28);
  for (int i = 0; i < 300; i++) {
    TransformDelta d;
    d.basis = r.rotation();
    d.pivot = r.vec(3);
    d.vector = r.vec(2);
    d.axis = r.vec(2);
    d.angle = r.range(-7, 7);
    for (int k = 0; k < 3; k++) {
      d.setKind(k);
      const double3 p = r.vec(4);
      const double3 local = d.basis.conjugated().rotate(p - d.pivot);
      const double3 u = d.axis * (1.0 / lengthOf(d.axis));
      double3 want;
      if (k == 0) {
        want = p + d.basis.rotate(d.vector);
      }
      else if (k == 1) { /* Rodrigues, in the basis */
        const double3 turned = local * std::cos(d.angle) + u.cross(local) * std::sin(d.angle) +
                               u * (u.dot(local) * (1.0 - std::cos(d.angle)));
        want = d.pivot + d.basis.rotate(turned);
      }
      else {
        want = d.pivot + d.basis.rotate(local * d.vector);
      }
      test_assert(lengthOf(litestl::math::transformPoint(d.matrix(), p) - want) < 1e-12);
    }
  }
}

/* Under parents that only rotate, move and scale uniformly (or mirror), every delta that keeps
 * an object free of shear is exact. */
void testDeltas(const Types &t)
{
  Rand r(29);
  for (int i = 0; i < 2000; i++) {
    TransformState p;
    const double size = r.range(0.2, 4.0);
    const bool mirror = i % 3 == 0;
    p.world = solve::affine(
        Linear::of(r.rotation()).scaled(double3(mirror ? -size : size, size, size)) *
            Linear::of(r.rotation()),
        r.vec(5));
    const int type = int(r.below(3)); /* Trs, RSrs, Matrix */
    const TransformType &k = type == 0 ? *t.trs : type == 1 ? *t.fbx : *t.matrix;
    const double pivots = i % 2 ? 3.0 : 0.0;
    ChannelBlock c = type == 0 ? trsChannels(r, k, r.scale(3, true), 3.0) :
                     type == 1 ? fbxChannels(r, k, fbx::kRSrs, r.scale(3, true), pivots) :
                                 k.defaults();
    const TransformState base = eval(k, c, p);
    for (int kind = 0; kind < 3; kind++) {
      TransformDelta d = deltaOf(DeltaKind(kind));
      d.basis = r.rotation();
      d.pivot = r.vec(5);
      d.vector = r.vec(3);
      d.axis = r.vec(1);
      d.angle = r.range(-3, 3);
      if (kind == 2) { /* uniform, or along the object's own axes */
        if (r.coin()) {
          d.vector = double3(1, 1, 1) * (r.range(0.1, 3) * (r.coin() ? -1.0 : 1.0));
        }
        else {
          Linear axes = Linear::of(base.world);
          for (double3 &col : axes.col) {
            col = col * (1.0 / lengthOf(col));
          }
          if (axes.col[0].cross(axes.col[1]).dot(axes.col[2]) < 0.0) {
            axes.col[0] = axes.col[0] * -1.0;
          }
          d.basis = quatd::fromMatrix(solve::affine(axes, double3()));
        }
      }
      const Result<ChannelBlock> out = solveDelta(k, EvalCtx(c), c, d, p);
      test_assert(out.ok());
      test_assert(out.ok() && difference(eval(k, *out, p).world, d.matrix() * base.world) < 1e-10);
      if (out.ok() && kind == 2 && type == 0) { /* only the scale and the location change */
        for (int n = trs::kRotationMode; n < trs::kScale; n++) {
          test_assert(out->get(n) == c.get(n));
        }
      }
    }
  }
}

/* FBX's RrSs and Rrs nodes turn exactly under any parent: their rotation comes before the
 * parent's scale. */
void testFbxTurns(const Types &t)
{
  Rand r(30);
  for (int i = 0; i < 2000; i++) {
    const TransformState p = chain(r, t, 3, 3.0, true);
    const ChannelBlock c =
        fbxChannels(r, *t.fbx, i % 2 ? fbx::kRrSs : fbx::kRrs, r.scale(3, true), i % 3 ? 2.0 : 0.0);
    TransformDelta d = deltaOf(DeltaKind::Rotate);
    d.axis = r.vec(1);
    d.angle = r.range(-3, 3);
    d.basis = r.rotation();
    d.pivot = r.vec(5);
    const Result<ChannelBlock> out = solveDelta(*t.fbx, EvalCtx(c), c, d, p);
    const mat4d want = d.matrix() * eval(*t.fbx, c, p).world;
    test_assert(out.ok());
    test_assert(out.ok() && difference(eval(*t.fbx, *out, p).world, want) < 1e-9 * 30);
  }
}

/* Non-uniform parents: the axis stays where it is, the origin goes where the delta says, and the
 * scale is untouched. */
void testShearedParents(const Types &t)
{
  Rand r(31);
  for (int i = 0; i < 2000; i++) {
    const TransformState p = chain(r, t, 2, 3.0, true);
    const ChannelBlock c = trsChannels(r, *t.trs, r.scale(3, true), 3.0);
    const TransformState base = eval(*t.trs, c, p);
    TransformDelta d = deltaOf(DeltaKind::Rotate);
    d.axis = r.vec(1);
    d.angle = r.range(-6, 6);
    d.pivot = r.vec(5);
    const Result<ChannelBlock> out = solveDelta(*t.trs, EvalCtx(c), c, d, p);
    test_assert(out.ok());
    if (!out.ok()) {
      continue;
    }
    const TransformState after = eval(*t.trs, *out, p);
    const double3 want =
        litestl::math::transformPoint(d.matrix(), solve::translationOf(base.world));
    test_assert(lengthOf(solve::translationOf(after.world) - want) < 1e-11 * 30);
    test_assert(lengthOf(get(*out, trs::kScale) - get(c, trs::kScale)) == 0.0);
    /* The object's line along the axis: world direction d.axis, in the object before and after. */
    mat4d inv0, inv1;
    test_assert(litestl::math::tryInverse(base.world, inv0) &&
                litestl::math::tryInverse(after.world, inv1));
    const double3 a = litestl::math::transformVector(inv0, d.axis);
    const double3 b = litestl::math::transformVector(inv1, d.axis);
    test_assert(lengthOf(a - b) < 1e-9 * lengthOf(a) * 10);
  }
  /* The pivot itself doesn't move, to the bit, and neither do other channels. */
  for (int kind = 1; kind < 3; kind++) {
    for (int i = 0; i < 300; i++) {
      const TransformState p = chain(r, t, 2, 3.0, true);
      const bool isTrs = r.coin();
      const TransformType &k = isTrs ? *t.trs : *t.fbx;
      const ChannelBlock c = isTrs ? trsChannels(r, k, r.scale(3, true), 3.0) :
                                     fbxChannels(r, k, int(r.below(3)), r.scale(3, true), 0.0);
      TransformDelta d = deltaOf(DeltaKind(kind));
      d.pivot = solve::translationOf(eval(k, c, p).world);
      d.axis = r.vec(1);
      d.angle = 1.0;
      d.vector = r.vec(2);
      const Result<ChannelBlock> out = solveDelta(k, EvalCtx(c), c, d, p);
      test_assert(out.ok() && lengthOf(get(*out, 0) - get(c, 0)) == 0.0);
    }
  }
}

/* A drag: the total from `base` each time, the previous result as `nearTo`. */
void testDrags(const Types &t)
{
  const double pi = 3.14159265358979323846;
  for (int order = 0; order < 6; order++) {
    for (int axis = 0; axis < 3; axis++) {
      for (int type = 0; type < 2; type++) {
        const TransformType &k = type == 0 ? *t.trs : *t.fbx;
        const int at = type == 0 ? int(trs::kRotationEuler) : int(fbx::kRotation);
        ChannelBlock base = k.defaults();
        base.set(type == 0 ? int(trs::kRotationMode) : int(fbx::kRotationOrder), order);
        ChannelBlock last = base;
        TransformDelta d = deltaOf(DeltaKind::Rotate);
        d.axis[axis] = 2.0;
        for (int step = 1; step <= 48; step++) {
          d.angle = step * pi / 12.0; /* to 720 degrees */
          const Result<ChannelBlock> out = solveDelta(k, EvalCtx(base), last, d, TransformState());
          test_assert(out.ok());
          double jump = 0.0;
          for (int i = 0; i < 3; i++) {
            jump = worst(jump, std::fabs(out->get(at + i) - last.get(at + i)));
          }
          test_assert(jump < 0.3);
          last = *out;
        }
        test_assert(std::fabs(last.get(at + axis) - 4.0 * pi) < 1e-9);
        for (int i = 0; i < 3; i++) {
          test_assert(i == axis || std::fabs(last.get(at + i)) < 1e-9);
        }
      }
    }
  }
  /* Near and through gimbal lock the world is right and the angles are continuous-ish. */
  Rand r(32);
  for (int i = 0; i < 300; i++) {
    ChannelBlock base = t.trs->defaults();
    base.set(trs::kRotationMode, double(i % 6));
    const int mid[6] = {1, 2, 0, 2, 0, 1};
    base.set(trs::kRotationEuler + mid[i % 6], pi / 2 - (i % 4 ? r.range(-1e-6, 1e-6) : 0.0));
    ChannelBlock last = base;
    TransformDelta d = deltaOf(DeltaKind::Rotate);
    d.axis = r.vec(1);
    for (int step = 1; step <= 20; step++) {
      d.angle = step * 0.05;
      const Result<ChannelBlock> out = solveDelta(*t.trs, EvalCtx(base), last, d, TransformState());
      test_assert(out.ok());
      const mat4d want = d.matrix() * eval(*t.trs, base, TransformState()).world;
      test_assert(difference(eval(*t.trs, *out, TransformState()).world, want) < 1e-7);
      last = *out;
    }
  }
  /* A quaternion stays in its hemisphere, also when the base's is the other one. */
  ChannelBlock base = t.trs->defaults();
  base.set(trs::kRotationMode, trs::kQuaternionMode);
  base.set(trs::kRotationQuaternion + 3, -1.0);
  ChannelBlock last = base;
  TransformDelta d = deltaOf(DeltaKind::Rotate);
  d.axis = double3(0, 0, 1);
  for (int step = 1; step <= 96; step++) {
    d.angle = step * pi / 24.0;
    const Result<ChannelBlock> out = solveDelta(*t.trs, EvalCtx(base), last, d, TransformState());
    double dot = 0.0;
    for (int i = 0; i < 4; i++) {
      dot += out->get(trs::kRotationQuaternion + i) * last.get(trs::kRotationQuaternion + i);
    }
    test_assert(out.ok() && dot > 0.9);
    last = *out;
  }
  /* A scale goes to 0 and back, and to a mirror. */
  base = t.trs->defaults();
  put(base, trs::kScale, double3(2, 3, 4));
  last = base;
  d = deltaOf(DeltaKind::Scale);
  for (double v : {0.5, 0.0, -1.0, 1.0}) {
    d.vector = double3(v, 1.0, 1.0);
    const Result<ChannelBlock> out = solveDelta(*t.trs, EvalCtx(base), last, d, TransformState());
    test_assert(out.ok() && lengthOf(get(*out, trs::kScale) - double3(2 * v, 3, 4)) < 1e-14);
    last = *out;
  }
  test_assert(last == base);
  /* Whatever the delta doesn't touch is the base's. */
  for (const TransformType *k : {t.trs, t.fbx}) {
    ChannelBlock other = k->defaults();
    for (int i = 0; i < other.size(); i++) {
      other.set(i, other.get(i) + 0.25);
    }
    d = deltaOf(DeltaKind::Translate);
    d.vector = double3(1, 2, 3);
    const Result<ChannelBlock> moved =
        solveDelta(*k, EvalCtx(k->defaults()), other, d, TransformState());
    test_assert(moved.ok() && lengthOf(get(*moved, 0) - d.vector) == 0.0);
    for (int i = 3; moved.ok() && i < moved->size(); i++) {
      test_assert(moved->get(i) == k->defaults().get(i));
    }
  }
  /* Overridden channels come back as they are stored, and the others follow. */
  ChannelBlock over(trs::kCount);
  litestl::util::Vector<uint8_t> set;
  for (int i = 0; i < trs::kCount; i++) {
    over.set(i, 10.0);
    set.append(uint8_t(i == 0 ? 1 : 0));
  }
  d = deltaOf(DeltaKind::Translate);
  d.vector = double3(1, 2, 3);
  FixedChannels fixed;
  Result<ChannelBlock> out =
      solveDelta(*t.trs, EvalCtx(base, 0.0, over, set), base, d, TransformState(), &fixed);
  test_assert(out.ok() && fixed.driven && !fixed.locked &&
              lengthOf(get(*out, trs::kLocation) - double3(base.get(trs::kLocation), 2, 3)) == 0.0);
  /* What is stored under them isn't looked at: it may be anything. */
  ChannelBlock odd = t.trs->defaults();
  odd.set(trs::kLocation, std::numeric_limits<double>::infinity());
  out = solveDelta(*t.trs, EvalCtx(odd, 0.0, over, set), odd, d, TransformState(), &fixed);
  test_assert(out.ok() && fixed.driven && out->get(trs::kLocation) > 1e300 &&
              out->get(trs::kLocation + 1) == 2.0);
  odd = t.trs->defaults();
  put(odd, trs::kRotationEuler, double3(3.3, 0.2, 3.3));
  for (int i = 0; i < trs::kCount; i++) {
    set[i] = uint8_t(i == trs::kRotationEuler || i == trs::kRotationEuler + 2 ? 1 : 0);
    over.set(i, 0.1);
  }
  d = deltaOf(DeltaKind::Rotate);
  d.axis = double3(0, 1, 0);
  d.angle = 0.01;
  out = solveDelta(*t.trs, EvalCtx(odd, 0.0, over, set), odd, d, TransformState(), &fixed);
  test_assert(out.ok() && std::fabs(out->get(trs::kRotationEuler + 1) - 0.21) < 0.01);
}

/* rotationScale itself, where the models don't reach. */
void testRotationScale()
{
  const double inf = std::numeric_limits<double>::infinity();
  solve::RotationScale rs;
  Linear x = Linear::identity(), a = Linear::identity();
  a.col[1] = double3();
  test_assert(!solve::rotationScale(x, a, quatd(), double3(1, 1, 1), rs));
  x.col[1] = double3();
  test_assert(!solve::rotationScale(x, a, quatd(), double3(1, 1, 1), rs));
  x = Linear::identity();
  a.col[1] = double3(0, inf, 0);
  test_assert(!solve::rotationScale(x, a, quatd(), double3(1, 1, 1), rs));
  a = Linear::identity();
  x.col[2][0] = inf;
  test_assert(!solve::rotationScale(x, a, quatd(), double3(1, 1, 1), rs));
  /* Lengths from denormal to huge. */
  for (double size : {1e-310, 1e-150, 1.0, 1e150, 1e300}) {
    x = Linear::of(quatd::fromAxisAngle(double3(1, 1, 0), 0.5)).scaled(double3(size, size, size));
    test_assert(solve::rotationScale(x, a, quatd(), double3(1, 1, 1), rs));
    test_assert(rs.residual < (size < 1e-300 ? 1e-9 : 1e-14));
    test_assert(rs.rotation.dot(quatd::fromAxisAngle(double3(1, 1, 0), 0.5)) > 1.0 - 1e-14);
    test_assert(std::fabs(rs.scale[0] / size - 1.0) < (size < 1e-300 ? 1e-9 : 1e-14));
  }
  /* Parallel columns: finite, and the best that can be done. */
  for (double3 &col : x.col) {
    col = double3(1, 2, 3);
  }
  test_assert(solve::rotationScale(x, a, quatd(), double3(1, 1, 1), rs));
  test_assert(rs.residual > 0.1);
  test_assert(lengthOf(rs.rotation.rotate(double3(1, 0, 0)) * rs.scale[0] - x.col[0]) < 1e-14);
  /* Two nearly parallel columns: the rotation comes from the others. */
  for (int skip = 0; skip < 3; skip++) {
    a = Linear::identity();
    a.col[(skip + 1) % 3] = a.col[(skip + 2) % 3];
    a.col[(skip + 1) % 3][skip] = 1e-9;
    x = (Linear::of(quatd::fromAxisAngle(double3(1, 2, 3), 1.0)) * a).scaled(double3(2, -3, 4));
    test_assert(solve::rotationScale(x, a, quatd(), double3(1, -1, 1), rs));
    test_assert(rs.residual < 1e-14 && std::fabs(rs.scale[1] + 3.0) < 1e-14);
  }
  a = Linear::identity();
  /* Nothing left: the current rotation. */
  const quatd q = quatd::fromAxisAngle(double3(1, 2, 3), 1.0);
  for (double3 &col : x.col) {
    col = double3();
  }
  test_assert(solve::rotationScale(x, a, q, double3(1, 1, 1), rs));
  test_assert(rs.rotation.dot(q) > 1.0 - 1e-15 && rs.residual == 0.0 && rs.scale[0] == 0.0);
}

} // namespace

int main()
{
  {
    Types t;
    testOwnWorld(t);
    testRepresentable(t);
    testSigns(t);
    testZeroColumns(t);
    testShear(t);
    testFloatTargets(t);
    testTranslations(t);
    testBadInput(t);
    testDeltaMatrix();
    testDeltas(t);
    testFbxTurns(t);
    testShearedParents(t);
    testDrags(t);
    testRotationScale();
  }
  return test_end();
}
