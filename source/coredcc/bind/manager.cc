#include "coredcc/bind/manager.h"

#include "coredcc/bind/catalog.h"
#include "coredcc/bind/checked.h"
#include "coredcc/bind/facades.h"
#include "coredcc/bind/foreign_command.h"
#include "coredcc/bind/graph_facades.h"
#include "coredcc/bind/layout_facades.h"
#include "coredcc/bind/library_facades.h"
#include "coredcc/bind/path_reader.h"
#include "coredcc/bind/run_facades.h"
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
  mgr->add(Bind<ScriptTable>());
  mgr->add(Bind<TableResult>());
  mgr->add(Bind<ScriptGraph>());
  mgr->add(Bind<GraphResult>());
  mgr->add(Bind<PathReader>());
  mgr->add(Bind<RunInputs>());
  mgr->add(Bind<ToolChecks>());
  mgr->add(Bind<ScriptBytes>());
  mgr->add(Bind<SaveResult>());
}

/* Those that return a def they make check their id themselves. */
void addFacades(litestl::binding::BindingManager *mgr)
{
  BIND_FACADE(mgr, "coredcc::statusError", script::statusError, MARGS("code", "message"));
  BIND_FACADE(mgr, "coredcc::statusText", script::statusText, MARGS("s"));

  BIND_FACADE(mgr, "coredcc::makeBool", script::makeBool, MARGS("v"));
  BIND_FACADE(mgr, "coredcc::makeInt", script::makeInt, MARGS("v"));
  BIND_FACADE(mgr, "coredcc::makeFloat", script::makeFloat, MARGS("v"));
  BIND_FACADE(mgr, "coredcc::makeString", script::makeString, MARGS("v"));
  BIND_FACADE(mgr, "coredcc::makeVec2", script::makeVec2, MARGS("x", "y"));
  BIND_FACADE(mgr, "coredcc::makeVec3", script::makeVec3, MARGS("x", "y", "z"));
  BIND_FACADE(mgr, "coredcc::makeVec4", script::makeVec4, MARGS("x", "y", "z", "w"));
  BIND_FACADE(mgr, "coredcc::makeQuat", script::makeQuat, MARGS("x", "y", "z", "w"));
  BIND_FACADE(mgr, "coredcc::makeMat4", script::makeMat4, MARGS("a", "b", "c", "d"));
  BIND_FACADE(mgr, "coredcc::makeFlags", script::makeFlags, MARGS("bits"));
  BIND_FACADE(mgr, "coredcc::makeNullPointer", script::makeNullPointer, MARGS());
  BIND_FACADE(mgr, "coredcc::makePointer", script::makePointer, MARGS("ctx", "text"));
  BIND_FACADE(mgr, "coredcc::readBool", script::readBool, MARGS("v"));
  BIND_FACADE(mgr, "coredcc::readInt", script::readInt, MARGS("v"));
  BIND_FACADE(mgr, "coredcc::readFloat", script::readFloat, MARGS("v"));
  BIND_FACADE(mgr, "coredcc::readString", script::readString, MARGS("v"));
  BIND_FACADE(mgr, "coredcc::readComponent", script::readComponent, MARGS("v", "i"));
  BIND_FACADE(mgr, "coredcc::readEnum", script::readEnum, MARGS("v"));
  BIND_FACADE(mgr, "coredcc::readFlags", script::readFlags, MARGS("v"));
  BIND_FACADE(mgr, "coredcc::pointerText", script::pointerText, MARGS("ctx", "v"));
  BIND_FACADE(mgr, "coredcc::valueToString", script::valueToString, MARGS("v"));

  BIND_FACADE(mgr, "coredcc::bagGet", script::bagGet, MARGS("bag", "name"));
  BIND_FACADE(mgr, "coredcc::bagSize", script::bagSize, MARGS("bag"));
  BIND_FACADE(mgr, "coredcc::bagName", script::bagName, MARGS("bag", "i"));
  BIND_FACADE(mgr, "coredcc::inputGet", script::inputGet, MARGS("ctx", "name"));
  BIND_FACADE(mgr, "coredcc::outputSet", script::outputSet, MARGS("ctx", "name", "value"));
  BIND_FACADE(mgr, "coredcc::outputGet", script::outputGet, MARGS("ctx", "name"));

  BIND_FACADE(mgr, "coredcc::resolvePath", script::resolvePath, MARGS("ctx", "path"));
  BIND_FACADE(mgr, "coredcc::pathMeta", script::pathMeta, MARGS("ctx", "path"));
  BIND_FACADE(mgr, "coredcc::apiCatalog", script::apiCatalog, MARGS("ctx"));
  BIND_FACADE(mgr, "coredcc::valueKeys", script::valueKeys, MARGS("ctx", "path", "v"));
  BIND_FACADE(mgr, "coredcc::getValue", script::getValue, MARGS("ctx", "path"));
  BIND_FACADE(mgr, "coredcc::setValue", script::setValue, MARGS("ctx", "path", "value"));
  BIND_FACADE(mgr, "coredcc::beginGesture", script::beginGesture, MARGS("ctx"));
  BIND_FACADE(mgr, "coredcc::cancelGesture", script::cancelGesture, MARGS("ctx", "gesture"));
  BIND_FACADE(mgr,
              "coredcc::setValueGesture",
              script::setValueGesture,
              MARGS("ctx", "path", "value", "gesture"));
  BIND_FACADE(mgr, "coredcc::writeValue", script::writeValue, MARGS("ctx", "path", "value"));
  BIND_FACADE(mgr, "coredcc::invoke", script::invoke, MARGS("ctx", "invocation"));
  BIND_FACADE(mgr, "coredcc::undo", script::undo, MARGS("ctx"));
  BIND_FACADE(mgr, "coredcc::redo", script::redo, MARGS("ctx"));
  BIND_FACADE(mgr, "coredcc::canUndo", script::canUndo, MARGS("ctx"));
  BIND_FACADE(mgr, "coredcc::canRedo", script::canRedo, MARGS("ctx"));

  BIND_FUNCTION(mgr, "coredcc::newCommandDef", script::newCommandDef, MARGS("id"))
      .returnsOwned();
  BIND_FACADE(mgr, "coredcc::defUiName", script::defUiName, MARGS("def", "s"));
  BIND_FACADE(mgr, "coredcc::defDescription", script::defDescription, MARGS("def", "s"));
  BIND_FACADE(mgr, "coredcc::defIcon", script::defIcon, MARGS("def", "s"));
  BIND_FACADE(mgr, "coredcc::defFlags", script::defFlags, MARGS("def", "flags"));
  BIND_FACADE(mgr, "coredcc::defInput", script::defInput, MARGS("def", "name", "defaultValue"));
  BIND_FACADE(mgr, "coredcc::defOutput", script::defOutput, MARGS("def", "name", "defaultValue"));
  BIND_FACADE(mgr,
              "coredcc::registerForeignCommand",
              script::registerForeignCommand,
              MARGS("ctx", "def", "factory", "slots"));
}

void addLayoutFacades(litestl::binding::BindingManager *mgr)
{
  BIND_FACADE(mgr, "coredcc::layoutJson", script::layoutJson, MARGS("xml"));
  BIND_FACADE(mgr,
              "coredcc::validateLayoutXml",
              script::validateLayoutXml,
              MARGS("ctx", "xml"));
  BIND_FACADE(mgr, "coredcc::newRunInputs", script::newRunInputs, MARGS());
  BIND_FACADE(mgr,
              "coredcc::runInputsSet",
              script::runInputsSet,
              MARGS("inputs", "name", "value"));
  BIND_FACADE(mgr,
              "coredcc::runAt",
              script::runAt,
              MARGS("ctx", "prefix", "id", "inputs", "lastUsed"));
  BIND_FACADE(mgr,
              "coredcc::canRunAt",
              script::canRunAt,
              MARGS("ctx", "prefix", "id", "inputs", "lastUsed"));
  BIND_FACADE(mgr,
              "coredcc::canInvokeAt",
              script::canInvokeAt,
              MARGS("ctx", "prefix", "invocation"));
  BIND_FACADE(mgr, "coredcc::newToolChecks", script::newToolChecks, MARGS());
  BIND_FACADE(mgr,
              "coredcc::toolChecksAdd",
              script::toolChecksAdd,
              MARGS("tools", "prefix", "invocation"));
  BIND_FACADE(mgr, "coredcc::toolChecksRun", script::toolChecksRun, MARGS("ctx", "tools"));
  BIND_FACADE(mgr,
              "coredcc::beginInteractiveAt",
              script::beginInteractiveAt,
              MARGS("ctx", "prefix", "id", "inputs", "lastUsed"));
  BIND_FACADE(mgr, "coredcc::interactiveId", script::interactiveId, MARGS("ctx"));
  BIND_FACADE(mgr,
              "coredcc::writeInteractive",
              script::writeInteractive,
              MARGS("ctx", "path", "value"));
  BIND_FACADE(mgr, "coredcc::commitInteractive", script::commitInteractive, MARGS("ctx"));
  BIND_FACADE(mgr, "coredcc::cancelInteractive", script::cancelInteractive, MARGS("ctx"));
  BIND_FACADE(mgr, "coredcc::lastCommandId", script::lastCommandId, MARGS("ctx"));
  BIND_FACADE(mgr, "coredcc::lastCommandInputs", script::lastCommandInputs, MARGS("ctx"));
  BIND_FACADE(mgr, "coredcc::rerunLast", script::rerunLast, MARGS("ctx", "inputs"));
  BIND_FACADE(mgr,
              "coredcc::invokeAt",
              script::invokeAt,
              MARGS("ctx", "prefix", "invocation"));

  BIND_FACADE(mgr, "coredcc::newPathReader", script::newPathReader, MARGS());
  BIND_FACADE(mgr, "coredcc::readerAdd", script::readerAdd, MARGS("reader", "path"));
  BIND_FACADE(mgr, "coredcc::readerCount", script::readerCount, MARGS("reader"));
  BIND_FACADE(mgr, "coredcc::readerRead", script::readerRead, MARGS("ctx", "reader"));
  BIND_FACADE(mgr, "coredcc::readerValue", script::readerValue, MARGS("reader", "i"));
  BIND_FACADE(mgr, "coredcc::readerWatch", script::readerWatch, MARGS("ctx", "reader"));
  BIND_FACADE(mgr, "coredcc::readerUnwatch", script::readerUnwatch, MARGS("reader"));
  BIND_FACADE(mgr, "coredcc::readerTakeChanged", script::readerTakeChanged, MARGS("reader"));
  BIND_FACADE(mgr,
              "coredcc::readerTakeChangedMeta",
              script::readerTakeChangedMeta,
              MARGS("reader"));
  BIND_FACADE(mgr, "coredcc::flushChanges", script::flushChanges, MARGS("ctx"));
  BIND_FACADE(mgr, "coredcc::watchedCount", script::watchedCount, MARGS("ctx"));
}

void addLibraryFacades(litestl::binding::BindingManager *mgr)
{
  BIND_FACADE(mgr, "coredcc::bytesSize", script::bytesSize, MARGS("bytes"));
  BIND_FACADE(mgr, "coredcc::bytesRead", script::bytesRead, MARGS("bytes", "offset", "out"));
  BIND_FACADE(mgr, "coredcc::librarySave", script::librarySave, MARGS("ctx"));
  BIND_FACADE(mgr,
              "coredcc::libraryLoad",
              script::libraryLoad,
              MARGS("ctx", "bytes", "sourceName", "maxBytes", "maxBlocks"));
  BIND_FACADE(mgr,
              "coredcc::libraryAppend",
              script::libraryAppend,
              MARGS("ctx", "bytes", "sourceName", "maxBytes", "maxBlocks"));
  BIND_FACADE(mgr, "coredcc::libraryResolve", script::libraryResolve, MARGS("ctx"));
  BIND_FACADE(mgr, "coredcc::clearHistory", script::clearHistory, MARGS("ctx"));
}

void addGraphFacades(litestl::binding::BindingManager *mgr)
{
  BIND_FUNCTION(mgr, "coredcc::newNodeType", script::newNodeType, MARGS("id")).returnsOwned();
  BIND_FACADE(mgr, "coredcc::nodeTypeUiName", script::nodeTypeUiName, MARGS("def", "s"));
  BIND_FACADE(mgr,
              "coredcc::nodeTypeInput",
              script::nodeTypeInput,
              MARGS("def", "name", "defaultValue"));
  BIND_FACADE(mgr,
              "coredcc::nodeTypePayloadInput",
              script::nodeTypePayloadInput,
              MARGS("def", "name", "socketType"));
  BIND_FACADE(mgr,
              "coredcc::nodeTypeOutput",
              script::nodeTypeOutput,
              MARGS("def", "name", "socketType"));
  BIND_FACADE(mgr,
              "coredcc::nodeTypeParam",
              script::nodeTypeParam,
              MARGS("def", "name", "defaultValue"));
  BIND_FACADE(mgr,
              "coredcc::registerForeignNodeType",
              script::registerForeignNodeType,
              MARGS("ctx", "def", "kernel", "slots"));
  BIND_FACADE(mgr,
              "coredcc::replaceForeignNodeType",
              script::replaceForeignNodeType,
              MARGS("ctx", "def", "kernel", "slots"));
  BIND_FACADE(mgr,
              "coredcc::nodeTypeSocketName",
              script::nodeTypeSocketName,
              MARGS("def", "output", "name", "uiName"));
  BIND_FACADE(mgr,
              "coredcc::nodeTypeRange",
              script::nodeTypeRange,
              MARGS("def", "name", "min", "max"));
  BIND_FACADE(mgr,
              "coredcc::nodeTypeItemParam",
              script::nodeTypeItemParam,
              MARGS("def", "name", "flags", "defaultValue"));
  BIND_FACADE(mgr,
              "coredcc::nodeTypeItem",
              script::nodeTypeItem,
              MARGS("def", "param", "key", "value", "uiName"));
  BIND_FACADE(mgr,
              "coredcc::nodeTypePointerParam",
              script::nodeTypePointerParam,
              MARGS("def", "name", "target", "weak"));
  BIND_FACADE(mgr,
              "coredcc::nodeTypeOutputAdapter",
              script::nodeTypeOutputAdapter,
              MARGS("def"));

  BIND_FACADE(mgr, "coredcc::nodeInput", script::nodeInput, MARGS("node", "name"));
  BIND_FACADE(mgr, "coredcc::nodeParam", script::nodeParam, MARGS("node", "name"));
  BIND_FACADE(mgr, "coredcc::nodeOutput", script::nodeOutput, MARGS("node", "name", "value"));
  BIND_FACADE(mgr, "coredcc::nodePass", script::nodePass, MARGS("node", "input", "output"));

  BIND_FACADE(mgr, "coredcc::nodeIsLinked", script::nodeIsLinked, MARGS("node", "name"));
  BIND_FACADE(mgr,
              "coredcc::nodeUseBlock",
              script::nodeUseBlock,
              MARGS("node", "param", "aspects"));
  BIND_FACADE(mgr, "coredcc::nodeCancelled", script::nodeCancelled, MARGS("node"));
  BIND_FACADE(mgr,
              "coredcc::nodeProgress",
              script::nodeProgress,
              MARGS("node", "fraction", "text"));
  BIND_FACADE(mgr, "coredcc::nodeInputTable", script::nodeInputTable, MARGS("node", "name"));
  BIND_FACADE(mgr,
              "coredcc::nodeOutputTable",
              script::nodeOutputTable,
              MARGS("node", "name", "table"));

  BIND_FACADE(mgr, "coredcc::newTable", script::newTable, MARGS());
  BIND_FACADE(mgr, "coredcc::tableRows", script::tableRows, MARGS("table"));
  BIND_FACADE(mgr, "coredcc::tableColumns", script::tableColumns, MARGS("table"));
  BIND_FACADE(mgr, "coredcc::tableColumnName", script::tableColumnName, MARGS("table", "column"));
  BIND_FACADE(mgr, "coredcc::tableKey", script::tableKey, MARGS("table", "row"));
  BIND_FACADE(mgr, "coredcc::tableAddColumn", script::tableAddColumn, MARGS("table", "name"));
  BIND_FACADE(mgr, "coredcc::tableAddRow", script::tableAddRow, MARGS("table", "key"));
  BIND_FACADE(mgr,
              "coredcc::tableSet",
              script::tableSet,
              MARGS("table", "column", "row", "value"));
  BIND_FACADE(mgr, "coredcc::tableGet", script::tableGet, MARGS("table", "column", "row"));

  BIND_FACADE(mgr, "coredcc::graphCreate", script::graphCreate, MARGS("ctx", "name"));
  BIND_FACADE(mgr,
              "coredcc::graphAddNode",
              script::graphAddNode,
              MARGS("ctx", "graphPath", "type", "x", "y"));
  BIND_FACADE(mgr,
              "coredcc::graphRemoveNode",
              script::graphRemoveNode,
              MARGS("ctx", "graphPath", "node"));
  BIND_FACADE(mgr,
              "coredcc::graphLink",
              script::graphLink,
              MARGS("ctx", "graphPath", "from", "fromSocket", "to", "toSocket"));
  BIND_FACADE(mgr,
              "coredcc::graphUnlink",
              script::graphUnlink,
              MARGS("ctx", "graphPath", "link"));
  BIND_FACADE(mgr,
              "coredcc::graphAddInterface",
              script::graphAddInterface,
              MARGS("ctx", "graphPath", "output", "name", "type"));

  BIND_FACADE(mgr, "coredcc::graphNewId", script::graphNewId, MARGS("ctx", "graphPath"));
  BIND_FACADE(mgr,
              "coredcc::graphDiagnostics",
              script::graphDiagnostics,
              MARGS("ctx", "graphPath"));

  BIND_FACADE(mgr, "coredcc::graphInstance", script::graphInstance, MARGS("ctx", "graphPath"));
  BIND_FACADE(mgr,
              "coredcc::graphSetInput",
              script::graphSetInput,
              MARGS("graph", "name", "value"));
  BIND_FACADE(mgr, "coredcc::graphClearInput", script::graphClearInput, MARGS("graph", "name"));
  BIND_FACADE(mgr, "coredcc::graphEvaluate", script::graphEvaluate, MARGS("graph", "output"));
  BIND_FACADE(mgr, "coredcc::graphErrorText", script::graphErrorText, MARGS("graph", "output"));
  BIND_FACADE(mgr,
              "coredcc::graphSetInputTable",
              script::graphSetInputTable,
              MARGS("graph", "name", "table"));
  BIND_FACADE(mgr,
              "coredcc::graphEvaluateTable",
              script::graphEvaluateTable,
              MARGS("graph", "output"));
  BIND_FACADE(mgr, "coredcc::graphWatch", script::graphWatch, MARGS("graph", "node", "socket"));
  BIND_FACADE(mgr, "coredcc::graphUnwatch", script::graphUnwatch, MARGS("graph", "watch"));
  BIND_FACADE(mgr, "coredcc::graphUpdate", script::graphUpdate, MARGS("graph"));
  BIND_FACADE(mgr, "coredcc::graphWatchStatus", script::graphWatchStatus, MARGS("graph", "watch"));
  BIND_FACADE(mgr, "coredcc::graphWatchValue", script::graphWatchValue, MARGS("graph", "watch"));
  BIND_FACADE(mgr, "coredcc::graphWatchTable", script::graphWatchTable, MARGS("graph", "watch"));
  BIND_FACADE(mgr,
              "coredcc::graphWatchLastGood",
              script::graphWatchLastGood,
              MARGS("graph", "watch"));
  BIND_FACADE(mgr,
              "coredcc::graphWatchChangedAt",
              script::graphWatchChangedAt,
              MARGS("graph", "watch"));
  BIND_FACADE(mgr,
              "coredcc::graphWatchIsCurrent",
              script::graphWatchIsCurrent,
              MARGS("graph", "watch"));
  BIND_FACADE(mgr,
              "coredcc::graphWatchErrorText",
              script::graphWatchErrorText,
              MARGS("graph", "watch"));
  BIND_FACADE(mgr,
              "coredcc::graphPeek",
              script::graphPeek,
              MARGS("graph", "node", "socket", "input"));
  BIND_FACADE(mgr,
              "coredcc::graphPeekTable",
              script::graphPeekTable,
              MARGS("graph", "node", "socket", "input"));
  BIND_FACADE(mgr, "coredcc::graphReport", script::graphReport, MARGS("graph", "node"));
  BIND_FACADE(mgr, "coredcc::graphRetry", script::graphRetry, MARGS("graph"));
  BIND_FACADE(mgr, "coredcc::graphClear", script::graphClear, MARGS("graph"));
  BIND_FACADE(mgr, "coredcc::graphDrop", script::graphDrop, MARGS("graph", "node"));
  BIND_FACADE(mgr,
              "coredcc::graphSetTimeLimit",
              script::graphSetTimeLimit,
              MARGS("graph", "milliseconds"));
  BIND_FACADE(mgr, "coredcc::graphClose", script::graphClose, MARGS("graph"));
  BIND_FACADE(mgr, "coredcc::graphInstanceCount", script::graphInstanceCount, MARGS("ctx"));
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
