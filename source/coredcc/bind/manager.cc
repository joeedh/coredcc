#include "coredcc/bind/manager.h"

#include "coredcc/bind/facades.h"
#include "coredcc/bind/foreign_command.h"
#include "coredcc/command/command.h"

#include "litestl/binding/binding.h"
#include "litestl/binding/manager.h"
#include "litestl/util/alloc.h"

namespace coredcc::script {

namespace {

void addTypes(litestl::binding::BindingManager *mgr)
{
  using litestl::binding::Bind;
  mgr->add(Bind<StatusCode>());
  mgr->add(Bind<Status>());
  mgr->add(Bind<PropValue::Type>());
  mgr->add(Bind<PropValue>());
  mgr->add(Bind<PropertyBag>());
  mgr->add(Bind<BoolResult>());
  mgr->add(Bind<IntResult>());
  mgr->add(Bind<FloatResult>());
  mgr->add(Bind<StringResult>());
  mgr->add(Bind<ValueResult>());
  mgr->add(Bind<BagResult>());
  mgr->add(Bind<Context>());
  mgr->add(Bind<CommandDef>());
  mgr->add(Bind<Command>());
  mgr->add(Bind<ForeignFactory>());
}

void addFacades(litestl::binding::BindingManager *mgr)
{
  BIND_FUNCTION(mgr, "coredcc::statusError", script::statusError, MARGS("code", "message"));
  BIND_FUNCTION(mgr, "coredcc::statusText", script::statusText, MARGS("s"));

  BIND_FUNCTION(mgr, "coredcc::makeBool", script::makeBool, MARGS("v"));
  BIND_FUNCTION(mgr, "coredcc::makeInt", script::makeInt, MARGS("v"));
  BIND_FUNCTION(mgr, "coredcc::makeFloat", script::makeFloat, MARGS("v"));
  BIND_FUNCTION(mgr, "coredcc::makeString", script::makeString, MARGS("v"));
  BIND_FUNCTION(mgr, "coredcc::makeVec2", script::makeVec2, MARGS("x", "y"));
  BIND_FUNCTION(mgr, "coredcc::makeVec3", script::makeVec3, MARGS("x", "y", "z"));
  BIND_FUNCTION(mgr, "coredcc::makeVec4", script::makeVec4, MARGS("x", "y", "z", "w"));
  BIND_FUNCTION(mgr, "coredcc::readBool", script::readBool, MARGS("v"));
  BIND_FUNCTION(mgr, "coredcc::readInt", script::readInt, MARGS("v"));
  BIND_FUNCTION(mgr, "coredcc::readFloat", script::readFloat, MARGS("v"));
  BIND_FUNCTION(mgr, "coredcc::readString", script::readString, MARGS("v"));
  BIND_FUNCTION(mgr, "coredcc::readComponent", script::readComponent, MARGS("v", "i"));
  BIND_FUNCTION(mgr, "coredcc::readEnum", script::readEnum, MARGS("v"));
  BIND_FUNCTION(mgr, "coredcc::readFlags", script::readFlags, MARGS("v"));
  BIND_FUNCTION(mgr, "coredcc::valueToString", script::valueToString, MARGS("v"));

  BIND_FUNCTION(mgr, "coredcc::bagGet", script::bagGet, MARGS("bag", "name"));
  BIND_FUNCTION(mgr, "coredcc::bagSize", script::bagSize, MARGS("bag"));
  BIND_FUNCTION(mgr, "coredcc::bagName", script::bagName, MARGS("bag", "i"));
  BIND_FUNCTION(mgr, "coredcc::inputGet", script::inputGet, MARGS("ctx", "name"));
  BIND_FUNCTION(mgr, "coredcc::outputSet", script::outputSet, MARGS("ctx", "name", "value"));
  BIND_FUNCTION(mgr, "coredcc::outputGet", script::outputGet, MARGS("ctx", "name"));

  BIND_FUNCTION(mgr, "coredcc::resolvePath", script::resolvePath, MARGS("ctx", "path"));
  BIND_FUNCTION(mgr, "coredcc::getValue", script::getValue, MARGS("ctx", "path"));
  BIND_FUNCTION(mgr, "coredcc::setValue", script::setValue, MARGS("ctx", "path", "value"));
  BIND_FUNCTION(mgr, "coredcc::writeValue", script::writeValue, MARGS("ctx", "path", "value"));
  BIND_FUNCTION(mgr, "coredcc::invoke", script::invoke, MARGS("ctx", "invocation"));
  BIND_FUNCTION(mgr, "coredcc::undo", script::undo, MARGS("ctx"));
  BIND_FUNCTION(mgr, "coredcc::redo", script::redo, MARGS("ctx"));
  BIND_FUNCTION(mgr, "coredcc::canUndo", script::canUndo, MARGS("ctx"));
  BIND_FUNCTION(mgr, "coredcc::canRedo", script::canRedo, MARGS("ctx"));

  BIND_FUNCTION(mgr, "coredcc::newCommandDef", script::newCommandDef, MARGS("id"))
      .returnsOwned();
  BIND_FUNCTION(mgr, "coredcc::defUiName", script::defUiName, MARGS("def", "s"));
  BIND_FUNCTION(mgr, "coredcc::defDescription", script::defDescription, MARGS("def", "s"));
  BIND_FUNCTION(mgr, "coredcc::defIcon", script::defIcon, MARGS("def", "s"));
  BIND_FUNCTION(mgr, "coredcc::defFlags", script::defFlags, MARGS("def", "flags"));
  BIND_FUNCTION(mgr, "coredcc::defInput", script::defInput, MARGS("def", "name", "defaultValue"));
  BIND_FUNCTION(mgr, "coredcc::defOutput", script::defOutput, MARGS("def", "name", "defaultValue"));
  BIND_FUNCTION(mgr,
                "coredcc::registerForeignCommand",
                script::registerForeignCommand,
                MARGS("ctx", "def", "factory", "slots"));
}

} // namespace

void addBindings(litestl::binding::BindingManager *mgr)
{
  litestl::alloc::PermanentGuard guard;
  addTypes(mgr);
  addFacades(mgr);
}

litestl::binding::BindingManager *bindings()
{
  static litestl::binding::BindingManager *mgr = nullptr;
  if (!mgr) {
    litestl::alloc::PermanentGuard guard;
    mgr = litestl::alloc::New<litestl::binding::BindingManager>("coredcc BindingManager");
    addBindings(mgr);
    mgr->validate();
  }
  return mgr;
}

} // namespace coredcc::script
