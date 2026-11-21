#include "coredcc/command/command.h"

#include "coredcc/base/text.h"

#include <string>

namespace coredcc {

CommandDef::CommandDef(std::string_view id)
    : id_(CommandId::of(id)), name_(lsString(id)), uiName_(lsString(uiNameFromApiName(id)))
{
}

CommandDef &CommandDef::impl(std::string_view implName, CommandFactory factory)
{
  implType_ = TypeId::of(implName);
  factory_ = std::move(factory);
  return *this;
}

CommandDef &CommandDef::uiName(std::string_view s)
{
  uiName_ = lsString(s);
  return *this;
}

CommandDef &CommandDef::description(std::string_view s)
{
  description_ = lsString(s);
  return *this;
}

CommandDef &CommandDef::icon(std::string_view s)
{
  icon_ = lsString(s);
  return *this;
}

CommandDef &CommandDef::flags(CommandFlags f)
{
  flags_ = f;
  return *this;
}

CommandDef &CommandDef::input(PropertyDef def)
{
  inputs_.append(std::move(def));
  return *this;
}

CommandDef &CommandDef::output(PropertyDef def)
{
  outputs_.append(std::move(def));
  return *this;
}

const PropertyDef *CommandDef::findInput(std::string_view name) const
{
  for (const PropertyDef &d : inputs_) {
    if (name == d.apiName().c_str()) {
      return &d;
    }
  }
  return nullptr;
}

PropertyBag CommandDef::newInputs() const
{
  PropertyBag bag;
  for (const PropertyDef &d : inputs_) {
    bag.add(d).ignore(); /* checked at registration */
  }
  return bag;
}

PropertyBag CommandDef::newOutputs() const
{
  PropertyBag bag;
  for (const PropertyDef &d : outputs_) {
    bag.add(d).ignore();
  }
  return bag;
}

/* ---- registry ---- */

CommandRegistry::~CommandRegistry()
{
  for (CommandDef *d : defs_) {
    litestl::alloc::Delete(d);
  }
}

namespace {

Status checkBag(const std::string &cmd,
                const char *what,
                const CommandDef &def,
                bool inputs)
{
  PropertyBag bag;
  const int n = inputs ? def.inputCount() : def.outputCount();
  for (int i = 0; i < n; i++) {
    Status s = bag.add(inputs ? def.inputDef(i) : def.outputDef(i));
    if (!s.ok()) {
      return Status::error(s.code(),
                           ("command '" + cmd + "' " + what + ": " + s.message()).c_str());
    }
  }
  return Status();
}

} // namespace

Result<const CommandDef *> CommandRegistry::add(CommandDef def)
{
  const std::string name = def.name().c_str();
  auto fail = [&name](StatusCode code, const std::string &why) {
    return Status::error(code, ("command '" + name + "': " + why).c_str());
  };
  if (name.empty()) {
    return fail(StatusCode::InvalidArgument, "an empty id");
  }
  Result<CommandId> id = rt_.registerId<IdDomain::Command>(name);
  if (!id.ok()) {
    return id.status();
  }
  if (byId_.lookup_ptr(id->value)) {
    return fail(StatusCode::AlreadyExists, "already registered");
  }
  CDCC_TRY(checkBag(name, "input", def, true));
  CDCC_TRY(checkBag(name, "output", def, false));
  if (!def.hasImpl()) {
    return fail(StatusCode::InvalidArgument, "no implementation (CommandDef::impl)");
  }
  const IfaceTable *table =
      rt_.ifaces().find(def.implType(), InterfaceId::of(Command::ifaceName));
  if (!table) {
    return fail(StatusCode::NotFound, "its implementation isn't registered as a Command");
  }
  if (def.has(CommandFlags::Interactive)) {
    const detail::SlotRef redo = detail::slotOf<Command>("redo");
    if (!table->hasSlot(redo.index)) {
      return fail(StatusCode::InvalidArgument,
                  "an Interactive command needs a redo slot (exec can't replay a drag)");
    }
  }
  CommandDef *stored = litestl::alloc::New<CommandDef>("coredcc::CommandDef", std::move(def));
  defs_.append(stored);
  byId_.add(id->value, stored);
  return static_cast<const CommandDef *>(stored);
}

const CommandDef *CommandRegistry::find(CommandId id) const
{
  CommandDef *const *d = byId_.lookup_ptr(id.value);
  return d ? *d : nullptr;
}

const CommandDef *CommandRegistry::find(std::string_view id) const
{
  return find(CommandId::of(id));
}

Status CommandRegistry::unknown(std::string_view id) const
{
  Status err = Status::error(StatusCode::NotFound,
                             ("no command '" + std::string(id) + "'").c_str());
  Suggester sg(id);
  for (const CommandDef *d : defs_) {
    sg.consider(d->name().c_str());
  }
  sg.addTo(err);
  return err;
}

Status CommandRegistry::validate() const
{
  if (int(byId_.size()) != int(defs_.size())) {
    return Status::error(StatusCode::Internal, "CommandRegistry: index and list differ");
  }
  for (const CommandDef *d : defs_) {
    if (find(d->id()) != d) {
      return Status::error(StatusCode::Internal, "CommandRegistry: a def isn't indexed");
    }
  }
  return Status();
}

} // namespace coredcc
