/* GraphDef: links in order, what is wrong with them, and what programs that rescan nodes
 * go by. */
#include "edit_fixture.h"

#include "coredcc/graph/interior.h"

using namespace fixture;

test_init;

namespace {

Status nothing(NodeExecContext &)
{
  return Status();
}

PropValue toThing(BlockId id)
{
  StableRef r;
  r.owner = OwnerKey::ofBlock(id);
  r.type = kThingStruct;
  return PropValue::ofPointer(r);
}

/* linkAt, diagnostics and newId. */
void testOrder()
{
  Doc d;
  const Graph g = makeGraph(d.lib, "g"), inner = makeGraph(d.lib, "in");
  GraphDef &def = *g.def;
  const int64_t x = addSocket(*inner.def, SocketSide::Output, "x", "float");
  litestl::util::Vector<NodeId> nodes;
  litestl::util::Vector<LinkId> links;
  nodes.append(add(def, "math.float"));
  for (int i = 1; i < 40; i++) {
    nodes.append(add(def, "math.float"));
    links.append(connect(def, nodes[i - 1], "value", nodes[i], "a"));
  }
  auto ordered = [&]() {
    int n = 0;
    for (int i = 0; i < def.linkCount(); i++) {
      const GraphLink *l = def.linkAt(i);
      n += l && def.link(l->id) == l && (i == 0 || def.linkAt(i - 1)->id < l->id);
    }
    return n == def.linkCount() && !def.linkAt(n) && !def.linkAt(-1);
  };
  test_assert(def.linkCount() == 39 && ordered());
  const LinkId first = def.linkAt(0)->id;
  const std::string path = "graphs[\"g\"].links[0]";
  test_assert(d.is(path + ".id", PropValue::ofInt(first.value)));
  test_assert(d.is(path + ".error", PropValue::ofString("")));
  const LinkState was = GraphDef::stateOf(*def.link(first));
  test_assert(def.unlink(first).ok() && ordered() && def.linkAt(0)->id != first);
  test_assert(def.insertLink(was).ok() && ordered() && def.linkAt(0)->id == first);
  const LinkSpec more[] = {{{nodes[0], "value"}, {nodes[5], "b"}},
                           {{nodes[1], "value"}, {nodes[6], "b"}}};
  litestl::util::Vector<LinkId> ids;
  test_assert(def.linkAll(more, ids).ok() && ordered() && def.linkCount() == 41);
  test_assert(def.removeNode(nodes[0]).ok() && ordered() && def.linkCount() == 39);

  /* Diagnostics, as they come and go. */
  const NodeId group = add(def, kGraphGroupType);
  test_assert(def.setGroup(group, inner.id).ok() && def.diagnostics().size() == 0);
  const LinkId a = connect(def, group, key(x), nodes[9], "b");
  const LinkId b = connect(def, group, key(x), nodes[8], "b");
  test_assert(!def.diagnostic(a) && def.diagnostics().size() == 0);
  test_assert(inner.def->removeInterface(x).ok());
  test_assert(def.diagnostics().size() == 2 && def.diagnostic(a)->link == a);
  test_assert(hasCode(def.diagnostic(b)->status, StatusCode::NotFound));
  test_assert(!def.diagnostic(links[10]) && !def.diagnostic(LinkId{1}));
  test_assert(!def.diagnostic(LinkId{(int64_t(1) << 52) - 1}));
  test_assert(def.diagnostics()[0].link < def.diagnostics()[1].link);
  test_assert(!def.diagnostic(a)->interior);
  Result<PropValue> text =
      d.get("graphs[\"g\"].links[#" + std::to_string(a.value) + "].error");
  test_assert(text.ok() && strstr(text->asString().c_str(), key(x).c_str()));
  /* Kept while nothing they depend on changes, and not when something does. */
  const LinkDiagnostic *at = def.diagnostics().data();
  test_assert(def.setLabel(nodes[9], "label").ok() && def.diagnostics().data() == at);
  test_assert(inner.def->addInterface(SocketSide::Output, "x", "float", -1, x).ok());
  test_assert(def.diagnostics().size() == 0);
  test_assert(inner.def->setInterfaceType(x, "mat4").ok() &&
              def.diagnostics().size() == 2);
  test_assert(hasCode(def.diagnostics()[0].status, StatusCode::TypeMismatch));
  test_assert(d.lib.markDeleted(inner.h).ok() &&
              def.diagnostics().size() == 2); /* last known */
  test_assert(def.unlink(a).ok() && def.diagnostics().size() == 1 && !def.diagnostic(a));
  test_assert(def.unlink(b).ok() && def.diagnostics().size() == 0);
  /* Interior links say so, at either end. */
  const Graph in2 = makeGraph(d.lib, "in2");
  const NodeId leaf = add(*in2.def, "math.float");
  const std::string leafKey = "@" + std::to_string(leaf.value);
  test_assert(def.setGroup(group, in2.id).ok());
  const LinkId from = connect(def, group, leafKey + ".value", nodes[9], "b");
  const LinkId to = connect(def, nodes[2], "value", group, leafKey + ".a");
  test_assert(!def.diagnostic(from) && !def.diagnostic(to) &&
              in2.def->removeNode(leaf).ok());
  test_assert(def.diagnostic(from) && def.diagnostic(from)->interior);
  test_assert(def.diagnostic(to) && def.diagnostic(to)->interior);

  /* Ids for pasting. */
  litestl::util::Vector<int64_t> made;
  for (int i = 0; i < 100; i++) {
    const int64_t id = def.newId();
    test_assert(id > 0 && id < int64_t(1) << 52 && !made.contains(id));
    made.append(id);
  }
  NodeState copy = def.stateOf(*def.node(nodes[3]));
  copy.id = NodeId{made[0]};
  test_assert(def.insertNode(copy).ok() && def.node(NodeId{made[0]}));
}

void testVersions()
{
  Doc d;
  const Graph g = makeGraph(d.lib, "g"), inner = makeGraph(d.lib, "in");
  GraphDef &def = *g.def;
  NodeTypeDef out("t.out");
  out.param(PropertyDef::pointerProp("thing", kThingStruct)).output("x", "float");
  out.param(PropertyDef::intProp("n")).flags(NodeTypeFlags::OutputAdapter);
  addType<nothing>(d.rt, out, "t.out");
  NodeTypeDef both("t.both");
  both.output("x", "float")
      .flags(NodeTypeFlags::OutputAdapter | NodeTypeFlags::ThreadSafe);
  addType<nothing>(d.rt, both, "t.both");

  uint64_t refs = def.refsVersion();
  auto moved = [&]() {
    const bool m = def.refsVersion() != refs;
    refs = def.refsVersion();
    return m;
  };
  auto count = [&](NodeTypeFlags flags) {
    int n = 0;
    def.forEachNode(flags, [&](const GraphNode &node) { n += node.type() != nullptr; });
    return n;
  };
  const NodeId plain = add(def, "math.float");
  test_assert(!moved() && count(NodeTypeFlags::OutputAdapter) == 0);
  const NodeId a = add(def, "t.out");
  test_assert(moved() && count(NodeTypeFlags::OutputAdapter) == 1);
  test_assert(count(NodeTypeFlags::ThreadSafe) == 0 && count(NodeTypeFlags::None) == 2);
  const NodeId b = add(def, "t.both");
  test_assert(!moved() && count(NodeTypeFlags::OutputAdapter) == 2);
  test_assert(count(NodeTypeFlags::OutputAdapter | NodeTypeFlags::ThreadSafe) == 1);
  const BlockHandle h = newThing(d.lib, "t", 1);
  test_assert(def.setParam(a, "n", PropValue::ofInt(3)).ok() && !moved());
  test_assert(def.setLiteral(plain, "a", PropValue::ofFloat(3)).ok() && !moved());
  test_assert(def.setPosition(a, 1, 2).ok() && !moved());
  test_assert(def.setParam(a, "thing", toThing(idOf(d.lib, h))).ok() && moved());
  test_assert(def.restoreRef(a, "thing", BlockId()).ok() && moved());
  test_assert(def.removeNode(b).ok() && !moved() &&
              count(NodeTypeFlags::OutputAdapter) == 1);
  const NodeState was = def.stateOf(*def.node(a));
  test_assert(def.removeNode(a).ok() && moved() &&
              count(NodeTypeFlags::OutputAdapter) == 0);
  test_assert(def.restoreNode(was).ok() && moved() &&
              count(NodeTypeFlags::OutputAdapter) == 1);
  const NodeId group = add(def, kGraphGroupType);
  test_assert(moved() && def.setGroup(group, inner.id).ok() && moved());
  test_assert(def.restoreRef(group, "", BlockId()).ok() && moved());
  test_assert(def.removeNode(group).ok() && moved() && def.removeNode(plain).ok() &&
              !moved());
  /* A type that comes late has flags and references. */
  NodeState late;
  late.id = NodeId{def.newId()};
  late.typeName = "t.late";
  test_assert(def.insertNode(late).ok() && count(NodeTypeFlags::OutputAdapter) == 1);
  moved();
  NodeTypeDef l("t.late");
  l.param(PropertyDef::pointerProp("thing", kThingStruct)).output("x", "float");
  l.flags(NodeTypeFlags::OutputAdapter);
  addType<nothing>(d.rt, l, "t.late");
  test_assert(moved() && count(NodeTypeFlags::OutputAdapter) == 2);
}

} // namespace

int main()
{
  testOrder();
  testVersions();
  return test_end();
}
