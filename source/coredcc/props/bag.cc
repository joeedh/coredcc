#include "coredcc/props/bag.h"

#include "coredcc/base/text.h"

#include <string>

namespace coredcc {

PropValue PropertyBag::unsetValue(const PropertyDef &def)
{
  return def.hasAccess(PropAccess::Optional) ? PropValue() : def.defaultValue();
}

Status PropertyBag::add(PropertyDef def)
{
  CDCC_TRY(def.validate());
  if (indexOf(def.apiName().c_str()) >= 0) {
    return Status::error(
        StatusCode::AlreadyExists,
        (std::string("property '") + def.apiName().c_str() + "' is already in the bag")
            .c_str());
  }
  PropValue v = unsetValue(def);
  entries_.append(Entry{std::move(def), std::move(v), false});
  return Status();
}

int PropertyBag::indexOf(std::string_view name) const
{
  for (int i = 0; i < int(entries_.size()); i++) {
    if (name == entries_[i].def.apiName().c_str()) {
      return i;
    }
  }
  return -1;
}

const PropValue *PropertyBag::get(std::string_view name) const
{
  const int i = indexOf(name);
  return i >= 0 ? &entries_[i].value : nullptr;
}

PropertyDef PropertyBag::resolvedDef(int i) const
{
  const PropertyDef &d = def(i);
  return d.isDynamic() ? d.resolved(this) : d;
}

litestl::util::Vector<int> PropertyBag::setOrder() const
{
  litestl::util::Vector<int> out;
  for (bool dynamic : {false, true}) {
    for (int i = 0; i < size(); i++) {
      if (def(i).isDynamic() == dynamic) {
        out.append(i);
      }
    }
  }
  return out;
}

bool PropertyBag::isSet(std::string_view name) const
{
  const int i = indexOf(name);
  return i >= 0 && entries_[i].wasSet;
}

Status PropertyBag::notFound(std::string_view name) const
{
  Status err =
      Status::error(StatusCode::NotFound,
                    (std::string("no property '") + std::string(name) + "'").c_str());
  Suggester sg(name);
  for (const Entry &e : entries_) {
    sg.consider(e.def.apiName().c_str());
  }
  sg.addTo(err);
  return err;
}

Status PropertyBag::set(std::string_view name, const PropValue &value)
{
  const int i = indexOf(name);
  if (i < 0) {
    return notFound(name);
  }
  return setAt(i, value);
}

Status PropertyBag::setAt(int i, const PropValue &value)
{
  CDCC_CHECK(i >= 0 && i < size(), "PropertyBag::setAt index out of range");
  const PropertyDef &def = entries_[i].def;
  if (value.isNone() && def.hasAccess(PropAccess::Optional)) {
    entries_[i].value = PropValue();
    entries_[i].wasSet = false;
    return Status();
  }
  Result<PropValue> v = coerce(resolvedDef(i), value);
  if (!v.ok()) {
    return v.status();
  }
  entries_[i].value = std::move(v).value();
  entries_[i].wasSet = true;
  return Status();
}

Status PropertyBag::reset(std::string_view name)
{
  const int i = indexOf(name);
  if (i < 0) {
    return notFound(name);
  }
  entries_[i].value = unsetValue(entries_[i].def);
  entries_[i].wasSet = false;
  return Status();
}

void PropertyBag::resetAll()
{
  for (Entry &e : entries_) {
    e.value = unsetValue(e.def);
    e.wasSet = false;
  }
}

} // namespace coredcc
