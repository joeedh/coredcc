#include "coredcc/bind/manager.h"

#include "coredcc/bind/catalog.h"
#include "coredcc/bind/checked.h"
#include "coredcc/bind/drive_facades.h"
#include "coredcc/bind/facades.h"
#include "coredcc/bind/foreign_command.h"
#include "coredcc/bind/foreign_transform.h"
#include "coredcc/bind/graph_facades.h"
#include "coredcc/bind/layout_facades.h"
#include "coredcc/bind/library_facades.h"
#include "coredcc/bind/path_reader.h"
#include "coredcc/bind/run_facades.h"
#include "coredcc/bind/scene_facades.h"
#include "coredcc/bind/tool_facades.h"
#include "coredcc/bvh/any_source.h"
#include "coredcc/command/command.h"
#include "coredcc/scene/evaluated.h"

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
  mgr->add(Bind<InvocationChecks>());
  mgr->add(Bind<ScriptBytes>());
  mgr->add(Bind<SaveResult>());
  mgr->add(Bind<mat4d>());
  mgr->add(Bind<quatd>());
  mgr->add(Bind<double3>());
  mgr->add(Bind<Box3d>());
  mgr->add(Bind<RayHit>());
  mgr->add(Bind<NearestHit>());
  mgr->add(Bind<AnyBvh>());
  mgr->add(Bind<ChannelBlock>());
  mgr->add(Bind<EvalContext>());
  mgr->add(Bind<TransformState>());
  mgr->add(Bind<DeltaKind>());
  mgr->add(Bind<TransformDelta>());
  mgr->add(Bind<SetWorldMode>());
  mgr->add(Bind<TransformModel>());
  mgr->add(Bind<TransformTypeInfo>());
  mgr->add(Bind<EvalStatus>());
  mgr->add(Bind<ObjectEval>());
  mgr->add(Bind<Handedness>());
  mgr->add(Bind<UpAxis>());
  mgr->add(Bind<View>());
  mgr->add(Bind<ViewPoint>());
  mgr->add(Bind<ViewRegion>());
  mgr->add(Bind<ViewMath>());
  mgr->add(Bind<RegionQuery>());
  mgr->add(Bind<PickOptions>());
  mgr->add(Bind<PickQuery>());
  mgr->add(Bind<PickSink>());
  mgr->add(Bind<NearQuery>());
  mgr->add(Bind<NearSink>());
  mgr->add(Bind<MatrixResult>());
  mgr->add(Bind<PickedObject>());
  mgr->add(Bind<PickList>());
  mgr->add(Bind<PickResult>());
}

/* Those that return a def they make check their id themselves. */
void addFacades(litestl::binding::BindingManager *mgr)
{
  BIND_FACADE(
      mgr, "coredcc::newErrorStatus", script::newErrorStatus, MARGS("code", "message"));
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
  BIND_FACADE(mgr,
              "coredcc::formatUnit",
              script::formatUnit,
              MARGS("value", "family", "unit", "precision"));
  BIND_FACADE(
      mgr, "coredcc::parseUnit", script::parseUnit, MARGS("text", "family", "unit"));
  BIND_FACADE(mgr, "coredcc::readFloat", script::readFloat, MARGS("v"));
  BIND_FACADE(mgr, "coredcc::readString", script::readString, MARGS("v"));
  BIND_FACADE(mgr, "coredcc::readComponent", script::readComponent, MARGS("v", "i"));
  BIND_FACADE(mgr, "coredcc::readEnum", script::readEnum, MARGS("v"));
  BIND_FACADE(mgr, "coredcc::readFlags", script::readFlags, MARGS("v"));
  BIND_FACADE(mgr, "coredcc::pointerText", script::pointerText, MARGS("ctx", "v"));
  BIND_FACADE(mgr, "coredcc::valueText", script::valueText, MARGS("v"));

  BIND_FACADE(mgr, "coredcc::bagGet", script::bagGet, MARGS("bag", "name"));
  BIND_FACADE(mgr, "coredcc::bagSize", script::bagSize, MARGS("bag"));
  BIND_FACADE(mgr, "coredcc::bagName", script::bagName, MARGS("bag", "i"));
  BIND_FACADE(mgr, "coredcc::commandInput", script::commandInput, MARGS("ctx", "name"));
  BIND_FACADE(mgr,
              "coredcc::setCommandOutput",
              script::setCommandOutput,
              MARGS("ctx", "name", "value"));
  BIND_FACADE(mgr, "coredcc::commandOutput", script::commandOutput, MARGS("ctx", "name"));

  BIND_FACADE(mgr, "coredcc::resolvePath", script::resolvePath, MARGS("ctx", "path"));
  BIND_FACADE(mgr, "coredcc::pathMeta", script::pathMeta, MARGS("ctx", "path"));
  BIND_FACADE(mgr, "coredcc::apiCatalog", script::apiCatalog, MARGS("ctx"));
  BIND_FACADE(mgr, "coredcc::valueKeys", script::valueKeys, MARGS("ctx", "path", "v"));
  BIND_FACADE(mgr, "coredcc::getValue", script::getValue, MARGS("ctx", "path"));
  BIND_FACADE(mgr, "coredcc::formatValue", script::formatValue, MARGS("ctx", "path"));
  BIND_FACADE(
      mgr, "coredcc::parseValue", script::parseValue, MARGS("ctx", "path", "text"));
  BIND_FACADE(mgr, "coredcc::setValue", script::setValue, MARGS("ctx", "path", "value"));
  BIND_FACADE(mgr, "coredcc::beginGesture", script::beginGesture, MARGS("ctx"));
  BIND_FACADE(
      mgr, "coredcc::cancelGesture", script::cancelGesture, MARGS("ctx", "gesture"));
  BIND_FACADE(mgr,
              "coredcc::setValueGesture",
              script::setValueGesture,
              MARGS("ctx", "path", "value", "gesture"));
  BIND_FACADE(
      mgr, "coredcc::writeValue", script::writeValue, MARGS("ctx", "path", "value"));
  BIND_FACADE(mgr, "coredcc::invoke", script::invoke, MARGS("ctx", "invocation"));
  BIND_FACADE(mgr, "coredcc::undo", script::undo, MARGS("ctx"));
  BIND_FACADE(mgr, "coredcc::redo", script::redo, MARGS("ctx"));
  BIND_FACADE(mgr, "coredcc::canUndo", script::canUndo, MARGS("ctx"));
  BIND_FACADE(mgr, "coredcc::canRedo", script::canRedo, MARGS("ctx"));

  BIND_FUNCTION(mgr, "coredcc::newCommandDef", script::newCommandDef, MARGS("id"))
      .returnsOwned();
  BIND_FACADE(
      mgr, "coredcc::commandDefUiName", script::commandDefUiName, MARGS("def", "s"));
  BIND_FACADE(mgr,
              "coredcc::commandDefDescription",
              script::commandDefDescription,
              MARGS("def", "s"));
  BIND_FACADE(mgr, "coredcc::commandDefIcon", script::commandDefIcon, MARGS("def", "s"));
  BIND_FACADE(
      mgr, "coredcc::commandDefFlags", script::commandDefFlags, MARGS("def", "flags"));
  BIND_FACADE(mgr,
              "coredcc::commandDefInput",
              script::commandDefInput,
              MARGS("def", "name", "defaultValue"));
  BIND_FACADE(mgr,
              "coredcc::commandDefOutput",
              script::commandDefOutput,
              MARGS("def", "name", "defaultValue"));
  BIND_OWNING_FACADE(mgr,
                     "coredcc::registerForeignCommand",
                     script::registerForeignCommand,
                     3,
                     MARGS("ctx", "def", "factory", "slots"));
}

void addLayoutFacades(litestl::binding::BindingManager *mgr)
{
  BIND_FACADE(mgr, "coredcc::parseLayout", script::parseLayout, MARGS("xml"));
  BIND_FACADE(
      mgr, "coredcc::validateLayout", script::validateLayout, MARGS("ctx", "xml"));
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
  BIND_FACADE(mgr, "coredcc::newInvocationChecks", script::newInvocationChecks, MARGS());
  BIND_FACADE(mgr,
              "coredcc::invocationChecksAdd",
              script::invocationChecksAdd,
              MARGS("checks", "prefix", "invocation"));
  BIND_FACADE(mgr,
              "coredcc::invocationChecksRun",
              script::invocationChecksRun,
              MARGS("ctx", "checks"));
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
  BIND_FACADE(
      mgr, "coredcc::invokeAt", script::invokeAt, MARGS("ctx", "prefix", "invocation"));

  BIND_FACADE(mgr, "coredcc::newPathReader", script::newPathReader, MARGS());
  BIND_FACADE(mgr, "coredcc::readerAdd", script::readerAdd, MARGS("reader", "path"));
  BIND_FACADE(mgr, "coredcc::readerCount", script::readerCount, MARGS("reader"));
  BIND_FACADE(mgr, "coredcc::readerRead", script::readerRead, MARGS("ctx", "reader"));
  BIND_FACADE(mgr, "coredcc::readerValue", script::readerValue, MARGS("reader", "i"));
  BIND_FACADE(mgr, "coredcc::readerWatch", script::readerWatch, MARGS("ctx", "reader"));
  BIND_FACADE(mgr, "coredcc::readerUnwatch", script::readerUnwatch, MARGS("reader"));
  BIND_FACADE(
      mgr, "coredcc::readerTakeChanged", script::readerTakeChanged, MARGS("reader"));
  BIND_FACADE(mgr,
              "coredcc::readerTakeChangedMeta",
              script::readerTakeChangedMeta,
              MARGS("reader"));
  BIND_FACADE(
      mgr, "coredcc::readerTake", script::readerTake, MARGS("ctx", "reader", "all"));
  BIND_FACADE(mgr, "coredcc::flushChanges", script::flushChanges, MARGS("ctx"));
  BIND_FACADE(mgr, "coredcc::watchedCount", script::watchedCount, MARGS("ctx"));
}

void addLibraryFacades(litestl::binding::BindingManager *mgr)
{
  BIND_FACADE(mgr, "coredcc::bytesByteSize", script::bytesByteSize, MARGS("bytes"));
  BIND_FACADE(
      mgr, "coredcc::bytesRead", script::bytesRead, MARGS("bytes", "offset", "out"));
  BIND_FACADE(mgr, "coredcc::saveLibrary", script::saveLibrary, MARGS("ctx"));
  BIND_FACADE(mgr,
              "coredcc::loadLibrary",
              script::loadLibrary,
              MARGS("ctx", "bytes", "sourceName", "maxBytes", "maxBlocks"));
  BIND_FACADE(mgr,
              "coredcc::appendLibrary",
              script::appendLibrary,
              MARGS("ctx", "bytes", "sourceName", "maxBytes", "maxBlocks"));
  BIND_FACADE(
      mgr, "coredcc::resolvePlaceholders", script::resolvePlaceholders, MARGS("ctx"));
  BIND_FACADE(mgr, "coredcc::clearHistory", script::clearHistory, MARGS("ctx"));
}

void addGraphFacades(litestl::binding::BindingManager *mgr)
{
  BIND_FUNCTION(mgr, "coredcc::newNodeTypeDef", script::newNodeTypeDef, MARGS("id"))
      .returnsOwned();
  BIND_FACADE(
      mgr, "coredcc::nodeTypeDefUiName", script::nodeTypeDefUiName, MARGS("def", "s"));
  BIND_FACADE(mgr,
              "coredcc::nodeTypeDefInput",
              script::nodeTypeDefInput,
              MARGS("def", "name", "defaultValue"));
  BIND_FACADE(mgr,
              "coredcc::nodeTypeDefPayloadInput",
              script::nodeTypeDefPayloadInput,
              MARGS("def", "name", "socketType"));
  BIND_FACADE(mgr,
              "coredcc::nodeTypeDefOutput",
              script::nodeTypeDefOutput,
              MARGS("def", "name", "socketType"));
  BIND_FACADE(mgr,
              "coredcc::nodeTypeDefParam",
              script::nodeTypeDefParam,
              MARGS("def", "name", "defaultValue"));
  BIND_OWNING_FACADE(mgr,
                     "coredcc::registerForeignNodeType",
                     script::registerForeignNodeType,
                     3,
                     MARGS("ctx", "def", "kernel", "slots"));
  BIND_OWNING_FACADE(mgr,
                     "coredcc::replaceForeignNodeType",
                     script::replaceForeignNodeType,
                     3,
                     MARGS("ctx", "def", "kernel", "slots"));
  BIND_FACADE(mgr,
              "coredcc::nodeTypeDefSocketName",
              script::nodeTypeDefSocketName,
              MARGS("def", "output", "name", "uiName"));
  BIND_FACADE(mgr,
              "coredcc::nodeTypeDefRange",
              script::nodeTypeDefRange,
              MARGS("def", "name", "min", "max"));
  BIND_FACADE(mgr,
              "coredcc::nodeTypeDefItemParam",
              script::nodeTypeDefItemParam,
              MARGS("def", "name", "flags", "defaultValue"));
  BIND_FACADE(mgr,
              "coredcc::nodeTypeDefItem",
              script::nodeTypeDefItem,
              MARGS("def", "param", "key", "value", "uiName"));
  BIND_FACADE(mgr,
              "coredcc::nodeTypeDefPointerParam",
              script::nodeTypeDefPointerParam,
              MARGS("def", "name", "target", "weak"));
  BIND_FACADE(mgr,
              "coredcc::nodeTypeDefOutputAdapter",
              script::nodeTypeDefOutputAdapter,
              MARGS("def"));

  BIND_FACADE(mgr, "coredcc::nodeInput", script::nodeInput, MARGS("node", "name"));
  BIND_FACADE(mgr, "coredcc::nodeParam", script::nodeParam, MARGS("node", "name"));
  BIND_FACADE(mgr,
              "coredcc::nodeSetOutput",
              script::nodeSetOutput,
              MARGS("node", "name", "value"));
  BIND_FACADE(
      mgr, "coredcc::nodePass", script::nodePass, MARGS("node", "input", "output"));

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
  BIND_FACADE(
      mgr, "coredcc::nodeInputTable", script::nodeInputTable, MARGS("node", "name"));
  BIND_FACADE(mgr,
              "coredcc::nodeSetOutputTable",
              script::nodeSetOutputTable,
              MARGS("node", "name", "table"));

  BIND_FACADE(mgr, "coredcc::newTable", script::newTable, MARGS());
  BIND_FACADE(mgr, "coredcc::tableRowCount", script::tableRowCount, MARGS("table"));
  BIND_FACADE(mgr, "coredcc::tableColumnCount", script::tableColumnCount, MARGS("table"));
  BIND_FACADE(
      mgr, "coredcc::tableColumnName", script::tableColumnName, MARGS("table", "column"));
  BIND_FACADE(mgr, "coredcc::tableKey", script::tableKey, MARGS("table", "row"));
  BIND_FACADE(
      mgr, "coredcc::tableAddColumn", script::tableAddColumn, MARGS("table", "name"));
  BIND_FACADE(mgr, "coredcc::tableAddRow", script::tableAddRow, MARGS("table", "key"));
  BIND_FACADE(mgr,
              "coredcc::tableSet",
              script::tableSet,
              MARGS("table", "column", "row", "value"));
  BIND_FACADE(
      mgr, "coredcc::tableGet", script::tableGet, MARGS("table", "column", "row"));

  BIND_FACADE(mgr, "coredcc::newGraph", script::newGraph, MARGS("ctx", "name"));
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

  BIND_FACADE(mgr,
              "coredcc::newGraphInstance",
              script::newGraphInstance,
              MARGS("ctx", "graphPath"));
  BIND_FACADE(mgr,
              "coredcc::graphSetInput",
              script::graphSetInput,
              MARGS("graph", "name", "value"));
  BIND_FACADE(
      mgr, "coredcc::graphClearInput", script::graphClearInput, MARGS("graph", "name"));
  BIND_FACADE(
      mgr, "coredcc::graphEvaluate", script::graphEvaluate, MARGS("graph", "output"));
  BIND_FACADE(
      mgr, "coredcc::graphErrorText", script::graphErrorText, MARGS("graph", "output"));
  BIND_FACADE(mgr,
              "coredcc::graphSetInputTable",
              script::graphSetInputTable,
              MARGS("graph", "name", "table"));
  BIND_FACADE(mgr,
              "coredcc::graphEvaluateTable",
              script::graphEvaluateTable,
              MARGS("graph", "output"));
  BIND_FACADE(
      mgr, "coredcc::graphWatch", script::graphWatch, MARGS("graph", "node", "socket"));
  BIND_FACADE(
      mgr, "coredcc::graphUnwatch", script::graphUnwatch, MARGS("graph", "watch"));
  BIND_FACADE(mgr, "coredcc::graphUpdate", script::graphUpdate, MARGS("graph"));
  BIND_FACADE(mgr,
              "coredcc::graphWatchStatus",
              script::graphWatchStatus,
              MARGS("graph", "watch"));
  BIND_FACADE(
      mgr, "coredcc::graphWatchValue", script::graphWatchValue, MARGS("graph", "watch"));
  BIND_FACADE(
      mgr, "coredcc::graphWatchTable", script::graphWatchTable, MARGS("graph", "watch"));
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
  BIND_FACADE(
      mgr, "coredcc::graphInstanceCount", script::graphInstanceCount, MARGS("ctx"));
}

void addSceneFacades(litestl::binding::BindingManager *mgr)
{
  BIND_FACADE(mgr, "coredcc::newVec3", script::newVec3, MARGS("x", "y", "z"));
  BIND_FACADE(mgr, "coredcc::vec3At", script::vec3At, MARGS("v", "i"));
  BIND_FACADE(mgr, "coredcc::vec3Put", script::vec3Put, MARGS("v", "i", "x"));

  BIND_FUNCTION(
      mgr, "coredcc::newTransformTypeDef", script::newTransformTypeDef, MARGS("id"))
      .returnsOwned();
  BIND_FACADE(mgr,
              "coredcc::transformTypeDefUiName",
              script::transformTypeDefUiName,
              MARGS("type", "s"));
  BIND_FACADE(mgr,
              "coredcc::transformTypeDefChannel",
              script::transformTypeDefChannel,
              MARGS("type", "name", "defaultValue", "distance"));
  BIND_FACADE(mgr,
              "coredcc::transformTypeDefVersion",
              script::transformTypeDefVersion,
              MARGS("type", "version"));
  BIND_FACADE(mgr,
              "coredcc::transformTypeDefEnumChannel",
              script::transformTypeDefEnumChannel,
              MARGS("type", "name"));
  BIND_FACADE(mgr,
              "coredcc::transformTypeDefChannelItem",
              script::transformTypeDefChannelItem,
              MARGS("type", "name", "key", "value", "uiName", "description"));
  BIND_FACADE(mgr,
              "coredcc::transformTypeDefChannelText",
              script::transformTypeDefChannelText,
              MARGS("type", "name", "what", "text"));
  BIND_FACADE(mgr,
              "coredcc::transformTypeDefChannelNumbers",
              script::transformTypeDefChannelNumbers,
              MARGS("type", "name", "what", "a", "b"));
  BIND_OWNING_FACADE(mgr,
                     "coredcc::registerForeignTransformType",
                     script::registerForeignTransformType,
                     3,
                     MARGS("ctx", "type", "model", "slots"));

  BIND_FACADE(mgr, "coredcc::newScene", script::newScene, MARGS("ctx", "name"));
  BIND_FACADE(mgr, "coredcc::setScene", script::setScene, MARGS("ctx", "scene"));
  BIND_FACADE(mgr, "coredcc::blockIdText", script::blockIdText, MARGS("ctx", "block"));
  BIND_FACADE(mgr, "coredcc::evaluateScene", script::evaluateScene, MARGS("ctx"));
  BIND_FACADE(
      mgr, "coredcc::sceneResults", script::sceneResults, MARGS("ctx", "places", "all"));
  BIND_FACADE(
      mgr, "coredcc::sceneObjects", script::sceneObjects, MARGS("ctx", "places", "all"));
  BIND_FACADE(
      mgr, "coredcc::sceneChildren", script::sceneChildren, MARGS("ctx", "object"));
  BIND_FACADE(mgr, "coredcc::worldMatrix", script::worldMatrix, MARGS("ctx", "object"));
  BIND_FACADE(
      mgr, "coredcc::notifyChanged", script::notifyChanged, MARGS("ctx", "block"));
  BIND_FACADE(
      mgr, "coredcc::pick", script::pick, MARGS("ctx", "view", "x", "y", "options"));
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
  addSceneFacades(mgr);
  BIND_FACADE(mgr, "coredcc::enableDrivers", script::enableDrivers, MARGS("ctx"));
  BIND_FACADE(mgr, "coredcc::disableDrivers", script::disableDrivers, MARGS("ctx"));
  BIND_FACADE(mgr, "coredcc::driverDiagnostics", script::driverDiagnostics, MARGS("ctx"));
  addToolBindings(mgr);
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
