#include "coredcc/bind/graph_instance_facades.h"

#include "coredcc/api/resolve.h"
#include "coredcc/base/json_writer.h"
#include "coredcc/bind/detail.h"
#include "coredcc/bind/graph_detail.h"
#include "coredcc/graph/node_type.h"
#include "coredcc/graph/provider.h"
#include "coredcc/lib/library.h"

#include <chrono>
#include <map>
#include <string>

namespace coredcc::script {

using detail::error;
using detail::failed;
using detail::ok;
using detail::view;
namespace lbt = litestl::binding::types;

/* A socket's value for scripts. */
ValueResult detail::toScript(const char *what, const string &name, const GraphValue &v)
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
Result<GraphValue> detail::fromScript(const SocketType &type,
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

/* The graph block `path` names: itself, or through a pointer member. */
Result<BlockId> detail::graphAt(Context &ctx, const string &path)
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

/* ---- evaluating ---- */

namespace {

struct TimeLimit {
  using Clock = std::chrono::steady_clock;
  double milliseconds = 0;
  Clock::time_point start;
  bool over() const
  {
    return milliseconds > 0 &&
           std::chrono::duration<double, std::milli>(Clock::now() - start).count() >= milliseconds;
  }
};

} // namespace

struct ScriptGraph::Core {
  /** Shared with the evaluation in progress. Null once closed. */
  std::shared_ptr<GraphInstance> instance;
  Runtime *rt = nullptr;
  LibraryRef lib;
  BlockId graph;
  std::map<int64_t, SocketHandle> watches;
  int64_t lastWatch = 0;
  /** Shared with the instance's monitor. */
  std::shared_ptr<TimeLimit> limit = std::make_shared<TimeLimit>();
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

/* The same for calls that look at what the instance has. */
Result<std::shared_ptr<GraphInstance>> idleInstance(const ScriptGraph &graph, const char *what)
{
  Result<std::shared_ptr<GraphInstance>> inst = instanceOf(graph, what);
  if (inst.ok()) {
    CDCC_TRY(graph.core()->rt->busyIfEvaluating(what));
  }
  return inst;
}

/* An instance about to evaluate. */
Result<std::shared_ptr<GraphInstance>> startedInstance(const ScriptGraph &graph, const char *what)
{
  Result<std::shared_ptr<GraphInstance>> inst = idleInstance(graph, what);
  if (inst.ok()) {
    graph.core()->limit->start = TimeLimit::Clock::now();
  }
  return inst;
}

struct Watch {
  std::shared_ptr<GraphInstance> inst;
  SocketHandle socket;
};

Result<Watch> watchOf(const ScriptGraph &graph, const char *what, int64_t id)
{
  Result<std::shared_ptr<GraphInstance>> inst = idleInstance(graph, what);
  if (!inst.ok()) {
    return inst.status();
  }
  const auto at = graph.core()->watches.find(id);
  if (at == graph.core()->watches.end()) {
    return error(StatusCode::NotFound,
                 std::string(what) + ": there is no watch " + std::to_string(id));
  }
  return Watch{std::move(inst).value(), at->second};
}

/* Its value, or why it has none. */
Result<const GraphValue *> valueOf(const Watch &w, const char *what)
{
  if (const GraphValue *v = w.inst->value(w.socket)) {
    return v;
  }
  CDCC_TRY(w.inst->status(w.socket));
  return error(StatusCode::FailedPrecondition, std::string(what) + ": it wasn't evaluated yet");
}

TableResult tableOf(const GraphValue &v, const string &name)
{
  if (v.type() != GraphValue::Type::Payload ||
      (v.asPayload() && v.asPayload().type() != payloadTypeId<Elements>()))
  {
    return failed<TableResult>(
        error(StatusCode::TypeMismatch, std::string("'") + name.c_str() + "' isn't a table"));
  }
  return ok<TableResult>(ScriptTable(v.asPayload()));
}

/* `node` is a path, or empty for the interface. */
Result<SocketPath> socketOf(const string &node, const string &socket, bool input)
{
  if (node.size() == 0) {
    return SocketPath::output(view(socket));
  }
  Result<NodePath> path = NodePath::parse(view(node));
  if (!path.ok()) {
    return path.status();
  }
  return SocketPath::of(
      std::move(path).value(), view(socket), input ? SocketSide::Input : SocketSide::Output);
}

string errorText(const GraphError *e)
{
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

const char *stateName(NodeReport::State s)
{
  switch (s) {
    case NodeReport::State::NotEvaluated:
      return "NotEvaluated";
    case NodeReport::State::Ok:
      return "Ok";
    case NodeReport::State::Failed:
      return "Failed";
    case NodeReport::State::InputFailed:
      break;
  }
  return "InputFailed";
}

} // namespace

GraphResult graphInstance(Context &ctx, const string &graphPath)
{
  if (Status s = detail::kernelGate(ctx, "graphInstance"); !s.ok()) {
    return failed<GraphResult>(std::move(s));
  }
  Result<BlockId> graph = detail::graphAt(ctx, graphPath);
  if (!graph.ok()) {
    return failed<GraphResult>(graph.status());
  }
  Runtime &rt = ctx.runtime();
  auto core = std::make_shared<ScriptGraph::Core>();
  core->rt = &rt;
  core->lib = ctx.library()->ref();
  core->graph = *graph;
  core->instance = std::make_shared<GraphInstance>(&rt, ctx.library(), *graph);
  GraphMonitor monitor;
  monitor.cancelled = [limit = core->limit]() { return limit->over(); };
  if (Status s = core->instance->setMonitor(std::move(monitor)); !s.ok()) {
    return failed<GraphResult>(std::move(s));
  }
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
  Result<GraphValue> v = detail::fromScript(*type, name, value);
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
  Result<std::shared_ptr<GraphInstance>> inst = startedInstance(graph, "graphEvaluate");
  if (!inst.ok()) {
    return failed<ValueResult>(inst.status());
  }
  const string name = output;
  const SocketPath path = SocketPath::output(view(name));
  if (Status s = (*inst)->evaluate(path); !s.ok()) {
    return failed<ValueResult>(std::move(s));
  }
  return detail::toScript("output", name, *(*inst)->value(path));
}

string graphErrorText(const ScriptGraph &graph, const string &output)
{
  Result<std::shared_ptr<GraphInstance>> inst = instanceOf(graph, "graphErrorText");
  if (!inst.ok()) {
    return inst.status().toString();
  }
  return errorText((*inst)->error(SocketPath::output(view(output))));
}

Status graphSetInputTable(const ScriptGraph &graph, const string &name, const ScriptTable &table)
{
  Result<std::shared_ptr<GraphInstance>> inst = instanceOf(graph, "graphSetInputTable");
  if (!inst.ok()) {
    return inst.status();
  }
  CDCC_TRY(table.check());
  return (*inst)->setInput(view(name), GraphValue::ofPayload(table.ref()));
}

TableResult graphEvaluateTable(const ScriptGraph &graph, const string &output)
{
  Result<std::shared_ptr<GraphInstance>> inst = startedInstance(graph, "graphEvaluateTable");
  if (!inst.ok()) {
    return failed<TableResult>(inst.status());
  }
  const string name = output;
  const SocketPath path = SocketPath::output(view(name));
  if (Status s = (*inst)->evaluate(path); !s.ok()) {
    return failed<TableResult>(std::move(s));
  }
  return tableOf(*(*inst)->value(path), name);
}

/* ---- watched sockets ---- */

IntResult graphWatch(const ScriptGraph &graph, const string &node, const string &socket)
{
  Result<std::shared_ptr<GraphInstance>> inst = idleInstance(graph, "graphWatch");
  if (!inst.ok()) {
    return failed<IntResult>(inst.status());
  }
  Result<SocketPath> path = socketOf(node, socket, false);
  if (!path.ok()) {
    return failed<IntResult>(path.status());
  }
  Result<SocketHandle> h = (*inst)->prepare(*path);
  if (!h.ok()) {
    return failed<IntResult>(h.status());
  }
  ScriptGraph::Core &core = *graph.core();
  core.watches[++core.lastWatch] = *h;
  return ok<IntResult>(core.lastWatch);
}

Status graphUnwatch(const ScriptGraph &graph, int64_t watch)
{
  Result<Watch> w = watchOf(graph, "graphUnwatch", watch);
  if (!w.ok()) {
    return w.status();
  }
  graph.core()->watches.erase(watch);
  return w->inst->release(w->socket);
}

StringResult graphUpdate(const ScriptGraph &graph)
{
  Result<std::shared_ptr<GraphInstance>> inst = startedInstance(graph, "graphUpdate");
  if (!inst.ok()) {
    return failed<StringResult>(inst.status());
  }
  litestl::util::Vector<int64_t> ids;
  litestl::util::Vector<SocketHandle> sockets;
  for (const auto &[id, socket] : graph.core()->watches) {
    ids.append(id);
    sockets.append(socket);
  }
  litestl::util::Vector<int> moved;
  if (Status s = (*inst)->evaluate({sockets.data(), size_t(sockets.size())}, moved); !s.ok()) {
    return failed<StringResult>(std::move(s));
  }
  JsonWriter out;
  out.beginArray();
  for (int i : moved) {
    out.integer(ids[i]);
  }
  out.endArray();
  return ok<StringResult>(string(out.take().c_str()));
}

Status graphWatchStatus(const ScriptGraph &graph, int64_t watch)
{
  Result<Watch> w = watchOf(graph, "graphWatchStatus", watch);
  if (!w.ok()) {
    return w.status();
  }
  return valueOf(*w, "graphWatchStatus").status();
}

ValueResult graphWatchValue(const ScriptGraph &graph, int64_t watch)
{
  Result<Watch> w = watchOf(graph, "graphWatchValue", watch);
  if (!w.ok()) {
    return failed<ValueResult>(w.status());
  }
  Result<const GraphValue *> v = valueOf(*w, "graphWatchValue");
  return v.ok() ? detail::toScript("watch", string("value"), **v) :
                  failed<ValueResult>(v.status());
}

TableResult graphWatchTable(const ScriptGraph &graph, int64_t watch)
{
  Result<Watch> w = watchOf(graph, "graphWatchTable", watch);
  if (!w.ok()) {
    return failed<TableResult>(w.status());
  }
  Result<const GraphValue *> v = valueOf(*w, "graphWatchTable");
  return v.ok() ? tableOf(**v, string("value")) : failed<TableResult>(v.status());
}

ValueResult graphWatchLastGood(const ScriptGraph &graph, int64_t watch)
{
  Result<Watch> w = watchOf(graph, "graphWatchLastGood", watch);
  if (!w.ok()) {
    return failed<ValueResult>(w.status());
  }
  const GraphValue *v = w->inst->lastGood(w->socket);
  return v ? detail::toScript("watch", string("value"), *v) : ok<ValueResult>(PropValue());
}

IntResult graphWatchChangedAt(const ScriptGraph &graph, int64_t watch)
{
  Result<Watch> w = watchOf(graph, "graphWatchChangedAt", watch);
  return w.ok() ? ok<IntResult>(int64_t(w->inst->changedAt(w->socket))) :
                  failed<IntResult>(w.status());
}

BoolResult graphWatchIsCurrent(const ScriptGraph &graph, int64_t watch)
{
  Result<Watch> w = watchOf(graph, "graphWatchIsCurrent", watch);
  return w.ok() ? ok<BoolResult>(w->inst->isCurrent(w->socket)) : failed<BoolResult>(w.status());
}

string graphWatchErrorText(const ScriptGraph &graph, int64_t watch)
{
  Result<Watch> w = watchOf(graph, "graphWatchErrorText", watch);
  return w.ok() ? errorText(w->inst->error(w->socket)) : w.status().toString();
}

/* ---- looking inside ---- */

static Result<GraphValue> peekAt(const ScriptGraph &graph,
                                 const char *what,
                                 const string &node,
                                 const string &socket,
                                 bool input)
{
  Result<std::shared_ptr<GraphInstance>> inst = idleInstance(graph, what);
  if (!inst.ok()) {
    return inst.status();
  }
  Result<SocketPath> path = socketOf(node, socket, input);
  return path.ok() ? (*inst)->peek(*path) : Result<GraphValue>(path.status());
}

ValueResult graphPeek(const ScriptGraph &graph,
                      const string &node,
                      const string &socket,
                      bool input)
{
  Result<GraphValue> v = peekAt(graph, "graphPeek", node, socket, input);
  return v.ok() ? detail::toScript("socket", socket, *v) : failed<ValueResult>(v.status());
}

TableResult graphPeekTable(const ScriptGraph &graph,
                           const string &node,
                           const string &socket,
                           bool input)
{
  Result<GraphValue> v = peekAt(graph, "graphPeekTable", node, socket, input);
  return v.ok() ? tableOf(*v, socket) : failed<TableResult>(v.status());
}

StringResult graphReport(const ScriptGraph &graph, const string &node)
{
  Result<std::shared_ptr<GraphInstance>> inst = idleInstance(graph, "graphReport");
  if (!inst.ok()) {
    return failed<StringResult>(inst.status());
  }
  Result<NodePath> path = NodePath::parse(view(node));
  if (!path.ok()) {
    return failed<StringResult>(path.status());
  }
  const NodeReport r = (*inst)->report(*path);
  JsonWriter out;
  out.beginObject();
  out.key("state").string(stateName(r.state));
  out.key("current").boolean(r.current);
  out.key("runs").unsignedInteger(r.runs);
  out.key("seconds").number(r.seconds);
  out.key("totalSeconds").number(r.totalSeconds);
  out.key("elements").integer(r.elements);
  out.key("cachedElements").integer(r.cachedElements);
  out.key("memory").beginObject();
  out.key("frames").integer(r.memory.frames);
  out.key("kernels").integer(r.memory.kernels);
  out.key("elements").integer(r.memory.elements);
  out.key("bytes").unsignedInteger(r.memory.bytes);
  out.key("payloadBytes").unsignedInteger(r.memory.payloadBytes);
  out.endObject();
  if (!r.error.status.ok()) {
    out.key("error").beginObject();
    out.key("code").string(statusCodeName(r.error.status.code()));
    out.key("message").string(r.error.status.message());
    out.key("node").string(r.error.origin.toString());
    if (r.error.socket.size() > 0) {
      out.key("socket").string(r.error.socket.c_str());
    }
    out.endObject();
  }
  out.endObject();
  return ok<StringResult>(string(out.take().c_str()));
}

Status graphRetry(const ScriptGraph &graph)
{
  Result<std::shared_ptr<GraphInstance>> inst = instanceOf(graph, "graphRetry");
  return inst.ok() ? (*inst)->retry() : inst.status();
}

Status graphClear(const ScriptGraph &graph)
{
  Result<std::shared_ptr<GraphInstance>> inst = instanceOf(graph, "graphClear");
  return inst.ok() ? (*inst)->clear() : inst.status();
}

Status graphDrop(const ScriptGraph &graph, const string &node)
{
  Result<std::shared_ptr<GraphInstance>> inst = instanceOf(graph, "graphDrop");
  if (!inst.ok()) {
    return inst.status();
  }
  Result<NodePath> path = NodePath::parse(view(node));
  return path.ok() ? (*inst)->drop(*path) : path.status();
}

Status graphSetTimeLimit(const ScriptGraph &graph, double milliseconds)
{
  Result<std::shared_ptr<GraphInstance>> inst = idleInstance(graph, "graphSetTimeLimit");
  if (!inst.ok()) {
    return inst.status();
  }
  if (!(milliseconds >= 0)) {
    return error(StatusCode::InvalidArgument, "graphSetTimeLimit: the limit is below 0");
  }
  graph.core()->limit->milliseconds = milliseconds;
  return Status();
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
