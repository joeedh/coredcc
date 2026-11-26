/* evaluateScene with graphs that drive objects (SceneDrivers). Not a test: run it on an
 * idle machine, with the tool, which builds it first.
 *
 *   node tools/bench.mjs drive [-- [--count N] [--reps N]]
 *
 * As bench_scene: chains of four objects, each with a data block that has bounds. Then 10
 * listed graphs, each with 10 readers of objects and 100 drivers of others' locations, a
 * tenth of them through the time. One line per case: best, median and worst, in ms. */
#include "coredcc/drive/drive.h"
#include "coredcc/drive/node_types.h"
#include "coredcc/graph/commands.h"
#include "coredcc/graph/provider.h"
#include "coredcc/lib/provider.h"
#include "coredcc/scene/builtins.h"
#include "coredcc/scene/capabilities.h"
#include "coredcc/scene/scene.h"

#include "litestl/util/alloc.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace coredcc;
namespace lbt = litestl::binding::types;
using litestl::util::Vector;

namespace {

struct Shape {
  Box3d box;
  Box3d localBounds() const
  {
    return box;
  }
  uint32_t domains() const
  {
    return 1;
  }
  void closestElements(const NearQuery &, NearSink &) const
  {
  }
  static lbt::Struct<Shape> *defineBindings()
  {
    auto *st = new lbt::Struct<Shape>("bench::Shape", sizeof(Shape));
    BIND_STRUCT_METHOD(st, localBounds, MARGS());
    BIND_STRUCT_METHOD(st, domains, MARGS());
    BIND_STRUCT_METHOD(st, closestElements, MARGS("query", "sink"));
    return st;
  }
};

struct ShapeProvider {
  void *data(const Datablock &b) const
  {
    return b.clientData();
  }
  void foreachRef(const Datablock &, RefVisitor &) const
  {
  }
  void destroy(const Datablock &b)
  {
    litestl::alloc::Delete(static_cast<Shape *>(b.clientData()));
  }
  static lbt::Struct<ShapeProvider> *defineBindings()
  {
    auto *st =
        new lbt::Struct<ShapeProvider>("bench::ShapeProvider", sizeof(ShapeProvider));
    BIND_STRUCT_METHOD(st, data, MARGS("block"));
    BIND_STRUCT_METHOD(st, foreachRef, MARGS("block", "visitor"));
    BIND_STRUCT_METHOD(st, destroy, MARGS("block"));
    return st;
  }
};

void must(const Status &s)
{
  if (!s.ok()) {
    fprintf(stderr, "failed: %s\n", s.message());
    exit(1);
  }
}

int g_reps = 41;

/* `before` isn't measured. */
template <class Before, class Fn> void measure(const char *name, Before &&before, Fn &&fn)
{
  std::vector<double> times;
  for (int i = 0; i < g_reps; i++) {
    before(i);
    const auto from = std::chrono::steady_clock::now();
    fn();
    times.push_back(
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - from)
            .count());
  }
  std::sort(times.begin(), times.end());
  printf("%-34s best %8.3f  median %8.3f  worst %8.3f\n",
         name,
         times.front(),
         times[times.size() / 2],
         times.back());
  fflush(stdout);
}

PropValue pointerTo(const BlockId &id)
{
  StableRef r;
  r.owner = OwnerKey::ofBlock(id);
  return PropValue::ofPointer(r);
}

NodeId add(GraphDef &def, const char *type)
{
  Result<NodeId> n = def.addNode(type);
  must(n.status());
  return *n;
}

void link(GraphDef &def, NodeId from, const char *out, NodeId to, const char *in)
{
  must(def.link({from, out}, {to, in}).status());
}

} // namespace

int main(int argc, char **argv)
{
  int count = 100000;
  for (int i = 1; i + 1 < argc; i += 2) {
    if (!strcmp(argv[i], "--count")) {
      count = std::max(atoi(argv[i + 1]), 4000);
    } else if (!strcmp(argv[i], "--reps")) {
      g_reps = std::max(atoi(argv[i + 1]), 1);
    }
  }

  Runtime rt;
  must(registerSceneTypes(rt));
  must(registerGraphCommands(rt));
  must(registerDriveTypes(rt));
  must(registerImpl<BlockProvider, ShapeProvider>(rt, "bench.Shape#provider"));
  must(registerImpl<Spatial, Shape>(rt, "bench.Shape"));
  Result<Owned<BlockProvider>> provider = own<BlockProvider>(
      rt, "bench.Shape#provider", litestl::alloc::New<ShapeProvider>("provider"));
  must(provider.status());
  BlockTypeInfo info;
  info.name = "bench.Shape";
  info.client = ClientId::of(kCoredccClient);
  info.storage = BlockStorage::Transient;
  must(rt.ext<BlockTypeRegistry>().add(info, std::move(*provider)).status());

  Library lib(&rt);
  Context ctx(&rt, &lib, PointerRef());
  Result<BlockHandle> sceneBlock = newScene(lib, "Scene");
  must(sceneBlock.status());
  Scene &scene = *Scene::of(lib, *sceneBlock);
  ctx.setScene(*sceneBlock);

  std::vector<BlockHandle> all;
  std::vector<Object *> objects;
  for (int i = 0; i < count; i++) {
    Result<BlockHandle> h = newObject(lib, "Object", kTrsTransform);
    must(h.status());
    Object *o = static_cast<Object *>(lib.data(*h));
    o->channels.set(trs::kLocation, i % 317);
    o->channels.set(trs::kLocation + 1, i / 317);
    o->channels.set(trs::kRotationEuler + 2, i * 0.01);
    if (i % 4 != 0) {
      must(lib.writeRef(o->parent.cell(), lib.get(all[i - 1])->id()));
    }
    Shape *s = litestl::alloc::New<Shape>("Shape");
    for (int k = 0; k < 3; k++) {
      s->box.min[k] = -1.0;
      s->box.max[k] = 1.0;
    }
    Result<BlockHandle> data = lib.create(TypeId::of("bench.Shape"), "Shape", s);
    must(data.status());
    must(lib.writeRef(o->data.cell(), lib.get(*data)->id()));
    must(scene.add(lib, *h));
    all.push_back(*h);
    objects.push_back(o);
  }
  const auto idOf = [&](int i) { return lib.get(all[i])->id(); };
  ChangeBus &bus = rt.ext<ChangeBus>();
  const auto evaluate = [&] { must(evaluateScene(ctx)); };
  const auto nothing = [](int) {};
  const auto selectOne = [&](int i) {
    Vector<BlockId> ids;
    ids.append(idOf(i % count));
    scene.setSelection(lib, ids);
  };
  printf("%d objects, %d repetitions, ms\n", count, g_reps);
  evaluate();
  measure("no drivers: unchanged", nothing, evaluate);
  measure("no drivers: epoch moved", selectOne, evaluate);

  Result<std::unique_ptr<SceneDrivers>> drivers = SceneDrivers::make(lib);
  must(drivers.status());
  evaluate();
  measure("drivers, no graphs: unchanged", nothing, evaluate);
  measure("drivers, no graphs: epoch moved", selectOne, evaluate);

  /* Readers of the last object of a chain (so G has its three ancestors too); drivers of
   * roots far from them. */
  std::vector<int> read;
  for (int g = 0; g < 10; g++) {
    Result<BlockHandle> gb = createGraph(lib, ("g" + std::to_string(g)).c_str());
    must(gb.status());
    GraphDef &def = *graphDefOf(lib, *gb);
    const NodeId time = add(def, drive::kTimeNode);
    const NodeId comb = add(def, "vec.combine");
    link(def, time, "seconds", comb, "x");
    for (int r = 0; r < 10; r++) {
      const int object = 4 * (g * 10 + r) + 3;
      read.push_back(object);
      const NodeId reader = add(def, drive::kStateNode);
      must(def.setParam(reader, "object", pointerTo(idOf(object))));
      for (int k = 0; k < 10; k++) {
        const int target = count / 2 + 4 * (g * 100 + r * 10 + k);
        const NodeId d = add(def, "scene.drive.vec3");
        must(def.setParam(d, "object", pointerTo(idOf(target))));
        must(def.setParam(d, "channel", PropValue::ofString("location")));
        if (k == 0) {
          link(def, comb, "vector", d, "value");
        } else {
          link(def, reader, "location", d, "value");
        }
      }
    }
    must(scene.insertGraph(lib, lib.get(*gb)->id()));
  }
  evaluate();
  printf("G has %d objects; %d driven\n", (*drivers)->transformCount(), 1000);
  measure("drivers: unchanged", nothing, evaluate);
  measure(
      "drivers: a read object moved",
      [&](int i) {
        const int object = read[i % read.size()];
        objects[object]->channels.set(trs::kLocation + 2, i);
        bus.notify(OwnerKey::ofBlock(idOf(object)), "channels");
      },
      evaluate);
  measure("drivers: the time changed", [&](int i) { scene.frame = i + 1; }, evaluate);
  measure("drivers: epoch moved", selectOne, evaluate);
  const BlockHandle driven = all[count / 2 + 4];
  must(computeState(lib, driven, scene.time()).status());
  std::vector<double> times;
  measure("computeState of a driven one x1000", nothing, [&] {
    for (int i = 0; i < 1000; i++) {
      must(computeState(lib, driven, scene.time()).status());
    }
  });
  drivers->reset();
  return 0;
}
