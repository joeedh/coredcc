#include "coredcc/bind/foreign_transform.h"

#include "coredcc/base/iface/registry.h"
#include "coredcc/bind/detail.h"
#include "coredcc/bind/foreign_command.h"

#include <span>
#include <string>

namespace coredcc::script {

using detail::error;
using detail::view;
namespace lf = litestl::binding::foreign;

TransformTypeInfo *newTransformType(const string &id)
{
  TransformTypeInfo *type = litestl::alloc::New<TransformTypeInfo>("coredcc::TransformTypeInfo");
  type->name = id;
  return type;
}

Status transformTypeUiName(TransformTypeInfo &type, const string &s)
{
  type.uiName = s;
  return Status();
}

Status transformChannel(TransformTypeInfo &type,
                        const string &name,
                        const PropValue &defaultValue,
                        bool distance)
{
  Result<PropertyDef> p = detail::scriptProp("transformChannel", name, defaultValue, false);
  if (!p.ok()) {
    return p.status();
  }
  PropertyDef def = std::move(p).value();
  if (distance) {
    if (def.kind() == PropKind::Bool || def.kind() == PropKind::Int) {
      return error(StatusCode::InvalidArgument,
                   std::string("transformChannel: '") + name.c_str() +
                       "' isn't a Float or a vector, so it can't be a distance");
    }
    def.subtype(PropSubtype::Distance).unit(UnitFamily::Length);
  }
  type.channels.append(std::move(def));
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
    return error(StatusCode::InvalidArgument, where + "the name must be dotted identifiers");
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
  return types.add(type, Owned<TransformModel>(m, [](void *, void *) {}, nullptr)).status();
}

Status registerForeignTransformType(Context &ctx,
                                    const TransformTypeInfo &type,
                                    uint32_t model,
                                    const string &slots)
{
  if (model == 0) {
    return error(StatusCode::InvalidArgument, "registerForeignTransformType: no model handle");
  }
  Runtime &rt = ctx.runtime();
  Status s = registerWith(rt, type, model, slots);
  if (s.ok()) {
    rt.ext<ForeignBindings>().keepHandle(model);
  }
  else {
    lf::release(model);
  }
  return s;
}

} // namespace coredcc::script
