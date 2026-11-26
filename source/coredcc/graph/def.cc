/* GraphDef: construction, reading, and the machinery edits share. The edits are in
 * def_edit.cc, the cycle checks in reach.cc. */
#include "coredcc/graph/def.h"

#include "coredcc/api/change_bus.h"
#include "coredcc/base/text.h"
#include "coredcc/graph/foreach.h"
#include "coredcc/graph/provider.h"
#include "coredcc/graph/system.h"
#include "coredcc/graph/topology.h"
#include "coredcc/lib/library.h"

#include <algorithm>
#include <cstring>
#include <string>

namespace coredcc {

using litestl::util::Map;
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
  if (inByKey_) {
    GraphLink *const *l = inByKey_->lookup_ptr({lsString(key)});
    return l ? *l : nullptr;
  }
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

GraphNode::~GraphNode()
{
  for (GraphSocketAddress *a : addresses_) {
    litestl::alloc::Delete(a);
  }
}

const NodeValue *GraphNode::literal(std::string_view key, const SocketType &type) const
{
  for (const NodeValue &v : literals_) {
    if (key == v.name.c_str() && !v.off) {
      Result<GraphValue> g = GraphValue::fromProp(v.value);
      if (g.ok() && type.holds(*g)) {
        return &v;
      }
    }
  }
  return nullptr;
}

const GraphSocketAddress *GraphNode::addressOf(std::string_view key)
{
  for (const GraphSocketAddress *a : addresses_) {
    if (key == a->key.c_str()) {
      return a;
    }
  }
  addresses_.append(litestl::alloc::New<GraphSocketAddress>(
      "coredcc::GraphSocketAddress", this, lsString(key)));
  return addresses_.last();
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
    lib->find(
        BlockId()); /* an alias added since re-keys the records, which moves the epoch */
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

bool isInterfaceName(std::string_view name)
{
  return !name.empty() && name[0] != '#' && name[0] != '@';
}

/* ---- GraphDef ---- */

GraphDef::GraphDef(Runtime *rt, Library *lib, BlockId self)
    : rt_(*rt), lib_(libraryRef(lib)), self_(self), serial_(rt->nextVersion())
{
  CDCC_CHECK(self.isNull() || lib, "a block's GraphDef needs its Library");
  rt_.ext<ChangeBus>();
  NodeTypeRegistry &types = rt_.ext<NodeTypeRegistry>();
  nextDef_ = types.defs_;
  if (nextDef_) {
    nextDef_->prevDef_ = this;
  }
  types.defs_ = this;
  structureVersion_ = contentVersion_ = interfaceVersion_ = refsVersion_ =
      rt_.nextVersion();
  rt_.ext<GraphSystem>().noteStructure(structureVersion_);
}

GraphDef::~GraphDef()
{
  for (auto &pair : links_) {
    litestl::alloc::Delete(pair.value);
  }
  for (GraphNode *n = first_; n;) {
    GraphNode *next = n->next_;
    litestl::alloc::Delete(n);
    n = next;
  }
  (prevDef_ ? prevDef_->nextDef_ : rt_.ext<NodeTypeRegistry>().defs_) = nextDef_;
  if (nextDef_) {
    nextDef_->prevDef_ = prevDef_;
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

const InterfaceSocket *GraphDef::interfaceSocket(SocketSide side,
                                                 std::string_view name) const
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

static void fromInterface(const Vector<InterfaceSocket> &iface, SocketInfos &out)
{
  for (const InterfaceSocket &s : iface) {
    out.append(SocketInfo{s.key(), s.name, s.typeName});
  }
}

void GraphDef::refreshSockets(const GraphNode &node) const
{
  const NodeType *type = node.type();
  if (!type || type->role == NodeRole::Kernel) {
    return;
  }
  const bool foreach = type->role == NodeRole::Foreach;
  const GraphDef *source = hasGroup(type->role) ? groupTarget(node) : this;
  /* Interface versions are unique, so one also says which def it is of. */
  if (source ? node.socketsVersion_ == source->interfaceVersion_
             : !foreach || (node.inputs_.size() > 0 && node.outputs_.size() > 0))
  {
    return;
  }
  node.inputs_.clear();
  node.outputs_.clear();
  node.socketsVersion_ = source ? source->interfaceVersion_ : 0;
  if (foreach) {
    const SocketInfo elements{
        kElementsSocket, kElementsSocket, PayloadTraits<Elements>::name};
    node.inputs_.append(elements);
    node.outputs_.append(elements);
  }
  if (source && type->role != NodeRole::Output) {
    fromInterface(source->ifaceIn_,
                  type->role == NodeRole::Input ? node.outputs_ : node.inputs_);
  }
  if (source && type->role != NodeRole::Input && !foreach) {
    fromInterface(source->ifaceOut_,
                  type->role == NodeRole::Output ? node.inputs_ : node.outputs_);
  }
  using Index = Map<detail::SocketKey, int>;
  auto index = [](const SocketInfos &list, std::unique_ptr<Index> &out) {
    out = list.size() > GraphNode::kScan ? std::make_unique<Index>() : nullptr;
    for (int i = 0; out && i < int(list.size()); i++) {
      out->add({list[i].key}, i);
    }
  };
  index(node.inputs_, node.inputIndex_);
  index(node.outputs_, node.outputIndex_);
}

const SocketInfos &GraphDef::sockets(const GraphNode &node, SocketSide side) const
{
  if (const NodeType *type = node.type(); type && type->role == NodeRole::Kernel) {
    return side == SocketSide::Input ? type->inputInfos : type->outputInfos;
  }
  refreshSockets(node);
  return side == SocketSide::Input ? node.inputs_ : node.outputs_;
}

std::optional<SocketInfo>
GraphDef::findSocket(const GraphNode &node, SocketSide side, std::string_view key) const
{
  if (isInteriorKey(key)) {
    const std::optional<InteriorKey> k = InteriorKey::parse(key);
    return k ? findInterior(*this, node, side, *k) : std::nullopt;
  }
  const SocketInfos &list = sockets(node, side);
  const bool input = side == SocketSide::Input;
  const Map<detail::SocketKey, int> *index =
      (input ? node.inputIndex_ : node.outputIndex_).get();
  if (index && &list == &(input ? node.inputs_ : node.outputs_)) {
    const int *i = index->lookup_ptr({lsString(key)});
    return i ? std::optional<SocketInfo>(list[*i]) : std::nullopt;
  }
  for (const SocketInfo &s : list) {
    if (key == s.key.c_str()) {
      return s;
    }
  }
  return std::nullopt;
}

GraphDef *GraphDef::groupTarget(const GraphNode &node) const
{
  const Library *lib = lib_.get();
  return lib && !node.group_.isNull() ? graphDefOf(*lib, lib->resolveRef(node.group_))
                                      : nullptr;
}

const GraphNode *GraphDef::nodeAt(int index) const
{
  if (index_.size() == 0) {
    forEachNode([this](const GraphNode &n) { index_.append(&n); });
  }
  return index >= 0 && index < int(index_.size()) ? index_[index] : nullptr;
}

GraphValue GraphDef::literalValue(const GraphNode &node,
                                  std::string_view key,
                                  const SocketType &type) const
{
  if (const NodeValue *set = node.literal(key, type)) {
    return GraphValue::fromProp(set->value).value();
  }
  const NodeType *t = node.type();
  if (t && t->role == NodeRole::Kernel) {
    const int i = t->inputIndex(key);
    return i >= 0 ? t->inputs[i].defaultValue : type.defaultValue;
  }
  if (t && hasGroup(t->role)) {
    const GraphDef *group = groupTarget(node);
    for (int i = 0; group && i < int(group->ifaceIn_.size()); i++) {
      if (key == group->ifaceIn_[i].key().c_str()) {
        Result<GraphValue> v = GraphValue::fromProp(group->ifaceIn_[i].defaultValue);
        return v.ok() && type.holds(*v) ? *v : type.defaultValue;
      }
    }
  }
  return type.defaultValue;
}

bool GraphDef::usesLiteral(const GraphNode &node, std::string_view key) const
{
  int i = 0;
  Result<const SocketType *> type = literalType(node, key, i);
  return type.ok() && node.literal(key, **type);
}

PropValue GraphDef::paramValue(const GraphNode &node, std::string_view name) const
{
  const NodeType *type = node.type();
  const int i = type ? type->paramIndex(name) : -1;
  if (i < 0 || type->params[i].def.kind() == PropKind::Pointer) {
    return PropValue();
  }
  const PropertyDef &def = type->params[i].def;
  if (const NodeValue *set = node.param(name)) {
    /* A file, or an older version of the type, may hold a value the def doesn't accept.
     */
    Result<PropValue> v = coerce(def, set->value);
    if (v.ok()) {
      return std::move(v).value();
    }
  }
  return def.hasAccess(PropAccess::Optional) ? PropValue() : def.defaultValue();
}

const Vector<LinkDiagnostic> &GraphDef::diagnostics() const
{
  /* All it depends on is in the topology's key, sockets of other defs included. */
  const std::shared_ptr<const GraphTopology> topo = topology();
  if (diagnosedAt_ == topo->serial) {
    return diagnostics_;
  }
  diagnosedAt_ = topo->serial;
  const SocketTypeRegistry &types = rt_.ext<SocketTypeRegistry>();
  auto typeOfSocket =
      [&](const GraphNode &n, SocketSide side, const string &key, Status &status) {
        if (const std::optional<SocketInfo> s = findSocket(n, side, key.c_str())) {
          const SocketType *t = types.find(s->typeName.c_str());
          if (!t) {
            status = types.unknown(s->typeName.c_str());
          }
          return t;
        }
        status = Status::error(StatusCode::NotFound,
                               (std::string("node ") + std::to_string(n.id().value) +
                                " (" + n.typeName().c_str() + ") has no " +
                                (side == SocketSide::Input ? "input '" : "output '") +
                                key.c_str() + "'")
                                   .c_str());
        return static_cast<const SocketType *>(nullptr);
      };
  diagnostics_.clear();
  for (int i = 0; i < linkCount(); i++) {
    const GraphLink &l = *linkAt(i);
    Status status;
    const SocketType *from = typeOfSocket(*l.from, SocketSide::Output, l.fromKey, status);
    const SocketType *to =
        status.ok() ? typeOfSocket(*l.to, SocketSide::Input, l.toKey, status) : nullptr;
    if (status.ok() && !types.canConvert(*from, *to)) {
      status = Status::error(StatusCode::TypeMismatch,
                             (std::string("there is no conversion from ") +
                              from->name.c_str() + " to " + to->name.c_str())
                                 .c_str());
    }
    if (!status.ok()) {
      diagnostics_.append(LinkDiagnostic{l.id,
                                         status,
                                         isInteriorKey(l.fromKey.c_str()) ||
                                             isInteriorKey(l.toKey.c_str())});
    }
  }
  return diagnostics_;
}

const LinkDiagnostic *GraphDef::diagnostic(LinkId link) const
{
  const Vector<LinkDiagnostic> &list = diagnostics();
  const LinkDiagnostic *end = list.data() + list.size();
  const LinkDiagnostic *at =
      std::lower_bound(list.data(), end, link, [](const LinkDiagnostic &d, LinkId id) {
        return d.link < id;
      });
  return at != end && at->link == link ? at : nullptr;
}

const GraphLink *GraphDef::linkAt(int index) const
{
  if (linkIndex_.size() == 0) {
    forEachLink([&](const GraphLink &l) { linkIndex_.append(&l); });
    std::sort(linkIndex_.begin(),
              linkIndex_.end(),
              [](const GraphLink *a, const GraphLink *b) { return a->id < b->id; });
  }
  return index >= 0 && index < int(linkIndex_.size()) ? linkIndex_[index] : nullptr;
}

const Vector<const GraphNode *> &GraphDef::flaggedNodes() const
{
  const uint64_t types = rt_.ext<NodeTypeRegistry>().stamp();
  if (flaggedAt_ != structureVersion_ || flaggedTypes_ != types) {
    flaggedAt_ = structureVersion_;
    flaggedTypes_ = types;
    flagged_.clear();
    forEachNode([&](const GraphNode &n) {
      if (n.type() && n.type()->flags != NodeTypeFlags::None) {
        flagged_.append(&n);
      }
    });
  }
  return flagged_;
}

NodeState GraphDef::stateOf(const GraphNode &node) const
{
  NodeState s;
  s.id = node.id_;
  s.typeName = node.typeName_;
  s.typeVersion = node.typeVersion_;
  s.label = node.label_;
  s.pos[0] = node.pos_[0];
  s.pos[1] = node.pos_[1];
  s.params = node.params_;
  s.literals = node.literals_;
  for (const NodeBlockRef &r : node.refs_) {
    s.refs.append(NodeState::Ref{r.name, r.cell.target, r.cell.kind == RefKind::Weak});
  }
  s.extras = node.extras_;
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
  return Status::error(
      StatusCode::NotFound,
      (std::string(what) + ": no node " + std::to_string(id.value)).c_str());
}

uint64_t GraphDef::stampNode(NodeId node)
{
  GraphEditNote e;
  e.kind = GraphEditNote::Kind::Node;
  e.def = serial_;
  e.node = node.value;
  contentVersion_ = rt_.ext<GraphSystem>().touch(e);
  return contentVersion_;
}

uint64_t GraphDef::stamp(bool structural, bool links)
{
  GraphSystem &system = rt_.ext<GraphSystem>();
  GraphEditNote e;
  e.kind = links ? GraphEditNote::Kind::Links : GraphEditNote::Kind::Def;
  e.def = serial_;
  contentVersion_ = system.touch(e);
  if (structural) {
    structureVersion_ = contentVersion_;
    system.noteStructure(structureVersion_);
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

void GraphDef::attachNode(GraphNode *n, GraphNode *after)
{
  n->def_ = this;
  n->prev_ = after;
  n->next_ = after ? after->next_ : first_;
  (n->prev_ ? n->prev_->next_ : first_) = n;
  (n->next_ ? n->next_->prev_ : last_) = n;
  index_.clear();
  byId_.add(n->id_.value, n);
  n->type_ = rt_.ext<NodeTypeRegistry>().find(n->typeName_.c_str());
  untyped_ += n->type_ ? 0 : 1;
}

void GraphDef::detachNode(GraphNode *n)
{
  (n->prev_ ? n->prev_->next_ : first_) = n->next_;
  (n->next_ ? n->next_->prev_ : last_) = n->prev_;
  index_.clear();
  byId_.remove(n->id_.value);
  untyped_ -= n->type_ ? 0 : 1;
}

void GraphDef::typeAdded(const NodeType &type)
{
  if (untyped_ == 0) {
    return;
  }
  for (GraphNode *n = first_; n; n = n->next_) {
    if (!n->type_ && strcmp(n->typeName_.c_str(), type.name.c_str()) == 0) {
      n->type_ = &type;
      untyped_--;
      matchParamRefs(n);
      refsVersion_ = rt_.nextVersion();
      if (Library *lib = lib_.get()) {
        lib->noteRefsChanged();
      }
    }
  }
}

void GraphDef::matchParamRefs(GraphNode *n)
{
  if (!n->type_) {
    return;
  }
  for (const NodeParamDef &p : n->type_->params) {
    bool have = p.def.kind() != PropKind::Pointer;
    for (NodeBlockRef &r : n->refs_) {
      if (!have && strcmp(r.name.c_str(), p.def.apiName().c_str()) == 0) {
        r.cell.kind = p.weak ? RefKind::Weak : RefKind::OptStrong;
        have = true;
      }
    }
    if (!have) {
      NodeBlockRef ref;
      ref.name = p.def.apiName();
      ref.cell.kind = p.weak ? RefKind::Weak : RefKind::OptStrong;
      n->refs_.append(ref);
    }
  }
}

GraphLink *GraphDef::attachLink(const LinkState &state, GraphNode *from, GraphNode *to)
{
  GraphLink *l = litestl::alloc::New<GraphLink>("coredcc::GraphLink");
  l->id = state.id;
  l->from = from;
  l->fromKey = state.fromKey;
  l->to = to;
  l->toKey = state.toKey;
  from->interiorLinks_ += isInteriorKey(l->fromKey.c_str());
  to->interiorLinks_ += isInteriorKey(l->toKey.c_str());
  l->outSlot = int(from->out_.size());
  l->inSlot = int(to->in_.size());
  from->out_.append(l);
  to->in_.append(l);
  links_.add(l->id.value, l);
  linkIndex_.clear();
  if (to->inByKey_) {
    to->inByKey_->add({l->toKey}, l);
  } else if (to->in_.size() > GraphNode::kScan) {
    to->inByKey_ = std::make_unique<Map<detail::SocketKey, GraphLink *>>();
    for (GraphLink *in : to->in_) {
      to->inByKey_->add({in->toKey}, in);
    }
  }
  return l;
}

void GraphDef::detachLink(GraphLink *link)
{
  link->from->interiorLinks_ -= isInteriorKey(link->fromKey.c_str());
  link->to->interiorLinks_ -= isInteriorKey(link->toKey.c_str());
  /* The last one takes its place. */
  auto remove = [link](Vector<GraphLink *, 2> &list, int GraphLink::*slot) {
    GraphLink *last = list.pop_back();
    if (last != link) {
      last->*slot = link->*slot;
      list[link->*slot] = last;
    }
  };
  remove(link->from->out_, &GraphLink::outSlot);
  remove(link->to->in_, &GraphLink::inSlot);
  if (link->to->inByKey_) {
    link->to->inByKey_->remove({link->toKey});
  }
  links_.remove(link->id.value);
  linkIndex_.clear();
  litestl::alloc::Delete(link);
}

void GraphDef::forEachRef(RefVisitor &visitor)
{
  for (GraphNode *n = first_; n; n = n->next_) {
    if (isType(*n, kGraphGroupType) || isType(*n, kGraphForeachType)) {
      visitor.visit(n->group_);
    }
    for (NodeBlockRef &r : n->refs_) {
      visitor.visit(r.cell);
    }
    for (BlockEntry &e : n->extras_) {
      for (RefCell &cell : e.refs) {
        visitor.visit(cell);
      }
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
    if (g.targetInterface != (target ? target->interfaceVersion_ : 0) ||
        g.sockets != (target ? target->interfaceVersion_ : g.node->socketsVersion_))
    {
      return false;
    }
    for (const GraphTopology::Interior &i : g.interior) {
      const std::optional<SocketInfo> s = findInterior(*this, *g.node, i.side, i.key);
      if (s.has_value() != i.found ||
          (s && strcmp(s->typeName.c_str(), i.typeName.c_str()) != 0))
      {
        return false;
      }
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
    return Status::error(StatusCode::Internal,
                         (std::string("GraphDef: ") + what).c_str());
  };
  int live = 0, outputs = 0, untyped = 0;
  for (const GraphNode *n = first_, *prev = nullptr; n; prev = n, n = n->next_) {
    outputs += isType(*n, kGraphOutputType);
    untyped += n->type_ ? 0 : 1;
    if (n->prev_ != prev || (!n->next_ && last_ != n) || node(n->id_) != n) {
      return bad("a node isn't where its id or the order says");
    }
    if (n->type_ != rt_.ext<NodeTypeRegistry>().find(n->typeName_.c_str())) {
      return bad("a node's type isn't the registered one");
    }
    if (index_.size() > 0 && (live >= int(index_.size()) || index_[live] != n)) {
      return bad("the index of nodes is out of date");
    }
    live++;
    for (int i = 0; i < int(n->in_.size()); i++) {
      const GraphLink *l = n->in_[i];
      if (l->to != n || l->inSlot != i || link(l->id) != l ||
          n->linkInto(l->toKey.c_str()) != l)
      {
        return bad("an incoming link is wrong, or shares its input");
      }
    }
    int interior = 0;
    for (const GraphLink *l : n->in_) {
      interior += isInteriorKey(l->toKey.c_str());
    }
    for (const GraphLink *l : n->out_) {
      interior += isInteriorKey(l->fromKey.c_str());
    }
    if (interior != n->interiorLinks_ ||
        (n->inByKey_ && n->inByKey_->size() != n->in_.size()))
    {
      return bad("the links of a node and their counts differ");
    }
    for (int i = 0; i < int(n->out_.size()); i++) {
      if (n->out_[i]->from != n || n->out_[i]->outSlot != i ||
          link(n->out_[i]->id) != n->out_[i])
      {
        return bad("an outgoing link is wrong");
      }
    }
  }
  if (live != int(byId_.size()) || untyped != untyped_ || (live == 0 && last_) ||
      outputs > 1)
  {
    return bad("node counts differ, or there are two graph.output nodes");
  }
  Status status;
  forEachLink([&](const GraphLink &l) {
    if (l.outSlot >= int(l.from->out_.size()) || l.from->out_[l.outSlot] != &l ||
        l.inSlot >= int(l.to->in_.size()) || l.to->in_[l.inSlot] != &l)
    {
      status = bad("a link isn't in its nodes' lists");
    }
  });
  return status;
}

} // namespace coredcc
