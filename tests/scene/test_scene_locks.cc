/* Locked channels, flags and world matrices through commands, and a host's objects by
 * path. */
#include "io_doc.h"

#include "coredcc/api/ref_text.h"
#include "coredcc/lib/duplicate.h"
#include "coredcc/scene/channel_io.h"

#include <cmath>
#include <cstring>

test_init;

using namespace scene_test;

namespace {

constexpr uint64_t kLocation = 7; /* of TRS: the first three */

TransformDelta translate(double x, double y, double z)
{
  TransformDelta t;
  t.vector = double3(x, y, z);
  return t;
}

PropValue text(const std::string &s)
{
  return PropValue::ofString(s.c_str());
}

/* ---- the solvers ---- */

/* Against the solution without locks: locked channels are as they were, the others as
 * they are there. */
void testSolvers()
{
  Types t;
  Rand r(7);
  int some = 0, none = 0;
  for (int k = 0; k < 3000 * coredcc_test::testScale(); k++) {
    const bool useTrs = r.coin();
    const TransformType &type = useTrs ? *t.trs : *t.fbx;
    const double3 scale = r.scale(2, true);
    const ChannelBlock c = useTrs ? trsChannels(r, type, scale, 3.0)
                                  : fbxChannels(r, type, int(r.below(3)), scale, 0.5);
    const TransformState parent = chain(r, t, int(r.below(3)), 2.0, true);
    const uint64_t locks =
        r.next() & r.next() & ((uint64_t(1) << type.channelCount()) - 1);
    const EvalCtx free(c), locked = EvalCtx(c).withLocks(locks);
    TransformDelta d;
    d.kind = DeltaKind(int(r.below(3)));
    d.vector = d.kind == DeltaKind::Scale ? r.scale(2, true) : r.vec(3.0);
    d.axis = r.vec(1.0);
    d.angle = r.range(-2, 2);
    d.pivot = r.vec(2.0);
    const Result<ChannelBlock> a = solveDelta(type, free, c, d, parent);
    FixedChannels fixed;
    const Result<ChannelBlock> b = solveDelta(type, locked, c, d, parent, &fixed);
    test_assert(!fixed.driven);
    if (!a.ok() || !b.ok()) {
      continue;
    }
    bool differs = false, lone = d.kind != DeltaKind::Scale;
    for (int i = 0; i < c.size(); i++) {
      if ((locks >> i) & 1) {
        test_assert(b->get(i) == c.get(i));
        differs = differs || std::fabs(a->get(i) - c.get(i)) > 1e-6;
      } else if (lone) {
        test_assert(b->get(i) == a->get(i));
      }
    }
    test_assert(!lone || !differs || fixed.locked);
    (fixed.locked ? some : none)++;

    /* A place. Exactly, or an error. */
    const mat4d target =
        eval(type, useTrs ? trsChannels(r, type, scale, 3.0) : c, parent).world;
    const Result<ChannelBlock> e =
        solveWorld(type, locked, target, parent, SetWorldMode::Exact);
    if (e.ok()) {
      test_assert(difference(eval(type, *e, parent).world, target) < 1e-6);
    }
    const Result<ChannelBlock> n =
        solveWorld(type, locked, target, parent, SetWorldMode::Closest, &fixed);
    for (int i = 0; n.ok() && i < c.size(); i++) {
      test_assert(!((locks >> i) & 1) || n->get(i) == c.get(i));
    }
    const Result<ChannelBlock> w =
        solveWorld(type, free, target, parent, SetWorldMode::Exact);
    test_assert(!w.ok() || fixed.any() || !n.ok() || e.ok());
  }
  printf("locks: %d in the way, %d not\n", some, none);
  test_assert(some > 500 && none > 100);

  /* Channels after the first 64 can't be locked. */
  ChannelBlock many(70);
  const EvalCtx all = EvalCtx(many).withLocks(~uint64_t(0));
  test_assert(all.isLocked(0) && all.isLocked(63) && !all.isLocked(64) &&
              !all.isLocked(-1));
  test_assert(all.isFixed(63) && !all.isFixed(64) && !EvalCtx(many).hasFixed());
}

/* ---- names ---- */

void testNames()
{
  Types t;
  const auto names = [&](uint64_t locks) { return detail::lockNames(*t.trs, locks); };
  test_assert(names(0).empty() && names(7) == "location" &&
              names(5) == "location.0 location.2");
  test_assert(names(8 | 7).rfind("location ", 0) == 0);
  std::string unknown;
  test_assert(detail::lockMask(*t.trs, " location.0  location.2 ", &unknown) == 5 &&
              unknown.empty());
  test_assert(detail::lockMask(*t.trs, "location nope location.3 x.", &unknown) == 7 &&
              unknown == "nope location.3 x.");
  /* Those after the first 64. */
  Runtime rt;
  must(registerImpl<TransformModel, FnModel>(rt, "test.many#impl"));
  Result<Owned<TransformModel>> owned =
      own<TransformModel>(rt, "test.many#impl", litestl::alloc::New<FnModel>("FnModel"));
  must(owned.status());
  TransformTypeInfo info;
  info.name = "test.many";
  for (int i = 0; i < 31; i++) {
    info.channels.append(PropertyDef::vecProp("v" + std::to_string(i), 2));
  }
  info.channels.append(PropertyDef::floatProp("x"));
  info.channels.append(PropertyDef::vecProp("over", 2));
  info.channels.append(PropertyDef::floatProp("y"));
  const TransformType &many =
      **rt.ext<TransformRegistry>().add(std::move(info), std::move(owned).value());
  unknown.clear();
  test_assert(detail::lockMask(many, "x over y over.1", &unknown) == uint64_t(3) << 62 &&
              unknown == "y over.1");
  test_assert(detail::lockNames(many, ~uint64_t(0) << 62) == "x over.0");

  Rand r(3);
  for (int k = 0; k < 200; k++) {
    const TransformType &type = k & 1 ? *t.trs : *t.fbx;
    const uint64_t locks = r.next() & ((uint64_t(1) << type.channelCount()) - 1);
    unknown.clear();
    test_assert(detail::lockMask(type, detail::lockNames(type, locks), &unknown) ==
                    locks &&
                unknown.empty());
  }
}

/* ---- commands ---- */

void testCommand()
{
  CommandDoc d;
  const BlockHandle a = d.object("a", 1, 2, 3), f = d.object("f"), h = d.hostObject("h"),
                    b = d.object("b", 5);
  d.obj(f).setTransformType(kFbxTransform);
  d.obj(f).channels = d.fbx->defaults();
  d.select({a, f, h});
  /* `location` is none of FBX's; hosts may have no slot. */
  PropertyBag out = d.cycle(kSetLocksCommand, {{"channels", text("location.0")}});
  test_assert(out.get("skipped")->asInt() == 2);
  test_assert(readObject(*d.lib, a)->info.locks == 1);
  d.select({f});
  test_assert(isError(d.fails(kSetLocksCommand, {{"channels", text("location.0 no")}}),
                      StatusCode::FailedPrecondition,
                      "no channel 'location.0 no'"));
  d.select({h});
  test_assert(d.fails(kSetLocksCommand).code() == StatusCode::Unsupported);
  test_assert(d.fails(kSetFlagsCommand).code() == StatusCode::Unsupported);
  d.select({a});
  d.ok(kSetLocksCommand, {{"channels", text("location.2")}});
  test_assert(readObject(*d.lib, a)->info.locks == 5);

  /* A drag. Undo of the lock isn't in its way. */
  d.select({a, b});
  out = d.cycle(kApplyDeltaCommand, {{"vector", PropValue::ofVec3(10, 20, 30)}});
  test_assert(out.get("locked")->asInt() == 1 && out.get("driven")->asInt() == 0);
  test_assert(lengthOf(get(d.obj(a).channels, 0) - double3(1, 22, 3)) == 0.0);
  test_assert(lengthOf(get(d.obj(b).channels, 0) - double3(15, 20, 30)) == 0.0);
  const CommandDef *def = d.rt.ext<CommandRegistry>().find(kDragDeltaCommand);
  must(d.ex->beginInteractive(*d.ctx, *def, def->newInputs()));
  ApplyDelta *drag = d.ex->interactiveCommand()->as<ApplyDelta>();
  test_assert(drag->locked() == 0);
  must(drag->update(*d.ctx, translate(1, 1, 1)));
  test_assert(drag->locked() == 1);
  must(drag->update(*d.ctx, translate(0, 1, 0)));
  test_assert(drag->locked() == 0);
  must(d.ex->cancel(*d.ctx));

  /* What keeps its place isn't hindered; another transform type has no locks, and undo
   * has them. */
  d.select({a});
  d.cycle(kSetParentCommand, {{"parent", d.ptr(b)}});
  test_assert(d.obj(a).channels.get(0) == -14.0 &&
              readObject(*d.lib, a)->info.locks == 5);
  out = d.cycle(kSetTransformTypeCommand, {{"type", text(kMatrixTransform)}});
  test_assert(readObject(*d.lib, a)->info.locks == 0);
  d.undo();
  test_assert(readObject(*d.lib, a)->info.locks == 5);
  d.ok(kSetTransformTypeCommand, {{"type", text(kTrsTransform)}}); /* it has it */
  test_assert(readObject(*d.lib, a)->info.locks == 5);

  /* Natively too. */
  must(writeTransformType(*d.ctx, a, TypeId::of(kTrsTransform), d.obj(a).channels));
  test_assert(readObject(*d.lib, a)->info.locks == 5);
  const ChannelBlock was = d.obj(a).channels;
  must(writeTransformType(*d.ctx, a, TypeId::of(kFbxTransform), d.fbx->defaults()));
  test_assert(readObject(*d.lib, a)->info.locks == 0);
  must(writeTransformType(*d.ctx, a, TypeId::of(kTrsTransform), was));
  must(writeLocks(*d.ctx, a, 5));

  /* All of them, and none. */
  d.cycle(kSetLocksCommand);
  test_assert(readObject(*d.lib, a)->info.locks == (uint64_t(1) << trs::kCount) - 1);
  d.cycle(kSetLocksCommand,
          {{"on", PropValue::ofBool(false)}, {"channels", text("location")}});
  test_assert(readObject(*d.lib, a)->info.locks == (uint64_t(1) << trs::kCount) - 8);
  d.cycle(kSetLocksCommand, {{"on", PropValue::ofBool(false)}});
  test_assert(readObject(*d.lib, a)->info.locks == 0);

  /* setWorldOf is a tool's. */
  d.ok(kSetLocksCommand, {{"channels", text("location.1")}});
  mat4d m = d.stateOf(a).world;
  static_cast<double *>(m)[13] += 1.0;
  test_assert(isError(setWorldOf(*d.lib, a, m, SetWorldMode::Exact, 0.0).status(),
                      StatusCode::FailedPrecondition,
                      "locked"));
  const Result<ChannelBlock> near = setWorldOf(*d.lib, a, m, SetWorldMode::Closest, 0.0);
  test_assert(near.ok() && near->get(1) == d.obj(a).channels.get(1));
}

void testFlags()
{
  CommandDoc d;
  const BlockHandle a = d.object("a"), b = d.object("b");
  d.select({a, b});
  const auto flags = [](ObjectFlags f) { return PropValue::ofFlags(uint32_t(f)); };
  d.cycle(kSetFlagsCommand, {{"flags", flags(ObjectFlags::Unselectable)}});
  test_assert(d.obj(a).flags == ObjectFlags::Unselectable);
  const PropValue both =
      text(formatBlockId(d.rt, d.idOf(a)) + ";" + formatBlockId(d.rt, d.idOf(b)));
  test_assert(!d.run(kSetFlagsCommand, {{"flags", flags(ObjectFlags::Hidden)}}).ok());
  d.cycle(kSetFlagsCommand, {{"flags", flags(ObjectFlags::Hidden)}, {"objects", both}});
  test_assert(d.obj(b).flags == (ObjectFlags::Hidden | ObjectFlags::Unselectable));
  d.cycle(kSetFlagsCommand,
          {{"flags", flags(ObjectFlags::Unselectable)},
           {"objects", both},
           {"on", PropValue::ofBool(false)}});
  test_assert(d.obj(b).flags == ObjectFlags::Hidden);
}

/* ---- setWorld ---- */

std::string numbers(const mat4d &m)
{
  std::string out;
  const double *d = m;
  for (int i = 0; i < 16; i++) {
    char buf[40];
    snprintf(buf, sizeof(buf), "%.17g ", d[i]);
    out += buf;
  }
  return out;
}

void testSetWorld()
{
  /* All objects of a random tree at random places, at once. */
  for (int k = 0; k < 40 * coredcc_test::testScale(); k++) {
    CommandDoc d;
    Rand r(k);
    Vector<BlockHandle> all;
    std::string ids, matrices;
    Vector<mat4d> want;
    for (int i = 0; i < 10; i++) {
      all.append(d.object(("o" + std::to_string(i)).c_str(), r.range(-3, 3)));
    }
    /* Any order: parents may come later. */
    for (int i = 0; i < 10; i++) {
      const int p = int(r.below(10));
      if (!isSelfOrAncestor(*d.lib, all[i], all[p])) {
        d.setParent(all[i], all[p]);
      }
    }
    for (int i = 0; i < 10; i++) {
      if (r.below(4) == 0) {
        want.append(mat4d());
        continue;
      }
      mat4d m;
      m.identity();
      const quatd q = r.rotation();
      m = q.toMatrix4() * m;
      double *e = m;
      for (int c = 0; c < 3; c++) {
        e[12 + c] = r.range(-5, 5);
      }
      want.append(m);
      ids += (ids.empty() ? "" : ";") + formatBlockId(d.rt, d.idOf(all[i]));
      matrices += numbers(m);
    }
    if (ids.empty()) {
      continue;
    }
    PropertyBag out =
        d.cycle(kSetWorldCommand, {{"objects", text(ids)}, {"matrices", text(matrices)}});
    test_assert(out.get("approximate")->asInt() == 0 && out.get("skipped")->asInt() == 0);
    for (int i = 0; i < 10; i++) {
      const double *e = want[i];
      if (e[15] == 1.0 &&
          ids.find(formatBlockId(d.rt, d.idOf(all[i]))) != std::string::npos)
      {
        test_assert(difference(d.stateOf(all[i]).world, want[i]) < 1e-9);
      }
    }
  }

  CommandDoc d;
  const BlockHandle a = d.object("a"), b = d.object("b");
  const std::string ia = formatBlockId(d.rt, d.idOf(a)),
                    ib = formatBlockId(d.rt, d.idOf(b));
  mat4d m;
  m.identity();
  static_cast<double *>(m)[12] = 4.0;
  const std::string one = numbers(m);
  const auto run = [&](const std::string &ids, const std::string &matrices) {
    return d
        .fails(kSetWorldCommand, {{"objects", text(ids)}, {"matrices", text(matrices)}})
        .code();
  };
  test_assert(run(ia, "") == StatusCode::InvalidArgument);
  test_assert(run("", "") == StatusCode::InvalidArgument);
  test_assert(run(ia, one + "1") == StatusCode::InvalidArgument);
  test_assert(run(ia, one + "x") == StatusCode::InvalidArgument);
  test_assert(run(ia, "1x " + one) == StatusCode::InvalidArgument);
  test_assert(run(ia + ";" + ia, one + one) == StatusCode::InvalidArgument);
  test_assert(run(ia + ";" + ia, one) == StatusCode::InvalidArgument);
  test_assert(run("what", one) == StatusCode::InvalidArgument);
  std::string nan = one;
  nan.replace(0, 1, "nan");
  test_assert(run(ia, nan) == StatusCode::InvalidArgument);
  std::string commas = one;
  for (char &c : commas) {
    c = c == ' ' ? ',' : c;
  }
  d.ok(kSetWorldCommand, {{"objects", text(ia)}, {"matrices", text(commas)}});
  test_assert(d.obj(a).channels.get(0) == 4.0);

  must(writeLocks(*d.ctx, a, 1));
  static_cast<double *>(m)[12] = 9.0;
  static_cast<double *>(m)[13] = 6.0;
  PropertyBag held =
      d.ok(kSetWorldCommand, {{"objects", text(ia)}, {"matrices", text(numbers(m))}});
  test_assert(held.get("approximate")->asInt() == 1 &&
              lengthOf(get(d.obj(a).channels, 0) - double3(4, 6, 0)) == 0.0);
  must(writeLocks(*d.ctx, a, 0));
  static_cast<double *>(m)[12] = 4.0;
  static_cast<double *>(m)[13] = 0.0;

  /* A shear: EXACT leaves it out, CLOSEST counts it. */
  static_cast<double *>(m)[4] = 0.5;
  const std::string sheared = numbers(m) + one;
  PropertyBag out = d.ok(kSetWorldCommand,
                         {{"objects", text(ia + ";" + ib)},
                          {"matrices", text(sheared)},
                          {"mode", PropValue::ofEnum(int64_t(SetWorldMode::Exact))}});
  test_assert(out.get("skipped")->asInt() == 1 && d.obj(b).channels.get(0) == 4.0);
  out = d.cycle(kSetWorldCommand,
                {{"objects", text(ia + ";" + ib)}, {"matrices", text(sheared)}});
  test_assert(out.get("skipped")->asInt() == 0 && out.get("approximate")->asInt() == 1);
  test_assert(
      isError(d.fails(kSetWorldCommand,
                      {{"objects", text(ia)},
                       {"matrices", text(numbers(m))},
                       {"mode", PropValue::ofEnum(int64_t(SetWorldMode::Exact))}}),
              StatusCode::FailedPrecondition,
              "'a'"));
}

/* ---- files ---- */

uint64_t locksIn(const Loaded &l)
{
  return readObject(l.lib, l.byName("a"))->info.locks;
}

void testFiles()
{
  IoDoc d;
  d.addEveryKind();
  const BlockHandle a = *newObject(*d.lib, "a", "io.type");
  must(d.sc().add(*d.lib, a));
  must(writeLocks(*d.ctx, a, 0b1011));
  const Bytes bytes = save(*d.lib);
  {
    Loaded l(d, bytes);
    test_assert(locksIn(l) == 0b1011 && same(save(l.lib), bytes));
    must(writeLocks(l.ctx, l.byName("a"), 0));
    Loaded none(d, save(l.lib));
    test_assert(locksIn(none) == 0);
  }
  /* A copy has them. */
  const Result<IdMap> ids = duplicateBlocks(*d.lib, std::span<const BlockHandle>(&a, 1));
  test_assert(ids.ok() &&
              readObject(*d.lib, d.lib->find(ids->find(d.idOf(a))))->info.locks ==
                  0b1011);

  /* Without the type they are kept as they are, and read when it comes. */
  IoDoc b;
  Loaded pending(b, bytes);
  test_assert(locksIn(pending) == 0 && same(save(pending.lib), bytes));
  b.addEveryKind();
  test_assert(locksIn(pending) == 0b1011);
  Loaded again(b, save(pending.lib));
  test_assert(locksIn(again) == 0b1011);

  /* Another build's channels have other numbers, and some aren't there. */
  IoDoc c;
  c.addEveryKind(1);
  Loaded other(c, bytes);
  const ObjectRead r = *readObject(other.lib, other.byName("a"));
  test_assert(detail::lockNames(*r.type, r.info.locks) == "on size mode");
  /* Those that it doesn't know come back. */
  must(writeLocks(*d.ctx, a, 0b1011 | 1 << 4));
  Loaded extra(c, save(*d.lib));
  const ObjectRead e = *readObject(extra.lib, extra.byName("a"));
  test_assert(detail::lockNames(*e.type, e.info.locks) == "on size mode");
  IoDoc f;
  f.addEveryKind();
  Loaded back(f, save(extra.lib));
  test_assert(locksIn(back) == (0b1011 | 1 << 4));

  /* Channels that no def has a name for. */
  IoDoc g;
  const BlockHandle odd = g.object("a");
  g.obj(odd).setTransformType("test.unknown");
  g.obj(odd).channels = ChannelBlock(3);
  must(writeLocks(*g.ctx, odd, 0b101));
  Loaded numbered(g, save(*g.lib));
  test_assert(locksIn(numbered) == 0b101);
}

/* ---- a host's object by path ---- */

/* A host-owned Transformable that can do everything. */
struct Actor {
  ObjectInfo info;
  ChannelBlock channels;
  Status answer;

  void read(ObjectInfo &out, ChannelBlock &c) const
  {
    out = info;
    c = channels;
  }
  Status writeChannels(const ChannelBlock &c)
  {
    channels = answer.ok() ? c : channels;
    return answer;
  }
  Status setParentId(const BlockId &parent)
  {
    info.parent = answer.ok() ? parent : info.parent;
    return answer;
  }
  Status setTransformTypeId(const TypeId &type)
  {
    info.transformType = type;
    info.locks = 0;
    return answer;
  }
  Status setFlags(uint32_t flags)
  {
    info.flags = answer.ok() ? flags : info.flags;
    return answer;
  }
  Status setLocks(uint64_t locks)
  {
    info.locks = answer.ok() ? locks : info.locks;
    return answer;
  }
  static lbt::Struct<Actor> *defineBindings()
  {
    auto *st = new lbt::Struct<Actor>("test::Actor", sizeof(Actor));
    BIND_STRUCT_METHOD(st, read, MARGS("info", "channels"));
    BIND_STRUCT_METHOD(st, writeChannels, MARGS("channels"));
    BIND_STRUCT_METHOD(st, setParentId, MARGS("parent"));
    BIND_STRUCT_METHOD(st, setTransformTypeId, MARGS("type"));
    BIND_STRUCT_METHOD(st, setFlags, MARGS("flags"));
    BIND_STRUCT_METHOD(st, setLocks, MARGS("locks"));
    return st;
  }
};

struct ActorDoc : CommandDoc {
  ActorDoc()
  {
    addSlotType("test.Actor");
    must(registerImpl<Transformable, Actor>(rt, "test.Actor"));
    ApiStruct st("test.Actor");
    st.parent(StructId::of(kSceneObjectStruct));
    must(rt.ext<ApiRegistry>().add(std::move(st)).status());
  }
  BlockHandle add(const char *name, double x)
  {
    const BlockHandle h = addSlot<Actor>("test.Actor", name);
    actor(h).info.transformType = TypeId::of(kTrsTransform);
    actor(h).channels = trs->defaults();
    actor(h).channels.set(trs::kLocation, x);
    must(sc().add(*lib, h));
    return h;
  }
  Actor &actor(BlockHandle h)
  {
    return *static_cast<Actor *>(slot(h).data);
  }
  PropValue read(const std::string &path)
  {
    Result<PropValue> v = getValue(*ctx, path);
    if (!v.ok()) {
      fprintf(stderr, "%s: %s\n", path.c_str(), v.status().message());
    }
    test_assert(v.ok());
    return v.ok() ? *v : PropValue();
  }
};

void testActor()
{
  ActorDoc d;
  const BlockHandle a = d.add("a", 1), b = d.add("b", 2), o = d.object("o", 3),
                    h = d.hostObject("h");
  const BlockHandle shape = d.shapeBlock("shape", 1.0);
  d.actor(a).info.data = d.idOf(shape);
  const std::string p = "scene.objects[\"a\"].", q = "scene.objects[\"h\"].";
  test_assert(d.read(p + "name") == PropValue::ofString("a"));
  test_assert(d.read(p + "transformType") == PropValue::ofString(kTrsTransform));
  test_assert(d.read(p + "data") == d.ptr(shape));
  test_assert(d.read(p + "parent") == PropValue::nullPointer());
  test_assert(d.read(p + "transform.location") == PropValue::ofVec3(1, 0, 0));
  test_assert(d.read("scene.objects[\"b\"].transform.location") ==
              PropValue::ofVec3(2, 0, 0));
  test_assert(d.read(q + "flags") == PropValue::ofFlags(0));

  must(setValue(*d.ctx, p + "flags[HIDDEN]", PropValue::ofBool(true)));
  test_assert(d.actor(a).info.flags == uint32_t(ObjectFlags::Hidden) &&
              d.read(p + "flags[HIDDEN]") == PropValue::ofBool(true));
  must(setValue(*d.ctx, p + "transform.location[1]", PropValue::ofFloat(7)));
  must(setValue(*d.ctx, p + "transform.scale", PropValue::ofVec3(2, 2, 2)));
  test_assert(lengthOf(get(d.actor(a).channels, trs::kLocation) - double3(1, 7, 0)) ==
              0.0);
  test_assert(lengthOf(get(d.actor(a).channels, trs::kScale) - double3(2, 2, 2)) == 0.0);
  test_assert(d.actor(b).channels.get(trs::kLocation) == 2.0);
  must(setValue(*d.ctx, q + "transform.location[2]", PropValue::ofFloat(9)));
  test_assert(d.host(h).channels.get(trs::kLocation + 2) == 9.0);
  must(setValue(*d.ctx, p + "parent", d.ptr(b)));
  test_assert(d.actor(a).info.parent == d.idOf(b));
  test_assert(d.read(p + "parent.name") == PropValue::ofString("b"));
  test_assert(isError(setValue(*d.ctx, "scene.objects[\"b\"].parent", d.ptr(a)),
                      StatusCode::FailedPrecondition,
                      "ancestor"));
  must(setValue(*d.ctx, p + "parent", d.ptr(o)));
  must(setValue(*d.ctx, p + "parent", PropValue::nullPointer()));
  test_assert(d.actor(a).info.parent.isNull());
  must(setValue(*d.ctx, p + "locks", text("location rotationEuler.1")));
  test_assert(d.actor(a).info.locks ==
              (kLocation | uint64_t(1) << (trs::kRotationEuler + 1)));
  test_assert(d.read(p + "locks") == text("location rotationEuler.1"));
  test_assert(isError(setValue(*d.ctx, p + "locks", text("location nope")),
                      StatusCode::InvalidArgument,
                      "nope"));
  must(setValue(*d.ctx, "scene.objects[\"o\"].locks", text("location.0")));
  test_assert(readObject(*d.lib, o)->info.locks == 1);

  /* What its slots say, and slots that aren't there. */
  d.actor(a).answer = boom("no");
  test_assert(isError(setValue(*d.ctx, p + "flags", PropValue::ofFlags(0)),
                      StatusCode::Internal));
  test_assert(
      isError(setValue(*d.ctx, p + "transform.location", PropValue::ofVec3(0, 0, 0)),
              StatusCode::Internal,
              "no"));
  test_assert(d.read(p + "transform.location") == PropValue::ofVec3(1, 7, 0));
  d.actor(a).answer = Status();
  for (const char *member : {"flags", "locks", "parent"}) {
    const PropValue v = !strcmp(member, "flags")   ? PropValue::ofFlags(1)
                        : !strcmp(member, "locks") ? text("location")
                                                   : d.ptr(b);
    test_assert(setValue(*d.ctx, q + member, v).code() == StatusCode::Unsupported);
  }
  test_assert(!setValue(*d.ctx, p + "data", d.ptr(shape)).ok());

  /* Undo, by path and by command. */
  const std::string before = d.describe();
  must(setValueUndo(*d.ctx, p + "transform.location", PropValue::ofVec3(5, 5, 5)));
  must(setValueUndo(*d.ctx, p + "flags", PropValue::ofFlags(2)));
  test_assert(d.actor(a).channels.get(trs::kLocation) == 5.0 &&
              d.actor(a).info.flags == 2);
  d.undo();
  d.undo();
  test_assert(d.describe() == before);
  test_assert(d.actor(a).channels.get(trs::kLocation) == 1.0 &&
              d.actor(a).info.flags == 1);
  d.redo();
  test_assert(d.actor(a).channels.get(trs::kLocation) == 5.0);
  d.actor(a).info.flags = 0;
  d.select({a, o, h});
  PropertyBag out = d.ok(kSetLocksCommand, {{"on", PropValue::ofBool(false)}});
  test_assert(out.get("skipped")->asInt() == 1 && d.actor(a).info.locks == 0 &&
              readObject(*d.lib, o)->info.locks == 0);
  d.undo();
  test_assert(d.actor(a).info.locks != 0 && readObject(*d.lib, o)->info.locks == 1);

  /* Without a transform type there is no transform. */
  d.actor(b).info.transformType = TypeId::of("test.none");
  test_assert(!getValue(*d.ctx, "scene.objects[\"b\"].transform.location").ok());
  test_assert(d.read("scene.objects[\"b\"].transformType") == PropValue::ofString(""));
  test_assert(d.read("scene.objects[\"b\"].locks") == PropValue::ofString(""));
}

/* Its name in an error, once; and what kind of block it is. */
void testErrors()
{
  CommandDoc d;
  const BlockHandle shape = d.shapeBlock("shape", 1.0), a = d.object("a");
  must(d.sc().add(*d.lib, shape));
  d.select({shape});
  Status s = d.fails(kApplyDeltaCommand);
  test_assert(!strcmp(s.message(), "'shape' isn't Transformable"));
  must(d.lib->markDeleted(a));
  test_assert(isError(
      readObject(*d.lib, a).status(), StatusCode::FailedPrecondition, "'a' isn't Live"));
  test_assert(isError(readObject(*d.lib, BlockHandle()).status(),
                      StatusCode::FailedPrecondition,
                      "no such block"));
}

} // namespace

int main()
{
  testSolvers();
  testNames();
  testCommand();
  testFlags();
  testSetWorld();
  testFiles();
  testActor();
  testErrors();
  return test_end();
}
