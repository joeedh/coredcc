/* The built-in node types with kernels: constants and a little math. */
#include "coredcc/graph/builtin_nodes.h"

#include <cmath>
#include <string>

namespace coredcc {

namespace {

Status constant(NodeExecContext &ctx)
{
  Result<GraphValue> v = GraphValue::fromProp(*ctx.param("value"));
  if (!v.ok()) {
    return v.status();
  }
  return ctx.setOutput(0, std::move(v).value());
}

enum class FloatOp : int {
  Add,
  Subtract,
  Multiply,
  Divide,
  Power,
  Minimum,
  Maximum,
  Modulo
};

Status mathFloat(NodeExecContext &ctx)
{
  const double a = ctx.input(0).asFloat();
  const double b = ctx.input(1).asFloat();
  double r = 0;
  switch (FloatOp(ctx.param("op")->asEnum())) {
  case FloatOp::Add:
    r = a + b;
    break;
  case FloatOp::Subtract:
    r = a - b;
    break;
  case FloatOp::Multiply:
    r = a * b;
    break;
  case FloatOp::Divide:
    r = a / b;
    break;
  case FloatOp::Power:
    r = std::pow(a, b);
    break;
  case FloatOp::Minimum:
    r = std::fmin(a, b);
    break;
  case FloatOp::Maximum:
    r = std::fmax(a, b);
    break;
  case FloatOp::Modulo:
    r = std::fmod(a, b);
    break;
  }
  return ctx.setOutput(0, GraphValue::ofFloat(r));
}

Status vecCombine(NodeExecContext &ctx)
{
  return ctx.setOutput(0,
                       GraphValue::ofVec3(ctx.input(0).asFloat(),
                                          ctx.input(1).asFloat(),
                                          ctx.input(2).asFloat()));
}

Status vecSeparate(NodeExecContext &ctx)
{
  const double *v = ctx.input(0).components();
  for (int i = 0; i < 3; i++) {
    CDCC_TRY(ctx.setOutput(i, GraphValue::ofFloat(v[i])));
  }
  return Status();
}

Status mat4Multiply(NodeExecContext &ctx)
{
  const double *a = ctx.input(0).components();
  const double *b = ctx.input(1).components();
  double r[16];
  for (int col = 0; col < 4; col++) {
    for (int row = 0; row < 4; row++) {
      double sum = 0;
      for (int k = 0; k < 4; k++) {
        sum += a[k * 4 + row] * b[col * 4 + k];
      }
      r[col * 4 + row] = sum;
    }
  }
  return ctx.setOutput(0, GraphValue::ofComponents(GraphValue::Type::Mat4, r));
}

} // namespace

void detail::addValueNodes(Runtime &rt, NodeTypeRegistry &reg)
{
  const SocketTypeRegistry &sockets = rt.ext<SocketTypeRegistry>();
  for (const char *type :
       {"bool", "int", "float", "vec2", "vec3", "vec4", "mat4", "quat", "string"})
  {
    const std::string name = std::string("value.") + type;
    NodeTypeDef def(name);
    def.uiName(type).param(sockets.find(type)->literalDef("value")).output("value", type);
    addBuiltin<constant>(rt, reg, def, name);
  }

  NodeTypeDef math("math.float");
  math.uiName("Math")
      .input(PropertyDef::floatProp("a"))
      .input(PropertyDef::floatProp("b"))
      .param(PropertyDef::enumProp("op")
                 .item("ADD", int(FloatOp::Add))
                 .item("SUBTRACT", int(FloatOp::Subtract))
                 .item("MULTIPLY", int(FloatOp::Multiply))
                 .item("DIVIDE", int(FloatOp::Divide))
                 .item("POWER", int(FloatOp::Power))
                 .item("MINIMUM", int(FloatOp::Minimum))
                 .item("MAXIMUM", int(FloatOp::Maximum))
                 .item("MODULO", int(FloatOp::Modulo)))
      .output("value", "float");
  addBuiltin<mathFloat>(rt, reg, math, "math.float");

  NodeTypeDef combine("vec.combine");
  combine.uiName("Combine")
      .input(PropertyDef::floatProp("x"))
      .input(PropertyDef::floatProp("y"))
      .input(PropertyDef::floatProp("z"))
      .output("vector", "vec3");
  addBuiltin<vecCombine>(rt, reg, combine, "vec.combine");

  NodeTypeDef separate("vec.separate");
  separate.uiName("Separate")
      .input(PropertyDef::vecProp("vector", 3))
      .output("x", "float")
      .output("y", "float")
      .output("z", "float");
  addBuiltin<vecSeparate>(rt, reg, separate, "vec.separate");

  NodeTypeDef mul("mat4.multiply");
  mul.uiName("Multiply Matrices")
      .input(PropertyDef::mat4Prop("a"))
      .input(PropertyDef::mat4Prop("b"))
      .output("matrix", "mat4");
  addBuiltin<mat4Multiply>(rt, reg, mul, "mat4.multiply");
}

} // namespace coredcc
