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
  test_assert(PropValue::ofFloat(nan).hash() == PropValue::ofFloat(nan).hash());
  /* -0 and 0 differ, matching their hashes. */
  test_assert(PropValue::ofFloat(-0.0) != PropValue::ofFloat(0.0));
  test_assert(PropValue::ofFloat(-0.0).hash() != PropValue::ofFloat(0.0).hash());
  test_assert(PropValue::ofFloat(-0.0).approxEqual(PropValue::ofFloat(0.0), 0.0));

  test_assert(PropValue::ofInt(1) != PropValue::ofFloat(1.0));
  test_assert(PropValue::ofInt(1) != PropValue::ofEnum(1));
  test_assert(PropValue::ofVec2(1, 2) == PropValue::ofVec2(1, 2));
  test_assert(PropValue::ofVec2(1, 2).hash() == PropValue::ofVec2(1, 2).hash());
  test_assert(PropValue::ofVec2(1, 2) != PropValue::ofVec3(1, 2, 0));
  test_assert(PropValue::ofString("a") == PropValue::ofString("a"));
  test_assert(PropValue::ofString("a").hash() != PropValue::ofString("b").hash());

  test_assert(PropValue::ofVec2(1, 2).approxEqual(PropValue::ofVec2(1.0001, 2), 1e-3));
  test_assert(!PropValue::ofVec2(1, 2).approxEqual(PropValue::ofVec2(1.1, 2), 1e-3));
  test_assert(PropValue::ofFloat(nan).approxEqual(PropValue::ofFloat(nan), 1e-3) == false);

  StableRef a;
  a.owner = OwnerKey::ofBlock(BlockId{ClientId::of("c"), LocalId{1, 2}});
  a.key.append(KeySegment::member("modifiers"));
  a.key.append(KeySegment::stringKey("Bevel"));
  StableRef b = a;
  test_assert(PropValue::ofPointer(a) == PropValue::ofPointer(b));
  test_assert(PropValue::ofPointer(a).hash() == PropValue::ofPointer(b).hash());
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

  StableRef r;
  r.owner = OwnerKey::ofRuntime(0x10);
  r.key.append(KeySegment::member("prefs"));
  r.key.append(KeySegment::stringKey("x\"y"));
  r.key.append(KeySegment::ofHandle(7));
  test_assert(r.toString() == "{runtime 0000000000000010}.prefs[\"x\\\"y\"][#7]");
}

int main()
{
  test_types_and_accessors();
  test_equality_and_hash();
  test_to_string();
  return test_end();
}
