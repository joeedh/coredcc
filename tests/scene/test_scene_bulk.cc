/* Results for drawing: the hierarchy, flags, what an evaluation did, bounds that are kept
 * until they change, and transform types that are evaluated many at a time. */
#include "command_doc.h"

#include "coredcc/api/ref_text.h"
#include "coredcc/scene/overrides.h"

#include <cmath>
#include <limits>

test_init;

using namespace scene_test;

namespace {

/* ---- the hierarchy ---- */

void testHierarchy()
{
  int cycles = 0, outsiders = 0;
  for (int k = 0; k < 40 * coredcc_test::testScale(); k++) {
    Rand r(k);
    Doc d;
    Vector<BlockHandle> all;
    for (int i = 0; i < 20; i++) {
      const std::string name = "o" + std::to_string(i);
      all.append(r.below(5) ? d.object(name.c_str()) : d.loose(name.c_str()));
    }
    test_assert(d.sc().children(BlockHandle()).size() == 0);
    for (int step = 0; step < 6; step++) {
      for (int i = 0; i < 4; i++) {
        const BlockHandle h = all[int(r.below(20))];
        d.setParent(h, r.below(3) ? all[int(r.below(20))] : BlockHandle());
        d.obj(h).flags = ObjectFlags(r.below(4));
      }
      d.evaluate();
      const Vector<BlockHandle> places = d.sc().evaluatedObjects();
      const std::span<const ObjectEval> results = d.sc().results();
      test_assert(results.size() == places.size());
      /* Whether a walk up from h comes back to it. */
      const auto onCycle = [&](BlockHandle h) {
        BlockHandle p = d.lib->resolveRef(d.obj(h).parent.cell());
        for (int n = 0; p && n < 30; n++) {
          if (p == h) {
            return true;
          }
          p = d.lib->resolveRef(d.obj(p).parent.cell());
        }
        return false;
      };
      Vector<BlockHandle> roots;
      for (int i = 0; i < int(places.size()); i++) {
        const ObjectEval &e = results[size_t(i)];
        const BlockHandle h = places[i], p = d.lib->resolveRef(d.obj(h).parent.cell());
        const bool cycle = onCycle(h);
        cycles += cycle ? 1 : 0;
        outsiders += e.member ? 0 : 1;
        test_assert(cycle || !p ? e.parent == -1 : places[e.parent] == p);
        test_assert(e.flags == uint32_t(d.obj(h).flags) && e.data == BlockHandle());
        test_assert(e.member == d.sc().contains(*d.lib, h));
        if (cycle || !p) {
          roots.append(h);
        }
        Vector<BlockHandle> want;
        for (BlockHandle c : places) {
          if (!onCycle(c) && d.lib->resolveRef(d.obj(c).parent.cell()) == h) {
            want.append(c);
          }
        }
        const std::span<const BlockHandle> got = d.sc().children(h);
        test_assert(got.size() == want.size());
        for (int c = 0; c < int(got.size()) && c < int(want.size()); c++) {
          test_assert(got[size_t(c)] == want[c]);
        }
      }
      const std::span<const BlockHandle> got = d.sc().children(BlockHandle());
      test_assert(got.size() == roots.size());
      for (int c = 0; c < int(got.size()) && c < int(roots.size()); c++) {
        test_assert(got[size_t(c)] == roots[c]);
      }
    }
    for (BlockHandle h : all) {
      if (d.sc().contains(*d.lib, h)) {
        continue;
      }
      ObjectEval e;
      test_assert(d.sc().evaluated(h, e) || d.sc().children(h).size() == 0);
    }
  }
  test_assert(cycles > 20 && outsiders > 20);

  /* What isn't a matrix is a change too. */
  Doc d;
  const BlockHandle a = d.object("a"), b = d.object("b"), s = d.shapeBlock("s", 1.0);
  d.evaluate();
  const uint64_t v = d.sc().lastChanges().version;
  d.obj(a).flags = ObjectFlags::Hidden;
  d.evaluate();
  test_assert(d.sc().lastChanges().version == v + 1 &&
              d.sc().lastChanges().places.size() == 1);
  d.setParent(b, a);
  d.evaluate();
  test_assert(d.sc().lastChanges().version == v + 2 && d.sc().children(a).size() == 1);
  d.setData(b, s);
  d.evaluate();
  test_assert(d.sc().lastChanges().version == v + 3 && d.result(b).data == s);
  const BlockHandle twin = d.shapeBlock("twin", 1.0); /* with the same bounds */
  d.setData(b, twin);
  d.evaluate();
  test_assert(d.sc().lastChanges().version == v + 4 && d.result(b).data == twin);
  d.setData(b, s);
  d.evaluate();
  d.evaluate();
  test_assert(d.sc().lastChanges().version == v + 5);
  d.setParent(b, BlockHandle());
  test_assert(d.sc().children(a).size() == 1); /* the last evaluation's */
  d.evaluate();
  test_assert(d.sc().children(a).size() == 0 &&
              d.sc().children(BlockHandle()).size() == 2);
}

/* ---- what an evaluation did ---- */

void testResult()
{
  Doc d;
  const BlockHandle a = d.hostObject("a", 1);
  EvaluateResult r;
  must(evaluateScene(*d.ctx, &r));
  test_assert(r.done && r.changed);
  must(evaluateScene(*d.ctx, &r));
  test_assert(r.done && !r.changed);
  d.host(a).onRead = [&](ObjectInfo &) { d.rt.setStickyError(boom()); };
  test_assert(!evaluateScene(*d.ctx, &r).ok() && r.done && r.changed);
  test_assert(!evaluateScene(*d.ctx, &r).ok() && r.done && !r.changed);
  d.host(a).onRead = nullptr;
  d.rt.setStickyError(boom());
  test_assert(!evaluateScene(*d.ctx, &r).ok() && !r.done && !r.changed);
  (void)d.rt.takeStickyError();
  test_assert(!evaluateScene(*d.ctx, BlockHandle(), &r).ok() && !r.done);
  int flip = 0;
  d.host(a).onRead = [&](ObjectInfo &info) {
    info.parent = flip++ & 1 ? BlockId() : d.idOf(a);
  };
  test_assert(evaluateScene(*d.ctx, &r).code() == StatusCode::FailedPrecondition &&
              !r.done);
}

/* ---- lists by path ---- */

void testLists()
{
  CommandDoc d;
  Vector<BlockHandle> all;
  for (int i = 0; i < 6; i++) {
    all.append(d.object(("o" + std::to_string(i)).c_str()));
  }
  const auto length = [&](const char *path) {
    Result<PropValue> v = getValue(*d.ctx, path);
    return v.ok() ? int(v->asInt()) : -1;
  };
  const auto item = [&](const std::string &path) {
    Result<PropValue> v = getValue(*d.ctx, path);
    return v.ok() ? d.lib->find(v->tryPointer()->owner.block) : BlockHandle();
  };
  test_assert(length("evaluated.objects.length") == 0);
  d.evaluate();
  test_assert(length("evaluated.objects.length") == 6 &&
              length("scene.selectedObjects.length") == 0);
  d.select({all[3], all[1]});
  test_assert(length("scene.selectedObjects.length") == 2);
  test_assert(item("scene.selectedObjects[0]") == all[3] &&
              item("scene.selectedObjects[1]") == all[1]);
  must(d.sc().remove(*d.lib, d.idOf(all[3])).status()); /* not a member any more */
  test_assert(length("scene.selectedObjects.length") == 1 &&
              item("scene.selectedObjects[0]") == all[1]);
  must(d.sc().add(*d.lib, all[3]));
  test_assert(length("scene.selectedObjects.length") == 2);
  must(d.lib->markDeleted(all[1]));
  test_assert(length("scene.selectedObjects.length") == 1);
  must(d.lib->restoreFromHistory(all[1]));
  test_assert(length("scene.selectedObjects.length") == 2);
  /* The results' list is the last evaluation's, without those that are gone. */
  test_assert(length("evaluated.objects.length") == 6);
  d.object("new");
  test_assert(length("evaluated.objects.length") == 6);
  d.evaluate();
  test_assert(length("evaluated.objects.length") == 7);
  const BlockHandle other = d.addScene("Other");
  d.ctx->setScene(other);
  test_assert(length("evaluated.objects.length") == 0);
  d.ctx->setScene(d.scene);
  test_assert(length("evaluated.objects.length") == 7);
  d.ctx->setScene(BlockHandle());
  test_assert(length("evaluated.objects.length") == 0);
  d.ctx->setScene(d.scene);
  test_assert(length("evaluated.objects.length") == 7);
  must(d.sc().remove(*d.lib, d.idOf(d.sc().objects(*d.lib)[6])).status());
  test_assert(length("evaluated.objects.length") == 7); /* it still has a record */
  must(d.lib->collect().status());
  test_assert(length("evaluated.objects.length") == 6);
}

/* ---- bounds ---- */

/* A shape whose type doesn't implement Invalidation. */
struct Plain {
  Box3d box;
  int asked = 0;

  Box3d localBounds() const
  {
    const_cast<Plain *>(this)->asked++;
    return box;
  }
  uint32_t domains() const
  {
    return 1;
  }
  void closestElements(const NearQuery &, NearSink &) const
  {
  }
  static lbt::Struct<Plain> *defineBindings()
  {
    auto *st = new lbt::Struct<Plain>("test::Plain", sizeof(Plain));
    BIND_STRUCT_METHOD(st, localBounds, MARGS());
    BIND_STRUCT_METHOD(st, domains, MARGS());
    BIND_STRUCT_METHOD(st, closestElements, MARGS("query", "sink"));
    return st;
  }
};

/* How far it goes along x: a little more, as boxes in the world do. */
bool reaches(Doc &d, BlockHandle h, double x)
{
  return std::fabs(d.result(h).bounds.max[0] - x) < 1e-9;
}

void testKeptBounds()
{
  Doc d;
  d.addSlotType("test.Plain");
  must(registerImpl<Spatial, Plain>(d.rt, "test.Plain"));
  ApiStruct st("test.Plain");
  st.parent(StructId::of(kObjectDataStruct));
  must(d.rt.ext<ApiRegistry>().add(std::move(st)).status());

  const BlockHandle a = d.object("a"), b = d.object("b", 10), c = d.object("c");
  const BlockHandle s = d.shapeBlock("s", 1.0), t = d.shapeBlock("t", 2.0),
                    p = d.addSlot<Plain>("test.Plain", "p");
  Plain &plain = *static_cast<Plain *>(d.slot(p).data);
  for (int i = 0; i < 3; i++) {
    plain.box.min[i] = -3.0;
    plain.box.max[i] = 3.0;
  }
  d.setData(a, s);
  d.setData(b, s);
  d.setData(c, p);
  int asked = 0;
  d.shapeRaw(s).onBounds = [&]() { asked++; };
  d.evaluate();
  test_assert(asked == 1 && plain.asked == 1); /* once for both */
  test_assert(reaches(d, a, 1.0) && reaches(d, b, 11.0));
  d.obj(b).channels.set(0, 20.0);
  d.evaluate();
  test_assert(asked == 1 && plain.asked == 2 && reaches(d, b, 21.0));
  plain.box.max[0] = 4.0; /* its type doesn't say when it changes, so it is asked */
  d.evaluate();
  test_assert(reaches(d, c, 4.0) && asked == 1);

  const auto resize = [&](double size) { d.shapeRaw(s).box.max[0] = size; };
  resize(5.0);
  d.evaluate();
  test_assert(asked == 1 && reaches(d, a, 1.0)); /* nobody said so */
  must(invalidateData(*d.ctx, s, InvalidateKinds::Selection));
  d.evaluate();
  test_assert(asked == 1);
  must(invalidateData(*d.ctx, t, InvalidateKinds::Positions));
  d.evaluate();
  test_assert(asked == 1);
  must(invalidateData(*d.ctx, s, InvalidateKinds::Positions));
  d.evaluate();
  test_assert(asked == 2 && reaches(d, a, 5.0) && reaches(d, b, 25.0));
  resize(6.0);
  must(invalidateData(*d.ctx, s, InvalidateKinds::Topology));
  d.evaluate();
  test_assert(asked == 3 && reaches(d, a, 6.0));
  ChangeBus &bus = d.rt.ext<ChangeBus>();
  resize(7.0);
  bus.notify(OwnerKey::ofBlock(d.idOf(s)), "x");
  d.evaluate();
  test_assert(asked == 4 && reaches(d, a, 7.0));
  resize(8.0);
  bus.notifyAll();
  d.evaluate();
  test_assert(asked == 5 && reaches(d, a, 8.0));
  d.loose("epoch");
  d.evaluate();
  test_assert(asked == 6);

  /* An exception: asked again, also when nothing is said. */
  d.shapeRaw(s).onBounds = [&]() {
    asked++;
    d.rt.setStickyError(boom("bounds"));
  };
  bus.notifyAll();
  test_assert(isError(evaluateScene(*d.ctx), StatusCode::Internal, "bounds"));
  test_assert(d.result(a).bounds.empty());
  d.shapeRaw(s).onBounds = [&]() { asked++; };
  const int before = asked;
  d.evaluate();
  test_assert(asked == before + 1 && reaches(d, a, 8.0));

  /* A change while it is asked isn't lost. */
  d.shapeRaw(s).onBounds = [&]() {
    if (asked++ == before + 1) {
      bus.notify(OwnerKey::ofBlock(d.idOf(s)));
    }
  };
  bus.notifyAll();
  d.evaluate(); /* for `a`, and again for `b` */
  test_assert(asked == before + 3);
  d.evaluate();
  test_assert(asked == before + 3);

  /* Two scenes keep their own. */
  const BlockHandle other = d.addScene("Other");
  must(Scene::of(*d.lib, other)->add(*d.lib, a));
  must(evaluateScene(*d.ctx, other));
  d.evaluate();
  const int both = asked;
  must(invalidateData(*d.ctx, s, InvalidateKinds::Positions));
  must(evaluateScene(*d.ctx, other));
  d.evaluate();
  test_assert(asked == both + 2);
  d.evaluate();
  must(evaluateScene(*d.ctx, other));
  test_assert(asked == both + 2);

  /* What stands in for a type's Spatial isn't kept. */
  Plain over;
  for (int i = 0; i < 3; i++) {
    over.box.min[i] = -0.5;
    over.box.max[i] = 0.5;
  }
  must(registerImpl<Spatial, Plain>(d.rt, "test.Over"));
  must(d.lib->setOverride(s,
                          InterfaceId::of(Spatial::ifaceName),
                          query<Spatial>(d.rt, TypeId::of("test.Over"), &over)));
  d.evaluate();
  test_assert(reaches(d, a, 0.5));
  {
    over.box.max[0] = 0.25;
    d.evaluate();
    test_assert(reaches(d, a, 0.25));
  }
}

/* ---- many at a time ---- */

/* World = parent * translation(x, y, time), and a turn by z about z; the local scale is
 * (1, 2, z). */
struct Many {
  Runtime *rt = nullptr;
  int single = 0, batches = 0, objects = 0;
  int mode = 0;
  double factor = 1.0;

  static TransformState
  one(double x, double y, double z, double time, const TransformState &p)
  {
    TransformState s;
    mat4d local;
    local.identity();
    double *m = local;
    m[0] = m[5] = std::cos(z);
    m[1] = std::sin(z);
    m[4] = -m[1];
    m[12] = x;
    m[13] = y;
    m[14] = time;
    s.world = p.world * local;
    s.rotation = p.rotation;
    s.localScale = double3(1, 2, z);
    return s;
  }
  TransformState evaluate(const EvalCtx &c, const TransformState &p) const
  {
    const_cast<Many *>(this)->single++;
    return one(factor * c.channel(0), c.channel(1), c.channel(2), c.time(), p);
  }
  Status setWorld(const EvalCtx &,
                  const mat4d &,
                  const TransformState &,
                  SetWorldMode,
                  ChannelBlock &) const
  {
    return Status::error(StatusCode::Unsupported, "Many");
  }
  Status evaluateMany(TransformBatch &b) const
  {
    Many &self = *const_cast<Many *>(this);
    self.batches++;
    self.objects += b.count();
    if (mode == 1) {
      return Status::error(StatusCode::Internal, "refused");
    }
    if (mode == 2) {
      rt->setStickyError(boom("thrown"));
      return Status();
    }
    if (mode == 3) {
      return Status(); /* no states */
    }
    const Vector<double> c = b.channels(), p = b.parents();
    test_assert(b.channelCount() == 3 && int(c.size()) == 3 * b.count() &&
                int(p.size()) == 23 * b.count());
    Vector<double> out;
    for (int i = 0; i < b.count(); i++) {
      TransformState parent;
      double *m = parent.world;
      for (int k = 0; k < 16; k++) {
        m[k] = p[i * 23 + k];
      }
      for (int k = 0; k < 4; k++) {
        parent.rotation[k] = p[i * 23 + 16 + k];
      }
      for (int k = 0; k < 3; k++) {
        parent.localScale[k] = p[i * 23 + 20 + k];
      }
      test_assert(sameBits(parent, b.parent(i)));
      const TransformState s =
          one(factor * c[i * 3], c[i * 3 + 1], c[i * 3 + 2], b.time(), parent);
      const double *w = s.world;
      for (int k = 0; k < 16; k++) {
        out.append(w[k]);
      }
      for (int k = 0; k < 4; k++) {
        out.append(s.rotation[k]);
      }
      for (int k = 0; k < 3; k++) {
        out.append(s.localScale[k]);
      }
    }
    Vector<double> less = out;
    less.pop_back();
    test_assert(b.setStates(less).code() == StatusCode::InvalidArgument);
    return b.setStates(out);
  }
  static lbt::Struct<Many> *defineBindings()
  {
    auto *st = new lbt::Struct<Many>("test::Many", sizeof(Many));
    BIND_STRUCT_METHOD(st, evaluate, MARGS("ctx", "parent"));
    BIND_STRUCT_METHOD(st, setWorld, MARGS("current", "target", "parent", "mode", "out"));
    BIND_STRUCT_METHOD(st, evaluateMany, MARGS("batch"));
    return st;
  }
};

/* The same without evaluateMany: the oracle. */
struct Single {
  double factor = 1.0;

  TransformState evaluate(const EvalCtx &c, const TransformState &p) const
  {
    return Many::one(factor * c.channel(0), c.channel(1), c.channel(2), c.time(), p);
  }
  Status setWorld(const EvalCtx &,
                  const mat4d &,
                  const TransformState &,
                  SetWorldMode,
                  ChannelBlock &) const
  {
    return Status();
  }
  static lbt::Struct<Single> *defineBindings()
  {
    auto *st = new lbt::Struct<Single>("test::Single", sizeof(Single));
    BIND_STRUCT_METHOD(st, evaluate, MARGS("ctx", "parent"));
    BIND_STRUCT_METHOD(st, setWorld, MARGS("current", "target", "parent", "mode", "out"));
    return st;
  }
};

template <class M> M *addModel(Doc &d, const char *name)
{
  const std::string impl = std::string(name) + "#impl";
  must(registerImpl<TransformModel, M>(d.rt, impl));
  M *m = litestl::alloc::New<M>("Model");
  Result<Owned<TransformModel>> owned = own<TransformModel>(d.rt, impl, m);
  must(owned.status());
  TransformTypeInfo info;
  info.name = name;
  for (const char *channel : {"x", "y", "z"}) {
    info.channels.append(PropertyDef::floatProp(channel));
  }
  must(d.rt.ext<TransformRegistry>()
           .add(std::move(info), std::move(owned).value())
           .status());
  return m;
}

/* Drives a channel of some, and fails for others. */
struct Driver : OverrideProvider {
  const Vector<BlockHandle> *list = nullptr;
  int step = 0;

  Status overridesOf(const Library &,
                     BlockHandle object,
                     const TransformType &,
                     double time,
                     ChannelOverrides &out) override
  {
    for (int i = 0; i < int(list->size()); i++) {
      if ((*list)[i] == object && (i + step) % 4 == 1) {
        out.set(1, time + i);
      }
      if ((*list)[i] == object && step == 3 && i % 9 == 4) {
        return Status::error(StatusCode::Internal, "driver");
      }
    }
    return Status();
  }
};

/* Random hierarchies with both types among others, with cycles, hosts that move parents,
 * and bad channels: the same results as without batches. */
void testBatches()
{
  int batched = 0, batches = 0, cycles = 0, others_ = 0;
  for (int k = 0; k < 30 * coredcc_test::testScale(); k++) {
    Doc d, o;
    Many *many = addModel<Many>(d, "test.m");
    addModel<Single>(o, "test.m");
    Many *other = addModel<Many>(d, "test.n");
    other->factor = addModel<Single>(o, "test.n")->factor = 2.0;
    d.addFnModel("test.last", [](const EvalCtx &, const TransformState &p) { return p; });
    many->rt = &d.rt;
    Vector<BlockHandle> all, others;
    Driver driver, otherDriver;
    driver.list = &all;
    otherDriver.list = &others;
    const bool driven = k % 2 == 0;
    if (driven) {
      must(setOverrideProvider(*d.lib, driver));
      must(setOverrideProvider(*o.lib, otherDriver));
    }
    const auto each = [&](auto &&fn) {
      fn(d, all);
      fn(o, others);
    };
    Rand r(k);
    const uint64_t seed = r.next();
    each([&](Doc &doc, Vector<BlockHandle> &list) {
      Rand s(seed);
      Types types;
      for (int i = 0; i < 25; i++) {
        const std::string name = "o" + std::to_string(i);
        if (s.below(5) == 0) {
          const BlockHandle h = doc.hostObject(name.c_str(), s.range(-2, 2));
          if (s.coin()) {
            doc.host(h).info.transformType = TypeId::of("test.m");
            doc.host(h).channels = ChannelBlock(3);
            doc.host(h).channels.set(1, s.range(-2, 2));
          }
          list.append(h);
          continue;
        }
        const BlockHandle h = doc.object(name.c_str(), s.range(-2, 2));
        if (s.below(3)) {
          doc.obj(h).setTransformType("test.m");
          doc.obj(h).channels = ChannelBlock(s.below(12) ? 3 : 2);
          for (int c = 0; c < doc.obj(h).channels.size(); c++) {
            doc.obj(h).channels.set(
                c,
                s.below(20) ? s.range(-2, 2) : std::numeric_limits<double>::infinity());
          }
        }
        list.append(h);
      }
    });
    for (int step = 0; step < 5; step++) {
      for (int i = 0; i < 6; i++) {
        const int a = int(r.below(25)), b = int(r.below(26));
        const double x = r.range(-2, 2);
        const bool change = r.below(4) == 0;
        each([&](Doc &doc, Vector<BlockHandle> &list) {
          const BlockHandle p = b < 25 ? list[b] : BlockHandle();
          if (doc.lib->get(list[a])->type() != TypeId::of(kObjectType)) {
            doc.host(list[a]).info.parent = p ? doc.idOf(p) : BlockId();
            return;
          }
          if (step % 2) { /* on the others, nothing is looked up again */
            doc.setParent(list[a], p);
          }
          if (change) { /* of another type, behind the back of what was looked up */
            const bool m = doc.obj(list[a]).transformTypeId() == TypeId::of("test.m");
            doc.obj(list[a]).setTransformType(!m      ? "test.m"
                                              : x > 0 ? "test.n"
                                                      : kTrsTransform);
            doc.obj(list[a]).channels =
                m && x <= 0 ? doc.trs->defaults() : ChannelBlock(3);
          }
          doc.obj(list[a]).channels.set(0, x);
          doc.obj(list[a]).flags = change ? ObjectFlags::Hidden : ObjectFlags::None;
        });
      }
      const double frame = std::floor(r.range(0, 50));
      driver.step = otherDriver.step = step;
      d.sc().frame = o.sc().frame = frame;
      const Status got = evaluateScene(*d.ctx), want = evaluateScene(*o.ctx);
      test_assert(got.code() == want.code() && got.ok() == (!driven || step != 3));
      for (int i = 0; i < 25; i++) {
        const ObjectEval e = d.result(all[i]), f = o.result(others[i]);
        test_assert(e.status == f.status && sameBits(e.state, f.state) &&
                    e.parent == f.parent && e.flags == f.flags);
        cycles += e.status == EvalStatus::Cycle ? 1 : 0;
      }
    }
    clearOverrideProvider(*d.lib, driver);
    clearOverrideProvider(*o.lib, otherDriver);
    batched += many->objects;
    others_ += other->objects;
    batches += many->batches;
  }
  printf("batches: %d with %d objects, %d on cycles\n", batches, batched, cycles);
  test_assert(batched > batches * 2 && cycles > 20 && others_ > 50);

  /* Errors: the objects of that batch are where their parents are. */
  Doc d;
  Many *many = addModel<Many>(d, "test.m");
  many->rt = &d.rt;
  const BlockHandle top = d.object("top", 7), a = d.object("a"), b = d.object("b"),
                    low = d.object("low", 1), t = d.object("t", 3);
  for (BlockHandle h : {a, b}) {
    d.obj(h).setTransformType("test.m");
    d.obj(h).channels = ChannelBlock(3);
    d.obj(h).channels.set(0, 5.0);
    d.setParent(h, top);
  }
  d.setParent(low, a);
  d.evaluate();
  test_assert(many->batches == 1 && many->single == 0 && many->objects == 2);
  test_assert(d.isAt(a, 12) && d.isAt(low, 13) && d.isAt(t, 3));
  for (int mode = 1; mode <= 3; mode++) {
    many->mode = mode;
    const char *what = mode == 1 ? "refused" : mode == 2 ? "thrown" : "no states";
    test_assert(isError(evaluateScene(*d.ctx), StatusCode::Internal, what));
    test_assert(!d.rt.hasStickyError());
    test_assert(d.statusOf(a) == EvalStatus::ModelFailed && d.isAt(a, 7) &&
                d.isAt(b, 7) && d.isAt(low, 8) && d.isAt(t, 3) &&
                d.statusOf(low) == EvalStatus::Ok);
  }
  many->mode = 0;
  d.evaluate();
  test_assert(d.isAt(b, 12));
  /* Of another type, which nobody said: by itself this time, and with the others from
   * then on. */
  d.obj(t).setTransformType("test.m");
  d.obj(t).channels = ChannelBlock(3);
  int together = many->objects;
  d.evaluate();
  test_assert(many->objects == together + 2 && d.isAt(t, 0));
  together = many->objects;
  d.evaluate();
  test_assert(many->objects == together + 3 && d.isAt(t, 0));
  /* Everything else asks for one. */
  const int before = many->single;
  test_assert(computeState(*d.lib, low, 0.0).ok() && many->single == before + 1);
}

} // namespace

int main()
{
  testHierarchy();
  testResult();
  testLists();
  testKeptBounds();
  testBatches();
  return test_end();
}
