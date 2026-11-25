#include "coredcc/scene/transform_registry.h"

#include "coredcc/api/change_bus.h"
#include "coredcc/scene/capabilities.h"
#include "coredcc/scene/channel_io.h"
#include "coredcc/scene/host_channels.h"

#include "litestl/util/alloc.h"

#include <cstring>
#include <string>

namespace coredcc {

using namespace detail;

namespace {

Status bad(const litestl::util::string &type, const std::string &why)
{
  return Status::error(StatusCode::InvalidArgument,
                       ("transform type '" + std::string(type.c_str()) + "': " + why).c_str());
}

Status checkDef(const litestl::util::string &type, const PropertyDef &def)
{
  const std::string name = def.apiName().c_str();
  CDCC_TRY(def.validate());
  if (channelsOf(def.kind()) == 0) {
    return bad(type, "'" + name + "': a " + propKindName(def.kind()) + " can't be a channel");
  }
  if (def.isDynamic()) {
    return bad(type, "'" + name + "': a channel can't be dynamic");
  }
  bool exact = def.kind() != PropKind::Int || exactInt(def.defaultValue().asInt());
  if (const EnumMeta *m = def.enumMeta()) {
    for (const EnumItem &item : m->items) {
      exact = exact && exactInt(item.value);
    }
  }
  return exact ? Status() : bad(type, "'" + name + "': a value is outside +-2^53");
}

ValueAccessor accessorFor(const PropertyDef &def, int32_t first)
{
  ValueAccessor a;
  a.get = [def, first](const PointerRef &self, Context &) {
    return decodeChannels(def, first, *static_cast<const ChannelBlock *>(self.data));
  };
  const int32_t end = first + channelsOf(def.kind());
  a.set = [first, end](const PointerRef &self, Context &ctx, const PropValue &v) {
    ChannelBlock &block = *static_cast<ChannelBlock *>(self.data);
    if (block.size() < end) {
      return Status::error(StatusCode::FailedPrecondition,
                           "the channels aren't this transform type's");
    }
    if (v.type() == PropValue::Type::Int && !exactInt(v.asInt())) {
      return Status::error(StatusCode::OutOfRange, "an integer channel holds +-2^53");
    }
    encodeChannels(v, first, block);
    if (!ctx.runtime().ext<HostChannels>().isCopy(&block)) {
      return Status();
    }
    Library *lib = ctx.library();
    Transformable t = lib && self.owner.kind == OwnerKey::Kind::Block ?
                          lib->query<Transformable>(lib->find(self.owner.block)) :
                          Transformable();
    return t ? t.writeChannels(block) :
               Status::error(StatusCode::FailedPrecondition, "the block isn't Transformable");
  };
  return a;
}

} // namespace

TransformRegistry::TransformRegistry(Runtime *rt) : rt_(*rt)
{
  ApiStruct base(kChannelsStruct);
  base.uiName("Transform").description("The channels of an object's transform.");
  const bool added = rt->ext<ApiRegistry>().add(std::move(base)).ok();
  CDCC_CHECK(added, "another struct is named coredcc.TransformChannels");
}

TransformRegistry::~TransformRegistry()
{
  for (TransformType *t : types_) {
    litestl::alloc::Delete(t);
  }
}

Status TransformRegistry::canAdd(const TransformTypeInfo &info) const
{
  CDCC_TRY(rt_.busyIfEvaluating("adding a transform type"));
  if (info.name.size() == 0) {
    return bad(info.name, "it has no name");
  }
  if (strlen(info.name.c_str()) != size_t(info.name.size())) {
    return bad(info.name, "a name can't hold a NUL");
  }
  if (rt_.ext<ApiRegistry>().find(StructId::of(info.name.c_str()))) {
    return Status::error(StatusCode::AlreadyExists,
                         ("transform type '" + std::string(info.name.c_str()) +
                          "': a transform type or a struct has that name already")
                             .c_str());
  }
  if (info.version > uint32_t(INT32_MAX)) {
    return bad(info.name, "its version is too large");
  }
  for (int i = 0; i < int(info.channels.size()); i++) {
    const std::string name = info.channels[i].apiName().c_str();
    CDCC_TRY(checkDef(info.name, info.channels[i]));
    if (name == "length" || name == "active") {
      return bad(info.name, "'" + name + "' is reserved for collections");
    }
    for (int k = 0; k < i; k++) {
      if (name == info.channels[k].apiName().c_str()) {
        return bad(info.name, "two channels are named '" + name + "'");
      }
    }
  }
  return Status();
}

Result<const TransformType *> TransformRegistry::add(TransformTypeInfo info,
                                                     Owned<TransformModel> model)
{
  if (!model) {
    return bad(info.name, "it has no model");
  }
  if (Status s = canAdd(info); !s.ok()) {
    return s;
  }
  ApiStruct st(info.name.c_str());
  st.uiName(info.uiName.c_str()).description(info.description.c_str());
  st.parent(StructId::of(kChannelsStruct));
  ChannelBlock defaults;
  litestl::util::Vector<uint8_t> distance;
  for (const PropertyDef &def : info.channels) {
    const int32_t first = defaults.size();
    defaults.resize(first + channelsOf(def.kind()));
    encodeChannels(def.defaultValue(), first, defaults);
    for (int32_t i = first; i < defaults.size(); i++) {
      distance.append(uint8_t(def.subtype() == PropSubtype::Distance ? 1 : 0));
    }
    st.add(def, accessorFor(def, first));
  }
  Result<TypeId> id = rt_.registerId<IdDomain::Type>(info.name.c_str());
  if (!id.ok()) {
    return id.status();
  }
  Result<const ApiStruct *> added = rt_.ext<ApiRegistry>().add(std::move(st));
  if (!added.ok()) {
    return added.status();
  }
  TransformType *type = litestl::alloc::New<TransformType>("coredcc::TransformType");
  type->id_ = *id;
  type->name_ = info.name;
  type->struct_ = (*added)->id();
  type->defaults_ = std::move(defaults);
  type->version_ = info.version;
  type->distance_ = std::move(distance);
  type->defs_ = std::move(info.channels);
  type->model_ = std::move(model);
  type->rt_ = &rt_;
  types_.append(type);
  byId_.add(id->value, type);
  rt_.ext<ChangeBus>().notifyAll(); /* objects that waited for it answer otherwise now */
  return static_cast<const TransformType *>(type);
}

const TransformType *TransformRegistry::find(TypeId id) const
{
  TransformType *const *t = byId_.lookup_ptr(id.value);
  return t ? *t : nullptr;
}

} // namespace coredcc
