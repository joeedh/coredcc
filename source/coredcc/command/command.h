#pragma once

/** \file
 * Command, an interface (§2a), and CommandDef/CommandRegistry, where commands are
 * declared (architecture §6).
 */

#include "coredcc/api/context.h"
#include "coredcc/base/flags.h"
#include "coredcc/base/iface/iface.h"
#include "coredcc/base/iface/owned.h"
#include "coredcc/props/bag.h"
#include "coredcc/props/def.h"

#include "litestl/util/function.h"
#include "litestl/util/map.h"
#include "litestl/util/vector.h"

#include <cstring>
#include <string>
#include <string_view>

namespace coredcc {

/**
 * One run of a command. The executor owns it (`Owned<Command>`) and keeps its input and
 * output bags, which the slots read through `ctx.inputs()` and `ctx.outputs()`.
 *
 * - `canRun`: ok, or why not (FailedPrecondition with a reason for the UI).
 * - `exec`: does the work and captures what undo needs. On failure the executor undoes
 *   the sub-commands it ran, and the command is dropped without its own `undo`, so exec
 *   must leave nothing half-done.
 * - `undo`, and the optional `redo`. Without `redo`, redo runs `exec` again: right
 *   only for commands whose result depends on their inputs alone. Interactive
 *   commands must have `redo`.
 * - `memSize`: bytes held for undo, for the history's budget.
 * - `discard`: the command leaves the history for good (redo truncation, the budget);
 *   release journal steps and the like. The object is destroyed after.
 * - `foldKey` (optional; 0 means never) and `foldFrom`: a command pushed with the same
 *   key as the head is folded into it: the head absorbs `next` (downcast with
 *   `next.as<T>()`) and returns true, and `next` is dropped.
 */
struct Command : IfaceRef {
  Status canRun(Context &ctx) const
  {
    return CDCC_CALL(canRun, ctx);
  }
  Status exec(Context &ctx)
  {
    return CDCC_CALL(exec, ctx);
  }
  void undo(Context &ctx)
  {
    CDCC_CALL(undo, ctx);
  }
  bool hasRedo() const
  {
    return CDCC_HAS(redo);
  }
  Status redo(Context &ctx)
  {
    return CDCC_CALL(redo, ctx);
  }
  uint64_t memSize() const
  {
    return CDCC_CALL(memSize);
  }
  void discard()
  {
    CDCC_CALL(discard);
  }
  bool hasFold() const
  {
    return CDCC_HAS(foldKey);
  }
  uint64_t foldKey() const
  {
    return CDCC_CALL(foldKey);
  }
  bool foldFrom(Command next)
  {
    return CDCC_CALL(foldFrom, next);
  }

  CDCC_INTERFACE(Command, "coredcc::Command")
  {
    BIND_STRUCT_METHOD(st, canRun, MARGS("ctx"));
    BIND_STRUCT_METHOD(st, exec, MARGS("ctx"));
    BIND_STRUCT_METHOD(st, undo, MARGS("ctx"));
    BIND_OPTIONAL_METHOD(st, redo, MARGS("ctx"));
    BIND_STRUCT_METHOD(st, memSize, MARGS());
    BIND_STRUCT_METHOD(st, discard, MARGS());
    BIND_OPTIONAL_METHOD(st, foldKey, MARGS());
    BIND_OPTIONAL_METHOD(st, foldFrom, MARGS("next"));
  }
};

enum class CommandFlags : uint32_t {
  None = 0,
  /** Hidden from menus, search and bindings listings. */
  Private = 1 << 0,
  /** Runs without entering the history. */
  NoUndo = 1 << 1,
  /** Pushes a barrier: nothing before it can be undone. */
  UndoBarrier = 1 << 2,
  /** Runs through beginInteractive/commit/cancel; needs a `redo` slot. */
  Interactive = 1 << 3,
};
CDCC_FLAG_OPERATORS(CommandFlags)

/** Makes the command instance for one run. */
using CommandFactory = litestl::util::function<Result<Owned<Command>>(Runtime &rt)>;

/** A declared command. Register it with CommandRegistry, which makes it immutable. */
class CommandDef {
public:
  /** `id` is dotted, e.g. "mesh.extrude". */
  explicit CommandDef(std::string_view id);

  /** The Command implementation and the factory making its instances. */
  CommandDef &impl(std::string_view implName, CommandFactory factory);
  CommandDef &uiName(std::string_view s);
  CommandDef &description(std::string_view s);
  CommandDef &icon(std::string_view s);
  CommandDef &flags(CommandFlags f);
  CommandDef &input(PropertyDef def);
  CommandDef &output(PropertyDef def);

  CommandId id() const
  {
    return id_;
  }
  const litestl::util::string &name() const
  {
    return name_;
  }
  const litestl::util::string &uiNameText() const
  {
    return uiName_;
  }
  const litestl::util::string &descriptionText() const
  {
    return description_;
  }
  const litestl::util::string &iconText() const
  {
    return icon_;
  }
  CommandFlags flagSet() const
  {
    return flags_;
  }
  bool has(CommandFlags f) const
  {
    return (flags_ & f) != CommandFlags::None;
  }
  TypeId implType() const
  {
    return implType_;
  }
  int inputCount() const
  {
    return int(inputs_.size());
  }
  const PropertyDef &inputDef(int i) const
  {
    return inputs_[i];
  }
  int outputCount() const
  {
    return int(outputs_.size());
  }
  const PropertyDef &outputDef(int i) const
  {
    return outputs_[i];
  }
  const PropertyDef *findInput(std::string_view name) const;
  /** A bag of the inputs at their defaults, and one of the outputs. */
  PropertyBag newInputs() const;
  PropertyBag newOutputs() const;
  bool hasImpl() const
  {
    return bool(factory_);
  }
  Result<Owned<Command>> instantiate(Runtime &rt) const
  {
    return factory_(rt);
  }

  /** Bound opaque, for bind/'s builder facades (newCommandDef and the def* calls). */
  static const litestl::binding::types::Struct<CommandDef> *defineBindings();

private:
  CommandId id_;
  litestl::util::string name_;
  litestl::util::string uiName_;
  litestl::util::string description_;
  litestl::util::string icon_;
  CommandFlags flags_ = CommandFlags::None;
  TypeId implType_;
  litestl::util::Vector<PropertyDef, 1> inputs_;
  litestl::util::Vector<PropertyDef, 1> outputs_;
  CommandFactory factory_;
};

/** Registered commands. A Runtime extension. */
class CommandRegistry {
public:
  static constexpr const char *kExtName = "coredcc.CommandRegistry";

  explicit CommandRegistry(Runtime *rt) : rt_(*rt)
  {
  }
  ~CommandRegistry();
  CommandRegistry(const CommandRegistry &) = delete;
  CommandRegistry &operator=(const CommandRegistry &) = delete;

  /**
   * Registers `def` after checking it: a unique id, valid and uniquely named inputs and
   * outputs, a factory, and an implementation registered for Command. An Interactive
   * def whose implementation has no `redo` slot is rejected, and so is one with only
   * one of `foldKey` and `foldFrom`.
   */
  Result<const CommandDef *> add(CommandDef def);
  /** add's checks on the def alone (id, bags), without its implementation: for callers
   * that register the implementation first and must not leave it behind on failure. */
  Status checkDef(const CommandDef &def) const;
  /** add's checks on the implementation's slots, from which of them it has: an
   * Interactive command needs `redo`, and `foldKey` comes with `foldFrom`. */
  static Status
  checkSlots(const CommandDef &def, bool hasRedo, bool hasFoldKey, bool hasFoldFrom);
  const CommandDef *find(CommandId id) const;
  const CommandDef *find(std::string_view id) const;
  /** NotFound for `id`, with suggestions. */
  Status unknown(std::string_view id) const;
  int size() const
  {
    return int(defs_.size());
  }
  const CommandDef &at(int i) const
  {
    return *defs_[i];
  }

  Status validate() const;

private:
  Runtime &rt_;
  litestl::util::Vector<CommandDef *> defs_;
  litestl::util::Map<uint64_t, CommandDef *> byId_;
};

struct InvocationArg;

namespace detail {
/** NotFound for an argument that `def` has no input for, with suggestions. */
Status unknownInput(const CommandDef &def, const InvocationArg &arg);
} // namespace detail

/**
 * Registers Impl as a Command implementation and a def whose factory makes a
 * default-constructed Impl. For native commands; foreign ones pass their own factory.
 */
template <class Impl>
Result<const CommandDef *> addNativeCommand(Runtime &rt, CommandDef def);

} // namespace coredcc

#include "coredcc/base/iface/registry.h"

namespace coredcc {

template <class Impl>
Result<const CommandDef *> addNativeCommand(Runtime &rt, CommandDef def)
{
  const std::string impl = std::string(def.name().c_str()) + "#impl";
  /* Check first: a failed add after registerImpl would leave the impl name taken. */
  if (Status s = rt.ext<CommandRegistry>().checkDef(def); !s.ok()) {
    return s;
  }
  const lbt::_StructBase *desc = nullptr;
  {
    litestl::alloc::PermanentGuard guard; /* as registerImpl does */
    desc = Impl::defineBindings();
  }
  auto has = [desc](const char *slot) {
    for (const lbt::Method *m : desc->methods) {
      if (strcmp(m->name.c_str(), slot) == 0) {
        return true;
      }
    }
    return false;
  };
  if (Status s =
          CommandRegistry::checkSlots(def, has("redo"), has("foldKey"), has("foldFrom"));
      !s.ok())
  {
    return s;
  }
  if (Status s = registerImpl<Command, Impl>(rt, impl); !s.ok()) {
    return s;
  }
  def.impl(impl, [impl](Runtime &r) {
    return own<Command>(r, impl, litestl::alloc::New<Impl>("coredcc::Command"));
  });
  return rt.ext<CommandRegistry>().add(std::move(def));
}

} // namespace coredcc

namespace litestl::binding {
/** CommandFlags, bound as an enum (command/bindings.cc). */
template <> struct Binder<coredcc::CommandFlags> {
  static const BindingBase *bind();
};
} // namespace litestl::binding
