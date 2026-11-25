/* GraphDef's edits. Each is checked completely before it changes anything. */
#include "coredcc/base/text.h"
#include "coredcc/graph/def.h"
#include "coredcc/graph/provider.h"
#include "coredcc/lib/library.h"

#include <cstring>
#include <string>

namespace coredcc {

using litestl::util::string;
using litestl::util::Vector;

namespace {

Status makeError(StatusCode code, const std::string &message)
{
  return Status::error(code, message.c_str());
}

bool is(const string &a, const char *b)
{
  return strcmp(a.c_str(), b) == 0;
}

bool isGroupType(const string &name)
{
  return is(name, kGraphGroupType) || is(name, kGraphForeachType);
}

bool hasRefs(const NodeExtras &extras)
{
  for (const BlockEntry &e : extras) {
    if (e.refs.size() > 0) {
      return true;
    }
  }
  return false;
}

int indexOfSocket(const SocketInfos &list, std::string_view key)
{
  for (int i = 0; i < int(list.size()); i++) {
    if (key == list[i].key.c_str()) {
      return i;
    }
  }
  return -1;
}

Status noSocket(const GraphNode &n,
                const SocketInfos &list,
                const char *kind,
                std::string_view key)
{
  Status s = makeError(StatusCode::NotFound,
                       "node '" + std::string(n.typeName().c_str()) + "' has no " + kind + " '" +
                           std::string(key) + "'");
  Suggester sg(key);
  for (const SocketInfo &i : list) {
    sg.consider(i.key.c_str());
  }
  sg.addTo(s);
  return s;
}

/* Sets, replaces or (on None) removes a param. */
void store(NodeValues &list, std::string_view name, PropValue value, uint64_t version)
{
  for (int i = 0; i < int(list.size()); i++) {
    if (name == list[i].name.c_str()) {
      if (value.isNone()) {
        list.remove_at(i);
      }
      else {
        list[i].value = std::move(value);
        list[i].version = version;
      }
      return;
    }
  }
  if (!value.isNone()) {
    list.append(NodeValue{lsString(name), std::move(value), version});
  }
}

/* The literal of a name and type, in use or not. */
NodeValue *findLiteral(NodeValues &list, std::string_view name, const SocketType &type)
{
  for (NodeValue &v : list) {
    if (name == v.name.c_str()) {
      Result<GraphValue> g = GraphValue::fromProp(v.value);
      if (g.ok() && type.holds(*g)) {
        return &v;
      }
    }
  }
  return nullptr;
}

} // namespace

/* ---- nodes ---- */

Result<NodeId> GraphDef::addNode(std::string_view type)
{
  CDCC_TRY(busy("addNode"));
  const NodeTypeRegistry &types = rt_.ext<NodeTypeRegistry>();
  const NodeType *t = types.find(type);
  if (!t) {
    return types.unknown(type);
  }
  NodeState state;
  state.id = NodeId{newId()};
  state.typeName = t->name;
  CDCC_TRY(insertNode(state));
  return state.id;
}

Status GraphDef::insertNode(const NodeState &state)
{
  if (isGroupType(state.typeName) && !state.group.isNull()) {
    CDCC_TRY(checkRecursion(state.group));
  }
  return restoreNode(state);
}

Status GraphDef::restoreNode(const NodeState &state)
{
  CDCC_TRY(busy("insertNode"));
  if (state.id.value <= 0 || state.id.value >= int64_t(1) << 52) {
    return makeError(StatusCode::InvalidArgument, "insertNode: the id isn't in 1 to 2^52-1");
  }
  if (idTaken(state.id.value)) {
    return makeError(StatusCode::AlreadyExists,
                     "insertNode: the id " + std::to_string(state.id.value) + " is taken");
  }
  if (is(state.typeName, kGraphOutputType)) {
    bool found = false;
    forEachNode([&](const GraphNode &n) { found |= is(n.typeName(), kGraphOutputType); });
    if (found) {
      return makeError(StatusCode::AlreadyExists, "a graph has at most one graph.output node");
    }
  }
  Library *lib = lib_.get();
  const bool isGroup = isGroupType(state.typeName);
  bool refs = isGroup && !state.group.isNull();
  for (const NodeState::Ref &r : state.refs) {
    refs |= !r.target.isNull();
  }
  refs |= hasRefs(state.extras);
  if (refs && !lib) {
    return makeError(StatusCode::FailedPrecondition,
                     "insertNode: a def without a Library can't refer to blocks");
  }
  if (lib && lib->sweeping()) {
    return makeError(StatusCode::Busy, "insertNode: the library is sweeping");
  }

  GraphNode *n = litestl::alloc::New<GraphNode>("coredcc::GraphNode");
  n->id_ = state.id;
  n->typeName_ = state.typeName;
  n->label_ = state.label;
  n->pos_[0] = state.pos[0];
  n->pos_[1] = state.pos[1];
  n->extras_ = state.extras;
  n->params_ = state.params;
  n->literals_ = state.literals;
  n->inputs_ = state.inputs;
  n->outputs_ = state.outputs;
  for (const NodeState::Ref &r : state.refs) {
    NodeBlockRef ref;
    ref.name = r.name;
    ref.cell.kind = r.weak ? RefKind::Weak : RefKind::OptStrong;
    n->refs_.append(ref);
  }
  GraphNode *after = last_;
  if (state.after) {
    GraphNode *const *found = byId_.lookup_ptr(state.after->value);
    after = found ? *found : state.after->isNull() ? nullptr : last_;
  }
  attachNode(n, after);
  matchParamRefs(n);
  n->typeVersion_ = state.typeVersion ? state.typeVersion : n->type_ ? n->type_->version : 1;
  if (refs) {
    for (int i = 0; i < int(state.refs.size()); i++) {
      lib->writeRef(n->refs_[i].cell, state.refs[i].target).ignore(); /* can't fail: checked */
    }
    if (isGroup) {
      lib->writeRef(n->group_, state.group).ignore();
    }
    lib->noteRefsChanged(); /* the extras' */
  }
  const uint64_t version = stamp(true);
  n->paramVersion_ = version;
  for (NodeValue &v : n->params_) {
    v.version = version;
  }
  for (NodeValue &v : n->literals_) {
    v.version = version;
  }
  announce(true);
  return Status();
}

Status GraphDef::removeNode(NodeId id)
{
  CDCC_TRY(busy("removeNode"));
  Result<GraphNode *> found = find(id, "removeNode");
  if (!found.ok()) {
    return found.status();
  }
  GraphNode *n = *found;
  while (n->in_.size() > 0) {
    detachLink(n->in_.last());
  }
  while (n->out_.size() > 0) {
    detachLink(n->out_.last());
  }
  bool refs = !n->group_.isNull() || hasRefs(n->extras_);
  for (const NodeBlockRef &r : n->refs_) {
    refs |= !r.cell.isNull();
  }
  detachNode(n);
  litestl::alloc::Delete(n);
  if (refs && lib_.get()) {
    lib_.get()->noteRefsChanged();
  }
  stamp(true);
  announce(true);
  return Status();
}

Status GraphDef::setLabel(NodeId id, std::string_view label)
{
  CDCC_TRY(busy("setLabel"));
  Result<GraphNode *> n = find(id, "setLabel");
  if (!n.ok()) {
    return n.status();
  }
  (*n)->label_ = lsString(label);
  announce(false);
  return Status();
}

Status GraphDef::setPosition(NodeId id, double x, double y)
{
  CDCC_TRY(busy("setPosition"));
  Result<GraphNode *> n = find(id, "setPosition");
  if (!n.ok()) {
    return n.status();
  }
  (*n)->pos_[0] = x;
  (*n)->pos_[1] = y;
  announce(false);
  return Status();
}

Status GraphDef::setExtra(NodeId id, std::string_view name, const PropValue &value)
{
  if (name.empty()) {
    return makeError(StatusCode::InvalidArgument, "setExtra: an extra needs a name");
  }
  if (value.type() == PropValue::Type::Pointer) {
    return makeError(StatusCode::Unsupported, "setExtra: an extra can't be a pointer");
  }
  NodeExtras values;
  if (!value.isNone()) {
    BlockEntry e;
    e.name = lsString(name);
    e.value = value;
    values.append(std::move(e));
  }
  return restoreExtra(id, name, values);
}

Status GraphDef::setParam(NodeId id, std::string_view name, const PropValue &value)
{
  CDCC_TRY(busy("setParam"));
  Result<GraphNode *> found = find(id, "setParam");
  if (!found.ok()) {
    return found.status();
  }
  GraphNode *n = *found;
  const NodeType *type = n->type();
  if (!type) {
    return rt_.ext<NodeTypeRegistry>().unknown(n->typeName_.c_str());
  }
  const int i = type->paramIndex(name);
  if (i < 0) {
    Status s = makeError(StatusCode::NotFound,
                         "node type '" + std::string(type->name.c_str()) + "' has no param '" +
                             std::string(name) + "'");
    Suggester sg(name);
    for (const NodeParamDef &p : type->params) {
      sg.consider(p.def.apiName().c_str());
    }
    sg.addTo(s);
    return s;
  }
  const NodeParamDef &param = type->params[i];
  if (param.def.kind() == PropKind::Pointer) {
    CDCC_TRY(setParamRef(n, param, value));
    n->paramVersion_ = stamp(false);
    announce(false);
    return Status();
  }
  PropValue v;
  if (!value.isNone()) {
    Result<PropValue> c = coerce(param.def, value);
    if (!c.ok()) {
      return c.status();
    }
    v = std::move(c).value();
  }
  n->paramVersion_ = stamp(false);
  store(n->params_, name, std::move(v), n->paramVersion_);
  announce(false);
  return Status();
}

Status GraphDef::setParamRef(GraphNode *n, const NodeParamDef &param, const PropValue &value)
{
  Library *lib = lib_.get();
  Result<PropValue> c = coerce(param.def, value);
  if (!c.ok()) {
    return c.status();
  }
  const StableRef &ref = c->asPointer();
  const std::string where = "param '" + std::string(param.def.apiName().c_str()) + "': ";
  BlockId target;
  if (!ref.isNull()) {
    if (!lib) {
      return makeError(StatusCode::FailedPrecondition,
                       where + "a def without a Library can't refer to blocks");
    }
    if (ref.owner.kind != OwnerKey::Kind::Block || ref.key.size() > 0) {
      return makeError(StatusCode::InvalidArgument, where + "only whole blocks can be targets");
    }
    target = rt_.ext<ClientRegistry>().canonical(ref.owner.block);
    if (!self_.isNull() && target == rt_.ext<ClientRegistry>().canonical(self_)) {
      return makeError(StatusCode::InvalidArgument,
                       where + "a graph can't point at its own block");
    }
  }
  return writeParamRef(n, param.def.apiName().c_str(), param.weak, target);
}

Status GraphDef::writeParamRef(GraphNode *n, std::string_view name, bool weak, BlockId target)
{
  Library *lib = lib_.get();
  for (NodeBlockRef &r : n->refs_) {
    if (name == r.name.c_str()) {
      return lib ? lib->writeRef(r.cell, target) : Status();
    }
  }
  /* The type was registered after the node was made. */
  NodeBlockRef r;
  r.name = lsString(name);
  r.cell.kind = weak ? RefKind::Weak : RefKind::OptStrong;
  if (lib) {
    CDCC_TRY(lib->writeRef(r.cell, target));
  }
  n->refs_.append(r);
  return Status();
}

Result<const SocketType *> GraphDef::literalType(const GraphNode &n,
                                                 std::string_view input,
                                                 int &index) const
{
  const SocketInfos &list = sockets(n, SocketSide::Input);
  index = indexOfSocket(list, input);
  if (index < 0) {
    return noSocket(n, list, "input", input);
  }
  const SocketTypeRegistry &types = rt_.ext<SocketTypeRegistry>();
  const SocketType *type = types.find(list[index].typeName.c_str());
  if (!type) {
    return types.unknown(list[index].typeName.c_str());
  }
  if (!type->hasLiteral()) {
    return makeError(StatusCode::Unsupported,
                     "input '" + std::string(input) + "' is a payload, which has no literal");
  }
  return type;
}

Status GraphDef::setLiteral(NodeId id, std::string_view input, const PropValue &value)
{
  CDCC_TRY(busy("setLiteral"));
  Result<GraphNode *> found = find(id, "setLiteral");
  if (!found.ok()) {
    return found.status();
  }
  GraphNode *n = *found;
  int i = 0;
  Result<const SocketType *> type = literalType(*n, input, i);
  if (!type.ok()) {
    return type.status();
  }
  PropValue v;
  if (!value.isNone()) {
    const bool fixed = n->type_ && n->type_->role == NodeRole::Kernel;
    Result<PropValue> c =
        coerce(fixed ? *n->type_->literal(i) : (*type)->literalDef("value"), value);
    if (!c.ok()) {
      return c.status();
    }
    v = std::move(c).value();
  }
  NodeValue *old = findLiteral(n->literals_, input, **type);
  const uint64_t version = stamp(false);
  if (v.isNone()) {
    if (old) {
      n->literals_.remove_at(int(old - n->literals_.data()));
    }
  }
  else if (old) {
    old->value = std::move(v);
    old->version = version;
    old->off = false;
  }
  else {
    n->literals_.append(NodeValue{lsString(input), std::move(v), version});
  }
  announce(false);
  return Status();
}

Status GraphDef::useLiteral(NodeId id, std::string_view input, bool on)
{
  CDCC_TRY(busy("useLiteral"));
  Result<GraphNode *> found = find(id, "useLiteral");
  if (!found.ok()) {
    return found.status();
  }
  GraphNode *n = *found;
  int i = 0;
  Result<const SocketType *> type = literalType(*n, input, i);
  if (!type.ok()) {
    return type.status();
  }
  if (NodeValue *old = findLiteral(n->literals_, input, **type)) {
    old->off = !on;
    old->version = stamp(false);
  }
  else if (on) {
    PropValue v = literalValue(*n, input, **type).toProp();
    n->literals_.append(NodeValue{lsString(input), std::move(v), stamp(false)});
  }
  announce(false);
  return Status();
}

Status GraphDef::setGroup(NodeId id, BlockId def)
{
  CDCC_TRY(busy("setGroup"));
  Result<GraphNode *> found = find(id, "setGroup");
  if (!found.ok()) {
    return found.status();
  }
  GraphNode *n = *found;
  if (!isGroupType(n->typeName_)) {
    return makeError(StatusCode::TypeMismatch,
                     "setGroup: node '" + std::string(n->typeName_.c_str()) + "' isn't a group");
  }
  Library *lib = lib_.get();
  if (!def.isNull()) {
    if (!lib) {
      return makeError(StatusCode::FailedPrecondition,
                       "setGroup: a def without a Library can't refer to blocks");
    }
    const Datablock *block = lib->get(lib->find(def));
    if (!block) {
      return makeError(StatusCode::NotFound, "setGroup: no such block");
    }
    if (!(block->type() == TypeId::of(kGraphDefType))) {
      return makeError(StatusCode::TypeMismatch, "setGroup: the block isn't a graph");
    }
    CDCC_TRY(checkRecursion(def));
  }
  if (lib) {
    CDCC_TRY(lib->writeRef(n->group_, def));
  }
  n->paramVersion_ = stamp(true);
  announce(true);
  return Status();
}

/* ---- links ---- */

Result<LinkId> GraphDef::link(SocketRef from, SocketRef to)
{
  CDCC_TRY(busy("link"));
  Result<GraphNode *> a = find(from.node, "link");
  Result<GraphNode *> b = find(to.node, "link");
  if (!a.ok() || !b.ok()) {
    return a.ok() ? b.status() : a.status();
  }
  const std::optional<SocketInfo> out = findSocket(**a, SocketSide::Output, from.key);
  if (!out) {
    return noSocket(**a, sockets(**a, SocketSide::Output), "output", from.key);
  }
  const std::optional<SocketInfo> in = findSocket(**b, SocketSide::Input, to.key);
  if (!in) {
    return noSocket(**b, sockets(**b, SocketSide::Input), "input", to.key);
  }
  const SocketTypeRegistry &types = rt_.ext<SocketTypeRegistry>();
  const SocketType *fromType = types.find(out->typeName.c_str());
  const SocketType *toType = types.find(in->typeName.c_str());
  if (!fromType || !toType) {
    return types.unknown((fromType ? in : out)->typeName.c_str());
  }
  if (!types.canConvert(*fromType, *toType)) {
    return makeError(StatusCode::TypeMismatch,
                     "there is no conversion from " + std::string(fromType->name.c_str()) +
                         " to " + toType->name.c_str());
  }
  if (wouldCycle(**a, from.key, **b, to.key)) {
    return makeError(StatusCode::InvalidArgument,
                     "linking '" + std::string(from.key) + "' to '" + std::string(to.key) +
                         "' would make a cycle");
  }
  if (const GraphLink *old = (*b)->linkInto(to.key)) {
    detachLink(const_cast<GraphLink *>(old));
  }
  LinkState state;
  state.id = LinkId{newId()};
  state.fromKey = lsString(from.key);
  state.toKey = lsString(to.key);
  attachLink(state, *a, *b);
  stamp(true);
  announce(true);
  return state.id;
}

Status GraphDef::insertLink(const LinkState &state)
{
  CDCC_TRY(busy("insertLink"));
  Result<GraphNode *> a = find(state.from, "insertLink");
  Result<GraphNode *> b = find(state.to, "insertLink");
  if (!a.ok() || !b.ok()) {
    return a.ok() ? b.status() : a.status();
  }
  if (state.id.value <= 0 || state.id.value >= int64_t(1) << 52) {
    return makeError(StatusCode::InvalidArgument, "insertLink: the id isn't in 1 to 2^52-1");
  }
  if (idTaken(state.id.value)) {
    return makeError(StatusCode::AlreadyExists, "insertLink: the id is taken");
  }
  if ((*b)->linkInto(state.toKey.c_str())) {
    return makeError(StatusCode::AlreadyExists,
                     "insertLink: input '" + std::string(state.toKey.c_str()) +
                         "' is linked already");
  }
  attachLink(state, *a, *b);
  stamp(true);
  announce(true);
  return Status();
}

Status GraphDef::unlink(LinkId id)
{
  CDCC_TRY(busy("unlink"));
  GraphLink *const *l = links_.lookup_ptr(id.value);
  if (!l) {
    return makeError(StatusCode::NotFound, "unlink: no link " + std::to_string(id.value));
  }
  detachLink(*l);
  stamp(true);
  announce(true);
  return Status();
}

/* ---- interface ---- */

Status GraphDef::checkInterfaceName(SocketSide side, std::string_view name, int64_t except) const
{
  if (!isInterfaceName(name)) {
    return makeError(StatusCode::InvalidArgument,
                     "an interface socket needs a name, which can't start with '#' or '@'");
  }
  const InterfaceSocket *twin = interfaceSocket(side, name);
  if (twin && twin->id != except) {
    return makeError(StatusCode::AlreadyExists,
                     "the interface has a socket '" + std::string(name) + "' already");
  }
  return Status();
}

static Status noInterface(int64_t id)
{
  return makeError(StatusCode::NotFound, "no interface socket #" + std::to_string(id));
}

Result<int64_t> GraphDef::addInterface(SocketSide side,
                                       std::string_view name,
                                       std::string_view socketType,
                                       int index,
                                       int64_t id)
{
  CDCC_TRY(busy("addInterface"));
  CDCC_TRY(checkInterfaceName(side, name, 0));
  const SocketTypeRegistry &types = rt_.ext<SocketTypeRegistry>();
  if (!types.find(socketType)) {
    return types.unknown(socketType);
  }
  Vector<InterfaceSocket> &list = side == SocketSide::Input ? ifaceIn_ : ifaceOut_;
  if (index < -1 || index > int(list.size())) {
    return makeError(StatusCode::OutOfRange, "addInterface: the index is out of range");
  }
  if (id < 0 || id >= int64_t(1) << 52) {
    return makeError(StatusCode::InvalidArgument, "addInterface: the id isn't in 1 to 2^52-1");
  }
  if (id != 0 && idTaken(id)) {
    return makeError(StatusCode::AlreadyExists, "addInterface: the id is taken");
  }
  InterfaceSocket s;
  s.id = id ? id : newId();
  s.name = lsString(name);
  s.typeName = lsString(socketType);
  const int64_t out = s.id;
  insertInterface(list, std::move(s), index);
  return out;
}

void GraphDef::insertInterface(Vector<InterfaceSocket> &list, InterfaceSocket socket, int index)
{
  socket.def = this;
  list.append(std::move(socket));
  for (int i = int(list.size()) - 1; index >= 0 && i > index; i--) {
    std::swap(list[i], list[i - 1]);
  }
  interfaceVersion_ = stamp(true);
  announce(true);
}

Status GraphDef::removeInterface(int64_t id)
{
  CDCC_TRY(busy("removeInterface"));
  SocketSide side;
  InterfaceSocket *s = findInterface(id, &side);
  if (!s) {
    return noInterface(id);
  }
  Vector<InterfaceSocket> &list = side == SocketSide::Input ? ifaceIn_ : ifaceOut_;
  list.remove_at(int(s - list.data()));
  interfaceVersion_ = stamp(true);
  announce(true);
  return Status();
}

Status GraphDef::moveInterface(int64_t id, int index)
{
  CDCC_TRY(busy("moveInterface"));
  SocketSide side;
  InterfaceSocket *s = findInterface(id, &side);
  if (!s) {
    return noInterface(id);
  }
  Vector<InterfaceSocket> &list = side == SocketSide::Input ? ifaceIn_ : ifaceOut_;
  if (index < 0 || index >= int(list.size())) {
    return makeError(StatusCode::OutOfRange, "moveInterface: the index is out of range");
  }
  int at = int(s - list.data());
  for (; at < index; at++) {
    std::swap(list[at], list[at + 1]);
  }
  for (; at > index; at--) {
    std::swap(list[at], list[at - 1]);
  }
  interfaceVersion_ = stamp(true);
  announce(true);
  return Status();
}

Status GraphDef::setInterfaceType(int64_t id, std::string_view socketType)
{
  CDCC_TRY(busy("setInterfaceType"));
  InterfaceSocket *s = findInterface(id);
  if (!s) {
    return noInterface(id);
  }
  const SocketTypeRegistry &types = rt_.ext<SocketTypeRegistry>();
  if (!types.find(socketType)) {
    return types.unknown(socketType);
  }
  s->typeName = lsString(socketType);
  interfaceVersion_ = stamp(true);
  Result<GraphValue> v = GraphValue::fromProp(s->defaultValue);
  if (!v.ok() || !types.find(socketType)->holds(*v)) {
    s->defaultValue = PropValue();
    s->defaultVersion = interfaceVersion_;
  }
  announce(true);
  return Status();
}

Status GraphDef::setInterfaceName(int64_t id, std::string_view name)
{
  CDCC_TRY(busy("setInterfaceName"));
  SocketSide side;
  InterfaceSocket *s = findInterface(id, &side);
  if (!s) {
    return noInterface(id);
  }
  CDCC_TRY(checkInterfaceName(side, name, id));
  s->name = lsString(name);
  interfaceVersion_ = stamp(true);
  announce(true);
  return Status();
}

Status GraphDef::setInterfaceDefault(int64_t id, const PropValue &value)
{
  CDCC_TRY(busy("setInterfaceDefault"));
  SocketSide side = SocketSide::Input;
  InterfaceSocket *s = findInterface(id, &side);
  if (!s) {
    return noInterface(id);
  }
  if (side != SocketSide::Input) {
    return makeError(StatusCode::InvalidArgument, "only interface inputs have defaults");
  }
  PropValue v;
  if (!value.isNone()) {
    const SocketTypeRegistry &types = rt_.ext<SocketTypeRegistry>();
    const SocketType *type = types.find(s->typeName.c_str());
    if (!type) {
      return types.unknown(s->typeName.c_str());
    }
    if (!type->hasLiteral()) {
      return makeError(StatusCode::Unsupported, "a payload socket has no default");
    }
    Result<PropValue> c = coerce(type->literalDef("value"), value);
    if (!c.ok()) {
      return c.status();
    }
    v = std::move(c).value();
  }
  s->defaultValue = std::move(v);
  s->defaultVersion = stamp(false);
  announce(false);
  return Status();
}

/* ---- for undo ---- */

Status GraphDef::restoreValues(NodeId id,
                               bool param,
                               std::string_view name,
                               const NodeValues &values)
{
  CDCC_TRY(busy("restoreValues"));
  Result<GraphNode *> n = find(id, "restoreValues");
  if (!n.ok()) {
    return n.status();
  }
  NodeValues &list = param ? (*n)->params_ : (*n)->literals_;
  const uint64_t version = stamp(false);
  for (int i = int(list.size()) - 1; i >= 0; i--) {
    if (name == list[i].name.c_str()) {
      list.remove_at(i);
    }
  }
  for (const NodeValue &v : values) {
    list.append(v);
    list.last().version = version;
  }
  if (param) {
    (*n)->paramVersion_ = version;
  }
  announce(false);
  return Status();
}

Status GraphDef::restoreExtra(NodeId id, std::string_view name, const NodeExtras &values)
{
  CDCC_TRY(busy("setExtra"));
  Result<GraphNode *> found = find(id, "setExtra");
  if (!found.ok()) {
    return found.status();
  }
  NodeExtras &list = (*found)->extras_;
  int at = 0;
  while (at < int(list.size()) && name != list[at].name.c_str()) {
    at++;
  }
  const bool refs = hasRefs(values) || (at < int(list.size()) && list[at].refs.size() > 0);
  if (at < int(list.size())) {
    list.remove_at(at);
  }
  for (const BlockEntry &e : values) {
    list.append(e);
  }
  if (refs && lib_.get()) {
    lib_.get()->noteRefsChanged();
  }
  announce(false);
  return Status();
}

Status GraphDef::restoreRef(NodeId id, std::string_view param, BlockId target)
{
  CDCC_TRY(busy("restoreRef"));
  Result<GraphNode *> found = find(id, "restoreRef");
  if (!found.ok()) {
    return found.status();
  }
  GraphNode *n = *found;
  Library *lib = lib_.get();
  if (!target.isNull() && !lib) {
    return makeError(StatusCode::FailedPrecondition,
                     "a def without a Library can't refer to blocks");
  }
  const bool group = param.empty();
  if (!group) {
    const NodeType *type = n->type();
    const int i = type ? type->paramIndex(param) : -1;
    CDCC_TRY(writeParamRef(n, param, i >= 0 && type->params[i].weak, target));
  }
  else if (lib) {
    CDCC_TRY(lib->writeRef(n->group_, target));
  }
  n->paramVersion_ = stamp(group);
  announce(group);
  return Status();
}

Status GraphDef::restoreInterface(SocketSide side, const InterfaceSocket &socket, int index)
{
  CDCC_TRY(busy("restoreInterface"));
  Vector<InterfaceSocket> &list = side == SocketSide::Input ? ifaceIn_ : ifaceOut_;
  if (idTaken(socket.id) || index < 0 || index > int(list.size())) {
    return makeError(StatusCode::FailedPrecondition, "restoreInterface: its place is gone");
  }
  InterfaceSocket s = socket;
  s.defaultVersion = rt_.touch();
  insertInterface(list, std::move(s), index);
  return Status();
}

Status GraphDef::restoreInterfaceType(int64_t id, const InterfaceSocket &was)
{
  CDCC_TRY(busy("restoreInterfaceType"));
  InterfaceSocket *s = findInterface(id);
  if (!s) {
    return noInterface(id);
  }
  s->typeName = was.typeName;
  s->defaultValue = was.defaultValue;
  s->defaultVersion = interfaceVersion_ = stamp(true);
  announce(true);
  return Status();
}

Status GraphDef::restoreInterfaceDefault(int64_t id, const PropValue &value)
{
  CDCC_TRY(busy("restoreInterfaceDefault"));
  InterfaceSocket *s = findInterface(id);
  if (!s) {
    return noInterface(id);
  }
  s->defaultValue = value;
  s->defaultVersion = stamp(false);
  announce(false);
  return Status();
}

} // namespace coredcc
