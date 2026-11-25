/* GraphDef: construction, reading, and the machinery edits share. The edits are in
 * def_edit.cc, the cycle checks in reach.cc. */
#include "coredcc/graph/def.h"

#include "coredcc/api/change_bus.h"
#include "coredcc/base/text.h"
#include "coredcc/graph/provider.h"
#include "coredcc/graph/topology.h"
#include "coredcc/lib/library.h"

#include <cstring>
#include <string>

namespace coredcc {

using litestl::util::string;
using litestl::util::Vector;

string interfaceKey(int64_t id)
{
  return string(("#" + std::to_string(id)).c_str());
}

string InterfaceSocket::key() const
{
  return interfaceKey(id);
}

const GraphLink *GraphNode::linkInto(std::string_view key) const
{
  for (const GraphLink *l : in_) {
    if (key == l->toKey.c_str()) {
      return l;
    }
  }
  return nullptr;
}

static const NodeValue *findValue(const NodeValues &list, std::string_view name)
{
  for (const NodeValue &v : list) {
    if (name == v.name.c_str()) {
      return &v;
    }
  }
  return nullptr;
}

const NodeValue *GraphNode::literal(std::string_view key) const
{
  return findValue(literals_, key);
}

const NodeValue *GraphNode::param(std::string_view name) const
{
  return findValue(params_, name);
}

BlockId GraphNode::blockTarget(std::string_view param) const
{
  for (const NodeBlockRef &r : refs_) {
    if (param == r.name.c_str()) {
      return r.cell.target;
    }
  }
  return BlockId();
}

GraphPassKey GraphPassKey::of(Runtime &rt, const Library *lib)
{
  GraphPassKey k;
  k.edit = rt.lastEdit();
  if (lib) {
    lib->find(BlockId()); /* an alias added since re-keys the records, which moves the epoch */
    k.epoch = lib->epoch();
  }
  k.clients = rt.ext<ClientRegistry>().generation();
  k.ifaceTables = rt.ifaces().tableCount();
  return k;
}

static bool isType(const GraphNode &n, const char *type)
{
  return strcmp(n.typeName().c_str(), type) == 0;
}

/* ---- GraphDef ---- */

GraphDef::GraphDef(Runtime *rt, Library *lib, BlockId self)
    : rt_(*rt), lib_(lib), self_(self), serial_(rt->nextVersion())
{
  CDCC_CHECK(self.isNull() || lib, "a block's GraphDef needs its Library");
  rt_.ext<NodeTypeRegistry>();
  rt_.ext<ChangeBus>();
  structureVersion_ = contentVersion_ = interfaceVersion_ = rt_.nextVersion();
}

GraphDef::~GraphDef()
{
  for (auto &pair : links_) {
    litestl::alloc::Delete(pair.value);
  }
  for (GraphNode *n : nodes_) {
    litestl::alloc::Delete(n);
  }
}

const GraphNode *GraphDef::node(NodeId id) const
{
  GraphNode *const *n = byId_.lookup_ptr(id.value);
  return n ? *n : nullptr;
}

const GraphLink *GraphDef::link(LinkId id) const
{
  GraphLink *const *l = links_.lookup_ptr(id.value);
  return l ? *l : nullptr;
}

const InterfaceSocket *GraphDef::interfaceSocket(int64_t id) const
{
  return const_cast<GraphDef *>(this)->findInterface(id);
}

const InterfaceSocket *GraphDef::interfaceSocket(SocketSide side, std::string_view name) const
{
  for (const InterfaceSocket &s : interfaceSockets(side)) {
    if (name == s.name.c_str()) {
      return &s;
    }
  }
  return nullptr;
}

InterfaceSocket *GraphDef::findInterface(int64_t id, SocketSide *side)
{
  for (SocketSide sd : {SocketSide::Input, SocketSide::Output}) {
    for (InterfaceSocket &s : sd == SocketSide::Input ? ifaceIn_ : ifaceOut_) {
      if (s.id == id) {
        if (side) {
          *side = sd;
        }
        return &s;
      }
    }
  }
  return nullptr;
}

/* ---- sockets ---- */

const NodeType *GraphDef::typeOf(const GraphNode &node) const
{
  if (!node.type_) {
    node.type_ = rt_.ext<NodeTypeRegistry>().find(node.typeName_.c_str());
  }
  return node.type_;
}

static void fromInterface(const Vector<InterfaceSocket> &iface, SocketInfos &out)
{
  out.clear();
  for (const InterfaceSocket &s : iface) {
    out.append(SocketInfo{s.key(), s.name, s.typeName});
  }
}

void GraphDef::refreshSockets(const GraphNode &node) const
{
  const NodeType *type = typeOf(node);
  if (!type || type->role == NodeRole::Kernel) {
    return;
  }
  const GraphDef *source = type->role == NodeRole::Group ? groupTarget(node) : this;
  /* Interface versions are unique, so one also says which def it is of. */
  if (!source || node.socketsVersion_ == source->interfaceVersion_) {
    return;
  }
  node.socketsVersion_ = source->interfaceVersion_;
  if (type->role != NodeRole::Output) {
    fromInterface(source->ifaceIn_,
                  type->role == NodeRole::Input ? node.outputs_ : node.inputs_);
  }
  if (type->role != NodeRole::Input) {
    fromInterface(source->ifaceOut_,
                  type->role == NodeRole::Output ? node.inputs_ : node.outputs_);
  }
}

const SocketInfos &GraphDef::sockets(const GraphNode &node, SocketSide side) const
{
  if (const NodeType *type = typeOf(node); type && type->role == NodeRole::Kernel) {
    return side == SocketSide::Input ? type->inputInfos : type->outputInfos;
  }
  refreshSockets(node);
  return side == SocketSide::Input ? node.inputs_ : node.outputs_;
}

GraphDef *GraphDef::groupTarget(const GraphNode &node) const
{
  const Library *lib = lib_.get();
  return lib && !node.group_.isNull() ? graphDefOf(*lib, lib->resolveRef(node.group_)) : nullptr;
}

PropValue GraphDef::paramValue(const GraphNode &node, std::string_view name) const
{
  const NodeType *type = typeOf(node);
  const int i = type ? type->paramIndex(name) : -1;
  if (i < 0 || type->params[i].def.kind() == PropKind::Pointer) {
    return PropValue();
  }
  const PropertyDef &def = type->params[i].def;
  if (const NodeValue *set = node.param(name)) {
    /* A file, or an older version of the type, may hold a value the def doesn't accept. */
    Result<PropValue> v = coerce(def, set->value);
    if (v.ok()) {
      return std::move(v).value();
    }
  }
  return def.hasAccess(PropAccess::Optional) ? PropValue() : def.defaultValue();
}

Vector<LinkDiagnostic> GraphDef::diagnostics() const
{
  const SocketTypeRegistry &types = rt_.ext<SocketTypeRegistry>();
  auto typeOfSocket = [&](const GraphNode &n, SocketSide side, const string &key, Status &status) {
    for (const SocketInfo &s : sockets(n, side)) {
      if (strcmp(s.key.c_str(), key.c_str()) == 0) {
        const SocketType *t = types.find(s.typeName.c_str());
        if (!t) {
          status = types.unknown(s.typeName.c_str());
        }
        return t;
      }
    }
    status = Status::error(StatusCode::NotFound,
                           (std::string("node ") + std::to_string(n.id().value) + " (" +
                            n.typeName().c_str() + ") has no " +
                            (side == SocketSide::Input ? "input '" : "output '") + key.c_str() +
                            "'")
                               .c_str());
    return static_cast<const SocketType *>(nullptr);
  };
  Vector<LinkDiagnostic> out;
  forEachLink([&](const GraphLink &l) {
    Status status;
    const SocketType *from = typeOfSocket(*l.from, SocketSide::Output, l.fromKey, status);
    const SocketType *to =
        status.ok() ? typeOfSocket(*l.to, SocketSide::Input, l.toKey, status) : nullptr;
    if (status.ok() && !types.canConvert(*from, *to)) {
      status = Status::error(StatusCode::TypeMismatch,
                             (std::string("there is no conversion from ") + from->name.c_str() +
                              " to " + to->name.c_str())
                                 .c_str());
    }
    if (!status.ok()) {
      out.append(LinkDiagnostic{l.id, status});
    }
  });
  return out;
}

NodeState GraphDef::stateOf(const GraphNode &node) const
{
  NodeState s;
  s.id = node.id_;
  s.typeName = node.typeName_;
  s.label = node.label_;
  s.pos[0] = node.pos_[0];
  s.pos[1] = node.pos_[1];
  s.params = node.params_;
  s.literals = node.literals_;
  for (const NodeBlockRef &r : node.refs_) {
    s.refs.append(NodeState::Ref{r.name, r.cell.target, r.cell.kind == RefKind::Weak});
  }
  s.group = node.group_.target;
  s.inputs = sockets(node, SocketSide::Input);
  s.outputs = sockets(node, SocketSide::Output);
  return s;
}

LinkState GraphDef::stateOf(const GraphLink &link)
{
  return LinkState{link.id, link.from->id(), link.fromKey, link.to->id(), link.toKey};
}

/* ---- shared by the edits ---- */

Status GraphDef::busy(const char *what) const
{
  return rt_.busyIfEvaluating(what);
}

Result<GraphNode *> GraphDef::find(NodeId id, const char *what) const
{
  if (GraphNode *const *n = byId_.lookup_ptr(id.value)) {
    return *n;
  }
  return Status::error(StatusCode::NotFound,
                       (std::string(what) + ": no node " + std::to_string(id.value)).c_str());
}

uint64_t GraphDef::stamp(bool structural)
{
  contentVersion_ = rt_.touch();
  if (structural) {
    structureVersion_ = contentVersion_;
  }
  return contentVersion_;
}

void GraphDef::announce(bool structural)
{
  if (self_.isNull()) {
    return;
  }
  ChangeBus &bus = rt_.ext<ChangeBus>();
  if (structural) {
    bus.bumpEpoch();
  }
  bus.notify(OwnerKey::ofBlock(self_));
}

bool GraphDef::idTaken(int64_t id) const
{
  return byId_.lookup_ptr(id) || links_.lookup_ptr(id) || interfaceSocket(id);
}

int64_t GraphDef::newId()
{
  while (true) {
    const int64_t id = int64_t(rt_.newLocalId().lo & ((uint64_t(1) << 52) - 1));
    if (id != 0 && !idTaken(id)) {
      return id;
    }
  }
}

void GraphDef::attachNode(GraphNode *n)
{
  n->slot_ = int(nodes_.size());
  nodes_.append(n);
  byId_.add(n->id_.value, n);
  typeOf(*n);
}

void GraphDef::attachLink(const LinkState &state, GraphNode *from, GraphNode *to)
{
  GraphLink *l = litestl::alloc::New<GraphLink>("coredcc::GraphLink");
  l->id = state.id;
  l->from = from;
  l->fromKey = state.fromKey;
  l->to = to;
  l->toKey = state.toKey;
  from->out_.append(l);
  to->in_.append(l);
  links_.add(l->id.value, l);
}

void GraphDef::detachLink(GraphLink *link)
{
  link->from->out_.remove(link);
  link->to->in_.remove(link);
  links_.remove(link->id.value);
  litestl::alloc::Delete(link);
}

void GraphDef::foreachRef(RefVisitor &visitor)
{
  for (GraphNode *n : nodes_) {
    if (!n) {
      continue;
    }
    if (isType(*n, kGraphGroupType)) {
      visitor.visit(n->group_);
    }
    for (NodeBlockRef &r : n->refs_) {
      visitor.visit(r.cell);
    }
  }
}

/* ---- topology ---- */

bool GraphDef::topologyCurrent() const
{
  if (!topo_ || topo_->structure != structureVersion_ ||
      topo_->nodeTypes != rt_.ext<NodeTypeRegistry>().stamp() ||
      topo_->socketTypes != rt_.ext<SocketTypeRegistry>().stamp())
  {
    return false;
  }
  for (const GraphTopology::Group &g : topo_->groups) {
    const GraphDef *target = groupTarget(*g.node);
    if (g.targetInterface != (target ? target->interfaceVersion_ : 0)) {
      return false;
    }
  }
  return true;
}

std::shared_ptr<const GraphTopology> GraphDef::topology() const
{
  const GraphPassKey key = GraphPassKey::of(rt_, lib_.get());
  if (!topo_ || !(key == topoKey_)) {
    if (!topologyCurrent()) {
      topo_ = GraphTopology::build(*this);
    }
    topoKey_ = key;
  }
  return topo_;
}

Status GraphDef::validate() const
{
  auto bad = [](const char *what) {
    return Status::error(StatusCode::Internal, (std::string("GraphDef: ") + what).c_str());
  };
  int live = 0, outputs = 0;
  for (int i = 0; i < int(nodes_.size()); i++) {
    const GraphNode *n = nodes_[i];
    if (!n) {
      continue;
    }
    live++;
    outputs += isType(*n, kGraphOutputType);
    if (n->slot_ != i || node(n->id_) != n) {
      return bad("a node isn't where its id or slot says");
    }
    for (const GraphLink *l : n->in_) {
      if (l->to != n || link(l->id) != l || n->linkInto(l->toKey.c_str()) != l) {
        return bad("an incoming link is wrong, or shares its input");
      }
    }
    for (const GraphLink *l : n->out_) {
      if (l->from != n || link(l->id) != l) {
        return bad("an outgoing link is wrong");
      }
    }
  }
  if (live != int(byId_.size()) || outputs > 1) {
    return bad("node counts differ, or there are two graph.output nodes");
  }
  Status status;
  forEachLink([&](const GraphLink &l) {
    if (!l.from->out_.contains(const_cast<GraphLink *>(&l)) ||
        !l.to->in_.contains(const_cast<GraphLink *>(&l)))
    {
      status = bad("a link isn't in its nodes' lists");
    }
  });
  return status;
}

} // namespace coredcc
