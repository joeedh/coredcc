#include "coredcc/graph/node_type.h"

#include "coredcc/api/change_bus.h"
#include "coredcc/base/assert.h"
#include "coredcc/base/text.h"
#include "coredcc/graph/api.h"
#include "coredcc/graph/builtin_nodes.h"
#include "coredcc/graph/def.h"
#include "coredcc/graph/foreach.h"

#include "litestl/util/alloc.h"

#include <string>

namespace coredcc {

namespace {

template <class V> int indexOfName(const V &list, std::string_view name)
{
  for (int i = 0; i < int(list.size()); i++) {
    if (name == list[i].name.c_str()) {
      return i;
    }
  }
  return -1;
}

Status invalid(const std::string &message)
{
  return Status::error(StatusCode::InvalidArgument, message.c_str());
}

bool isDottedName(std::string_view s)
{
  while (true) {
    const size_t dot = s.find('.');
    if (!isIdentifier(s.substr(0, dot))) {
      return false;
    }
    if (dot == std::string_view::npos) {
      return true;
    }
    s.remove_prefix(dot + 1);
  }
}

/* ApiRegistry refuses members with these names, and step 6 makes a struct member of every
 * input and param. */
Status checkMemberName(const std::string &where, const char *name)
{
  const std::string_view n(name);
  if (!isIdentifier(n)) {
    return invalid(where + ": '" + name + "' isn't an identifier");
  }
  if (n == "length" || n == "active") {
    return invalid(where + ": the name '" + name + "' is reserved");
  }
  return Status();
}

} // namespace

int NodeType::inputIndex(std::string_view inputName) const
{
  return indexOfName(inputs, inputName);
}

int NodeType::outputIndex(std::string_view outputName) const
{
  return indexOfName(outputs, outputName);
}

int NodeType::paramIndex(std::string_view paramName) const
{
  for (int i = 0; i < int(params.size()); i++) {
    if (paramName == params[i].def.apiName().c_str()) {
      return i;
    }
  }
  return -1;
}

/* ---- NodeTypeDef ---- */

NodeTypeDef::NodeTypeDef(std::string_view name) : name_(lsString(name)), uiName_(lsString(name)) {}

NodeTypeDef &NodeTypeDef::uiName(std::string_view s)
{
  uiName_ = lsString(s);
  return *this;
}

NodeTypeDef &NodeTypeDef::flags(NodeTypeFlags f)
{
  flags_ = f;
  return *this;
}

NodeTypeDef &NodeTypeDef::version(uint32_t v)
{
  version_ = v;
  return *this;
}

NodeTypeDef &NodeTypeDef::input(PropertyDef def)
{
  Input in;
  in.name = def.apiName();
  in.literalDef = int(literalDefs_.size());
  literalDefs_.append(std::move(def));
  inputs_.append(std::move(in));
  return *this;
}

NodeTypeDef &NodeTypeDef::payloadInput(std::string_view name,
                                       std::string_view socketType,
                                       std::string_view uiName)
{
  Input in;
  in.name = lsString(name);
  in.uiName = lsString(uiName);
  in.payloadType = lsString(socketType);
  inputs_.append(std::move(in));
  return *this;
}

NodeTypeDef &NodeTypeDef::output(std::string_view name,
                                 std::string_view socketType,
                                 std::string_view uiName)
{
  Output out;
  out.name = lsString(name);
  out.uiName = lsString(uiName);
  out.socketType = lsString(socketType);
  outputs_.append(std::move(out));
  return *this;
}

NodeTypeDef &NodeTypeDef::param(PropertyDef def, bool weak)
{
  params_.append(NodeParamDef{std::move(def), weak});
  return *this;
}

NodeTypeDef &NodeTypeDef::kernel(Owned<GraphKernel> kernel)
{
  kernel_ = std::move(kernel);
  return *this;
}

PropertyDef *NodeTypeDef::inputDef(std::string_view name)
{
  for (PropertyDef &p : literalDefs_) {
    if (name == p.apiName().c_str()) {
      return &p;
    }
  }
  return nullptr;
}

PropertyDef *NodeTypeDef::paramDef(std::string_view name)
{
  for (NodeParamDef &p : params_) {
    if (name == p.def.apiName().c_str()) {
      return &p.def;
    }
  }
  return nullptr;
}

bool NodeTypeDef::setSocketUiName(bool output, std::string_view name, std::string_view uiName)
{
  if (output) {
    const int i = indexOfName(outputs_, name);
    if (i >= 0) {
      outputs_[i].uiName = lsString(uiName);
    }
    return i >= 0;
  }
  const int i = indexOfName(inputs_, name);
  if (i >= 0) {
    inputs_[i].uiName = lsString(uiName);
    if (PropertyDef *p = inputDef(name)) {
      p->uiName(uiName);
    }
  }
  return i >= 0;
}

/* ---- NodeTypeRegistry ---- */

NodeTypeRegistry::NodeTypeRegistry(Runtime *rt) : rt_(*rt)
{
  const bool ok = rt_.ext<SocketTypeRegistry>().addPayloadType<Elements>().ok();
  CDCC_CHECK(ok, "registering the elements payload");
  detail::addGraphStructs(rt_);
  struct Special {
    const char *name, *uiName;
    NodeRole role;
  };
  const Special special[] = {
      {kGraphInputType, "Group Input", NodeRole::Input},
      {kGraphOutputType, "Group Output", NodeRole::Output},
      {kGraphGroupType, "Group", NodeRole::Group},
      {kGraphForeachType, "For Each", NodeRole::Foreach},
  };
  for (const Special &s : special) {
    NodeType *t = litestl::alloc::New<NodeType>("coredcc::NodeType");
    t->name = s.name;
    t->uiName = s.uiName;
    t->role = s.role;
    if (s.role == NodeRole::Foreach) {
      t->params.append(NodeParamDef{foreachMaxCachedParam()});
    }
    const bool stored = store(t).ok();
    CDCC_CHECK(stored, "registering a built-in node type");
  }
  detail::addValueNodes(rt_, *this);
  detail::addElementNodes(rt_, *this);
}

NodeTypeRegistry::~NodeTypeRegistry()
{
  for (NodeType *t : types_) {
    litestl::alloc::Delete(t);
  }
}

static std::string idName(std::string_view name)
{
  return "node:" + std::string(name);
}

NodeTypeDef NodeTypeDef::copy() const
{
  NodeTypeDef d(name_.c_str());
  d.uiName_ = uiName_;
  d.flags_ = flags_;
  d.version_ = version_;
  d.inputs_ = inputs_;
  d.outputs_ = outputs_;
  d.literalDefs_ = literalDefs_;
  d.params_ = params_;
  return d;
}

Result<const NodeType *> NodeTypeRegistry::store(NodeType *type)
{
  Result<TypeId> id = rt_.registerId<IdDomain::Type>(idName(type->name.c_str()));
  if (!id.ok()) {
    litestl::alloc::Delete(type);
    return id.status();
  }
  if (Status s = detail::addNodeStructs(rt_, *type); !s.ok()) {
    litestl::alloc::Delete(type);
    return s;
  }
  rt_.ext<ChangeBus>().bumpEpoch(); /* nodes of the type show as another struct now */
  stamp_ = rt_.touch();
  type->stamp = type->kernelStamp = stamp_;
  types_.append(type);
  byId_.add(id->value, type);
  for (GraphDef *d = defs_; d; d = d->nextDef_) {
    d->typeAdded(*type);
  }
  return static_cast<const NodeType *>(type);
}

Result<const NodeType *> NodeTypeRegistry::add(NodeTypeDef def)
{
  CDCC_TRY(rt_.busyIfEvaluating("adding a node type"));
  if (!def.kernel_) {
    return invalid(std::string("node type '") + def.name_.c_str() + "' has no kernel");
  }
  Result<NodeType *> type = build(def);
  if (!type.ok()) {
    return type.status();
  }
  (*type)->kernel = std::move(def.kernel_);
  return store(*type);
}

/* Whether a param of a replacement is that of the type. */
static bool sameParam(const NodeParamDef &a, const NodeParamDef &b)
{
  if (strcmp(a.def.apiName().c_str(), b.def.apiName().c_str()) != 0 ||
      a.def.kind() != b.def.kind() || a.weak != b.weak ||
      a.def.hasAccess(PropAccess::Optional) != b.def.hasAccess(PropAccess::Optional))
  {
    return false;
  }
  if (a.def.kind() == PropKind::Pointer && !(a.def.target()->target == b.def.target()->target)) {
    return false;
  }
  const EnumMeta *x = a.def.enumMeta(), *y = b.def.enumMeta();
  bool same = !x || x->items.size() == y->items.size();
  for (int i = 0; same && x && i < int(x->items.size()); i++) {
    same = x->items[i].value == y->items[i].value &&
           strcmp(x->items[i].key.c_str(), y->items[i].key.c_str()) == 0;
  }
  return same;
}

Status NodeTypeRegistry::checkReplace(const NodeTypeDef &def) const
{
  CDCC_TRY(rt_.busyIfEvaluating("replacing a kernel"));
  const NodeType *old = find(def.name_.c_str());
  if (!old) {
    return unknown(def.name_.c_str());
  }
  Result<NodeType *> made = build(def, true);
  if (!made.ok()) {
    return made.status();
  }
  const NodeType &type = **made;
  bool same = old->role == NodeRole::Kernel && old->version == type.version &&
              old->flags == type.flags && old->inputs.size() == type.inputs.size() &&
              old->outputs.size() == type.outputs.size() &&
              old->params.size() == type.params.size();
  for (int i = 0; same && i < int(type.inputs.size()); i++) {
    same = type.inputs[i].type == old->inputs[i].type &&
           old->inputIndex(type.inputs[i].name.c_str()) == i;
  }
  for (int i = 0; same && i < int(type.outputs.size()); i++) {
    same = type.outputs[i].type == old->outputs[i].type &&
           old->outputIndex(type.outputs[i].name.c_str()) == i;
  }
  for (int i = 0; same && i < int(type.params.size()); i++) {
    same = sameParam(type.params[i], old->params[i]);
  }
  litestl::alloc::Delete(*made);
  return same ? Status() :
                Status::error(StatusCode::FailedPrecondition,
                              (std::string("node type '") + def.name_.c_str() +
                               "': its sockets, params, flags or version are others; only its "
                               "kernel can be replaced")
                                  .c_str());
}

Status NodeTypeRegistry::replaceKernel(NodeTypeDef def)
{
  if (!def.kernel_) {
    return invalid(std::string("node type '") + def.name_.c_str() + "' has no kernel");
  }
  CDCC_TRY(checkReplace(def));
  NodeType *type = *byId_.lookup_ptr(TypeId::of(idName(def.name_.c_str())).value);
  type->kernel = std::move(def.kernel_);
  stamp_ = rt_.touch(); /* instances go by this, and foreach nodes */
  type->kernelStamp = stamp_;
  return Status();
}

Status NodeTypeRegistry::checkDef(const NodeTypeDef &def) const
{
  CDCC_TRY(rt_.busyIfEvaluating("adding a node type"));
  Result<NodeType *> type = build(def);
  if (type.ok()) {
    litestl::alloc::Delete(*type);
  }
  return type.status();
}

static litestl::util::string shownName(const litestl::util::string &name)
{
  return lsString(uiNameFromApiName(name.c_str()));
}

Result<NodeType *> NodeTypeRegistry::build(const NodeTypeDef &def, bool replaces) const
{
  const SocketTypeRegistry &sockets = rt_.ext<SocketTypeRegistry>();
  const std::string where = std::string("node type '") + def.name_.c_str() + "'";

  if (!isDottedName(def.name_.c_str())) {
    return invalid(where + ": the name must be dotted identifiers");
  }
  if (!replaces && find(def.name_.c_str())) {
    return Status::error(StatusCode::AlreadyExists, (where + " is registered already").c_str());
  }
  if (!replaces) {
    CDCC_TRY(detail::checkNodeStructs(rt_, def.name_.c_str()));
  }
  if (def.outputs_.size() == 0) {
    return invalid(where + " has no outputs");
  }
  if (def.version_ == 0) {
    return invalid(where + ": versions start at 1");
  }

  NodeType *t = litestl::alloc::New<NodeType>("coredcc::NodeType");
  struct Guard {
    NodeType *t;
    ~Guard()
    {
      litestl::alloc::Delete(t);
    }
  } guard{t};
  t->name = def.name_;
  t->uiName = def.uiName_;
  t->flags = def.flags_;
  t->version = def.version_;

  for (const NodeTypeDef::Input &in : def.inputs_) {
    CDCC_TRY(checkMemberName(where, in.name.c_str()));
    if (t->inputIndex(in.name.c_str()) >= 0) {
      return invalid(where + ": two inputs are named '" + in.name.c_str() + "'");
    }
    NodeInputDef out;
    out.name = in.name;
    out.uiName = in.uiName.size() > 0 ? in.uiName : shownName(in.name);
    out.literalDef = in.literalDef;
    if (in.literalDef >= 0) {
      const PropertyDef &p = def.literalDefs_[in.literalDef];
      CDCC_TRY(p.validate());
      if (p.uiName().size() > 0) {
        out.uiName = p.uiName();
      }
      if (p.isDynamic() || p.hasAccess(PropAccess::Optional)) {
        return invalid(where + ": input '" + in.name.c_str() +
                       "' is dynamic or Optional, which inputs can't be");
      }
      out.type = sockets.builtin(graphTypeFor(p.kind()));
      if (!out.type) {
        return invalid(where + ": input '" + in.name.c_str() +
                       "' has a kind sockets can't carry");
      }
      Result<GraphValue> v = GraphValue::fromProp(p.defaultValue());
      if (!v.ok() || !out.type->holds(*v)) {
        return invalid(where + ": input '" + in.name.c_str() + "' has no usable default");
      }
      out.defaultValue = std::move(v).value();
    }
    else {
      out.type = sockets.find(in.payloadType.c_str());
      if (!out.type) {
        return sockets.unknown(in.payloadType.c_str());
      }
      if (out.type->hasLiteral()) {
        return invalid(where + ": input '" + in.name.c_str() + "' isn't a payload");
      }
      out.defaultValue = out.type->defaultValue;
    }
    t->inputs.append(std::move(out));
  }

  for (const NodeTypeDef::Output &out : def.outputs_) {
    CDCC_TRY(checkMemberName(where, out.name.c_str()));
    if (t->outputIndex(out.name.c_str()) >= 0) {
      return invalid(where + ": two outputs are named '" + out.name.c_str() + "'");
    }
    const SocketType *type = sockets.find(out.socketType.c_str());
    if (!type) {
      return sockets.unknown(out.socketType.c_str());
    }
    t->outputs.append(
        NodeOutputDef{out.name, out.uiName.size() > 0 ? out.uiName : shownName(out.name), type});
  }

  for (const NodeParamDef &p : def.params_) {
    const char *name = p.def.apiName().c_str();
    CDCC_TRY(checkMemberName(where, name));
    if (t->paramIndex(name) >= 0) {
      return invalid(where + ": two params are named '" + name + "'");
    }
    CDCC_TRY(p.def.validate());
    if (p.def.isDynamic() || p.def.kind() == PropKind::Collection) {
      return invalid(where + ": param '" + name +
                     "' is dynamic or a Collection, which params can't be");
    }
    if (p.def.kind() == PropKind::Pointer && p.def.target()->target.isNull()) {
      return invalid(where + ": param '" + name + "' names no target struct");
    }
    if (p.weak && p.def.kind() != PropKind::Pointer) {
      return invalid(where + ": param '" + name + "' is weak, but isn't a pointer");
    }
    t->params.append(p);
  }

  for (const NodeInputDef &d : t->inputs) {
    t->inputInfos.append(SocketInfo{d.name, d.uiName, d.type->name});
  }
  for (const NodeOutputDef &d : t->outputs) {
    t->outputInfos.append(SocketInfo{d.name, d.uiName, d.type->name});
  }
  t->literalDefs = def.literalDefs_;
  guard.t = nullptr;
  return t;
}

const NodeType *NodeTypeRegistry::find(std::string_view name) const
{
  NodeType *const *t = byId_.lookup_ptr(TypeId::of(idName(name)).value);
  return t && name == (*t)->name.c_str() ? *t : nullptr; /* a file's name can have its id */
}

Status NodeTypeRegistry::unknown(std::string_view name) const
{
  Status s = Status::error(StatusCode::NotFound,
                           ("no node type '" + std::string(name) + "'").c_str());
  Suggester sg(name);
  for (const NodeType *t : types_) {
    sg.consider(t->name.c_str());
  }
  sg.addTo(s);
  return s;
}

Status NodeTypeRegistry::validate() const
{
  for (const NodeType *t : types_) {
    if (find(t->name.c_str()) != t || bool(t->kernel) != (t->role == NodeRole::Kernel)) {
      return Status::error(StatusCode::Internal,
                           (std::string("node type '") + t->name.c_str() +
                            "' isn't registered consistently")
                               .c_str());
    }
  }
  return Status();
}

} // namespace coredcc
