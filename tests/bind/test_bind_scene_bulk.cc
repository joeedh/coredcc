/* Scenes for scripts, through a fake foreign host: results as flat arrays, and what
 * transform types can be beyond what test_bind_scene.cc has. */
#include "../scene/io_doc.h"
#include "fake_host.h"

#include "coredcc/api/ref_text.h"
#include "coredcc/bind/facades.h"
#include "coredcc/bind/foreign_command.h"
#include "coredcc/bind/foreign_transform.h"
#include "coredcc/bind/manager.h"
#include "coredcc/bind/scene_facades.h"
#include "coredcc/scene/channel_entries.h"
#include "coredcc/scene/solve.h"

#include <limits>
#include <new>

using namespace scene_test;
namespace sc = coredcc::script;
using fakehost::Obj;
using litestl::util::string;

test_init;

namespace {

std::string textOf(CommandDoc &d, BlockHandle h)
{
  const sc::StringResult r = sc::pointerText(*d.ctx, d.ptr(h));
  test_assert(r.status.ok());
  return r.value.c_str();
}

string str(const std::string &s)
{
  return string(s.c_str());
}

/* ---- results ---- */

void testResults()
{
  CommandDoc d;
  Context &ctx = *d.ctx;
  sc::SceneTableResult t = sc::sceneResults(ctx, {}, true);
  test_assert(t.status.ok() && t.value.placeCount() == 0 && t.value.version() == 0.0);
  const BlockHandle a = d.object("a", 1), b = d.object("b", 2), out = d.loose("out", 3),
                    s = d.shapeBlock("s", 1.0);
  d.setParent(b, out);
  d.setData(a, s);
  d.obj(a).flags = ObjectFlags::Hidden;
  d.evaluate();
  t = sc::sceneResults(ctx, {}, true);
  const sc::RefListResult refs = sc::sceneObjects(ctx, {}, true);
  test_assert(t.status.ok() && t.value.placeCount() == 3 && refs.value.size() == 3);
  test_assert(t.value.allChanged() && t.value.version() == 1.0);
  const Vector<double> matrices = t.value.matrices(), bounds = t.value.bounds();
  test_assert(matrices.size() == 48 && bounds.size() == 18);
  for (int i = 0; i < 3; i++) {
    const BlockHandle h = d.sc().evaluatedObjects()[i];
    const ObjectEval e = d.result(h);
    const double *m = e.dataWorld();
    for (int k = 0; k < 16; k++) {
      test_assert(matrices[i * 16 + k] == m[k]);
    }
    for (int k = 0; k < 3; k++) {
      test_assert(bounds[i * 6 + k] == e.bounds.min[k] &&
                  bounds[i * 6 + 3 + k] == e.bounds.max[k]);
    }
    test_assert(t.value.status()[i] == 0 && t.value.parents()[i] == e.parent);
    test_assert(t.value.flags()[i] == (h == a ? 1 : 0) &&
                t.value.members()[i] == (h == out ? 0 : 1));
    test_assert(textOf(d, h) == refs.value.at(i).c_str());
  }
  test_assert(refs.value.at(-1).size() == 0 && refs.value.at(3).size() == 0);

  /* What changed, and only that. */
  d.obj(b).channels.set(0, 7.0);
  d.evaluate();
  t = sc::sceneResults(ctx, {}, false);
  test_assert(t.value.placeCount() == 3 && t.value.matrices().size() == 0 &&
              !t.value.allChanged());
  test_assert(t.value.version() == 2.0 && t.value.changed().size() == 1);
  t = sc::sceneResults(ctx, t.value.changed(), false);
  test_assert(t.value.matrices().size() == 16 && t.value.matrices()[12] == 10.0);
  test_assert(sc::sceneResults(ctx, {0, 3}, false).status.code() ==
              StatusCode::OutOfRange);
  test_assert(sc::sceneResults(ctx, {-1}, false).status.code() == StatusCode::OutOfRange);
  test_assert(sc::sceneObjects(ctx, {3}, false).status.code() == StatusCode::OutOfRange);
  test_assert(sc::sceneResults(ctx, {0}, true).status.code() ==
              StatusCode::InvalidArgument);

  sc::RefListResult kids = sc::sceneChildren(ctx, str(textOf(d, out)));
  test_assert(kids.status.ok() && kids.value.size() == 1 &&
              textOf(d, b) == kids.value.at(0).c_str());
  kids = sc::sceneChildren(ctx, "");
  test_assert(kids.value.size() == 2);
  test_assert(sc::sceneChildren(ctx, "nonsense").status.code() ==
              StatusCode::InvalidArgument);

  /* One that has no record any more. */
  must(d.sc().remove(*d.lib, d.idOf(a)).status());
  must(d.lib->collect().status());
  test_assert(sc::sceneObjects(ctx, {0}, false).value.at(0).size() == 0);

  ctx.setScene(BlockHandle());
  test_assert(sc::sceneResults(ctx, {}, true).status.code() ==
              StatusCode::FailedPrecondition);
  test_assert(sc::sceneObjects(ctx, {}, true).status.code() ==
              StatusCode::FailedPrecondition);
  test_assert(sc::sceneChildren(ctx, "").status.code() == StatusCode::FailedPrecondition);
  ctx.setScene(d.scene);
}

/* ---- channels ---- */

/* World = parent + (x, y, 0). */
struct Plane {
  int singles = 0, batches = 0, upgrades = 0;
  bool fail = false;

  static void put(TransformState &s, const TransformState &parent, double x, double y)
  {
    s.world = solve::affine(Linear::identity(),
                            solve::translationOf(parent.world) + double3(x, y, 0));
    s.rotation = parent.rotation;
  }
  Obj make()
  {
    Obj o;
    o.methods["evaluate"] = [this](void **args, void *ret, string *) {
      singles++;
      const EvalCtx &c = *static_cast<const EvalCtx *>(args[0]);
      put(*new (ret) TransformState(),
          *static_cast<const TransformState *>(args[1]),
          c.channel(0),
          c.channelCount() > 1 ? c.channel(1) : 0.0);
      return true;
    };
    o.methods["setWorld"] = [](void **, void *ret, string *) {
      new (ret) Status();
      return true;
    };
    o.methods["evaluateMany"] = [this](void **args, void *ret, string *err) {
      batches++;
      if (fail) {
        *err = "evaluateMany: Error: no states";
        return false;
      }
      TransformBatch &b = *static_cast<TransformBatch *>(args[0]);
      const Vector<double> c = b.channels();
      Vector<double> out;
      for (int i = 0; i < b.count(); i++) {
        TransformState s;
        put(s, b.parent(i), c[i * 2], c[i * 2 + 1]);
        const double *m = s.world;
        for (int k = 0; k < 16; k++) {
          out.append(m[k]);
        }
        for (int k = 0; k < 4; k++) {
          out.append(s.rotation[k]);
        }
        for (int k = 0; k < 3; k++) {
          out.append(s.localScale[k]);
        }
      }
      new (ret) Status(b.setStates(out));
      return true;
    };
    o.methods["pivot"] = [](void **args, void *ret, string *) {
      const TransformState &parent = *static_cast<const TransformState *>(args[1]);
      new (ret) double3(solve::translationOf(parent.world) + double3(0, 0, 9));
      return true;
    };
    o.methods["upgrade"] = [this](void **args, void *ret, string *err) {
      upgrades++;
      if (fail) {
        *err = "upgrade: Error: too old";
        return false;
      }
      ChannelEntries &e = *static_cast<ChannelEntries *>(args[1]);
      test_assert(*static_cast<const uint32_t *>(args[0]) == 0);
      test_assert(e.rename("wide", "x"));
      new (ret) Status();
      return true;
    };
    return o;
  }
};

void testChannels()
{
  fakehost::Host host;
  {
    Plane plane;
    Doc d;
    TransformTypeInfo *type = sc::newTransformType("js.full");
    must(sc::transformTypeVersion(*type, 3));
    test_assert(isError(
        sc::transformTypeVersion(*type, -1), StatusCode::InvalidArgument, "negative"));
    must(sc::transformChannel(*type, "q", PropValue::ofQuat(0, 0, 1, 0), false));
    test_assert(
        isError(sc::transformChannel(*type, "r", PropValue::ofQuat(0, 0, 0, 1), true),
                StatusCode::InvalidArgument,
                "distance"));
    must(sc::transformChannel(*type, "len", sc::makeFloat(2), true));
    must(sc::transformEnumChannel(*type, "mode"));
    must(sc::transformChannelItem(*type, "mode", "SLOW", 1, "Slow", "Less"));
    must(sc::transformChannelItem(*type, "mode", "FAST", -5, "Fast", ""));
    must(sc::transformChannelText(*type, "mode", "default", "FAST"));
    must(sc::transformChannelText(*type, "len", "uiName", "Length"));
    must(sc::transformChannelText(*type, "len", "description", "How long"));
    must(sc::transformChannelText(*type, "len", "icon", "ruler"));
    must(sc::transformChannelText(*type, "len", "subtype", "Distance"));
    must(sc::transformChannelText(*type, "len", "unit", "Length"));
    must(sc::transformChannelText(*type, "len", "displayUnit", "cm"));
    const double inf = std::numeric_limits<double>::infinity();
    must(sc::transformChannelNumbers(*type, "len", "range", 0, inf));
    must(sc::transformChannelNumbers(*type, "len", "softRange", 1, 4));
    must(sc::transformChannelNumbers(*type, "len", "step", 0.5, 0));
    must(sc::transformChannelNumbers(*type, "len", "precision", 3, 0));

    const StatusCode bad = StatusCode::InvalidArgument;
    const double nan = std::numeric_limits<double>::quiet_NaN();
    test_assert(
        isError(sc::transformChannelItem(*type, "len", "X", 1, "", ""), bad, "Enum"));
    test_assert(
        isError(sc::transformChannelItem(*type, "none", "X", 1, "", ""), bad, "Enum"));
    for (double v : {0.5, nan, 1e17}) {
      test_assert(
          isError(sc::transformChannelItem(*type, "mode", "X", v, "", ""), bad, "whole"));
    }
    test_assert(isError(
        sc::transformChannelText(*type, "none", "uiName", "x"), bad, "no such channel"));
    test_assert(
        isError(sc::transformChannelText(*type, "len", "bogus", "x"), bad, "bogus"));
    test_assert(isError(
        sc::transformChannelText(*type, "len", "subtype", "x"), bad, "subtype 'x'"));
    test_assert(isError(
        sc::transformChannelText(*type, "len", "unit", "x"), bad, "unit family 'x'"));
    test_assert(isError(
        sc::transformChannelText(*type, "q", "displayUnit", "cm"), bad, "needs a unit"));
    test_assert(isError(
        sc::transformChannelText(*type, "mode", "default", "x"), bad, "item 'x'"));
    test_assert(
        isError(sc::transformChannelText(*type, "len", "default", "x"), bad, "item 'x'"));
    test_assert(isError(
        sc::transformChannelNumbers(*type, "none", "step", 1, 0), bad, "no such"));
    test_assert(
        isError(sc::transformChannelNumbers(*type, "len", "bogus", 1, 0), bad, "bogus"));
    for (double v : {-1.0, nan, 1e9}) {
      test_assert(isError(
          sc::transformChannelNumbers(*type, "len", "precision", v, 0), bad, "digits"));
    }
    must(sc::registerForeignTransformType(
        *d.ctx, *type, host.add(plane.make()), "evaluate setWorld upgrade"));
    const TransformType *t = d.rt.ext<TransformRegistry>().find("js.full");
    test_assert(t && t->channelCount() == 6 && t->version() == 3);
    const double want[] = {0, 0, 1, 0, 2, -5};
    for (int i = 0; t && i < 6; i++) {
      test_assert(t->defaults().get(i) == want[i] && t->isDistance(i) == (i == 4));
    }
    const ApiStruct *st = d.rt.ext<ApiRegistry>().find(StructId::of("js.full"));
    const ApiMember *len = st ? st->findOwn("len") : nullptr;
    const ApiMember *mode = st ? st->findOwn("mode") : nullptr;
    test_assert(len && mode);
    if (len && mode) {
      test_assert(!strcmp(len->def().uiName().c_str(), "Length") &&
                  !strcmp(len->def().description().c_str(), "How long") &&
                  !strcmp(len->def().icon().c_str(), "ruler") &&
                  len->def().subtype() == PropSubtype::Distance);
      const NumericMeta &n = *len->def().numeric();
      test_assert(n.hardMin == 0 && n.hardMax == inf && n.softMin == 1 && n.softMax == 4);
      test_assert(n.step == 0.5 && n.precision == 3 && n.unit == UnitFamily::Length &&
                  !strcmp(n.displayUnit.c_str(), "cm"));
      const EnumMeta &e = *mode->def().enumMeta();
      test_assert(e.items.size() == 2 && !strcmp(e.items[0].uiName.c_str(), "Slow") &&
                  !strcmp(e.items[0].description.c_str(), "Less") &&
                  e.items[1].value == -5);
    }
    litestl::alloc::Delete(type);

    /* What a def may not be is found at the end. */
    const auto refused = [&](auto &&fill) {
      TransformTypeInfo *info = sc::newTransformType("js.bad");
      must(sc::transformChannel(*info, "x", sc::makeFloat(1), false));
      fill(*info);
      const fakehost::lf::Handle h = host.add(plane.make());
      test_assert(
          !sc::registerForeignTransformType(*d.ctx, *info, h, "evaluate setWorld").ok());
      test_assert(host.releasesOf(h) == 1);
      litestl::alloc::Delete(info);
    };
    refused(
        [](TransformTypeInfo &info) { must(sc::transformEnumChannel(info, "mode")); });
    refused([](TransformTypeInfo &info) {
      must(sc::transformChannelNumbers(info, "x", "range", 2, 1));
    });
    refused([](TransformTypeInfo &info) {
      must(sc::transformChannelText(info, "x", "unit", "Length"));
      must(sc::transformChannelText(info, "x", "displayUnit", "parsec"));
    });
  }
  test_assert(host.live() == 0 && host.allReleasedOnce());
}

/* ---- slots ---- */

void registerPlane(Doc &d,
                   fakehost::Host &host,
                   Plane &plane,
                   const char *slots,
                   const char *first = "x",
                   int version = 0)
{
  TransformTypeInfo *type = sc::newTransformType("js.plane");
  must(sc::transformTypeVersion(*type, version));
  must(sc::transformChannel(*type, first, sc::makeFloat(0), false));
  must(sc::transformChannel(*type, "y", sc::makeFloat(0), false));
  must(sc::registerForeignTransformType(*d.ctx, *type, host.add(plane.make()), slots));
  litestl::alloc::Delete(type);
}

void testSlots()
{
  fakehost::Host host;
  Plane plane;
  {
    IoDoc d;
    registerPlane(d, host, plane, "evaluate setWorld evaluateMany pivot");
    const BlockHandle top = d.object("top", 10);
    Vector<BlockHandle> all;
    for (int i = 0; i < 4; i++) {
      const BlockHandle h = d.object("o");
      d.obj(h).setTransformType("js.plane");
      d.obj(h).channels = ChannelBlock(2);
      d.obj(h).channels.set(0, i);
      d.obj(h).channels.set(1, 5.0);
      d.setParent(h, i < 3 ? top : all[0]);
      all.append(h);
    }
    d.evaluate();
    test_assert(plane.batches == 2 && plane.singles == 0);
    const auto xOf = [&](BlockHandle h) { return d.result(h).state.world[3][0]; };
    test_assert(xOf(all[2]) == 12.0 && xOf(all[3]) == 13.0);
    test_assert(d.result(all[3]).state.world[3][1] == 10.0);
    Result<double3> p = computePivot(*d.lib, all[2], 0.0);
    test_assert(p.ok() && (*p)[0] == 10.0 && (*p)[2] == 9.0);
    plane.fail = true;
    test_assert(isError(evaluateScene(*d.ctx), StatusCode::Internal, "no states"));
    test_assert(d.statusOf(all[2]) == EvalStatus::ModelFailed && xOf(all[3]) == 10.0);
    plane.fail = false;
  }
  {
    Plane first, second;
    IoDoc old, now;
    registerPlane(old, host, first, "evaluate setWorld", "wide");
    registerPlane(now, host, second, "evaluate setWorld upgrade", "x", 1);
    const BlockHandle h = *newObject(*old.lib, "v", "js.plane");
    must(old.sc().add(*old.lib, h));
    old.obj(h).channels.set(0, 3.0);
    const Bytes bytes = save(*old.lib);
    {
      Loaded l(now, bytes);
      Result<TransformState> s = computeState(l.lib, l.byName("v"), 0.0);
      test_assert(s.ok() && solve::translationOf(s->world)[0] == 3.0 &&
                  second.upgrades == 1);
    }
    second.fail = true;
    Loaded l(now, bytes);
    test_assert(!computeState(l.lib, l.byName("v"), 0.0).ok() &&
                !now.rt.hasStickyError());
    second.fail = false;
  }
  test_assert(host.live() == 0 && host.allReleasedOnce());
}

} // namespace

int main()
{
  test_assert(sc::bindings());
  testResults();
  testChannels();
  testSlots();
  return test_end();
}
