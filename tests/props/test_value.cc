#include "coredcc/props/value.h"

#include "test_util.h"

#include <cmath>
#include <cstring>
#include <limits>

test_init;

using namespace coredcc;

static void test_types_and_accessors()
{
  test_assert(PropValue().isNone());
  test_assert(PropValue::ofBool(true).asBool());
  test_assert(PropValue::ofInt(-5).asInt() == -5);
  test_assert(PropValue::ofFloat(0.5).asFloat() == 0.5);
  test_assert(strcmp(PropValue::ofString("hi").asString().c_str(), "hi") == 0);
  test_assert(PropValue::ofEnum(3).asEnum() == 3);
  test_assert(PropValue::ofFlags(6).asFlags() == 6);

  PropValue v = PropValue::ofVec3(1, 2, 3);
  test_assert(v.type() == PropValue::Type::Vec3 && v.componentCount() == 3);
  test_assert(v.component(2) == 3.0);
  test_assert(PropValue::identityMat4().component(5) == 1.0);
  test_assert(PropValue::identityMat4().component(4) == 0.0);
  test_assert(PropValue::ofQuat(0, 0, 0, 1).componentCount() == 4);

  /* try* never aborts. */
  test_assert(!PropValue::ofInt(1).tryFloat());
  test_assert(*PropValue::ofInt(1).tryInt() == 1);
  test_assert(PropValue::ofInt(1).tryString() == nullptr);
  test_assert(!PropValue::ofEnum(1).tryInt()); /* Enum is not Int */
  test_assert(PropValue::nullPointer().tryPointer()->isNull());
}

static void test_equality_and_hash()
{
  const double nan = std::numeric_limits<double>::quiet_NaN();
  /* NaN equals itself, so a NaN-valued watcher doesn't refire. */
  test_assert(PropValue::ofFloat(nan) == PropValue::ofFloat(nan));
  test_assert(PropValue::ofFloat(nan).computeHash() == PropValue::ofFloat(nan).computeHash());
  /* -0 and 0 differ, matching their hashes. */
  test_assert(PropValue::ofFloat(-0.0) != PropValue::ofFloat(0.0));
  test_assert(PropValue::ofFloat(-0.0).computeHash() != PropValue::ofFloat(0.0).computeHash());
  test_assert(PropValue::ofFloat(-0.0).approxEqual(PropValue::ofFloat(0.0), 0.0));

  test_assert(PropValue::ofInt(1) != PropValue::ofFloat(1.0));
  test_assert(PropValue::ofInt(1) != PropValue::ofEnum(1));
  test_assert(PropValue::ofVec2(1, 2) == PropValue::ofVec2(1, 2));
  test_assert(PropValue::ofVec2(1, 2).computeHash() == PropValue::ofVec2(1, 2).computeHash());
  test_assert(PropValue::ofVec2(1, 2) != PropValue::ofVec3(1, 2, 0));
  test_assert(PropValue::ofString("a") == PropValue::ofString("a"));
  test_assert(PropValue::ofString("a").computeHash() != PropValue::ofString("b").computeHash());

  test_assert(PropValue::ofVec2(1, 2).approxEqual(PropValue::ofVec2(1.0001, 2), 1e-3));
  test_assert(!PropValue::ofVec2(1, 2).approxEqual(PropValue::ofVec2(1.1, 2), 1e-3));
  test_assert(PropValue::ofFloat(nan).approxEqual(PropValue::ofFloat(nan), 1e-3)); /* reflexive */

  StableRef a;
  a.owner = OwnerKey::ofBlock(BlockId{ClientId::of("c"), LocalId{1, 2}});
  a.key.append(KeySegment::member("modifiers"));
  a.key.append(KeySegment::stringKey("Bevel"));
  StableRef b = a;
  test_assert(PropValue::ofPointer(a) == PropValue::ofPointer(b));
  test_assert(PropValue::ofPointer(a).computeHash() == PropValue::ofPointer(b).computeHash());
  b.key.append(KeySegment::ofHandle(4));
  test_assert(PropValue::ofPointer(a) != PropValue::ofPointer(b));
  test_assert(PropValue::ofPointer(a) != PropValue::nullPointer());
}

static void test_to_string()
{
  test_assert(PropValue::ofBool(false).toString() == "false");
  test_assert(PropValue::ofInt(-3).toString() == "-3");
  test_assert(PropValue::ofFloat(0.1).toString() == "0.1");
  test_assert(PropValue::ofVec3(1, 0.5, -2).toString() == "(1, 0.5, -2)");
  test_assert(PropValue::ofString("a\"b\\c\n").toString() == "\"a\\\"b\\\\c\\n\"");
  test_assert(PropValue::ofEnum(2).toString() == "#2");
  test_assert(PropValue::ofFlags(10).toString() == "0xa");
  test_assert(PropValue::nullPointer().toString() == "{null}");
  test_assert(PropValue::ofFloat(std::nan("")).toString() == "nan"); /* on every platform */
  test_assert(PropValue::ofVec2(INFINITY, -INFINITY).toString() == "(inf, -inf)");

  StableRef r;
  r.owner = OwnerKey::ofRuntime(0x10);
  r.key.append(KeySegment::member("prefs"));
  r.key.append(KeySegment::stringKey("x\"y"));
  r.key.append(KeySegment::ofHandle(7));
  test_assert(r.toString() == "{runtime 0000000000000010}.prefs[\"x\\\"y\"][#7]");
}

static void test_storage()
{
  /* Scalars and vectors stay small and inline; big payloads live out of line. */
  static_assert(sizeof(PropValue) <= 48);
  double m[16];
  for (int i = 0; i < 16; i++) {
    m[i] = i;
  }
  PropValue a = PropValue::ofComponents(PropValue::Type::Mat4, m);
  PropValue b = a; /* deep copy */
  test_assert(a == b && b.component(15) == 15.0);
  PropValue c = std::move(b);
  test_assert(b.isNone() && c == a);
  c = PropValue::ofString("replaced");
  test_assert(strcmp(c.asString().c_str(), "replaced") == 0);
  PropValue &self = c;
  c = self;
  test_assert(strcmp(c.asString().c_str(), "replaced") == 0);
  a = PropValue::ofInt(3);
  test_assert(a.asInt() == 3);
}

static void test_null_pointers()
{
  /* Every spelling of null is the one null Pointer. */
  StableRef keyOnly;
  keyOnly.key.append(KeySegment::member("x"));
  StableRef nullBlock;
  nullBlock.owner = OwnerKey::ofBlock(BlockId{});
  test_assert(keyOnly.isNull() && nullBlock.isNull());
  test_assert(PropValue::ofPointer(keyOnly) == PropValue::nullPointer());
  test_assert(PropValue::ofPointer(nullBlock) == PropValue::nullPointer());
  test_assert(PropValue::ofPointer(nullBlock).computeHash() ==
              PropValue::nullPointer().computeHash());
  test_assert(OwnerKey::ofBlock(BlockId{}) == OwnerKey());

  /* Owner kinds and the target type distinguish refs. */
  StableRef r;
  r.owner = OwnerKey::ofRuntime(5);
  StableRef blk;
  blk.owner = OwnerKey::ofBlock(BlockId{ClientId{5}, LocalId{0, 5}});
  test_assert(!(r == blk));
  StableRef typed = r;
  typed.type = StructId::of("Mesh");
  test_assert(!(typed == r) && typed.computeHash() != r.computeHash());

  /* Segment kinds and boundaries hash apart (lengths are hashed). */
  StableRef one = r;
  one.key.append(KeySegment::member("a\x01" "b"));
  StableRef two = r;
  two.key.append(KeySegment::member("a"));
  two.key.append(KeySegment::stringKey("b"));
  test_assert(!(one == two) && one.computeHash() != two.computeHash());
  StableRef h = r;
  h.key.append(KeySegment::ofHandle(1));
  StableRef k = r;
  k.key.append(KeySegment::stringKey("1"));
  test_assert(!(h == k));
}

int main()
{
  test_storage();
  test_null_pointers();
  test_types_and_accessors();
  test_equality_and_hash();
  test_to_string();
  return test_end();
}
