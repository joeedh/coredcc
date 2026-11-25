#include "coredcc/testapp/items.h"

#include "coredcc/base/iface/owned.h"
#include "coredcc/base/iface/registry.h"
#include "coredcc/lib/block_io.h"
#include "coredcc/lib/registry.h"

#include "litestl/binding/binding.h"
#include "litestl/util/alloc.h"

#include <cstring>

namespace testapp {

using namespace coredcc;
namespace lbt = litestl::binding::types;

namespace {

const ClientId kClient = ClientId::of("testapp");
const TypeId kItemType = TypeId::of("testapp.Item");

struct ItemProvider {
  void *data(const Datablock &b) const
  {
    return b.clientData();
  }
  void foreachRef(const Datablock &b, RefVisitor &v) const
  {
    if (Item *item = static_cast<Item *>(b.clientData())) {
      v.visit(item->other);
    }
  }
  void destroy(const Datablock &b)
  {
    litestl::alloc::Delete(static_cast<Item *>(b.clientData()));
  }
  Status serialize(const Datablock &b, BlockWriter &out) const
  {
    const Item *item = static_cast<const Item *>(b.clientData());
    out.bytes().f64(item->weight);
    out.ref(item->other);
    return Status();
  }
  Status deserialize(const Datablock &, BlockReader &in)
  {
    Item *item = litestl::alloc::New<Item>("testapp::Item");
    in.setData(item);
    item->weight = in.bytes().f64();
    in.ref(item->other);
    return Status();
  }
  BlockId rekey(const Datablock &imported, const RekeyContext &context) const
  {
    BlockId id{imported.id().client, LocalId{0, 1}};
    while (!context.isFree(id)) {
      id.local.lo++;
    }
    return id;
  }
  static lbt::Struct<ItemProvider> *defineBindings()
  {
    auto *st = new lbt::Struct<ItemProvider>("testapp::ItemProvider", sizeof(ItemProvider));
    BIND_STRUCT_METHOD(st, data, MARGS("block"));
    BIND_STRUCT_METHOD(st, foreachRef, MARGS("block", "visitor"));
    BIND_STRUCT_METHOD(st, destroy, MARGS("block"));
    BIND_STRUCT_METHOD(st, serialize, MARGS("block", "out"));
    BIND_STRUCT_METHOD(st, deserialize, MARGS("block", "in"));
    BIND_STRUCT_METHOD(st, rekey, MARGS("imported", "context"));
    return st;
  }
};

PointerRef itemRef(Library &lib, BlockHandle h)
{
  const Datablock *b = lib.get(h);
  return b && b->type() == kItemType && b->state() == BlockState::Live ?
             PointerRef{lib.data(h), kItem, OwnerKey::ofBlock(b->id())} :
             PointerRef();
}

/* A pointer to an item, held in the cell that `cellOf` gives. */
PointerAccessor pointerIn(litestl::util::function<RefCell *(const PointerRef &, Context &)> cellOf)
{
  PointerAccessor p;
  p.get = [cellOf](const PointerRef &self, Context &ctx) {
    return itemRef(*ctx.library(), ctx.library()->resolveRef(*cellOf(self, ctx)));
  };
  p.set = [cellOf](const PointerRef &self, Context &ctx, const PointerRef &target) {
    return ctx.library()->writeRef(*cellOf(self, ctx), target.owner.block);
  };
  p.cell = cellOf;
  return p;
}

litestl::util::Vector<BlockHandle> itemsOf(const Library &lib)
{
  litestl::util::Vector<BlockHandle> out;
  lib.forEach([&](BlockHandle h, const Datablock &b) {
    if (b.type() == kItemType && b.state() == BlockState::Live) {
      out.append(h);
    }
  });
  return out;
}

Datablock const *blockOf(const PointerRef &self, Context &ctx)
{
  return ctx.library()->get(ctx.library()->find(self.owner.block));
}

} // namespace

void registerItems(Runtime &rt)
{
  CDCC_CHECK(rt.ext<ClientRegistry>().registerClient("testapp").ok(), "testapp: the client");
  const char *impl = "testapp.Item#provider";
  CDCC_CHECK((registerImpl<BlockProvider, ItemProvider>(rt, impl).ok()), "testapp: ItemProvider");
  Result<Owned<BlockProvider>> p =
      own<BlockProvider>(rt, impl, litestl::alloc::New<ItemProvider>("testapp::ItemProvider"));
  BlockTypeInfo info;
  info.name = "testapp.Item";
  info.client = kClient;
  info.dataStruct = kItem;
  info.payloadVersion = 1;
  CDCC_CHECK(p.ok() && rt.ext<BlockTypeRegistry>().add(info, std::move(p).value()).ok(),
             "testapp: registering the block type");

  ApiStruct s("TestItem");
  ValueAccessor name;
  name.get = [](const PointerRef &self, Context &ctx) {
    return PropValue::ofString(blockOf(self, ctx)->name().c_str());
  };
  name.set = [](const PointerRef &self, Context &ctx, const PropValue &v) {
    return ctx.library()->rename(ctx.library()->find(self.owner.block), v.asString().c_str());
  };
  s.add(PropertyDef::stringProp("name").maxLength(64), std::move(name));
  s.addField(PropertyDef::floatProp("weight").range(0, 100), &Item::weight);
  s.addPointer(PropertyDef::pointerProp("other", kItem),
               pointerIn([](const PointerRef &self, Context &) {
                 return &static_cast<Item *>(self.data)->other;
               }));
  CDCC_CHECK(rt.ext<ApiRegistry>().add(std::move(s)).ok(), "testapp: registering TestItem");
}

void addItems(Library &lib)
{
  uint64_t n = 1;
  for (const char *name : {"Cube", "Ball"}) {
    Result<BlockHandle> h = lib.createWithId(kItemType, BlockId{kClient, LocalId{0, n++}}, name,
                                             litestl::alloc::New<Item>("testapp::Item"));
    CDCC_CHECK(h.ok(), "testapp: creating an item");
    lib.setFlag(*h, BlockFlags::Root, true);
  }
}

void addItemKeys(PropertyDef &def, Context &ctx)
{
  for (BlockHandle h : itemsOf(*ctx.library())) {
    const Datablock &b = *ctx.library()->get(h);
    if (!def.enumMeta()->findKey(b.name().c_str())) {
      def.item(b.name().c_str(), int64_t(b.id().local.lo));
    }
  }
}

void addItemMembers(ApiStruct &root, RefCell &cell)
{
  PointerAccessor active = pointerIn([&cell](const PointerRef &, Context &) { return &cell; });
  CollectionAccessor items;
  items.length = [](const PointerRef &, Context &ctx) {
    return int64_t(itemsOf(*ctx.library()).size());
  };
  items.at = [](const PointerRef &, Context &ctx, int64_t i) {
    return itemRef(*ctx.library(), itemsOf(*ctx.library())[int(i)]);
  };
  items.lookup = [](const PointerRef &, Context &ctx, const KeySegment &key) {
    Library &lib = *ctx.library();
    for (BlockHandle h : itemsOf(lib)) {
      if (key.kind == KeySegment::Kind::StringKey &&
          strcmp(lib.get(h)->name().c_str(), key.text.c_str()) == 0)
      {
        return itemRef(lib, h);
      }
    }
    return PointerRef();
  };
  items.keyOf = [](const PointerRef &, Context &ctx, const PointerRef &item) {
    return KeySegment::stringKey(blockOf(item, ctx)->name().c_str());
  };
  items.active = active.get;
  items.activeMember = "activeItem";
  items.itemStruct = kItem;
  root.addCollection(PropertyDef::collectionProp("items", kItem), std::move(items));
  root.addPointer(PropertyDef::pointerProp("activeItem", kItem), std::move(active));
}

} // namespace testapp
