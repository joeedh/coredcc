#include "coredcc/scene/builtins.h"

#include "coredcc/bvh/box.h"
#include "coredcc/scene/models.h"

#include <cmath>
#include <string>

namespace coredcc {

namespace models {

using solve::Linear;

double3 readVec3(const EvalCtx &ctx, int32_t first)
{
  return double3(ctx.channel(first), ctx.channel(first + 1), ctx.channel(first + 2));
}

void writeVec3(ChannelBlock &out, int32_t first, const double3 &v)
{
  for (int32_t i = 0; i < 3; i++) {
    out.set(first + i, v[i]);
  }
}

PropertyDef orderEnum(const char *name)
{
  const char *const orders[] = {"XYZ", "XZY", "YXZ", "YZX", "ZXY", "ZYX"};
  PropertyDef def = PropertyDef::enumProp(name);
  for (int i = 0; i < 6; i++) {
    def.item(orders[i], i);
  }
  def.defaultValue(PropValue::ofEnum(0));
  return def;
}

double3 nearestEuler(const quatd &q, litestl::math::EulerOrder order, double3 hint)
{
  for (int i = 0; i < 3; i++) {
    hint[i] = bvh::finite(hint[i]) ? hint[i] : 0.0;
  }
  return litestl::math::quatToEuler(q, order, hint);
}

TransformState childState(const TransformState &parent,
                          const Linear &world,
                          const double3 &localTranslation,
                          const quatd &localRotation,
                          const double3 &localScale)
{
  TransformState s;
  s.world = solve::affine(world, litestl::math::transformPoint(parent.world, localTranslation));
  s.rotation = (parent.rotation * localRotation).normalized();
  s.localScale = localScale;
  return s;
}

Status singularParent()
{
  return Status::error(StatusCode::FailedPrecondition,
                       "the parent's matrix has no inverse (a scale of 0, or nearly 0)");
}

Status unsolvable()
{
  return Status::error(StatusCode::FailedPrecondition,
                       "the target isn't representable: it isn't finite under this parent");
}

namespace {

/* A vector scaled along the delta's basis. */
double3 scaleVector(const TransformDelta &delta, const double3 &factors, const double3 &v)
{
  const quatd b = delta.basis.normalized();
  return b.rotate(b.conjugated().rotate(v) * factors);
}

double3 signsOf(const TransformDelta &delta)
{
  double3 s;
  for (int i = 0; i < 3; i++) {
    s[i] = delta.vector[i] < 0.0 ? -1.0 : 1.0;
  }
  return s;
}

double3 unitOf(const double3 &v)
{
  return v / std::hypot(v[0], v[1], v[2]);
}

} // namespace

double3 shiftOf(const TransformDelta &delta, const double3 &point)
{
  const double3 arm = point - delta.pivot;
  switch (delta.kind) {
    case DeltaKind::Translate:
      return delta.basis.normalized().rotate(delta.vector);
    case DeltaKind::Rotate:
      return delta.rotation().rotate(arm) - arm;
    case DeltaKind::Scale:
      return scaleVector(delta, delta.vector, arm) - arm;
  }
  return double3();
}

double3 factorsAlong(const TransformDelta &delta, const Linear &axes)
{
  const double3 sizes(
      std::fabs(delta.vector[0]), std::fabs(delta.vector[1]), std::fabs(delta.vector[2]));
  double3 f;
  for (int i = 0; i < 3; i++) {
    const double3 c = unitOf(axes.col[i]);
    f[i] = scaleVector(delta, sizes, c).dot(c);
  }
  return f;
}

/* But for rounding. */
static bool sameRotation(const quatd &a, const quatd &b)
{
  const double side = a.dot(b) < 0.0 ? -1.0 : 1.0;
  double e = 0.0;
  for (int i = 0; i < 4; i++) {
    e = std::fmax(e, std::fabs(a[i] - b[i] * side));
  }
  return e < 1e-12;
}

bool mirrors(const TransformDelta &delta)
{
  const double3 s = signsOf(delta);
  return s[0] < 0.0 || s[1] < 0.0 || s[2] < 0.0;
}

Linear mirrored(const TransformDelta &delta, const Linear &m)
{
  const double3 s = signsOf(delta);
  Linear r;
  for (int i = 0; i < 3; i++) {
    r.col[i] = scaleVector(delta, s, m.col[i]);
  }
  return r;
}

Status takeMirror(const Linear &x,
                  quatd &rotation,
                  double3 &scale,
                  bool &turned)
{
  /* Pairs of signs are a half turn. For a mirror with a determinant of -1, rotationScale adds the one
   * that is missing. */
  static const double kSigns[4][3] = {{1, 1, 1}, {-1, -1, 1}, {-1, 1, -1}, {1, -1, -1}};
  const quatd current = rotation.normalized();
  double best = -1.0;
  double3 factors;
  for (const double *f : kSigns) {
    const double3 signs(f[0], f[1], f[2]);
    solve::RotationScale rs;
    if (!solve::rotationScale(
            x.scaled(signs), Linear::identity(), current, double3(1, 1, 1), rs))
    {
      return unsolvable();
    }
    const double nearness = std::fabs(rs.rotation.dot(current));
    if (nearness > best + 1e-9) {
      best = nearness;
      turned = !sameRotation(rs.rotation, current);
      rotation = turned ? rs.rotation : current;
      for (int i = 0; i < 3; i++) {
        factors[i] = signs[i] * (rs.scale[i] < 0.0 ? -1.0 : 1.0);
      }
    }
  }
  scale = scale * factors;
  return Status();
}

quatd turnUnder(const TransformDelta &delta, const solve::ParentSolver &solver, const Linear &p)
{
  const double3 axis = delta.basis.normalized().rotate(delta.axis);
  const bool mirrored = unitOf(p.col[0]).cross(unitOf(p.col[1])).dot(unitOf(p.col[2])) < 0.0;
  return quatd::fromAxisAngle(solver.vector(axis), mirrored ? -delta.angle : delta.angle);
}

namespace {

template <class Model> Status addBuiltin(Runtime &rt, TransformTypeInfo info)
{
  TransformRegistry &reg = rt.ext<TransformRegistry>();
  if (reg.find(info.name.c_str())) {
    return Status();
  }
  const std::string impl = std::string(info.name.c_str()) + "#impl";
  CDCC_TRY((registerImpl<TransformModel, Model>(rt, impl)));
  Result<Owned<TransformModel>> model =
      own<TransformModel>(rt, impl, litestl::alloc::New<Model>("coredcc::TransformModel"));
  if (!model.ok()) {
    return model.status();
  }
  Result<const TransformType *> added = reg.add(std::move(info), std::move(*model));
  return added.ok() ? Status() : added.status();
}

} // namespace

} // namespace models

Status registerTransformTypes(Runtime &rt)
{
  CDCC_TRY(models::addBuiltin<models::Trs>(rt, models::trsInfo()));
  CDCC_TRY(models::addBuiltin<models::Fbx>(rt, models::fbxInfo()));
  return models::addBuiltin<models::Matrix>(rt, models::matrixInfo());
}

} // namespace coredcc
