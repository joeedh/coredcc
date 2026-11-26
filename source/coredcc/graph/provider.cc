#include "coredcc/graph/provider.h"

#include "coredcc/base/iface/owned.h"
#include "coredcc/base/iface/registry.h"
#include "coredcc/graph/def_io.h"
#include "coredcc/lib/provider.h"

#include "litestl/binding/binding.h"

#include <string>

namespace coredcc {

namespace lbt = litestl::binding::types;

namespace {

struct GraphProvider {
  void *data(const Datablock &block) const
  {
    return block.clientData();
  }
  void foreachRef(const Datablock &block, RefVisitor &visitor) const
  {
    /* None for a block that readPayload didn't fill. */
    if (GraphDef *def = static_cast<GraphDef *>(block.clientData())) {
      def->foreachRef(visitor);
    }
  }
  void destroy(const Datablock &block)
  {
    litestl::alloc::Delete(static_cast<GraphDef *>(block.clientData()));
  }
  Status serialize(const Datablock &block, BlockWriter &out) const
  {
    if (!block.clientData()) {
      return Status::error(StatusCode::FailedPrecondition, "a graph block without data");
    }
    writeGraphDef(*static_cast<GraphDef *>(block.clientData()), out);
    return Status(); /* or the writer's */
  }
  Status deserialize(const Datablock &block, BlockReader &in)
  {
    GraphDef *def = readGraphDef(in, block.id());
    if (!def) {
      return in.bytes().status();
    }
    in.setData(def);
    return Status();
  }
  static lbt::Struct<GraphProvider> *defineBindings()
  {
    auto *st =
        new lbt::Struct<GraphProvider>("coredcc::GraphProvider", sizeof(GraphProvider));
    BIND_STRUCT_METHOD(st, data, MARGS("block"));
    BIND_STRUCT_METHOD(st, foreachRef, MARGS("block", "visitor"));
    BIND_STRUCT_METHOD(st, destroy, MARGS("block"));
    BIND_STRUCT_METHOD(st, serialize, MARGS("block", "out"));
    BIND_STRUCT_METHOD(st, deserialize, MARGS("block", "in"));
    return st;
  }
};

} // namespace

Status registerGraphBlocks(Runtime &rt)
{
  BlockTypeRegistry &types = rt.ext<BlockTypeRegistry>();
  if (types.find(TypeId::of(kGraphDefType))) {
    return Status();
  }
  const std::string impl = std::string(kGraphDefType) + "#provider";
  CDCC_TRY((registerImpl<BlockProvider, GraphProvider>(rt, impl)));
  Result<Owned<BlockProvider>> provider = own<BlockProvider>(
      rt, impl, litestl::alloc::New<GraphProvider>("coredcc::GraphProvider"));
  if (!provider.ok()) {
    return provider.status();
  }
  BlockTypeInfo info;
  info.name = kGraphDefType;
  info.client = ClientId::of(kCoredccClient);
  info.dataStruct = StructId::of(kGraphDefType);
  info.payloadVersion = kGraphDefPayload;
  info.storage = BlockStorage::Payload;
  return types.add(std::move(info), std::move(provider).value()).status();
}

Result<BlockHandle> newGraph(Library &lib, std::string_view name)
{
  Runtime &rt = lib.runtime();
  const BlockId id{ClientId::of(kCoredccClient), rt.newLocalId()};
  GraphDef *def = litestl::alloc::New<GraphDef>("coredcc::GraphDef", &rt, &lib, id);
  Result<BlockHandle> h = lib.createWithId(TypeId::of(kGraphDefType), id, name, def);
  if (!h.ok()) {
    litestl::alloc::Delete(def);
  }
  return h;
}

GraphDef *graphDefOf(const Library &lib, BlockHandle h)
{
  const Datablock *b = lib.get(h);
  if (!b || !(b->type() == TypeId::of(kGraphDefType))) {
    return nullptr;
  }
  return static_cast<GraphDef *>(lib.data(h));
}

} // namespace coredcc
