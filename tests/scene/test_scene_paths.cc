/* Object, Scene and the results of evaluateScene by datapath. */
#include "scene_doc.h"

test_init;

using namespace scene_test;

namespace {

PropValue blockValue(Doc &d, BlockHandle h)
{
  StableRef r;
  r.owner = OwnerKey::ofBlock(d.idOf(h));
  r.type = d.rt.ext<BlockTypeRegistry>().find(d.lib->get(h)->type())->info.dataStruct;
  return PropValue::ofPointer(r);
}

PropValue read(Doc &d, const std::string &path)
{
  Result<PropValue> v = getValue(*d.ctx, path);
  if (!v.ok()) {
    fprintf(stderr, "%s: %s\n", path.c_str(), v.status().message());
  }
  test_assert(v.ok());
  return v.ok() ? *v : PropValue();
}

StatusCode readError(Doc &d, const std::string &path)
{
  return getValue(*d.ctx, path).status().code();
}

Status write(Doc &d, const std::string &path, const PropValue &v)
{
  return setValue(*d.ctx, path, v);
}

void testObject()
{
  Doc d;
  const BlockHandle a = d.object("a", 1, 2, 3), b = d.object("b"), h = d.hostObject("h");
  const BlockHandle shape = d.shapeBlock("shape", 1.0);
  const std::string p = "scene.objects[\"a\"].";
  test_assert(read(d, p + "name") == PropValue::ofString("a"));
  test_assert(!write(d, p + "name", PropValue::ofString("x")).ok());
  test_assert(!strcmp(d.lib->get(a)->name().c_str(), "a"));
  test_assert(read(d, p + "transformType") == PropValue::ofString(kTrsTransform));
  test_assert(!write(d, p + "transformType", PropValue::ofString(kFbxTransform)).ok());

  must(write(d, p + "flags[HIDDEN]", PropValue::ofBool(true)));
  test_assert(d.obj(a).flags == ObjectFlags::Hidden);
  must(write(d, p + "flags[UNSELECTABLE]", PropValue::ofBool(true)));
  must(write(d, p + "flags[HIDDEN]", PropValue::ofBool(false)));
  test_assert(d.obj(a).flags == ObjectFlags::Unselectable);
  Result<ObjectRead> r = readObject(*d.lib, a);
  test_assert(r.ok() && r->info.flags == uint32_t(ObjectFlags::Unselectable));

  /* The transform, a struct of the transform type over the channels. */
  test_assert(read(d, p + "transform.location") == PropValue::ofVec3(1, 2, 3));
  must(write(d, p + "transform.location[1]", PropValue::ofFloat(9.0)));
  test_assert(d.obj(a).channels.get(trs::kLocation + 1) == 9.0);
  Result<ResolvedPath> rp = resolve(*d.ctx, p + "transform.scale");
  test_assert(rp.ok() && rp->canonical.owner == OwnerKey::ofBlock(d.idOf(a)));
  d.obj(a).setTransformType(kFbxTransform);
  d.obj(a).channels = d.fbx->defaults();
  test_assert(readError(d, p + "transform.location") == StatusCode::NotFound);
  test_assert(read(d, p + "transform.inheritType").type() == PropValue::Type::Enum);
  d.obj(a).setTransformType("test.missing");
  test_assert(!getValue(*d.ctx, p + "transform.inheritType").ok());
  test_assert(read(d, p + "transformType") == PropValue::ofString("test.missing"));

  /* Links. */
  test_assert(read(d, p + "parent") == PropValue::nullPointer());
  must(write(d, p + "parent", blockValue(d, b)));
  test_assert(d.lib->resolveRef(d.obj(a).parent.cell()) == b);
  test_assert(read(d, p + "parent.name") == PropValue::ofString("b"));
  test_assert(isError(write(d, "scene.objects[\"b\"].parent", blockValue(d, a)),
                      StatusCode::FailedPrecondition,
                      "ancestor"));
  test_assert(
      isError(write(d, p + "parent", blockValue(d, a)), StatusCode::FailedPrecondition, "ancestor"));
  must(write(d, p + "parent", blockValue(d, h))); /* a host's kind of object */
  test_assert(d.lib->resolveRef(d.obj(a).parent.cell()) == h);
  test_assert(!write(d, p + "parent", blockValue(d, shape)).ok());
  must(write(d, p + "parent", PropValue::nullPointer()));
  test_assert(d.obj(a).parent.cell().isNull());
  must(write(d, p + "data", blockValue(d, shape)));
  test_assert(d.lib->resolveRef(d.obj(a).data.cell()) == shape);
  test_assert(!write(d, p + "data", blockValue(d, b)).ok());
  must(write(d, p + "data", PropValue::nullPointer()));
  test_assert(d.obj(a).data.cell().isNull());
}

void testScene()
{
  Doc d;
  const BlockHandle a = d.object("a"), b = d.object("twin"), c = d.object("twin"),
                    h = d.hostObject("h"), out = d.loose("out");
  test_assert(read(d, "scene.name") == PropValue::ofString("Scene"));
  must(write(d, "scene.frame", PropValue::ofFloat(12.5)));
  test_assert(d.sc().frame == 12.5 && read(d, "scene.frame") == PropValue::ofFloat(12.5));
  must(write(d, "scene.fps", PropValue::ofFloat(25.0)));
  test_assert(d.sc().fps == 25.0);

  test_assert(read(d, "scene.objects.length") == PropValue::ofInt(4));
  test_assert(read(d, "scene.objects[0].name") == PropValue::ofString("a"));
  test_assert(read(d, "scene.objects[2]") == blockValue(d, c));
  test_assert(readError(d, "scene.objects[4]") == StatusCode::OutOfRange);
  test_assert(read(d, "scene.objects[\"twin\"]") == blockValue(d, b)); /* the first one */
  test_assert(readError(d, "scene.objects[\"out\"]") == StatusCode::NotFound);

  /* By handle. */
  const auto key = [](BlockHandle x) {
    return std::to_string((int64_t(x.generation) << 32) | int64_t(x.index));
  };
  test_assert(read(d, "scene.objects[#" + key(c) + "]") == blockValue(d, c));
  test_assert(readError(d, "scene.objects[#" + key(out) + "]") == StatusCode::NotFound);
  test_assert(readError(d, "scene.objects[#1]") == StatusCode::NotFound);
  const PointerRef holder = detail::blockRoot(*d.ctx, d.scene);
  const ApiStruct *st = d.rt.ext<ApiRegistry>().find(StructId::of(kSceneType));
  const CollectionAccessor &objects = st->findOwn("objects")->collection();
  const PointerRef item = objects.at(holder, *d.ctx, 2);
  const KeySegment k = objects.keyOf(holder, *d.ctx, item);
  test_assert(k.kind == KeySegment::Kind::Handle);
  test_assert(objects.lookup(holder, *d.ctx, k).data == item.data);

  /* The active object: a member of the scene, also a host's. */
  test_assert(read(d, "scene.activeObject") == PropValue::nullPointer());
  test_assert(readError(d, "scene.objects.active") == StatusCode::NotFound);
  must(write(d, "scene.objects.active", blockValue(d, b)));
  test_assert(d.sc().active(*d.lib) == b && read(d, "scene.objects.active.name") ==
                                                PropValue::ofString("twin"));
  must(write(d, "scene.activeObject", blockValue(d, h)));
  test_assert(d.sc().active(*d.lib) == h);
  test_assert(isError(write(d, "scene.activeObject", blockValue(d, out)),
                      StatusCode::FailedPrecondition,
                      "isn't in the scene"));
  test_assert(!write(d, "scene.activeObject", blockValue(d, d.scene)).ok());
  test_assert(d.sc().active(*d.lib) == h);
  must(write(d, "scene.objects.active", PropValue::nullPointer()));
  test_assert(!d.sc().active(*d.lib) && d.sc().activeCell().isNull());

  /* The selection. */
  test_assert(read(d, "scene.selectedObjects.length") == PropValue::ofInt(0));
  Vector<BlockId> ids;
  for (BlockHandle x : {c, out, a}) {
    ids.append(d.idOf(x));
  }
  d.sc().setSelection(*d.lib, ids);
  test_assert(read(d, "scene.selectedObjects.length") == PropValue::ofInt(2));
  test_assert(read(d, "scene.selectedObjects[0]") == blockValue(d, c));
  test_assert(read(d, "scene.selectedObjects[\"a\"]") == blockValue(d, a));
  test_assert(readError(d, "scene.selectedObjects[\"out\"]") == StatusCode::NotFound);
  test_assert(readError(d, "scene.selectedObjects[2]") == StatusCode::OutOfRange);
  test_assert(read(d, "scene.selectedObjects[#" + key(c) + "]") == blockValue(d, c));
  test_assert(readError(d, "scene.selectedObjects[#" + key(b) + "]") == StatusCode::NotFound);

  /* Without a Library. */
  Context none(&d.rt, nullptr, d.ctx->root());
  none.setScene(d.scene);
  test_assert(!getValue(none, "scene.objects.length").ok());
  test_assert(getValue(none, "evaluated.objects.length").ok());
}

void testIdText()
{
  Doc d;
  BlockId id;
  id.client = ClientId::of(kCoredccClient);
  id.local.hi = 0x0123456789abcdefull;
  id.local.lo = 5;
  const std::string text = blockIdText(d.rt, id);
  test_assert(text == std::string(kCoredccClient) + ",0123456789abcdef0000000000000005");
  test_assert(parseBlockIdText(text) == id);
  id.client = ClientId::of("nobody.knows.it");
  const std::string unknown = blockIdText(d.rt, id);
  test_assert(unknown[0] == '#' && parseBlockIdText(unknown) == id);
  const std::string hex(32, 'a');
  for (const std::string &bad : {std::string(),
                                 std::string(","),
                                 "coredcc" + hex,
                                 "," + hex,
                                 "coredcc," + hex + "0",
                                 "coredcc," + hex.substr(1),
                                 "coredcc," + std::string(31, 'a') + "G",
                                 "coredcc," + std::string(32, 'A'),
                                 "#12," + hex,
                                 "#" + std::string(16, 'x') + "," + hex})
  {
    test_assert(parseBlockIdText(bad).isNull());
  }
  test_assert(parseBlockIdText("a,b," + hex).client == ClientId::of("a,b"));
}

void testEvaluated()
{
  Doc d;
  const BlockHandle a = d.object("a", 1, 2, 3), b = d.object("b", 10);
  d.setParent(b, a);
  const BlockHandle shape = d.shapeBlock("shape", 1.0);
  d.setData(b, shape);
  const auto path = [&d](BlockHandle h, const char *member) {
    return "evaluated.objects[\"" + blockIdText(d.rt, d.idOf(h)) + "\"]." + member;
  };
  test_assert(readError(d, path(a, "world")) == StatusCode::NotFound);
  test_assert(read(d, "evaluated.objects.length") == PropValue::ofInt(0));
  const uint64_t edit = d.rt.lastEdit();
  d.evaluate();
  test_assert(d.rt.lastEdit() == edit);
  const ObjectEval e = d.result(b);
  test_assert(read(d, path(b, "world")) ==
              PropValue::ofComponents(PropValue::Type::Mat4,
                                      static_cast<const double *>(e.state.world)));
  test_assert(read(d, path(b, "boundsMin")) ==
              PropValue::ofVec3(e.bounds.min[0], e.bounds.min[1], e.bounds.min[2]));
  test_assert(read(d, path(b, "boundsMax")) ==
              PropValue::ofVec3(e.bounds.max[0], e.bounds.max[1], e.bounds.max[2]));
  test_assert(read(d, path(b, "status")) == PropValue::ofEnum(int64_t(EvalStatus::Ok)));
  test_assert(read(d, "evaluated.objects.length") == PropValue::ofInt(2));
  test_assert(readError(d, "evaluated.objects[2]") == StatusCode::OutOfRange);
  test_assert(readError(d, "evaluated.objects[\"a\"]") == StatusCode::NotFound);
  test_assert(readError(d, path(shape, "world")) == StatusCode::NotFound);

  /* Read-only, and addressed by id. */
  test_assert(!write(d, path(b, "status"), PropValue::ofEnum(1)).ok());
  Result<StableRef> ref = canonicalize(*d.ctx, *parsePath("evaluated.objects[1].world"));
  test_assert(ref.ok());
  Result<ResolvedPath> again = resolve(*d.ctx, *ref);
  test_assert(again.ok() && *getValue(*d.ctx, *again) == read(d, path(b, "world")));

  /* Watchers. */
  ChangeBus &bus = d.rt.ext<ChangeBus>();
  int fired = 0;
  Result<ScopedConnection> w = bus.watch(
      *d.ctx, path(b, "world"), [&](const PropValue &, const Status &) { fired++; });
  must(w.status());
  bus.flush();
  int heard = 0;
  ScopedConnection l = bus.addListener([&](const OwnerKey *k) {
    heard++;
    test_assert(*k == d.rt.ext<ApiRegistry>().runtimeOwner(kEvaluatedOwner).owner);
    test_assert(!d.rt.evaluating());
  });
  d.evaluate(); /* nothing differs */
  bus.flush();
  test_assert(fired == 0 && heard == 0);
  d.obj(a).channels.set(trs::kLocation, 4.0);
  d.evaluate();
  test_assert(heard == 1);
  bus.flush();
  test_assert(fired == 1);
  /* Only the bounds, or only the status. */
  d.shape(shape).box.max[0] = 2.0;
  d.evaluate();
  test_assert(heard == 2);
  bus.flush();
  test_assert(fired == 1);
  d.obj(a).setTransformType("test.missing");
  put(d.obj(b).channels, trs::kLocation, double3(0, 0, 0));
  d.evaluate();
  d.obj(a).setTransformType(kTrsTransform);
  d.obj(a).channels = d.trs->defaults();
  heard = 0;
  d.evaluate();
  test_assert(heard == 1 && d.isAt(b, 0));
  /* Another parent that changes nothing. */
  heard = 0;
  d.setParent(b, d.object("still"));
  d.evaluate();
  test_assert(heard == 1); /* one more object */
  d.setParent(b, a);
  d.evaluate();
  test_assert(heard == 1 && d.sc().counters().rebuilds > 2);
  bus.flush();

  /* Another scene, and none. */
  const BlockHandle other = d.addScene("Other");
  fired = 0;
  d.ctx->setScene(other);
  bus.flush();
  test_assert(fired == 1 && readError(d, path(b, "world")) == StatusCode::NotFound);
  d.ctx->setScene(d.scene);
  bus.flush();
  test_assert(fired == 2 && read(d, path(b, "world")).type() == PropValue::Type::Mat4);
  d.ctx->setScene(BlockHandle());
  test_assert(readError(d, path(b, "world")) == StatusCode::NotFound);
  test_assert(read(d, "evaluated.objects.length") == PropValue::ofInt(0));
  d.ctx->setScene(d.scene);

  /* Objects that went since. */
  must(d.lib->markDeleted(b));
  must(d.lib->destroyFromHistory(b));
  test_assert(read(d, "evaluated.objects.length") == PropValue::ofInt(2));
  d.evaluate();
  test_assert(read(d, "evaluated.objects.length") == PropValue::ofInt(2));
}

} // namespace

int main()
{
  testObject();
  testScene();
  testIdText();
  testEvaluated();
  return test_end();
}
