/* A Spatial that a script implements, through a fake foreign host, end to end: what
 * pickScene, pickRegion and nearestInScene answer is the same as for a native one with
 * the same triangles. The script makes the calls that scripts can make: numbers in
 * vectors, and AnyBvh. */
#include "../scene/pick_doc.h"
#include "fake_host.h"

#include "coredcc/bind/foreign_command.h"
#include "coredcc/bvh/any_source.h"

#include <new>

using namespace scene_test;
namespace sc = coredcc::script;
using litestl::util::string;

test_init;

namespace {

/* The "JS object". */
struct Mesh {
  std::vector<double> corners; /* nine for each triangle */
  AnyBvh bvh;
  bool throwPick = false, throwNear = false, throwRegion = false, badSizes = false;
  int picks = 0, given = 0;
  Status lastBulk;

  int size() const
  {
    return int(corners.size() / 9);
  }
  void add(const Prim &p)
  {
    for (const double3 &q : p.q) {
      for (int i = 0; i < 3; i++) {
        corners.push_back(q[i]);
      }
    }
  }
  void build()
  {
    Vector<double> bounds;
    for (int t = 0; t < size(); t++) {
      const double *c = corners.data() + t * 9;
      for (int i = 0; i < 3; i++) {
        bounds.append(std::fmin(c[i], std::fmin(c[i + 3], c[i + 6])));
      }
      for (int i = 0; i < 3; i++) {
        bounds.append(std::fmax(c[i], std::fmax(c[i + 3], c[i + 6])));
      }
    }
    must(bvh.buildFrom(bounds));
  }

  fakehost::Obj make()
  {
    fakehost::Obj o;
    o.methods["localBounds"] = [this](void **, void *ret, string *) {
      Box3d *box = new (ret) Box3d();
      for (size_t i = 0; i < corners.size(); i += 3) {
        box->include(corners[i], corners[i + 1], corners[i + 2]);
      }
      return true;
    };
    o.methods["domains"] = [](void **, void *ret, string *) {
      *static_cast<uint32_t *>(ret) = 1u << int(ElementDomain::Face);
      return true;
    };
    o.methods["pick"] = [this](void **args, void *, string *err) {
      const PickQuery &query = *static_cast<const PickQuery *>(args[0]);
      PickSink &sink = *static_cast<PickSink *>(args[1]);
      picks++;
      if (throwPick) {
        *err = "pick: Error: no triangles";
        return false;
      }
      const Vector<int32_t> near = bvh.region(query.planeValues());
      Vector<double> values;
      for (int32_t t : near) {
        for (int i = 0; i < (badSizes ? 8 : 9); i++) {
          values.append(corners[size_t(t) * 9 + size_t(i)]);
        }
      }
      given += int(near.size());
      lastBulk = sink.triangles(ElementDomain::Face, near, values);
      return true;
    };
    o.methods["closestElements"] = [this](void **args, void *, string *err) {
      const NearQuery &query = *static_cast<const NearQuery *>(args[0]);
      NearSink &sink = *static_cast<NearSink *>(args[1]);
      if (throwNear) {
        *err = "closestElements: Error: nothing";
        return false;
      }
      const Vector<int32_t> near = bvh.sphereOverlap(query.point(), query.radius());
      Vector<double> positions;
      for (int32_t t : near) {
        const double *c = corners.data() + size_t(t) * 9;
        const double3 at = bvh::closestOnTriangle(query.point(),
                                                  double3(c[0], c[1], c[2]),
                                                  double3(c[3], c[4], c[5]),
                                                  double3(c[6], c[7], c[8]));
        for (int i = 0; i < 3; i++) {
          positions.append(at[i]);
        }
      }
      lastBulk = sink.addAll(near, positions);
      return true;
    };
    o.methods["elementsInRegion"] = [this](void **args, void *ret, string *err) {
      const RegionQuery &query = *static_cast<const RegionQuery *>(args[0]);
      if (throwRegion) {
        *err = "elementsInRegion: Error: none";
        return false;
      }
      new (ret) Vector<int32_t>(bvh.region(query.planeValues()));
      return true;
    };
    return o;
  }
};

const char *const kSlots[] = {
    "localBounds", "domains", "closestElements", "pick", "elementsInRegion"};

struct SpatialDoc : PickDoc {
  fakehost::Host &host_;

  explicit SpatialDoc(fakehost::Host &host) : host_(host)
  {
    addSlotType("js.Mesh");
    sc::ForeignBindings &fb = rt.ext<sc::ForeignBindings>();
    must(registerForeign<Spatial>(rt, "js.Mesh", fb.dispatcher(), nullptr, kSlots));
  }
  BlockHandle meshBlock(Mesh &mesh)
  {
    Slot *s = litestl::alloc::New<Slot>("Slot");
    s->data = reinterpret_cast<void *>(uintptr_t(host_.add(mesh.make())));
    s->free = [](void *p) { fakehost::lf::release(fakehost::lf::Handle(uintptr_t(p))); };
    Result<BlockHandle> h = lib->create(TypeId::of("js.Mesh"), "mesh", s);
    must(h.status());
    return *h;
  }
};

void testSame()
{
  fakehost::Host host;
  {
    Rand r(71);
    Mesh meshes[12];
    std::vector<std::vector<Prim>> prims;
    for (Mesh &m : meshes) {
      prims.emplace_back();
      for (int i = 0; i < 40; i++) {
        const double3 c = r.vec(0.8);
        prims.back().push_back(face(c + r.vec(0.2), c + r.vec(0.2), c + r.vec(0.2)));
        m.add(prims.back().back());
      }
      m.build();
    }
    SpatialDoc foreign(host);
    PickDoc native;
    for (int i = 0; i < 12; i++) {
      const mat4d world = solve::affine(Linear::identity(), r.vec(1.5));
      foreign.placed("mesh", world, foreign.meshBlock(meshes[i]));
      const BlockHandle data = native.geoBlock("geo", prims[size_t(i)]);
      native.geo(data).useBvh = true;
      native.placed("geo", world, data);
    }
    foreign.evaluate();
    native.evaluate();
    int hits = 0, near = 0, elements = 0;
    for (int k = 0; k < 200; k++) {
      ViewCase c = randomView(r);
      c.eye = r.vec(4.0);
      c.target = r.vec(0.5);
      const ViewMath view = viewOf(c);
      const double x = r.range(700, 1300), y = r.range(300, 800);
      PickOptions o;
      o.radius = r.range(0.0, 20.0);
      o.maxHits = int32_t(r.below(2));
      const Vector<PickHit> a = foreign.picks(view, x, y, o),
                            b = native.picks(view, x, y, o);
      test_assert(a.size() == b.size());
      for (int i = 0; i < int((std::min)(a.size(), b.size())); i++) {
        test_assert(a[i].element == b[i].element && a[i].domain == b[i].domain &&
                    a[i].distance == b[i].distance &&
                    a[i].viewDistance == b[i].viewDistance);
        hits++;
      }
      NearestOptions n;
      n.domain = ElementDomain::Face;
      n.maxCount = 5;
      Vector<NearHit> na, nb;
      const double3 p = r.vec(2.0);
      must(nearestInScene(*foreign.ctx, foreign.scene, p, 0.7, n, {}, na));
      must(nearestInScene(*native.ctx, native.scene, p, 0.7, n, {}, nb));
      test_assert(na.size() == nb.size());
      for (int i = 0; i < int((std::min)(na.size(), nb.size())); i++) {
        test_assert(na[i].element == nb[i].element && na[i].distance == nb[i].distance);
        near++;
      }
      /* The script's tree answers with boxes: never fewer than the native one's
       * triangles. */
      const ViewRegion region = view.regionFromRect(x, y, x + 200.0, y + 100.0);
      Vector<RegionHit> ra, rb;
      must(pickRegion(*foreign.ctx, foreign.scene, region, ElementDomain::Face, {}, ra));
      must(pickRegion(*native.ctx, native.scene, region, ElementDomain::Face, {}, rb));
      size_t found[2] = {};
      for (const RegionHit &h : ra) {
        found[0] += h.elements.size();
      }
      for (const RegionHit &h : rb) {
        found[1] += h.elements.size();
      }
      test_assert(found[0] >= found[1] && found[0] <= found[1] * 3 + 20);
      elements += int(found[1]);
    }
    test_assert(hits > 100 && near > 100 && elements > 300);
  }
  test_assert(host.live() == 0 && host.allReleasedOnce());
}

void testErrors()
{
  fakehost::Host host;
  {
    Mesh bad, good;
    bad.add(face(double3(-1, -1, 0), double3(1, -1, 0), double3(0, 1, 0)));
    good.add(face(double3(-1, -1, 1), double3(1, -1, 1), double3(0, 1, 1)));
    bad.build();
    good.build();
    SpatialDoc d(host);
    const BlockHandle a = d.placed("bad", identity(), d.meshBlock(bad));
    const BlockHandle b = d.placed("good", identity(), d.meshBlock(good));
    d.evaluate();
    const ViewMath view = viewOf(ViewCase());
    const ViewPoint p = view.project(double3());
    test_assert(d.picks(view, p.x, p.y, PickOptions()).size() == 2);
    /* What it throws is returned, with the others' hits. */
    Vector<PickHit> hits;
    bad.throwPick = true;
    test_assert(isError(d.tryPick(view, p.x, p.y, PickOptions(), hits),
                        StatusCode::Internal,
                        "no triangles"));
    test_assert(hits.size() == 1 && hits[0].object == b && !d.rt.hasStickyError());
    bad.throwPick = false;
    bad.throwNear = bad.throwRegion = true;
    NearestOptions n;
    n.domain = ElementDomain::Face;
    Vector<NearHit> near;
    test_assert(isError(nearestInScene(*d.ctx, d.scene, double3(), 5.0, n, {}, near),
                        StatusCode::Internal,
                        "nothing"));
    test_assert(near.size() == 1 && near[0].object == b);
    Vector<RegionHit> region;
    test_assert(isError(pickRegion(*d.ctx,
                                   d.scene,
                                   view.regionFromRect(0, 0, 2000, 1100),
                                   ElementDomain::None,
                                   {},
                                   region),
                        StatusCode::Internal,
                        "none"));
    test_assert(region.size() == 1 && region[0].object == b);
    /* Vectors of the wrong length: nothing is taken, and the script is told. */
    bad.throwNear = bad.throwRegion = false;
    bad.badSizes = true;
    hits = d.picks(view, p.x, p.y, PickOptions());
    test_assert(hits.size() == 1 && hits[0].object == b);
    test_assert(bad.lastBulk.code() == StatusCode::InvalidArgument && good.lastBulk.ok());
    /* Only what may come first is asked: the good one is in front. */
    bad.badSizes = false;
    bad.picks = 0;
    PickOptions first;
    first.maxHits = 1;
    hits = d.picks(view, p.x, p.y, first);
    test_assert(hits.size() == 1 && hits[0].object == b && bad.picks == 0);
    (void)a;
  }
  test_assert(host.live() == 0 && host.allReleasedOnce());
}

} // namespace

int main()
{
  testSame();
  testErrors();
  return test_end();
}
