/* core.object.duplicate and core.scene.duplicate, core.object.new with data and a place,
 * and linking several objects. */
#include "io_doc.h"

#include "coredcc/api/ref_text.h"

#include <cmath>

test_init;

using namespace scene_test;

namespace {

PropValue text(const std::string &s)
{
  return PropValue::ofString(s.c_str());
}

Vector<BlockHandle>
handlesOf(CommandDoc &d, const PropertyBag &out, const char *name = "objects")
{
  Vector<BlockHandle> list;
  Result<Vector<BlockId>> ids = parseBlockIds(d.rt, out.get(name)->asString().c_str());
  test_assert(ids.ok());
  for (int i = 0; ids.ok() && i < int(ids->size()); i++) {
    list.append(d.lib->find((*ids)[i]));
  }
  return list;
}

std::string idsOf(CommandDoc &d, std::initializer_list<BlockHandle> handles)
{
  Vector<BlockId> ids;
  for (BlockHandle h : handles) {
    ids.append(d.idOf(h));
  }
  return formatBlockIds(d.rt, {ids.data(), size_t(ids.size())});
}

/* ---- objects ---- */

void testObjects()
{
  int parents = 0, outside = 0;
  for (int k = 0; k < 40 * coredcc_test::testScale(); k++) {
    CommandDoc d;
    Rand r(k);
    Vector<BlockHandle> all;
    for (int i = 0; i < 10; i++) {
      all.append(d.object(("o" + std::to_string(i)).c_str(), r.range(-3, 3)));
      if (i && r.coin()) {
        d.setParent(all[i], all[int(r.below(uint32_t(i)))]);
      }
      d.obj(all[i]).flags = r.below(5) ? ObjectFlags::None : ObjectFlags::Hidden;
      must(writeLocks(*d.ctx, all[i], r.below(8)));
    }
    Vector<BlockId> ids;
    for (int i = 0; i < 10; i++) {
      if (r.below(3) == 0) {
        ids.append(d.idOf(all[i]));
      }
    }
    if (ids.size() == 0) {
      continue;
    }
    const bool children = r.coin(), select = r.coin();
    d.select({all[0]});
    must(d.lib->writeRef(d.sc().activeCell(), ids[0]));
    /* What should be copied: the named ones, then what is below them, in the scene's
     * order. */
    Vector<BlockHandle> want;
    const auto has = [&](BlockHandle h) {
      bool yes = false;
      for (BlockHandle x : want) {
        yes = yes || x == h;
      }
      return yes;
    };
    for (const BlockId &id : ids) {
      want.append(d.lib->find(id));
    }
    const int named = int(want.size());
    for (int i = 0; children && i < 10; i++) {
      bool below = false;
      for (int n = 0; n < named; n++) {
        below = below || isSelfOrAncestor(*d.lib, want[n], all[i]);
      }
      if (below && !has(all[i])) {
        want.append(all[i]);
      }
    }
    const int members = int(d.sc().objects(*d.lib).size());
    const std::string named_ = formatBlockIds(d.rt, {ids.data(), size_t(ids.size())});
    PropertyBag out = d.cycle(kDuplicateObjectCommand,
                              {{"objects", text(named_)},
                               {"children", PropValue::ofBool(children)},
                               {"select", PropValue::ofBool(select)}});
    const Vector<BlockHandle> copies = handlesOf(d, out);
    test_assert(copies.size() == want.size() &&
                out.get("count")->asInt() == int64_t(want.size()));
    const Vector<BlockHandle> now = d.sc().objects(*d.lib);
    test_assert(int(now.size()) == members + int(want.size()));
    for (int i = 0; i < int(want.size()) && i < int(copies.size()); i++) {
      test_assert(now[members + i] == copies[i]);
      const Object &a = d.obj(want[i]), &b = d.obj(copies[i]);
      test_assert(a.flags == b.flags && a.channels.get(0) == b.channels.get(0));
      test_assert(readObject(*d.lib, want[i])->info.locks ==
                  readObject(*d.lib, copies[i])->info.locks);
      const BlockHandle p = d.lib->resolveRef(a.parent.cell()),
                        q = d.lib->resolveRef(b.parent.cell());
      int at = -1;
      for (int n = 0; n < int(want.size()); n++) {
        at = want[n] == p ? n : at;
      }
      test_assert(q == (at >= 0 ? copies[at] : p));
      parents += at >= 0 ? 1 : 0;
      outside += at < 0 && p ? 1 : 0;
      test_assert(sameBits(d.stateOf(want[i]), d.stateOf(copies[i])));
    }
    const Vector<BlockHandle> selected = d.sc().selected(*d.lib);
    test_assert(select ? selected.size() == copies.size() && selected[0] == copies[0]
                       : selected.size() == 1 && selected[0] == all[0]);
    test_assert(d.sc().active(*d.lib) == (select ? copies[0] : want[0]));
  }
  test_assert(parents > 10 && outside > 10);

  /* Data: the same, or copies. Hosts' objects and data can't be copied. */
  IoDoc d;
  const BlockHandle a = d.object("a", 1), b = d.object("b", 2),
                    dataBlock = d.object("data", 0);
  d.setData(a, dataBlock);
  d.setData(b, dataBlock);
  d.select({a, b});
  PropertyBag out = d.ok(kDuplicateObjectCommand);
  Vector<BlockHandle> copies = handlesOf(d, out);
  test_assert(d.lib->resolveRef(d.obj(copies[0]).data.cell()) == dataBlock);
  d.undo();
  out = d.cycle(kDuplicateObjectCommand, {{"linkData", PropValue::ofBool(false)}});
  copies = handlesOf(d, out);
  const BlockHandle copied = d.lib->resolveRef(d.obj(copies[0]).data.cell());
  test_assert(copied && copied != dataBlock && copies.size() == 2 &&
              d.lib->resolveRef(d.obj(copies[1]).data.cell()) == copied);
  test_assert(!d.sc().contains(*d.lib, copied));
  d.undo();
  const BlockHandle h = d.hostObject("h");
  d.select({a, h});
  test_assert(d.fails(kDuplicateObjectCommand).code() == StatusCode::Unsupported);
  d.host(h).onRead = [&](ObjectInfo &) { d.rt.setStickyError(boom()); };
  test_assert(
      d.fails(kDuplicateObjectCommand, {{"linkData", PropValue::ofBool(false)}}).code() ==
      StatusCode::Internal);
  d.host(h).onRead = nullptr;
  d.select({});
  test_assert(d.fails(kDuplicateObjectCommand).code() == StatusCode::FailedPrecondition);
  /* A member that is no object. */
  const BlockHandle odd = d.shapeBlock("odd", 1.0);
  must(d.sc().add(*d.lib, odd));
  d.select({a});
  d.cycle(kDuplicateObjectCommand, {{"children", PropValue::ofBool(true)}});

  /* Then a drag, as two steps. */
  d.select({a});
  d.ok(kDuplicateObjectCommand);
  d.ok(kApplyObjectDeltaCommand, {{"vector", PropValue::ofVec3(5, 0, 0)}});
  test_assert(d.obj(a).channels.get(0) == 1.0 &&
              d.obj(d.sc().selected(*d.lib)[0]).channels.get(0) == 6.0);
}

/* ---- scenes ---- */

void testScenes()
{
  IoDoc d;
  const BlockHandle a = d.object("a", 1), b = d.object("b", 2), data = d.object("data");
  d.setParent(b, a);
  d.setData(a, data);
  must(d.sc().remove(*d.lib, d.idOf(data)).status());
  d.select({b});
  must(d.lib->writeRef(d.sc().activeCell(), d.idOf(b)));
  d.sc().frame = 12;
  const int members = 2;
  PropertyBag out = d.cycle(kDuplicateSceneCommand, {{"name", text("Copy")}});
  BlockHandle copy = d.blockOut(out, "scene");
  Scene &s = *Scene::of(*d.lib, copy);
  test_assert(d.lib->get(copy)->hasFlag(BlockFlags::Root));
  test_assert(!strcmp(d.lib->get(copy)->name().c_str(), "Copy") && s.frame == 12);
  Vector<BlockHandle> objects = s.objects(*d.lib);
  test_assert(int(objects.size()) == members && objects[0] != a && objects[1] != b);
  test_assert(d.lib->resolveRef(d.obj(objects[1]).parent.cell()) == objects[0]);
  test_assert(d.lib->resolveRef(d.obj(objects[0]).data.cell()) == data);
  test_assert(s.selected(*d.lib).size() == 1 && s.selected(*d.lib)[0] == objects[1] &&
              s.active(*d.lib) == objects[1]);
  test_assert(d.ctx->scene() == d.scene && d.sc().objects(*d.lib)[0] == a);
  /* It and its objects outlive the history. */
  d.history->clear();
  must(d.lib->collect().status());
  test_assert(Scene::of(*d.lib, copy) && s.objects(*d.lib).size() == 2);

  out = d.ok(kDuplicateSceneCommand,
             {{"scene", d.ptr(copy)}, {"objects", PropValue::ofBool(false)}});
  const BlockHandle shared = d.blockOut(out, "scene");
  test_assert(Scene::of(*d.lib, shared)->objects(*d.lib)[0] == objects[0] &&
              !strcmp(d.lib->get(shared)->name().c_str(), "Copy"));
  out = d.cycle(kDuplicateSceneCommand, {{"linkData", PropValue::ofBool(false)}});
  copy = d.blockOut(out, "scene");
  objects = Scene::of(*d.lib, copy)->objects(*d.lib);
  const BlockHandle own = d.lib->resolveRef(d.obj(objects[0]).data.cell());
  test_assert(own && own != data);
  /* Undone and out of the history, it is gone. */
  d.undo();
  d.history->clear();
  must(d.lib->collect().status());
  test_assert(!d.lib->get(copy));

  test_assert(isError(d.fails(kDuplicateSceneCommand, {{"scene", d.ptr(a)}}),
                      StatusCode::FailedPrecondition,
                      "isn't a scene"));
  d.ctx->setScene(BlockHandle());
  test_assert(d.fails(kDuplicateSceneCommand).code() == StatusCode::FailedPrecondition);
}

/* ---- add and link ---- */

void testAdd()
{
  CommandDoc d;
  const BlockHandle p = d.object("p", 10), shape = d.shapeBlock("shape", 1.0);
  put(d.obj(p).channels, trs::kRotationEuler, double3(0, 0, 0.5));
  d.select({p});
  must(d.lib->writeRef(d.sc().activeCell(), d.idOf(p)));
  const double half = std::sqrt(0.5);
  PropertyBag out = d.cycle(kNewObjectCommand,
                            {{"location", PropValue::ofVec3(1, 2, 3)},
                             {"rotation", PropValue::ofQuat(0, 0, 3 * half, 3 * half)},
                             {"scale", PropValue::ofVec3(2, 3, 4)},
                             {"parent", d.ptr(p)},
                             {"data", d.ptr(shape)},
                             {"select", PropValue::ofBool(true)}});
  const BlockHandle h = d.blockOut(out, "object");
  mat4d want;
  want.identity();
  double *m = want;
  m[0] = m[5] = 0.0;
  m[1] = 2.0;
  m[4] = -3.0;
  m[10] = 4.0;
  m[12] = 1.0;
  m[13] = 2.0;
  m[14] = 3.0;
  test_assert(difference(d.stateOf(h).world, want) < 1e-12);
  test_assert(d.lib->resolveRef(d.obj(h).data.cell()) == shape);
  test_assert(d.sc().selected(*d.lib).size() == 1 && d.sc().selected(*d.lib)[0] == h &&
              d.sc().active(*d.lib) == h);
  int heard = 0;
  ScopedConnection c =
      d.rt.ext<ChangeBus>().addChangeListener([&](const BusChange &change) {
        heard += change.member == "selectedObjects" ? 1 : 0;
      });
  d.undo();
  test_assert(heard == 1 && d.sc().selected(*d.lib)[0] == p &&
              d.sc().active(*d.lib) == p);
  d.redo();
  test_assert(heard == 2);
  d.ok(kSelectSceneCommand, {{"objects", text(idsOf(d, {p}))}});
  test_assert(heard == 3);
  d.undo();
  test_assert(heard == 4);
  test_assert(d.sc().selected(*d.lib)[0] == h && d.sc().active(*d.lib) == h);
  out = d.ok(kNewObjectCommand);
  test_assert(d.sc().selected(*d.lib)[0] == h && d.sc().active(*d.lib) == h);
  test_assert(isError(d.fails(kNewObjectCommand, {{"data", d.ptr(p)}}),
                      StatusCode::FailedPrecondition,
                      "can't be an object's data"));
  /* A rotation that isn't one. */
  out = d.ok(kNewObjectCommand, {{"rotation", PropValue::ofQuat(0, 0, 0, 0)}});
  mat4d one;
  one.identity();
  test_assert(difference(d.stateOf(d.blockOut(out, "object")).world, one) == 0.0);
}

void testLink()
{
  CommandDoc d;
  const BlockHandle a = d.loose("a"), b = d.loose("b"), c = d.object("c"),
                    s = d.shapeBlock("s", 1.0);
  for (BlockHandle h : {a, b}) {
    d.lib->setFlag(h, BlockFlags::FakeUser, true);
  }
  d.cycle(kLinkSceneCommand, {{"objects", text(idsOf(d, {b, a, b}))}});
  Vector<BlockHandle> now = d.sc().objects(*d.lib);
  test_assert(now.size() == 3 && now[1] == b && now[2] == a);
  d.undo();
  test_assert(d.sc().objects(*d.lib).size() == 1);
  test_assert(d.fails(kLinkSceneCommand, {{"objects", text(idsOf(d, {a, c}))}}).code() ==
              StatusCode::AlreadyExists);
  test_assert(isError(d.fails(kLinkSceneCommand, {{"objects", text(idsOf(d, {a, s}))}}),
                      StatusCode::FailedPrecondition,
                      "'s' isn't Transformable"));
  test_assert(d.fails(kLinkSceneCommand, {{"objects", text("")}}).code() ==
              StatusCode::FailedPrecondition);
  test_assert(d.fails(kLinkSceneCommand).code() == StatusCode::FailedPrecondition);
  test_assert(
      d.fails(kLinkSceneCommand, {{"objects", text(idsOf(d, {a}))}, {"object", d.ptr(b)}})
          .code() == StatusCode::InvalidArgument);
  d.cycle(kLinkSceneCommand, {{"object", d.ptr(a)}});
  test_assert(d.sc().contains(*d.lib, a));
}

} // namespace

int main()
{
  testObjects();
  testScenes();
  testAdd();
  testLink();
  return test_end();
}
