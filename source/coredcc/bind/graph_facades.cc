#include "coredcc/bind/graph_facades.h"

#include "coredcc/api/change_bus.h"
#include "coredcc/api/ref_text.h"
#include "coredcc/base/iface/registry.h"
#include "coredcc/base/json_writer.h"
#include "coredcc/base/text.h"
#include "coredcc/bind/detail.h"
#include "coredcc/bind/foreign_command.h"
#include "coredcc/bind/graph_detail.h"
#include "coredcc/graph/commands.h"
#include "coredcc/graph/provider.h"
#include "coredcc/lib/library.h"

#include <span>
#include <string>

namespace coredcc::script {

using detail::error;
using detail::failed;
using detail::ok;
using detail::view;
namespace lbt = litestl::binding::types;
namespace lf = litestl::binding::foreign;

/* ---- node types ---- */

NodeTypeDef *newNodeTypeDef(const string &id)
{
  return litestl::alloc::New<NodeTypeDef>("coredcc::NodeTypeDef", detail::withoutNul(id));
}

Status nodeTypeDefUiName(NodeTypeDef &def, const string &s)
{
  def.uiName(view(s));
  return Status();
}

/* As detail::scriptProp, also of a Quat or Mat4. */
static Result<PropertyDef>
graphProp(const char *what, const string &name, const PropValue &defaultValue)
{
  const PropValue::Type type = defaultValue.type();
  const bool quat = type == PropValue::Type::Quat;
  if (!quat && type != PropValue::Type::Mat4) {
    return detail::scriptProp(what, name, defaultValue, false);
  }
  Result<PropertyDef> named =
      detail::scriptProp(what, name, PropValue::ofBool(false), false);
  if (!named.ok()) {
    return named;
  }
  PropertyDef p =
      quat ? PropertyDef::quatProp(view(name)) : PropertyDef::mat4Prop(view(name));
  p.defaultValue(defaultValue);
  return p;
}

Status
nodeTypeDefInput(NodeTypeDef &def, const string &name, const PropValue &defaultValue)
{
  Result<PropertyDef> p = graphProp("nodeTypeDefInput", name, defaultValue);
  if (!p.ok()) {
    return p.status();
  }
  def.input(std::move(p).value());
  return Status();
}

Status
nodeTypeDefPayloadInput(NodeTypeDef &def, const string &name, const string &socketType)
{
  def.payloadInput(view(name), view(socketType));
  return Status();
}

Status nodeTypeDefOutput(NodeTypeDef &def, const string &name, const string &socketType)
{
  def.output(view(name), view(socketType));
  return Status();
}

Status
nodeTypeDefParam(NodeTypeDef &def, const string &name, const PropValue &defaultValue)
{
  Result<PropertyDef> p = graphProp("nodeTypeDefParam", name, defaultValue);
  if (!p.ok()) {
    return p.status();
  }
  def.param(std::move(p).value());
  return Status();
}

static Status notThere(const char *what, const string &name)
{
  return error(StatusCode::NotFound,
               std::string(what) + ": the def has no '" + name.c_str() + "'");
}

Status nodeTypeDefItemParam(NodeTypeDef &def,
                            const string &name,
                            bool flags,
                            int64_t defaultValue)
{
  Result<PropertyDef> named =
      detail::scriptProp("nodeTypeDefItemParam", name, PropValue::ofBool(false), false);
  if (!named.ok()) {
    return named.status();
  }
  PropertyDef p =
      flags ? PropertyDef::flagsProp(view(name)) : PropertyDef::enumProp(view(name));
  p.defaultValue(flags ? PropValue::ofFlags(uint64_t(defaultValue))
                       : PropValue::ofEnum(defaultValue));
  def.param(std::move(p));
  return Status();
}

Status nodeTypeDefItem(NodeTypeDef &def,
                       const string &param,
                       const string &key,
                       int64_t value,
                       const string &uiName)
{
  PropertyDef *p = def.paramDef(view(param));
  if (!p) {
    return notThere("nodeTypeDefItem", param);
  }
  if (!p->enumMeta()) {
    return error(StatusCode::TypeMismatch,
                 std::string("nodeTypeDefItem: '") + param.c_str() + "' has no items");
  }
  p->item(view(key), value, view(uiName));
  return Status();
}

Status nodeTypeDefPointerParam(NodeTypeDef &def,
                               const string &name,
                               const string &target,
                               bool weak)
{
  Result<PropertyDef> named = detail::scriptProp(
      "nodeTypeDefPointerParam", name, PropValue::ofBool(false), false);
  if (!named.ok()) {
    return named.status();
  }
  if (target.size() == 0) {
    return error(StatusCode::InvalidArgument,
                 "nodeTypeDefPointerParam: there is no struct ''");
  }
  def.param(PropertyDef::pointerProp(view(name), StructId::of(view(target))), weak);
  return Status();
}

Status nodeTypeDefSocketName(NodeTypeDef &def,
                             bool output,
                             const string &name,
                             const string &uiName)
{
  return def.setSocketUiName(output, view(name), view(uiName))
             ? Status()
             : notThere("nodeTypeDefSocketName", name);
}

Status nodeTypeDefRange(NodeTypeDef &def, const string &name, double min, double max)
{
  PropertyDef *p = def.paramDef(view(name));
  p = p ? p : def.inputDef(view(name));
  if (!p) {
    return notThere("nodeTypeDefRange", name);
  }
  if (!p->numeric()) {
    return error(StatusCode::TypeMismatch,
                 std::string("nodeTypeDefRange: '") + name.c_str() + "' is no number");
  }
  p->range(min, max);
  return Status();
}

Status nodeTypeDefOutputAdapter(NodeTypeDef &def)
{
  def.flags(NodeTypeFlags::OutputAdapter);
  return Status();
}

static Status
registerWith(Runtime &rt, const NodeTypeDef &def, lf::Handle kernel, const string &slots)
{
  const std::string name = def.name().c_str();
  if (const std::string_view ns = detail::reservedNamespace(
          name, {"core", "graph", "scene", "math", "vec", "mat4", "value", "elements"});
      !ns.empty())
  {
    return error(StatusCode::InvalidArgument,
                 "node type '" + name + "': the '" + std::string(ns) +
                     ".' namespace is reserved for coredcc (as are 'core.', 'graph.', "
                     "'scene.', 'math.', 'vec.', 'mat4.', 'value.' and 'elements.', in "
                     "any case)");
  }
  NodeTypeRegistry &types = rt.ext<NodeTypeRegistry>();
  CDCC_TRY(types.checkDef(def));
  CDCC_TRY(foreignImplementable(coredcc::detail::ifaceDesc<GraphKernel>()));
  const std::vector<std::string> names = detail::splitSlots(slots);
  std::vector<const char *> present;
  for (const std::string &n : names) {
    present.push_back(n.c_str());
  }
  const std::string impl = name + "#kernel";
  CDCC_TRY(registerForeign<GraphKernel>(
      rt,
      impl,
      rt.ext<ForeignBindings>().dispatcher(),
      nullptr,
      std::span<const char *const>(present.data(), present.size())));
  GraphKernel k = query<GraphKernel>(
      rt, TypeId::of(impl), reinterpret_cast<void *>(uintptr_t(kernel)));
  if (!k) {
    return error(StatusCode::Internal,
                 "node type '" + name + "': its kernel's table is gone");
  }
  NodeTypeDef copy = def.copy();
  /* ForeignBindings owns the handle, so this and the registry may go in either order. */
  copy.kernel(Owned<GraphKernel>(k, [](void *, void *) {}, nullptr));
  return types.add(std::move(copy)).status();
}

Status registerForeignNodeType(Context &ctx,
                               const NodeTypeDef &def,
                               uint32_t kernel,
                               const string &slots)
{
  if (kernel == 0) {
    return error(StatusCode::InvalidArgument,
                 "registerForeignNodeType: no kernel handle");
  }
  Runtime &rt = ctx.runtime();
  Status s = registerWith(rt, def, kernel, slots);
  if (s.ok()) {
    rt.ext<ForeignBindings>().keepHandle(kernel);
  } else {
    lf::release(kernel);
  }
  return s;
}

/* The type's kernel is replaced; its old handle in `old`. */
static Status replaceWith(Runtime &rt,
                          const NodeTypeDef &def,
                          lf::Handle kernel,
                          const string &slots,
                          lf::Handle &old)
{
  NodeTypeRegistry &types = rt.ext<NodeTypeRegistry>();
  CDCC_TRY(types.checkReplace(def));
  const NodeType *type = types.find(def.name().c_str());
  const IfaceTable *table = type->kernel.get().table();
  if (table->dispatcher != rt.ext<ForeignBindings>().dispatcher()) {
    return error(StatusCode::FailedPrecondition,
                 std::string("node type '") + def.name().c_str() + "' isn't a script's");
  }
  const std::vector<std::string> names = detail::splitSlots(slots);
  if (names.size() != 1 || names[0] != "evaluate") {
    return error(StatusCode::TypeMismatch,
                 std::string("node type '") + def.name().c_str() +
                     "': a kernel has the slot 'evaluate', and no other");
  }
  old = lf::Handle(reinterpret_cast<uintptr_t>(type->kernel.get().self()));
  const std::string impl = std::string(def.name().c_str()) + "#kernel";
  GraphKernel k = query<GraphKernel>(
      rt, TypeId::of(impl), reinterpret_cast<void *>(uintptr_t(kernel)));
  if (!k) {
    return error(StatusCode::Internal,
                 std::string("node type '") + def.name().c_str() +
                     "': its kernel's table is gone");
  }
  NodeTypeDef copy = def.copy();
  copy.kernel(Owned<GraphKernel>(k, [](void *, void *) {}, nullptr));
  return types.replaceKernel(std::move(copy));
}

Status replaceForeignNodeType(Context &ctx,
                              const NodeTypeDef &def,
                              uint32_t kernel,
                              const string &slots)
{
  if (kernel == 0) {
    return error(StatusCode::InvalidArgument, "replaceForeignNodeType: no kernel handle");
  }
  Runtime &rt = ctx.runtime();
  lf::Handle old = 0;
  Status s = replaceWith(rt, def, kernel, slots, old);
  if (s.ok()) {
    rt.ext<ForeignBindings>().keepHandle(kernel);
    rt.ext<ForeignBindings>().dropHandle(old);
  } else {
    lf::release(kernel);
  }
  return s;
}

/* ---- in a kernel ---- */

template <class List>
static Status noSuch(const char *what, const string &name, const List &list)
{
  Status s = error(StatusCode::NotFound,
                   std::string("the node has no ") + what + " '" + name.c_str() + "'");
  Suggester sg(view(name));
  for (const auto &item : list) {
    sg.consider(item.name.c_str());
  }
  sg.addTo(s);
  return s;
}

ValueResult nodeInput(NodeExecContext &node, const string &name)
{
  const GraphValue *v = node.input(view(name));
  return v ? detail::toScript("input", name, *v)
           : failed<ValueResult>(noSuch("input", name, node.type().inputs));
}

static Status noParam(const NodeExecContext &node, const string &name)
{
  Status s = error(StatusCode::NotFound,
                   std::string("the node has no param '") + name.c_str() + "'");
  Suggester sg(view(name));
  for (const NodeParamDef &p : node.type().params) {
    sg.consider(p.def.apiName().c_str());
  }
  sg.addTo(s);
  return s;
}

ValueResult nodeParam(NodeExecContext &node, const string &name)
{
  const PropValue *v = node.param(view(name));
  return v ? ok<ValueResult>(*v) : failed<ValueResult>(noParam(node, name));
}

Status nodeSetOutput(NodeExecContext &node, const string &name, const PropValue &value)
{
  const int i = node.type().outputIndex(view(name));
  if (i < 0) {
    return noSuch("output", name, node.type().outputs);
  }
  Result<GraphValue> v = detail::fromScript(*node.type().outputs[i].type, name, value);
  return v.ok() ? node.setOutput(i, std::move(v).value()) : v.status();
}

Status nodePass(NodeExecContext &node, const string &input, const string &output)
{
  const GraphValue *v = node.input(view(input));
  if (!v) {
    return noSuch("input", input, node.type().inputs);
  }
  const int i = node.type().outputIndex(view(output));
  return i >= 0 ? node.setOutput(i, *v) : noSuch("output", output, node.type().outputs);
}

BoolResult nodeIsLinked(NodeExecContext &node, const string &name)
{
  return node.input(view(name))
             ? ok<BoolResult>(node.isLinked(view(name)))
             : failed<BoolResult>(noSuch("input", name, node.type().inputs));
}

StringResult nodeUseBlock(NodeExecContext &node, const string &param, uint32_t aspects)
{
  if (!node.param(view(param))) {
    return failed<StringResult>(noParam(node, param));
  }
  const BlockUse use =
      node.paramBlock(view(param), aspects ? ChangeAspects(aspects) : kAllAspects);
  const Datablock *b =
      use.handle && node.library() ? node.library()->get(use.handle) : nullptr;
  return ok<StringResult>(
      string(b && use.data ? formatBlockId(node.runtime(), b->id()).c_str() : ""));
}

bool nodeCancelled(NodeExecContext &node)
{
  return node.cancelled();
}

Status nodeProgress(NodeExecContext &node, double fraction, const string &text)
{
  node.progress(fraction, view(text));
  return Status();
}

TableResult nodeInputTable(NodeExecContext &node, const string &name)
{
  const int i = node.type().inputIndex(view(name));
  if (i < 0) {
    return failed<TableResult>(noSuch("input", name, node.type().inputs));
  }
  if (node.type().inputs[i].type->payload != payloadTypeId<Elements>()) {
    return failed<TableResult>(
        error(StatusCode::TypeMismatch,
              std::string("input '") + name.c_str() + "' isn't a table"));
  }
  return ok<TableResult>(ScriptTable(node.input(i).asPayload()));
}

Status
nodeSetOutputTable(NodeExecContext &node, const string &name, const ScriptTable &table)
{
  const int i = node.type().outputIndex(view(name));
  if (i < 0) {
    return noSuch("output", name, node.type().outputs);
  }
  CDCC_TRY(table.check());
  return node.setOutput(i, GraphValue::ofPayload(table.ref()));
}

/* ---- editing ---- */

static PropValue pointerTo(BlockId graph)
{
  StableRef ref;
  ref.owner = OwnerKey::ofBlock(graph);
  ref.type = StructId::of(kGraphDefType);
  return PropValue::ofPointer(ref);
}

struct Arg {
  const char *name;
  PropValue value;
};

/* Runs a graph command, with the graph at `path` (unless null) as its `graph`. */
static Result<PropertyBag> runGraph(Context &ctx,
                                    const char *what,
                                    const char *command,
                                    const string *path,
                                    std::initializer_list<Arg> args)
{
  CDCC_TRY(detail::slotGate(ctx, what, {"exec"}));
  if (!ctx.executor()) {
    return error(StatusCode::FailedPrecondition,
                 std::string(what) + ": the context has no executor");
  }
  const CommandRegistry &reg = ctx.runtime().ext<CommandRegistry>();
  const CommandDef *def = reg.find(command);
  if (!def) {
    return reg.unknown(command);
  }
  PropertyBag in = def->newInputs();
  if (path) {
    Result<BlockId> graph = detail::graphAt(ctx, *path);
    if (!graph.ok()) {
      return graph.status();
    }
    CDCC_TRY(in.set("graph", pointerTo(*graph)));
  }
  for (const Arg &a : args) {
    CDCC_TRY(in.set(a.name, a.value));
  }
  return detail::runCommand(ctx, *def, std::move(in));
}

static IntResult intOutput(const Result<PropertyBag> &out, const char *name)
{
  return out.ok() ? ok<IntResult>(out->get(name)->asInt())
                  : failed<IntResult>(out.status());
}

ValueResult newGraph(Context &ctx, const string &name)
{
  Result<PropertyBag> out = runGraph(ctx,
                                     "newGraph",
                                     kNewGraphCommand,
                                     nullptr,
                                     {{"name", PropValue::ofString(view(name))}});
  return out.ok() ? ok<ValueResult>(*out->get("graph"))
                  : failed<ValueResult>(out.status());
}

IntResult graphAddNode(
    Context &ctx, const string &graphPath, const string &type, double x, double y)
{
  return intOutput(
      runGraph(ctx,
               "graphAddNode",
               kAddGraphNodeCommand,
               &graphPath,
               {{"type", PropValue::ofString(view(type))}, {"position", makeVec2(x, y)}}),
      "node");
}

Status graphRemoveNode(Context &ctx, const string &graphPath, int64_t node)
{
  return runGraph(ctx,
                  "graphRemoveNode",
                  kRemoveGraphNodeCommand,
                  &graphPath,
                  {{"node", makeInt(node)}})
      .status();
}

IntResult graphLink(Context &ctx,
                    const string &graphPath,
                    int64_t from,
                    const string &fromSocket,
                    int64_t to,
                    const string &toSocket)
{
  return intOutput(runGraph(ctx,
                            "graphLink",
                            kAddGraphLinkCommand,
                            &graphPath,
                            {{"from", makeInt(from)},
                             {"fromSocket", PropValue::ofString(view(fromSocket))},
                             {"to", makeInt(to)},
                             {"toSocket", PropValue::ofString(view(toSocket))}}),
                   "link");
}

Status graphUnlink(Context &ctx, const string &graphPath, int64_t link)
{
  return runGraph(ctx,
                  "graphUnlink",
                  kRemoveGraphLinkCommand,
                  &graphPath,
                  {{"link", makeInt(link)}})
      .status();
}

IntResult graphAddInterface(Context &ctx,
                            const string &graphPath,
                            bool output,
                            const string &name,
                            const string &type)
{
  return intOutput(runGraph(ctx,
                            "graphAddInterface",
                            kAddGraphInterfaceCommand,
                            &graphPath,
                            {{"side", PropValue::ofEnum(output ? 1 : 0)},
                             {"name", PropValue::ofString(view(name))},
                             {"type", PropValue::ofString(view(type))}}),
                   "socket");
}

/* ---- looking at a graph ---- */

/* The def at a path, for what reads it. */
static Result<GraphDef *> defAt(Context &ctx, const char *what, const string &path)
{
  CDCC_TRY(detail::kernelGate(ctx, what));
  Result<BlockId> graph = detail::graphAt(ctx, path);
  if (!graph.ok()) {
    return graph.status();
  }
  return graphDefOf(*ctx.library(), ctx.library()->find(*graph));
}

IntResult graphNewId(Context &ctx, const string &graphPath)
{
  Result<GraphDef *> def = defAt(ctx, "graphNewId", graphPath);
  return def.ok() ? ok<IntResult>((*def)->newId()) : failed<IntResult>(def.status());
}

StringResult graphDiagnostics(Context &ctx, const string &graphPath)
{
  Result<GraphDef *> def = defAt(ctx, "graphDiagnostics", graphPath);
  if (!def.ok()) {
    return failed<StringResult>(def.status());
  }
  JsonWriter out;
  out.beginArray();
  for (const LinkDiagnostic &d : (*def)->diagnostics()) {
    out.beginObject();
    out.key("link").integer(d.link.value);
    out.key("code").string(statusCodeName(d.status.code()));
    out.key("message").string(d.status.message());
    out.key("interior").boolean(d.interior);
    out.endObject();
  }
  out.endArray();
  return ok<StringResult>(string(out.take().c_str()));
}

} // namespace coredcc::script
