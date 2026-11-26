#include "coredcc/testapp/boxes.h"

#include "coredcc/api/ref_text.h"
#include "coredcc/api/struct.h"
#include "coredcc/base/iface/owned.h"
#include "coredcc/base/iface/registry.h"
#include "coredcc/draw/drawlist.h"
#include "coredcc/draw/overlay.h"
#include "coredcc/lib/block_io.h"
#include "coredcc/lib/registry.h"
#include "coredcc/scene/capabilities.h"
#include "coredcc/scene/nearest.h"
#include "coredcc/scene/object.h"

#include "litestl/binding/binding.h"
#include "litestl/util/alloc.h"

namespace testapp {

using namespace coredcc;
namespace lbt = litestl::binding::types;

namespace {

const ClientId kClient = ClientId::of("testapp");
const TypeId kBoxType = TypeId::of("testapp.Box");
const StructId kBoxStruct = StructId::of("TestBox");

struct BoxProvider {
  void *data(const Datablock &b) const
  {
    return b.clientData();
  }
  void foreachRef(const Datablock &, RefVisitor &) const
  {
  }
  void destroy(const Datablock &b)
  {
    litestl::alloc::Delete(static_cast<Box *>(b.clientData()));
  }
  Status serialize(const Datablock &b, BlockWriter &out) const
  {
    out.bytes().f64(static_cast<const Box *>(b.clientData())->half);
    return Status();
  }
  Status deserialize(const Datablock &, BlockReader &in)
  {
    Box *box = litestl::alloc::New<Box>("testapp::Box");
    in.setData(box);
    box->half = in.bytes().f64();
    return Status();
  }
  BlockId rekey(const Datablock &imported, const RekeyContext &context) const
  {
    BlockId id{imported.id().client, LocalId{1, 1}};
    while (!context.isFree(id)) {
      id.local.lo++;
    }
    return id;
  }
  static lbt::Struct<BoxProvider> *defineBindings()
  {
    auto *st = new lbt::Struct<BoxProvider>("testapp::BoxProvider", sizeof(BoxProvider));
    BIND_STRUCT_METHOD(st, data, MARGS("block"));
    BIND_STRUCT_METHOD(st, foreachRef, MARGS("block", "visitor"));
    BIND_STRUCT_METHOD(st, destroy, MARGS("block"));
    BIND_STRUCT_METHOD(st, serialize, MARGS("block", "out"));
    BIND_STRUCT_METHOD(st, deserialize, MARGS("block", "in"));
    BIND_STRUCT_METHOD(st, rekey, MARGS("imported", "context"));
    return st;
  }
};

/* Spatial and OverlayDrawable of a Box: the data is the Box itself. */
struct BoxShape {
  Box box;

  Box3d localBounds() const
  {
    Box3d b;
    for (int i = 0; i < 3; i++) {
      b.min[i] = -box.half;
      b.max[i] = box.half;
    }
    return b;
  }
  uint32_t domains() const
  {
    return 0;
  }
  void closestElements(const NearQuery &, NearSink &) const
  {
  }
  /* A filled cube, orange when selected (lighter when active), and its corners. */
  void drawOverlay(const ViewMath &, uint32_t state, DrawList &out) const
  {
    const bool selected = (state & uint32_t(OverlayState::Selected)) != 0;
    const bool active = (state & uint32_t(OverlayState::Active)) != 0;
    DrawStyle style;
    style.color = active ? 0xff66aaffu : selected ? 0xff2288ffu : 0xff999999u;
    mat4d m;
    m.identity();
    double *d = m;
    d[0] = d[5] = d[10] = 2.0 * box.half;
    out.shape(style, ShapeKind::Cube, m).ignore();
  }
  static lbt::Struct<BoxShape> *defineBindings()
  {
    auto *st = new lbt::Struct<BoxShape>("testapp::BoxShape", sizeof(BoxShape));
    BIND_STRUCT_METHOD(st, localBounds, MARGS());
    BIND_STRUCT_METHOD(st, domains, MARGS());
    BIND_STRUCT_METHOD(st, closestElements, MARGS("query", "sink"));
    BIND_STRUCT_METHOD(st, drawOverlay, MARGS("view", "state", "out"));
    return st;
  }
};
static_assert(sizeof(BoxShape) == sizeof(Box), "the capabilities read the Box as it is");

} // namespace

void registerBoxes(Runtime &rt)
{
  const char *impl = "testapp.Box#provider";
  CDCC_CHECK((registerImpl<BlockProvider, BoxProvider>(rt, impl).ok()),
             "testapp: BoxProvider");
  Result<Owned<BlockProvider>> p = own<BlockProvider>(
      rt, impl, litestl::alloc::New<BoxProvider>("testapp::BoxProvider"));
  BlockTypeInfo info;
  info.name = "testapp.Box";
  info.client = kClient;
  info.dataStruct = kBoxStruct;
  info.payloadVersion = 1;
  CDCC_CHECK(p.ok() && rt.ext<BlockTypeRegistry>().add(info, std::move(p).value()).ok(),
             "testapp: registering the box type");
  CDCC_CHECK((registerImpl<Spatial, BoxShape>(rt, "testapp.Box").ok()),
             "testapp: box Spatial");
  CDCC_CHECK((registerImpl<OverlayDrawable, BoxShape>(rt, "testapp.Box").ok()),
             "testapp: box OverlayDrawable");

  ApiStruct s("TestBox");
  s.parent(StructId::of(kObjectDataStruct));
  s.addField(PropertyDef::floatProp("half").range(0, 100), &Box::half); /* half the edge */
  CDCC_CHECK(rt.ext<ApiRegistry>().add(std::move(s)).ok(),
             "testapp: registering TestBox");
}

std::string createBox(Context &ctx, double half)
{
  Library *lib = ctx.library();
  Result<BlockHandle> h =
      lib->create(kBoxType, "Box", litestl::alloc::New<Box>("testapp::Box"));
  if (!h.ok()) {
    return "";
  }
  static_cast<Box *>(lib->data(*h))->half = half;
  lib->setFlag(*h, BlockFlags::Root, true);
  StableRef ref;
  ref.owner = OwnerKey::ofBlock(lib->get(*h)->id());
  ref.type = kBoxStruct;
  return formatRef(ctx.runtime(), ref);
}

} // namespace testapp
