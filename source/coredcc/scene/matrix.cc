#include "coredcc/scene/models.h"

#include "litestl/binding/binding.h"
#include "litestl/util/alloc.h"

namespace coredcc::models {

using solve::Linear;

TransformState Matrix::evaluate(const EvalCtx &ctx, const TransformState &parent) const
{
  Linear local;
  for (int i = 0; i < 3; i++) {
    local.col[i] = readVec3(ctx, matrix_transform::kColumns + i * 3);
  }
  const double3 t = readVec3(ctx, matrix_transform::kTranslation);
  double3 same, scale;
  quatd rotation;
  litestl::math::decomposeTrs(solve::affine(local, t), same, rotation, scale);
  return childState(parent, Linear::of(parent.world) * local, t, rotation, scale);
}

Status Matrix::setWorld(const EvalCtx &,
                        const mat4d &target,
                        const TransformState &parent,
                        SetWorldMode,
                        ChannelBlock &out) const
{
  solve::ParentSolver solver;
  if (!solver.set(parent.world)) {
    return singularParent();
  }
  const Linear local = solver.linear(Linear::of(target));
  for (int i = 0; i < 3; i++) {
    writeVec3(out, matrix_transform::kColumns + i * 3, local.col[i]);
  }
  writeVec3(out, matrix_transform::kTranslation, solver.point(solve::translationOf(target)));
  return Status();
}

TransformTypeInfo matrixInfo()
{
  TransformTypeInfo info;
  info.name = kMatrixTransform;
  info.uiName = "Matrix";
  const char *const names[] = {"columnX", "columnY", "columnZ"};
  for (int i = 0; i < 3; i++) {
    info.channels.append(PropertyDef::vecProp(names[i], 3).defaultValue(
        PropValue::ofVec3(i == 0 ? 1.0 : 0.0, i == 1 ? 1.0 : 0.0, i == 2 ? 1.0 : 0.0)));
  }
  info.channels.append(PropertyDef::vecProp("translation", 3)
                           .subtype(PropSubtype::Distance)
                           .unit(UnitFamily::Length));
  return info;
}

litestl::binding::types::Struct<Matrix> *Matrix::defineBindings()
{
  namespace lbt = litestl::binding::types;
  static lbt::Struct<Matrix> *st = nullptr;
  if (!st) {
    litestl::alloc::PermanentGuard guard;
    st = new lbt::Struct<Matrix>("coredcc::models::Matrix", sizeof(Matrix));
    BIND_STRUCT_METHOD(st, evaluate, MARGS("ctx", "parent"));
    BIND_STRUCT_METHOD(st, setWorld, MARGS("current", "target", "parent", "mode", "out"));
  }
  return st;
}

} // namespace coredcc::models
