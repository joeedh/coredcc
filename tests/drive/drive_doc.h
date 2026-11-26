#pragma once

/* A scene document with graphs and the drive types, and (unless asked not to) a
 * SceneDrivers. Helpers make users' graphs with adapters. */

#include "../scene/io_doc.h"

#include "coredcc/api/ref_text.h"
#include "coredcc/drive/drive.h"
#include "coredcc/drive/driver_list.h"
#include "coredcc/drive/node_types.h"
#include "coredcc/graph/commands.h"
#include "coredcc/graph/provider.h"
#include "coredcc/scene/solve.h"

#include <memory>
#include <string>

namespace drive_test {

using namespace scene_test;
using namespace coredcc::drive;

inline PropValue pointerTo(const BlockId &id)
{
  StableRef r;
  r.owner = OwnerKey::ofBlock(id);
  return PropValue::ofPointer(r);
}

inline double3 at(const TransformState &s)
{
  return solve::translationOf(s.world);
}

inline bool sameState(const TransformState &a, const TransformState &b)
{
  return memcmp(&a, &b, sizeof(TransformState)) == 0;
}

struct DriveDoc : IoDoc {
  std::unique_ptr<SceneDrivers> drivers;

  explicit DriveDoc(bool withDrivers = true)
  {
    must(registerGraphCommands(rt));
    must(registerDriveTypes(rt));
    must(registerDriveTypes(rt)); /* idempotent */
    must(rt.ext<CommandRegistry>().validate());
    if (withDrivers) {
      Result<std::unique_ptr<SceneDrivers>> d = SceneDrivers::make(*lib);
      must(d.status());
      drivers = std::move(d).value();
    }
  }
  ~DriveDoc()
  {
    drivers.reset();
  }

  /** The document's list of drivers, made when there is none. */
  DriverList &list()
  {
    BlockHandle h = DriverList::find(*lib);
    if (h.isNull()) {
      Result<BlockHandle> made = newDriverList(*lib);
      must(made.status());
      h = *made;
    }
    return *DriverList::of(*lib, h);
  }
  /** A graph block, in the document's list unless `listed` is false. */
  BlockHandle graph(const char *name, bool listed = true)
  {
    Result<BlockHandle> g = newGraph(*lib, name);
    must(g.status());
    if (listed) {
      must(list().insert(*lib, idOf(*g)));
    }
    return *g;
  }
  GraphDef &def(BlockHandle g)
  {
    return *graphDefOf(*lib, g);
  }
  NodeId node(BlockHandle g, const char *type)
  {
    Result<NodeId> n = def(g).addNode(type);
    must(n.status());
    return *n;
  }
  void link(BlockHandle g, NodeId from, const char *out, NodeId to, const char *in)
  {
    must(def(g).link({from, out}, {to, in}).status());
  }
  /** A `scene.drive.<kind>` of the object's channel. */
  NodeId driver(BlockHandle g, const char *kind, BlockHandle object, const char *channel)
  {
    const NodeId n = node(g, (std::string(kDrivePrefix) + kind).c_str());
    must(def(g).setParam(n, "object", pointerTo(idOf(object))));
    must(def(g).setParam(n, "channel", PropValue::ofString(channel)));
    return n;
  }
  NodeId reader(BlockHandle g, BlockHandle object)
  {
    const NodeId n = node(g, kStateNode);
    must(def(g).setParam(n, "object", pointerTo(idOf(object))));
    return n;
  }
  /** A constant vec3 driving the object's location. */
  NodeId
  driveLocation(BlockHandle g, BlockHandle object, double x, double y = 0, double z = 0)
  {
    const NodeId v = node(g, "value.vec3");
    must(def(g).setParam(v, "value", PropValue::ofVec3(x, y, z)));
    const NodeId d = driver(g, "vec3", object, "location");
    link(g, v, "value", d, "value");
    return d;
  }
  /** `location` follows the other object's, plus `dx`. */
  void follow(BlockHandle g, BlockHandle object, BlockHandle leader, double dx = 0)
  {
    const NodeId r = reader(g, leader);
    const NodeId sep = node(g, "vec.separate");
    const NodeId add = node(g, "math.float");
    must(def(g).setParam(add, "op", PropValue::ofEnum(0)));
    must(def(g).setLiteral(add, "b", PropValue::ofFloat(dx)));
    const NodeId comb = node(g, "vec.combine");
    link(g, r, "location", sep, "vector");
    link(g, sep, "x", add, "a");
    link(g, add, "value", comb, "x");
    link(g, sep, "y", comb, "y");
    link(g, sep, "z", comb, "z");
    const NodeId d = driver(g, "vec3", object, "location");
    link(g, comb, "vector", d, "value");
  }
  void moveTo(BlockHandle h, double x)
  {
    put(obj(h).channels, trs::kLocation, double3(x, 0.0, 0.0));
    rt.ext<ChangeBus>().notify(OwnerKey::ofBlock(idOf(h)), "channels");
  }
  /** Where computeState puts it (through the provider, if any). */
  double3 stateAt(BlockHandle h, double time = 0.0)
  {
    Result<TransformState> s = computeState(*lib, h, time);
    must(s.status());
    return at(*s);
  }
  bool evaluatedAt(BlockHandle h, double x, double y = 0, double z = 0)
  {
    return isAt(h, x, y, z);
  }
  bool hasDiagnostic(const char *part)
  {
    for (const DriverDiagnostic &d : drivers->diagnostics()) {
      if (strstr(d.message.c_str(), part)) {
        return true;
      }
    }
    return false;
  }
};

/* A saved document, loaded with drivers of its own: what a sync from scratch gives. */
struct Reloaded {
  Loaded loaded;
  std::unique_ptr<SceneDrivers> drivers;

  explicit Reloaded(DriveDoc &d) : loaded(d, save(*d.lib))
  {
    Result<std::unique_ptr<SceneDrivers>> s = SceneDrivers::make(loaded.lib);
    must(s.status());
    drivers = std::move(s).value();
  }
  ~Reloaded()
  {
    drivers.reset();
  }
  Status evaluate()
  {
    return evaluateScene(loaded.ctx);
  }
  bool evaluated(const char *name, ObjectEval &out) const
  {
    return Scene::of(loaded.lib, loaded.byName("Scene"))
        ->evaluated(loaded.byName(name), out);
  }
};

} // namespace drive_test
