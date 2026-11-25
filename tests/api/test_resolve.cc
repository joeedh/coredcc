#include "scene_fixture.h"

#include "coredcc/parse/path.h"

#include <cmath>
#include <string_view>

test_init;

using namespace fixture;

static Path P(const char *text)
{
  Result<Path> p = parsePath(text);
  test_assert(p.ok());
  return p.ok() ? *p : Path();
}

static void test_grammar_forms()
{
  World w;
  Context ctx = w.context();
  w.object(0).location[2] = 7;
  test_assert(*getValue(ctx, "objects[0].location[2]") == PropValue::ofFloat(7));
  test_assert(*getValue(ctx, "objects[\"Cube\"].location") == PropValue::ofVec3(0, 0, 7));
  test_assert(*getValue(ctx, "objects.length") == PropValue::ofInt(2));
  test_assert(*getValue(ctx, "objects.active.name") == PropValue::ofString("Cube"));
  test_assert(*getValue(ctx, "objects.active.data.radius") == PropValue::ofFloat(1));
  test_assert(*getValue(ctx, "tool.strength") == PropValue::ofFloat(0.5));

  /* Enum and flag keys read as Bools and write the whole member. */
  test_assert(*getValue(ctx, "objects[1].mode[OBJECT]") == PropValue::ofBool(true));
  test_assert(setValue(ctx, "objects[1].mode[EDIT]", PropValue::ofBool(true)).ok());
  test_assert(w.object(1).mode == 1);
  test_assert(!setValue(ctx, "objects[1].mode[EDIT]", PropValue::ofBool(false)).ok());
  test_assert(setValue(ctx, "objects[1].flags[HIDE]", PropValue::ofBool(true)).ok());
  test_assert(setValue(ctx, "objects[1].flags[SELECT]", PropValue::ofBool(true)).ok());
  test_assert(w.object(1).flags == 3);
  test_assert(setValue(ctx, "objects[1].flags[HIDE]", PropValue::ofBool(false)).ok());
  test_assert(w.object(1).flags == 1);
  test_assert(*getValue(ctx, "objects[1].flags[HIDE]") == PropValue::ofBool(false));

  /* Component writes. */
  test_assert(setValue(ctx, "objects[\"Sphere\"].location[1]", PropValue::ofFloat(2.5)).ok());
  test_assert(w.object(1).location[1] == 2.5f);
  test_assert(setValue(ctx, "objects[\"Sphere\"].location[0]", PropValue::ofInt(4)).ok());
  test_assert(w.object(1).location[0] == 4.0f);

  /* A Pointer member reads as the address of its target block. */
  Result<PropValue> data = getValue(ctx, "objects[0].data");
  test_assert(data.ok() && data->tryPointer()->owner == OwnerKey::ofBlock(w.meshId(0)));
  test_assert(data->tryPointer()->type == kMesh);
  /* A struct path reads as its own address. */
  Result<PropValue> ob = getValue(ctx, "objects[\"Sphere\"]");
  test_assert(ob.ok() && ob->tryPointer()->owner == OwnerKey::ofBlock(w.objectId(1)));
  test_assert(w.rt.validateAll().ok());
}

static void test_errors()
{
  World w;
  Context ctx = w.context();
  Result<PropValue> r = getValue(ctx, "objects[0].locaton");
  test_assert(r.status().code() == StatusCode::NotFound);
  test_assert(hasMsg(r.status(), "no member 'locaton' in Object (at offset 10)"));
  test_assert(r.status().suggestions().size() == 1 &&
              strcmp(r.status().suggestions()[0].c_str(), "location") == 0);
  test_assert(getValue(ctx, "objects[5].name").status().code() == StatusCode::OutOfRange);
  test_assert(getValue(ctx, "objects[\"Nope\"].name").status().code() == StatusCode::NotFound);
  test_assert(getValue(ctx, "objects[0].location[3]").status().code() == StatusCode::OutOfRange);
  test_assert(getValue(ctx, "objects[0].mode[NOPE]").status().code() == StatusCode::NotFound);
  test_assert(!getValue(ctx, "objects[0].location.x").ok());
  test_assert(!getValue(ctx, "objects[0].location[0][0]").ok());
  test_assert(!getValue(ctx, "objects.length.x").ok());
  test_assert(!getValue(ctx, "tool[0]").ok());
  test_assert(getValue(ctx, "objects").status().code() == StatusCode::Unsupported);
  test_assert(!getValue(ctx, "objects[").ok()); /* a parse error */
  w.scene.active = -1;
  test_assert(getValue(ctx, "objects.active.name").status().code() == StatusCode::NotFound);
  /* A null pointer reads as the null Pointer, and can't be walked. */
  test_assert(w.lib.writeRef(w.object(0).mesh, BlockId{}).ok());
  test_assert(*getValue(ctx, "objects[0].data") == PropValue::nullPointer());
  test_assert(hasMsg(getValue(ctx, "objects[0].data.radius").status(), "pointer 'data' is null"));
}

static void test_write_path()
{
  World w;
  Context ctx = w.context();
  ChangeBus &bus = w.rt.ext<ChangeBus>();
  g_radiusUpdates = 0;

  /* Clamped, then the update hook, then a change on the mesh. */
  test_assert(setValue(ctx, "objects[0].data.radius", PropValue::ofFloat(20)).ok());
  test_assert(w.mesh(0).radius == 10 && g_radiusUpdates == 1);
  test_assert(bus.pendingCount() == 1);

  /* Read-only members, and paths that aren't members. */
  test_assert(setValue(ctx, "tool.version", PropValue::ofInt(9)).code() ==
              StatusCode::FailedPrecondition);
  test_assert(setValue(ctx, "objects[0].name", PropValue::ofString("x")).code() ==
              StatusCode::FailedPrecondition);
  test_assert(setValue(ctx, "objects.length", PropValue::ofInt(9)).code() ==
              StatusCode::FailedPrecondition);
  test_assert(setValue(ctx, "objects[0]", PropValue::ofInt(9)).code() ==
              StatusCode::FailedPrecondition);
  test_assert(w.tool.version == 3 && g_radiusUpdates == 1);
  test_assert(setValue(ctx, "tool.strength", PropValue::ofString("x")).code() ==
              StatusCode::TypeMismatch);

  /* getMeta reports read-only and the dynamic range. */
  Result<PropertyDef> meta = getMeta(ctx, "tool.version");
  test_assert(meta.ok() && meta->hasAccess(PropAccess::ReadOnly));
  meta = getMeta(ctx, "objects[0].name");
  test_assert(meta.ok() && meta->hasAccess(PropAccess::ReadOnly));
  meta = getMeta(ctx, "tool.pick");
  test_assert(meta.ok() && meta->numeric()->hardMax == 3);
  meta = getMeta(ctx, "objects.length");
  test_assert(meta.ok() && meta->kind() == PropKind::Int && meta->hasAccess(PropAccess::ReadOnly));
  meta = getMeta(ctx, "objects[0].location[1]");
  test_assert(meta.ok() && meta->kind() == PropKind::Vec3);

  /* The Context-aware dynamic hook clamps writes. */
  test_assert(setValue(ctx, "tool.pick", PropValue::ofInt(10)).ok() && w.tool.pick == 3);
  test_assert(setValue(ctx, "tool.count", PropValue::ofInt(2)).ok());
  test_assert(setValue(ctx, "tool.pick", PropValue::ofInt(10)).ok() && w.tool.pick == 1);
}

static void test_canonical_paths()
{
  World w;
  Context ctx = w.context();

  /* `active` and indices become keys, rooted at the innermost block. */
  Result<StableRef> loc = canonicalize(ctx, P("objects.active.location"));
  test_assert(loc.ok() && loc->owner == OwnerKey::ofBlock(w.objectId(0)));
  test_assert(loc->key.size() == 1 && strcmp(loc->key[0].text.c_str(), "location") == 0);
  Result<StableRef> byIndex = canonicalize(ctx, P("objects[0].location[2]"));
  test_assert(byIndex.ok() && *byIndex == *loc); /* components widen to the member */

  /* The hop through `data` is dropped: the mesh owns the member. */
  Result<StableRef> radius = canonicalize(ctx, P("objects.active.data.radius"));
  test_assert(radius.ok() && radius->owner == OwnerKey::ofBlock(w.meshId(0)));
  test_assert(radius->key.size() == 1 && strcmp(radius->key[0].text.c_str(), "radius") == 0);

  /* Runtime owners root their own members. */
  Result<StableRef> strength = canonicalize(ctx, P("tool.strength"));
  test_assert(strength.ok() && strength->owner.kind == OwnerKey::Kind::Runtime);
  test_assert(strength->key.size() == 1);

  /* The canonical path survives a change of `active`... */
  w.scene.active = 1;
  Result<ResolvedPath> again = resolve(ctx, *loc);
  test_assert(again.ok() && again->ptr.data == &w.object(0));
  w.object(0).location[0] = 3;
  test_assert(*getValue(ctx, *again) == PropValue::ofVec3(3, 0, 0));
  test_assert(setValue(ctx, *again, PropValue::ofVec3(1, 2, 3)).ok());
  test_assert(w.object(0).location[1] == 2 && w.object(1).location[1] == 0);
  /* ...and a reassigned `data`. */
  test_assert(w.lib.writeRef(w.object(0).mesh, w.meshId(1)).ok());
  Result<ResolvedPath> r2 = resolve(ctx, *radius);
  test_assert(r2.ok() && r2->ptr.data == &w.mesh(0));

  /* Struct addresses within a runtime owner keep their keys. */
  Result<StableRef> scene = canonicalize(ctx, P("objects[\"Sphere\"]"));
  test_assert(scene.ok() && scene->owner == OwnerKey::ofBlock(w.objectId(1)) &&
              scene->key.size() == 0 && scene->type == kObject);
  Result<ResolvedPath> sp = resolve(ctx, *scene);
  test_assert(sp.ok() && sp->kind == ResolvedPath::Kind::Struct && sp->ptr.data == &w.object(1));
  StableRef wrongType = *scene;
  wrongType.type = kMesh;
  test_assert(resolve(ctx, wrongType).status().code() == StatusCode::TypeMismatch);

  /* `.length` isn't addressable state; a Struct path from a runtime owner is. */
  Result<StableRef> len = canonicalize(ctx, P("objects.length"));
  test_assert(len.ok() && len->owner.kind == OwnerKey::Kind::Runtime);
}

static void test_dangling_refs()
{
  World w;
  Context ctx = w.context();
  Result<StableRef> radius = canonicalize(ctx, P("objects[0].data.radius"));
  test_assert(radius.ok());
  /* Repoint the object, then delete the mesh: the ref fails cleanly. */
  test_assert(w.lib.writeRef(w.object(0).mesh, w.meshId(1)).ok());
  test_assert(w.lib.markDeleted(w.meshes[0]).ok());
  Result<ResolvedPath> r = resolve(ctx, *radius);
  test_assert(r.status().code() == StatusCode::NotFound);
  test_assert(hasMsg(r.status(), "the owner is gone"));

  /* A key that no longer matches. */
  StableRef byKey;
  byKey.owner = OwnerKey::ofRuntime(TypeId::of("owner:scene").value);
  byKey.key.append(KeySegment::member("objects"));
  byKey.key.append(KeySegment::stringKey("Gone"));
  byKey.key.append(KeySegment::member("location"));
  test_assert(resolve(ctx, byKey).status().code() == StatusCode::NotFound);
  StableRef unknown;
  unknown.owner = OwnerKey::ofRuntime(TypeId::of("owner:nope").value);
  test_assert(resolve(ctx, unknown).status().code() == StatusCode::NotFound);
  test_assert(resolve(ctx, StableRef()).status().code() == StatusCode::InvalidArgument);

  /* A path through the deleted block. */
  test_assert(w.lib.writeRef(w.object(1).mesh, w.meshId(0)).ok());
  test_assert(getValue(ctx, "objects[1].data.radius").status().code() == StatusCode::NotFound);
}

static void test_pointer_writes()
{
  World w;
  Context ctx = w.context();
  const uint64_t epoch = w.lib.epoch();
  Result<PropValue> cubeMesh = getValue(ctx, "objects[0].data");
  test_assert(cubeMesh.ok());
  test_assert(setValue(ctx, "objects[1].data", *cubeMesh).ok());
  test_assert(w.lib.resolveRef(w.object(1).mesh) == w.meshes[0]);
  test_assert(w.lib.epoch() != epoch);
  /* The wrong struct type, and a non-Pointer value. */
  Result<PropValue> anObject = getValue(ctx, "objects[0]");
  test_assert(setValue(ctx, "objects[1].data", *anObject).code() == StatusCode::TypeMismatch);
  test_assert(setValue(ctx, "objects[1].data", PropValue::ofInt(1)).code() ==
              StatusCode::TypeMismatch);
  /* Null clears it. */
  test_assert(setValue(ctx, "objects[1].data", PropValue::nullPointer()).ok());
  test_assert(!w.lib.resolveRef(w.object(1).mesh));
  /* A pointer with no setter. */
  test_assert(setValue(ctx, "tool", PropValue::nullPointer()).code() ==
              StatusCode::FailedPrecondition);
}

static void test_prefix_and_batch()
{
  World w;
  Context ctx = w.context();
  ctx.setPrefix(P("objects[\"Sphere\"]"));
  test_assert(setValue(ctx, "location[2]", PropValue::ofFloat(9)).ok());
  test_assert(w.object(1).location[2] == 9);
  Result<StableRef> c = canonicalize(ctx, P("location"));
  test_assert(c.ok() && c->owner == OwnerKey::ofBlock(w.objectId(1)));
  ctx.setPrefix(P("objects[0].location"));
  test_assert(hasMsg(getValue(ctx, "x").status(), "prefix must name a struct"));
  /* A pointer is its target. */
  ctx.setPrefix(*parsePath("objects[1].data"));
  test_assert(getValue(ctx, "radius").ok());
  test_assert(canonicalize(ctx, *parsePath("radius"))->owner == OwnerKey::ofBlock(w.meshId(1)));
  w.object(1).mesh.target = BlockId();
  w.lib.noteRefsChanged();
  test_assert(getValue(ctx, "radius").status().code() == StatusCode::NotFound &&
              hasMsg(getValue(ctx, "radius").status(), "pointer 'data' is null") &&
              hasMsg(getValue(ctx, "radius").status(), "offset 10"));
  ctx.setPrefix(*parsePath("tool.activeBrushNoAddress"));
  test_assert(getValue(ctx, "size").status().code() == StatusCode::Unsupported);
  w.object(1).mesh.target = w.meshId(1);
  w.lib.noteRefsChanged();
  ctx.clearPrefix();

  const std::string_view paths[] = {"tool.strength", "objects.bogus", "objects.length"};
  litestl::util::Vector<PropValue> values;
  litestl::util::Vector<Status> errors;
  getValues(ctx, paths, values, errors);
  {
    std::vector<Path> parsed;
    std::vector<const Path *> pointers;
    for (std::string_view p : paths) {
      parsed.push_back(parsePath(p).valueOr(Path()));
    }
    for (const Path &p : parsed) {
      pointers.push_back(&p);
    }
    litestl::util::Vector<PropValue> same = values;
    litestl::util::Vector<Status> sameErrors;
    same.append(PropValue());
    getValues(ctx, pointers, same, sameErrors);
    test_assert(same.size() == values.size() && sameErrors.size() == errors.size());
    for (int i = 0; i < int(values.size()); i++) {
      test_assert(same[i] == values[i] && sameErrors[i].ok() == errors[i].ok());
    }
  }
  test_assert(values.size() == 3 && errors.size() == 3);
  test_assert(values[0] == PropValue::ofFloat(0.5) && errors[0].ok());
  test_assert(values[1].isNone() && errors[1].code() == StatusCode::NotFound);
  test_assert(values[2] == PropValue::ofInt(2));
}

static void test_registry_checks()
{
  Runtime rt;
  ApiRegistry &reg = rt.ext<ApiRegistry>();
  ValueAccessor getOnly{[](const PointerRef &, Context &) { return PropValue::ofInt(0); }, {}};

  ApiStruct dup("Dup");
  dup.add(PropertyDef::intProp("a"), getOnly);
  dup.add(PropertyDef::intProp("a"), getOnly);
  test_assert(hasMsg(reg.add(std::move(dup)).status(), "defined twice"));

  ApiStruct reserved("Reserved");
  reserved.add(PropertyDef::intProp("length"), getOnly);
  test_assert(hasMsg(reg.add(std::move(reserved)).status(), "reserved"));

  ApiStruct noGet("NoGet");
  noGet.add(PropertyDef::intProp("a"), ValueAccessor{});
  test_assert(hasMsg(reg.add(std::move(noGet)).status(), "no getter"));

  ApiStruct noKey("NoKey");
  CollectionAccessor c;
  c.length = [](const PointerRef &, Context &) { return int64_t(0); };
  c.at = [](const PointerRef &, Context &, int64_t) { return PointerRef(); };
  c.lookup = [](const PointerRef &, Context &, const KeySegment &) { return PointerRef(); };
  noKey.addCollection(PropertyDef::collectionProp("items", kObject), c);
  test_assert(hasMsg(reg.add(std::move(noKey)).status(), "keyOf"));

  ApiStruct noCandidates("NoCandidates");
  PointerAccessor pa;
  pa.get = [](const PointerRef &, Context &) { return PointerRef(); };
  pa.refine = [](const PointerRef &) { return kMesh; };
  noCandidates.addPointer(PropertyDef::pointerProp("p", StructId()), pa);
  test_assert(hasMsg(reg.add(std::move(noCandidates)).status(), "declared candidates"));

  ApiStruct ok("Ok");
  ok.add(PropertyDef::intProp("a"), getOnly);
  test_assert(reg.add(std::move(ok)).ok());
  ApiStruct again("Ok");
  test_assert(reg.add(std::move(again)).status().code() == StatusCode::AlreadyExists);

  /* Runtime owners are unique by name. */
  int x = 0;
  test_assert(reg.addRuntimeOwner("prefs", StructId::of("Ok"), [&x]() { return (void *)&x; }).ok());
  test_assert(reg.addRuntimeOwner("prefs", StructId::of("Ok"), [&x]() { return (void *)&x; })
                  .status()
                  .code() == StatusCode::AlreadyExists);
  test_assert(reg.runtimeOwner("prefs").data == &x);
  test_assert(reg.runtimeOwner("none").isNull());
  test_assert(rt.validateAll().ok());
}

/* Parents contribute members, and suggestions come from the whole chain. */
static void test_parents()
{
  Runtime rt;
  ApiRegistry &reg = rt.ext<ApiRegistry>();
  struct Base {
    int a = 1;
  };
  struct Derived : Base {
    int b = 2;
  };
  ApiStruct base("Base");
  base.add(PropertyDef::intProp("alpha"),
           ValueAccessor{[](const PointerRef &s, Context &) {
                           return PropValue::ofInt(static_cast<Base *>(s.data)->a);
                         },
                         {}});
  test_assert(reg.add(std::move(base)).ok());
  ApiStruct derived("Derived");
  derived.parent(StructId::of("Base"));
  derived.add(PropertyDef::intProp("beta"),
              ValueAccessor{[](const PointerRef &s, Context &) {
                              return PropValue::ofInt(static_cast<Derived *>(s.data)->b);
                            },
                            {}});
  test_assert(reg.add(std::move(derived)).ok());
  Derived d;
  Result<OwnerKey> k =
      reg.addRuntimeOwner("d", StructId::of("Derived"), [&d]() { return (void *)&d; });
  test_assert(k.ok());
  Context ctx(&rt, nullptr, reg.runtimeOwner("d"));
  test_assert(*getValue(ctx, "alpha") == PropValue::ofInt(1));
  test_assert(*getValue(ctx, "beta") == PropValue::ofInt(2));
  Result<PropValue> miss = getValue(ctx, "alpah");
  test_assert(miss.status().suggestions().size() == 1);
}

/* Review findings for lib/api (2026-11-21). */
static void test_owner_crossings()
{
  World w;
  Context ctx = w.context();
  /* A fixed nested struct keeps its hop; crossing into a block's non-root struct is
   * refused rather than given a canonical path naming the wrong struct (B1). */
  Result<StableRef> mod = canonicalize(ctx, P("objects[0].mod.strength"));
  test_assert(mod.ok() && mod->owner == OwnerKey::ofBlock(w.objectId(0)) && mod->key.size() == 2);
  test_assert(setValue(ctx, *resolve(ctx, *mod), PropValue::ofFloat(0.75)).ok());
  test_assert(w.object(0).mod.strength == 0.75);
  Result<ResolvedPath> bad = resolve(ctx, P("firstMod.strength"));
  test_assert(bad.status().code() == StatusCode::Unsupported);
  test_assert(hasMsg(bad.status(), "isn't the owner's root"));
  test_assert(getValue(ctx, "firstMod").status().code() == StatusCode::Unsupported);

  /* A settable pointer inside one owner reads as its target's own address (B2). */
  Result<PropValue> old = getValue(ctx, "tool.activeBrush");
  test_assert(old.ok());
  const StableRef *a = old->tryPointer();
  test_assert(a->key.size() == 2 && strcmp(a->key[1].text.c_str(), "A") == 0 && a->type == kBrush);
  Result<PropValue> b = getValue(ctx, "tool.brushes[1]");
  test_assert(setValue(ctx, "tool.activeBrush", *b).ok() && w.tool.activeBrush == 1);
  test_assert(setValue(ctx, "tool.activeBrush", *old).ok() && w.tool.activeBrush == 0);
  /* Paths through it canonicalize to the target, not the hop. */
  Result<StableRef> size = canonicalize(ctx, P("tool.activeBrush.size"));
  test_assert(size.ok() && size->key.size() == 3 && strcmp(size->key[1].text.c_str(), "A") == 0);
  w.tool.activeBrush = 1;
  test_assert(setValue(ctx, *resolve(ctx, *size), PropValue::ofFloat(4)).ok());
  test_assert(w.tool.brushes[0].size == 4 && w.tool.brushes[1].size == 1);
  /* Without an address, reading or walking the settable pointer is Unsupported. */
  test_assert(getValue(ctx, "tool.activeBrushNoAddress").status().code() == StatusCode::Unsupported);
  test_assert(getValue(ctx, "tool.activeBrushNoAddress.size").status().code() ==
              StatusCode::Unsupported);
}

static void test_review_fixes()
{
  World w;
  Context ctx = w.context();
  /* A dynamic hook's ReadOnly blocks writes (B5). */
  test_assert(setValue(ctx, "tool.guarded", PropValue::ofInt(1)).ok());
  w.tool.locked = true;
  test_assert(getMeta(ctx, "tool.guarded")->hasAccess(PropAccess::ReadOnly));
  test_assert(setValue(ctx, "tool.guarded", PropValue::ofInt(5)).code() ==
              StatusCode::FailedPrecondition);
  test_assert(w.tool.guarded == 1);

  /* A getter of the wrong type is an error, never an abort (B7). */
  test_assert(getValue(ctx, "tool.broken").status().code() == StatusCode::Internal);
  test_assert(getValue(ctx, "tool.broken[0]").status().code() == StatusCode::Internal);
  test_assert(setValue(ctx, "tool.broken[0]", PropValue::ofFloat(1)).code() == StatusCode::Internal);

  /* A base-typed pointer walks into the subtype, and its values write back (D1). */
  test_assert(*getValue(ctx, "objects[0].anyData.radius") == PropValue::ofFloat(1));
  Result<PropValue> anyData = getValue(ctx, "objects[0].anyData");
  test_assert(anyData.ok() && anyData->tryPointer()->type == kMesh);
  StableRef asBase = *anyData->tryPointer();
  asBase.type = kIdBase;
  test_assert(resolve(ctx, asBase).ok()); /* a Mesh is an IdBase */
  test_assert(w.rt.ext<ApiRegistry>().isA(kMesh, kIdBase) &&
              !w.rt.ext<ApiRegistry>().isA(kIdBase, kMesh));
  test_assert(setValue(ctx, "objects[1].data", *anyData).ok());

  /* Registry listing and the description getter (D8). */
  ApiRegistry &reg = w.rt.ext<ApiRegistry>();
  bool sawMesh = false;
  for (int i = 0; i < reg.structCount(); i++) {
    sawMesh |= reg.structAt(i).id() == kMesh;
  }
  test_assert(sawMesh);
}

/* keyOf must return a key (B10). */
static void test_bad_key_of()
{
  Runtime rt;
  ApiRegistry &reg = rt.ext<ApiRegistry>();
  struct Item {
    int v = 3;
  } item;
  ApiStruct it("Item");
  it.add(PropertyDef::intProp("v"),
         ValueAccessor{[](const PointerRef &s, Context &) {
                         return PropValue::ofInt(static_cast<Item *>(s.data)->v);
                       },
                       {}});
  test_assert(reg.add(std::move(it)).ok());
  ApiStruct root("Root");
  CollectionAccessor c;
  c.length = [](const PointerRef &, Context &) { return int64_t(1); };
  c.at = [&item](const PointerRef &, Context &, int64_t) {
    return PointerRef{&item, StructId::of("Item"), OwnerKey()};
  };
  c.lookup = [](const PointerRef &, Context &, const KeySegment &) { return PointerRef(); };
  c.keyOf = [](const PointerRef &, Context &, const PointerRef &) { return KeySegment(); };
  root.addCollection(PropertyDef::collectionProp("bad", StructId::of("Item")), std::move(c));
  test_assert(reg.add(std::move(root)).ok());
  int r = 0;
  test_assert(reg.addRuntimeOwner("root", StructId::of("Root"), [&r]() { return (void *)&r; }).ok());
  Context ctx(&rt, nullptr, reg.runtimeOwner("root"));
  Result<PropValue> v = getValue(ctx, "bad[0].v");
  test_assert(v.status().code() == StatusCode::Internal && hasMsg(v.status(), "keyOf"));
}

int main()
{
  test_owner_crossings();
  test_review_fixes();
  test_bad_key_of();
  test_grammar_forms();
  test_errors();
  test_write_path();
  test_canonical_paths();
  test_dangling_refs();
  test_pointer_writes();
  test_prefix_and_batch();
  test_registry_checks();
  test_parents();
  return test_end();
}
