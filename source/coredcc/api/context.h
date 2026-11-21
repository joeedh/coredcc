#pragma once

/** \file
 * PointerRef, the transient reference the datapath layer walks with, and Context, what
 * every datapath call runs in (architecture §5).
 */

#include "coredcc/base/runtime.h"
#include "coredcc/parse/path.h"
#include "coredcc/props/ref.h"

namespace coredcc {

class Library;
class CommandExecutor;

/**
 * A struct instance during one call: its data, its ApiStruct, and the owner (block or
 * runtime owner) its members belong to. Never stored, never bound. A pointer or
 * collection accessor sets `owner` when it crosses into another block, which is how
 * canonical paths drop hops such as `object.data`.
 */
struct PointerRef {
  void *data = nullptr;
  StructId type;
  OwnerKey owner;

  bool isNull() const
  {
    return data == nullptr;
  }
};

/**
 * What a datapath call runs in. The host builds one per document view: the Runtime, the
 * Library, the root struct (`root`, a host-defined struct, usually a runtime owner),
 * and in P1e the command executor.
 *
 * A **path prefix** makes paths resolve from another struct than the root, as a panel
 * does to run against its own data (Blender's context override). The sticky error is
 * the Runtime's; a Context has none of its own.
 */
class Context {
 public:
  Context(Runtime *rt, Library *lib, PointerRef root) : rt_(*rt), lib_(lib), root_(root) {}

  Runtime &runtime() const
  {
    return rt_;
  }
  Library *library() const
  {
    return lib_;
  }
  const PointerRef &root() const
  {
    return root_;
  }
  CommandExecutor *executor() const
  {
    return executor_;
  }
  void setExecutor(CommandExecutor *executor)
  {
    executor_ = executor;
  }

  /** Paths resolve from `prefix` (itself resolved from the root) until cleared. */
  void setPrefix(Path prefix)
  {
    prefix_ = std::move(prefix);
    hasPrefix_ = true;
  }
  void clearPrefix()
  {
    hasPrefix_ = false;
  }
  const Path *prefix() const
  {
    return hasPrefix_ ? &prefix_ : nullptr;
  }

 private:
  Runtime &rt_;
  Library *lib_;
  PointerRef root_;
  CommandExecutor *executor_ = nullptr;
  Path prefix_;
  bool hasPrefix_ = false;
};

} // namespace coredcc
