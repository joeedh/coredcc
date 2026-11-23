/* Binding descriptors for command types. Command itself is bound by CDCC_INTERFACE. */
#include "coredcc/command/command.h"

#include "litestl/binding/binding.h"
#include "litestl/util/alloc.h"

const litestl::binding::BindingBase *litestl::binding::Binder<coredcc::CommandFlags>::bind()
{
  using coredcc::CommandFlags;
  static types::Enum *e = nullptr;
  if (!e) {
    alloc::PermanentGuard guard;
    e = new types::Enum("coredcc::CommandFlags", sizeof(CommandFlags));
    e->isBitMask = true;
    e->addItem("None", CommandFlags::None);
    e->addItem("Private", CommandFlags::Private);
    e->addItem("NoUndo", CommandFlags::NoUndo);
    e->addItem("UndoBarrier", CommandFlags::UndoBarrier);
    e->addItem("Interactive", CommandFlags::Interactive);
  }
  return e;
}

namespace coredcc {

const lbt::Struct<CommandDef> *CommandDef::defineBindings()
{
  static lbt::Struct<CommandDef> *st = nullptr;
  if (!st) {
    litestl::alloc::PermanentGuard guard;
    st = new lbt::Struct<CommandDef>("coredcc::CommandDef", sizeof(CommandDef));
  }
  return st;
}

} // namespace coredcc
