/* Pointer inputs in invocations: as text, as datapaths, and with a path prefix. */
#include "command_fixture.h"

#include "coredcc/api/ref_text.h"
#include "coredcc/base/text.h"

test_init;

namespace {

/* Takes an IdBase and a Brush, and any pointer; remembers them. */
struct TakePointers {
  Status canRun(Context &) const
  {
    return Status();
  }
  Status exec(Context &ctx)
  {
    for (const char *name : {"any", "base", "brush"}) {
      CDCC_TRY(ctx.outputs()->set(name, *ctx.inputs()->get(name)));
    }
    return Status();
  }
  void undo(Context &)
  {
  }
  uint64_t byteSize() const
  {
    return 0;
  }
  void discard()
  {
  }
  static lbt::Struct<TakePointers> *defineBindings()
  {
    auto *st = new lbt::Struct<TakePointers>("test::TakePointers", sizeof(TakePointers));
    BIND_COMMAND_CORE(st);
    return st;
  }
};

struct PointerWorld {
  Setup s;
  PointerWorld()
  {
    CommandDef def("test.take");
    def.input(
           PropertyDef::pointerProp("any", StructId()).access(PropAccess::SaveLastValue))
        .input(PropertyDef::pointerProp("base", kIdBase))
        .input(PropertyDef::pointerProp("brush", kBrush))
        .output(PropertyDef::pointerProp("any", StructId()))
        .output(PropertyDef::pointerProp("base", StructId()))
        .output(PropertyDef::pointerProp("brush", StructId()));
    test_assert(addNativeCommand<TakePointers>(s.w.rt, std::move(def)).ok());
  }
  Result<PropertyBag> run(const std::string &args)
  {
    return invoke(s.ctx, "test.take(" + args + ")");
  }
  /* What `name` was, given `args`. */
  StableRef got(const char *name, const std::string &args)
  {
    Result<PropertyBag> r = run(args);
    if (!r.ok()) {
      fprintf(stderr, "%s: %s\n", args.c_str(), r.status().message());
      test_assert(false);
      return StableRef();
    }
    return r->get(name)->asPointer();
  }
};

std::string quoted(const std::string &s)
{
  std::string out;
  appendQuoted(out, s);
  return out;
}

} // namespace

static void test_invocations()
{
  PointerWorld p;
  World &w = p.s.w;
  const BlockId cube = w.objectId(0), mesh = w.meshId(1);
  StableRef want;
  want.owner = OwnerKey::ofBlock(cube);
  want.type = kObject;
  test_assert(p.got("any", "any='objects[\"Cube\"]'") == want);
  test_assert(p.got("any", "any='objects[0]'") == want);
  test_assert(p.got("any", "any=" + quoted(formatRef(w.rt, want))) == want);
  /* A pointer member is its target. Its struct may inherit from the input's. */
  StableRef target;
  target.owner = OwnerKey::ofBlock(mesh);
  target.type = kMesh;
  test_assert(p.got("any", "any='objects[1].data'") == target);
  target.type = kIdBase;
  test_assert(p.got("base", "base='objects[1].data'") == target);
  StableRef typed = target;
  typed.type = kMesh;
  test_assert(p.got("base", "base=" + quoted(formatRef(w.rt, typed))) == target);
  typed.type = StructId();
  test_assert(p.got("base", "base=" + quoted(formatRef(w.rt, typed))) == typed);
  test_assert(p.got("brush", "brush='tool.activeBrush'").key.size() == 2);
  test_assert(p.got("brush", "brush='tool.brushes.active'").key.size() == 2);

  /* Null: the default, the empty text, and a pointer that is null. */
  test_assert(p.got("any", "").isNull() && p.got("any", "any=''").isNull() &&
              p.got("any", "any=' '").isNull());
  w.object(1).mesh.target = BlockId();
  test_assert(p.got("any", "any='objects[1].data'").isNull());
  /* LastUsed doesn't keep them. */
  test_assert(!p.got("any", "any='objects[0]'").isNull() && p.got("any", "").isNull());
  /* A reference needn't resolve: that is the command's business. */
  test_assert(
      !p.got("any", "any='@block(x, 00000000000000000000000000000009)'").isNull());

  const auto refused = [&](const char *args, StatusCode code, const char *why) {
    Result<PropertyBag> r = p.run(args);
    if (r.ok()) {
      fprintf(stderr, "%s: ok\n", args);
      return false;
    }
    return r.status().code() == code && hasMsg(r.status(), why);
  };
  test_assert(refused("any='objcts[0]'", StatusCode::NotFound, "objcts"));
  test_assert(refused("any='objects['", StatusCode::InvalidArgument, "offset"));
  test_assert(refused(
      "any='tool.strength'", StatusCode::InvalidArgument, "neither a struct nor"));
  test_assert(
      refused("any='objects'", StatusCode::InvalidArgument, "neither a struct nor"));
  test_assert(
      refused("any='objects[0].location[1]'", StatusCode::InvalidArgument, "neither"));
  test_assert(refused("any='@block(x'", StatusCode::InvalidArgument, "reference:"));
  test_assert(refused("any=objects", StatusCode::InvalidArgument, "quoted"));
  test_assert(refused("any=3", StatusCode::InvalidArgument, "quoted"));
  test_assert(refused("any=('a')", StatusCode::InvalidArgument, "single value"));
  test_assert(
      refused("brush='objects[0]'", StatusCode::TypeMismatch, "another struct type"));
  test_assert(
      refused("base='@owner(tool) : Tool'", StatusCode::TypeMismatch, "another struct"));
  /* An unaddressed pointer has no reference to give. */
  test_assert(
      refused("any='tool.activeBrushNoAddress'", StatusCode::Unsupported, "address"));
}

static void test_prefix()
{
  PointerWorld p;
  p.s.ctx.setPrefix(*parsePath("objects[1]"));
  StableRef want;
  want.owner = OwnerKey::ofBlock(p.s.w.meshId(1));
  want.type = kMesh;
  test_assert(p.got("any", "any='data'") == want);
  test_assert(!p.run("any='objects[0]'").ok());
  p.s.ctx.clearPrefix();
  /* The command got a reference, so the prefix doesn't matter to it or to its redo. */
  test_assert(p.s.ex.undo(p.s.ctx).ok() && p.s.ex.redo(p.s.ctx).ok());
}

static void test_literals()
{
  PointerWorld p;
  const Runtime &rt = p.s.w.rt;
  const PropertyDef def = PropertyDef::pointerProp("p", StructId());
  StableRef odd;
  odd.owner = OwnerKey::ofBlock(p.s.w.objectId(0));
  odd.key.append(KeySegment::stringKey("say \"hi\\\"\n"));
  odd.type = kMod;
  for (const StableRef &ref : {odd, StableRef()}) {
    Result<std::string> lit = formatLiteral(rt, def, PropValue::ofPointer(ref));
    test_assert(lit.ok());
    Result<Invocation> inv = parseInvocation("x(a=" + *lit + ")");
    test_assert(inv.ok());
    Result<PropValue> back = parseLiteral(rt, def, inv->args[0].value);
    test_assert(back.ok() && back->asPointer() == ref);
  }
  test_assert(*formatLiteral(rt, def, PropValue::nullPointer()) == "\"\"");
  test_assert(formatLiteral(rt, PropertyDef::collectionProp("c", kMod), PropValue())
                  .status()
                  .code() == StatusCode::Unsupported);
  /* A literal isn't resolved, so a datapath isn't one. */
  Result<Invocation> inv = parseInvocation("x(a='objects[0]' b=3)");
  test_assert(
      hasMsg(parseLiteral(rt, def, inv->args[0].value).status(), "p: reference:"));
  test_assert(
      hasMsg(parseLiteral(rt, def, inv->args[1].value).status(), "a quoted reference"));
  test_assert(
      hasMsg(parseLiteral(rt, PropertyDef::collectionProp("c", kMod), inv->args[0].value)
                 .status(),
             "no literal form"));
}

int main()
{
  test_invocations();
  test_prefix();
  test_literals();
  return test_end();
}
