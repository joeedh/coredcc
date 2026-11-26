/* The generated graph: what it is made from (a Description), and making it. It is made
 * again, from scratch and in a fixed order, whenever the description differs, so that a
 * document gives the same graph whatever the order of its edits. */
#include "coredcc/api/ref_text.h"
#include "coredcc/base/iface/registry.h"
#include "coredcc/drive/driver_list.h"
#include "coredcc/drive/state.h"
#include "coredcc/graph/interior.h"
#include "coredcc/graph/provider.h"
#include "coredcc/scene/capabilities.h"
#include "coredcc/scene/scene.h"
#include "coredcc/scene/transform_registry.h"

#include <string>

namespace coredcc {

using litestl::util::Vector;
using namespace drive;

namespace {

template <class V> bool sameList(const V &a, const V &b)
{
  if (a.size() != b.size()) {
    return false;
  }
  for (int i = 0; i < int(a.size()); i++) {
    if (!(a[i] == b[i])) {
      return false;
    }
  }
  return true;
}

bool sameAdapter(const Adapter &a, const Adapter &b)
{
  if (a.kind != b.kind || a.node != b.node || a.object != b.object ||
      a.driveKind != b.driveKind || strcmp(a.channel.c_str(), b.channel.c_str()) != 0 ||
      a.groups.size() != b.groups.size())
  {
    return false;
  }
  for (int i = 0; i < int(a.groups.size()); i++) {
    if (a.groups[i] != b.groups[i]) {
      return false;
    }
  }
  return true;
}

NodePath pathOf(const Vector<NodeId, 2> &groups, NodeId node)
{
  NodePath p;
  for (NodeId g : groups) {
    p.groups.append(g);
  }
  p.node = node;
  return p;
}

PropValue pointerTo(const BlockId &id)
{
  StableRef r;
  r.owner = OwnerKey::ofBlock(id);
  return PropValue::ofPointer(r);
}

/* The flattened offset and kind of a channel def, by name; false if there is none. */
bool channelOf(const TransformType &type, std::string_view name, DriveKind &kind)
{
  for (const PropertyDef &def : type.channelDefs()) {
    if (name == def.apiName().c_str()) {
      kind = kindOf(def.kind());
      return true;
    }
  }
  return false;
}

} // namespace

bool drive::Description::operator==(const Description &b) const
{
  if (graphs.size() != b.graphs.size() || !sameList(objects, b.objects) ||
      !sameList(driven, b.driven) || !sameList(notes, b.notes))
  {
    return false;
  }
  for (int i = 0; i < int(graphs.size()); i++) {
    const GraphDesc &x = graphs[i], &y = b.graphs[i];
    if (x.id != y.id || x.adapters.size() != y.adapters.size()) {
      return false;
    }
    for (int k = 0; k < int(x.adapters.size()); k++) {
      if (!sameAdapter(x.adapters[k], y.adapters[k])) {
        return false;
      }
    }
  }
  return true;
}

void SceneDrivers::State::scanLists(Library &lib)
{
  const TypeId listType = TypeId::of(kDriverListType);
  if (rescan || int64_t(lib.size()) != scannedSize ||
      lib.removedCount() != scannedRemoved)
  {
    lists = DriverList::allLists(lib); /* by id, not by the Library's order */
    placeholders.clear();
    lib.forEach([&](BlockHandle h, const Datablock &b) {
      if (b.type() != listType && b.state() == BlockState::Placeholder) {
        placeholders.append(h);
      }
    });
    scannedSize = int64_t(lib.size());
    scannedRemoved = lib.removedCount();
    rescan = false;
    return;
  }
  /* A placeholder that was filled in place moves neither counter. */
  for (int i = int(placeholders.size()) - 1; i >= 0; i--) {
    const Datablock *b = lib.get(placeholders[i]);
    if (!b || b->state() != BlockState::Placeholder) {
      if (b && b->type() == listType) {
        lists = DriverList::allLists(lib);
      }
      placeholders.remove_at(i);
    }
  }
}

drive::Description SceneDrivers::State::describe(Library &lib)
{
  Description d;
  diagnostics.clear();
  litestl::util::Map<BlockId, char> listed;
  Vector<AdapterIssue> issues;
  /* Every list: a graph that two name counts once. */
  for (BlockHandle lh : lists) {
    const Datablock *lb = lib.get(lh);
    if (lb && lb->state() == BlockState::Placeholder) {
      diagnostics.append(DriverDiagnostic{lb->id(),
                                          NodePath(),
                                          "a list of drivers isn't readable: its type "
                                          "wasn't registered when it was loaded"});
    }
    const DriverList *list =
        lb && lb->state() == BlockState::Live ? DriverList::of(lib, lh) : nullptr;
    if (!list) {
      continue;
    }
    for (BlockHandle gh : list->graphs(lib)) {
      const BlockId id = lib.get(gh)->id();
      if (!listed.add(id, 0)) {
        continue;
      }
      if (!graphDefOf(lib, gh)) {
        diagnostics.append(DriverDiagnostic{id,
                                            NodePath(),
                                            "the list of drivers names a block "
                                            "that isn't a graph"});
        continue;
      }
      GraphDesc g;
      g.id = id;
      scan.scan(lib, gh, g.adapters, issues);
      for (const AdapterIssue &i : issues) {
        diagnostics.append(DriverDiagnostic{id, pathOf(i.groups, i.node), i.message});
      }
      d.graphs.append(std::move(g));
    }
  }
  scan.prune();
  for (const DriverDiagnostic &dg : diagnostics) {
    d.notes.append(formatBlockId(*rt, dg.graph) + " " + dg.node.toString() + " " +
                   dg.message.c_str());
  }

  /* The objects that readers name, and their ancestors, in the order they are found. */
  const TransformRegistry &types = rt->ext<TransformRegistry>();
  litestl::util::Map<BlockId, int> index;
  for (const GraphDesc &g : d.graphs) {
    for (const Adapter &a : g.adapters) {
      if (a.kind != AdapterKind::State) {
        continue;
      }
      litestl::util::Map<BlockId, int> onPath;
      BlockId at = a.object;
      while (!at.isNull() && !index.lookup_ptr(at)) {
        const BlockHandle h = lib.find(at);
        const Datablock *b = lib.get(h);
        const Transformable tr = b && b->state() == BlockState::Live
                                     ? lib.query<Transformable>(h)
                                     : Transformable();
        if (!tr) {
          break;
        }
        ObjectInfo info;
        ChannelBlock channels;
        tr.read(info, channels);
        if (rt->hasStickyError()) {
          (void)rt->takeStickyError(); /* the kernel will find it out, and say so */
        }
        ObjectDesc o;
        o.id = b->id();
        o.parent = info.parent;
        o.parentText = formatBlockId(*rt, info.parent);
        const TransformType *type = types.find(info.transformType);
        o.type = type ? type->name().c_str() : "";
        const int n = int(d.objects.size());
        index.add(o.id, n);
        onPath.add(o.id, n);
        d.objects.append(std::move(o));
        if (const int *loop =
                info.parent.isNull() ? nullptr : onPath.lookup_ptr(info.parent))
        {
          for (int k = *loop; k <= n; k++) {
            d.objects[k].cycle = true; /* as P3: no parent until that changes */
          }
          break;
        }
        at = info.parent;
      }
    }
  }
  for (ObjectDesc &o : d.objects) {
    if (o.cycle || o.parent.isNull() || !index.lookup_ptr(o.parent)) {
      o.parent = BlockId();
    }
  }
  litestl::util::Map<BlockId, char> seen;
  for (const GraphDesc &g : d.graphs) {
    for (const Adapter &a : g.adapters) {
      if (a.kind != AdapterKind::Drive || a.object.isNull() || !seen.add(a.object, 0)) {
        continue;
      }
      ObjectDesc o;
      o.id = a.object;
      const BlockHandle h = lib.find(a.object);
      const Datablock *b = lib.get(h);
      const Transformable tr = b && b->state() == BlockState::Live
                                   ? lib.query<Transformable>(h)
                                   : Transformable();
      o.transformable = bool(tr);
      if (tr) {
        ObjectInfo info;
        ChannelBlock channels;
        tr.read(info, channels);
        if (rt->hasStickyError()) {
          (void)rt->takeStickyError();
        }
        const TransformType *type = types.find(info.transformType);
        o.type = type ? type->name().c_str() : "";
      }
      d.driven.append(std::move(o));
    }
  }
  return d;
}

void SceneDrivers::State::rebuild(Library &lib)
{
  inst.reset(); /* before its def */
  def = std::make_unique<GraphDef>(rt, &lib);
  inst = std::make_unique<GraphInstance>(rt, &lib, GraphSource(def.get()));
  inst->setUserData(inner);
  nodeOf.clear();
  stateOf.clear();
  statusHandles.clear();
  problems.clear();
  litestl::util::Map<BlockId, char> wasDriven;
  for (auto &pair : drivesOf) {
    wasDriven.add(pair.key, 0);
  }
  drivesOf.clear();
  drives.clear();
  handles.clear();
  const auto diagnose = [&](const BlockId &graph, NodePath node, std::string message) {
    diagnostics.append(DriverDiagnostic{graph, std::move(node), message.c_str()});
  };
  const auto problem = [&](const BlockId &object, const std::string &why) {
    if (!problems.lookup_ptr(object)) {
      problems.add(object, why);
    }
  };

  timeInput = *def->addInterface(SocketSide::Input, "time", "float");
  const NodeId input = *def->addNode(kGraphInputType);
  const std::string timeKey = interfaceKey(timeInput).c_str();

  Vector<NodeId> groupNode;
  for (const GraphDesc &g : desc.graphs) {
    const NodeId n = *def->addNode(kGraphGroupType);
    if (Status s = def->setGroup(n, g.id); !s.ok()) {
      diagnose(g.id, NodePath(), s.message());
    }
    groupNode.append(n);
  }

  const TransformRegistry &types = rt->ext<TransformRegistry>();
  for (const ObjectDesc &o : desc.objects) {
    const TransformType *type = o.type.empty() ? nullptr : types.find(o.type);
    Result<const NodeType *> t = transformNodeType(*rt, type);
    if (!t.ok()) {
      problem(o.id, t.status().message());
      continue;
    }
    const NodeId n = *def->addNode((*t)->name.c_str());
    (void)def->setParam(n, "object", pointerTo(o.id));
    (void)def->setParam(n, "parentId", PropValue::ofString(o.parentText));
    (void)def->setParam(n, "cycle", PropValue::ofBool(o.cycle));
    nodeOf.add(o.id, n);
  }

  /* Parents and time first: they can't make a cycle, and are never left out. */
  Vector<LinkSpec> first;
  for (const ObjectDesc &o : desc.objects) {
    const NodeId *n = nodeOf.lookup_ptr(o.id);
    if (!n) {
      continue;
    }
    if (const NodeId *p = o.parent.isNull() ? nullptr : nodeOf.lookup_ptr(o.parent)) {
      first.append(LinkSpec{{*p, "state"}, {*n, "parent"}});
    }
    first.append(LinkSpec{{input, timeKey}, {*n, "time"}});
  }
  Vector<LinkId> ids;
  if (Status s = def->linkAll(std::span<const LinkSpec>(first.data(), first.size()), ids);
      !s.ok())
  {
    diagnose(BlockId(),
             NodePath(),
             "the hierarchy couldn't be linked: " + std::string(s.message()));
  }

  /* Then readers, time and drivers, one at a time in order: those that would close a
   * cycle are left out. */
  litestl::util::Map<BlockId, const ObjectDesc *> typeOf;
  for (const ObjectDesc &o : desc.driven) {
    typeOf.add(o.id, &o);
  }
  for (const ObjectDesc &o : desc.objects) {
    typeOf.add_overwrite(o.id, &o);
  }
  litestl::util::Map<litestl::util::string, int> channelUses;
  for (int gi = 0; gi < int(desc.graphs.size()); gi++) {
    for (const Adapter &a : desc.graphs[gi].adapters) {
      if (a.kind != AdapterKind::Drive || a.object.isNull()) {
        continue;
      }
      const litestl::util::string key =
          (formatBlockId(*rt, a.object) + "/" + a.channel.c_str()).c_str();
      int *uses = channelUses.lookup_ptr(key);
      if (uses) {
        (*uses)++;
      } else {
        channelUses.add(key, 1);
      }
    }
  }
  for (int gi = 0; gi < int(desc.graphs.size()); gi++) {
    const GraphDesc &g = desc.graphs[gi];
    const NodeId group = groupNode[gi];
    for (const Adapter &a : g.adapters) {
      const NodePath at = pathOf(a.groups, a.node);
      if (a.kind == AdapterKind::Time) {
        const std::string to = interiorKey(a, "time");
        (void)def->link({input, timeKey}, {group, to});
        continue;
      }
      if (a.object.isNull()) {
        diagnose(g.id, at, "its object isn't set");
        continue;
      }
      const NodeId *target = nodeOf.lookup_ptr(a.object);
      if (a.kind == AdapterKind::State) {
        if (!target) {
          diagnose(g.id, at, "its object isn't there, or isn't Transformable");
          continue;
        }
        const std::string to = interiorKey(a, "state");
        if (Status s = def->link({*target, "state"}, {group, to}).status(); !s.ok()) {
          diagnose(
              g.id, at, "reading it would make a cycle: " + std::string(s.message()));
        } else {
          (void)def->link({*target, "status"}, {group, interiorKey(a, "status")});
        }
        continue;
      }

      DriveEntry e;
      e.object = a.object;
      e.channel = a.channel;
      e.kind = a.driveKind;
      NodePath out;
      out.groups.append(group);
      for (NodeId inner : a.groups) {
        out.groups.append(inner);
      }
      out.node = a.node;
      Result<SocketHandle> h = inst->prepare(SocketPath::of(out, "value"));
      e.handle = h.ok() ? *h : SocketHandle();
      Vector<int> *list = drivesOf.lookup_ptr(a.object);
      if (!list) {
        drivesOf.add(a.object, Vector<int>());
        list = drivesOf.lookup_ptr(a.object);
      }
      list->append(int(drives.size()));
      drives.append(e);
      handles.append(e.handle);

      const litestl::util::string key =
          (formatBlockId(*rt, a.object) + "/" + a.channel.c_str()).c_str();
      if (*channelUses.lookup_ptr(key) > 1) {
        const std::string why =
            "its channel '" + std::string(a.channel.c_str()) + "' is driven twice";
        diagnose(g.id, at, why);
        problem(a.object, why);
        continue;
      }
      const ObjectDesc *const *found = typeOf.lookup_ptr(a.object);
      if (!found || !(*found)->transformable) {
        continue; /* nothing asks for its overrides */
      }
      const ObjectDesc *o = *found;
      const TransformType *type = o->type.empty() ? nullptr : types.find(o->type);
      DriveKind kind = DriveKind::None;
      if (!type || !channelOf(*type, a.channel.c_str(), kind) || kind != a.driveKind) {
        const std::string why =
            !type ? "its transform type isn't registered, and a graph drives it"
            : kind == DriveKind::None
                ? "its transform type has no channel '" + std::string(a.channel.c_str()) +
                      "', which a graph drives"
                : "its channel '" + std::string(a.channel.c_str()) + "' is a " +
                      kindName(kind) + ", and a graph drives it with a " +
                      kindName(a.driveKind);
        diagnose(g.id, at, why);
        if (type) { /* without a type it is MissingModel, as in the direct pass */
          problem(a.object, why);
        }
        continue;
      }
      if (!target) {
        continue; /* only the direct pass applies it */
      }
      const std::string from = interiorKey(a, "value");
      const std::string to = std::string(kChannelPrefix) + a.channel.c_str();
      if (Status s = def->link({group, from}, {*target, to}).status(); !s.ok()) {
        const std::string why =
            "driving it would make a cycle: " + std::string(s.message());
        diagnose(g.id, at, why);
        problem(a.object, why);
      }
    }
  }

  for (const ObjectDesc &o : desc.objects) {
    const NodeId *n = nodeOf.lookup_ptr(o.id);
    const std::string *why = problems.lookup_ptr(o.id);
    if (n && why) {
      (void)def->setParam(*n, "error", PropValue::ofString(why->c_str()));
    }
  }
  for (const ObjectDesc &o : desc.objects) {
    if (const NodeId *n = nodeOf.lookup_ptr(o.id)) {
      Result<SocketHandle> h = inst->prepare(SocketPath::of(*n, "status"));
      const SocketHandle sh = h.ok() ? *h : SocketHandle();
      statusHandles.append(sh);
      handles.append(sh);
      /* For tests and tools: instance().value() has only the sockets asked for. */
      Result<SocketHandle> state = inst->prepare(SocketPath::of(*n, "state"));
      handles.append(state.ok() ? *state : SocketHandle());
      stateOf.add(o.id, handles.last());
    }
  }

  bool sameDriven = int(wasDriven.size()) == int(drivesOf.size());
  for (auto &pair : drivesOf) {
    sameDriven = sameDriven && wasDriven.lookup_ptr(pair.key);
  }
  if (!sameDriven) {
    drivenVersion++;
  }
  evaluated = false;
  built = true;
  rebuilds++;
}

Status SceneDrivers::State::sync(Library &lib, bool force)
{
  (void)lib.find(BlockId()); /* a pending renormalization of ids moves the epoch now */
  const auto keyNow = [&] {
    SyncKey k;
    k.epoch = lib.epoch();
    k.clients = rt->ext<ClientRegistry>().generation();
    k.tables = rt->ifaces().tableCount();
    k.transformTypes = rt->ext<TransformRegistry>().count();
    /* Less those that syncing registers, which change nothing a scan saw. */
    k.nodeTypes = rt->ext<NodeTypeRegistry>().size() - ownTransformTypeCount(*rt);
    k.lastEdit = rt->lastEdit();
    k.size = int64_t(lib.size());
    k.removed = lib.removedCount();
    return k;
  };
  if (built && !force && keyNow() == synced) {
    return Status();
  }
  scanLists(lib);
  Vector<DriverDiagnostic> before = std::move(diagnostics);
  Description d = describe(lib);
  if (!built || force || !(d == desc)) {
    desc = std::move(d);
    rebuild(lib);
  } else {
    /* The same graph: what the scan said stays, with what the build added. */
    Vector<DriverDiagnostic> now = std::move(diagnostics);
    diagnostics = std::move(before);
    (void)now;
  }
  synced = keyNow();
  return Status();
}

} // namespace coredcc
