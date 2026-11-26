/* drive/'s node types, each by itself in a standalone graph: the transform kernel against
 * computeState, readers, drivers and time. */
#include "drive_doc.h"

#include "coredcc/graph/instance.h"

test_init;

using namespace drive_test;

namespace {

struct Standalone {
  DriveDoc &d;
  GraphDef def;
  GraphInstance inst;

  explicit Standalone(DriveDoc &doc)
      : d(doc), def(&doc.rt, doc.lib.get()),
        inst(&doc.rt, doc.lib.get(), GraphSource(&def))
  {
  }
  NodeId transform(BlockHandle object, const TransformType *type)
  {
    Result<const NodeType *> t = transformNodeType(d.rt, type);
    must(t.status());
    Result<NodeId> n = def.addNode((*t)->name.c_str());
    must(n.status());
    must(def.setParam(*n, "object", pointerTo(d.idOf(object))));
    const BlockId parent = d.obj(object).parent.cell().target;
    must(def.setParam(*n, "parentId", PropValue::ofString(formatBlockId(d.rt, parent))));
    return *n;
  }
  NodeId add(const char *type)
  {
    Result<NodeId> n = def.addNode(type);
    must(n.status());
    return *n;
  }
  const GraphValue *value(NodeId n, const char *socket)
  {
    const SocketPath p = SocketPath::of(n, socket);
    (void)inst.evaluate(p);
    return inst.value(p);
  }
  TransformState state(NodeId n)
  {
    const GraphValue *v = value(n, "state");
    test_assert(v && v->payload<TransformState>());
    return v && v->payload<TransformState>() ? *v->payload<TransformState>()
                                             : TransformState();
  }
  int64_t status(NodeId n)
  {
    const GraphValue *v = value(n, "status");
    test_assert(v != nullptr);
    return v ? v->asInt() : -99;
  }
};

TransformState computed(DriveDoc &d, BlockHandle h)
{
  Result<TransformState> s = computeState(*d.lib, h, 0.0);
  must(s.status());
  return *s;
}

/* Nodes for a parent and child, linked, agree bitwise with computeState for each type. */
void testTransform()
{
  DriveDoc d(false);
  for (const char *typeName : {kTrsTransform, kFbxTransform, kMatrixTransform}) {
    const BlockHandle p = d.object("p", 1, 2, 3);
    put(d.obj(p).channels, trs::kRotationEuler, double3(0.1, 0.2, 0.3));
    const BlockHandle c = d.object("c", 4, 0, 0);
    if (strcmp(typeName, kTrsTransform) != 0) {
      Result<PropertyBag> r =
          d.run(kSetObjectTransformTypeCommand,
                {{"objects", PropValue::ofString(formatBlockId(d.rt, d.idOf(c)))},
                 {"type", PropValue::ofString(typeName)}});
      must(r.status());
    }
    d.setParent(c, p);
    const TransformType *type = d.rt.ext<TransformRegistry>().find(typeName);

    Standalone g(d);
    const NodeId np = g.transform(p, d.trs), nc = g.transform(c, type);
    must(g.def.link({np, "state"}, {nc, "parent"}).status());
    test_assert(sameState(g.state(np), computed(d, p)));
    test_assert(sameState(g.state(nc), computed(d, c)));
    test_assert(g.status(nc) == int64_t(EvalStatus::Ok));

    /* A stale parent, then a stale type. */
    d.setParent(c, BlockHandle());
    d.rt.ext<ChangeBus>().notify(OwnerKey::ofBlock(d.idOf(c)), "parent");
    test_assert(g.status(nc) == kStaleStatus);
    d.setParent(c, p);
    d.rt.ext<ChangeBus>().notify(OwnerKey::ofBlock(d.idOf(c)), "parent");
    test_assert(g.status(nc) == int64_t(EvalStatus::Ok));
    const NodeId wrong = g.transform(c, strcmp(typeName, kTrsTransform) ? d.trs : d.fbx);
    test_assert(g.status(wrong) == kStaleStatus);
  }
}

/* A linked channel input overrides the stored channel; the result is what computeState
 * gives with that channel stored. The cycle param turns Ok into Cycle. */
void testOverrides()
{
  DriveDoc d(false);
  const BlockHandle o = d.object("o", 1, 0, 0);
  Standalone g(d);
  const NodeId n = g.transform(o, d.trs);
  const NodeId v = g.add("value.vec3");
  must(g.def.setParam(v, "value", PropValue::ofVec3(7, 8, 9)));
  must(g.def.link({v, "value"}, {n, "c_location"}).status());
  const TransformState driven = g.state(n);
  test_assert(at(driven)[0] == 7 && at(driven)[1] == 8 && at(driven)[2] == 9);
  put(d.obj(o).channels, trs::kLocation, double3(7, 8, 9));
  test_assert(sameState(driven, computed(d, o)));

  must(g.def.setParam(n, "cycle", PropValue::ofBool(true)));
  test_assert(g.status(n) == int64_t(EvalStatus::Cycle));

  /* Not Transformable, or not there: the node fails. */
  const BlockHandle shape = d.shapeBlock("s", 1);
  Result<NodeId> bad = g.def.addNode("scene.transform.core.transform.trs");
  must(g.def.setParam(*bad, "object", pointerTo(d.idOf(shape))));
  const SocketPath p = SocketPath::of(*bad, "state");
  (void)g.inst.evaluate(p);
  test_assert(g.inst.value(p) == nullptr && g.inst.error(p) != nullptr);
}

/* An unregistered type: `scene.transform.missing` gives the parent's state and
 * MissingModel, as the direct pass does. */
void testMissing()
{
  DriveDoc d(false);
  const BlockHandle p = d.object("p", 5, 0, 0);
  const BlockHandle h = d.hostObject("h", 3);
  d.host(h).info.transformType = TypeId::of("test.notRegistered");
  d.host(h).info.parent = d.idOf(p);
  Standalone g(d);
  const NodeId np = g.transform(p, d.trs);
  Result<const NodeType *> t = transformNodeType(d.rt, nullptr);
  test_assert(t.ok() && strcmp((*t)->name.c_str(), kMissingTransform) == 0);
  const NodeId nh = g.add(kMissingTransform);
  must(g.def.setParam(nh, "object", pointerTo(d.idOf(h))));
  must(g.def.setParam(
      nh, "parentId", PropValue::ofString(formatBlockId(d.rt, d.idOf(p)))));
  must(g.def.link({np, "state"}, {nh, "parent"}).status());
  test_assert(g.status(nh) == int64_t(EvalStatus::MissingModel));
  test_assert(sameState(g.state(nh), computed(d, p)));
  test_assert(sameState(g.state(nh), computed(d, h)));
}

/* Readers give the parts of a state; a reader without a state fails. Drivers pass their
 * value. Time gives its input. */
void testAdapters()
{
  DriveDoc d(false);
  const BlockHandle o = d.object("o", 1, 2, 3);
  d.obj(o).channels.set(trs::kScale, 2.0);
  Standalone g(d);
  const NodeId n = g.transform(o, d.trs);
  const NodeId r = g.add(kStateNode);
  must(g.def.setParam(r, "object", pointerTo(d.idOf(o))));
  must(g.def.link({n, "state"}, {r, "state"}).status());
  must(g.def.link({n, "status"}, {r, "status"}).status());
  const GraphValue *loc = g.value(r, "location");
  test_assert(loc && loc->components()[0] == 1 && loc->components()[2] == 3);
  const GraphValue *scale = g.value(r, "scale");
  test_assert(scale && scale->components()[0] == 2 && scale->components()[1] == 1);
  const GraphValue *rot = g.value(r, "rotation");
  test_assert(rot && rot->components()[3] == 1);
  const GraphValue *world = g.value(r, "world");
  test_assert(world && world->components()[12] == 1);

  const NodeId lonely = g.add(kStateNode);
  const SocketPath p = SocketPath::of(lonely, "location");
  (void)g.inst.evaluate(p);
  test_assert(g.inst.error(p) &&
              strstr(g.inst.error(p)->status.message(), "SceneDrivers"));

  for (const char *kind : {"bool", "int", "float", "vec2", "vec3", "vec4", "quat"}) {
    const std::string name = std::string(kDrivePrefix) + kind;
    test_assert(kindOfDriveType(name) != DriveKind::None);
    const NodeId v = g.add((std::string("value.") + kind).c_str());
    const NodeId dr = g.add(name.c_str());
    must(g.def.link({v, "value"}, {dr, "value"}).status());
    const GraphValue *out = g.value(dr, "value");
    const GraphValue *in = g.value(v, "value");
    double a[4] = {}, b[4] = {};
    test_assert(out && in && flatten(*out, kindOfDriveType(name), a) &&
                flatten(*in, kindOfDriveType(name), b) && memcmp(a, b, sizeof(a)) == 0);
  }
  test_assert(kindOfDriveType("scene.drive.mat4") == DriveKind::None);
  test_assert(kindOf(PropKind::Enum) == DriveKind::Int);

  const NodeId t = g.add(kTimeNode);
  must(g.def.setLiteral(t, "time", PropValue::ofFloat(2.5)));
  const GraphValue *seconds = g.value(t, "seconds");
  test_assert(seconds && seconds->asFloat() == 2.5);
}

} // namespace

int main()
{
  testTransform();
  testOverrides();
  testMissing();
  testAdapters();
  return test_end();
}
