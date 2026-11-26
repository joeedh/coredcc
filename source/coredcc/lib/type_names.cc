#include "coredcc/lib/type_names.h"

#include "coredcc/base/runtime.h"

#include "litestl/util/alloc.h"

#include <string>

namespace coredcc {

namespace {
std::string_view viewOf(const litestl::util::string &s)
{
  return std::string_view(s.c_str(), size_t(s.size()));
}
} // namespace

TypeAliases::~TypeAliases()
{
  for (Alias *a : aliases_) {
    litestl::alloc::Delete(a);
  }
}

const TypeAliases::Alias *TypeAliases::find(TypeNameKind kind, std::string_view old) const
{
  for (const Alias *a : aliases_) {
    if (a->kind == kind && viewOf(a->old) == old) {
      return a;
    }
  }
  return nullptr;
}

Status TypeAliases::add(TypeNameKind kind, std::string_view old, std::string_view current)
{
  const std::string what = "type alias '" + std::string(old) + "' -> '" +
                           std::string(current) + "'";
  if (old.empty() || current.empty() || old == current) {
    return Status::error(StatusCode::InvalidArgument,
                         (what + ": the names must differ and not be empty").c_str());
  }
  for (const Alias *a : aliases_) {
    if (a->kind == kind && (viewOf(a->old) == old || viewOf(a->old) == current ||
                            viewOf(a->current) == old))
    {
      return Status::error(StatusCode::AlreadyExists,
                           (what + ": a name is already in an alias").c_str());
    }
  }
  Alias *a = litestl::alloc::New<Alias>("TypeAliases::Alias");
  a->kind = kind;
  a->old = litestl::util::string(std::string(old).c_str());
  a->current = litestl::util::string(std::string(current).c_str());
  aliases_.append(a);
  return Status();
}

std::string_view TypeAliases::current(TypeNameKind kind, std::string_view name) const
{
  const Alias *a = find(kind, name);
  return a ? viewOf(a->current) : name;
}

std::string_view canonicalTypeName(const Runtime &rt, TypeNameKind kind, std::string_view name)
{
  const TypeAliases *aliases = rt.findExt<TypeAliases>();
  return aliases ? aliases->current(kind, name) : name;
}

Status TypeAliases::checkOldNames(const Runtime &rt) const
{
  for (const Alias *a : aliases_) {
    /* The name each registry registers its types under (graph/node_type.cc,
     * graph/socket_type.cc). */
    const char *prefix = a->kind == TypeNameKind::Node     ? "node:"
                         : a->kind == TypeNameKind::Socket ? "socket:"
                                                           : "";
    const std::string registered = prefix + std::string(viewOf(a->old));
    if (rt.nameOf(IdDomain::Type, TypeId::of(registered).value)) {
      return Status::error(StatusCode::FailedPrecondition,
                           ("type alias '" + std::string(viewOf(a->old)) + "' -> '" +
                            std::string(viewOf(a->current)) +
                            "': the old name is still a registered type")
                               .c_str());
    }
  }
  return Status();
}

Status checkTypeAliases(const Runtime &rt)
{
  const TypeAliases *aliases = rt.findExt<TypeAliases>();
  return aliases ? aliases->checkOldNames(rt) : Status();
}

} // namespace coredcc
