/* Objects and scenes in files: saving and loading, transform types that aren't there or have other
 * channels, duplicating, appending, and payloads that are wrong. */
#include "command_doc.h"
#include "fuzz.h"

#include "coredcc/api/library_io.h"
#include "coredcc/lib/duplicate.h"
#include "coredcc/lib/payload_access.h"
#include "coredcc/scene/channel_io.h"

#include <cmath>
#include <limits>

test_init;

using namespace scene_test;

namespace {

using Bytes = Vector<uint8_t>;
using detail::PayloadAccess;

std::span<const uint8_t> view(const Bytes &b)
{
  return std::span<const uint8_t>(b.data(), size_t(b.size()));
}
bool same(const Bytes &a, const Bytes &b)
{
  return a.size() == b.size() && memcmp(a.data(), b.data(), size_t(a.size())) == 0;
}
bool sameBits(double a, double b)
{
  return memcmp(&a, &b, sizeof(a)) == 0;
}
Bytes save(const Library &lib)
{
  SaveReport report;
  Result<Bytes> b = saveLibrary(lib, &report);
  must(b.status());
  test_assert(report.issues.total == 0);
  return *b;
}

/* A transform type with a channel of each kind. `version` 1 has other channels: `size` is an Int,
 * `count` a Float, `mode` has other items and numbers, and `extra` isn't there. */
TransformTypeInfo everyKind(int version)
{
  TransformTypeInfo info;
  info.name = "io.type";
  info.channels.append(PropertyDef::boolProp("on"));
  PropertyDef size = version ? PropertyDef::intProp("size") : PropertyDef::floatProp("size");
  info.channels.append(size);
  info.channels.append(version ? PropertyDef::floatProp("count") : PropertyDef::intProp("count"));
  const int base = version ? 20 : 0;
  PropertyDef mode = PropertyDef::enumProp("mode");
  if (!version) {
    mode.item("ONE", 1).item("TWO", 2).item("THREE", 3).defaultValue(PropValue::ofEnum(1));
  }
  else {
    mode.item("TWO", base + 2).item("ONE", base + 1).defaultValue(PropValue::ofEnum(base + 2));
  }
  info.channels.append(std::move(mode));
  if (!version) {
    info.channels.append(PropertyDef::vecProp("extra", 3));
  }
  info.channels.append(PropertyDef::vecProp("at", 2));
  info.channels.append(PropertyDef::vecProp("four", 4));
  info.channels.append(PropertyDef::quatProp("turn"));
  return info;
}

struct IoDoc : CommandDoc {
  IoDoc()
  {
    lib->setAuditRelations(true);
  }
  void addEveryKind(int version = 0)
  {
    const char *impl = "io.type#impl";
    must(registerImpl<TransformModel, FnModel>(rt, impl));
    FnModel *m = litestl::alloc::New<FnModel>("FnModel");
    m->fn = [](const EvalCtx &, const TransformState &p) { return p; };
    Result<Owned<TransformModel>> owned = own<TransformModel>(rt, impl, m);
    must(owned.status());
    must(rt.ext<TransformRegistry>().add(everyKind(version), std::move(owned).value()).status());
  }
  BlockHandle byName(const char *name)
  {
    BlockHandle found;
    lib->forEach([&](BlockHandle h, const Datablock &b) {
      if (strcmp(b.name().c_str(), name) == 0 && b.state() == BlockState::Live) {
        found = h;
      }
    });
    return found;
  }
};

/* Loads into a library of its own. */
struct Loaded {
  Library lib;
  Context ctx;
  LoadReport report;

  Loaded(IoDoc &d, const Bytes &bytes, int64_t issues = 0) : lib(&d.rt), ctx(&d.rt, &lib, d.ctx->root())
  {
    lib.setAuditRelations(true);
    Result<LoadReport> r = loadLibrary(ctx, view(bytes));
    must(r.status());
    report = *r;
    for (const IoIssue &i : report.issues.first) {
      if (issues == 0) {
        printf("issue: %s\n", i.why.toString().c_str());
      }
    }
    test_assert(report.issues.total == issues);
    ctx.setScene(byName("Scene"));
  }
  BlockHandle byName(const char *name) const
  {
    BlockHandle found;
    lib.forEach([&](BlockHandle h, const Datablock &b) {
      if (strcmp(b.name().c_str(), name) == 0 && b.state() == BlockState::Live) {
        found = h;
      }
    });
    return found;
  }
  Object &obj(const char *name) const
  {
    const BlockHandle h = byName(name);
    test_assert(bool(h));
    return *static_cast<Object *>(lib.get(h)->clientData());
  }
  Scene &sc() const
  {
    return *Scene::of(lib, byName("Scene"));
  }
};

double oddNumber(coredcc_test::SplitMix64 &r)
{
  const double odd[] = {0.0, -0.0, 1.0, 1e300, 5e-324, std::numeric_limits<double>::infinity(),
                        -std::numeric_limits<double>::infinity(), 0.1, -1.0 / 3.0};
  return r.below(3) ? double(int64_t(r.next())) * 0x1p-40 : odd[r.below(9)];
}

bool sameChannels(const ChannelBlock &a, const ChannelBlock &b)
{
  bool same = a.size() == b.size();
  for (int i = 0; i < a.size() && same; i++) {
    same = sameBits(a.get(i), b.get(i));
  }
  return same;
}

/* ---- saving and loading ---- */

/* Random documents. What is compared is the data itself, not payloads. */
void testRoundTrip()
{
  coredcc_test::SplitMix64 r(5);
  for (int k = 0; k < 40 * coredcc_test::testScale(); k++) {
    IoDoc d;
    d.addEveryKind();
    const char *const types[] = {kTrsTransform, kFbxTransform, kMatrixTransform, "io.type"};
    std::vector<BlockHandle> objects;
    const int n = int(r.below(12));
    for (int i = 0; i < n; i++) {
      const std::string name = "o" + std::to_string(i);
      Result<BlockHandle> h = newObject(*d.lib, name, types[r.below(4)]);
      must(h.status());
      Object &o = d.obj(*h);
      const TransformType &type = *d.rt.ext<TransformRegistry>().find(o.transformTypeId());
      if (r.below(4)) {
        int32_t first = 0;
        for (const PropertyDef &def : type.channelDefs()) {
          for (int c = 0; c < detail::channelsOf(def.kind()); c++) {
            const bool integer = def.kind() == PropKind::Int;
            const double v = def.kind() == PropKind::Enum ?
                                 double(def.enumMeta()->items[int(r.below(uint32_t(
                                     def.enumMeta()->items.size())))].value) :
                             def.kind() == PropKind::Bool ? double(r.below(2)) :
                             integer ? double(int64_t(r.next() >> 11) - (int64_t(1) << 52)) :
                                       oddNumber(r);
            o.channels.set(first + c, v);
          }
          first += detail::channelsOf(def.kind());
        }
      }
      o.flags = ObjectFlags(r.below(4));
      if (i > 0 && r.below(2)) {
        d.setParent(*h, objects[r.below(uint32_t(i))]);
      }
      if (i > 0 && r.below(3) == 0) {
        d.setData(*h, objects[r.below(uint32_t(i))]);
      }
      if (r.below(5)) {
        must(d.sc().add(*d.lib, *h, int(r.below(uint32_t(d.sc().cells().size() + 1)))));
      }
      else {
        d.lib->setFlag(*h, BlockFlags::FakeUser, true);
      }
      objects.push_back(*h);
    }
    Vector<BlockId> selection;
    for (BlockHandle h : objects) {
      if (r.below(2)) {
        selection.append(d.idOf(h));
      }
    }
    d.sc().setSelection(*d.lib, selection);
    if (n && r.below(2)) {
      must(d.lib->writeRef(d.sc().activeCell(), d.idOf(objects[r.below(uint32_t(n))])));
    }
    d.sc().frame = double(int64_t(r.next())) * 0x1p-50;
    d.sc().fps = 1.0 + r.below(100);

    const Bytes bytes = save(*d.lib);
    test_assert(same(bytes, save(*d.lib)));
    Loaded b(d, bytes);
    test_assert(b.report.missingType == 0 && b.report.missingBlock == 0);
    test_assert(b.report.records == n + 1 && same(bytes, save(b.lib)));
    for (int i = 0; i < n; i++) {
      const Object &x = d.obj(objects[i]), &y = b.obj(("o" + std::to_string(i)).c_str());
      test_assert(x.transformType() == y.transformType() && x.flags == y.flags);
      test_assert(x.parent.target() == y.parent.target() && x.data.target() == y.data.target());
      test_assert(y.data.cell().kind == RefKind::OptStrong && y.parent.cell().kind == RefKind::Weak);
      test_assert(sameChannels(x.channels, y.channels) && y.unread().empty());
    }
    Scene &x = d.sc(), &y = b.sc();
    test_assert(x.cells().size() == y.cells().size() && x.selection().size() == y.selection().size());
    for (int i = 0; i < int(y.cells().size()); i++) {
      test_assert(x.cells()[i].target == y.cells()[i].target);
      test_assert(y.cells()[i].kind == RefKind::OptStrong);
    }
    for (int i = 0; i < int(y.selection().size()); i++) {
      test_assert(x.selection()[i].target == y.selection()[i].target);
      test_assert(y.selection()[i].kind == RefKind::Weak);
    }
    test_assert(x.activeCell().target == y.activeCell().target);
    test_assert(y.activeCell().kind == RefKind::Weak);
    test_assert(sameBits(x.frame, y.frame) && sameBits(x.fps, y.fps));
    test_assert(b.lib.get(b.byName("Scene"))->hasFlag(BlockFlags::Root));
    test_assert(b.lib.validate().ok());
    /* It is a document like any other. */
    d.evaluate();
    must(evaluateScene(b.ctx));
    for (BlockHandle h : d.sc().evaluatedObjects()) {
      ObjectEval f;
      const ObjectEval e = d.result(h);
      test_assert(b.sc().evaluated(b.lib.find(d.idOf(h)), f) && e.status == f.status);
      for (int i = 0; i < 16; i++) {
        const double *m = e.state.world, *w = f.state.world;
        test_assert(sameBits(m[i], w[i]));
      }
    }
    test_assert(b.lib.collect().ok() && b.byName("Scene"));
  }
}

/* What isn't there any more. */
void testGone()
{
  IoDoc d;
  const BlockHandle a = d.object("a"), b = d.object("b", 1.0), c = d.object("c", 2.0);
  d.setParent(a, b);
  d.setParent(c, a);
  d.setData(c, b);
  d.select({a, b, c});
  must(d.lib->writeRef(d.sc().activeCell(), d.idOf(b)));
  must(d.lib->markDeleted(b));
  Loaded l(d, save(*d.lib));
  test_assert(!l.byName("b") && l.sc().cells().size() == 2 && l.sc().objects(l.lib).size() == 2);
  /* Weak references keep their ids. */
  test_assert(l.sc().selection().size() == 3 && l.sc().selected(l.lib).size() == 2);
  test_assert(!l.sc().active(l.lib) && !l.lib.resolveRef(l.obj("a").parent.cell()));
  test_assert(l.obj("c").data.target().isNull() && !l.obj("c").parent.target().isNull());
  /* Places are found without the one that isn't there. */
  test_assert(l.sc().indexOf(l.lib, l.byName("c")) == 1);
  must(l.sc().add(l.lib, l.byName("Scene")));
  must(evaluateScene(l.ctx));
  /* Cells that are null. */
  int cell = 0;
  d.sc().foreachCell([&](RefCell &r) {
    r.target = cell++ == 0 || r.kind == RefKind::Weak ? BlockId() : r.target;
  });
  Loaded nulls(d, save(*d.lib));
  test_assert(nulls.sc().cells().size() == 1 && nulls.sc().objects(nulls.lib)[0] == nulls.byName("c"));
  test_assert(nulls.sc().selection().size() == 0);
  /* A host's number that isn't one doesn't make a file that can't be read. */
  d.sc().frame = std::numeric_limits<double>::quiet_NaN();
  d.sc().fps = std::numeric_limits<double>::infinity();
  Loaded m(d, save(*d.lib));
  test_assert(m.sc().frame == 0.0 && m.sc().fps == 24.0);
}

/* The data of an unregistered block type, and an object without its data in memory. */
void testNoData()
{
  IoDoc d;
  const BlockId id{ClientId::of(kCoredccClient), d.rt.newLocalId()};
  const BlockId id2{ClientId::of(kCoredccClient), d.rt.newLocalId()};
  d.lib->setAuditRelations(false);
  const BlockHandle o = *d.lib->createWithId(TypeId::of(kObjectType), id, "none", nullptr);
  const BlockHandle s = *d.lib->createWithId(TypeId::of(kSceneType), id2, "none", nullptr);
  for (BlockHandle h : {o, s}) {
    test_assert(isError(writePayload(*d.lib, h).status(), StatusCode::FailedPrecondition, "data"));
    test_assert(!duplicateBlocks(*d.lib, std::span<const BlockHandle>(&h, 1)).ok());
    test_assert(d.rt.ext<BlockTypeRegistry>().find(d.lib->get(h)->type())->provider->memSize(
                    *d.lib->get(h)) == 0);
  }
}

/* ---- transform types ---- */

BlockHandle addOfType(IoDoc &d, const char *name)
{
  const BlockHandle h = d.object(name);
  Object &o = d.obj(h);
  o.setTransformType("io.type");
  o.channels = d.rt.ext<TransformRegistry>().find("io.type")->defaults();
  /* on, size, count, mode, extra, at, four, turn */
  const double v[] = {1, 2.5, 7, 3, 8, 9, 10, 11, 12, 13, 14, 15, 16, 0.5, -0.5, 0.5, 2.0};
  for (int i = 0; i < int(std::size(v)); i++) {
    o.channels.set(i, v[i]);
  }
  return h;
}

/* A build without the type keeps its channels, and reads them when it has it. */
void testMissingType()
{
  IoDoc a;
  a.addEveryKind();
  const BlockHandle h = addOfType(a, "odd");
  const Object &from = a.obj(h);
  const BlockHandle child = a.object("child", 3.0);
  a.setParent(child, h);
  const Bytes bytes = save(*a.lib);

  IoDoc b;
  Loaded l(b, bytes);
  Object &o = l.obj("odd");
  test_assert(o.channels.size() == 0 && o.unread().size() == 8);
  test_assert(strcmp(o.transformType().c_str(), "io.type") == 0);
  test_assert(same(save(l.lib), bytes));
  must(evaluateScene(l.ctx));
  ObjectEval e;
  test_assert(l.sc().evaluated(l.byName("odd"), e) && e.status == EvalStatus::MissingModel);
  test_assert(l.sc().evaluated(l.byName("child"), e) && e.status == EvalStatus::Ok);
  /* A copy has them too. */
  const BlockHandle odd = l.byName("odd");
  Result<IdMap> ids = duplicateBlocks(l.lib, std::span<const BlockHandle>(&odd, 1));
  must(ids.status());
  Object &copy = *static_cast<Object *>(l.lib.get(l.lib.find(ids->find(a.idOf(h))))->clientData());
  test_assert(copy.unread().size() == 8 && copy.memSize() > sizeof(Object));
  /* Changing the type, and back as an undo does. */
  ObjectInfo info;
  ChannelBlock none;
  o.read(info, none);
  test_assert(none.size() == 0);
  must(writeTransformType(l.ctx, odd, TypeId::of(kTrsTransform), b.trs->defaults()));
  test_assert(o.unread().empty() && o.channels.size() == b.trs->channelCount());
  {
    Loaded trs(b, save(l.lib));
    test_assert(sameChannels(trs.obj("odd").channels, b.trs->defaults()));
    test_assert(trs.obj("odd").unread().empty());
  }
  must(writeTransformType(l.ctx, odd, TypeId::of("io.type"), none));
  test_assert(o.unread().size() == 8);
  /* The type comes. */
  b.addEveryKind();
  must(evaluateScene(l.ctx));
  test_assert(l.sc().evaluated(odd, e) && e.status == EvalStatus::Ok);
  test_assert(sameChannels(o.channels, from.channels) && o.unread().empty());
  /* Also by a path, or by a save, as the first thing that happens. */
  test_assert(copy.channels.size() == 0);
  Result<PropValue> v = getValue(l.ctx, "scene.objects[\"odd\"].transform.size");
  test_assert(v.ok() && v->asFloat() == 2.5);
  test_assert(same(save(l.lib), bytes) || l.lib.usersOf(odd).size() > 0);
  IoDoc c;
  Loaded late(c, bytes);
  c.addEveryKind();
  v = getValue(late.ctx, "scene.objects[\"odd\"].transform.count");
  test_assert(v.ok() && v->asInt() == 7);
  IoDoc g;
  Loaded later(g, bytes);
  g.addEveryKind();
  test_assert(same(save(later.lib), bytes) && later.obj("odd").unread().empty());
}

/* Channels that are written while the type isn't there replace the file's; they have no names. */
void testNumbers()
{
  IoDoc a, b;
  a.addEveryKind();
  addOfType(a, "odd");
  const Bytes bytes = save(*a.lib);
  Loaded l(b, bytes);
  ChannelBlock three(3);
  three.set(1, -0.0);
  three.set(2, 5.0);
  must(writeTransform(l.ctx, l.byName("odd"), three));
  test_assert(l.obj("odd").unread().empty());
  Loaded again(b, save(l.lib));
  test_assert(sameChannels(again.obj("odd").channels, three) && again.obj("odd").unread().empty());
  test_assert(same(save(again.lib), save(l.lib)));
  b.addEveryKind();
  ObjectInfo info;
  ChannelBlock read;
  l.obj("odd").read(info, read);
  test_assert(sameChannels(read, three));
  /* Nor do those of another count. */
  a.obj(a.byName("odd")).channels = three;
  Loaded wrong(a, save(*a.lib));
  test_assert(sameChannels(wrong.obj("odd").channels, three));
}

/* Entries of a build that doesn't exist: of a type of value, or with references. */
struct Fields {
  IoDoc &d;
  BlockWriter out;
  Vector<BlockEntry> entries;
  Vector<double> numbers;
  RefCell parent{RefKind::Weak}, data{RefKind::OptStrong};

  explicit Fields(IoDoc &doc) : d(doc), out(*doc.lib) {}
  BlockEntry &entry(const char *name, PropValue v)
  {
    BlockEntry e;
    e.name = name;
    e.value = std::move(v);
    entries.append(std::move(e));
    return entries[int(entries.size()) - 1];
  }
  BlockPayload payload(const char *type = kTrsTransform)
  {
    out.bytes().str(type);
    out.bytes().u32(0);
    out.ref(parent);
    out.ref(data);
    out.entries(std::span<const BlockEntry>(entries.data(), size_t(entries.size())));
    out.bytes().array(std::span<const double>(numbers.data(), size_t(numbers.size())));
    Result<BlockPayload> p = PayloadAccess::take(out, 1);
    must(p.status());
    return *p;
  }
};

Status readInto(IoDoc &d, const char *type, const BlockPayload &p, BlockHandle *out = nullptr)
{
  const BlockId id{ClientId::of(kCoredccClient), d.rt.newLocalId()};
  Result<BlockHandle> h = d.lib->createWithId(TypeId::of(type), id, "read", nullptr);
  must(h.status());
  const Status s = readPayload(*d.lib, *h, p);
  test_assert(s.ok() == (d.lib->get(*h)->clientData() != nullptr));
  if (out) {
    *out = *h;
  }
  return s;
}

/* A build where the type has other channels. */
void testOtherVersion()
{
  IoDoc a, b;
  a.addEveryKind();
  const BlockHandle h = addOfType(a, "odd");
  const Bytes bytes = save(*a.lib);
  b.addEveryKind(1);
  Loaded l(b, bytes);
  Object &o = l.obj("odd");
  const auto is = [&](const char *channel, double v) {
    const Result<PropValue> r = getValue(
        l.ctx, ("scene.objects[\"odd\"].transform." + std::string(channel)).c_str());
    return r.ok() && (r->type() == PropValue::Type::Int ? double(r->asInt()) :
                      r->type() == PropValue::Type::Enum ? double(r->asEnum()) :
                                                           r->asFloat()) == v;
  };
  /* 2.5 isn't an Int, THREE isn't there, nor is `extra`. */
  test_assert(is("size", 0.0) && is("count", 7.0) && is("mode", 22.0) && o.unread().size() == 3);
  test_assert(o.channels.get(4) == 11.0 && o.channels.get(5) == 12.0 && o.channels.get(13) == 2.0);
  test_assert(l.lib.validate().ok());
  /* The old build gets them back, and those that were set here. */
  {
    Loaded back(a, save(l.lib));
    Object &x = back.obj("odd");
    test_assert(x.channels.get(0) == 1.0 && x.channels.get(2) == 7.0 && x.unread().size() <= 2);
    test_assert(x.channels.get(4) == 8.0 && x.channels.get(5) == 9.0 && x.channels.get(6) == 10.0);
  }
  must(setValue(l.ctx, "scene.objects[\"odd\"].transform.mode", PropValue::ofString("ONE")));
  must(setValue(l.ctx, "scene.objects[\"odd\"].transform.size", PropValue::ofInt(4)));
  must(setValue(l.ctx, "scene.objects[\"odd\"].transform.count", PropValue::ofFloat(7.5)));
  Loaded back(a, save(l.lib));
  Object &x = back.obj("odd");
  test_assert(x.channels.get(1) == 4.0 && x.channels.get(3) == 1.0);
  test_assert(x.channels.get(2) == 0.0 && x.unread().size() == 1); /* the count isn't an Int now */
  test_assert(x.channels.get(4) == 8.0);
  /* Whole numbers become Ints. */
  a.obj(h).channels.set(1, -3.0);
  a.obj(h).channels.set(3, 2.0);
  Loaded whole(b, save(*a.lib));
  test_assert(whole.obj("odd").channels.get(1) == -3.0 && whole.obj("odd").channels.get(3) == 22.0);
  a.obj(h).channels.set(1, 1e18);
  Loaded large(b, save(*a.lib));
  test_assert(large.obj("odd").channels.get(1) == 0.0 && large.obj("odd").unread().size() == 2);
  /* Numbers that a channel can't hold, and a zero that isn't one of an Int. */
  Fields f(b);
  f.entry("size", PropValue::ofFloat(-0.0));
  f.entry("count", PropValue::ofInt((int64_t(1) << 53) + 1));
  f.entry("size", PropValue::ofInt((int64_t(1) << 53) + 2));
  b.lib->setAuditRelations(false);
  BlockHandle read;
  must(readInto(b, kObjectType, f.payload("io.type"), &read));
  test_assert(sameBits(b.obj(read).channels.get(1), 0.0) && b.obj(read).channels.get(2) == 0.0);
  test_assert(b.obj(read).unread().size() == 2);
}

void testUnread()
{
  IoDoc d;
  d.lib->setAuditRelations(false);
  const BlockHandle target = d.loose("target");
  Fields f(d);
  f.entry("location", PropValue::ofVec3(1, 2, 3));
  f.entry("new", PropValue::ofString("text"));
  BlockEntry &strong = f.entry("strong", PropValue::ofFloat(1));
  strong.refs.append(RefCell(RefKind::Strong));
  strong.refs[0].target = d.idOf(target);
  BlockEntry &raw = f.entry("rotation", PropValue());
  raw.unknownTag = 200;
  raw.raw.append(7);
  f.entry("location", PropValue::ofVec3(4, 5, 6));
  f.entry("scale", PropValue::ofVec2(4, 5));
  /* A name of a channel, with a reference: not what this build means by it. */
  BlockEntry &odd = f.entry("scale", PropValue::ofVec3(9, 9, 9));
  odd.refs.append(RefCell(RefKind::Weak));
  odd.refs[0].target = d.idOf(target);
  BlockHandle h;
  must(readInto(d, kObjectType, f.payload(), &h));
  Object &o = d.obj(h);
  test_assert(o.unread().size() == 5);
  ChannelBlock want = d.trs->defaults();
  put(want, trs::kLocation, double3(4, 5, 6));
  test_assert(sameChannels(o.channels, want));
  Result<BlockPayload> again = writePayload(*d.lib, h);
  must(again.status());
  must(checkPayloadRefs(*d.lib, h, *again));
  test_assert(again->refs.size() == 4);
  BlockHandle h2;
  must(readInto(d, kObjectType, *again, &h2));
  test_assert(d.obj(h2).unread().size() == 5 && d.obj(h2).unread()[2].raw.size() == 1);
  test_assert(same(writePayload(*d.lib, h2)->bytes, again->bytes));
  /* They keep their targets, while they are the object's. */
  d.lib->setFlag(h2, BlockFlags::Root, true);
  must(d.lib->collect().status());
  test_assert(d.lib->get(target) && d.lib->usersOf(target).size() == 1);
  must(writeTransformType(*d.ctx, h2, d.matrix->id(), d.matrix->defaults()));
  test_assert(d.lib->usersOf(target).size() == 0);
  must(writeTransformType(*d.ctx, h2, d.trs->id(), want));
  test_assert(d.lib->usersOf(target).size() == 1 && d.obj(h2).unread().size() == 5);
  d.lib->setFlag(h2, BlockFlags::Root, false);
}

/* ---- duplicating and appending ---- */

void testDuplicate()
{
  IoDoc d;
  const BlockHandle a = d.object("a", 1.0), b = d.object("b", 2.0), data = d.shapeBlock("data", 1.0);
  d.setParent(b, a);
  d.setData(b, data);
  d.setData(a, data);
  d.select({b});
  must(d.lib->writeRef(d.sc().activeCell(), d.idOf(a)));
  d.sc().frame = 7.0;
  d.sc().fps = 25.0;
  d.evaluate();
  const BlockHandle set[] = {d.scene, b};
  const BlockId sceneId = d.idOf(d.scene);
  Result<IdMap> ids = duplicateBlocks(*d.lib, set);
  must(ids.status());
  Scene &copy = *Scene::of(*d.lib, d.lib->find(ids->find(sceneId)));
  const BlockHandle b2 = d.lib->find(ids->find(d.idOf(b)));
  test_assert(copy.objects(*d.lib).size() == 2 && copy.objects(*d.lib)[0] == a);
  test_assert(copy.objects(*d.lib)[1] == b2 && copy.indexOf(*d.lib, b2) == 1);
  test_assert(copy.indexOf(*d.lib, b) == -1 && copy.selected(*d.lib).size() == 1);
  test_assert(copy.selected(*d.lib)[0] == b2 && copy.active(*d.lib) == a && copy.frame == 7.0);
  test_assert(copy.fps == 25.0 && copy.evaluatedObjects().size() == 0);
  test_assert(d.obj(b2).parent.target() == d.idOf(a) && d.obj(b2).data.target() == d.idOf(data));
  test_assert(sameChannels(d.obj(b2).channels, d.obj(b).channels));
  /* They are each on their own. */
  d.obj(b2).channels.set(0, 50.0);
  test_assert(d.obj(b).channels.get(0) == 2.0);
  must(copy.remove(*d.lib, d.idOf(a)).status());
  test_assert(d.sc().objects(*d.lib).size() == 2 && d.lib->validate().ok());
  must(evaluateScene(*d.ctx, d.lib->find(ids->find(sceneId))));
  test_assert(copy.evaluatedObjects().size() == 2); /* with the parent */
  /* A host-owned data block that isn't saved. */
  SaveReport report;
  test_assert(saveLibrary(*d.lib, &report).ok() && report.transient.size() == 1);
}

void testAppend()
{
  IoDoc a, b;
  const BlockHandle x = a.object("x", 1.0), y = a.object("y", 2.0);
  a.setParent(y, x);
  a.select({y});
  must(a.lib->writeRef(a.sc().activeCell(), a.idOf(x)));
  const Bytes bytes = save(*a.lib);
  for (int i = 0; i < 2; i++) {
    Result<AppendResult> r = appendLibrary(*b.ctx, view(bytes));
    must(r.status());
    test_assert(r->report.issues.total == 0 && r->ids.size() == 3);
    const BlockHandle s = b.lib->find(r->ids.find(a.idOf(a.scene)));
    const BlockHandle x2 = b.lib->find(r->ids.find(a.idOf(x)));
    const BlockHandle y2 = b.lib->find(r->ids.find(a.idOf(y)));
    Scene &sc = *Scene::of(*b.lib, s);
    test_assert(sc.objects(*b.lib).size() == 2 && sc.indexOf(*b.lib, y2) == 1);
    test_assert(sc.active(*b.lib) == x2 && sc.selected(*b.lib)[0] == y2);
    test_assert(b.obj(y2).parent.target() == b.idOf(x2));
    test_assert(b.lib->get(s)->hasFlag(BlockFlags::FakeUser));
    must(evaluateScene(*b.ctx, s));
    ObjectEval e;
    test_assert(sc.evaluated(y2, e) && solve::translationOf(e.state.world)[0] == 3.0);
  }
  test_assert(b.sc().objects(*b.lib).size() == 0 && b.lib->validate().ok());
}

/* Two names of a client: one block is in the list once. */
void testAliases()
{
  IoDoc d;
  const char *const aliases[] = {"other.client"};
  must(d.rt.ext<ClientRegistry>().registerClient(kCoredccClient, aliases).status());
  const BlockHandle a = d.object("a", 1.0);
  BlockId alias = d.idOf(a);
  alias.client = ClientId::of("other.client");
  BlockWriter out(*d.lib);
  RefCell cell(RefKind::OptStrong), weak(RefKind::Weak);
  for (int list = 0; list < 2; list++) {
    out.bytes().u32(4);
    cell.target = d.idOf(a);
    out.ref(cell);
    out.ref(RefCell(RefKind::OptStrong));
    cell.target = alias;
    out.ref(cell);
    out.ref(cell);
    if (list == 0) {
      out.ref(weak);
    }
  }
  out.bytes().f64(1);
  out.bytes().f64(2);
  BlockHandle h;
  d.lib->setAuditRelations(false);
  must(readInto(d, kSceneType, *PayloadAccess::take(out, 1), &h));
  const Scene &s = *Scene::of(*d.lib, h);
  test_assert(s.cells().size() == 1 && s.selection().size() == 1 && s.fps == 2.0);
  test_assert(s.objects(*d.lib).size() == 1 && s.selected(*d.lib).size() == 1);
}

/* ---- payloads that are wrong ---- */

void testHostile()
{
  IoDoc d;
  d.addEveryKind();
  d.lib->setAuditRelations(false);
  const BlockHandle a = addOfType(d, "a"), b = d.object("b", 2.0);
  d.setParent(a, b);
  d.setData(a, b);
  d.select({a, b});
  const BlockPayload payloads[] = {*writePayload(*d.lib, a),
                                   *writePayload(*d.lib, b),
                                   *writePayload(*d.lib, d.scene)};
  coredcc_test::SplitMix64 r(9);
  int refused = 0, read = 0;
  for (int k = 0; k < 3; k++) {
    const char *type = k < 2 ? kObjectType : kSceneType;
    const BlockPayload &p = payloads[k];
    for (int n = 0; n < int(p.bytes.size()); n++) {
      BlockPayload cut = p;
      cut.bytes.resize(size_t(n));
      test_assert(isError(readInto(d, type, cut), StatusCode::InvalidArgument));
    }
    BlockPayload more = p;
    more.bytes.append(0);
    test_assert(!readInto(d, type, more).ok());
    for (int i = 0; i < 3000 * coredcc_test::testScale(); i++) {
      BlockPayload bad = p;
      for (uint32_t c = 1 + r.below(3); c > 0; c--) {
        bad.bytes[int(r.below(uint32_t(bad.bytes.size())))] = uint8_t(r.next());
      }
      BlockHandle h;
      const Status s = readInto(d, type, bad, &h);
      (s.ok() ? read : refused)++;
      if (s.ok()) {
        must(writePayload(*d.lib, h).status());
        if (k == 2) {
          must(d.sc().contains(*d.lib, h) ? Status() : evaluateScene(*d.ctx, h));
        }
      }
      must(d.lib->markDeleted(h));
      must(d.lib->destroyFromHistory(h));
    }
  }
  test_assert(refused > 500 && read > 500);
  /* What isn't read leaves no name behind. */
  BlockWriter out(*d.lib);
  out.bytes().str("io.nothing");
  test_assert(!readInto(d, kObjectType, *PayloadAccess::take(out, 1)).ok());
  test_assert(!d.rt.nameOf(TypeId::of("io.nothing")));
  d.lib->setQuarantine(false);
}

void testMemory()
{
  IoDoc d;
  const BlockHandle a = d.object("a");
  const BlockTypeRegistry &types = d.rt.ext<BlockTypeRegistry>();
  const auto size = [&](BlockHandle h) {
    const Datablock &b = *d.lib->get(h);
    return types.find(b.type())->provider->memSize(b);
  };
  test_assert(size(a) >= sizeof(Object) + 10 * sizeof(double));
  const uint64_t one = size(d.scene);
  d.object("b");
  test_assert(size(d.scene) == one + sizeof(RefCell));
  d.select({a});
  test_assert(size(d.scene) == one + 2 * sizeof(RefCell));
}

} // namespace

int main()
{
  testRoundTrip();
  testGone();
  testNoData();
  testMissingType();
  testNumbers();
  testOtherVersion();
  testUnread();
  testDuplicate();
  testAppend();
  testAliases();
  testHostile();
  testMemory();
  return test_end();
}
