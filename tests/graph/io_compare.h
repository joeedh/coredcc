#pragma once

/* A comparison of two graphs that doesn't use the writer, and names that share an id. */

#include "graph_fixture.h"

#include "coredcc/graph/payload.h"
#include "coredcc/lib/id_map.h"

#include <algorithm>

namespace fixture {

/* Names of node types, and of socket types, whose FNV-1a hashes are equal. */
inline constexpr const char *kTwinNodes[] = {"AaEYXRBdJBF2UWXr", "Lg9Oe2I5HLNNPhBl"};
inline constexpr const char *kTwinSockets[] = {"ay3j90PqNS0HIiVz", "jg3fl09NHqXtN8Tx"};
struct Twin {};

} // namespace fixture

template <> struct coredcc::PayloadTraits<fixture::Twin> {
  static constexpr const char *name = fixture::kTwinSockets[0];
};

namespace fixture {

inline bool differs(const char *what, int64_t id)
{
  printf("graphs differ: %s of %lld\n", what, (long long)id);
  return false;
}

/* Enums in files are keys, so a param read by a build without its type is a string. */
inline PropValue shown(const GraphDef &d, const GraphNode &n, const NodeValue &v)
{
  const PropValue p = d.paramValue(n, v.name.c_str());
  return p.isNone() ? v.value : p;
}

inline bool sameSockets(const SocketInfos &a, const SocketInfos &b)
{
  return a.size() == b.size() &&
         std::equal(a.begin(), a.end(), b.begin(), [](const SocketInfo &x, const SocketInfo &y) {
           return x.key == y.key && x.name == y.name && x.typeName == y.typeName;
         });
}

/* Whether b is what a is, except for versions. With `ids`, b's references are to what those of
 * a became. */
inline bool sameGraph(const GraphDef &a, const GraphDef &b, const IdMap *ids = nullptr)
{
  const auto target = [ids](const BlockId &id) {
    const BlockId to = ids ? ids->find(id) : BlockId();
    return to.isNull() ? id : to;
  };
  for (const SocketSide side : {SocketSide::Input, SocketSide::Output}) {
    const auto &x = a.interfaceSockets(side), &y = b.interfaceSockets(side);
    if (x.size() != y.size()) {
      return differs("interface size", 0);
    }
    for (int i = 0; i < int(x.size()); i++) {
      if (x[i].id != y[i].id || x[i].name != y[i].name || x[i].typeName != y[i].typeName ||
          !(x[i].defaultValue == y[i].defaultValue) || y[i].def != &b || y[i].defaultVersion == 0)
      {
        return differs("interface", x[i].id);
      }
    }
  }
  if (a.nodeCount() != b.nodeCount() || a.linkCount() != b.linkCount()) {
    return differs("counts", 0);
  }
  for (int i = 0; i < a.nodeCount(); i++) {
    const GraphNode &x = *a.nodeAt(i), &y = *b.nodeAt(i);
    const int64_t id = x.id().value;
    if (x.id() != y.id() || x.typeName() != y.typeName() || x.label() != y.label() ||
        x.x() != y.x() || x.y() != y.y() || b.node(x.id()) != &y || &y.def() != &b)
    {
      return differs("node", id);
    }
    if (x.params().size() != y.params().size() || x.literals().size() != y.literals().size() ||
        x.blockRefs().size() != y.blockRefs().size() || y.paramVersion() == 0)
    {
      return differs("sizes", id);
    }
    for (int k = 0; k < int(x.params().size()); k++) {
      const NodeValue &v = x.params()[k], &w = y.params()[k];
      if (v.name != w.name || !(shown(a, x, v) == shown(b, y, w)) || w.version == 0) {
        return differs("param", id);
      }
    }
    for (int k = 0; k < int(x.literals().size()); k++) {
      const NodeValue &v = x.literals()[k], &w = y.literals()[k];
      if (v.name != w.name || !(v.value == w.value) || v.off != w.off || w.version == 0) {
        return differs("literal", id);
      }
    }
    for (int k = 0; k < int(x.blockRefs().size()); k++) {
      const NodeBlockRef &v = x.blockRefs()[k], &w = y.blockRefs()[k];
      if (v.name != w.name || v.cell.kind != w.cell.kind || target(v.cell.target) != w.cell.target) {
        return differs("reference", id);
      }
    }
    if (target(x.group().target) != y.group().target || x.group().kind != y.group().kind) {
      return differs("group", id);
    }
    for (const SocketSide side : {SocketSide::Input, SocketSide::Output}) {
      if (!sameSockets(a.sockets(x, side), b.sockets(y, side))) {
        return differs("sockets", id);
      }
    }
    if (x.linksIn().size() != y.linksIn().size() || x.linksOut().size() != y.linksOut().size()) {
      return differs("links", id);
    }
    for (int k = 0; k < int(x.linksIn().size()); k++) {
      const LinkState v = GraphDef::stateOf(*x.linksIn()[k]), w = GraphDef::stateOf(*y.linksIn()[k]);
      if (v.id != w.id || v.from != w.from || v.fromKey != w.fromKey || v.toKey != w.toKey ||
          b.link(v.id) != y.linksIn()[k] || y.linksIn()[k]->to != &y)
      {
        return differs("link", v.id.value);
      }
    }
  }
  return true;
}

} // namespace fixture
