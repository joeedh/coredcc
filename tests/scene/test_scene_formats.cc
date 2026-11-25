/* What another build may have put into the payloads of objects and scenes, and parents that are in no
 * scene. */
#include "io_doc.h"

#include "coredcc/api/ref_text.h"
#include "coredcc/lib/duplicate.h"

#include <functional>
#include <limits>

test_init;

using namespace scene_test;

namespace {

double xOf(Library &lib, BlockHandle h)
{
  Result<TransformState> s = computeState(lib, h, 0.0);
  must(s.status());
  return solve::translationOf(s->world)[0];
}

/* ---- parents that are in no scene ---- */

void testLooseParents()
{
  IoDoc d;
  const BlockHandle p = d.object("p", 10.0), c = d.object("c", 1.0), g = d.loose("g", double3(100, 0, 0));
  d.setParent(c, p);
  d.setParent(p, g);
  d.select({p});
  d.ok(kUnlinkCommand);
  d.evaluate();
  test_assert(d.isAt(c, 111.0));
  {
    Loaded l(d, save(*d.lib));
    test_assert(l.byName("p") && l.byName("g") && xOf(l.lib, l.byName("c")) == 111.0);
    must(evaluateScene(l.ctx));
    test_assert(l.sc().evaluatedObjects().size() == 3 && l.sc().objects(l.lib).size() == 1);
    must(l.lib.collect().status());
    test_assert(xOf(l.lib, l.byName("c")) == 111.0);
    /* An append brings them along. */
    IoDoc other;
    Result<AppendResult> r = appendLibrary(*other.ctx, view(save(l.lib)));
    must(r.status());
    test_assert(xOf(*other.lib, other.byName("c")) == 111.0);
  }
  d.history->clear();
  must(d.lib->collect().status());
  test_assert(d.lib->get(p) && d.lib->get(g) && xOf(*d.lib, c) == 111.0);
  /* A copy of the child keeps its parent too, after the child is gone. */
  Result<IdMap> ids = duplicateBlocks(*d.lib, std::span<const BlockHandle>(&c, 1));
  must(ids.status());
  const BlockHandle copy = d.lib->find(ids->find(d.idOf(c)));
  must(d.sc().add(*d.lib, copy));
  d.select({c});
  d.ok(kDeleteObjectsCommand);
  d.history->clear();
  must(d.lib->collect().status());
  test_assert(!d.lib->get(c) && d.lib->get(p) && xOf(*d.lib, copy) == 111.0);
  /* Without children they go. */
  d.setParent(copy, BlockHandle());
  must(d.lib->collect().status());
  test_assert(!d.lib->get(p) && !d.lib->get(g) && d.lib->get(copy) && d.lib->validate().ok());
}

/* A parent's deletion isn't its children's. A path may name an object outside the scene. */
void testDeletedParent()
{
  IoDoc d;
  const BlockHandle c = d.object("c", 1.0), g = d.loose("g", double3(100, 0, 0));
  must(setValue(*d.ctx, "scene.objects[\"c\"].parent", d.ptr(g)));
  must(d.lib->collect().status());
  test_assert(d.lib->get(g) && xOf(*d.lib, c) == 101.0 && d.lib->usersOf(g).size() == 1);
  test_assert(deletePreview(*d.lib, std::span<const BlockHandle>(&g, 1)).size() == 1);
  for (int i = 0; i < 2; i++) {
    d.ok(kDeleteBlockCommand, {{"block", d.ptr(g)}});
    test_assert(d.lib->get(c)->state() == BlockState::Live && xOf(*d.lib, c) == 1.0);
    test_assert(d.lib->validate().ok());
    d.undo();
    test_assert(xOf(*d.lib, c) == 101.0);
  }
  d.redo();
  d.history->clear();
  must(d.lib->collect().status());
  test_assert(!d.lib->get(g) && xOf(*d.lib, c) == 1.0 && d.lib->validate().ok());
  {
    Loaded l(d, save(*d.lib));
    test_assert(l.obj("c").parent.target().isNull() && xOf(l.lib, l.byName("c")) == 1.0);
  }
  /* A cycle that nothing else keeps. */
  const BlockHandle x = d.loose("x"), y = d.loose("y");
  d.setParent(x, y);
  d.setParent(y, x);
  must(d.lib->collect().status());
  test_assert(!d.lib->get(x) && !d.lib->get(y));
}

/* ---- fields that this build doesn't know ---- */

void addOdd(Fields &f, Vector<BlockEntry> &to, BlockHandle strong, BlockHandle weak)
{
  Vector<RefCell> cells;
  for (BlockHandle h : {strong, weak}) {
    RefCell cell(h == strong ? RefKind::OptStrong : RefKind::Weak);
    cell.target = f.d.idOf(h);
    cells.append(cell);
  }
  to.append(detail::refField("newer", std::span<const RefCell>(cells.data(), 2)));
  to.append(detail::valueField("dataMatrix", PropValue::ofString("later")));
  BlockEntry raw = detail::valueField("flags", PropValue());
  raw.unknownTag = 200;
  raw.raw.append(7);
  to.append(std::move(raw));
}

void testObjectFields()
{
  IoDoc d;
  d.lib->setAuditRelations(false);
  const BlockHandle kept = d.loose("kept"), gone = d.loose("gone"), p = d.loose("p", double3(5, 0, 0));
  Fields f(d);
  addOdd(f, f.fields, kept, gone);
  RefCell two[] = {RefCell(RefKind::OptStrong), RefCell(RefKind::OptStrong)};
  two[0].target = two[1].target = d.idOf(kept);
  f.fields.append(detail::refField("parent", two));
  RefCell parent(RefKind::Strong); /* the file's kind doesn't matter */
  parent.target = d.idOf(p);
  f.fields.append(detail::refField("parent", std::span<const RefCell>(&parent, 1)));
  f.fields.append(detail::valueField("data", PropValue::ofInt(3)));
  f.fields.append(detail::valueField("transformVersion", PropValue::ofInt(-1)));
  f.fields.append(detail::valueField("transformVersion", PropValue::ofInt(int64_t(1) << 40)));
  f.fields.append(detail::valueField("flags", PropValue::ofFlags(1)));
  f.fields.append(detail::valueField("flags", PropValue::ofFlags(4)));
  f.fields[int(f.fields.size()) - 1].refs.append(parent);
  f.fields.append(detail::refField("parent", std::span<const RefCell>(&parent, 1)));
  f.fields[int(f.fields.size()) - 1].unknownTag = 201;
  f.fields.append(detail::valueField("flags", PropValue::ofFlags(2)));
  BlockHandle h;
  must(readInto(d, kObjectType, f.payload(), &h));
  Object &o = d.obj(h);
  test_assert(o.unreadFields().size() == 9 && o.unread().empty());
  test_assert(o.flags == ObjectFlags::Unselectable && o.parent.target() == d.idOf(p));
  test_assert(o.data.target().isNull() && o.parent.cell().kind == RefKind::OptStrong);
  test_assert(sameChannels(o.channels, d.trs->defaults()));
  /* They are references like others. */
  d.lib->setFlag(h, BlockFlags::Root, true);
  must(d.lib->collect().status());
  test_assert(d.lib->get(kept) && !d.lib->get(gone) && d.lib->get(p));
  d.lib->setFlag(h, BlockFlags::FakeUser, true);
  Result<BlockPayload> again = writePayload(*d.lib, h);
  must(again.status());
  must(checkPayloadRefs(*d.lib, h, *again));
  BlockHandle h2;
  must(readInto(d, kObjectType, *again, &h2));
  test_assert(same(writePayload(*d.lib, h2)->bytes, again->bytes) && d.obj(h2).unreadFields().size() == 9);
  Result<IdMap> ids = duplicateBlocks(*d.lib, std::span<const BlockHandle>(&h, 1));
  must(ids.status());
  const BlockHandle copy = d.lib->find(ids->find(d.idOf(h)));
  test_assert(d.obj(copy).unreadFields().size() == 9);
  test_assert(d.obj(copy).memSize() >= sizeof(Object) + 9 * sizeof(BlockEntry));
  d.lib->setFlag(h2, BlockFlags::Root, true);
  Loaded l(d, save(*d.lib)); /* with the copy, whose FakeUser flag is a copy too */
  int found = 0;
  l.lib.forEach([&](BlockHandle x, const Datablock &b) {
    if (b.type() == TypeId::of(kObjectType) && b.state() == BlockState::Live) {
      const Object &y = *static_cast<Object *>(l.lib.get(x)->clientData());
      found += y.unreadFields().size() == 9 && y.unreadFields()[2].raw.size() == 1;
    }
  });
  test_assert(found == 3);
}

void testSceneFields()
{
  IoDoc d;
  d.lib->setAuditRelations(false);
  const BlockHandle kept = d.object("kept", 1.0), weak = d.object("weak", 2.0);
  Fields f(d);
  Vector<BlockEntry> &e = f.fields;
  addOdd(f, e, kept, weak);
  const auto vec2 = [](double x, double y) {
    const double c[] = {x, y};
    return PropValue::ofComponents(PropValue::Type::Vec2, c);
  };
  const double nan = std::numeric_limits<double>::quiet_NaN();
  e.append(detail::valueField("frame", PropValue::ofFloat(7.0)));
  e.append(detail::valueField("frame", PropValue::ofFloat(nan)));
  e.append(detail::valueField("startFrame", PropValue::ofFloat(3.0)));
  e.append(detail::valueField("startFrame", PropValue::ofString("x")));  /* 1 */
  e.append(detail::valueField("lengthUnit", PropValue::ofString("cm")));
  e.append(detail::valueField("lengthUnit", PropValue::ofString("parsec"))); /* 2 */
  e.append(detail::valueField("angleUnit", PropValue::ofString("turns")));   /* 3 */
  e.append(detail::valueField("massUnit", PropValue::ofInt(1)));             /* 4 */
  e.append(detail::valueField("timeUnit", PropValue::ofString("f")));
  for (const PropValue &v : {vec2(24.5, 1), vec2(0, 1), vec2(1e9, 1), vec2(nan, 1), vec2(50, 2),
                             vec2(2997, 100), vec2(1, 1000000)}) /* 5 to 11 */
  {
    e.append(detail::valueField("frameRate", v));
  }
  e.append(detail::valueField("objects", PropValue::ofInt(1))); /* 12 */
  f.out.entries(std::span<const BlockEntry>(e.data(), size_t(e.size())));
  BlockHandle h;
  must(readInto(d, kSceneType, *PayloadAccess::take(f.out, 1), &h));
  const Scene &s = *Scene::of(*d.lib, h);
  test_assert(s.unreadFields().size() == 15 && s.frame == 7.0 && s.units.startFrame == 3.0);
  test_assert(s.units.rate == FrameRate() && s.cells().size() == 0);
  test_assert(strcmp(s.units.displayUnit(UnitFamily::Length).symbol, "cm") == 0);
  test_assert(strcmp(s.units.displayUnit(UnitFamily::Time).symbol, "f") == 0);
  test_assert(s.units.displayUnit(UnitFamily::Mass).scale == 1.0);

  Result<BlockPayload> again = writePayload(*d.lib, h);
  must(again.status());
  must(checkPayloadRefs(*d.lib, h, *again));
  BlockHandle h2;
  must(readInto(d, kSceneType, *again, &h2));
  Scene &s2 = *Scene::of(*d.lib, h2);
  test_assert(same(writePayload(*d.lib, h2)->bytes, again->bytes) && s2.units == s.units);
  test_assert(s2.unreadFields().size() == 15 && s2.frame == 7.0);
  /* A setting that this build couldn't read is still the last one, so a newer build has it. Until it
   * is set. */
  const auto last = [&](const char *name) {
    BlockPayload p = *writePayload(*d.lib, h2);
    BlockReader in(*d.lib, p);
    Vector<BlockEntry> all;
    in.entries(all);
    PropValue v;
    for (const BlockEntry &x : all) {
      v = strcmp(x.name.c_str(), name) == 0 ? x.value : v;
    }
    return v;
  };
  test_assert(last("angleUnit") == PropValue::ofString("turns"));
  test_assert(last("frameRate") == vec2(1, 1000000) && last("startFrame") == PropValue::ofFloat(3.0));
  test_assert(last("lengthUnit") == PropValue::ofString("cm"));
  must(s2.units.setDisplayUnit(UnitFamily::Angle, "rad"));
  s2.units.rate = *FrameRate::of(2997, 100);
  test_assert(last("angleUnit") == PropValue::ofString("rad") && last("frameRate") == vec2(30000, 1001));
  test_assert(last("massUnit") == PropValue::ofInt(1) && last("factorUnit").tryString());

  Scene *copy = s2.duplicate();
  test_assert(copy->unreadFields().size() == 15 && copy->memSize() == s2.memSize());
  test_assert(s2.memSize() >= sizeof(Scene) + 15 * sizeof(BlockEntry));
  litestl::alloc::Delete(copy);
  d.lib->setFlag(h2, BlockFlags::Root, true);
  d.sc().remove(*d.lib, d.idOf(kept)).status().ignore();
  d.sc().remove(*d.lib, d.idOf(weak)).status().ignore();
  must(d.lib->collect().status());
  test_assert(d.lib->get(kept) && !d.lib->get(weak));
}

/* ---- versions of transform types ---- */

/* `size` of version 0 is `scale` in version 2, and twice as much; `mode` A is B since version 1. */
struct Upgrader {
  std::function<Status(uint32_t, ChannelEntries &)> fn;
  mutable int calls = 0;

  TransformState evaluate(const EvalCtx &c, const TransformState &p) const
  {
    TransformState s = p;
    double *m = s.world;
    m[12] += c.channel(0);
    return s;
  }
  Status setWorld(const EvalCtx &,
                  const mat4d &,
                  const TransformState &,
                  SetWorldMode,
                  ChannelBlock &) const
  {
    return Status::error(StatusCode::Unsupported, "Upgrader");
  }
  Status upgrade(uint32_t version, ChannelEntries &channels) const
  {
    calls++;
    return fn(version, channels);
  }
  static lbt::Struct<Upgrader> *defineBindings()
  {
    auto *st = new lbt::Struct<Upgrader>("test::Upgrader", sizeof(Upgrader));
    BIND_STRUCT_METHOD(st, evaluate, MARGS("ctx", "parent"));
    BIND_STRUCT_METHOD(st, setWorld, MARGS("current", "target", "parent", "mode", "out"));
    BIND_STRUCT_METHOD(st, upgrade, MARGS("version", "channels"));
    return st;
  }
};

struct VersionDoc : IoDoc {
  Upgrader *upgrader = nullptr;

  Status addType(uint32_t version, const char *channel, bool withUpgrade)
  {
    TransformTypeInfo info;
    info.name = "io.versioned";
    info.version = version;
    info.channels.append(PropertyDef::floatProp(channel));
    info.channels.append(PropertyDef::floatProp("extra"));
    if (!withUpgrade) {
      const char *impl = "io.versioned#plain";
      must(registerImpl<TransformModel, FnModel>(rt, impl));
      FnModel *m = litestl::alloc::New<FnModel>("FnModel");
      m->fn = [](const EvalCtx &c, const TransformState &p) { return Upgrader().evaluate(c, p); };
      return rt.ext<TransformRegistry>()
          .add(std::move(info), own<TransformModel>(rt, impl, m).value())
          .status();
    }
    const char *impl = "io.versioned#impl";
    must(registerImpl<TransformModel, Upgrader>(rt, impl));
    upgrader = litestl::alloc::New<Upgrader>("Upgrader");
    upgrader->fn = [](uint32_t from, ChannelEntries &c) {
      test_assert(from < 2);
      if (from == 0) {
        c.rename("size", "scale");
        c.set("scale", PropValue::ofFloat(c.get("scale").asFloat() * 2.0));
      }
      return Status();
    };
    return rt.ext<TransformRegistry>()
        .add(std::move(info), own<TransformModel>(rt, impl, upgrader).value())
        .status();
  }
  Bytes saved(uint32_t version, const char *channel)
  {
    must(addType(version, channel, version == 2));
    const BlockHandle h = *newObject(*lib, "v", "io.versioned");
    must(sc().add(*lib, h));
    obj(h).channels.set(0, 3.0);
    obj(h).channels.set(1, 5.0);
    return save(*lib);
  }
};

/* Whether the payload has an entry of the name, in either list. */
bool hasEntry(Library &lib, BlockHandle h, const char *name)
{
  const BlockPayload p = *writePayload(lib, h);
  BlockReader in(lib, p);
  Vector<BlockEntry> all;
  in.entries(all);
  in.entries(all);
  test_assert(in.bytes().ok() && in.bytes().atEnd());
  return detail::hasField(all, name);
}

void testVersions()
{
  VersionDoc old, middle, now;
  const Bytes v0 = old.saved(0, "size"), v1 = middle.saved(1, "scale"), v2 = now.saved(2, "scale");
  test_assert(now.upgrader->calls == 0);
  {
    Loaded l(now, v0);
    test_assert(now.upgrader->calls == 0 && l.obj("v").channels.size() == 0); /* when it's asked for */
    test_assert(same(save(l.lib), v0));
    test_assert(xOf(l.lib, l.byName("v")) == 6.0 && now.upgrader->calls == 1);
    test_assert(l.obj("v").channels.get(1) == 5.0 && l.obj("v").unread().empty());
    test_assert(xOf(l.lib, l.byName("v")) == 6.0 && now.upgrader->calls == 1);
    /* It is of the new version then. */
    Loaded again(now, save(l.lib));
    test_assert(xOf(again.lib, again.byName("v")) == 6.0 && now.upgrader->calls == 1);
  }
  {
    Loaded l(now, v1);
    test_assert(xOf(l.lib, l.byName("v")) == 3.0 && now.upgrader->calls == 2);
  }
  {
    Loaded l(now, v2);
    test_assert(l.obj("v").channels.size() == 2 && now.upgrader->calls == 2);
  }
  /* Without the slot, by name. */
  {
    Loaded l(middle, v0);
    test_assert(xOf(l.lib, l.byName("v")) == 0.0 && l.obj("v").unread().size() == 1);
    test_assert(l.obj("v").channels.get(1) == 5.0);
    /* The old value is `size` of no version in particular: it stays, and the file is of this one. */
    Loaded again(middle, save(l.lib));
    test_assert(again.obj("v").unread().size() == 1 && again.obj("v").channels.get(1) == 5.0);
  }
  /* A newer file's are kept, unread. */
  Loaded l(old, v2);
  must(evaluateScene(l.ctx));
  ObjectEval e;
  test_assert(l.sc().evaluated(l.byName("v"), e) && e.status == EvalStatus::Invalid);
  test_assert(l.obj("v").channels.size() == 0 && l.obj("v").unread().size() == 2);
  test_assert(same(save(l.lib), v2));
  /* Also next to channels that are put there directly, which are no named ones then. */
  l.obj("v").channels = ChannelBlock(2);
  test_assert(hasEntry(l.lib, l.byName("v"), "#0"));
  l.obj("v").channels = ChannelBlock();
  {
    /* Another type, and back. */
    l.ctx.setExecutor(old.ex.get());
    const TypeId id = l.obj("v").transformTypeId();
    must(writeTransformType(l.ctx, l.byName("v"), TypeId::of(kTrsTransform), old.trs->defaults()));
    test_assert(hasEntry(l.lib, l.byName("v"), "location"));
    test_assert(!hasEntry(l.lib, l.byName("v"), "transformVersion"));
    Loaded trs(old, save(l.lib));
    test_assert(trs.obj("v").channels.size() == old.trs->channelCount());
    l.obj("v").setTransformType("io.nothing");
    test_assert(!hasEntry(l.lib, l.byName("v"), "transformVersion"));
    must(writeTransformType(l.ctx, l.byName("v"), id, ChannelBlock()));
    test_assert(same(save(l.lib), v2));
    /* Channels of this build's replace them. */
    ChannelBlock two(2);
    two.set(0, 9.0);
    must(writeTransform(l.ctx, l.byName("v"), two));
    Loaded mine(old, save(l.lib));
    test_assert(xOf(mine.lib, mine.byName("v")) == 9.0 && mine.obj("v").unread().empty());
  }
  /* A build without the type keeps the version. */
  IoDoc none;
  Loaded without(none, v2);
  test_assert(same(save(without.lib), v2));
  test_assert(hasEntry(without.lib, without.byName("v"), "transformVersion"));
  ChannelBlock two(2);
  two.set(0, 4.0);
  must(writeTransform(without.ctx, without.byName("v"), two));
  Loaded numbers(now, save(without.lib));
  test_assert(xOf(numbers.lib, numbers.byName("v")) == 4.0);
  /* A type that comes after the file. */
  VersionDoc late;
  Loaded waits(late, v2);
  must(late.addType(2, "scale", true));
  test_assert(xOf(waits.lib, waits.byName("v")) == 3.0 && late.upgrader->calls == 0);
  VersionDoc large;
  test_assert(large.addType(uint32_t(1) << 31, "scale", false).code() == StatusCode::InvalidArgument);
}

/* A hook that can't: whoever asks first hears of it, and the file's channels stay. */
void testFailedUpgrade()
{
  VersionDoc old, now;
  const Bytes v0 = old.saved(0, "size");
  must(now.addType(2, "scale", true));
  const auto work = now.upgrader->fn;
  for (int thrown = 0; thrown < 2; thrown++) {
    now.upgrader->calls = 0;
    now.upgrader->fn = [&](uint32_t, ChannelEntries &c) {
      c.remove("size");
      if (thrown) {
        now.rt.setStickyError(Status::error(StatusCode::Internal, "no way"));
      }
      return thrown ? Status() : Status::error(StatusCode::OutOfRange, "no way");
    };
    Loaded l(now, v0);
    Result<TransformState> s = computeState(l.lib, l.byName("v"), 0.0);
    test_assert(s.status().code() == (thrown ? StatusCode::Internal : StatusCode::OutOfRange));
    test_assert(strstr(s.status().message(), "io.versioned', version 0: no way"));
    test_assert(!now.rt.hasStickyError() && now.upgrader->calls == 1);
    must(evaluateScene(l.ctx));
    ObjectEval e;
    test_assert(l.sc().evaluated(l.byName("v"), e) && e.status == EvalStatus::Invalid);
    test_assert(now.upgrader->calls == 1 && same(save(l.lib), v0));
    /* An error that is there already is another one's, and it isn't tried then. */
    Loaded l2(now, v0);
    now.rt.setStickyError(Status::error(StatusCode::Internal, "before"));
    l2.obj("v").resolve();
    test_assert(now.upgrader->calls == 1);
    test_assert(strcmp(now.rt.takeStickyError().message(), "before") == 0);
    now.upgrader->fn = work;
    test_assert(xOf(l2.lib, l2.byName("v")) == 6.0);
  }
}

void testEntries()
{
  Vector<BlockEntry> e;
  ChannelEntries c(e);
  e.append(detail::valueField("a", PropValue::ofFloat(1)));
  e.append(detail::valueField("a", PropValue::ofFloat(2)));
  BlockEntry raw = detail::valueField("raw", PropValue::ofFloat(1));
  raw.unknownTag = 200;
  e.append(raw);
  test_assert(c.get("a") == PropValue::ofFloat(2) && c.get("b").isNone() && c.get("raw").isNone());
  test_assert(!c.rename("b", "a") && !c.remove("b") && e.size() == 3);
  test_assert(c.rename("a", "a") && e.size() == 3);
  test_assert(c.rename("a", "raw") && e.size() == 2 && c.get("raw") == PropValue::ofFloat(2));
  c.set("raw", PropValue::ofFloat(5));
  test_assert(e.size() == 1 && c.get("raw") == PropValue::ofFloat(5));
  test_assert(c.remove("raw") && e.size() == 0);
}

/* ---- memory ---- */

/* What a load says it needs, for each object. */
void testLoadCost()
{
  IoDoc d;
  constexpr int kObjects = 2000;
  BlockHandle last;
  for (int i = 0; i < kObjects; i++) {
    const BlockHandle h = d.object(("o" + std::to_string(i)).c_str(), 1.0);
    if (last) {
      d.setParent(h, last);
    }
    last = h;
  }
  const Bytes bytes = save(*d.lib);
  uint64_t low = 0, high = uint64_t(1) << 30;
  while (low + 10000 < high) {
    const uint64_t at = (low + high) / 2;
    Library lib(&d.rt);
    Context ctx(&d.rt, &lib, d.ctx->root());
    LoadOptions options;
    options.limits.maxBytes = at;
    Result<LoadReport> r = loadLibrary(ctx, view(bytes), options);
    (r.ok() && r->issues.total == 0 ? high : low) = at;
  }
  /* Entries that are kept count. */
  Fields f(d);
  for (int i = 0; i < 3000; i++) {
    f.entry("x", PropValue::ofFloat(i));
  }
  BlockHandle h;
  d.lib->setAuditRelations(false);
  must(readInto(d, kObjectType, f.payload(), &h));
  d.lib->setFlag(h, BlockFlags::Root, true);
  const Bytes large = save(*d.lib);
  const auto loads = [&](uint64_t limit) {
    Library lib(&d.rt);
    Context ctx(&d.rt, &lib, d.ctx->root());
    LoadOptions options;
    options.limits.maxBytes = limit;
    Result<LoadReport> r = loadLibrary(ctx, view(large), options);
    return r.ok() && r->issues.total == 0;
  };
  const uint64_t kept = 3000 * sizeof(BlockEntry);
  test_assert(!loads(high + kept) && loads(high + 8 * kept));
  printf("a load takes %d bytes of its limit for an object\n", int(high / kObjects));
  test_assert(high / kObjects < 5000);
}

} // namespace

int main()
{
  testLooseParents();
  testDeletedParent();
  testObjectFields();
  testSceneFields();
  testVersions();
  testFailedUpgrade();
  testEntries();
  testLoadCost();
  return test_end();
}
