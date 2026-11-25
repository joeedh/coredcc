#include "coredcc/graph/node_type.h"

#include "coredcc/api/change_bus.h"
#include "coredcc/base/assert.h"
#include "coredcc/base/text.h"
#include "coredcc/graph/api.h"

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

NodeTypeDef &NodeTypeDef::input(PropertyDef def)
{
  Input in;
  in.name = def.apiName();
  in.literalDef = int(literalDefs_.size());
  literalDefs_.append(std::move(def));
  inputs_.append(std::move(in));
  return *this;
}

NodeTypeDef &NodeTypeDef::payloadInput(std::string_view name, std::string_view socketType)
{
  Input in;
  in.name = lsString(name);
  in.payloadType = lsString(socketType);
  inputs_.append(std::move(in));
  return *this;
}

NodeTypeDef &NodeTypeDef::output(std::string_view name, std::string_view socketType)
{
  Output out;
  out.name = lsString(name);
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

/* ---- NodeTypeRegistry ---- */

NodeTypeRegistry::NodeTypeRegistry(Runtime *rt) : rt_(*rt)
{
  rt_.ext<SocketTypeRegistry>();
  detail::addGraphStructs(rt_);
  struct Special {
    const char *name, *uiName;
    NodeRole role;
  };
  const Special special[] = {
      {kGraphInputType, "Group Input", NodeRole::Input},
      {kGraphOutputType, "Group Output", NodeRole::Output},
      {kGraphGroupType, "Group", NodeRole::Group},
  };
  for (const Special &s : special) {
    NodeType *t = litestl::alloc::New<NodeType>("coredcc::NodeType");
    t->name = s.name;
    t->uiName = s.uiName;
    t->role = s.role;
    const bool ok = store(t).ok();
    CDCC_CHECK(ok, "registering a built-in node type");
  }
  detail::addBuiltinNodes(rt_, *this);
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
  type->stamp = stamp_;
  types_.append(type);
  byId_.add(id->value, type);
  return static_cast<const NodeType *>(type);
}

Result<const NodeType *> NodeTypeRegistry::add(NodeTypeDef def)
{
  CDCC_TRY(rt_.busyIfEvaluating("adding a node type"));
  const SocketTypeRegistry &sockets = rt_.ext<SocketTypeRegistry>();
  const std::string where = std::string("node type '") + def.name_.c_str() + "'";

  if (!isDottedName(def.name_.c_str())) {
    return invalid(where + ": the name must be dotted identifiers");
  }
  if (find(def.name_.c_str())) {
    return Status::error(StatusCode::AlreadyExists, (where + " is registered already").c_str());
  }
  CDCC_TRY(detail::checkNodeStructs(rt_, def.name_.c_str()));
  if (!def.kernel_) {
    return invalid(where + " has no kernel");
  }
  if (def.outputs_.size() == 0) {
    return invalid(where + " has no outputs");
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

  for (const NodeTypeDef::Input &in : def.inputs_) {
    CDCC_TRY(checkMemberName(where, in.name.c_str()));
    if (t->inputIndex(in.name.c_str()) >= 0) {
      return invalid(where + ": two inputs are named '" + in.name.c_str() + "'");
    }
    NodeInputDef out;
    out.name = in.name;
    out.literalDef = in.literalDef;
    if (in.literalDef >= 0) {
      const PropertyDef &p = def.literalDefs_[in.literalDef];
      CDCC_TRY(p.validate());
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
    t->outputs.append(NodeOutputDef{out.name, type});
  }

  for (NodeParamDef &p : def.params_) {
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
    t->params.append(std::move(p));
  }

  for (const NodeInputDef &d : t->inputs) {
    t->inputInfos.append(SocketInfo{d.name, d.name, d.type->name});
  }
  for (const NodeOutputDef &d : t->outputs) {
    t->outputInfos.append(SocketInfo{d.name, d.name, d.type->name});
  }
  t->literalDefs = std::move(def.literalDefs_);
  t->kernel = std::move(def.kernel_);
  guard.t = nullptr;
  return store(t);
}

const NodeType *NodeTypeRegistry::find(std::string_view name) const
{
  NodeType *const *t = byId_.lookup_ptr(TypeId::of(idName(name)).value);
  return t ? *t : nullptr;
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
