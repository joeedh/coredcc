/* GraphDef: linkAll, nodes with many links, and the limits of interfaces and interior links. */
#include "def_fixture.h"

#include "coredcc/graph/interior.h"

#include <algorithm>
#include <set>
#include <vector>

using namespace fixture;

test_init;

namespace {

std::string inner(NodeId n, const char *socket)
{
  return "@" + std::to_string(n.value) + "." + socket;
}

/* All links, as from.key>to.key. */
std::set<std::string> linksOf(const GraphDef &d)
{
  std::set<std::string> out;
  d.forEachNode([&](const GraphNode &n) {
    for (const GraphLink *l : n.linksIn()) {
      out.insert(std::to_string(l->from->id().value) + "." + l->fromKey.c_str() + ">" +
                 std::to_string(l->to->id().value) + "." + l->toKey.c_str());
      test_assert(d.link(l->id) == l && l->id.value > 0);
    }
  });
  return out;
}

void testLinkAll()
{
  World w;
  const Graph g = newGraph(w.lib, "g");
  GraphDef &d = *g.def;
  std::vector<NodeId> n;
  for (int i = 0; i < 6; i++) {
    n.push_back(add(d, "test.two"));
  }
  litestl::util::Vector<LinkId> ids;
  const std::vector<LinkSpec> specs = {{{n[0], "x"}, {n[1], "a"}},
                                       {{n[1], "y"}, {n[2], "a"}},
                                       {{n[0], "y"}, {n[2], "b"}}};
  const uint64_t edit = w.rt.lastEdit();
  test_assert(d.linkAll(specs, ids).ok() && ids.size() == 3 && d.linkCount() == 3);
  test_assert(w.rt.lastEdit() > edit);
  for (int i = 0; i < 3; i++) {
    const GraphLink *l = d.link(ids[i]);
    const LinkSpec &s = specs[size_t(i)];
    test_assert(l && l->from->id() == s.from.node && l->to->id() == s.to.node);
    test_assert(l->fromKey.c_str() == s.from.key && l->toKey.c_str() == s.to.key);
  }
  const std::set<std::string> before = linksOf(d);
  const LinkId old = ids[1];

  /* Nothing of a batch with an error in it stays, whatever it replaced. */
  auto refused = [&](const std::vector<LinkSpec> &list, StatusCode code) {
    return hasCode(d.linkAll(list, ids), code) && ids.size() == 0 && linksOf(d) == before &&
           d.linkCount() == 3 && d.node(n[2])->linkInto("a")->id == old && d.validate().ok();
  };
  const LinkSpec replace{{n[0], "x"}, {n[2], "a"}}, more{{n[2], "x"}, {n[3], "a"}};
  test_assert(refused({replace, more, {{NodeId{999}, "x"}, {n[4], "a"}}}, StatusCode::NotFound));
  test_assert(refused({replace, more, {{n[0], "nothing"}, {n[4], "a"}}}, StatusCode::NotFound));
  test_assert(refused({replace, more, {{n[0], "x"}, {n[4], "nothing"}}}, StatusCode::NotFound));
  test_assert(refused({replace, more, {{n[0], "m"}, {n[4], "a"}}}, StatusCode::TypeMismatch));
  test_assert(refused({replace, more, {{n[0], "y"}, {n[2], "a"}}}, StatusCode::InvalidArgument));
  test_assert(refused({replace, more, more}, StatusCode::InvalidArgument));
  /* Cycles through links of the batch, through one that stays, and of one node. */
  test_assert(refused({replace, more, {{n[3], "x"}, {n[2], "b"}}}, StatusCode::InvalidArgument));
  test_assert(refused({more, {{n[3], "y"}, {n[0], "a"}}}, StatusCode::InvalidArgument));
  test_assert(refused({replace, {{n[4], "x"}, {n[4], "a"}}}, StatusCode::InvalidArgument));
  /* Its error names a link that is on the cycle. */
  const std::vector<LinkSpec> list = {replace, more, {{n[4], "y"}, {n[4], "b"}}};
  test_assert(strcmp(d.linkAll(list, ids).message(), "linking 'y' to 'b' would make a cycle") == 0);

  /* One that breaks a cycle by what it replaces is fine. */
  const std::vector<LinkSpec> turn = {{{n[2], "x"}, {n[0], "a"}},
                                      {{n[5], "x"}, {n[1], "a"}},
                                      {{n[5], "x"}, {n[2], "b"}}};
  test_assert(d.linkAll(turn, ids).ok() && d.linkCount() == 4 && ids.size() == 3);
  test_assert(d.linkAll({}, ids).ok() && ids.size() == 0);

  /* A cycle that was there (as from a file) and isn't the batch's is no matter. */
  LinkState bad;
  bad.id = LinkId{5000};
  bad.from = bad.to = n[3];
  bad.fromKey = "x";
  bad.toKey = "b";
  test_assert(d.insertLink(bad).ok());
  const std::vector<LinkSpec> after = {{{n[3], "x"}, {n[4], "a"}}};
  const std::vector<LinkSpec> into = {{{n[5], "x"}, {n[3], "a"}}};
  test_assert(d.linkAll(after, ids).ok() && d.linkAll(into, ids).ok());
  const std::vector<LinkSpec> round = {{{n[4], "x"}, {n[3], "a"}}};
  test_assert(hasCode(d.linkAll(round, ids), StatusCode::InvalidArgument));
  test_assert(d.node(n[3])->linkInto("a")->from->id() == n[5]);
}

/* Through groups a cycle goes only where the def inside connects. */
void testLinkAllGroups()
{
  World w;
  const Graph in = newGraph(w.lib, "in"), g = newGraph(w.lib, "g");
  const int64_t i0 = addSocket(*in.def, SocketSide::Input, "i0", "float");
  const int64_t i1 = addSocket(*in.def, SocketSide::Input, "i1", "float");
  const int64_t o0 = addSocket(*in.def, SocketSide::Output, "o0", "float");
  const NodeId input = add(*in.def, "graph.input"), output = add(*in.def, "graph.output");
  const NodeId deep = add(*in.def, "test.two");
  connect(*in.def, input, key(i0), output, key(o0));
  GraphDef &d = *g.def;
  const NodeId group = add(d, "graph.group"), t = add(d, "test.two");
  test_assert(d.setGroup(group, in.id).ok());
  litestl::util::Vector<LinkId> ids;
  const std::string a = inner(deep, "a"), x = inner(deep, "x");
  const std::string ki0 = key(i0), ki1 = key(i1), ko0 = key(o0);
  auto all = [&](const std::vector<LinkSpec> &list) { return d.linkAll(list, ids); };
  test_assert(hasCode(all({{{group, ko0}, {t, "a"}}, {{t, "x"}, {group, ki0}}}),
                      StatusCode::InvalidArgument));
  test_assert(hasCode(all({{{group, x}, {t, "a"}}, {{t, "x"}, {group, a}}}),
                      StatusCode::InvalidArgument));
  test_assert(d.linkCount() == 0);
  test_assert(all({{{group, ko0}, {t, "a"}}, {{t, "x"}, {group, ki1}}, {{t, "x"}, {group, a}}})
                  .ok());
  test_assert(hasCode(all({{{group, x}, {t, "b"}}}), StatusCode::InvalidArgument));
}

/* A node with many links finds them by key, however they come and go. */
void testWide()
{
  World w;
  const Graph in = newGraph(w.lib, "in"), g = newGraph(w.lib, "g");
  GraphDef &d = *g.def;
  const NodeId group = add(d, "graph.group");
  test_assert(d.setGroup(group, in.id).ok());
  std::vector<int64_t> sockets;
  std::vector<NodeId> from;
  for (int i = 0; i < 40; i++) {
    sockets.push_back(
        addSocket(*in.def, SocketSide::Input, ("s" + std::to_string(i)).c_str(), "float"));
    from.push_back(add(d, "test.two"));
  }
  std::vector<int> linked(40, -1);
  uint32_t rnd = 7;
  int most = 0;
  for (int step = 0; step < 2000; step++) {
    rnd = rnd * 1664525 + 1013904223;
    const size_t s = (rnd >> 8) % 40, f = (rnd >> 16) % 40;
    const bool grow = (step / 200) % 2 == 0;
    if (grow) {
      connect(d, from[f], "y", group, key(sockets[s]));
      linked[s] = int(f);
    }
    else if (linked[s] >= 0) {
      test_assert(d.unlink(d.node(group)->linkInto(key(sockets[s]))->id).ok());
      linked[s] = -1;
    }
    int count = 0;
    for (size_t i = 0; i < 40; i++) {
      const GraphLink *l = d.node(group)->linkInto(key(sockets[i]));
      test_assert((l ? l->from->id() : NodeId()) ==
                  (linked[i] < 0 ? NodeId() : from[size_t(linked[i])]));
      count += linked[i] >= 0;
    }
    test_assert(int(d.node(group)->linksIn().size()) == count && d.linkCount() == count);
    most = std::max(most, count);
  }
  test_assert(most > 30 && d.validate().ok());
  /* A source that goes takes its links along. */
  for (NodeId id : from) {
    test_assert(d.removeNode(id).ok());
  }
  test_assert(d.linkCount() == 0 && d.node(group)->linksIn().size() == 0);
  test_assert(!d.node(group)->linkInto(key(sockets[0])));
}

/* ---- limits ---- */

void testInterfaceLimit()
{
  World w;
  const Graph g = newGraph(w.lib, "g");
  for (SocketSide side : {SocketSide::Input, SocketSide::Output}) {
    for (int i = 0; i < GraphDef::kMaxInterface; i++) {
      addSocket(*g.def, side, ("s" + std::to_string(i)).c_str(), "float");
    }
    test_assert(hasCode(g.def->addInterface(side, "more", "float"), StatusCode::OutOfRange));
    test_assert(g.def->removeInterface(g.def->interfaceSockets(side)[5].id).ok());
    addSocket(*g.def, side, "more", "float");
    test_assert(hasCode(g.def->addInterface(side, "again", "float"), StatusCode::OutOfRange));
  }
}

void testInteriorLimit()
{
  World w;
  const Graph in = newGraph(w.lib, "in"), g = newGraph(w.lib, "g");
  const int n = GraphDef::kMaxInteriorLinks;
  std::vector<NodeId> nodes;
  for (int i = 0; i < n; i++) {
    nodes.push_back(add(*in.def, "test.two"));
  }
  const int64_t face = addSocket(*in.def, SocketSide::Input, "face", "float");
  GraphDef &d = *g.def;
  const NodeId group = add(d, "graph.group"), from = add(d, "test.two"), to = add(d, "test.two");
  const NodeId other = add(d, "graph.group");
  test_assert(d.setGroup(group, in.id).ok() && d.setGroup(other, in.id).ok());
  auto link = [&](int64_t id, NodeId a, const std::string &k, NodeId b, const std::string &l) {
    LinkState s;
    s.id = LinkId{id};
    s.from = a;
    s.fromKey = k.c_str();
    s.to = b;
    s.toKey = l.c_str();
    return d.insertLink(s);
  };
  /* Up to the limit, in both directions: they count together. */
  test_assert(link(100000, group, inner(nodes[0], "x"), to, "a").ok());
  for (int i = 0; i < n - 1; i++) {
    test_assert(link(100001 + i, from, "x", group, inner(nodes[size_t(i)], "b")).ok());
  }
  const std::string last = inner(nodes[size_t(n - 1)], "b");
  litestl::util::Vector<LinkId> ids;
  const std::vector<LinkSpec> spec = {{{from, "x"}, {group, last}}};
  test_assert(hasCode(link(200000, from, "x", group, last), StatusCode::OutOfRange));
  test_assert(hasCode(link(200000, group, inner(nodes[0], "x"), to, "b"), StatusCode::OutOfRange));
  test_assert(hasCode(d.linkAll(spec, ids), StatusCode::OutOfRange));
  test_assert(hasCode(d.link({from, "x"}, {group, last}), StatusCode::OutOfRange));
  test_assert(d.linkCount() == n);
  /* The other group has its own, and the interface isn't interior. */
  test_assert(link(200000, from, "x", other, last).ok());
  test_assert(link(200001, from, "x", group, key(face)).ok());
  test_assert(d.unlink(LinkId{100000}).ok());
  test_assert(link(200002, from, "x", group, last).ok());
  test_assert(hasCode(link(200003, group, inner(nodes[0], "x"), to, "a"), StatusCode::OutOfRange));
  test_assert(d.removeNode(from).ok() && d.linkCount() == 0);
  test_assert(link(200003, group, inner(nodes[0], "x"), to, "a").ok() && d.validate().ok());
}

} // namespace

int main()
{
  testLinkAll();
  testLinkAllGroups();
  testWide();
  testInterfaceLimit();
  testInteriorLimit();
  return test_end();
}
