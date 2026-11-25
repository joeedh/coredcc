/* What paths can name, without an instance. */
#include "scene_fixture.h"

#include "coredcc/api/static_resolve.h"
#include "coredcc/base/text.h"

#include <string>
#include <vector>

test_init;

using namespace fixture;
using litestl::util::Vector;

namespace {

/* Nodes of a type family: Base <- Left (x: float, next: Base) and Base <- Right (x: Bool). */
const StructId kBase = StructId::of("Base");

PointerAccessor pointerTo(StructId target)
{
  PointerAccessor p;
  p.get = [](const PointerRef &, Context &) { return PointerRef(); };
  p.target = target;
  return p;
}

CollectionAccessor collection()
{
  CollectionAccessor c;
  c.length = [](const PointerRef &, Context &) { return int64_t(0); };
  c.at = [](const PointerRef &, Context &, int64_t) { return PointerRef(); };
  c.lookup = [](const PointerRef &, Context &, const KeySegment &) { return PointerRef(); };
  c.keyOf = [](const PointerRef &, Context &, const PointerRef &) { return KeySegment::ofHandle(1); };
  return c;
}

struct StaticWorld {
  World w;
  StaticResolver r{w.rt};

  StaticWorld()
  {
    ApiRegistry &reg = w.rt.ext<ApiRegistry>();
    const ValueAccessor none{[](const PointerRef &, Context &) { return PropValue(); }, {}};
    ApiStruct base("Base");
    base.add(PropertyDef::intProp("shared"), none);
    base.addPointer(PropertyDef::pointerProp("again", kBase), pointerTo(kBase));
    test_assert(reg.add(std::move(base)).ok());
    ApiStruct left("Left");
    left.parent(kBase);
    left.add(PropertyDef::floatProp("x"), none);
    left.add(PropertyDef::intProp("leftOnly"), none);
    left.addPointer(PropertyDef::pointerProp("way", kMod), pointerTo(kMod));
    left.addPointer(PropertyDef::pointerProp("both", kBase), pointerTo(kBase));
    test_assert(reg.add(std::move(left)).ok());
    ApiStruct right("Right");
    right.parent(kBase);
    right.add(PropertyDef::boolProp("x"), none);
    right.add(PropertyDef::intProp("rightOnly"), none);
    CollectionAccessor way = collection();
    way.itemStruct = kMod;
    way.active = [](const PointerRef &, Context &) { return PointerRef(); };
    right.addCollection(PropertyDef::collectionProp("way", kMod), std::move(way));
    CollectionAccessor both = collection();
    both.itemStruct = kMod;
    right.addCollection(PropertyDef::collectionProp("both", kMod), std::move(both));
    test_assert(reg.add(std::move(right)).ok());
    ApiStruct root("Root");
    root.addPointer(PropertyDef::pointerProp("base", kBase), pointerTo(kBase));
    PointerAccessor refined = pointerTo(StructId());
    refined.refine = [](const PointerRef &) { return StructId::of("Left"); };
    refined.candidates.append(StructId::of("Left"));
    refined.candidates.append(kMod);
    root.addPointer(PropertyDef::pointerProp("refined", StructId()), std::move(refined));
    PointerAccessor doubled = pointerTo(StructId());
    doubled.refine = [](const PointerRef &) { return kBase; };
    doubled.candidates.append(StructId::of("Left"));
    doubled.candidates.append(kBase);
    root.addPointer(PropertyDef::pointerProp("doubled", StructId()), std::move(doubled));
    root.addPointer(PropertyDef::pointerProp("unregistered", StructId::of("Nothing")),
                    pointerTo(StructId::of("Nothing")));
    CollectionAccessor any = collection();
    root.addCollection(PropertyDef::collectionProp("any", StructId::of("Nothing")), std::move(any));
    CollectionAccessor keyed = collection();
    keyed.itemStruct = kBase;
    keyed.staticItem = [](const KeySegment &key) -> Result<StructId> {
      if (key.kind == KeySegment::Kind::StringKey && strcmp(key.text.c_str(), "left") == 0) {
        return StructId::of("Left");
      }
      if (key.kind == KeySegment::Kind::Handle && key.handle == 7) {
        return kMod;
      }
      if (key.kind == KeySegment::Kind::Handle && key.handle == 8) {
        return StructId::of("Nothing");
      }
      return Status::error(StatusCode::NotFound, "no such thing");
    };
    root.addCollection(PropertyDef::collectionProp("keyed", kBase), std::move(keyed));
    Result<const ApiStruct *> added = reg.add(std::move(root));
    test_assert(added.ok());
    if (!added.ok()) {
      fprintf(stderr, "%s\n", added.status().message());
    }
  }

  /* The targets, as "kind struct.member". */
  std::string names(StructId from, const char *path)
  {
    Result<StaticResolution> res = r.resolve(from, path);
    if (!res.ok()) {
      return std::string("! ") + res.status().message();
    }
    std::string out;
    for (const StaticTarget &t : res->targets) {
      static const char *const kinds[] = {"struct", "member", "component", "key", "length"};
      out += out.empty() ? "" : ", ";
      out += std::string(kinds[int(t.kind)]) + " " +
             w.rt.ext<ApiRegistry>().find(t.type)->name().c_str();
      out += t.member ? std::string(".") + t.member->name() : "";
    }
    return out + (res->unknown ? " ?" : "");
  }
  bool is(StructId from, const char *path, const char *want)
  {
    const std::string got = names(from, path);
    if (got != want) {
      fprintf(stderr, "%s\ngot  %s\nwant %s\n", path, got.c_str(), want);
    }
    return got == want;
  }
  std::string suggestion(StructId from, const char *path)
  {
    Result<StaticResolution> res = r.resolve(from, path);
    test_assert(!res.ok());
    return res.status().suggestions().size() ? res.status().suggestions()[0].c_str() : "";
  }
};

const StructId kRoot = StructId::of("Root");

} // namespace

static void test_paths()
{
  StaticWorld s;
  test_assert(s.is(kScene, "objects", "member Scene.objects"));
  test_assert(s.is(kScene, "objects[0]", "struct Object"));
  test_assert(s.is(kScene, "objects[\"x\"].location", "member Object.location"));
  test_assert(s.is(kScene, "objects[#5].location[2]", "component Object.location"));
  test_assert(s.is(kScene, "objects.active.flags[HIDE]", "key Object.flags"));
  test_assert(s.is(kScene, "objects.active.mode[EDIT]", "key Object.mode"));
  test_assert(s.is(kScene, "objects.length", "length Scene.objects"));
  test_assert(s.is(kScene, "tool", "member Scene.tool"));
  test_assert(s.is(kScene, "tool.brushes.active.size", "member Brush.size"));
  test_assert(s.is(kScene, "objects.active.data.radius", "member Mesh.radius"));
  /* Through a pointer to the base. */
  test_assert(s.is(kScene, "objects.active.anyData.radius", "member Mesh.radius"));
  test_assert(s.is(kScene, "objects.active.mod.strength", "member Mod.strength"));
}

static void test_errors()
{
  StaticWorld s;
  test_assert(s.is(kScene, "nothing", "! no member 'nothing' in Scene (at offset 0)"));
  test_assert(s.is(kScene, "objects[0].nothing", "! no member 'nothing' in Object (at offset 10)"));
  test_assert(s.is(kScene, "objects.activ", "! collection 'objects' has no member 'activ' (only "
                                            ".active and .length) (at offset 7)"));
  test_assert(s.suggestion(kScene, "objects.activ") == "active");
  test_assert(s.suggestion(kScene, "objects[0].locaton") == "location");
  test_assert(s.is(kScene, "objects.length.x", "! nothing can follow '.length' (at offset 14)"));
  test_assert(s.is(kScene, "objects[KEY]", "! 'objects' takes [index], [\"key\"], [#handle], "
                                           ".active or .length (at offset 7)"));
  test_assert(s.is(kScene, "objects[0].location[3]",
                   "! 'location' has 3 components (at offset 19)"));
  test_assert(s.is(kScene, "objects[0].location[0].x",
                   "! nothing can follow 'location[...]' (at offset 22)"));
  test_assert(s.is(kScene, "objects[0].location.x",
                   "! 'location' (Vec3) has no members or components (at offset 19)"));
  test_assert(s.is(kScene, "objects[0].mode[0]",
                   "! 'mode' (Enum) has no members or components (at offset 15)"));
  test_assert(s.is(kScene, "objects[0].mode[NONE]", "! 'mode' has no key 'NONE' (at offset 15)"));
  test_assert(s.is(kScene, "tool.locked[true]",
                   "! 'locked' (Bool) has no members or components (at offset 11)"));
  test_assert(s.is(kScene, "tool[0]", "! a subscript needs a collection or vector (at offset 4)"));
  test_assert(s.is(kRoot, "any.active", "! 'any' has no active item (at offset 3)"));
  test_assert(!s.r.resolve(kScene, "a..b").ok());
  /* The paths that resolve() refuses for the same reasons. */
  Context ctx = s.w.context();
  for (const char *path : {"nothing", "objects[0].nothing", "objects.activ", "objects.length.x",
                           "objects[KEY]", "objects[0].location[3]", "objects[0].location[0].x",
                           "objects[0].location.x", "objects[0].mode[0]", "objects[0].mode[NONE]",
                           "tool[0]"})
  {
    const Status a = resolve(ctx, path).status(), b = s.r.resolve(kScene, path).status();
    test_assert(a.code() == b.code() && strcmp(a.message(), b.message()) == 0);
  }
}

static void test_several()
{
  StaticWorld s;
  test_assert(s.is(kRoot, "base.shared", "member Base.shared")); /* once, though three have it */
  test_assert(s.is(kRoot, "base.x", "member Left.x, member Right.x"));
  test_assert(s.is(kRoot, "base.leftOnly", "member Left.leftOnly"));
  test_assert(s.is(kRoot, "base.again.again.rightOnly", "member Right.rightOnly"));
  /* A pointer and a collection of one name: they take one segment, and two. */
  test_assert(s.is(kRoot, "base.way", "member Left.way, member Right.way"));
  test_assert(s.is(kRoot, "base.way.strength", "member Mod.strength"));
  test_assert(s.is(kRoot, "base.way.active.strength", "member Mod.strength"));
  test_assert(s.is(kRoot, "base.way[0].strength", "member Mod.strength"));
  test_assert(s.is(kRoot, "base.way.length", "length Right.way"));
  test_assert(s.is(kRoot, "refined.leftOnly", "member Left.leftOnly"));
  test_assert(s.is(kRoot, "refined.strength", "member Mod.strength"));
  test_assert(s.is(kRoot, "base.nothing",
                   "! no member 'nothing' in any of Base, Left, Right (at offset 4)"));
  test_assert(s.is(kRoot, "refined.nothing",
                   "! no member 'nothing' in any of Left, Mod (at offset 7)"));
  test_assert(s.suggestion(kRoot, "base.rightOnl") == "rightOnly");
  test_assert(s.suggestion(kRoot, "base.leftOnl") == "leftOnly");
  test_assert(s.suggestion(kRoot, "base.share") == "shared");
  /* The error of the furthest. */
  test_assert(s.is(kRoot, "base.way.active.x", "! no member 'x' in Mod (at offset 15)"));
  test_assert(s.is(kRoot, "base.x.y", "! 'x' (Float) has no members or components (at offset 6)"));
  /* One of two that came as far: the collection's, which was first. */
  test_assert(s.is(kRoot, "base.both.nothing",
                   "! collection 'both' has no member 'nothing' (only .active and .length) (at "
                   "offset 9)"));
  /* Many. */
  ApiRegistry &reg = s.w.rt.ext<ApiRegistry>();
  for (int i = 0; i < 5; i++) {
    ApiStruct more("More" + std::to_string(i));
    more.parent(kBase);
    test_assert(reg.add(std::move(more)).ok());
  }
  /* Registered since, and found. */
  test_assert(s.is(kRoot, "base.nothing",
                   "! no member 'nothing' in any of Base, Left, Right, More0 and 4 more (at "
                   "offset 4)"));
  test_assert(s.r.withSubtypes(kBase).structs.size() == 8 &&
              s.r.withSubtypes(kBase).structs[0] == kBase);
  test_assert(s.r.withSubtypes(StructId::of("Left")).structs.size() == 1);
}

static void test_unknown()
{
  StaticWorld s;
  test_assert(s.is(kRoot, "unregistered.a.b[0]", " ?"));
  test_assert(s.is(kRoot, "unregistered", "member Root.unregistered"));
  test_assert(s.is(kRoot, "any[0].x", " ?") && s.is(kRoot, "any[\"a\"]", " ?"));
  test_assert(s.is(kRoot, "any.length", "length Root.any"));
  test_assert(s.is(StructId::of("Nothing"), "a", " ?"));
  StaticStructs none;
  test_assert(!s.r.resolve(none, *parsePath("a")).ok() && !s.r.resolve(none, Path()).ok());
  /* Some are known. */
  StaticStructs some = s.r.only(kMod);
  some.unknown = true;
  Result<StaticResolution> r = s.r.resolve(some, *parsePath("strength"));
  test_assert(r.ok() && r->unknown && r->targets.size() == 1);
  r = s.r.resolve(some, *parsePath("other"));
  test_assert(r.ok() && r->unknown && r->targets.size() == 0);
  test_assert(s.r.only(StructId::of("Nothing")).unknown && s.r.only(kMod).structs.size() == 1);
}

static void test_items()
{
  StaticWorld s;
  test_assert(s.is(kRoot, "keyed[\"left\"].leftOnly", "member Left.leftOnly"));
  test_assert(s.is(kRoot, "keyed[#7].strength", "member Mod.strength"));
  test_assert(s.is(kRoot, "keyed[\"left\"].rightOnly",
                   "! no member 'rightOnly' in Left (at offset 13)"));
  test_assert(s.is(kRoot, "keyed[\"right\"].x", "! no such thing (at offset 5)"));
  test_assert(s.is(kRoot, "keyed[#8].x", " ?"));
  /* A position says nothing. */
  test_assert(s.is(kRoot, "keyed[0].rightOnly", "member Right.rightOnly"));
}

static void test_structs()
{
  StaticWorld s;
  const auto structs = [&](StructId from, const char *path) {
    Result<StaticStructs> r =
        s.r.resolveStructs(s.r.only(from), *path ? *parsePath(path) : Path());
    if (!r.ok()) {
      return std::string("! ") + r.status().message();
    }
    std::string out;
    for (StructId id : r->structs) {
      out += out.empty() ? "" : " ";
      out += s.w.rt.ext<ApiRegistry>().find(id)->name().c_str();
    }
    return out + (r->unknown ? "?" : "");
  };
  test_assert(structs(kRoot, "") == "Root" && structs(kRoot, "base") == "Base Left Right");
  test_assert(structs(kRoot, "doubled") == "Left Base Right");
  test_assert(structs(kRoot, "refined") == "Left Mod" && structs(kRoot, "unregistered") == "?");
  test_assert(structs(kScene, "objects[0]") == "Object");
  test_assert(structs(kScene, "objects[0].location") ==
              "! 'objects[0].location' isn't a struct");
  test_assert(structs(kScene, "objects") == "! 'objects' isn't a struct");
  test_assert(structs(kScene, "nothing") == "! no member 'nothing' in Scene (at offset 0)");
  /* A struct in one of the cases is enough. */
  test_assert(structs(kRoot, "base.way") == "Mod");
}

/* Whatever resolves in a document resolves statically, to the same. */
static void test_against_resolve()
{
  StaticWorld s;
  Context ctx = s.w.context();
  std::vector<std::string> paths = {""};
  int checked = 0;
  for (size_t i = 0; i < paths.size() && paths.size() < 2000; i++) {
    const std::string path = paths[i];
    const StructId type = path.empty() ? kScene : StructId();
    Result<ResolvedPath> rp = path.empty() ? Result<ResolvedPath>(ResolvedPath()) :
                                             resolve(ctx, path);
    if (!rp.ok()) {
      continue;
    }
    StructId in = type;
    if (!path.empty()) {
      Result<StaticResolution> r = s.r.resolve(kScene, path);
      test_assert(r.ok());
      bool found = false;
      for (const StaticTarget &t : r->targets) {
        found |= t.kind == rp->kind && t.member == rp->member &&
                 (rp->kind != ResolvedPath::Kind::Struct || t.type == rp->ptr.type);
      }
      if (!found) {
        fprintf(stderr, "%s: %s\n", path.c_str(), s.names(kScene, path.c_str()).c_str());
      }
      test_assert(found);
      checked++;
      in = rp->kind == ResolvedPath::Kind::Struct ? rp->ptr.type : StructId();
    }
    if (!in || path.size() > 60) {
      continue;
    }
    for (const ApiStruct *st = s.w.rt.ext<ApiRegistry>().find(in); st;
         st = s.w.rt.ext<ApiRegistry>().find(st->parentId()))
    {
      for (int m = 0; m < st->memberCount(); m++) {
        const std::string next = (path.empty() ? "" : path + ".") + st->member(m).name();
        paths.push_back(next);
        for (const char *sub : {"[0]", "[1]", "[5]", ".active", ".length", "[\"Cube\"]", "[\"A\"]",
                                "[HIDE]", "[EDIT]"})
        {
          paths.push_back(next + sub);
        }
        if (st->member(m).kind() == MemberKind::Pointer) {
          paths.push_back(next + ".radius");
          paths.push_back(next + ".size");
          paths.push_back(next + ".strength");
        }
      }
    }
  }
  test_assert(checked > 60);
}

int main()
{
  test_paths();
  test_errors();
  test_several();
  test_unknown();
  test_items();
  test_structs();
  test_against_resolve();
  return test_end();
}
