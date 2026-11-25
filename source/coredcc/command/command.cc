#include "coredcc/command/command.h"

#include "coredcc/base/text.h"
#include "coredcc/parse/invocation.h"

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

Status CommandRegistry::checkDef(const CommandDef &def) const
{
  const std::string name = def.name().c_str();
  if (name.empty()) {
    return Status::error(StatusCode::InvalidArgument, "command '': an empty id");
  }
  /* Ids the invocation parser can't read could never be invoked. */
  for (size_t start = 0;;) {
    const size_t dot = name.find('.', start);
    const std::string_view part = std::string_view(name).substr(start, dot - start);
    if (!isIdentifier(part)) {
      return Status::error(StatusCode::InvalidArgument,
                           ("command '" + name +
                            "': not a dotted identifier (like 'mesh.extrude')")
                               .c_str());
    }
    if (dot == std::string::npos) {
      break;
    }
    start = dot + 1;
  }
  if (find(name)) {
    return Status::error(StatusCode::AlreadyExists,
                         ("command '" + name + "': already registered").c_str());
  }
  const char *taken = rt_.nameOf(CommandId::of(name));
  if (taken && name != taken) {
    return Status::error(StatusCode::AlreadyExists,
                         ("command '" + name + "': its id collides with '" + taken + "'").c_str());
  }
  CDCC_TRY(checkBag(name, "input", def, true));
  return checkBag(name, "output", def, false);
}

Status CommandRegistry::checkSlots(const CommandDef &def,
                                   bool hasRedo,
                                   bool hasFoldKey,
                                   bool hasFoldFrom)
{
  const std::string name = def.name().c_str();
  if (def.has(CommandFlags::Interactive) && !hasRedo) {
    return Status::error(StatusCode::InvalidArgument,
                         ("command '" + name +
                          "': an Interactive command needs a redo slot (exec can't replay a drag)")
                             .c_str());
  }
  if (hasFoldKey != hasFoldFrom) {
    return Status::error(StatusCode::InvalidArgument,
                         ("command '" + name +
                          "': foldKey and foldFrom come together: implement both or neither")
                             .c_str());
  }
  return Status();
}

Result<const CommandDef *> CommandRegistry::add(CommandDef def)
{
  const std::string name = def.name().c_str();
  auto fail = [&name](StatusCode code, const std::string &why) {
    return Status::error(code, ("command '" + name + "': " + why).c_str());
  };
  CDCC_TRY(checkDef(def));
  Result<CommandId> id = rt_.registerId<IdDomain::Command>(name);
  if (!id.ok()) {
    return id.status();
  }
  if (!def.hasImpl()) {
    return fail(StatusCode::InvalidArgument, "no implementation (CommandDef::impl)");
  }
  const IfaceTable *table =
      rt_.ifaces().find(def.implType(), InterfaceId::of(Command::ifaceName));
  if (!table) {
    return fail(StatusCode::NotFound, "its implementation isn't registered as a Command");
  }
  auto has = [table](const char *slot) {
    return table->hasSlot(detail::slotOf<Command>(slot).index);
  };
  CDCC_TRY(checkSlots(def, has("redo"), has("foldKey"), has("foldFrom")));
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

Status detail::unknownInput(const CommandDef &def, const InvocationArg &arg)
{
  Status err = Status::error(StatusCode::NotFound,
                             (std::string(def.name().c_str()) + " has no input '" +
                              arg.name.c_str() + "' (at offset " + std::to_string(arg.offset) + ")")
                                 .c_str());
  Suggester sg(arg.name.c_str());
  for (int i = 0; i < def.inputCount(); i++) {
    sg.consider(def.inputDef(i).apiName().c_str());
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
