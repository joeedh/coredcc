#pragma once

/** \file
 * PointerRef, the transient reference the datapath layer walks with, and Context, what
 * every datapath call runs in (architecture §5).
 */

#include "coredcc/base/runtime.h"
#include "coredcc/lib/block.h"
#include "coredcc/parse/path.h"
#include "coredcc/props/ref.h"

#include <optional>
#include <utility>

namespace litestl::binding::types {
template <typename T> struct Struct;
}

namespace coredcc {

class Library;
class PropertyBag;
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
  /** A non-null root needs an owner (a runtime owner, usually): changes are posted by
   * owner, and canonical paths start at one. */
  Context(Runtime *rt, Library *lib, PointerRef root) : rt_(*rt), lib_(lib), root_(root)
  {
    CDCC_CHECK(root.isNull() || !root.owner.isNull(), "Context: the root has no owner");
  }

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

  /**
   * The active scene: a block of whatever type the host's scenes are (coredcc's own is
   * `coredcc.Scene`); null for none. Setting another bumps the ChangeBus's structural epoch.
   * The handle isn't checked: the block may be deleted or detached later.
   */
  BlockHandle scene() const
  {
    return scene_;
  }
  void setScene(BlockHandle scene);

  /** Paths resolve from `prefix` (itself resolved from the root) until cleared. It names a
   * struct, or a Pointer member, whose target is meant. */
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

  /** The running command's input and output bags, set by the executor around each
   * Command slot call; null outside one. */
  const PropertyBag *inputs() const
  {
    return inputs_;
  }
  PropertyBag *outputs() const
  {
    return outputs_;
  }
  void setCommandBags(const PropertyBag *inputs, PropertyBag *outputs)
  {
    inputs_ = inputs;
    outputs_ = outputs;
  }

  /** Bound without members, so interface slots can take a Context&. */
  static const litestl::binding::types::Struct<Context> *defineBindings();

 private:
  Runtime &rt_;
  Library *lib_;
  PointerRef root_;
  CommandExecutor *executor_ = nullptr;
  BlockHandle scene_;
  Path prefix_;
  bool hasPrefix_ = false;
  const PropertyBag *inputs_ = nullptr;
  PropertyBag *outputs_ = nullptr;
};

/**
 * Sets a Context's path prefix, or clears it (null), for the length of a scope. With
 * `invoke`, it runs a layout's tool: its pointer arguments and the command see the prefix, and
 * so do the command's undo and redo later.
 */
class PrefixScope {
 public:
  PrefixScope(Context &ctx, const Path *prefix) : ctx_(ctx)
  {
    if (const Path *before = ctx.prefix()) {
      before_ = *before;
    }
    if (prefix) {
      ctx.setPrefix(*prefix);
    }
    else {
      ctx.clearPrefix();
    }
  }
  ~PrefixScope()
  {
    if (before_) {
      ctx_.setPrefix(std::move(*before_));
    }
    else {
      ctx_.clearPrefix();
    }
  }
  PrefixScope(const PrefixScope &) = delete;
  PrefixScope &operator=(const PrefixScope &) = delete;

 private:
  Context &ctx_;
  std::optional<Path> before_;
};

} // namespace coredcc
