#include "coredcc/bind/manager.h"

#include "coredcc/bind/facades.h"
#include "coredcc/bind/foreign_command.h"
#include "coredcc/bind/graph_facades.h"
#include "coredcc/bind/layout_facades.h"
#include "coredcc/bind/library_facades.h"
#include "coredcc/bind/path_reader.h"
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
  mgr->add(Bind<CommandFlags>());
  mgr->add(Bind<CommandDef>());
  mgr->add(Bind<Command>());
  mgr->add(Bind<ForeignFactory>());
  mgr->add(Bind<NodeTypeDef>());
  mgr->add(Bind<NodeExecContext>());
  mgr->add(Bind<GraphKernel>());
  mgr->add(Bind<ScriptGraph>());
  mgr->add(Bind<GraphResult>());
  mgr->add(Bind<PathReader>());
  mgr->add(Bind<ScriptBytes>());
  mgr->add(Bind<SaveResult>());
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
  BIND_FUNCTION(mgr, "coredcc::makeFlags", script::makeFlags, MARGS("bits"));
  BIND_FUNCTION(mgr, "coredcc::makeNullPointer", script::makeNullPointer, MARGS());
  BIND_FUNCTION(mgr, "coredcc::makePointer", script::makePointer, MARGS("ctx", "text"));
  BIND_FUNCTION(mgr, "coredcc::readBool", script::readBool, MARGS("v"));
  BIND_FUNCTION(mgr, "coredcc::readInt", script::readInt, MARGS("v"));
  BIND_FUNCTION(mgr, "coredcc::readFloat", script::readFloat, MARGS("v"));
  BIND_FUNCTION(mgr, "coredcc::readString", script::readString, MARGS("v"));
  BIND_FUNCTION(mgr, "coredcc::readComponent", script::readComponent, MARGS("v", "i"));
  BIND_FUNCTION(mgr, "coredcc::readEnum", script::readEnum, MARGS("v"));
  BIND_FUNCTION(mgr, "coredcc::readFlags", script::readFlags, MARGS("v"));
  BIND_FUNCTION(mgr, "coredcc::pointerText", script::pointerText, MARGS("ctx", "v"));
  BIND_FUNCTION(mgr, "coredcc::valueToString", script::valueToString, MARGS("v"));

  BIND_FUNCTION(mgr, "coredcc::bagGet", script::bagGet, MARGS("bag", "name"));
  BIND_FUNCTION(mgr, "coredcc::bagSize", script::bagSize, MARGS("bag"));
  BIND_FUNCTION(mgr, "coredcc::bagName", script::bagName, MARGS("bag", "i"));
  BIND_FUNCTION(mgr, "coredcc::inputGet", script::inputGet, MARGS("ctx", "name"));
  BIND_FUNCTION(mgr, "coredcc::outputSet", script::outputSet, MARGS("ctx", "name", "value"));
  BIND_FUNCTION(mgr, "coredcc::outputGet", script::outputGet, MARGS("ctx", "name"));

  BIND_FUNCTION(mgr, "coredcc::resolvePath", script::resolvePath, MARGS("ctx", "path"));
  BIND_FUNCTION(mgr, "coredcc::pathMeta", script::pathMeta, MARGS("ctx", "path"));
  BIND_FUNCTION(mgr, "coredcc::valueKeys", script::valueKeys, MARGS("ctx", "path", "v"));
  BIND_FUNCTION(mgr, "coredcc::getValue", script::getValue, MARGS("ctx", "path"));
  BIND_FUNCTION(mgr, "coredcc::setValue", script::setValue, MARGS("ctx", "path", "value"));
  BIND_FUNCTION(mgr,
                "coredcc::setValueGesture",
                script::setValueGesture,
                MARGS("ctx", "path", "value", "gesture"));
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

void addLayoutFacades(litestl::binding::BindingManager *mgr)
{
  BIND_FUNCTION(mgr, "coredcc::layoutJson", script::layoutJson, MARGS("xml"));
  BIND_FUNCTION(mgr,
                "coredcc::validateLayoutXml",
                script::validateLayoutXml,
                MARGS("ctx", "xml"));
  BIND_FUNCTION(mgr,
                "coredcc::invokeAt",
                script::invokeAt,
                MARGS("ctx", "prefix", "invocation"));

  BIND_FUNCTION(mgr, "coredcc::newPathReader", script::newPathReader, MARGS());
  BIND_FUNCTION(mgr, "coredcc::readerAdd", script::readerAdd, MARGS("reader", "path"));
  BIND_FUNCTION(mgr, "coredcc::readerCount", script::readerCount, MARGS("reader"));
  BIND_FUNCTION(mgr, "coredcc::readerRead", script::readerRead, MARGS("ctx", "reader"));
  BIND_FUNCTION(mgr, "coredcc::readerValue", script::readerValue, MARGS("reader", "i"));
  BIND_FUNCTION(mgr, "coredcc::readerWatch", script::readerWatch, MARGS("ctx", "reader"));
  BIND_FUNCTION(mgr, "coredcc::readerUnwatch", script::readerUnwatch, MARGS("reader"));
  BIND_FUNCTION(mgr, "coredcc::readerTakeChanged", script::readerTakeChanged, MARGS("reader"));
  BIND_FUNCTION(mgr, "coredcc::flushChanges", script::flushChanges, MARGS("ctx"));
  BIND_FUNCTION(mgr, "coredcc::watchedCount", script::watchedCount, MARGS("ctx"));
}

void addLibraryFacades(litestl::binding::BindingManager *mgr)
{
  BIND_FUNCTION(mgr, "coredcc::bytesSize", script::bytesSize, MARGS("bytes"));
  BIND_FUNCTION(mgr, "coredcc::bytesRead", script::bytesRead, MARGS("bytes", "offset", "out"));
  BIND_FUNCTION(mgr, "coredcc::librarySave", script::librarySave, MARGS("ctx"));
  BIND_FUNCTION(mgr,
                "coredcc::libraryLoad",
                script::libraryLoad,
                MARGS("ctx", "bytes", "sourceName", "maxBytes", "maxBlocks"));
  BIND_FUNCTION(mgr,
                "coredcc::libraryAppend",
                script::libraryAppend,
                MARGS("ctx", "bytes", "sourceName", "maxBytes", "maxBlocks"));
  BIND_FUNCTION(mgr, "coredcc::libraryResolve", script::libraryResolve, MARGS("ctx"));
  BIND_FUNCTION(mgr, "coredcc::clearHistory", script::clearHistory, MARGS("ctx"));
}

void addGraphFacades(litestl::binding::BindingManager *mgr)
{
  BIND_FUNCTION(mgr, "coredcc::newNodeType", script::newNodeType, MARGS("id")).returnsOwned();
  BIND_FUNCTION(mgr, "coredcc::nodeTypeUiName", script::nodeTypeUiName, MARGS("def", "s"));
  BIND_FUNCTION(mgr,
                "coredcc::nodeTypeInput",
                script::nodeTypeInput,
                MARGS("def", "name", "defaultValue"));
  BIND_FUNCTION(mgr,
                "coredcc::nodeTypePayloadInput",
                script::nodeTypePayloadInput,
                MARGS("def", "name", "socketType"));
  BIND_FUNCTION(mgr,
                "coredcc::nodeTypeOutput",
                script::nodeTypeOutput,
                MARGS("def", "name", "socketType"));
  BIND_FUNCTION(mgr,
                "coredcc::nodeTypeParam",
                script::nodeTypeParam,
                MARGS("def", "name", "defaultValue"));
  BIND_FUNCTION(mgr,
                "coredcc::registerForeignNodeType",
                script::registerForeignNodeType,
                MARGS("ctx", "def", "kernel", "slots"));

  BIND_FUNCTION(mgr, "coredcc::nodeInput", script::nodeInput, MARGS("node", "name"));
  BIND_FUNCTION(mgr, "coredcc::nodeParam", script::nodeParam, MARGS("node", "name"));
  BIND_FUNCTION(mgr, "coredcc::nodeOutput", script::nodeOutput, MARGS("node", "name", "value"));
  BIND_FUNCTION(mgr, "coredcc::nodePass", script::nodePass, MARGS("node", "input", "output"));

  BIND_FUNCTION(mgr, "coredcc::graphCreate", script::graphCreate, MARGS("ctx", "name"));
  BIND_FUNCTION(mgr,
                "coredcc::graphAddNode",
                script::graphAddNode,
                MARGS("ctx", "graphPath", "type", "x", "y"));
  BIND_FUNCTION(mgr,
                "coredcc::graphRemoveNode",
                script::graphRemoveNode,
                MARGS("ctx", "graphPath", "node"));
  BIND_FUNCTION(mgr,
                "coredcc::graphLink",
                script::graphLink,
                MARGS("ctx", "graphPath", "from", "fromSocket", "to", "toSocket"));
  BIND_FUNCTION(mgr,
                "coredcc::graphUnlink",
                script::graphUnlink,
                MARGS("ctx", "graphPath", "link"));
  BIND_FUNCTION(mgr,
                "coredcc::graphAddInterface",
                script::graphAddInterface,
                MARGS("ctx", "graphPath", "output", "name", "type"));

  BIND_FUNCTION(mgr, "coredcc::graphInstance", script::graphInstance, MARGS("ctx", "graphPath"));
  BIND_FUNCTION(mgr,
                "coredcc::graphSetInput",
                script::graphSetInput,
                MARGS("graph", "name", "value"));
  BIND_FUNCTION(mgr, "coredcc::graphClearInput", script::graphClearInput, MARGS("graph", "name"));
  BIND_FUNCTION(mgr, "coredcc::graphEvaluate", script::graphEvaluate, MARGS("graph", "output"));
  BIND_FUNCTION(mgr, "coredcc::graphErrorText", script::graphErrorText, MARGS("graph", "output"));
  BIND_FUNCTION(mgr, "coredcc::graphClose", script::graphClose, MARGS("graph"));
  BIND_FUNCTION(mgr, "coredcc::graphInstanceCount", script::graphInstanceCount, MARGS("ctx"));
}

} // namespace

void addBindings(litestl::binding::BindingManager *mgr)
{
  litestl::alloc::PermanentGuard guard;
  addTypes(mgr);
  addFacades(mgr);
  addGraphFacades(mgr);
  addLayoutFacades(mgr);
  addLibraryFacades(mgr);
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
