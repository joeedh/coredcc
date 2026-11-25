/* evaluateScene on many native objects, each with a data block that has bounds. Not a
 * test: run it on an idle machine, with the tool, which builds it first.
 *
 *   node tools/bench.mjs scene [-- [--count N] [--reps N] [--leaf N]]
 *
 * One line per case: the best, the median and the worst of the repetitions, in ms. The
 * targets (P3's exit criteria) are for 100,000 objects, and include what evaluateScene
 * does to the scene's BVH: 20 ms when nothing structural changed, 40 when the epoch moved
 * and nothing differs, 80 after a structural change. */
#include "coredcc/api/resolve.h"
#include "coredcc/lib/provider.h"
#include "coredcc/scene/builtins.h"
#include "coredcc/scene/capabilities.h"
#include "coredcc/scene/pick_scene.h"
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
  printf("%-28s best %7.2f  median %7.2f  worst %7.2f\n",
         name,
         times.front(),
         times[times.size() / 2],
         times.back());
  fflush(stdout);
}

} // namespace

int main(int argc, char **argv)
{
  int count = 100000, leaf = 0;
  for (int i = 1; i + 1 < argc; i += 2) {
    if (!strcmp(argv[i], "--count")) {
      count = std::max(atoi(argv[i + 1]), 2);
    } else if (!strcmp(argv[i], "--leaf")) {
      leaf = atoi(argv[i + 1]);
    } else if (!strcmp(argv[i], "--reps")) {
      g_reps = std::max(atoi(argv[i + 1]), 1);
    }
  }

  Runtime rt;
  must(registerSceneTypes(rt));
  if (leaf > 0) {
    rt.ext<SceneSettings>().bvh.leafSize = leaf;
  }
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

  /* Chains of four, moved and rotated. */
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

  const auto evaluate = [&] { must(evaluateScene(ctx)); };
  const auto nothing = [](int) {};
  printf("%d objects, %d repetitions, ms\n", count, g_reps);
  evaluate();
  measure("unchanged", nothing, evaluate);
  measure(
      "every root moved",
      [&](int i) {
        for (int k = 0; k < count; k += 4) {
          objects[k]->channels.set(trs::kLocation + 2, i);
        }
      },
      evaluate);
  measure(
      "epoch moved (selection)",
      [&](int i) {
        Vector<BlockId> ids;
        ids.append(lib.get(all[i % count])->id());
        scene.setSelection(lib, ids);
      },
      evaluate);
  measure(
      "structural (a parent)",
      [&](int i) {
        must(lib.writeRef(objects[count - 1]->parent.cell(),
                          lib.get(all[i % (count / 2)])->id()));
      },
      evaluate);
  measure(
      "structural (unlink, link)",
      [&](int i) {
        const BlockHandle h = all[(i * 7) % count];
        must(scene.remove(lib, lib.get(h)->id()).status());
        must(scene.add(lib, h));
      },
      evaluate);

  const SceneBvh::Counters &n = scene.bvh().counters();
  printf("BVH: %d builds, %d refits\n", n.builds, n.refits);
  /* Objects, at 1,000 places. */
  Vector<BlockHandle> found;
  size_t sum = 0;
  const auto place = [](int k) { return double3(k * 7 % 317, k * 13 % 315, g_reps - 1); };
  measure("objects on 1000 rays", nothing, [&] {
    for (int k = 0; k < 1000; k++) {
      Ray ray;
      ray.origin = place(k) + double3(0.0, 0.0, 200.0);
      ray.dir = double3(0.1, 0.05, -1.0);
      must(scene.bvh().objectsOnRay(ray, found));
      sum += found.size();
    }
  });
  measure("objects near 1000 points", nothing, [&] {
    for (int k = 0; k < 1000; k++) {
      must(scene.bvh().objectsNear(place(k), 3.0, found));
      sum += found.size();
    }
  });
  printf("  %d objects\n", int(sum / size_t(g_reps)));
  View v;
  v.viewport[2] = 1920.0;
  v.viewport[3] = 1080.0;
  v.view = litestl::math::lookAt(
      double3(150.0, -100.0, 100.0), double3(150.0, 150.0, 0.0), double3(0.0, 0.0, 1.0));
  v.proj = litestl::math::perspective(1.0, 16.0 / 9.0, 0.1, 10000.0, v.depth, false);
  ViewMath view;
  must(view.set(v));
  for (double radius : {10.0, 200.0}) {
    Vector<PickHit> hits;
    size_t picked = 0;
    const std::string name = "pick, radius " + std::to_string(int(radius));
    measure(name.c_str(), nothing, [&] {
      PickOptions options;
      options.radius = radius;
      must(pickScene(ctx, *sceneBlock, view, 960.0, 540.0, options, {}, hits));
      picked = hits.size();
    });
    printf("  %d objects\n", int(picked));
  }
  return 0;
}
