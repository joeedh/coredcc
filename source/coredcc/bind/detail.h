#pragma once

/** \file
 * Shared by bind's .cc files.
 */

#include "coredcc/api/context.h"
#include "coredcc/base/status.h"
#include "coredcc/bind/facades.h"
#include "coredcc/command/command.h"
#include "coredcc/props/def.h"

#include "litestl/binding/binding.h"
#include "litestl/util/alloc.h"

#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

namespace coredcc::script::detail {

inline std::string_view view(const string &s)
{
  return std::string_view(s.c_str(), size_t(s.size()));
}

/** For an id that is checked later: with a NUL, one that is refused. */
inline std::string withoutNul(const string &s)
{
  std::string out(view(s));
  for (char &c : out) {
    c = c == '\0' ? '?' : c;
  }
  return out;
}

inline Status error(StatusCode code, const std::string &message)
{
  return Status::error(code, message.c_str());
}

template <class R, class T> R ok(T value)
{
  R r;
  r.value = std::move(value);
  return r;
}

template <class R> R failed(Status s)
{
  R r;
  r.status = std::move(s);
  return r;
}

/** Busy unless no script slot is running, or the innermost one is one of Command's in
 * `allowed`: a script's discard, run while the host clears the history, must not push
 * into it. */
Status
slotGate(Context &ctx, const char *what, std::initializer_list<const char *> allowed);
/** Busy while the innermost script slot is a graph kernel's or a transform model's. */
Status kernelGate(Context &ctx, const char *what);

/** Runs a command, as a sub-command from a script command's exec. */
Result<PropertyBag> runCommand(Context &ctx, const CommandDef &def, PropertyBag inputs);

/** A def of `defaultValue`'s kind. `what` names the facade in errors. */
Result<PropertyDef> scriptProp(const char *what,
                               const string &name,
                               const PropValue &defaultValue,
                               bool taken);

/** An enum value's key, or flags' keys joined by '|'. NotFound unless items have all of
 * it. */
Result<std::string> keysOf(const PropertyDef &def, const PropValue &value);

/** The words of `slots`. */
std::vector<std::string> splitSlots(const string &slots);

/* Descriptors of result structs: a Status, and a value. */
#define CDCC_RESULT_BINDINGS(T)                                                          \
  const litestl::binding::types::Struct<T> *T::defineBindings()                          \
  {                                                                                      \
    static litestl::binding::types::Struct<T> *st = nullptr;                             \
    if (!st) {                                                                           \
      litestl::alloc::PermanentGuard guard;                                              \
      st = new litestl::binding::types::Struct<T>("coredcc::" #T, sizeof(T));            \
      BIND_STRUCT_COPY_CONSTRUCTOR(st);                                                  \
      BIND_STRUCT_MEMBER(st, status);                                                    \
      BIND_STRUCT_MEMBER(st, value);                                                     \
    }                                                                                    \
    return st;                                                                           \
  }

} // namespace coredcc::script::detail
