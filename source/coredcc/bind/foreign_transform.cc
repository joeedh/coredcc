#include "coredcc/bind/foreign_transform.h"

#include "coredcc/base/iface/registry.h"
#include "coredcc/bind/detail.h"
#include "coredcc/bind/foreign_command.h"

#include <cmath>
#include <optional>
#include <span>
#include <string>

namespace coredcc::script {

using detail::error;
using detail::view;
namespace lf = litestl::binding::foreign;

TransformTypeInfo *newTransformTypeDef(const string &id)
{
  TransformTypeInfo *type =
      litestl::alloc::New<TransformTypeInfo>("coredcc::TransformTypeInfo");
  type->name = id;
  return type;
}

Status transformTypeDefUiName(TransformTypeInfo &type, const string &s)
{
  type.uiName = s;
  return Status();
}

Status transformTypeDefChannel(TransformTypeInfo &type,
                               const string &name,
                               const PropValue &defaultValue,
                               bool distance)
{
  const bool quat = defaultValue.type() == PropValue::Type::Quat;
  Result<PropertyDef> p =
      detail::scriptProp("transformTypeDefChannel",
                         name,
                         quat ? PropValue::ofBool(false) : defaultValue,
                         false);
  if (!p.ok()) {
    return p.status();
  }
  PropertyDef def = std::move(p).value();
  if (quat) {
    def = PropertyDef::quatProp(view(name)).defaultValue(defaultValue);
  }
  if (distance) {
    if (quat || def.kind() == PropKind::Bool || def.kind() == PropKind::Int) {
      return error(StatusCode::InvalidArgument,
                   std::string("transformTypeDefChannel: '") + name.c_str() +
                       "' isn't a Float or a vector, so it can't be a distance");
    }
    def.subtype(PropSubtype::Distance).unit(UnitFamily::Length);
  }
  type.channels.append(std::move(def));
  return Status();
}

Status transformTypeDefVersion(TransformTypeInfo &type, int32_t version)
{
  if (version < 0) {
    return error(StatusCode::InvalidArgument, "transformTypeDefVersion: it is negative");
  }
  type.version = uint32_t(version);
  return Status();
}

Status transformEnumChannel(TransformTypeInfo &type, const string &name)
{
  Result<PropertyDef> p = detail::scriptProp(
      "transformTypeDefChannel", name, PropValue::ofBool(false), false);
  if (!p.ok()) {
    return p.status();
  }
  type.channels.append(PropertyDef::enumProp(view(name)));
  return Status();
}

/* The channel that a call is about: the last one of that name. */
static PropertyDef *channelOf(TransformTypeInfo &type, const string &name)
{
  for (int i = int(type.channels.size()) - 1; i >= 0; i--) {
    if (type.channels[i].apiName() == name) {
      return &type.channels[i];
    }
  }
  return nullptr;
}

static Status invalid(const string &channel, const std::string &why)
{
  return error(StatusCode::InvalidArgument,
               std::string("transform channel '") + channel.c_str() + "': " + why);
}

Status transformTypeDefChannelItem(TransformTypeInfo &type,
                                   const string &name,
                                   const string &key,
                                   double value,
                                   const string &uiName,
                                   const string &description)
{
  PropertyDef *def = channelOf(type, name);
  if (!def || def->kind() != PropKind::Enum) {
    return invalid(name, "it isn't an Enum channel");
  }
  if (!(value >= -channels::kMaxInt && value <= channels::kMaxInt) ||
      std::floor(value) != value)
  {
    return invalid(name, "an item's value must be a whole number within +-2^53");
  }
  def->item(view(key), int64_t(value), view(uiName), view(description));
  return Status();
}

Status transformTypeDefChannelText(TransformTypeInfo &type,
                                   const string &name,
                                   const string &what,
                                   const string &text)
{
  PropertyDef *def = channelOf(type, name);
  if (!def) {
    return invalid(name, "there is no such channel");
  }
  const std::string_view w = view(what);
  if (w == "uiName") {
    def->uiName(view(text));
  } else if (w == "description") {
    def->description(view(text));
  } else if (w == "icon") {
    def->icon(view(text));
  } else if (w == "subtype") {
    for (int i = 0; i <= int(PropSubtype::Password); i++) {
      if (view(text) == propSubtypeName(PropSubtype(i))) {
        def->subtype(PropSubtype(i));
        return Status();
      }
    }
    return invalid(name, std::string("no subtype '") + text.c_str() + "'");
  } else if (w == "unit") {
    const std::optional<UnitFamily> family = unitFamilyOfName(view(text));
    if (!family) {
      return invalid(name, std::string("no unit family '") + text.c_str() + "'");
    }
    def->unit(*family);
  } else if (w == "displayUnit") {
    const NumericMeta *n = def->numeric();
    if (!n || n->unit == UnitFamily::None) {
      return invalid(name, "a display unit needs a unit family");
    }
    def->unit(n->unit, view(text));
  } else if (w == "default") {
    const EnumItem *item =
        def->enumMeta() ? def->enumMeta()->findKey(view(text)) : nullptr;
    if (!item) {
      return invalid(name, std::string("no item '") + text.c_str() + "'");
    }
    def->defaultValue(PropValue::ofEnum(item->value));
  } else {
    return invalid(name, std::string("nothing is named '") + what.c_str() + "'");
  }
  return Status();
}

Status transformTypeDefChannelNumbers(
    TransformTypeInfo &type, const string &name, const string &what, double a, double b)
{
  PropertyDef *def = channelOf(type, name);
  if (!def) {
    return invalid(name, "there is no such channel");
  }
  const std::string_view w = view(what);
  if (w == "range") {
    def->range(a, b);
  } else if (w == "softRange") {
    def->softRange(a, b);
  } else if (w == "step") {
    def->step(a);
  } else if (w == "precision") {
    if (!(a >= 0.0 && a <= 100.0)) {
      return invalid(name, "a precision is a number of digits");
    }
    def->precision(int(a));
  } else {
    return invalid(name, std::string("nothing is named '") + what.c_str() + "'");
  }
  return Status();
}

/* At least two identifiers, with dots between them. */
static bool isDotted(std::string_view s)
{
  int dots = 0;
  bool start = true;
  for (char c : s) {
    const bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
    const bool digit = c >= '0' && c <= '9';
    if (c == '.' ? start : !(letter || (digit && !start))) {
      return false;
    }
    start = c == '.';
    dots += start;
  }
  return dots > 0 && !start;
}

static Status registerWith(Runtime &rt,
                           const TransformTypeInfo &type,
                           lf::Handle model,
                           const string &slots)
{
  const std::string name = type.name.c_str();
  const std::string where = "transform type '" + name + "': ";
  if (name.rfind("core.", 0) == 0) {
    return error(StatusCode::InvalidArgument,
                 where + "the 'core.' namespace is reserved for coredcc");
  }
  if (!isDotted(view(type.name))) {
    return error(StatusCode::InvalidArgument,
                 where + "the name must be dotted identifiers");
  }
  TransformRegistry &types = rt.ext<TransformRegistry>();
  CDCC_TRY(types.canAdd(type));
  CDCC_TRY(foreignImplementable(coredcc::detail::ifaceDesc<TransformModel>()));
  const std::vector<std::string> names = detail::splitSlots(slots);
  std::vector<const char *> present;
  for (const std::string &n : names) {
    present.push_back(n.c_str());
  }
  const std::string impl = name + "#model";
  CDCC_TRY(registerForeign<TransformModel>(
      rt,
      impl,
      rt.ext<ForeignBindings>().dispatcher(),
      nullptr,
      std::span<const char *const>(present.data(), present.size())));
  const TransformModel m = query<TransformModel>(
      rt, TypeId::of(impl), reinterpret_cast<void *>(uintptr_t(model)));
  if (!m) {
    return error(StatusCode::Internal, where + "its model's table is gone");
  }
  /* ForeignBindings owns the handle, so this and the registry may go in either order. */
  return types.add(type,
                   Owned<TransformModel>(
                       m, [](void *, void *) {}, nullptr))
      .status();
}

Status registerForeignTransformType(Context &ctx,
                                    const TransformTypeInfo &type,
                                    uint32_t model,
                                    const string &slots)
{
  if (model == 0) {
    return error(StatusCode::InvalidArgument,
                 "registerForeignTransformType: no model handle");
  }
  Runtime &rt = ctx.runtime();
  Status s = registerWith(rt, type, model, slots);
  if (s.ok()) {
    rt.ext<ForeignBindings>().keepHandle(model);
  } else {
    lf::release(model);
  }
  return s;
}

} // namespace coredcc::script
