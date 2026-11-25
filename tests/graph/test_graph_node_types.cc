#include "coredcc/graph/node_type.h"

#include "test_util.h"

#include <cstring>
#include <string>

using namespace coredcc;

test_init;

namespace {

struct Mesh {
  int verts = 0;
};

} // namespace

template <> struct coredcc::PayloadTraits<Mesh> {
  static constexpr const char *name = "test.Mesh";
};

namespace {

Status scale(NodeExecContext &ctx)
{
  const Mesh *m = ctx.input("mesh")->payload<Mesh>();
  const double f = ctx.input("factor")->asFloat();
  return ctx.setOutput(
      "out", GraphValue::ofPayload(makePayload(Mesh{int((m ? m->verts : 1) * f)})));
}

Status other(NodeExecContext &)
{
  return Status();
}

Owned<GraphKernel> kernel(Runtime &rt)
{
  Result<Owned<GraphKernel>> k = makeKernel<FnKernel<scale>>(rt, "test.scale#kernel");
  test_assert(k.ok());
  return std::move(k).value();
}

/* A valid type, for tests that break one thing. */
NodeTypeDef good(Runtime &rt, const char *name)
{
  NodeTypeDef def(name);
  def.input(PropertyDef::floatProp("factor").range(0, 10).defaultValue(
                PropValue::ofFloat(2)))
      .payloadInput("mesh", "test.Mesh")
      .output("out", "test.Mesh")
      .param(PropertyDef::enumProp("mode").item("FAST", 0).item("EXACT", 1))
      .kernel(kernel(rt));
  return def;
}

void testRegistration()
{
  Runtime rt;
  NodeTypeRegistry &reg = rt.ext<NodeTypeRegistry>();
  test_assert(rt.validateAll().ok());
  test_assert(rt.ext<SocketTypeRegistry>().addPayloadType<Mesh>().ok());

  const uint64_t before = rt.lastEdit();
  Result<const NodeType *> t = reg.add(std::move(good(rt, "test.scale").uiName("Scale")));
  test_assert(t.ok() && rt.lastEdit() > before && (*t)->stamp == rt.lastEdit());
  test_assert(reg.find("test.scale") == *t && (*t)->role == NodeRole::Kernel);
  test_assert(strcmp((*t)->uiName.c_str(), "Scale") == 0);
  test_assert((*t)->inputs.size() == 2 &&
              (*t)->inputs[0].type->valueType == GraphValue::Type::Float);
  test_assert((*t)->inputs[0].defaultValue == GraphValue::ofFloat(2));
  test_assert((*t)->literal(0)->numeric()->hardMax == 10 && (*t)->literal(1) == nullptr);
  test_assert((*t)->inputs[1].defaultValue == GraphValue::ofPayload(PayloadRef()));
  test_assert((*t)->inputIndex("mesh") == 1 && (*t)->inputIndex("out") == -1);
  test_assert((*t)->outputIndex("out") == 0 && (*t)->paramIndex("mode") == 0);
  test_assert((*t)->paramIndex("factor") == -1);

  Result<const NodeType *> u = reg.add(good(rt, "test.other"));
  test_assert(u.ok() && (*u)->stamp != (*t)->stamp);

  /* Everything a registration refuses. Nothing is registered, and nothing is an edit. */
  const int count = reg.size();
  const uint64_t edit = rt.lastEdit();
  const StructId target = StructId::of("Mesh");
  auto bad = [&](const char *what, StatusCode code, NodeTypeDef def) {
    Result<const NodeType *> r = reg.add(std::move(def));
    if (r.ok() || r.status().code() != code) {
      printf("registration accepted, or gave another error: %s\n", what);
    }
    test_assert(!r.ok() && r.status().code() == code);
  };
  const StatusCode kInvalid = StatusCode::InvalidArgument;
  bad("again", StatusCode::AlreadyExists, good(rt, "test.scale"));
  for (const char *name :
       {"", ".", "a.", ".a", "a..b", "a b", "a.1", "a/b", "#a", "@a.b"})
  {
    bad(name, kInvalid, good(rt, name));
  }
  bad("no kernel", kInvalid, std::move(NodeTypeDef("test.x").output("o", "float")));
  bad("no output", kInvalid, std::move(NodeTypeDef("test.x").kernel(kernel(rt))));
  bad("input length",
      kInvalid,
      std::move(good(rt, "test.x").input(PropertyDef::floatProp("length"))));
  bad("input active",
      kInvalid,
      std::move(good(rt, "test.x").payloadInput("active", "test.Mesh")));
  bad("input name",
      kInvalid,
      std::move(good(rt, "test.x").payloadInput("a b", "test.Mesh")));
  bad("twin input",
      kInvalid,
      std::move(good(rt, "test.x").input(PropertyDef::intProp("mesh"))));
  bad("enum input",
      kInvalid,
      std::move(good(rt, "test.x").input(PropertyDef::enumProp("e").item("A", 0))));
  bad("pointer input",
      kInvalid,
      std::move(good(rt, "test.x").input(PropertyDef::pointerProp("p", target))));
  bad("bad range",
      kInvalid,
      std::move(good(rt, "test.x").input(PropertyDef::floatProp("r").range(2, 1))));
  bad("optional input",
      kInvalid,
      std::move(good(rt, "test.x")
                    .input(PropertyDef::floatProp("o").access(PropAccess::Optional))));
  bad("dynamic input",
      kInvalid,
      std::move(good(rt, "test.x")
                    .input(PropertyDef::floatProp("d").dynamic(
                        [](PropertyDef &, const void *) {}))));
  bad("unknown payload",
      StatusCode::NotFound,
      std::move(good(rt, "test.x").payloadInput("m", "test.Mush")));
  bad("payload input of a value type",
      kInvalid,
      std::move(good(rt, "test.x").payloadInput("m", "float")));
  bad("unknown output",
      StatusCode::NotFound,
      std::move(good(rt, "test.x").output("o", "flaot")));
  bad("twin output", kInvalid, std::move(good(rt, "test.x").output("out", "float")));
  bad("output length", kInvalid, std::move(good(rt, "test.x").output("length", "float")));
  bad("output name", kInvalid, std::move(good(rt, "test.x").output("", "float")));
  bad("param length",
      kInvalid,
      std::move(good(rt, "test.x").param(PropertyDef::intProp("length"))));
  bad("param name",
      kInvalid,
      std::move(good(rt, "test.x").param(PropertyDef::intProp("a.b"))));
  bad("twin param",
      kInvalid,
      std::move(good(rt, "test.x").param(PropertyDef::intProp("mode"))));
  bad("enum without items",
      kInvalid,
      std::move(good(rt, "test.x").param(PropertyDef::enumProp("e"))));
  bad("collection param",
      kInvalid,
      std::move(good(rt, "test.x").param(PropertyDef::collectionProp("c", target))));
  bad("dynamic param",
      kInvalid,
      std::move(good(rt, "test.x")
                    .param(PropertyDef::floatProp("d").dynamic(
                        [](PropertyDef &, const void *) {}))));
  bad("pointer without a target",
      kInvalid,
      std::move(good(rt, "test.x").param(PropertyDef::pointerProp("p", StructId()))));
  bad("weak value",
      kInvalid,
      std::move(good(rt, "test.x").param(PropertyDef::intProp("w"), true)));
  {
    EvaluationScope scope(rt);
    bad("evaluating", StatusCode::Busy, good(rt, "test.x"));
  }
  test_assert(reg.size() == count && !reg.find("test.x") && rt.lastEdit() == edit);

  NodeTypeDef more = good(rt, "test.x");
  more.param(PropertyDef::pointerProp("p", target), true)
      .param(PropertyDef::floatProp("maybe").access(PropAccess::Optional));
  Result<const NodeType *> x = reg.add(std::move(more));
  test_assert(x.ok() && (*x)->params[1].weak && !(*x)->params[0].weak);
  /* One kernel class may serve two types, but a name is one class. */
  test_assert((makeKernel<FnKernel<other>>(rt, "test.scale#kernel").status().code() ==
               StatusCode::AlreadyExists));
  test_assert(reg.unknown("test.scael").suggestions().size() > 0);
  test_assert(rt.validateAll().ok());
}

struct Blocks final : ExecBlocks {
  int uses = 0;
  int queries = 0;
  ChangeAspects aspects = 0;
  BlockUse useBlock(BlockId, ChangeAspects a) override
  {
    uses++;
    aspects = a;
    return BlockUse{BlockHandle{1, 1}, this};
  }
  IfaceRef queryBlock(BlockHandle, InterfaceId) override
  {
    queries++;
    return IfaceRef();
  }
};

void testExecContext()
{
  Runtime rt;
  NodeTypeRegistry &reg = rt.ext<NodeTypeRegistry>();
  test_assert(rt.ext<SocketTypeRegistry>().addPayloadType<Mesh>().ok());
  NodeTypeDef def = good(rt, "test.scale");
  def.output("count", "int").param(PropertyDef::pointerProp("block", StructId::of("S")));
  const NodeType *type = *reg.add(std::move(def));

  const GraphValue factor = GraphValue::ofFloat(3);
  const GraphValue mesh = GraphValue::ofPayload(makePayload(Mesh{2}));
  const NodeExecContext::Input inputs[] = {{&factor, false}, {&mesh, true}};
  const PropValue params[] = {PropValue::ofEnum(1), PropValue()};
  const BlockId blocks[] = {BlockId(), BlockId{ClientId::of("c"), LocalId{1, 2}}};
  int user = 0;
  Blocks host;
  NodeExecContext::Setup setup;
  setup.rt = &rt;
  setup.type = type;
  setup.inputs = inputs;
  setup.params = params;
  setup.paramBlocks = blocks;
  setup.blocks = &host;
  setup.userData = &user;
  NodeExecContext ctx(setup);

  test_assert(&ctx.type() == type && &ctx.runtime() == &rt && ctx.userData() == &user);
  test_assert(ctx.library() == nullptr && ctx.inputCount() == 2);
  test_assert(&ctx.input(0) == &factor && ctx.input("mesh") == &mesh &&
              !ctx.input("out"));
  test_assert(!ctx.isLinked(0) && ctx.isLinked(1) && ctx.isLinked("mesh"));
  test_assert(!ctx.isLinked("factor") && !ctx.isLinked("no") && !ctx.isLinked(9));
  test_assert(ctx.param("mode")->asEnum() == 1 && !ctx.param("no"));
  test_assert(ctx.param("block")->isNone());
  test_assert(ctx.paramBlock("block").data == &host && host.uses == 1);
  test_assert(host.aspects == kAllAspects);
  test_assert(!ctx.paramBlock("mode").handle && !ctx.paramBlock("no").handle &&
              host.uses == 1);
  test_assert(!ctx.useBlock(BlockId()).handle && host.uses == 1);
  test_assert(ctx.useBlock(blocks[1]).handle && host.uses == 2 &&
              host.aspects == kAllAspects);
  test_assert(ctx.useBlock(blocks[1], 4).handle && host.aspects == 4);
  test_assert(ctx.paramBlock("block", 2).handle && host.aspects == 2);
  test_assert(!ctx.queryBlockRaw(BlockHandle{1, 1}, InterfaceId::of("x")) &&
              host.queries == 1);

  /* Nothing set, then one of two, then both. */
  test_assert(ctx.checkOutputs().code() == StatusCode::Internal);
  test_assert(type->kernel.get().evaluate(ctx).ok());
  test_assert(ctx.outputs()[0].payload<Mesh>()->verts == 6);
  test_assert(ctx.checkOutputs().code() == StatusCode::Internal);
  test_assert(ctx.setOutput("count", GraphValue::ofInt(4)).ok() &&
              ctx.checkOutputs().ok());

  /* A wrong type fails the node, even if the output was set before. */
  test_assert(ctx.setOutput("count", GraphValue::ofFloat(4)).code() ==
              StatusCode::TypeMismatch);
  test_assert(ctx.outputs()[1] == GraphValue::ofInt(4));
  test_assert(ctx.checkOutputs().code() == StatusCode::Internal);
  NodeExecContext other(setup);
  test_assert(other.setOutput(0, mesh).ok() &&
              other.setOutput(1, GraphValue::ofInt(1)).ok());
  test_assert(other.checkOutputs().ok());
  test_assert(other.setOutput("none", mesh).code() == StatusCode::NotFound);
  test_assert(other.checkOutputs().code() == StatusCode::Internal);
}

/* Runs a built-in type with these inputs and params. */
litestl::util::Vector<GraphValue> run(Runtime &rt,
                                      const char *name,
                                      std::initializer_list<GraphValue> values,
                                      PropValue param = PropValue())
{
  const NodeType *t = rt.ext<NodeTypeRegistry>().find(name);
  litestl::util::Vector<NodeExecContext::Input> inputs;
  for (const GraphValue &v : values) {
    inputs.append({&v, false});
  }
  const BlockId none;
  NodeExecContext::Setup setup;
  setup.rt = &rt;
  setup.type = t;
  setup.inputs = {inputs.data(), size_t(inputs.size())};
  setup.params = {&param, size_t(t->params.size())};
  setup.paramBlocks = {&none, size_t(t->params.size())};
  NodeExecContext ctx(setup);
  test_assert(t->kernel.get().evaluate(ctx).ok() && ctx.checkOutputs().ok());
  return ctx.outputs();
}

void testBuiltins()
{
  Runtime rt;
  NodeTypeRegistry &reg = rt.ext<NodeTypeRegistry>();
  test_assert(reg.find("graph.input")->role == NodeRole::Input);
  test_assert(reg.find("graph.output")->role == NodeRole::Output);
  test_assert(reg.find("graph.group")->role == NodeRole::Group);

  const double ops[][2] = {
      {0, 9}, {1, 5}, {2, 14}, {3, 3.5}, {4, 49}, {5, 2}, {6, 7}, {7, 1}};
  for (const auto &o : ops) {
    const auto out = run(rt,
                         "math.float",
                         {GraphValue::ofFloat(7), GraphValue::ofFloat(2)},
                         PropValue::ofEnum(int64_t(o[0])));
    test_assert(out[0] == GraphValue::ofFloat(o[1]));
  }
  const PropertyDef &op = reg.find("math.float")->params[0].def;
  test_assert(int(op.enumMeta()->items.size()) == int(std::size(ops)));

  test_assert(run(rt, "value.string", {}, PropValue::ofString("hi"))[0] ==
              GraphValue::ofString("hi"));
  test_assert(run(rt, "value.mat4", {}, PropValue::identityMat4())[0] ==
              GraphValue::identityMat4());
  for (const char *type :
       {"bool", "int", "float", "vec2", "vec3", "vec4", "mat4", "quat", "string"})
  {
    const std::string name = std::string("value.") + type;
    const NodeType *t = reg.find(name);
    test_assert(t && strcmp(t->outputs[0].type->name.c_str(), type) == 0);
    test_assert(run(rt, name.c_str(), {}, t->params[0].def.defaultValue())[0] ==
                t->outputs[0].type->defaultValue);
  }
  test_assert(
      run(rt,
          "vec.combine",
          {GraphValue::ofFloat(1), GraphValue::ofFloat(2), GraphValue::ofFloat(3)})[0] ==
      GraphValue::ofVec3(1, 2, 3));
  const auto parts = run(rt, "vec.separate", {GraphValue::ofVec3(4, 5, 6)});
  test_assert(parts[0] == GraphValue::ofFloat(4) && parts[2] == GraphValue::ofFloat(6));
  test_assert(parts[1] == GraphValue::ofFloat(5));

  /* A translation by (1, 2, 3) after a scale by 2. Not symmetric, so the order shows. */
  const double tr[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 1, 2, 3, 1};
  const double sc[16] = {2, 0, 0, 0, 0, 2, 0, 0, 0, 0, 2, 0, 0, 0, 0, 1};
  const double want[16] = {2, 0, 0, 0, 0, 2, 0, 0, 0, 0, 2, 0, 1, 2, 3, 1};
  const auto type = GraphValue::Type::Mat4;
  test_assert(
      run(rt,
          "mat4.multiply",
          {GraphValue::ofComponents(type, tr), GraphValue::ofComponents(type, sc)})[0] ==
      GraphValue::ofComponents(type, want));
}

} // namespace

int main()
{
  testRegistration();
  testExecContext();
  testBuiltins();
  return test_end();
}
