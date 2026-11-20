#include "coredcc/base/iface/registry.h"

#include <cstring>
#include <string>

namespace coredcc {

using litestl::binding::BindingBase;
using litestl::binding::BindingType;
using litestl::util::string;

void foreignSlotPresent(void *, void **, void *)
{
  CDCC_CHECK(false, "a foreign slot was called without its dispatcher");
}

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

std::string describeMethod(const lbt::Method *m)
{
  std::string s = describeType(m->returnType) + " " + m->name.c_str() + "(";
  for (int i = 0; i < int(m->params.size()); i++) {
    s += (i ? ", " : "") + describeType(m->params[i].type);
  }
  return s + (m->isConst ? ") const" : ")");
}

/* Exact type identity: kind, const-ness of pointees, and for structs the full name,
 * size and C++ type key (a name alone accepted a type bound under another's name). */
bool sameType(const BindingBase *a, const BindingBase *b)
{
  if (!a || !b) {
    return a == b;
  }
  if (a->type != b->type) {
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
      if (sa->structSize != sb->structSize) {
        return false;
      }
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
  return describeType(a) == describeType(b) ? " (different C++ types with the same name)" : "";
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
    if (!sameType(slot->params[i].type, impl->params[i].type)) {
      return "argument " + std::to_string(i + 1) + " ('" + slot->params[i].name.c_str() +
             "') is " + describeType(impl->params[i].type) + ", slot wants " +
             describeType(slot->params[i].type) +
             sameNameNote(slot->params[i].type, impl->params[i].type);
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

/* Declaration rules that can be checked on the descriptor (see iface.h). */
std::string declarationProblem(const lbt::_StructBase *iface)
{
  if (!iface->isInterface) {
    return "not declared with CDCC_INTERFACE";
  }
  const auto &ms = iface->methods;
  for (int i = 0; i < int(ms.size()); i++) {
    const lbt::Method *m = ms[i];
    for (int j = 0; j < i; j++) {
      if (ms[j]->name == m->name) {
        return std::string("slot '") + m->name.c_str() + "' is overloaded";
      }
    }
    if (m->returnType && m->returnType->type == BindingType::Reference) {
      return std::string("slot '") + m->name.c_str() + "' returns a reference";
    }
    for (const lbt::MethodParam &p : m->params) {
      const BindingBase *t = p.type;
      const BindingBase *inner = nullptr;
      bool isConstRef = false;
      if (t->type == BindingType::Reference) {
        inner = static_cast<const lbt::Reference *>(t)->refType;
        isConstRef = static_cast<const lbt::Reference *>(t)->isConst;
      } else if (t->type == BindingType::Pointer) {
        inner = static_cast<const lbt::Pointer *>(t)->ptrType;
      }
      if (inner && isPrimitive(inner) && !isConstRef) {
        return std::string("slot '") + m->name.c_str() + "' has a primitive out-parameter '" +
               p.name.c_str() + "'";
      }
    }
  }
  return "";
}

std::string toStd(std::string_view s)
{
  return std::string(s);
}

} // namespace

IfaceRegistry::~IfaceRegistry()
{
  for (IfaceTable *t : tables_) {
    litestl::alloc::Delete(t);
  }
}

Status IfaceRegistry::addInterface(const lbt::_StructBase *iface)
{
  const char *name = iface->name.c_str();
  Result<InterfaceId> id = rt_.registerId<IdDomain::Interface>(name);
  if (!id.ok()) {
    return id.status();
  }
  if (const lbt::_StructBase *const *existing = interfaces_.lookup_ptr(id->value)) {
    if (*existing == iface) {
      return Status();
    }
    return Status::error(StatusCode::AlreadyExists,
                         (std::string("interface ") + name + " registered twice").c_str());
  }
  std::string problem = declarationProblem(iface);
  if (!problem.empty()) {
    return Status::error(StatusCode::InvalidArgument,
                         (std::string("interface ") + name + ": " + problem).c_str());
  }
  interfaces_.add(id->value, iface);
  return Status();
}

const lbt::_StructBase *IfaceRegistry::findInterface(InterfaceId id) const
{
  const lbt::_StructBase *const *d = interfaces_.lookup_ptr(id.value);
  return d ? *d : nullptr;
}

Result<IfaceTable *> IfaceRegistry::buildNative(const lbt::_StructBase *iface,
                                                const lbt::_StructBase *impl,
                                                std::string_view implName)
{
  std::string errs;
  IfaceTable *t = litestl::alloc::New<IfaceTable>("coredcc::IfaceTable");
  t->iface = InterfaceId::of(iface->name.c_str());
  t->implType = TypeId::of(implName);
  t->ifaceDesc = iface;
  t->implDesc = impl;
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
    t->thunks.append(thunk);
    if (thunk || (!found && slot->isOptional)) {
      continue;
    }
    errs += std::string("\n  slot ") + slot->name.c_str() + " [" + describeMethod(slot) +
            "]: " + (found ? "no matching signature" + why : "missing");
  }
  if (!errs.empty()) {
    litestl::alloc::Delete(t);
    return Status::error(StatusCode::TypeMismatch,
                         (toStd(implName) + " does not implement " + iface->name.c_str() +
                          ":" + errs)
                             .c_str());
  }
  return t;
}

Status IfaceRegistry::store(IfaceTable *table)
{
  TableKey key{table->implType.value, table->iface.value};
  if (byKey_.contains(key)) {
    const char *impl = rt_.nameOf(table->implType);
    const char *iface = table->ifaceDesc->name.c_str();
    litestl::alloc::Delete(table);
    return Status::error(StatusCode::AlreadyExists,
                         (std::string(impl ? impl : "?") + " already implements " + iface)
                             .c_str());
  }
  tables_.append(table);
  byKey_.add(key, table);
  return Status();
}

Status IfaceRegistry::addImpl(const lbt::_StructBase *iface,
                              const lbt::_StructBase *impl,
                              std::string_view implName,
                              std::span<const lbt::_StructBase *const> upcastIfaces)
{
  const TypeId implType = TypeId::of(implName);
  if (find(implType, InterfaceId::of(iface->name.c_str()))) {
    return Status::error(
        StatusCode::AlreadyExists,
        (toStd(implName) + " already implements " + iface->name.c_str()).c_str());
  }
  Result<IfaceTable *> main = buildNative(iface, impl, implName);
  if (!main.ok()) {
    return main.status();
  }
  IfaceTable *table = *main;
  /* Build every upcast table before storing anything, so a failure leaves no trace. */
  litestl::util::Vector<IfaceTable *> extra;
  for (const lbt::_StructBase *u : upcastIfaces) {
    Result<IfaceTable *> t = buildNative(u, impl, implName);
    if (!t.ok()) {
      litestl::alloc::Delete(table);
      for (IfaceTable *e : extra) {
        litestl::alloc::Delete(e);
      }
      return t.status();
    }
    extra.append(*t);
    table->upcasts.append(*t);
  }
  CDCC_TRY(store(table));
  for (IfaceTable *e : extra) {
    if (byKey_.contains(TableKey{e->implType.value, e->iface.value})) {
      tables_.append(e); /* owned, but the existing table stays the queryable one */
      continue;
    }
    CDCC_TRY(store(e));
  }
  return Status();
}

Status IfaceRegistry::addForeign(InterfaceId ifaceId,
                                 std::string_view implName,
                                 ForeignDispatcher *dispatcher,
                                 void *foreignClass,
                                 std::span<const char *const> presentSlots)
{
  CDCC_CHECK(dispatcher != nullptr, "a foreign implementation needs a dispatcher");
  const lbt::_StructBase *iface = findInterface(ifaceId);
  if (!iface) {
    return Status::error(StatusCode::NotFound, "foreign implementation of an unregistered interface");
  }
  std::string errs;
  for (const char *name : presentSlots) {
    if (!iface->findMethod(name)) {
      errs += std::string("\n  '") + name + "' is not a slot";
    }
  }
  IfaceTable *t = litestl::alloc::New<IfaceTable>("coredcc::IfaceTable");
  t->iface = ifaceId;
  t->implType = TypeId::of(implName);
  t->flags = IfaceFlags::ThreadAffine | IfaceFlags::Foreign;
  t->ifaceDesc = iface;
  t->dispatcher = dispatcher;
  t->foreignClass = foreignClass;
  for (const lbt::Method *slot : iface->methods) {
    bool present = false;
    for (const char *name : presentSlots) {
      present = present || strcmp(name, slot->name.c_str()) == 0;
    }
    t->thunks.append(present ? &foreignSlotPresent : nullptr);
    if (!present && !slot->isOptional) {
      errs += std::string("\n  slot ") + slot->name.c_str() + " [" + describeMethod(slot) +
              "]: missing";
    }
  }
  if (!errs.empty()) {
    litestl::alloc::Delete(t);
    return Status::error(StatusCode::TypeMismatch,
                         (toStd(implName) + " does not implement " + iface->name.c_str() +
                          ":" + errs)
                             .c_str());
  }
  return store(t);
}

const IfaceTable *IfaceRegistry::find(TypeId implType, InterfaceId iface) const
{
  IfaceTable *const *t = byKey_.lookup_ptr(TableKey{implType.value, iface.value});
  return t ? *t : nullptr;
}

Status IfaceRegistry::validateAll() const
{
  std::string errs;
  for (const IfaceTable *t : tables_) {
    std::string problem = declarationProblem(t->ifaceDesc);
    if (!problem.empty()) {
      errs += std::string("\n") + t->ifaceDesc->name.c_str() + ": " + problem;
      continue;
    }
    if (hasFlag(t->flags, IfaceFlags::Foreign)) {
      continue; /* checked against the foreign class when it was implemented */
    }
    const char *implName = rt_.nameOf(t->implType);
    Result<IfaceTable *> rebuilt = const_cast<IfaceRegistry *>(this)->buildNative(
        t->ifaceDesc, t->implDesc, implName ? implName : "?");
    if (!rebuilt.ok()) {
      errs += std::string("\n") + rebuilt.status().message();
      continue;
    }
    bool same = (*rebuilt)->thunks.size() == t->thunks.size();
    for (int i = 0; same && i < int(t->thunks.size()); i++) {
      same = (*rebuilt)->thunks[i] == t->thunks[i];
    }
    litestl::alloc::Delete(*rebuilt);
    if (!same) {
      errs += std::string("\n") + (implName ? implName : "?") + " as " +
              t->ifaceDesc->name.c_str() + ": table is stale";
    }
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
