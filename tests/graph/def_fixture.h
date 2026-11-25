#pragma once

/* A World with the node type `test.two`, which has every kind of socket and param, for the
 * tests of GraphDef. */

#include "graph_fixture.h"

#include "coredcc/graph/topology.h"

namespace fixture {

struct Mesh {
  int verts = 0;
};

} // namespace fixture

template <> struct coredcc::PayloadTraits<fixture::Mesh> {
  static constexpr const char *name = "test.Mesh";
};

namespace fixture {

inline Status nothing(NodeExecContext &)
{
  return Status();
}

inline void addTypes(Runtime &rt)
{
  test_assert(rt.ext<SocketTypeRegistry>().addPayloadType<Mesh>().ok());
  NodeTypeDef two("test.two");
  two.input(PropertyDef::floatProp("a").range(0, 10).defaultValue(PropValue::ofFloat(1)))
      .input(PropertyDef::floatProp("b"))
      .payloadInput("mesh", "test.Mesh")
      .output("x", "float")
      .output("y", "float")
      .output("m", "mat4")
      .param(PropertyDef::intProp("count").range(0, 8).defaultValue(PropValue::ofInt(2)))
      .param(PropertyDef::floatProp("maybe").access(PropAccess::Optional))
      .param(PropertyDef::pointerProp("thing", kThingStruct))
      .param(PropertyDef::pointerProp("hint", kThingStruct), true);
  addType<nothing>(rt, two, "test.two");
}

struct World {
  Runtime rt;
  Library lib;
  World() : lib(&rt)
  {
    addThings(rt);
    test_assert(registerGraphBlocks(rt).ok() && registerGraphBlocks(rt).ok());
    addTypes(rt);
    lib.setQuarantine(true);
  }
  ~World()
  {
    test_assert(rt.validateAll().ok() && lib.validate().ok());
  }
};

inline bool isText(const litestl::util::string &a, const char *b)
{
  return strcmp(a.c_str(), b) == 0;
}

inline PropValue pointerTo(BlockId id, StructId type)
{
  StableRef r;
  r.owner = OwnerKey::ofBlock(id);
  r.type = type;
  return PropValue::ofPointer(r);
}

} // namespace fixture
