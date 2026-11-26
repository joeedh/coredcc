#pragma once

/* A Runtime with a Library, graph blocks, a block type `test.Thing` and a few node types,
 * for the graph tests. */

#include "coredcc/api/change_bus.h"
#include "coredcc/graph/def.h"
#include "coredcc/graph/provider.h"
#include "coredcc/lib/library.h"

#include "litestl/tests/test_util.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace fixture {

using namespace coredcc;
namespace lbt = litestl::binding::types;

struct Thing {
  double value = 0;
  /** What its queryInterface says, whatever the interface. */
  QueryAnswer answer;
};

struct ThingProvider {
  void *data(const Datablock &b) const
  {
    return b.clientData();
  }
  void foreachRef(const Datablock &, RefVisitor &) const
  {
  }
  void destroy(const Datablock &b)
  {
    litestl::alloc::Delete(static_cast<Thing *>(b.clientData()));
  }
  QueryAnswer queryInterface(const Datablock &b, InterfaceId) const
  {
    const Thing *t = static_cast<const Thing *>(b.clientData());
    return t ? t->answer : QueryAnswer();
  }
  static lbt::Struct<ThingProvider> *defineBindings()
  {
    auto *st =
        new lbt::Struct<ThingProvider>("test::ThingProvider", sizeof(ThingProvider));
    BIND_STRUCT_METHOD(st, data, MARGS("block"));
    BIND_STRUCT_METHOD(st, foreachRef, MARGS("block", "visitor"));
    BIND_STRUCT_METHOD(st, destroy, MARGS("block"));
    BIND_STRUCT_METHOD(st, queryInterface, MARGS("block", "iface"));
    return st;
  }
};

/* A block that refers to one other, to see what GC does with graphs' refs. */
inline const TypeId kThing = TypeId::of("test.Thing");
inline const StructId kThingStruct = StructId::of("Thing");

inline void addThings(Runtime &rt)
{
  test_assert(
      (registerImpl<BlockProvider, ThingProvider>(rt, "test.Thing#provider").ok()));
  Result<Owned<BlockProvider>> p = own<BlockProvider>(
      rt, "test.Thing#provider", litestl::alloc::New<ThingProvider>("provider"));
  test_assert(p.ok());
  BlockTypeInfo info;
  info.name = "test.Thing";
  info.client = ClientId::of(kCoredccClient);
  info.dataStruct = kThingStruct;
  test_assert(rt.ext<BlockTypeRegistry>().add(info, std::move(p).value()).ok());
}

template <Status (*Fn)(NodeExecContext &)>
const NodeType *addType(Runtime &rt, NodeTypeDef &def, const char *name)
{
  Result<Owned<GraphKernel>> k =
      makeKernel<FnKernel<Fn>>(rt, std::string(name) + "#kernel");
  test_assert(k.ok());
  def.kernel(std::move(k).value());
  Result<const NodeType *> t = rt.ext<NodeTypeRegistry>().add(std::move(def));
  if (!t.ok()) {
    printf("%s\n", t.status().toString().c_str());
  }
  test_assert(t.ok());
  return *t;
}

/* Runs the expression and checks its code, printing what it was if it differs. */
inline bool hasCode(const Status &s, StatusCode code)
{
  if (s.code() != code) {
    printf("got %s\n", s.toString().c_str());
  }
  return s.code() == code;
}
template <class T> bool hasCode(const Result<T> &r, StatusCode code)
{
  return hasCode(r.status(), code);
}

/* A block's id, and its GraphDef. */
inline BlockId idOf(const Library &lib, BlockHandle h)
{
  return lib.get(h)->id();
}

struct Graph {
  BlockHandle h;
  BlockId id;
  GraphDef *def = nullptr;
};

inline Graph makeGraph(Library &lib, const char *name)
{
  Result<BlockHandle> h = newGraph(lib, name);
  test_assert(h.ok());
  return Graph{*h, idOf(lib, *h), graphDefOf(lib, *h)};
}

inline BlockHandle newThing(Library &lib, const char *name, double value)
{
  Thing *t = litestl::alloc::New<Thing>("test::Thing");
  t->value = value;
  Result<BlockHandle> h = lib.create(kThing, name, t);
  test_assert(h.ok());
  return *h;
}

inline NodeId add(GraphDef &def, const char *type)
{
  Result<NodeId> id = def.addNode(type);
  if (!id.ok()) {
    printf("addNode: %s\n", id.status().toString().c_str());
  }
  test_assert(id.ok());
  return *id;
}

inline LinkId
connect(GraphDef &def, NodeId from, std::string_view out, NodeId to, std::string_view in)
{
  Result<LinkId> l = def.link({from, out}, {to, in});
  if (!l.ok()) {
    printf("link: %s\n", l.status().toString().c_str());
  }
  test_assert(l.ok());
  return *l;
}

inline int64_t
addSocket(GraphDef &def, SocketSide side, const char *name, const char *type)
{
  Result<int64_t> id = def.addInterface(side, name, type);
  test_assert(id.ok());
  return *id;
}

inline std::string key(int64_t id)
{
  return interfaceKey(id).c_str();
}

} // namespace fixture
