#pragma once

#include "coredcc/base/id.h"
#include "coredcc/base/status.h"

#include "litestl/util/string.h"
#include "litestl/util/vector.h"

#include <string_view>

namespace coredcc {

class Runtime;

/** The kinds of stored type name. Each kind has its own aliases. */
enum class TypeNameKind : int32_t {
  Block,
  Attachment,
  Slot,
  Transform,
  Node,
  Socket,
};

/**
 * Old names of renamed types (`rt.ext<TypeAliases>()`). A file that names a type by its
 * old name loads as if it named the current one, and saves the current name. Aliases are
 * between names, not ids: the sites that read a type name keep the name, and must keep
 * the current one.
 */
class TypeAliases {
public:
  static constexpr const char *kExtName = "coredcc.TypeAliases";
  explicit TypeAliases(Runtime *) {}
  ~TypeAliases();
  TypeAliases(const TypeAliases &) = delete;
  TypeAliases &operator=(const TypeAliases &) = delete;

  /** `old` is read as `current`. InvalidArgument for an empty name or `old == current`;
   * AlreadyExists if `old` already is an alias, or if either name is the other side of
   * an alias of this kind (aliases don't chain). */
  Status add(TypeNameKind kind, std::string_view old, std::string_view current);

  /** The current name for `name`: what it is an alias of, or `name` itself. A view of
   * the table's copy (valid while the Runtime lives) or of `name`. */
  std::string_view current(TypeNameKind kind, std::string_view name) const;

  /** checkTypeAliases. */
  Status checkOldNames(const Runtime &rt) const;

private:
  struct Alias {
    TypeNameKind kind = TypeNameKind::Block;
    litestl::util::string old;
    litestl::util::string current;
  };
  const Alias *find(TypeNameKind kind, std::string_view old) const;
  /* Allocated one by one, so a view of `current` stays valid as the table grows. */
  litestl::util::Vector<Alias *> aliases_;
};

/** The current name of a stored type name (TypeAliases::current), without creating the
 * extension. */
std::string_view
canonicalTypeName(const Runtime &rt, TypeNameKind kind, std::string_view name);

/** FailedPrecondition if an alias's old name is still a registered type of its kind:
 * every stored use of it would load as the current type. loadLibrary and appendLibrary
 * check this before they read a file. */
Status checkTypeAliases(const Runtime &rt);

} // namespace coredcc

namespace coredcc::detail {

/** The id of a block type, attachment type or slot namespace by a name that was stored,
 * or that a caller gave as text; null for an empty name. A name read from a file goes
 * through canonicalTypeName first. */
inline TypeId typeNamed(std::string_view name)
{
  return name.empty() ? TypeId() : TypeId::of(name);
}

} // namespace coredcc::detail
