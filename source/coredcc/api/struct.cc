#include "coredcc/api/struct.h"

#include "coredcc/base/text.h"

#include <cstring>
#include <string>

namespace coredcc {

bool ApiMember::readOnly() const
{
  if (def_.hasAccess(PropAccess::ReadOnly)) {
    return true;
  }
  switch (kind()) {
    case MemberKind::Value:
      return !value_.set;
    case MemberKind::Pointer:
      return !pointer_.set;
    case MemberKind::Collection:
      return true;
  }
  return true;
}

ApiStruct::ApiStruct(std::string_view name)
    : id_(StructId::of(name)), name_(lsString(name)), uiName_(lsString(uiNameFromApiName(name)))
{
}

ApiStruct &ApiStruct::uiName(std::string_view s)
{
  uiName_ = lsString(s);
  return *this;
}

ApiStruct &ApiStruct::description(std::string_view s)
{
  description_ = lsString(s);
  return *this;
}

ApiStruct &ApiStruct::parent(StructId parent)
{
  parent_ = parent;
  return *this;
}

ApiMember &ApiStruct::add(PropertyDef def, ValueAccessor accessor)
{
  CDCC_CHECK(def.kind() != PropKind::Pointer && def.kind() != PropKind::Collection,
             "add takes value members; use addPointer or addCollection");
  ApiMember m(std::move(def));
  m.value_ = std::move(accessor);
  members_.append(std::move(m));
  return members_.last();
}

ApiMember &ApiStruct::addPointer(PropertyDef def, PointerAccessor accessor)
{
  CDCC_CHECK(def.kind() == PropKind::Pointer, "addPointer takes a pointerProp def");
  if (!accessor.target && def.target()->target && !accessor.refine) {
    accessor.target = def.target()->target;
  }
  ApiMember m(std::move(def));
  m.pointer_ = std::move(accessor);
  members_.append(std::move(m));
  return members_.last();
}

ApiMember &ApiStruct::addCollection(PropertyDef def, CollectionAccessor accessor)
{
  CDCC_CHECK(def.kind() == PropKind::Collection, "addCollection takes a collectionProp def");
  if (!accessor.itemStruct) {
    accessor.itemStruct = def.target()->target;
  }
  ApiMember m(std::move(def));
  m.collection_ = std::move(accessor);
  members_.append(std::move(m));
  return members_.last();
}

const ApiMember *ApiStruct::findOwn(std::string_view name) const
{
  for (const ApiMember &m : members_) {
    if (name == m.name()) {
      return &m;
    }
  }
  return nullptr;
}

/* ---- registry ---- */

ApiRegistry::~ApiRegistry()
{
  for (ApiStruct *s : structs_) {
    litestl::alloc::Delete(s);
  }
}

Result<const ApiStruct *> ApiRegistry::add(ApiStruct st)
{
  const std::string sname = st.name().c_str();
  auto fail = [&sname](const std::string &why) {
    return Status::error(StatusCode::InvalidArgument, ("struct '" + sname + "': " + why).c_str());
  };
  Result<StructId> id = rt_.registerId<IdDomain::Struct>(sname);
  if (!id.ok()) {
    return id.status();
  }
  if (byId_.lookup_ptr(id->value)) {
    return Status::error(StatusCode::AlreadyExists,
                         ("struct '" + sname + "' is already registered").c_str());
  }
  for (int i = 0; i < st.memberCount(); i++) {
    const ApiMember &m = st.member(i);
    const std::string mname = m.name();
    CDCC_TRY(m.def().validate());
    for (int j = 0; j < i; j++) {
      if (mname == st.member(j).name()) {
        return fail("member '" + mname + "' is defined twice");
      }
    }
    if (mname == "length" || mname == "active") {
      /* They would shadow the collection pseudo-members on paths. */
      return fail("'" + mname + "' is reserved for collections");
    }
    switch (m.kind()) {
      case MemberKind::Value:
        if (!m.value().get) {
          return fail("member '" + mname + "' has no getter");
        }
        break;
      case MemberKind::Pointer:
        if (!m.pointer().get) {
          return fail("pointer '" + mname + "' has no getter");
        }
        if (!m.pointer().target && (!m.pointer().refine || m.pointer().candidates.size() == 0)) {
          return fail("pointer '" + mname +
                      "' needs a static target, or a refine with declared candidates");
        }
        break;
      case MemberKind::Collection: {
        const CollectionAccessor &c = m.collection();
        if (!c.length || !c.at || !c.lookup || !c.keyOf) {
          return fail("collection '" + mname + "' needs length, at, lookup and keyOf");
        }
        if (!c.itemStruct) {
          return fail("collection '" + mname + "' has no item struct");
        }
        break;
      }
    }
  }
  ApiStruct *stored = litestl::alloc::New<ApiStruct>("coredcc::ApiStruct", std::move(st));
  structs_.append(stored);
  byId_.add(id->value, stored);
  return static_cast<const ApiStruct *>(stored);
}

const ApiStruct *ApiRegistry::find(StructId id) const
{
  ApiStruct *const *s = byId_.lookup_ptr(id.value);
  return s ? *s : nullptr;
}

const ApiMember *ApiRegistry::findMember(StructId type, std::string_view name) const
{
  /* Bounded, in case of a parent cycle. */
  for (int depth = 0; depth < 64 && type; depth++) {
    const ApiStruct *s = find(type);
    if (!s) {
      return nullptr;
    }
    if (const ApiMember *m = s->findOwn(name)) {
      return m;
    }
    type = s->parentId();
  }
  return nullptr;
}

bool ApiRegistry::isA(StructId type, StructId base) const
{
  for (int depth = 0; depth < 64 && type; depth++) {
    if (type == base) {
      return true;
    }
    const ApiStruct *s = find(type);
    type = s ? s->parentId() : StructId();
  }
  return false;
}

Status ApiRegistry::unknownMember(StructId type, std::string_view name) const
{
  const ApiStruct *s = find(type);
  Status err = Status::error(StatusCode::NotFound,
                             ("no member '" + std::string(name) + "' in " +
                              (s ? s->name().c_str() : "an unregistered struct"))
                                 .c_str());
  Suggester sg(name);
  for (int depth = 0; depth < 64 && s; depth++) {
    for (int i = 0; i < s->memberCount(); i++) {
      sg.consider(s->member(i).name());
    }
    s = find(s->parentId());
  }
  sg.addTo(err);
  return err;
}

Result<OwnerKey> ApiRegistry::addRuntimeOwner(std::string_view name,
                                              StructId type,
                                              litestl::util::function<void *()> data)
{
  Result<TypeId> id = rt_.registerId<IdDomain::Type>("owner:" + std::string(name));
  if (!id.ok()) {
    return id.status();
  }
  if (owners_.lookup_ptr(id->value)) {
    return Status::error(StatusCode::AlreadyExists,
                         ("runtime owner '" + std::string(name) + "' is registered").c_str());
  }
  owners_.add(id->value, Owner{type, std::move(data)});
  return OwnerKey::ofRuntime(id->value);
}

PointerRef ApiRegistry::runtimeOwner(const OwnerKey &owner) const
{
  if (owner.kind != OwnerKey::Kind::Runtime) {
    return PointerRef();
  }
  const Owner *o = owners_.lookup_ptr(owner.nameHash);
  if (!o) {
    return PointerRef();
  }
  return PointerRef{o->data(), o->type, owner};
}

PointerRef ApiRegistry::runtimeOwner(std::string_view name) const
{
  return runtimeOwner(OwnerKey::ofRuntime(TypeId::of("owner:" + std::string(name)).value));
}

namespace {

/* A collection's activeMember must be a settable pointer to its items, in the same struct. */
Status validateActive(const ApiRegistry &reg, const ApiStruct &st, const ApiMember &list)
{
  const CollectionAccessor &c = list.collection();
  if (c.activeMember.size() == 0) {
    return Status();
  }
  const ApiMember *m = reg.findMember(st.id(), c.activeMember.c_str());
  const char *why = nullptr;
  if (!c.active) {
    why = "without `active`";
  }
  else if (!m || !m->pointer().set) {
    why = "which isn't a settable pointer of the struct";
  }
  else {
    const StructId target = m->pointer().target;
    bool fits = target && (reg.isA(c.itemStruct, target) || reg.isA(target, c.itemStruct));
    for (StructId t : m->pointer().candidates) {
      fits = fits || reg.isA(t, c.itemStruct);
    }
    why = fits ? nullptr : "which points to another struct";
  }
  return why ? Status::error(StatusCode::InvalidArgument,
                             ("struct '" + std::string(st.name().c_str()) + "': collection '" +
                              list.name() + "' has the activeMember '" +
                              c.activeMember.c_str() + "', " + why)
                                 .c_str()) :
               Status();
}

} // namespace

Status ApiRegistry::validate() const
{
  if (int(byId_.size()) != int(structs_.size())) {
    return Status::error(StatusCode::Internal, "ApiRegistry: index and list differ");
  }
  for (const ApiStruct *s : structs_) {
    if (find(s->id()) != s) {
      return Status::error(StatusCode::Internal, "ApiRegistry: a struct isn't indexed");
    }
    for (int i = 0; i < s->memberCount(); i++) {
      if (s->member(i).kind() == MemberKind::Collection) {
        CDCC_TRY(validateActive(*this, *s, s->member(i)));
      }
    }
  }
  return Status();
}

} // namespace coredcc
