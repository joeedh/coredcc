#include "coredcc/scene/transform_registry.h"

#include "litestl/util/alloc.h"

#include <string>

namespace coredcc {

namespace {

/* The channels of a def, or 0 for a kind that has none. */
int32_t channelsOf(PropKind kind)
{
  switch (kind) {
    case PropKind::Bool:
    case PropKind::Int:
    case PropKind::Float:
    case PropKind::Enum:
      return 1;
    case PropKind::Vec2:
      return 2;
    case PropKind::Vec3:
      return 3;
    case PropKind::Vec4:
    case PropKind::Quat:
      return 4;
    default:
      return 0;
  }
}

bool exactInt(int64_t v)
{
  return v >= -int64_t(channels::kMaxInt) && v <= int64_t(channels::kMaxInt);
}

void encode(const PropValue &v, int32_t first, ChannelBlock &out)
{
  switch (v.type()) {
    case PropValue::Type::Bool:
      out.set(first, v.asBool() ? 1.0 : 0.0);
      break;
    case PropValue::Type::Int:
      out.set(first, double(v.asInt()));
      break;
    case PropValue::Type::Enum:
      out.set(first, double(v.asEnum()));
      break;
    case PropValue::Type::Float:
      out.set(first, v.asFloat());
      break;
    default:
      for (int32_t i = 0; i < int32_t(v.componentCount()); i++) {
        out.set(first + i, v.component(i));
      }
      break;
  }
}

PropValue decode(const PropertyDef &def, int32_t first, const ChannelBlock &in)
{
  const double v = in.get(first);
  switch (def.kind()) {
    case PropKind::Bool:
      return PropValue::ofBool(v >= 0.5);
    case PropKind::Int:
      return PropValue::ofInt(channels::toInt(v));
    case PropKind::Float:
      return PropValue::ofFloat(v);
    case PropKind::Enum: {
      const EnumMeta &m = *def.enumMeta();
      const EnumItem *item = m.findValue(channels::toInt(v));
      return PropValue::ofEnum(item ? item->value : m.items[0].value);
    }
    default: {
      double c[4];
      for (int32_t i = 0; i < 4; i++) {
        c[i] = in.get(first + i);
      }
      return PropValue::ofComponents(valueTypeFor(def.kind()), c);
    }
  }
}

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
    return decode(def, first, *static_cast<const ChannelBlock *>(self.data));
  };
  const int32_t end = first + channelsOf(def.kind());
  a.set = [first, end](const PointerRef &self, Context &, const PropValue &v) {
    ChannelBlock &block = *static_cast<ChannelBlock *>(self.data);
    if (block.size() < end) {
      return Status::error(StatusCode::FailedPrecondition,
                           "the channels aren't this transform type's");
    }
    if (v.type() == PropValue::Type::Int && !exactInt(v.asInt())) {
      return Status::error(StatusCode::OutOfRange, "an integer channel holds +-2^53");
    }
    encode(v, first, block);
    return Status();
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

Result<const TransformType *> TransformRegistry::add(TransformTypeInfo info,
                                                     Owned<TransformModel> model)
{
  if (!model) {
    return bad(info.name, "it has no model");
  }
  ApiStruct st(info.name.c_str());
  st.uiName(info.uiName.c_str()).description(info.description.c_str());
  st.parent(StructId::of(kChannelsStruct));
  ChannelBlock defaults;
  litestl::util::Vector<uint8_t> distance;
  for (const PropertyDef &def : info.channels) {
    if (Status s = checkDef(info.name, def); !s.ok()) {
      return s;
    }
    const int32_t first = defaults.size();
    defaults.resize(first + channelsOf(def.kind()));
    encode(def.defaultValue(), first, defaults);
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
  type->distance_ = std::move(distance);
  type->model_ = std::move(model);
  type->rt_ = &rt_;
  types_.append(type);
  byId_.add(id->value, type);
  return static_cast<const TransformType *>(type);
}

const TransformType *TransformRegistry::find(TypeId id) const
{
  TransformType *const *t = byId_.lookup_ptr(id.value);
  return t ? *t : nullptr;
}

} // namespace coredcc
