#include "coredcc/drive/adapters.h"

#include "coredcc/graph/interior.h"
#include "coredcc/graph/provider.h"
#include "coredcc/lib/registry.h"

#include <string>

namespace coredcc::drive {

using litestl::util::Vector;

namespace {

constexpr int kMaxDepth = 64;

const PropValue *paramOf(const GraphNode &n, const char *name)
{
  for (const NodeValue &v : n.params()) {
    if (strcmp(v.name.c_str(), name) == 0) {
      return &v.value;
    }
  }
  return nullptr;
}

AdapterIssue issueAt(const Vector<NodeId, 2> &groups, NodeId node, std::string message)
{
  AdapterIssue i;
  i.groups = groups;
  i.node = node;
  i.message = message.c_str();
  return i;
}

} // namespace

std::string interiorKey(const Adapter &a, std::string_view socket)
{
  InteriorKey k;
  for (NodeId g : a.groups) {
    k.groups.append(g);
  }
  k.node = a.node;
  k.key = litestl::util::string(std::string(socket).c_str());
  return k.toString();
}

const AdapterScan::Entry &
AdapterScan::entryOf(const Library &lib, const GraphDef &def, const BlockId &id)
{
  std::unique_ptr<Entry> *found = nullptr;
  if (entries_.add_uninitialized(id, &found)) {
    *found = std::make_unique<Entry>();
  }
  Entry &e = **found;
  e.used = true;
  if (e.serial == def.serial() && e.content == def.contentVersion() &&
      e.epoch == lib.epoch())
  {
    return e;
  }
  e.serial = def.serial();
  e.content = def.contentVersion();
  e.epoch = lib.epoch();
  e.own.clear();
  e.issues.clear();
  e.children.clear();
  /* By canonical id, as SceneDrivers asks: an alias's id names the same block. */
  const ClientRegistry &clients = lib.runtime().ext<ClientRegistry>();
  def.forEachNode([&](const GraphNode &n) {
    const NodeType *t = n.type();
    if (!t) {
      return;
    }
    if (hasGroup(t->role)) {
      Child c;
      c.node = n.id();
      c.def = lib.resolveRef(n.group()) ? lib.get(lib.resolveRef(n.group()))->id()
                                        : n.group().target;
      c.foreach = t->role == NodeRole::Foreach;
      e.children.append(c);
      return;
    }
    Adapter a;
    a.node = n.id();
    const std::string_view name(t->name.c_str());
    if (name == kTimeNode) {
      a.kind = AdapterKind::Time;
    } else if (name == kStateNode) {
      a.kind = AdapterKind::State;
      a.object = clients.canonical(n.blockTarget("object"));
    } else if (kindOfDriveType(name) != DriveKind::None) {
      a.kind = AdapterKind::Drive;
      a.object = clients.canonical(n.blockTarget("object"));
      a.driveKind = kindOfDriveType(name);
      const PropValue *channel = paramOf(n, "channel");
      a.channel = channel ? channel->asString() : litestl::util::string();
    } else if (name.rfind(kTransformPrefix, 0) == 0 || name == kMissingTransform) {
      const Vector<NodeId, 2> none;
      e.issues.append(issueAt(none,
                              n.id(),
                              "a transform node is for the graph SceneDrivers makes; "
                              "it does nothing here"));
      return;
    } else {
      return; /* a script's type may have the flags too: names count */
    }
    e.own.append(std::move(a));
  });
  return e;
}

void AdapterScan::expand(const Library &lib,
                         const BlockId &id,
                         const Vector<NodeId, 2> &prefix,
                         int depth,
                         bool inForeach,
                         Vector<Adapter> &out,
                         Vector<AdapterIssue> &issues)
{
  const GraphDef *def = graphDefOf(lib, lib.find(id));
  if (!def) {
    return;
  }
  if (depth > kMaxDepth) {
    issues.append(issueAt(prefix, NodeId(), "groups nested deeper than 64 are left out"));
    return;
  }
  for (const BlockId &above : path_) {
    if (above == id) {
      issues.append(
          issueAt(prefix, NodeId(), "a graph that contains itself is left out"));
      return;
    }
  }
  path_.append(id);
  const Entry &e = entryOf(lib, *def, id);
  for (const Adapter &own : e.own) {
    if (inForeach) {
      issues.append(issueAt(prefix,
                            own.node,
                            "an adapter in a foreach body can't be connected; it is "
                            "left out"));
      continue;
    }
    Adapter a = own;
    Vector<NodeId, 2> groups = prefix;
    for (NodeId g : own.groups) {
      groups.append(g);
    }
    a.groups = std::move(groups);
    out.append(std::move(a));
  }
  for (const AdapterIssue &i : e.issues) {
    issues.append(i);
  }
  /* A copy: entries may move while the children are scanned. */
  const Vector<Child> children = e.children;
  for (const Child &c : children) {
    Vector<NodeId, 2> inner = prefix;
    inner.append(c.node);
    expand(lib, c.def, inner, depth + 1, inForeach || c.foreach, out, issues);
  }
  path_.remove_at(int(path_.size()) - 1);
}

void AdapterScan::scan(const Library &lib,
                       BlockHandle graph,
                       Vector<Adapter> &out,
                       Vector<AdapterIssue> &issues)
{
  out.clear();
  issues.clear();
  const Datablock *b = lib.get(graph);
  if (b) {
    expand(lib, b->id(), Vector<NodeId, 2>(), 0, false, out, issues);
  }
}

void AdapterScan::prune()
{
  Vector<BlockId> unused;
  for (auto &pair : entries_) {
    if (!pair.value->used) {
      unused.append(pair.key);
    }
    pair.value->used = false;
  }
  for (const BlockId &id : unused) {
    entries_.remove(id);
  }
}

} // namespace coredcc::drive
