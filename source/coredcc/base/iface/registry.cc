#include "coredcc/base/iface/registry.h"

#include <cstring>
#include <string>

namespace coredcc {

using litestl::binding::BindingBase;
using litestl::binding::BindingType;
using litestl::util::string;
using litestl::util::Vector;

void foreignSlotPresent(void *, void **, void *)
{
  CDCC_CHECK(false, "a foreign slot was called without its dispatcher");
}

namespace detail {
void reportCallFailure(const IfaceTable &table, Status status)
{
  if (table.runtime) {
    table.runtime->setStickyError(std::move(status));
  }
}
} // namespace detail

namespace {

std::string describeType(const BindingBase *b)
{
  if (!b) {
    return "void";
  }
  if (b->type == BindingType::Reference) {
    auto *r = static_cast<const lbt::Reference *>(b);
    return (r->isConst ? "const " : "") + describeType(r->refType) + "&";
  }
  if (b->type == BindingType::Pointer) {
    auto *p = static_cast<const lbt::Pointer *>(b);
    return (p->isConst ? "const " : "") + describeType(p->ptrType) + "*";
  }
  return b->buildFullName().c_str();
}

std::string describeParam(const lbt::MethodParam &p)
{
  return describeType(p.type) + (p.isRvalueRef ? "&&" : "");
}

std::string describeMethod(const lbt::Method *m)
{
  std::string s = describeType(m->returnType) + " " + m->name.c_str() + "(";
  for (int i = 0; i < int(m->params.size()); i++) {
    s += (i ? ", " : "") + describeParam(m->params[i]);
  }
  return s + (m->isConst ? ") const" : ")");
}

/* Exact type identity: kind, size, const-ness of pointees, the C++ type key of
 * structs, then the full name. A name alone accepted a type bound under another's name
 * (the prototype), and an enum of another width (the feature review). */
bool sameType(const BindingBase *a, const BindingBase *b)
{
  if (!a || !b) {
    return a == b;
  }
  if (a->type != b->type || a->getSize() != b->getSize()) {
    return false;
  }
  switch (a->type) {
  case BindingType::Reference: {
    auto *ra = static_cast<const lbt::Reference *>(a);
    auto *rb = static_cast<const lbt::Reference *>(b);
    return ra->isConst == rb->isConst && sameType(ra->refType, rb->refType);
  }
  case BindingType::Pointer: {
    auto *pa = static_cast<const lbt::Pointer *>(a);
    auto *pb = static_cast<const lbt::Pointer *>(b);
    return pa->isConst == pb->isConst && sameType(pa->ptrType, pb->ptrType);
  }
  case BindingType::Struct: {
    auto *sa = static_cast<const lbt::_StructBase *>(a);
    auto *sb = static_cast<const lbt::_StructBase *>(b);
    if (sa->typeKey && sb->typeKey && sa->typeKey != sb->typeKey) {
      return false;
    }
    break;
  }
  default:
    break;
  }
  return a->buildFullName() == b->buildFullName();
}

/* Explains two types that print the same but aren't sameType(). */
std::string sameNameNote(const BindingBase *a, const BindingBase *b)
{
  return describeType(a) == describeType(b) ? " (different C++ types with the same name)"
                                            : "";
}

/* Why `impl` can't fill `slot`, or "" if it can. */
std::string mismatch(const lbt::Method *slot, const lbt::Method *impl)
{
  if (!sameType(slot->returnType, impl->returnType)) {
    return "return type " + describeType(impl->returnType) +
           " != " + describeType(slot->returnType) +
           sameNameNote(slot->returnType, impl->returnType);
  }
  if (slot->params.size() != impl->params.size()) {
    return "takes " + std::to_string(impl->params.size()) + " arguments, slot takes " +
           std::to_string(slot->params.size());
  }
  for (int i = 0; i < int(slot->params.size()); i++) {
    const lbt::MethodParam &sp = slot->params[i];
    const lbt::MethodParam &ip = impl->params[i];
    if (!sameType(sp.type, ip.type) || sp.isRvalueRef != ip.isRvalueRef) {
      return "argument " + std::to_string(i + 1) + " ('" + sp.name.c_str() + "') is " +
             describeParam(ip) + ", slot wants " + describeParam(sp) +
             (sp.isRvalueRef == ip.isRvalueRef ? sameNameNote(sp.type, ip.type) : "");
    }
  }
  if (slot->isConst && !impl->isConst) {
    return "slot is const but the method is not";
  }
  return "";
}

bool isPrimitive(const BindingBase *b)
{
  return b && (b->type == BindingType::Number || b->type == BindingType::Boolean);
}

/* The pointee of a Pointer or Reference, else null; `isConst` gets its const-ness. */
const BindingBase *pointee(const BindingBase *t, bool &isConst)
{
  if (t->type == BindingType::Reference) {
    isConst = static_cast<const lbt::Reference *>(t)->isConst;
    return static_cast<const lbt::Reference *>(t)->refType;
  }
  if (t->type == BindingType::Pointer) {
    isConst = static_cast<const lbt::Pointer *>(t)->isConst;
    return static_cast<const lbt::Pointer *>(t)->ptrType;
  }
  return nullptr;
}

/* Declaration rules that can be checked on the descriptor (see iface.h). */
std::string declarationProblem(const lbt::_StructBase *iface)
{
  if (!iface->isInterface) {
    return "not declared with CDCC_INTERFACE";
  }
  const auto &ms = iface->methods;
  for (int i = 0; i < int(ms.size()); i++) {
    const lbt::Method *m = ms[i];
    const std::string name = m->name.c_str();
    const std::string slot = "slot '" + name + "'";
    if (name == "table" || name == "self" || name == "as") {
      return slot + " uses a name reserved by IfaceRef";
    }
    if (m->isStatic) {
      return slot + " is static";
    }
    for (int j = 0; j < i; j++) {
      if (ms[j]->name == m->name) {
        return slot + " is overloaded";
      }
    }
    if (m->returnType && m->returnType->type == BindingType::Reference) {
      return slot + " returns a reference";
    }
    for (const lbt::MethodParam &p : m->params) {
      bool isConst = false;
      const BindingBase *inner = pointee(p.type, isConst);
      if (!inner) {
        continue;
      }
      bool innerConst = false;
      if (pointee(inner, innerConst)) {
        return slot + " has a pointer-to-pointer parameter '" + p.name.c_str() + "'";
      }
      if (isPrimitive(inner) && !isConst) {
        return slot + " has a primitive out-parameter '" + p.name.c_str() + "'";
      }
    }
  }
  return "";
}

std::string toStd(std::string_view s)
{
  return std::string(s);
}

std::string notImplementedMessage(std::string_view implName,
                                  const lbt::_StructBase *iface,
                                  const std::string &errs)
{
  return toStd(implName) + " does not implement " + iface->name.c_str() + ":" + errs;
}

/* Matches `impl` against `iface`, slot by slot. */
Result<IfaceTable *> buildNative(const lbt::_StructBase *iface,
                                 const lbt::_StructBase *impl,
                                 std::string_view implName)
{
  std::string errs;
  Vector<lbt::MethodThunk> thunks;
  for (const lbt::Method *slot : iface->methods) {
    lbt::MethodThunk thunk = nullptr;
    bool found = false;
    std::string why;
    for (const lbt::Method *m : impl->methods) {
      if (m->name != slot->name || m->isStatic) {
        continue;
      }
      found = true;
      std::string w = mismatch(slot, m);
      if (w.empty()) {
        thunk = m->thunk;
        break;
      }
      why += "\n    candidate " + describeMethod(m) + ": " + w;
    }
    thunks.append(thunk);
    if (thunk || (!found && slot->isOptional)) {
      continue;
    }
    errs += std::string("\n  slot ") + slot->name.c_str() + " [" + describeMethod(slot) +
            "]: " + (found ? "no matching signature" + why : "missing");
  }
  if (!errs.empty()) {
    return Status::error(StatusCode::TypeMismatch,
                         notImplementedMessage(implName, iface, errs).c_str());
  }
  IfaceTable *t = litestl::alloc::New<IfaceTable>("coredcc::IfaceTable");
  t->iface = InterfaceId::of(iface->name.c_str());
  t->implType = TypeId::of(implName);
  t->ifaceDesc = iface;
  t->implDesc = impl;
  t->thunks = std::move(thunks);
  return t;
}

/* A foreign table for `iface`: present slots get a placeholder thunk. */
Result<IfaceTable *> buildForeign(const lbt::_StructBase *iface,
                                  std::string_view implName,
                                  std::span<const char *const> presentSlots)
{
  std::string errs;
  Vector<lbt::MethodThunk> thunks;
  for (const lbt::Method *slot : iface->methods) {
    bool present = false;
    for (const char *name : presentSlots) {
      present = present || strcmp(name, slot->name.c_str()) == 0;
    }
    thunks.append(present ? &foreignSlotPresent : nullptr);
    if (!present && !slot->isOptional) {
      errs += std::string("\n  slot ") + slot->name.c_str() + " [" +
              describeMethod(slot) + "]: missing";
    }
  }
  if (!errs.empty()) {
    return Status::error(StatusCode::TypeMismatch,
                         notImplementedMessage(implName, iface, errs).c_str());
  }
  IfaceTable *t = litestl::alloc::New<IfaceTable>("coredcc::IfaceTable");
  t->iface = InterfaceId::of(iface->name.c_str());
  t->implType = TypeId::of(implName);
  t->flags = IfaceFlags::ThreadAffine | IfaceFlags::Foreign;
  t->ifaceDesc = iface;
  t->thunks = std::move(thunks);
  return t;
}

void deleteAll(Vector<IfaceTable *> &tables)
{
  for (IfaceTable *t : tables) {
    litestl::alloc::Delete(t);
  }
  tables.clear();
}

} // namespace

IfaceRegistry::~IfaceRegistry()
{
  deleteAll(tables_);
}

Status IfaceRegistry::addInterface(const lbt::_StructBase *iface,
                                   std::span<const lbt::_StructBase *const> related)
{
  const char *name = iface->name.c_str();
  Result<InterfaceId> id = rt_.registerId<IdDomain::Interface>(name);
  if (!id.ok()) {
    return id.status();
  }
  if (const InterfaceEntry *existing = interfaces_.lookup_ptr(id->value)) {
    if (existing->desc == iface) {
      return Status();
    }
    return Status::error(StatusCode::AlreadyExists,
                         (std::string("interface ") + name +
                          " registered twice (two C++ interfaces with one name?)")
                             .c_str());
  }
  std::string problem = declarationProblem(iface);
  if (!problem.empty()) {
    return Status::error(StatusCode::InvalidArgument,
                         (std::string("interface ") + name + ": " + problem).c_str());
  }
  InterfaceEntry entry;
  entry.desc = iface;
  for (const lbt::_StructBase *r : related) {
    entry.related.append(r);
  }
  interfaces_.add(id->value, std::move(entry));
  return Status();
}

const lbt::_StructBase *IfaceRegistry::findInterface(InterfaceId id) const
{
  const InterfaceEntry *e = interfaces_.lookup_ptr(id.value);
  return e ? e->desc : nullptr;
}

Status IfaceRegistry::storeFamily(Vector<IfaceTable *> &family)
{
  IfaceTable *main = family[0];
  if (find(main->implType, main->iface)) {
    const char *impl = rt_.nameOf(main->implType);
    std::string msg = std::string(impl ? impl : "?") + " already implements " +
                      main->ifaceDesc->name.c_str();
    deleteAll(family);
    return Status::error(StatusCode::AlreadyExists, msg.c_str());
  }
  for (int i = 0; i < int(family.size()); i++) {
    IfaceTable *t = family[i];
    t->runtime = &rt_;
    if (i > 0) {
      if (const IfaceTable *existing = find(t->implType, t->iface)) {
        main->related.append(existing);
        litestl::alloc::Delete(t);
        family[i] = nullptr;
        continue;
      }
      main->related.append(t);
    }
    tables_.append(t);
    byKey_.add(TableKey{t->implType.value, t->iface.value}, t);
  }
  /* Each related table also reaches its own relatives, for upcasts from it. */
  for (int i = 1; i < int(family.size()); i++) {
    IfaceTable *t = family[i];
    if (!t) {
      continue;
    }
    for (const IfaceTable *r : main->related) {
      if (r != t) {
        t->related.append(r);
      }
    }
    t->related.append(main);
  }
  family.clear();
  return Status();
}

Status IfaceRegistry::addImpl(const lbt::_StructBase *iface,
                              const lbt::_StructBase *impl,
                              std::string_view implName,
                              IfaceFlags flags)
{
  const InterfaceEntry *entry =
      interfaces_.lookup_ptr(InterfaceId::of(iface->name.c_str()).value);
  if (!entry || entry->desc != iface) {
    return Status::error(
        StatusCode::NotFound,
        (std::string("interface ") + iface->name.c_str() + " is not registered").c_str());
  }
  Result<TypeId> type = rt_.registerId<IdDomain::Type>(implName);
  if (!type.ok()) {
    return type.status();
  }
  Vector<IfaceTable *> family;
  Result<IfaceTable *> main = buildNative(iface, impl, implName);
  if (!main.ok()) {
    return main.status();
  }
  family.append(*main);
  for (const lbt::_StructBase *r : entry->related) {
    Result<IfaceTable *> t = buildNative(r, impl, implName);
    if (!t.ok()) {
      deleteAll(family);
      return t.status();
    }
    family.append(*t);
  }
  for (IfaceTable *t : family) {
    t->flags = flags;
  }
  return storeFamily(family);
}

Status IfaceRegistry::addForeign(InterfaceId ifaceId,
                                 std::string_view implName,
                                 ForeignDispatcher *dispatcher,
                                 void *foreignClass,
                                 std::span<const char *const> presentSlots)
{
  CDCC_CHECK(dispatcher != nullptr, "a foreign implementation needs a dispatcher");
  const InterfaceEntry *entry = interfaces_.lookup_ptr(ifaceId.value);
  if (!entry) {
    return Status::error(StatusCode::NotFound,
                         "foreign implementation of an unregistered interface");
  }
  Result<TypeId> type = rt_.registerId<IdDomain::Type>(implName);
  if (!type.ok()) {
    return type.status();
  }
  /* Names that match no slot of the interface or of any related interface. */
  std::string unknown;
  for (const char *name : presentSlots) {
    bool known = entry->desc->findMethod(name) != nullptr;
    for (const lbt::_StructBase *r : entry->related) {
      known = known || r->findMethod(name) != nullptr;
    }
    if (!known) {
      unknown += std::string("\n  '") + name + "' is not a slot";
    }
  }
  if (!unknown.empty()) {
    return Status::error(StatusCode::TypeMismatch,
                         notImplementedMessage(implName, entry->desc, unknown).c_str());
  }
  Vector<IfaceTable *> family;
  Result<IfaceTable *> main = buildForeign(entry->desc, implName, presentSlots);
  if (!main.ok()) {
    return main.status();
  }
  family.append(*main);
  for (const lbt::_StructBase *r : entry->related) {
    Result<IfaceTable *> t = buildForeign(r, implName, presentSlots);
    if (!t.ok()) {
      deleteAll(family);
      return t.status();
    }
    family.append(*t);
  }
  for (IfaceTable *t : family) {
    t->dispatcher = dispatcher;
    t->foreignClass = foreignClass;
  }
  return storeFamily(family);
}

const IfaceTable *IfaceRegistry::find(TypeId implType, InterfaceId iface) const
{
  IfaceTable *const *t = byKey_.lookup_ptr(TableKey{implType.value, iface.value});
  return t ? *t : nullptr;
}

Status IfaceRegistry::validateAll() const
{
  std::string errs;
  auto fail = [&errs](const IfaceTable *t, const char *what) {
    errs += std::string("\n  ") + t->ifaceDesc->name.c_str() + ": " + what;
  };
  for (const IfaceTable *t : tables_) {
    std::string problem = declarationProblem(t->ifaceDesc);
    if (!problem.empty()) {
      errs += "\n  " + problem;
    }
    if (findInterface(t->iface) != t->ifaceDesc) {
      fail(t, "interface not registered under its id");
    }
    if (!rt_.nameOf(t->implType)) {
      fail(t, "implementation type name not registered");
    }
    if (find(t->implType, t->iface) != t) {
      fail(t, "table missing from the lookup map");
    }
    if (int(t->thunks.size()) != int(t->ifaceDesc->methods.size())) {
      fail(t, "thunk count differs from slot count");
    }
    if (hasFlag(t->flags, IfaceFlags::Foreign) != (t->dispatcher != nullptr)) {
      fail(t, "foreign flag and dispatcher disagree");
    }
    for (int i = 0; i < int(t->thunks.size()); i++) {
      if (!t->thunks[i] && !t->ifaceDesc->methods[i]->isOptional) {
        fail(t, "required slot without a thunk");
      }
    }
    for (const IfaceTable *r : t->related) {
      if (!tables_.contains(const_cast<IfaceTable *>(r)) || r->implType != t->implType) {
        fail(t, "related table not stored, or of another implementation");
      }
    }
  }
  if (int(byKey_.size()) != int(tables_.size())) {
    errs += "\n  lookup map and table list differ in size";
  }
  if (!errs.empty()) {
    return Status::error(StatusCode::Internal, ("validateAll failed:" + errs).c_str());
  }
  return Status();
}

const lbt::_StructBase *IfaceRegistry::cachedImplDesc(TypeId implType) const
{
  const lbt::_StructBase *const *d = implDescs_.lookup_ptr(implType.value);
  return d ? *d : nullptr;
}

void IfaceRegistry::cacheImplDesc(TypeId implType, const lbt::_StructBase *desc)
{
  implDescs_.add(implType.value, desc);
}

} // namespace coredcc
