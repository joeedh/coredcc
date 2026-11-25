#include "coredcc/graph/topology.h"

namespace coredcc {

int GraphTopology::inputIndex(const Node &n, std::string_view key) const
{
  for (int i = 0; i < n.inCount; i++) {
    if (key == inputs[n.inBegin + i].key.c_str()) {
      return i;
    }
  }
  return -1;
}

int GraphTopology::outputIndex(const Node &n, std::string_view key) const
{
  for (int i = 0; i < n.outCount; i++) {
    if (key == outputs[n.outBegin + i].key.c_str()) {
      return i;
    }
  }
  return -1;
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
      t->groups.append(Group{&n, index, target ? target->interfaceVersion() : 0});
    }
    else if (out.role == NodeRole::Output) {
      t->outputNode = index;
    }
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
      }
    }
  }
  return t;
}

} // namespace coredcc
