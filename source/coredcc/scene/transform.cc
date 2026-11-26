#include "coredcc/scene/transform.h"

#include "coredcc/bvh/box.h"
#include "coredcc/scene/models.h"
#include "coredcc/scene/transform_registry.h"

#include <cmath>
#include <cstring>
#include <limits>

namespace coredcc {

using solve::Linear;

/* ---- channels ---- */

bool ChannelBlock::operator==(const ChannelBlock &b) const
{
  return size() == b.size() &&
         (size() == 0 || memcmp(data(), b.data(), size_t(size()) * sizeof(double)) == 0);
}

int64_t channels::toInt(double v)
{
  if (!(v > -kMaxInt)) {
    return v <= -kMaxInt ? int64_t(-kMaxInt) : 0;
  }
  return v >= kMaxInt ? int64_t(kMaxInt) : int64_t(std::round(v));
}

int64_t EvalContext::intChannel(int32_t i) const
{
  return channels::toInt(channel(i));
}

ChannelBlock EvalContext::snapshot() const
{
  ChannelBlock out(channelCount());
  for (int32_t i = 0; i < out.size(); i++) {
    out.set(i, channel(i));
  }
  return out;
}

/* ---- states and deltas ---- */

namespace {

bool finiteMatrix(const mat4d &m)
{
  const double *d = m;
  for (int i = 0; i < 16; i++) {
    if (!bvh::finite(d[i])) {
      return false;
    }
  }
  return true;
}

bool finite3(const double3 &v)
{
  return bvh::finite(v[0]) && bvh::finite(v[1]) && bvh::finite(v[2]);
}

bool finiteQuat(const quatd &q)
{
  return bvh::finite(q[0]) && bvh::finite(q[1]) && bvh::finite(q[2]) && bvh::finite(q[3]);
}

double lengthOf(const double3 &v)
{
  return std::hypot(v[0], v[1], v[2]);
}

Status failed(const char *what)
{
  return Status::error(StatusCode::FailedPrecondition, what);
}

} // namespace

bool TransformState::finite() const
{
  return finiteMatrix(world) && finiteQuat(rotation) && finite3(localScale);
}

quatd TransformDelta::rotation() const
{
  return kind == DeltaKind::Rotate
             ? quatd::fromAxisAngle(basis.normalized().rotate(axis), angle)
             : quatd();
}

bool TransformDelta::valid() const
{
  const int32_t k = int32_t(kind);
  if (k < 0 || k > int32_t(DeltaKind::Scale) || !finiteQuat(basis) || !finite3(pivot) ||
      !finite3(vector) || !finite3(axis) || !bvh::finite(angle))
  {
    return false;
  }
  /* A basis or an axis of length 0 has no direction: normalized, it isn't a rotation. */
  const double b = basis[0] * basis[0] + basis[1] * basis[1] + basis[2] * basis[2] +
                   basis[3] * basis[3];
  return b > 0.0 && (kind != DeltaKind::Rotate || axis.dot(axis) > 0.0);
}

bool TransformDelta::identity() const
{
  if (kind == DeltaKind::Translate) {
    return vector[0] == 0.0 && vector[1] == 0.0 && vector[2] == 0.0;
  }
  if (kind == DeltaKind::Rotate) {
    return angle == 0.0;
  }
  return vector[0] == 1.0 && vector[1] == 1.0 && vector[2] == 1.0;
}

mat4d TransformDelta::matrix() const
{
  const quatd b = basis.normalized();
  if (kind == DeltaKind::Translate) {
    return solve::affine(Linear::identity(), b.rotate(vector));
  }
  const Linear m = kind == DeltaKind::Rotate
                       ? Linear::of(rotation())
                       : Linear::of(b).scaled(vector) * Linear::of(b.conjugated());
  return solve::affine(m, pivot - m.apply(pivot));
}

/* ---- checked calls ---- */

namespace {

/* Before calling into a model. */
Status ready(const TransformType &type, const EvalContext &ctx)
{
  if (type.runtime().hasStickyError()) {
    return failed("a transform model can't be called while an error is pending");
  }
  if (ctx.channelCount() != type.channelCount()) {
    return failed("the channels aren't this transform type's");
  }
  return Status();
}

/* A foreign model's exception, right after the call. */
Status thrown(const TransformType &type)
{
  return type.runtime().hasStickyError() ? type.runtime().takeStickyError() : Status();
}

Status checkTarget(const mat4d &target)
{
  const double *d = target;
  if (!finiteMatrix(target) || d[3] != 0.0 || d[7] != 0.0 || d[11] != 0.0 || d[15] != 1.0)
  {
    return Status::error(StatusCode::InvalidArgument,
                         "a transform's target must be finite and affine");
  }
  return Status();
}

Status checkParent(const TransformState &parent)
{
  solve::ParentSolver solver;
  return solver.set(parent.world) ? Status() : models::singularParent();
}

/* Within a few roundings of the numbers that were added up. */
bool sameTranslation(const TransformType &type,
                     const EvalContext &ctx,
                     const TransformState &state,
                     const TransformState &parent,
                     const mat4d &target)
{
  double lengths = 0.0, scale = 0.0;
  for (int32_t i = 0; i < type.channelCount(); i++) {
    lengths += type.isDistance(i) ? std::fabs(ctx.channel(i)) : 0.0;
  }
  for (int i = 0; i < 3; i++) {
    scale = std::fmax(scale, std::fabs(state.localScale[i]));
  }
  const double3 want = solve::translationOf(target);
  const double size =
      Linear::of(parent.world).norm() * lengths * (1.0 + scale) + lengthOf(want);
  const double e = lengthOf(solve::translationOf(state.world) - want);
  return e <= 16.0 * std::numeric_limits<double>::epsilon() * size;
}

/* The state of a model's answer. */
Status checkResult(const TransformType &type,
                   const EvalContext &out,
                   const TransformState &parent,
                   TransformState &state)
{
  Result<TransformState> r = evaluateTransform(type, out, parent);
  if (!r.ok()) {
    return r.status();
  }
  state = *r;
  return state.finite() ? Status()
                        : failed("the transform isn't representable: it isn't finite");
}

/* Fixed channels as they are stored. Which of them were to change. */
FixedChannels putBackFixed(const EvalContext &ctx, ChannelBlock &out)
{
  FixedChannels f;
  for (int32_t i = 0; ctx.hasFixed() && i < out.size(); i++) {
    if (!ctx.isFixed(i)) {
      continue;
    }
    const double was = ctx.channel(i), is = out.get(i);
    if (!(std::fabs(is - was) <=
          1e-9 * std::fmax(1.0, std::fmax(std::fabs(is), std::fabs(was)))))
    {
      (ctx.isOverridden(i) ? f.driven : f.locked) = true;
    }
    out.set(i, ctx.stored(i));
  }
  return f;
}

} // namespace

Result<TransformState> evaluateTransform(const TransformType &type,
                                         const EvalContext &ctx,
                                         const TransformState &parent)
{
  if (Status s = ready(type, ctx); !s.ok()) {
    return s;
  }
  const TransformState state = type.model().evaluate(ctx, parent);
  if (Status s = thrown(type); !s.ok()) {
    return s;
  }
  return state;
}

Result<double3> transformPivot(const TransformType &type,
                               const EvalContext &ctx,
                               const TransformState &parent)
{
  if (!type.model().hasPivot()) {
    Result<TransformState> s = evaluateTransform(type, ctx, parent);
    return s.ok() ? Result<double3>(solve::translationOf(s->world))
                  : Result<double3>(s.status());
  }
  if (Status s = ready(type, ctx); !s.ok()) {
    return s;
  }
  const double3 p = type.model().pivot(ctx, parent);
  if (Status s = thrown(type); !s.ok()) {
    return s;
  }
  return p;
}

Result<ChannelBlock> solveWorld(const TransformType &type,
                                const EvalContext &current,
                                const mat4d &target,
                                const TransformState &parent,
                                SetWorldMode mode,
                                FixedChannels *fixed)
{
  if (fixed) {
    *fixed = FixedChannels();
  }
  if (Status s = checkTarget(target); !s.ok()) {
    return s;
  }
  Result<TransformState> now = evaluateTransform(type, current, parent);
  if (!now.ok()) {
    return now.status();
  }
  ChannelBlock out = current.snapshot();
  if (now->finite() && solve::sameLinear(now->world, target) &&
      sameTranslation(type, current, *now, parent, target))
  {
    putBackFixed(current, out);
    return out;
  }
  if (Status s = checkParent(parent); !s.ok()) {
    return s;
  }
  Status s = type.model().setWorld(current, target, parent, mode, out);
  if (Status t = thrown(type); !t.ok()) {
    return t;
  }
  if (!s.ok()) {
    return s;
  }
  const FixedChannels inTheWay = putBackFixed(current, out);
  if (fixed) {
    *fixed = inTheWay;
  }
  if (mode == SetWorldMode::Exact && inTheWay.any()) {
    return failed("the target isn't representable: channels are locked or driven");
  }
  const EvalContext result = current.withChannels(out);
  TransformState state;
  if (s = checkResult(type, result, parent, state); !s.ok()) {
    return s;
  }
  if (!inTheWay.any() && !sameTranslation(type, result, state, parent, target)) {
    return failed("the target isn't representable: the translation differs");
  }
  if (mode == SetWorldMode::Exact && !solve::sameLinear(state.world, target)) {
    return failed("the target isn't representable: the rotation, scale or shear differs");
  }
  return out;
}

Result<ChannelBlock> solveDelta(const TransformType &type,
                                const EvalContext &base,
                                const ChannelBlock &nearTo,
                                const TransformDelta &delta,
                                const TransformState &parent,
                                FixedChannels *fixed)
{
  FixedChannels ignored;
  FixedChannels &inTheWay = fixed ? *fixed : ignored;
  inTheWay = FixedChannels();
  if (!delta.valid()) {
    return Status::error(StatusCode::InvalidArgument, "the transform delta isn't valid");
  }
  if (Status s = ready(type, base); !s.ok()) {
    return s;
  }
  if (Status s = ready(type, EvalContext(nearTo)); !s.ok()) {
    return s;
  }
  if (!type.model().hasApplyDelta()) {
    Result<TransformState> from = evaluateTransform(type, base, parent);
    if (!from.ok()) {
      return from.status();
    }
    return solveWorld(type,
                      base.withChannels(nearTo),
                      delta.matrix() * from->world,
                      parent,
                      SetWorldMode::Closest,
                      &inTheWay);
  }
  if (Status s = checkParent(parent); !s.ok()) {
    return s;
  }
  /* As the model sees the base's. */
  const ChannelBlock hint = base.withChannels(nearTo).snapshot();
  ChannelBlock out = hint;
  Status s = type.model().applyDelta(base, hint, delta, parent, out);
  if (Status t = thrown(type); !t.ok()) {
    return t;
  }
  if (!s.ok()) {
    return s;
  }
  inTheWay = putBackFixed(base, out);
  TransformState state;
  if (s = checkResult(type, base.withChannels(out), parent, state); !s.ok()) {
    return s;
  }
  if (delta.kind != DeltaKind::Scale) {
    return out;
  }
  /* A model scales along its own axes. Other channels may give exactly what was asked
   * for. */
  Result<TransformState> from = evaluateTransform(type, base, parent);
  if (!from.ok()) {
    return from.status();
  }
  const mat4d target = delta.matrix() * from->world;
  if (solve::sameLinear(state.world, target)) {
    return out;
  }
  Result<ChannelBlock> exact =
      solveWorld(type, base, target, parent, SetWorldMode::Exact);
  const bool threw = exact.status().code() == StatusCode::Internal;
  return exact.ok() || threw ? exact : Result<ChannelBlock>(std::move(out));
}

} // namespace coredcc
