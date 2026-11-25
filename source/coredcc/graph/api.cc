/* Graphs in datapaths: the structs coredcc.GraphDef, GraphNode, GraphLink, GraphSocket and
 * GraphInterface, and those of each node type. */
#include "coredcc/graph/api.h"

#include "coredcc/api/struct.h"
#include "coredcc/base/assert.h"
#include "coredcc/base/text.h"
#include "coredcc/graph/def.h"
#include "coredcc/graph/provider.h"
#include "coredcc/lib/library.h"

#include <cstring>
#include <string>

namespace coredcc {

using litestl::util::Vector;

namespace {

constexpr const char *kLinkStruct = "coredcc.GraphLink";
constexpr const char *kSocketStruct = "coredcc.GraphSocket";
constexpr const char *kInterfaceStruct = "coredcc.GraphInterface";

StructId structOf(const char *prefix, std::string_view type)
{
  return StructId::of(std::string(prefix) + ":" + std::string(type));
}

const SocketType *socketType(GraphDef &def, const litestl::util::string &name)
{
  return def.runtime().ext<SocketTypeRegistry>().find(name.c_str());
}

GraphDef &graphOf(const PointerRef &self)
{
  return *static_cast<GraphDef *>(self.data);
}
GraphNode &nodeOf(const PointerRef &self)
{
  return *static_cast<GraphNode *>(self.data);
}

PointerRef nodeRef(const GraphNode *n)
{
  PointerRef r;
  if (n) {
    r.data = const_cast<GraphNode *>(n);
    r.type = graphNodeStruct(*n);
  }
  return r;
}

/* `self` is a struct inside its owner. */
PointerRef inside(void *data, StructId type)
{
  PointerRef r;
  r.data = data;
  r.type = type;
  return r;
}

ApiStruct &readOnly(ApiStruct &st,
                    PropertyDef def,
                    litestl::util::function<PropValue(const PointerRef &, Context &)> get)
{
  ValueAccessor a;
  a.get = std::move(get);
  st.add(std::move(def), std::move(a));
  return st;
}

/* ---- pointers to blocks ---- */

/* What a pointer member shows for a cell: the block's root, or null when it isn't Live, or
 * is the graph's own block (which only a direct write of the cell can make it). */
PointerRef blockRef(const GraphDef &def, const RefCell &cell)
{
  const Library *lib = def.library();
  const ClientRegistry &clients = def.runtime().ext<ClientRegistry>();
  PointerRef r;
  if (!lib || cell.isNull() ||
      (!def.self().isNull() && clients.canonical(cell.target) == clients.canonical(def.self())))
  {
    return r;
  }
  const BlockHandle h = lib->resolveRef(cell);
  const Datablock *b = lib->get(h);
  const BlockTypeRegistry::Entry *type =
      b ? def.runtime().ext<BlockTypeRegistry>().find(b->type()) : nullptr;
  if (type) {
    r.data = lib->data(h);
    r.type = type->info.dataStruct;
    r.owner = OwnerKey::ofBlock(b->id());
  }
  return r;
}

/* The block `target` is the data of. Null for a null target. */
Result<BlockId> blockOf(const GraphDef &def, const PointerRef &target)
{
  if (target.isNull()) {
    return BlockId();
  }
  const Library *lib = def.library();
  const bool block = lib && target.owner.kind == OwnerKey::Kind::Block;
  if (!block || lib->data(lib->find(target.owner.block)) != target.data) {
    return Status::error(StatusCode::InvalidArgument,
                         "only whole blocks of the graph's Library can be targets");
  }
  return target.owner.block;
}

RefCell *paramCell(GraphNode &n, const std::string &name)
{
  for (const NodeBlockRef &r : n.blockRefs()) {
    if (name == r.name.c_str()) {
      return const_cast<RefCell *>(&r.cell);
    }
  }
  return nullptr;
}

/* ---- members every struct of a kind has ---- */

CollectionAccessor nodesAccessor()
{
  CollectionAccessor c;
  c.length = [](const PointerRef &self, Context &) { return int64_t(graphOf(self).nodeCount()); };
  c.at = [](const PointerRef &self, Context &, int64_t i) {
    return nodeRef(graphOf(self).nodeAt(int(i)));
  };
  c.lookup = [](const PointerRef &self, Context &, const KeySegment &key) {
    if (key.kind == KeySegment::Kind::Handle) {
      return nodeRef(graphOf(self).node(NodeId{key.handle}));
    }
    const GraphNode *found = nullptr;
    bool unique = true;
    graphOf(self).forEachNode([&](const GraphNode &n) {
      if (strcmp(n.label().c_str(), key.text.c_str()) == 0) {
        unique = !found;
        found = &n;
      }
    });
    return nodeRef(unique ? found : nullptr);
  };
  c.keyOf = [](const PointerRef &, Context &, const PointerRef &item) {
    return KeySegment::ofHandle(nodeOf(item).id().value);
  };
  c.itemStruct = StructId::of(kGraphNodeStruct);
  return c;
}

CollectionAccessor linksAccessor()
{
  CollectionAccessor c;
  const StructId type = StructId::of(kLinkStruct);
  c.length = [](const PointerRef &self, Context &) { return int64_t(graphOf(self).linkCount()); };
  c.at = [type](const PointerRef &self, Context &, int64_t i) {
    PointerRef r;
    graphOf(self).forEachLink([&](const GraphLink &l) {
      if (i-- == 0) {
        r = inside(const_cast<GraphLink *>(&l), type);
      }
    });
    return r;
  };
  c.lookup = [type](const PointerRef &self, Context &, const KeySegment &key) {
    const GraphLink *l = key.kind == KeySegment::Kind::Handle ?
                             graphOf(self).link(LinkId{key.handle}) :
                             nullptr;
    return inside(const_cast<GraphLink *>(l), type);
  };
  c.keyOf = [](const PointerRef &, Context &, const PointerRef &item) {
    return KeySegment::ofHandle(static_cast<const GraphLink *>(item.data)->id.value);
  };
  c.itemStruct = type;
  return c;
}

PointerRef interfaceRef(const InterfaceSocket *s)
{
  PointerRef r;
  if (s) {
    r.data = const_cast<InterfaceSocket *>(s);
    r.type = structOf(kInterfaceStruct, s->typeName.c_str());
    /* Payload types have no struct of their own. */
    if (!s->def->runtime().ext<ApiRegistry>().find(r.type)) {
      r.type = StructId::of(kInterfaceStruct);
    }
  }
  return r;
}

CollectionAccessor interfaceAccessor(SocketSide side)
{
  CollectionAccessor c;
  c.length = [side](const PointerRef &self, Context &) {
    return int64_t(graphOf(self).interfaceSockets(side).size());
  };
  c.at = [side](const PointerRef &self, Context &, int64_t i) {
    return interfaceRef(&graphOf(self).interfaceSockets(side)[int(i)]);
  };
  c.lookup = [side](const PointerRef &self, Context &, const KeySegment &key) {
    const GraphDef &def = graphOf(self);
    if (key.kind != KeySegment::Kind::Handle) {
      return interfaceRef(def.interfaceSocket(side, key.text.c_str()));
    }
    for (const InterfaceSocket &s : def.interfaceSockets(side)) {
      if (s.id == key.handle) {
        return interfaceRef(&s);
      }
    }
    return PointerRef();
  };
  c.keyOf = [](const PointerRef &, Context &, const PointerRef &item) {
    return KeySegment::ofHandle(static_cast<const InterfaceSocket *>(item.data)->id);
  };
  c.itemStruct = StructId::of(kInterfaceStruct);
  return c;
}

/* ---- sockets of group and output nodes ---- */

const SocketInfo *infoOf(const GraphSocketAddress &a)
{
  for (const SocketInfo &s : a.node->def().sockets(*a.node, SocketSide::Input)) {
    if (strcmp(s.key.c_str(), a.key.c_str()) == 0) {
      return &s;
    }
  }
  return nullptr;
}
const GraphSocketAddress &addressOf(const PointerRef &self)
{
  return *static_cast<const GraphSocketAddress *>(self.data);
}

PointerRef socketRef(GraphNode &n, const SocketInfo &s)
{
  PointerRef r;
  r.data = const_cast<GraphSocketAddress *>(n.addressOf(s.key.c_str()));
  r.type = structOf(kSocketStruct, s.typeName.c_str());
  if (!n.def().runtime().ext<ApiRegistry>().find(r.type)) {
    r.type = StructId::of(kSocketStruct);
  }
  return r;
}

CollectionAccessor socketsAccessor()
{
  CollectionAccessor c;
  c.length = [](const PointerRef &self, Context &) {
    const GraphNode &n = nodeOf(self);
    return int64_t(n.def().sockets(n, SocketSide::Input).size());
  };
  c.at = [](const PointerRef &self, Context &, int64_t i) {
    GraphNode &n = nodeOf(self);
    return socketRef(n, n.def().sockets(n, SocketSide::Input)[int(i)]);
  };
  c.lookup = [](const PointerRef &self, Context &, const KeySegment &key) {
    GraphNode &n = nodeOf(self);
    const bool handle = key.kind == KeySegment::Kind::Handle;
    const std::string want = handle ? interfaceKey(key.handle).c_str() : key.text.c_str();
    for (const SocketInfo &s : n.def().sockets(n, SocketSide::Input)) {
      if (want == (handle ? s.key : s.name).c_str()) {
        return socketRef(n, s);
      }
    }
    return PointerRef();
  };
  c.keyOf = [](const PointerRef &, Context &, const PointerRef &item) {
    return KeySegment::ofHandle(std::strtoll(addressOf(item).key.c_str() + 1, nullptr, 10));
  };
  c.itemStruct = StructId::of(kSocketStruct);
  return c;
}

/* Whether these are the sockets last seen of a group that isn't there. Which those are
 * isn't undoable, so a write through a path couldn't be undone. */
bool lost(const GraphSocketAddress &a)
{
  const NodeType *type = a.node->type();
  return type && hasGroup(type->role) && !a.node->def().groupTarget(*a.node);
}

PropValue socketValue(const GraphSocketAddress &a, const SocketType &type)
{
  return a.node->def().literalValue(*a.node, a.key.c_str(), type).toProp();
}

void addSocketStructs(ApiRegistry &reg, const SocketType &type)
{
  ApiStruct socket(std::string(kSocketStruct) + ":" + type.name.c_str());
  socket.parent(StructId::of(kSocketStruct));
  ValueAccessor value;
  value.get = [](const PointerRef &self, Context &) {
    const GraphSocketAddress &a = addressOf(self);
    const SocketInfo *s = infoOf(a);
    const SocketType *t = s ? socketType(a.node->def(), s->typeName) : nullptr;
    return t ? socketValue(a, *t) : PropValue();
  };
  value.set = [](const PointerRef &self, Context &, const PropValue &v) {
    const GraphSocketAddress &a = addressOf(self);
    return a.node->def().setLiteral(a.node->id(), a.key.c_str(), v);
  };
  socket.add(type.literalDef("value"), std::move(value))
      .dynamic([](PropertyDef &def, const PointerRef &self, Context &) {
        const GraphSocketAddress &a = addressOf(self);
        if (lost(a) || !a.node->def().usesLiteral(*a.node, a.key.c_str())) {
          def.access(PropAccess::ReadOnly);
        }
      });
  ValueAccessor isSet;
  isSet.get = [](const PointerRef &self, Context &) {
    const GraphSocketAddress &a = addressOf(self);
    return PropValue::ofBool(a.node->def().usesLiteral(*a.node, a.key.c_str()));
  };
  isSet.set = [](const PointerRef &self, Context &, const PropValue &v) {
    const GraphSocketAddress &a = addressOf(self);
    return a.node->def().useLiteral(a.node->id(), a.key.c_str(), v.asBool());
  };
  socket.add(PropertyDef::boolProp("isSet"), std::move(isSet))
      .dynamic([](PropertyDef &def, const PointerRef &self, Context &) {
        if (lost(addressOf(self))) {
          def.access(PropAccess::ReadOnly);
        }
      });
  bool ok = reg.add(std::move(socket)).ok();

  ApiStruct iface(std::string(kInterfaceStruct) + ":" + type.name.c_str());
  iface.parent(StructId::of(kInterfaceStruct));
  ValueAccessor def;
  def.get = [](const PointerRef &self, Context &) {
    const InterfaceSocket &s = *static_cast<const InterfaceSocket *>(self.data);
    const SocketType *t = socketType(*s.def, s.typeName);
    Result<GraphValue> v = GraphValue::fromProp(s.defaultValue);
    return v.ok() && t->holds(*v) ? s.defaultValue : t->defaultValue.toProp();
  };
  def.set = [](const PointerRef &self, Context &, const PropValue &v) {
    const InterfaceSocket &s = *static_cast<const InterfaceSocket *>(self.data);
    return s.def->setInterfaceDefault(s.id, v);
  };
  iface.add(type.literalDef("default"), std::move(def))
      .dynamic([](PropertyDef &d, const PointerRef &self, Context &) {
        const InterfaceSocket &s = *static_cast<const InterfaceSocket *>(self.data);
        bool input = false;
        for (const InterfaceSocket &i : s.def->interfaceSockets(SocketSide::Input)) {
          input |= i.id == s.id;
        }
        if (!input) {
          d.access(PropAccess::ReadOnly);
        }
      });
  ok = ok && reg.add(std::move(iface)).ok();
  CDCC_CHECK(ok, "registering a graph socket struct");
}

PropValue text(const litestl::util::string &s)
{
  return PropValue::ofString(s.c_str());
}

ApiStruct graphStruct()
{
  ApiStruct st(kGraphDefType);
  st.uiName("Graph");
  ValueAccessor name;
  name.get = [](const PointerRef &self, Context &) {
    const GraphDef &def = graphOf(self);
    const Library *lib = def.library();
    const Datablock *b = lib ? lib->get(lib->find(def.self())) : nullptr;
    return PropValue::ofString(b ? b->name().c_str() : "");
  };
  name.set = [](const PointerRef &self, Context &, const PropValue &v) {
    const GraphDef &def = graphOf(self);
    Library *lib = def.library();
    if (!lib || def.self().isNull()) {
      return Status::error(StatusCode::FailedPrecondition, "a standalone graph has no name");
    }
    return lib->rename(lib->find(def.self()), v.asString().c_str());
  };
  st.add(PropertyDef::stringProp("name"), std::move(name));
  st.addCollection(PropertyDef::collectionProp("nodes", StructId::of(kGraphNodeStruct)),
                   nodesAccessor());
  st.addCollection(PropertyDef::collectionProp("links", StructId::of(kLinkStruct)),
                   linksAccessor());
  st.addCollection(PropertyDef::collectionProp("inputs", StructId::of(kInterfaceStruct)),
                   interfaceAccessor(SocketSide::Input));
  st.addCollection(PropertyDef::collectionProp("outputs", StructId::of(kInterfaceStruct)),
                   interfaceAccessor(SocketSide::Output));
  return st;
}

ApiStruct nodeStruct()
{
  ApiStruct st(kGraphNodeStruct);
  st.uiName("Node");
  readOnly(st, PropertyDef::intProp("id"), [](const PointerRef &self, Context &) {
    return PropValue::ofInt(nodeOf(self).id().value);
  });
  readOnly(st, PropertyDef::stringProp("type"), [](const PointerRef &self, Context &) {
    return text(nodeOf(self).typeName());
  });
  ValueAccessor label;
  label.get = [](const PointerRef &self, Context &) { return text(nodeOf(self).label()); };
  label.set = [](const PointerRef &self, Context &, const PropValue &v) {
    return nodeOf(self).def().setLabel(nodeOf(self).id(), v.asString().c_str());
  };
  st.add(PropertyDef::stringProp("label"), std::move(label));
  ValueAccessor position;
  position.get = [](const PointerRef &self, Context &) {
    const double c[2] = {nodeOf(self).x(), nodeOf(self).y()};
    return PropValue::ofComponents(PropValue::Type::Vec2, c);
  };
  position.set = [](const PointerRef &self, Context &, const PropValue &v) {
    return nodeOf(self).def().setPosition(nodeOf(self).id(), v.component(0), v.component(1));
  };
  st.add(PropertyDef::vecProp("position", 2), std::move(position));
  return st;
}

ApiStruct linkStruct()
{
  ApiStruct st(kLinkStruct);
  st.uiName("Link");
  auto link = [](const PointerRef &self) -> const GraphLink & {
    return *static_cast<const GraphLink *>(self.data);
  };
  readOnly(st, PropertyDef::intProp("id"), [link](const PointerRef &self, Context &) {
    return PropValue::ofInt(link(self).id.value);
  });
  readOnly(st, PropertyDef::intProp("from"), [link](const PointerRef &self, Context &) {
    return PropValue::ofInt(link(self).from->id().value);
  });
  readOnly(st, PropertyDef::stringProp("fromSocket"), [link](const PointerRef &self, Context &) {
    return text(link(self).fromKey);
  });
  readOnly(st, PropertyDef::intProp("to"), [link](const PointerRef &self, Context &) {
    return PropValue::ofInt(link(self).to->id().value);
  });
  readOnly(st, PropertyDef::stringProp("toSocket"), [link](const PointerRef &self, Context &) {
    return text(link(self).toKey);
  });
  return st;
}

ApiStruct socketStruct()
{
  ApiStruct st(kSocketStruct);
  st.uiName("Socket");
  readOnly(st, PropertyDef::stringProp("name"), [](const PointerRef &self, Context &) {
    const SocketInfo *s = infoOf(addressOf(self));
    return s ? text(s->name) : PropValue::ofString("");
  });
  readOnly(st, PropertyDef::stringProp("type"), [](const PointerRef &self, Context &) {
    const SocketInfo *s = infoOf(addressOf(self));
    return s ? text(s->typeName) : PropValue::ofString("");
  });
  return st;
}

ApiStruct interfaceStruct()
{
  ApiStruct st(kInterfaceStruct);
  st.uiName("Interface Socket");
  auto socket = [](const PointerRef &self) -> const InterfaceSocket & {
    return *static_cast<const InterfaceSocket *>(self.data);
  };
  ValueAccessor name;
  name.get = [socket](const PointerRef &self, Context &) { return text(socket(self).name); };
  name.set = [socket](const PointerRef &self, Context &, const PropValue &v) {
    return socket(self).def->setInterfaceName(socket(self).id, v.asString().c_str());
  };
  st.add(PropertyDef::stringProp("name"), std::move(name));
  readOnly(st, PropertyDef::stringProp("type"), [socket](const PointerRef &self, Context &) {
    return text(socket(self).typeName);
  });
  return st;
}

/* ---- node types ---- */

ApiStruct paramsStruct(const NodeType &type)
{
  ApiStruct st(std::string(kGraphParamsStruct) + ":" + type.name.c_str());
  for (const NodeParamDef &p : type.params) {
    const std::string name = p.def.apiName().c_str();
    if (p.def.kind() == PropKind::Pointer) {
      PointerAccessor a;
      a.get = [name](const PointerRef &self, Context &) {
        const RefCell *cell = paramCell(nodeOf(self), name);
        return cell ? blockRef(nodeOf(self).def(), *cell) : PointerRef();
      };
      a.set = [name](const PointerRef &self, Context &, const PointerRef &target) {
        GraphDef &def = nodeOf(self).def();
        Result<BlockId> block = blockOf(def, target);
        if (!block.ok()) {
          return block.status();
        }
        StableRef ref;
        ref.owner = block->isNull() ? OwnerKey() : OwnerKey::ofBlock(*block);
        ref.type = target.type;
        return def.setParam(nodeOf(self).id(), name, PropValue::ofPointer(ref));
      };
      a.cell = [name](const PointerRef &self, Context &) { return paramCell(nodeOf(self), name); };
      a.target = p.def.target()->target;
      st.addPointer(p.def, std::move(a));
      continue;
    }
    ValueAccessor a;
    const PropValue unset = p.def.defaultValue();
    a.get = [name, unset](const PointerRef &self, Context &) {
      const PropValue v = nodeOf(self).def().paramValue(nodeOf(self), name);
      return v.isNone() ? unset : v;
    };
    a.set = [name](const PointerRef &self, Context &, const PropValue &v) {
      return nodeOf(self).def().setParam(nodeOf(self).id(), name, v);
    };
    st.add(p.def, std::move(a));
  }
  return st;
}

ApiStruct inputsStruct(const NodeType &type)
{
  ApiStruct st(std::string(kGraphInputsStruct) + ":" + type.name.c_str());
  for (int i = 0; i < int(type.inputs.size()); i++) {
    const PropertyDef *def = type.literal(i);
    if (!def) {
      continue; /* a payload */
    }
    const std::string name = def->apiName().c_str();
    const SocketType *socket = type.inputs[i].type;
    ValueAccessor a;
    a.get = [name, socket](const PointerRef &self, Context &) {
      const GraphNode &n = nodeOf(self);
      return n.def().literalValue(n, name, *socket).toProp();
    };
    a.set = [name](const PointerRef &self, Context &, const PropValue &v) {
      return nodeOf(self).def().setLiteral(nodeOf(self).id(), name, v);
    };
    st.add(*def, std::move(a));
  }
  return st;
}

/* A fixed pointer to another struct over the same node. */
void addPart(ApiStruct &st, const char *name, StructId type)
{
  PointerAccessor a;
  a.get = [type](const PointerRef &self, Context &) { return inside(self.data, type); };
  a.target = type;
  st.addPointer(PropertyDef::pointerProp(name, type), std::move(a));
}

} // namespace

StructId graphNodeStruct(const GraphNode &node)
{
  return node.type() ? structOf(kGraphNodeStruct, node.typeName().c_str()) :
                       StructId::of(kGraphNodeStruct);
}

void detail::addGraphStructs(Runtime &rt)
{
  ApiRegistry &reg = rt.ext<ApiRegistry>();
  bool ok = reg.add(graphStruct()).ok() && reg.add(nodeStruct()).ok() &&
            reg.add(linkStruct()).ok() && reg.add(socketStruct()).ok() &&
            reg.add(interfaceStruct()).ok();
  CDCC_CHECK(ok, "registering the graph structs");
  const SocketTypeRegistry &types = rt.ext<SocketTypeRegistry>();
  for (int t = 0; t < int(GraphValue::Type::Payload); t++) {
    if (const SocketType *type = types.builtin(GraphValue::Type(t))) {
      addSocketStructs(reg, *type);
    }
  }
}

Status detail::checkNodeStructs(Runtime &rt, std::string_view type)
{
  const ApiRegistry &reg = rt.ext<ApiRegistry>();
  for (const char *prefix : {kGraphNodeStruct, kGraphParamsStruct, kGraphInputsStruct}) {
    if (reg.find(structOf(prefix, type))) {
      const std::string name = std::string(prefix) + ":" + std::string(type);
      return Status::error(StatusCode::AlreadyExists,
                           ("the struct '" + name + "' is registered already").c_str());
    }
  }
  return Status();
}

Status detail::addNodeStructs(Runtime &rt, const NodeType &type)
{
  ApiRegistry &reg = rt.ext<ApiRegistry>();
  ApiStruct st(std::string(kGraphNodeStruct) + ":" + type.name.c_str());
  st.uiName(type.uiName.c_str()).parent(StructId::of(kGraphNodeStruct));
  if (type.params.size() > 0 || type.role == NodeRole::Kernel) {
    CDCC_TRY(reg.add(paramsStruct(type)).status());
    addPart(st, "params", structOf(kGraphParamsStruct, type.name.c_str()));
  }
  if (type.role == NodeRole::Kernel) {
    CDCC_TRY(reg.add(inputsStruct(type)).status());
    addPart(st, "inputs", structOf(kGraphInputsStruct, type.name.c_str()));
  }
  if (hasGroup(type.role)) {
    PointerAccessor a;
    a.get = [](const PointerRef &self, Context &) {
      return blockRef(nodeOf(self).def(), nodeOf(self).group());
    };
    a.set = [](const PointerRef &self, Context &, const PointerRef &target) {
      Result<BlockId> block = blockOf(nodeOf(self).def(), target);
      return block.ok() ? nodeOf(self).def().setGroup(nodeOf(self).id(), *block) : block.status();
    };
    a.cell = [](const PointerRef &self, Context &) {
      return const_cast<RefCell *>(&nodeOf(self).group());
    };
    a.target = StructId::of(kGraphDefType);
    st.addPointer(PropertyDef::pointerProp("group", a.target), std::move(a));
  }
  if (hasGroup(type.role) || type.role == NodeRole::Output) {
    st.addCollection(PropertyDef::collectionProp("sockets", StructId::of(kSocketStruct)),
                     socketsAccessor());
  }
  return reg.add(std::move(st)).status();
}

} // namespace coredcc
