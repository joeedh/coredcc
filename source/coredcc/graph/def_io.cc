#include "coredcc/graph/def_io.h"

#include "coredcc/base/text.h"
#include "coredcc/lib/library.h"
#include "coredcc/props/value_io.h"

#include <algorithm>
#include <cstring>
#include <set>
#include <string>
#include <utility>

namespace coredcc {

using litestl::util::string;
using litestl::util::Vector;

namespace {

/* Bytes that one of each takes at least. */
constexpr size_t kMinInterface = 8 + 4 + 4;
constexpr size_t kMinNode = 8 + 4 + 4 + 4 + 16 + 6 * 4;
constexpr size_t kMinRef = 4 + 1 + 4;
constexpr size_t kMinSocket = 3 * 4;
constexpr size_t kMinLink = 8 + 8 + 4 + 8 + 4;
/* Roughly what maps and lists take for a node or a link. */
constexpr size_t kIndexBytes = 256;

bool hasGroupRef(const string &type)
{
  return strcmp(type.c_str(), kGraphGroupType) == 0 || strcmp(type.c_str(), kGraphForeachType) == 0;
}

/* ---- writing ---- */

void writeSockets(BytesWriter &out, const SocketInfos &list)
{
  out.u32(uint32_t(list.size()));
  for (const SocketInfo &s : list) {
    out.str(s.key.c_str());
    out.str(s.name.c_str());
    out.str(s.typeName.c_str());
  }
}

BlockEntry entryOf(const string &name, PropValue value)
{
  BlockEntry e;
  e.name = name;
  e.value = std::move(value);
  return e;
}

/* A param as it is stored: enums by key. */
PropValue storedParam(const GraphNode &n, const NodeValue &v)
{
  const int i = n.type() ? n.type()->paramIndex(v.name.c_str()) : -1;
  return i >= 0 ? storedValue(n.type()->params[i].def, v.value) : v.value;
}

/* Equal graphs give equal bytes, so what a node has in no order is written in one. */
template <class T> bool byName(const T &a, const T &b)
{
  return strcmp(a.name.c_str(), b.name.c_str()) < 0;
}

Vector<const NodeValue *> sorted(const NodeValues &list)
{
  Vector<const NodeValue *> out;
  for (const NodeValue &v : list) {
    out.append(&v);
  }
  std::sort(out.begin(), out.end(), [](const NodeValue *a, const NodeValue *b) {
    const int c = strcmp(a->name.c_str(), b->name.c_str());
    return c != 0 ? c < 0 : a->value.type() < b->value.type();
  });
  return out;
}

void writeNode(const GraphDef &def, const GraphNode &n, BlockWriter &out)
{
  BytesWriter &b = out.bytes();
  b.i64(n.id().value);
  b.str(n.typeName().c_str());
  b.u32(n.typeVersion());
  b.str(n.label().c_str());
  b.f64(n.x());
  b.f64(n.y());
  Vector<BlockEntry> params, literals;
  const Vector<const NodeValue *> setParams = sorted(n.params()), setLiterals = sorted(n.literals());
  for (const NodeValue *v : setParams) {
    params.append(entryOf(v->name, storedParam(n, *v)));
  }
  for (const NodeValue *v : setLiterals) {
    literals.append(entryOf(v->name, v->value));
  }
  out.entries(std::span<const BlockEntry>(params.data(), size_t(params.size())));
  out.entries(std::span<const BlockEntry>(literals.data(), size_t(literals.size())));
  for (const NodeValue *v : setLiterals) {
    b.boolean(v->off);
  }
  b.u32(uint32_t(n.blockRefs().size()));
  for (const NodeBlockRef &r : n.blockRefs()) {
    b.str(r.name.c_str());
    b.boolean(r.cell.kind == RefKind::Weak);
    out.ref(r.cell);
  }
  Vector<BlockEntry> extras;
  for (const BlockEntry &e : n.extras()) {
    extras.append(e);
  }
  std::stable_sort(extras.begin(), extras.end(), byName<BlockEntry>);
  out.entries(std::span<const BlockEntry>(extras.data(), size_t(extras.size())));
  if (hasGroupRef(n.typeName())) {
    out.ref(n.group());
  }
  writeSockets(b, def.sockets(n, SocketSide::Input));
  writeSockets(b, def.sockets(n, SocketSide::Output));
}

} // namespace

void writeGraphDef(const GraphDef &def, BlockWriter &out)
{
  BytesWriter &b = out.bytes();
  for (const SocketSide side : {SocketSide::Input, SocketSide::Output}) {
    const Vector<InterfaceSocket> &list = def.interfaceSockets(side);
    b.u32(uint32_t(list.size()));
    for (const InterfaceSocket &s : list) {
      b.i64(s.id);
      b.str(s.name.c_str());
      b.str(s.typeName.c_str());
      if (side == SocketSide::Input) {
        out.entry(entryOf(string(), s.defaultValue));
      }
    }
  }
  b.u32(uint32_t(def.nodeCount()));
  def.forEachNode([&](const GraphNode &n) { writeNode(def, n, out); });
  /* By node and id: the def has no order of links. */
  b.u32(uint32_t(def.linkCount()));
  def.forEachNode([&](const GraphNode &n) {
    Vector<const GraphLink *> links;
    for (const GraphLink *l : n.linksIn()) {
      links.append(l);
    }
    std::sort(links.begin(), links.end(),
              [](const GraphLink *a, const GraphLink *c) { return a->id < c->id; });
    for (const GraphLink *l : links) {
      b.i64(l->id.value);
      b.i64(l->from->id().value);
      b.str(l->fromKey.c_str());
      b.i64(l->to->id().value);
      b.str(l->toKey.c_str());
    }
  });
}

/* ---- reading ---- */

class GraphDefReader {
 public:
  GraphDefReader(BlockReader &in, GraphDef &def)
      : in_(in), b_(in.bytes()), def_(def), version_(def.contentVersion_)
  {
  }

  void read()
  {
    readInterface(SocketSide::Input);
    readInterface(SocketSide::Output);
    const uint32_t nodes = b_.count(kMinNode);
    in_.charge(uint64_t(nodes) * (sizeof(GraphNode) + kIndexBytes));
    for (uint32_t i = 0; i < nodes && b_.ok(); i++) {
      readNode();
    }
    const uint32_t links = b_.count(kMinLink);
    in_.charge(uint64_t(links) * (sizeof(GraphLink) + kIndexBytes));
    for (uint32_t i = 0; i < links && b_.ok(); i++) {
      readLink();
    }
  }

 private:
  void check(bool ok, const char *what)
  {
    if (!ok) {
      b_.fail(StatusCode::InvalidArgument, what);
    }
  }
  string text()
  {
    return lsString(b_.str());
  }
  /* An id that nothing before has. */
  int64_t id()
  {
    const int64_t id = b_.i64();
    check(id > 0 && id < int64_t(1) << 52, "a graph: an id that isn't in 1 to 2^52-1");
    check(ids_.insert(id).second, "a graph: an id that is there twice");
    return id;
  }
  /* The name of a node or socket type. Ids are name hashes, so a file can have a name with
   * the id of another, which would be taken for it. */
  string typeName(const char *kind)
  {
    const string name = text();
    const std::string idName = std::string(kind) + ":" + name.c_str();
    const uint64_t id = TypeId::of(idName).value;
    Library &lib = in_.library();
    const char *known = lib.runtime().nameOf(IdDomain::Type, id);
    if (!known && !lib.fileNames().add(IdDomain::Type, idName)) {
      known = lib.fileNames().nameOf(IdDomain::Type, id); /* null: the table is full */
    }
    check(!known || idName == known, "a graph: a type name that has the id of another");
    return name;
  }

  PropValue value(BlockEntry &e)
  {
    if (e.unknownTag) {
      b_.fail(StatusCode::Unsupported, "a graph: a value of a type that isn't known");
    }
    check(e.refs.size() == 0 && e.value.type() != PropValue::Type::Pointer,
          "a graph: a pointer among the values");
    return std::move(e.value);
  }
  /* Params are unique by name, literals by name and type. */
  void readValues(NodeValues &out, bool byType)
  {
    Vector<BlockEntry> entries;
    in_.entries(entries);
    std::set<std::pair<std::string, PropValue::Type>> seen;
    for (BlockEntry &e : entries) {
      NodeValue v;
      v.name = e.name;
      v.value = value(e);
      v.version = version_;
      const PropValue::Type type = byType ? v.value.type() : PropValue::Type::None;
      check(!v.value.isNone(), "a graph: a value that isn't set");
      check(seen.insert({v.name.c_str(), type}).second, "a graph: a value that is there twice");
      out.append(std::move(v));
    }
  }
  /* Kept as they are. */
  void readExtras(NodeExtras &out)
  {
    Vector<BlockEntry> entries;
    in_.entries(entries);
    std::set<std::string> seen;
    for (BlockEntry &e : entries) {
      check(e.name.size() > 0, "a graph: an extra without a name");
      check(seen.insert(e.name.c_str()).second, "a graph: an extra that is there twice");
      out.append(std::move(e));
    }
  }
  void readSockets(SocketInfos &out, bool keep)
  {
    const uint32_t n = b_.count(kMinSocket);
    in_.charge(uint64_t(n) * sizeof(SocketInfo) * 2);
    for (uint32_t i = 0; i < n && b_.ok(); i++) {
      SocketInfo s;
      s.key = text();
      check(s.key.size() > 0, "a graph: a socket without a key");
      s.name = text();
      s.typeName = typeName("socket");
      if (keep) {
        out.append(std::move(s));
      }
    }
  }

  void readInterface(SocketSide side)
  {
    Vector<InterfaceSocket> &list = side == SocketSide::Input ? def_.ifaceIn_ : def_.ifaceOut_;
    std::set<std::string> names;
    const uint32_t n = b_.count(kMinInterface);
    in_.charge(uint64_t(n) * sizeof(InterfaceSocket) * 2);
    for (uint32_t i = 0; i < n && b_.ok(); i++) {
      InterfaceSocket s;
      s.def = &def_;
      s.id = id();
      s.name = text();
      s.typeName = typeName("socket");
      s.defaultVersion = version_;
      check(names.insert(s.name.c_str()).second, "a graph: an interface name that is there twice");
      check(isInterfaceName(s.name.c_str()), "a graph: a name interface sockets can't have");
      if (side == SocketSide::Input) {
        Vector<BlockEntry> entry;
        in_.entries(entry);
        check(entry.size() == 1, "a graph: an interface input without its default");
        if (b_.ok()) {
          s.defaultValue = value(entry[0]);
        }
      }
      list.append(std::move(s));
    }
  }

  void readNode()
  {
    GraphNode *n = litestl::alloc::New<GraphNode>("coredcc::GraphNode");
    n->id_ = NodeId{id()};
    n->typeName_ = typeName("node");
    def_.attachNode(n, def_.last_); /* which owns it now */
    n->typeVersion_ = b_.u32();
    check(n->typeVersion_ > 0, "a graph: a node type of version 0");
    if (strcmp(n->typeName_.c_str(), kGraphOutputType) == 0) {
      check(!hasOutput_, "a graph: a second graph.output");
      hasOutput_ = true;
    }
    n->label_ = text();
    n->pos_[0] = b_.f64();
    n->pos_[1] = b_.f64();
    n->paramVersion_ = version_;
    readValues(n->params_, false);
    readValues(n->literals_, true);
    for (NodeValue &v : n->literals_) {
      v.off = b_.boolean();
    }
    std::set<std::string> names;
    const uint32_t refs = b_.count(kMinRef);
    in_.charge(uint64_t(refs) * sizeof(NodeBlockRef) * 2);
    for (uint32_t i = 0; i < refs && b_.ok(); i++) {
      NodeBlockRef r;
      r.name = text();
      r.cell.kind = b_.boolean() ? RefKind::Weak : RefKind::OptStrong;
      in_.ref(r.cell);
      check(names.insert(r.name.c_str()).second, "a graph: a reference that is there twice");
      n->refs_.append(r);
    }
    readExtras(n->extras_);
    if (hasGroupRef(n->typeName_)) {
      in_.ref(n->group_);
    }
    /* A kernel's type says what its sockets are. */
    const bool keep = !n->type_ || n->type_->role != NodeRole::Kernel;
    readSockets(n->inputs_, keep);
    readSockets(n->outputs_, keep);
    def_.matchParamRefs(n);
  }

  void readLink()
  {
    LinkState s;
    s.id = LinkId{id()};
    s.from = NodeId{b_.i64()};
    s.fromKey = text();
    s.to = NodeId{b_.i64()};
    s.toKey = text();
    GraphNode *const *from = def_.byId_.lookup_ptr(s.from.value);
    GraphNode *const *to = def_.byId_.lookup_ptr(s.to.value);
    check(from && to, "a graph: a link to a node that isn't there");
    check(linked_.insert({s.to.value, s.toKey.c_str()}).second,
          "a graph: an input with two links");
    if (b_.ok()) {
      def_.attachLink(s, *from, *to);
    }
  }

  BlockReader &in_;
  BytesReader &b_;
  GraphDef &def_;
  uint64_t version_;
  std::set<int64_t> ids_;
  std::set<std::pair<int64_t, std::string>> linked_;
  bool hasOutput_ = false;
};

GraphDef *readGraphDef(BlockReader &in, BlockId self)
{
  GraphDef *def = litestl::alloc::New<GraphDef>(
      "coredcc::GraphDef", &in.library().runtime(), &in.library(), self);
  GraphDefReader(in, *def).read();
  if (!in.bytes().ok()) {
    litestl::alloc::Delete(def);
    return nullptr;
  }
  return def;
}

} // namespace coredcc
