#include "coredcc/bind/graph_facades.h"

#include "coredcc/api/resolve.h"
#include "coredcc/base/iface/registry.h"
#include "coredcc/base/text.h"
#include "coredcc/bind/detail.h"
#include "coredcc/bind/foreign_command.h"
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

NodeTypeDef *newNodeType(const string &id)
{
  return litestl::alloc::New<NodeTypeDef>("coredcc::NodeTypeDef", view(id));
}

Status nodeTypeUiName(NodeTypeDef &def, const string &s)
{
  def.uiName(view(s));
  return Status();
}

Status nodeTypeInput(NodeTypeDef &def, const string &name, const PropValue &defaultValue)
{
  Result<PropertyDef> p = detail::scriptProp("nodeTypeInput", name, defaultValue, false);
  if (!p.ok()) {
    return p.status();
  }
  def.input(std::move(p).value());
  return Status();
}

Status nodeTypePayloadInput(NodeTypeDef &def, const string &name, const string &socketType)
{
  def.payloadInput(view(name), view(socketType));
  return Status();
}

Status nodeTypeOutput(NodeTypeDef &def, const string &name, const string &socketType)
{
  def.output(view(name), view(socketType));
  return Status();
}

Status nodeTypeParam(NodeTypeDef &def, const string &name, const PropValue &defaultValue)
{
  Result<PropertyDef> p = detail::scriptProp("nodeTypeParam", name, defaultValue, false);
  if (!p.ok()) {
    return p.status();
  }
  def.param(std::move(p).value());
  return Status();
}

static Status registerWith(Runtime &rt,
                           const NodeTypeDef &def,
                           lf::Handle kernel,
                           const string &slots)
{
  const std::string name = def.name().c_str();
  if (name.rfind("core.", 0) == 0 || name.rfind("graph.", 0) == 0) {
    return error(StatusCode::InvalidArgument,
                 "node type '" + name + "': the 'core.' and 'graph.' namespaces are reserved "
                                        "for coredcc");
  }
  NodeTypeRegistry &types = rt.ext<NodeTypeRegistry>();
  CDCC_TRY(types.check(def));
  CDCC_TRY(foreignImplementable(coredcc::detail::ifaceDesc<GraphKernel>()));
  const std::vector<std::string> names = detail::splitSlots(slots);
  std::vector<const char *> present;
  for (const std::string &n : names) {
    present.push_back(n.c_str());
  }
  const std::string impl = name + "#kernel";
  CDCC_TRY(registerForeign<GraphKernel>(rt,
                                        impl,
                                        rt.ext<ForeignBindings>().dispatcher(),
                                        nullptr,
                                        std::span<const char *const>(present.data(),
                                                                     present.size())));
  GraphKernel k = query<GraphKernel>(
      rt, TypeId::of(impl), reinterpret_cast<void *>(uintptr_t(kernel)));
  if (!k) {
    return error(StatusCode::Internal, "node type '" + name + "': its kernel's table is gone");
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
    return error(StatusCode::InvalidArgument, "registerForeignNodeType: no kernel handle");
  }
  Runtime &rt = ctx.runtime();
  Status s = registerWith(rt, def, kernel, slots);
  if (s.ok()) {
    rt.ext<ForeignBindings>().keepHandle(kernel);
  }
  else {
    lf::release(kernel);
  }
  return s;
}

/* ---- in a kernel ---- */

template <class List> static Status noSuch(const char *what, const string &name, const List &list)
{
  Status s = error(StatusCode::NotFound, std::string("the node has no ") + what + " '" +
                                             name.c_str() + "'");
  Suggester sg(view(name));
  for (const auto &item : list) {
    sg.consider(item.name.c_str());
  }
  sg.addTo(s);
  return s;
}

/* A socket's value for scripts. */
static ValueResult toScript(const char *what, const string &name, const GraphValue &v)
{
  if (v.type() == GraphValue::Type::Payload) {
    return failed<ValueResult>(error(StatusCode::Unsupported,
                                     std::string(what) + " '" + name.c_str() +
                                         "' is a payload, which scripts can't read (nodePass "
                                         "passes it on)"));
  }
  return ok<ValueResult>(v.toProp());
}

/* A script's value for a socket. */
static Result<GraphValue> fromScript(const SocketType &type,
                                     const string &name,
                                     const PropValue &value)
{
  if (!type.hasLiteral()) {
    return error(StatusCode::Unsupported, std::string("'") + name.c_str() + "' is a payload (" +
                                              type.name.c_str() + "), which scripts can't make");
  }
  Result<PropValue> v = coerce(type.literalDef(view(name)), value);
  return v.ok() ? GraphValue::fromProp(*v) : Result<GraphValue>(v.status());
}

ValueResult nodeInput(NodeExecContext &node, const string &name)
{
  const GraphValue *v = node.input(view(name));
  return v ? toScript("input", name, *v) :
             failed<ValueResult>(noSuch("input", name, node.type().inputs));
}

ValueResult nodeParam(NodeExecContext &node, const string &name)
{
  if (const PropValue *v = node.param(view(name))) {
    return ok<ValueResult>(*v);
  }
  Status s = error(StatusCode::NotFound, std::string("the node has no param '") + name.c_str() +
                                             "'");
  Suggester sg(view(name));
  for (const NodeParamDef &p : node.type().params) {
    sg.consider(p.def.apiName().c_str());
  }
  sg.addTo(s);
  return failed<ValueResult>(std::move(s));
}

Status nodeOutput(NodeExecContext &node, const string &name, const PropValue &value)
{
  const int i = node.type().outputIndex(view(name));
  if (i < 0) {
    return noSuch("output", name, node.type().outputs);
  }
  Result<GraphValue> v = fromScript(*node.type().outputs[i].type, name, value);
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

/* ---- editing ---- */

/* The graph block `path` names: itself, or through a pointer member. */
static Result<BlockId> graphAt(Context &ctx, const string &path)
{
  Result<ResolvedPath> r = resolve(ctx, view(path));
  if (!r.ok()) {
    return r.status();
  }
  StableRef ref;
  if (r->kind == ResolvedPath::Kind::Struct) {
    ref = r->canonical;
  }
  else if (r->kind == ResolvedPath::Kind::Member && r->member->def().kind() == PropKind::Pointer) {
    Result<PropValue> v = coredcc::getValue(ctx, view(path));
    if (!v.ok()) {
      return v.status();
    }
    ref = *v->tryPointer();
  }
  const Library *lib = ctx.library();
  const bool block = ref.owner.kind == OwnerKey::Kind::Block && ref.key.size() == 0;
  if (!block || !lib || !graphDefOf(*lib, lib->find(ref.owner.block))) {
    return error(StatusCode::InvalidArgument, std::string("'") + path.c_str() + "' isn't a graph");
  }
  return ref.owner.block;
}

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
    Result<BlockId> graph = graphAt(ctx, *path);
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
  return out.ok() ? ok<IntResult>(out->get(name)->asInt()) : failed<IntResult>(out.status());
}

ValueResult graphCreate(Context &ctx, const string &name)
{
  Result<PropertyBag> out = runGraph(
      ctx, "graphCreate", kGraphCreateCommand, nullptr, {{"name", makeString(name)}});
  return out.ok() ? ok<ValueResult>(*out->get("graph")) : failed<ValueResult>(out.status());
}

IntResult graphAddNode(Context &ctx,
                       const string &graphPath,
                       const string &type,
                       double x,
                       double y)
{
  return intOutput(runGraph(ctx, "graphAddNode", kGraphNodeAddCommand, &graphPath,
                            {{"type", makeString(type)}, {"position", makeVec2(x, y)}}),
                   "node");
}

Status graphRemoveNode(Context &ctx, const string &graphPath, int64_t node)
{
  return runGraph(ctx, "graphRemoveNode", kGraphNodeRemoveCommand, &graphPath,
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
  return intOutput(runGraph(ctx, "graphLink", kGraphLinkCommand, &graphPath,
                            {{"from", makeInt(from)},
                             {"fromSocket", makeString(fromSocket)},
                             {"to", makeInt(to)},
                             {"toSocket", makeString(toSocket)}}),
                   "link");
}

Status graphUnlink(Context &ctx, const string &graphPath, int64_t link)
{
  return runGraph(ctx, "graphUnlink", kGraphUnlinkCommand, &graphPath, {{"link", makeInt(link)}})
      .status();
}

IntResult graphAddInterface(Context &ctx,
                            const string &graphPath,
                            bool output,
                            const string &name,
                            const string &type)
{
  return intOutput(runGraph(ctx, "graphAddInterface", kGraphInterfaceAddCommand, &graphPath,
                            {{"side", PropValue::ofEnum(output ? 1 : 0)},
                             {"name", makeString(name)},
                             {"type", makeString(type)}}),
                   "socket");
}

/* ---- evaluating ---- */

struct ScriptGraph::Core {
  /** Shared with the evaluation in progress. Null once closed. */
  std::shared_ptr<GraphInstance> instance;
  Runtime *rt = nullptr;
  LibraryRef lib;
  BlockId graph;
};

namespace {

/* The Runtime's script instances, which it takes along: a script's GC may drop the last
 * handle only after the Runtime is gone. */
class ScriptGraphs {
 public:
  static constexpr const char *kExtName = "coredcc.script.ScriptGraphs";
  explicit ScriptGraphs(Runtime *rt)
  {
    rt->ext<NodeTypeRegistry>(); /* made first, so it goes after the instances */
  }
  ~ScriptGraphs()
  {
    for (const std::weak_ptr<ScriptGraph::Core> &w : cores_) {
      if (std::shared_ptr<ScriptGraph::Core> c = w.lock()) {
        c->instance.reset();
      }
    }
  }
  void add(const std::shared_ptr<ScriptGraph::Core> &core)
  {
    std::erase_if(cores_, [](const auto &w) { return w.expired(); });
    cores_.push_back(core);
  }
  int count() const
  {
    int n = 0;
    for (const std::weak_ptr<ScriptGraph::Core> &w : cores_) {
      const std::shared_ptr<ScriptGraph::Core> c = w.lock();
      n += c && c->instance;
    }
    return n;
  }

 private:
  std::vector<std::weak_ptr<ScriptGraph::Core>> cores_;
};

Result<std::shared_ptr<GraphInstance>> instanceOf(const ScriptGraph &graph, const char *what)
{
  if (!graph.core() || !graph.core()->instance) {
    return error(StatusCode::FailedPrecondition,
                 std::string(what) + ": the graph instance was closed");
  }
  return graph.core()->instance;
}

} // namespace

GraphResult graphInstance(Context &ctx, const string &graphPath)
{
  if (Status s = detail::kernelGate(ctx, "graphInstance"); !s.ok()) {
    return failed<GraphResult>(std::move(s));
  }
  Result<BlockId> graph = graphAt(ctx, graphPath);
  if (!graph.ok()) {
    return failed<GraphResult>(graph.status());
  }
  Runtime &rt = ctx.runtime();
  auto core = std::make_shared<ScriptGraph::Core>();
  core->rt = &rt;
  core->lib = ctx.library()->ref();
  core->graph = *graph;
  core->instance = std::make_shared<GraphInstance>(&rt, ctx.library(), *graph);
  rt.ext<ScriptGraphs>().add(core);
  return ok<GraphResult>(ScriptGraph(std::move(core)));
}

Status graphSetInput(const ScriptGraph &graph, const string &name, const PropValue &value)
{
  Result<std::shared_ptr<GraphInstance>> inst = instanceOf(graph, "graphSetInput");
  if (!inst.ok()) {
    return inst.status();
  }
  const ScriptGraph::Core &core = *graph.core();
  const Library *lib = core.lib.get();
  const GraphDef *def = lib ? graphDefOf(*lib, lib->find(core.graph)) : nullptr;
  const InterfaceSocket *s = def ? def->interfaceSocket(SocketSide::Input, view(name)) : nullptr;
  const SocketType *type =
      s ? core.rt->ext<SocketTypeRegistry>().find(s->typeName.c_str()) : nullptr;
  if (!type) { /* the instance says which of these is wrong */
    return (*inst)->setInput(view(name), GraphValue::ofBool(false));
  }
  Result<GraphValue> v = fromScript(*type, name, value);
  return v.ok() ? (*inst)->setInput(view(name), std::move(v).value()) : v.status();
}

Status graphClearInput(const ScriptGraph &graph, const string &name)
{
  Result<std::shared_ptr<GraphInstance>> inst = instanceOf(graph, "graphClearInput");
  return inst.ok() ? (*inst)->setInput(view(name), GraphValue()) : inst.status();
}

ValueResult graphEvaluate(const ScriptGraph &graph, const string &output)
{
  /* Kept here: a kernel may close the graph, or drop the script's last handle. */
  Result<std::shared_ptr<GraphInstance>> inst = instanceOf(graph, "graphEvaluate");
  if (!inst.ok()) {
    return failed<ValueResult>(inst.status());
  }
  const string name = output;
  const SocketPath path = SocketPath::output(view(name));
  if (Status s = (*inst)->evaluate(path); !s.ok()) {
    return failed<ValueResult>(std::move(s));
  }
  return toScript("output", name, *(*inst)->value(path));
}

string graphErrorText(const ScriptGraph &graph, const string &output)
{
  Result<std::shared_ptr<GraphInstance>> inst = instanceOf(graph, "graphErrorText");
  if (!inst.ok()) {
    return inst.status().toString();
  }
  const GraphError *e = (*inst)->error(SocketPath::output(view(output)));
  if (!e) {
    return string();
  }
  std::string text = e->status.toString().c_str();
  if (e->origin.node) {
    text += " (node " + e->origin.toString();
    text += e->socket.size() > 0 ? std::string(", input '") + e->socket.c_str() + "')" : ")";
  }
  return string(text.c_str());
}

void graphClose(const ScriptGraph &graph)
{
  if (graph.core()) {
    const std::shared_ptr<GraphInstance> last = std::move(graph.core()->instance);
    graph.core()->instance.reset();
  }
}

int32_t graphInstanceCount(Context &ctx)
{
  const ScriptGraphs *graphs = ctx.runtime().findExt<ScriptGraphs>();
  return graphs ? graphs->count() : 0;
}

const lbt::Struct<ScriptGraph> *ScriptGraph::defineBindings()
{
  static lbt::Struct<ScriptGraph> *st = nullptr;
  if (!st) {
    litestl::alloc::PermanentGuard guard;
    st = new lbt::Struct<ScriptGraph>("coredcc::ScriptGraph", sizeof(ScriptGraph));
    BIND_STRUCT_COPY_CONSTRUCTOR(st);
  }
  return st;
}

CDCC_RESULT_BINDINGS(GraphResult)

} // namespace coredcc::script
