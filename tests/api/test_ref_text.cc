/* Pointer values as text: what is written, what is accepted, and round trips. */
#include "scene_fixture.h"

#include "coredcc/api/ref_text.h"
#include "coredcc/lib/registry.h"

#include "fuzz.h"

#include <string>

test_init;

using namespace fixture;
using coredcc_test::SplitMix64;
using coredcc_test::testScale;

static StableRef blockRefOf(ClientId client, uint64_t hi, uint64_t lo)
{
  StableRef r;
  r.owner = OwnerKey::ofBlock(BlockId{client, LocalId{hi, lo}});
  return r;
}

static bool formats(const Runtime &rt, const StableRef &ref, const char *want)
{
  const std::string got = formatRef(rt, ref);
  Result<StableRef> back = parseRef(rt, got);
  if (got != want || !back.ok() || !(*back == ref)) {
    fprintf(stderr, "%s, want %s: %s\n", got.c_str(), want, back.status().message());
    return false;
  }
  return true;
}

static void test_format()
{
  World w;
  Runtime &rt = w.rt;
  const ClientId own = ClientId::of(kCoredccClient);
  test_assert(formats(rt, StableRef(), ""));
  StableRef r = blockRefOf(own, 0x0123456789abcdef, 1);
  test_assert(formats(rt, r, "@block(coredcc, 0123456789abcdef0000000000000001)"));
  r.type = kMesh;
  test_assert(formats(rt, r, "@block(coredcc, 0123456789abcdef0000000000000001) : Mesh"));
  r.key.append(KeySegment::member("mods"));
  r.key.append(KeySegment::stringKey("a \"b\"\\\n\xC3\xA9"));
  r.key.append(KeySegment::ofHandle(INT64_MAX));
  r.key.append(KeySegment::member("odd name"));
  test_assert(formats(
      rt,
      r,
      "@block(coredcc, 0123456789abcdef0000000000000001).mods"
      "[\"a \\\"b\\\"\\\\\\n\xC3\xA9\"][#9223372036854775807].\"odd name\" : Mesh"));
  StableRef tool;
  tool.owner = OwnerKey::ofRuntime(TypeId::of("owner:tool").value);
  tool.key.append(KeySegment::stringKey(""));
  test_assert(formats(rt, tool, "@owner(tool)[\"\"]"));

  /* Names that aren't dotted identifiers are quoted, and ids without one are written. */
  for (const char *name : {"my client", "a..b", "1a", "a.", "#x"}) {
    test_assert(rt.ext<ClientRegistry>().add(name).ok());
    const std::string want =
        "@block(\"" + std::string(name) + "\", " + std::string(31, '0') + "1)";
    test_assert(formats(rt, blockRefOf(ClientId::of(name), 0, 1), want.c_str()));
  }
  test_assert(rt.ext<ClientRegistry>().add("acme.tools_2").ok());
  test_assert(formats(rt,
                      blockRefOf(ClientId::of("acme.tools_2"), 0, 1),
                      "@block(acme.tools_2, 00000000000000000000000000000001)"));
  StableRef unknown = blockRefOf(ClientId{0xabc}, 0, 2);
  unknown.type = StructId{0x1};
  test_assert(formats(rt,
                      unknown,
                      "@block(#0000000000000abc, 00000000000000000000000000000002)"
                      " : #0000000000000001"));
  StableRef noOwner;
  noOwner.owner = OwnerKey::ofRuntime(0xffffffffffffffff);
  test_assert(formats(rt, noOwner, "@owner(#ffffffffffffffff)"));
  noOwner.owner =
      OwnerKey::ofRuntime(TypeId::of("test.Mesh").value); /* no owner's name */
  const std::string text = formatRef(rt, noOwner);
  test_assert(text.substr(0, 8) == "@owner(#" && *parseRef(rt, text) == noOwner);
}

static void test_parse()
{
  World w;
  Runtime &rt = w.rt;
  const StableRef want = []() {
    StableRef r = blockRefOf(ClientId::of(kCoredccClient), 0xAB, 1);
    r.key.append(KeySegment::member("m"));
    r.key.append(KeySegment::ofHandle(3));
    r.key.append(KeySegment::stringKey("k"));
    r.type = kMesh;
    return r;
  }();
  const std::string hex = "00000000000000ab0000000000000001";
  for (const std::string &text :
       std::initializer_list<std::string>{
           "@block(coredcc, " + hex + ").m[#3][\"k\"] : Mesh",
           "@block(coredcc," + hex + ").m[#3]['k']:Mesh",
           " \t@ block ( \"coredcc\" , 00000000000000AB0000000000000001 ) . m [ #3 ] [ "
           "'k' ] : Mesh ",
           "@block(coredcc, " + hex + ").'m'[#003][\"k\"] : 'Mesh'",
       })
  {
    Result<StableRef> r = parseRef(rt, text);
    if (!r.ok() || !(*r == want)) {
      fprintf(stderr, "%s: %s\n", text.c_str(), r.status().message());
      test_assert(false);
    }
  }
  for (const char *text : {"", " ", "\t "}) {
    Result<StableRef> r = parseRef(rt, text);
    test_assert(r.ok() && r->isNull());
  }
  /* An alias is its client's current name. */
  const char *aliases[] = {"old.acme"};
  test_assert(rt.ext<ClientRegistry>().add("new.acme", aliases).ok());
  test_assert(parseRef(rt, "@block(old.acme, " + hex + ")")->owner.block.client ==
              ClientId::of("new.acme"));
  /* Without any client registered, names are still ids. */
  Runtime bare;
  test_assert(parseRef(bare, "@block(x, " + hex + ")")->owner.block.client ==
              ClientId::of("x"));

  struct Case {
    std::string text;
    size_t offset;
    const char *why;
  };
  const std::string block = "@block(coredcc, " + hex + ")";
  const Case cases[] = {
      {"x", 0, "expected '@'"},
      {"@", 1, "'block' or 'owner'"},
      {"@thing(x)", 1, "'block' or 'owner'"},
      {"@block", 6, "expected '('"},
      {"@block(", 7, "expected a name"},
      {"@block()", 7, "expected a name"},
      {"@block(\"\", " + hex + ")", 7, "an empty name"},
      {"@block(a., " + hex + ")", 9, "expected a name"},
      {"@block(a " + hex + ")", 9, "expected ','"},
      {"@block(a, 12)", 12, "a hexadecimal digit"},
      {"@block(a, " + hex + "0)", 42, "expected ')'"},
      {"@block(a, " + std::string(32, '0') + ")", 10, "a null id"},
      {"@block(#12, " + hex + ")", 10, "a hexadecimal digit"},
      {"@block(#0000000000000000, " + hex + ")", 7, "an empty name"},
      {"@block(\"a", 9, "unterminated"},
      {"@owner(tool", 11, "expected ')'"},
      {"@owner(tool) x", 13, "expected the end"},
      {"@owner(tool):", 13, "expected a name"},
      {"@owner(tool) : A B", 17, "expected the end"},
      {block + ".", 50, "a member name"},
      {block + ".3", 50, "a member name"},
      {block + "[", 50, "a quoted key or '#handle'"},
      {block + "[3]", 50, "a quoted key or '#handle'"},
      {block + "[#-1]", 51, "an integer"},
      {block + "[#9223372036854775808]", 51, "int64 range"},
      {block + "['a'", 53, "expected ']'"},
      {block + " : \"\"", 52, "an empty name"},
      {block + std::string(1, '\0'), 49, "expected the end"},
      {std::string(kMaxPathLength + 1, ' '), 0, "longer than"},
  };
  for (const Case &c : cases) {
    size_t offset = 12345;
    Result<StableRef> r = parseRef(rt, c.text, &offset);
    if (r.ok() || offset != c.offset || !strstr(r.status().message(), c.why)) {
      fprintf(stderr,
              "%.60s: %d, %s\n",
              c.text.c_str(),
              int(offset),
              r.ok() ? "ok" : r.status().message());
      test_assert(false);
    }
  }
}

/* A value read from a path, as text and back, resolves to the same thing. */
static void test_resolves()
{
  World w;
  Context ctx = w.context();
  for (const char *path : {"objects[0]",
                           "objects[\"Sphere\"].data",
                           "tool",
                           "tool.brushes[1]",
                           "objects[0].mod",
                           "tool.activeBrush"})
  {
    Result<PropValue> v = getValue(ctx, path);
    test_assert(v.ok());
    const std::string text = formatRef(w.rt, v->asPointer());
    Result<StableRef> back = parseRef(w.rt, text);
    test_assert(back.ok() && *back == v->asPointer());
    Result<ResolvedPath> a = resolve(ctx, *back);
    test_assert(a.ok() && a->kind == ResolvedPath::Kind::Struct &&
                a->canonical == v->asPointer());
  }
}

static void test_fuzz()
{
  World w;
  const std::string good =
      "@block(\"a b\", 00000000000000ab0000000000000001).m[#3][\"k\\n\"] : "
      "#00000000000000ff";
  SplitMix64 rng(3);
  int ok = 0;
  const char pool[] = "@()[].:,#\"'\\ \t0aZz_\x80\xff";
  for (int i = 0; i < 40000 * testScale(); i++) {
    std::string s = good;
    for (uint32_t n = 1 + rng.below(4); n > 0; n--) {
      const size_t at = rng.below(uint32_t(s.size()));
      switch (rng.below(3)) {
      case 0:
        s[at] = pool[rng.below(sizeof(pool))];
        break;
      case 1:
        s.erase(at, 1 + rng.below(4));
        break;
      default:
        s.insert(at, 1, pool[rng.below(sizeof(pool))]);
      }
      if (s.empty()) {
        break;
      }
    }
    size_t offset = 0;
    Result<StableRef> r = parseRef(w.rt, s, &offset);
    test_assert(offset <= s.size());
    if (r.ok()) {
      ok++;
      const std::string text = formatRef(w.rt, *r);
      Result<StableRef> again = parseRef(w.rt, text);
      test_assert(again.ok() && *again == *r && formatRef(w.rt, *again) == text);
    }
  }
  test_assert(ok > 1000);
}

static void test_block_ids()
{
  World w;
  Runtime &rt = w.rt;
  test_assert(rt.ext<ClientRegistry>().add("a b,c").ok());
  const char *old[] = {"before.acme"};
  test_assert(rt.ext<ClientRegistry>().add("acme", old).ok());
  const BlockId own{ClientId::of(kCoredccClient), {0x0123456789abcdef, 1}};
  const BlockId odd{ClientId::of("a b,c"), {0, 2}};
  const BlockId unknown{ClientId{0xabc}, {3, 0}};
  test_assert(formatBlockId(rt, own) == "coredcc,0123456789abcdef0000000000000001");
  test_assert(formatBlockId(rt, odd) == "\"a b,c\",00000000000000000000000000000002");
  test_assert(formatBlockId(rt, unknown) ==
              "#0000000000000abc,00000000000000030000000000000000");
  test_assert(formatBlockId(rt, BlockId()).empty() && parseBlockId(rt, " ")->isNull());
  for (const BlockId &id : {own, odd, unknown}) {
    test_assert(*parseBlockId(rt, formatBlockId(rt, id)) == id);
    StableRef ref;
    ref.owner = OwnerKey::ofBlock(id);
    /* The same id text as in a reference, but for a space. */
    test_assert(*parseRef(rt, "@block(" + formatBlockId(rt, id) + ")") == ref);
  }
  test_assert(*parseBlockId(rt, " coredcc , 0123456789ABCDEF0000000000000001 ; ") == own);
  test_assert(parseBlockId(rt, "before.acme," + std::string(31, '0') + "1")->client ==
              ClientId::of("acme"));

  const BlockId all[] = {own, odd, unknown, own};
  const std::string text = formatBlockIds(rt, all);
  Result<litestl::util::Vector<BlockId>> back = parseBlockIds(rt, text);
  test_assert(back.ok() && back->size() == 4 && (*back)[0] == own && (*back)[1] == odd &&
              (*back)[2] == unknown && (*back)[3] == own);
  test_assert(formatBlockIds(rt, std::span<const BlockId>()).empty());
  for (const char *none : {"", " ", "\t"}) {
    test_assert(parseBlockIds(rt, none).ok() && parseBlockIds(rt, none)->size() == 0);
  }
  test_assert(parseBlockIds(rt, text + ";")->size() == 4);

  const std::string hex(31, '0');
  const auto bad = [&](const std::string &s, size_t at, const char *what) {
    size_t offset = 999, one = 999;
    Status st = parseBlockIds(rt, s, &offset).status();
    Status single = parseBlockId(rt, s, &one).status();
    return st.code() == StatusCode::InvalidArgument && strstr(st.message(), what) &&
           offset == at && one == at && single.code() == st.code();
  };
  test_assert(bad("coredcc", 7, "','"));
  test_assert(bad(";", 0, "a name"));
  test_assert(bad("coredcc," + hex, 39, "hexadecimal"));
  test_assert(bad("coredcc," + hex + "11", 40, "';'"));
  test_assert(bad("coredcc," + hex + "0", 8, "a null id"));
  test_assert(bad("coredcc," + hex + "1;;", 41, "a name"));
  test_assert(bad("@block(coredcc," + hex + "1)", 0, "a name"));
  size_t offset = 0;
  test_assert(hasMsg(parseBlockId(rt, text, &offset).status(), "more than one") &&
              offset == 40);
  const std::string large(kMaxBlockIdsLength + 1, ' ');
  test_assert(hasMsg(parseBlockIds(rt, large).status(), "longer than"));
  test_assert(parseBlockIds(rt, std::string_view(large).substr(1)).ok());
}

int main()
{
  test_block_ids();
  test_format();
  test_parse();
  test_resolves();
  test_fuzz();
  return test_end();
}
