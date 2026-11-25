#include "coredcc/graph/topology.h"

#include "coredcc/base/text.h"

#include <cstring>

namespace coredcc {

using litestl::util::Map;
using litestl::util::string;
using litestl::util::Vector;

uint64_t GraphTopology::SocketAt::computeHash() const
{
  return (detail::fnv1a64(key.c_str()) ^ node.computeHash()) * 0xC2B2AE3D27D4EB4Full +
         uint64_t(side);
}

bool GraphTopology::SocketAt::operator==(const SocketAt &b) const
{
  return node == b.node && side == b.side && strcmp(key.c_str(), b.key.c_str()) == 0;
}

void GraphTopology::indexSockets(const Node &n)
{
  /* Of two with one key, the first counts, as in a scan: add doesn't overwrite. */
  for (int i = 0; n.inCount > kScan && i < n.inCount; i++) {
    wide_.add(SocketAt{n.id, SocketSide::Input, inputs[n.inBegin + i].key}, i);
  }
  for (int i = 0; n.outCount > kScan && i < n.outCount; i++) {
    wide_.add(SocketAt{n.id, SocketSide::Output, outputs[n.outBegin + i].key}, i);
  }
}

int GraphTopology::wideIndex(const Node &n, SocketSide side, std::string_view key) const
{
  const int *i = wide_.lookup_ptr(SocketAt{n.id, side, lsString(key)});
  return i ? *i : -1;
}

int GraphTopology::inputIndex(const Node &n, std::string_view key) const
{
  if (n.inCount > kScan) {
    return wideIndex(n, SocketSide::Input, key);
  }
  for (int i = 0; i < n.inCount; i++) {
    if (key == inputs[n.inBegin + i].key.c_str()) {
      return i;
    }
  }
  return -1;
}

int GraphTopology::outputIndex(const Node &n, std::string_view key) const
{
  if (n.outCount > kScan) {
    return wideIndex(n, SocketSide::Output, key);
  }
  for (int i = 0; i < n.outCount; i++) {
    if (key == outputs[n.outBegin + i].key.c_str()) {
      return i;
    }
  }
  return -1;
}

void GraphTopology::addInterior(const GraphDef &def, Node &n, int index)
{
  const SocketTypeRegistry &types = def.runtime().ext<SocketTypeRegistry>();
  Map<InteriorKey, bool> seen[2];
  auto use = [&](SocketSide side, const string &text, bool works) {
    const std::optional<InteriorKey> key = InteriorKey::parse(text.c_str());
    if (!key || !seen[int(side)].add(*key, true)) {
      return;
    }
    Vector<Interior, 1> &list = groups.last().interior;
    Interior i;
    i.key = *key;
    i.side = side;
    if (const std::optional<SocketInfo> s = findInterior(def, *n.node, side, *key)) {
      i.found = true;
      i.typeName = s->typeName;
      const SocketType *type = types.find(s->typeName.c_str());
      if (side == SocketSide::Output) {
        i.index = n.outCount++;
        groups.last().interiorOuts.append(int(list.size()));
        outputs.append(Output{text, type});
      }
      else if (works) {
        i.index = n.inCount++;
        Input in;
        in.key = text;
        in.type = type;
        in.node = index;
        inputs.append(std::move(in));
      }
    }
    list.append(std::move(i));
  };
  for (const GraphLink *l : n.node->linksIn()) {
    const bool works = def.findSocket(*l->from, SocketSide::Output, l->fromKey.c_str()).has_value();
    use(SocketSide::Input, l->toKey, works);
  }
  for (const GraphLink *l : n.node->linksOut()) {
    use(SocketSide::Output, l->fromKey, true);
  }
}

std::shared_ptr<const GraphTopology> GraphTopology::build(const GraphDef &def)
{
  Runtime &rt = def.runtime();
  const SocketTypeRegistry &types = rt.ext<SocketTypeRegistry>();
  std::shared_ptr<GraphTopology> t = std::make_shared<GraphTopology>();
  t->serial = rt.nextVersion();
  t->structure = def.structureVersion();
  t->nodeTypes = rt.ext<NodeTypeRegistry>().stamp();
  t->socketTypes = types.stamp();

  def.forEachNode([&](const GraphNode &n) {
    Node out;
    out.node = &n;
    out.id = n.id();
    /* First: finding the sockets finds the type. */
    const SocketInfos &ins = def.sockets(n, SocketSide::Input);
    const SocketInfos &outs = def.sockets(n, SocketSide::Output);
    out.type = n.type();
    out.role = n.type() ? n.type()->role : NodeRole::Kernel;
    out.inBegin = int(t->inputs.size());
    out.inCount = int(ins.size());
    out.outBegin = int(t->outputs.size());
    out.outCount = int(outs.size());
    out.interfaceIns = out.inCount;
    out.interfaceOuts = out.outCount;
    const int index = int(t->nodes.size());
    for (const SocketInfo &s : ins) {
      Input in;
      in.node = index;
      in.key = s.key;
      in.type = types.find(s.typeName.c_str());
      t->inputs.append(std::move(in));
    }
    for (const SocketInfo &s : outs) {
      t->outputs.append(Output{s.key, types.find(s.typeName.c_str())});
    }
    if (hasGroup(out.role)) {
      out.group = int(t->groups.size());
      const GraphDef *target = def.groupTarget(n);
      t->groups.append(Group{&n, index, target ? target->interfaceVersion() : 0, n.socketsVersion(), {}, {}});
      if (out.role == NodeRole::Group) {
        t->addInterior(def, out, index);
      }
    }
    else if (out.role == NodeRole::Output) {
      t->outputNode = index;
    }
    t->indexSockets(out);
    t->indexOf.add(n.id().value, index);
    t->nodes.append(out);
  });

  for (int i = 0; i < int(t->nodes.size()); i++) {
    const Node &n = t->nodes[i];
    for (const GraphLink *l : n.node->linksIn()) {
      const int to = t->inputIndex(n, l->toKey.c_str());
      const int from = t->find(l->from->id());
      const int out = t->outputIndex(t->nodes[from], l->fromKey.c_str());
      if (to >= 0 && out >= 0) {
        t->inputs[n.inBegin + to].fromNode = from;
        t->inputs[n.inBegin + to].fromOutput = out;
        if (t->nodes[from].role == NodeRole::Input) {
          t->fromInterface.append(n.inBegin + to);
        }
      }
    }
  }
  return t;
}

} // namespace coredcc
