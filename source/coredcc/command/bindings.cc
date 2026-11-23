/* Binding descriptors for command types. Command itself is bound by CDCC_INTERFACE. */
#include "coredcc/command/command.h"

#include "litestl/binding/binding.h"
#include "litestl/util/alloc.h"

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
