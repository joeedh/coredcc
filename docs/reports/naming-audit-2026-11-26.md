# Naming audit of the public API (before P6 binds it for N-API)

Read-only audit of coredcc at `2e2ae00`, 2026-11-26. Every site below was read in the code. Paths are relative
to the repo root; `S/` means `source/coredcc/`. "Bound" means a name a script sees (a facade, a bound struct
member or method, an interface slot, a command id or a datapath name). "Stored" means a name written into
library files.

Break counts give the number of lines that name the thing, from `grep -rnw`, in four places: `S/` (the library and
the TS layer), `tests/` (native tests and `tests/wasm`), `apps/testapp/src` and `docs/`. The test app's own source
calls almost none of the facades directly, so it rarely breaks.

---

## 1. Prevailing conventions, with counts

### Facade names (`coredcc::<name>`, 212 bound in `S/bind/manager.cc` and `S/bind/tool_facades.cc`)
- **There are two conventions, and the first argument decides which applies:**
  - **Subject first** (`<subject><Verb><Noun>`) when the first argument is the thing acted on: a handle, a def or
    a path that names one. Examples: `graph*` (34), `node*` and `nodeType*` (21), `reader*` (9), `table*` (8),
    `toolDef*` (6), `bag*` (3), `bytes*` (2), `runInputs*`, `toolChecks*`, `vec3*`. About 110 names.
  - **Verb first** when the first argument is only the Context. Examples: `getValue`, `setValue`, `writeValue`,
    `formatValue`, `parseValue`, `resolvePath`, `invoke`, `invokeAt`, `runAt`, `canRunAt`, `beginInteractiveAt`,
    `commitInteractive`, `cancelInteractive`, `writeInteractive`, `rerunLast`, `undo`, `redo`, `beginGesture`,
    `cancelGesture`, `newScene`, `setScene`, `evaluateScene`, `notifyChanged`, `enableDrivers`,
    `disableDrivers`, `clearHistory`, `flushChanges`, plus `make*` (12) and `read*` (7). About 60 names.
- **Context-only facades that break the verb-first rule** (19), because their prefix names a subsystem of the
  Context rather than an argument: `library{Save,Load,Append,Resolve}`, `tool{SetActive,Event,Draw,Cancel,
  SetStatus,SetOutcome,PushModal,AddGizmo,AddForeignGizmo,RemoveGizmo}`, `dragUpdate`, `dragUpdateEach`,
  `elementsDragUpdate`, `outputSet` and `graphCreate`. This group is exactly the source of known item 3.
- **Constructors are `new<Type>`** (9): `newCommandDef`, `newToolDef`, `newNodeType`, `newTransformType`,
  `newRunInputs`, `newToolChecks`, `newPathReader`, `newTable` and `newScene`. The outlier is `graphInstance`,
  which also constructs but has no `new`.
- **Registration is `registerForeign<Thing>`** (5), plus `replaceForeignNodeType`. All agree.
- **A string form is `<thing>Text`**: `statusText`, `pointerText`, `blockIdText`, `graphErrorText` and
  `graphWatchErrorText`. All agree.

### TS layer (`S/bind/ts/*.ts`, a flat namespace through `index.ts`)
- **Actions are verb first** (about 45). Examples: `saveLibrary`, `loadLibrary`, `appendLibrary`,
  `resolvePlaceholders`, `parseLayout`, `validateLayout`, `setActiveTool`, `cancelTool`, `setToolStatus`,
  `setToolOutcome`, `pushModal`, `addGizmo`, `removeGizmo`, `sendEvent`, `drawTools`, `newScene`, `setScene`,
  `evaluateScene`, `enableDrivers`, `addDriverGraph`, `removeDriverGraph`, `beginInteractive`, `run`, `canRun`,
  `invoke`, `set`, `get`, `write`, `undo`, `redo`, `flushChanges`, `setWorld` and `pick`.
- **Queries are noun phrases.** Examples: `toolStatus`, `activeTool`, `sceneResults`, `sceneObjects`,
  `worldMatrix`, `driverDiagnostics`, `lastCommand` and `interactive`.
- **Declarations are `define<Thing>`** (4): `defineTool`, `defineStrokeBrush`, `defineNodeType` and
  `defineTransformType`. The outlier is `registerCommand`.
- **TS actions that are subject first** (9, the outliers): `graphCreate`, `graphAddNode`, `graphRemoveNode`,
  `graphLink`, `graphUnlink`, `graphAddInterface`, `dragUpdate`, `dragUpdateEach` and `elementsDragUpdate`.
  These are the TS functions that copied their facade's name.
- **The C++ library and TS already agree** for library IO and layout: C++ has `saveLibrary`, `loadLibrary`,
  `appendLibrary`, `resolvePlaceholders`, `parseLayout` and `validateLayout` in `S/api/library_io.h`,
  `S/lib/io/library_io.h` and `S/layout/xml.h`. Only the facades differ.

### C++ public headers
- **Containers have `size()` and other objects have `xCount()`.** There are 160 `size()` against about 45
  `xCount()` (`componentCount`, `sampleCount`, `planeCount`, `nodeCount`, `recordCount` and others). They
  agree; see §3.
- **Lookups are `find`** (34, plus 18 `findX`). `lookup` appears once, inside litestl.
- **Free functions that make typed blocks are `new<Block>`** (3): `newScene` (`S/scene/scene.h:241`),
  `newObject` (`S/scene/object.h:196`) and `newDriverList` (`S/drive/driver_list.h:84`). The outlier is
  `createGraph` (`S/graph/provider.h:21`).
- **Iteration is `forEach<X>`** (9): `forEach`, `forEachSlot`, `forEachOverride`, `forEachAttachment`,
  `forEachOpaqueAttachment`, `forEachLink`, `forEachNode`, `forEachElement` and `forEachRelated`. The outliers
  are `foreachRef`, `foreachDataRef`, `foreachCell` and `foreachCellOf` (4). `graph.foreach` and
  `foreachMaxCachedParam` are excluded, because there "foreach" is the name of a node.
- **Registries check a def with `checkDef`** (`CommandRegistry` and `NodeTypeRegistry`). The outliers are
  `ToolRegistry::validateDef` (`S/tool/tool.h`) and `TransformRegistry::canAdd` (`S/scene/transform_registry.h`).
  C++ only.
- **Context types are `XContext`**: `Context`, `NodeExecContext` (25 lines) and `RekeyContext` (6). The outlier
  is `EvalCtx` (43 lines in headers, 230 in total), which is bound and appears in TS slot signatures.
- **Command id constants are `k<Verb><Noun>Command`** (about 30, for example `kDeleteBlockCommand`,
  `kAddObjectCommand` and `kSetParentCommand`). The outliers mirror the id's order instead: `kGraphNodeAddCommand`
  and the other seven in `S/graph/commands.h:27-35`, plus `kElementsApplyDeltaCommand` and
  `kElementsDragDeltaCommand` (`S/tool/elements.h:102,104`). `kAddGraphCommand` and `kRemoveGraphCommand`
  (`S/drive/commands.h:17-18`) are ambiguous next to `graph.create`. C++ only.

### Enums crossing to scripts
- **Closed sets of alternatives are `enum class` in C++.** There are about 60 in `S/**/*.h`.
- **Bound as a script enum (`types::Enum`):** `StatusCode`, `PropValue::Type`, `CommandFlags`, `SetWorldMode` and
  `EvalStatus` (`S/scene/bindings.cc:9-34`). So binding an enum already works.
- **Not bound, so carried as int32 instead.** Three different patterns are in use:
  - **An enum member plus `xValue()` and `setX(int)`:** `InputEvent::typeValue/setType` and
    `pointerValue/setPointer` (`S/tool/input.h:126-141`), and `TransformDelta::kindValue/setKind`. That is 3
    enums.
  - **A raw `int32_t` member:** `DrawStyle::depth` (`S/draw/drawlist.h:59`), `StrokeSettings::space` and `method`
    (`S/tool/stroke.h:43-44`). Also 3. `DrawList::shapeOf(style, int32 kind, m)` (`S/draw/drawlist.h:166`) takes
    a ShapeKind as int32 too.
  - **Bare getters plus one setter for all of them:** `View::depthRange()`, `hand()`, `upAxis()` and
    `setConventions(depthRange, hand, upAxis)` (`S/draw/view.h:63-74`). Also 3.
- **Closed sets as int32 constant namespaces:** only `eventresult::{kUnhandled,kHandled}` and
  `modalresult::{kContinue,kPassThrough,kFinish,kCancel}` (`S/tool/tool.h:28-43`). This is known item 2.
- **Open sets and bit masks as int32 constants, which is correct:** `keys::`, `modifiers::`, `drawflags::` and
  the gizmo part indices (`S/tool/gizmos/gizmo_common.h:33-39`).

### Command ids (38 under `core.`, 10 under `graph.`)
- **Form:** `<ns>.<noun>.<verb>` or `<ns>.<noun>.<verbNoun>`, in lowerCamel. Examples: `core.object.setParent`
  and `core.block.unlinkRef`.
- **Namespace:** `core.` (38). The outliers are the graph commands: `graph.create`, `graph.node.add`,
  `graph.node.remove`, `graph.link`, `graph.unlink`, `graph.interface.{add,remove,move,setType}`
  (`S/graph/commands.h:27-35`) and `graph.edit` (`S/graph/ops.cc:605`).
- **Noun:** singular, as in `core.object`, `core.block`, `core.scene`, `core.library` and `core.tool` (5). The
  outliers are `core.elements.*` (2) and `core.drivers.*` (2).
- **Making a block:** `new` (`core.scene.new`). `core.object.add` adds to a scene, so it is a different
  operation. The outlier is `graph.create`.
- **Commands that have no noun:** `core.setProperty` and `core.stroke` (2). See question Q4.

### Datapath and registry names
- **ApiStruct names are `coredcc.<Pascal>`** (about 13), for example `coredcc.SceneObject`, `coredcc.GraphNode` and
  `coredcc.UnitSettings`. Per-type structs add `:<id>`, as in `coredcc.GraphParams:<type>`.
- **ApiStruct outliers with no namespace (5):** `ToolSettings` and `ToolSettings:<tool>` (`S/tool/settings.cc:11,177`),
  `ToolSettingsCheck:<tool>#<hash>` (`S/tool/registry.cc:103`), and `CommandDefaults` and `CommandDefaults:<cmd>`
  (`S/command/last_used.cc:21,219`). They are script-visible through `pathMeta`'s `struct` field and the catalog.
- **Datapath enum keys are UPPER_SNAKE** (about 70, for example `TRANSLATE`, `INPUT` and `DRAG_DOT`). Bound
  script enums are Pascal (`Exact`, `ModelFailed`). See Q5.

### TS string unions
- **Lowercase** for unions that TS defines itself (12): `DepthMode`, `RecordKind`, `ShapeKind`, `ToolEventType`,
  `ModalResult`, the pointer types, the delta kind, `side`, `handedness`, `up`, `ViewSpec.depth` and `LayoutKind`.
- **Pascal** for strings that C++ JSON produces from enum item names (5): `PathKind`, `ValueType`, layout
  `severity`, `NodeReport.state` and `ui.slider`.

---

## 2. Proposed renames

The columns are: current name and sites; proposal; the convention it follows; scope (C++, bound or stored); and
what breaks.

### A. "depth" (known item 1). Six meanings, not three
Every site was read. The proposal keeps the bare word `depth` for one meaning only, normalized-device-coordinate
depth (as in `ViewPoint::depth` and `ViewMath::unproject(x, y, depth)`), and qualifies the other five.

| # | Current | Means | Sites | Proposed | Scope | Breaks |
|---|---|---|---|---|---|---|
| A1 | `DrawStyle::depth` (int32 holding a DepthMode) | depth-test mode | `S/draw/drawlist.h:59`; bound `S/draw/bindings.cc:105`; uses `S/draw/drawlist.cc:53-54,137`, `S/draw/drawlist_reader.cc:162`, `S/tool/gizmos/transform_gizmo.cc:92`, `S/tool/select_tool.cc:141`, `S/tool/stroke_tool.cc:34`; TS `DrawStyle.depth` and `DrawRecord.depth` (`S/bind/ts/drawlist.ts:31,40,67-69,198`); testapp `apps/testapp/src/overlay/tessellate.ts:98`, `apps/testapp/tests/ta3.test.ts:19`; `tests/draw/test_drawlist.cc:32-37,92-94,156`; `tests/wasm/tool.test.ts:149,162`; `apps/unreal-spike/.../DrawListPdi.cpp:156` | `depthMode` (typed `DepthMode` if Q2 binds it) | bound, C++, TS | about 20 lines, 1 testapp file, the Unreal spike. The format byte is not renamed: `docs/drawlist-format.md:36` already says "depth mode" |
| A2 | `DrawList::depth()` | matrix stack depth | `S/draw/drawlist.h:118`; bound `S/draw/bindings.cc:128`; `S/draw/drawlist.cc:286-287`, `S/draw/drawlist.h:213`; `tests/draw/test_drawlist.cc:176,288,292`. `setFloor(int32_t depth)` at `:131` names its parameter after it | `matrixDepth()` (parameter `matrixDepth`) | bound, C++ | 7 lines |
| A3 | `GizmoHit::depth` | ViewMath::viewDistance | `S/tool/tool.h:153`; bound `S/tool/bindings.cc:77`; `S/tool/gizmos/transform_gizmo.cc:373`, `S/tool/manager.cc:275`; TS `GizmoHit.depth` (`S/bind/ts/tool.ts:358,383`); `tests/wasm/tool.test.ts:127` | `viewDistance` | bound, TS | about 6 lines |
| A4 | `PickHit::depth` and bound `PickedObject::depth` | viewDistance | `S/scene/pick.h:272`; `S/bind/scene_facades.h:168`; bound `S/bind/scene_facades.cc:318`; `S/bind/scene_facades.cc:294`; `S/scene/pick.cc:200,215,253,255,267-268,286-291`; `S/scene/pick_scene.cc:65`; TS `PickedObject.depth` (`S/bind/ts/scene.ts:467,545`) | `viewDistance` | bound, TS | about 15 lines in `S/`, plus pick tests (`grep -rn "\.depth" tests/scene`) |
| A5 | `PickQuery::limitDepth()` and the `limitDepth` constructor parameter | viewDistance | `S/scene/pick.h:144-148,164,201-203,257`; bound `S/scene/query_bindings.cc:65`; `S/scene/pick.cc:120-163`; `S/scene/pick_scene.cc:48,113,122` | `maxViewDistance` | bound, C++ | about 12 lines |
| A6 | `ViewPoint::distance` | viewDistance | `S/draw/view.h:86`; bound `S/draw/bindings.cc:49` | `viewDistance`, so that every hit and point uses one word for it | bound | a few lines |
| A7 | `View::depth` (a `DepthRange`) | clip-space convention | `S/draw/view.h:41`; `S/draw/view.cc:83,87,191,262,288,291`; `tests/tool/view_cases.h:91,107-108`, `tests/draw/test_view.cc:105,121-122,612`; TS `ViewSpec.depth` (`S/bind/ts/scene.ts:451,502`, `S/bind/ts/tool.ts:159`); `tests/wasm/scene.test.ts:315` | `depthRange`. The script accessor `depthRange()` (`S/draw/view.h:64`) then clashes, which disappears if Q2 binds the enum | C++, TS | about 20 lines |
| A8 | `ForeignBindings::depth()` | nesting of foreign calls | `S/bind/foreign_command.h` (inside `class ForeignBindings`); `S/bind/foreign_command.cc:135`. TS-internal `scriptSlots.depth` (`S/bind/ts/core.ts:165,200,391,395`, `S/bind/ts/graph.ts:250,257`) | `callDepth` | C++ only, and TS-internal (`index.ts` doesn't export `scriptSlots`) | about 8 lines |

- **Left alone:** `ViewPoint::depth` (`S/draw/view.h:84`, bound `S/draw/bindings.cc:48`) and the `depth` parameter of
  `ViewMath::unproject`, `unprojectTo` and `unprojectToView` (`S/draw/view.h:135,178,189`). They are the
  meaning that keeps the word.
- **Left alone:** `detail::Occluder::depth` (`S/scene/pick.h:310`). It is C++ internal (`detail`).
- **`distance` stays "logical pixels from the cursor"** in `GizmoHit`, `PickHit` and `PickedObject`, which
  already agree. Other structs keep `distance` where it means world distance (`NearestHit`) or distance along a
  stroke (`StrokeSample`); each is documented and lives in its own struct.

### B. Results and enums carried as numbers (known item 2)
| # | Current | Sites | Proposed | Convention | Scope | Breaks |
|---|---|---|---|---|---|---|
| B1 | `eventresult::kUnhandled/kHandled` (int32) | `S/tool/tool.h:28-31`; slot `Tool::onEvent` returns `int32_t` (`S/tool/tool.h:73`); `grep -rn "eventresult::" source tests apps` | `enum class EventResult : int32_t { Unhandled, Handled }`, bound as a `types::Enum` | enum class, as the other closed sets are | bound (an interface slot's return), C++ | every tool's `onEvent`, about 30 lines (`grep -rn "eventresult::"`) |
| B2 | `modalresult::kContinue/kPassThrough/kFinish/kCancel` | `S/tool/tool.h:34-43`; `Modal::onEvent` (`S/tool/tool.h:111`); TS `ModalResult = 'continue' \| 'pass' \| 'finish' \| 'cancel'` (`S/bind/ts/tool.ts:312`) | `enum class ModalResult { Continue, PassThrough, Finish, Cancel }`; TS `'pass'` becomes `'passThrough'`, so that the TS name and the C++ name agree | enum class | bound, TS | every modal, about 40 lines (`grep -rn "modalresult::\|'pass'"`) |
| B3 | `DepthMode`, `ShapeKind`, `EventType`, `PointerType`, `DeltaKind`, `StrokeSpace`, `StrokeMethod`, `DepthRange`, `Handedness` and `UpAxis` are unbound, so they are carried as int32 in three different shapes (§1) | `S/draw/drawlist.h:39,42,59,166`; `S/tool/input.h:126-141`; `S/tool/stroke.h:43-44`; `S/draw/view.h:63-74`; `TransformDelta::kindValue/setKind` | Bind them as `types::Enum`, as `SetWorldMode` and `EvalStatus` already are. Then make the members typed (`DrawStyle::depthMode` becomes a `DepthMode`), drop `typeValue`, `setType`, `pointerValue`, `setPointer`, `kindValue`, `setKind`, `depthRange()`, `hand()`, `upAxis()` and `setConventions`, and bind `shape(style, ShapeKind, m)` directly instead of `shapeOf`. If enums can't be members of bound structs, pick one accessor shape for all (Q2) | enum class | bound | the TS layer's index-to-string tables (`depthModes`, `shapeKinds`, `eventTypes`, `pointerTypes`, `deltaKinds`); about 40 lines in `S/bind/ts`; tests that set `s.depth = int32_t(...)` |
| B4 | `View::hand()` abbreviates the member `handedness` | `S/draw/view.h:63,68`; bound `S/draw/bindings.cc:30-31` | `handedness()` (or nothing, after B3) | no abbreviations in bound names | bound | 2 TS lines (`setConventions` calls) |

### C. Facade names that differ from C++ and TS (known item 3)
The C++ library and TS agree in each row below; only the facade is out of line. The proposal follows the
verb-first rule for Context-only facades (§1); Q1 asks whether to adopt it.

| # | Facade now (`S/bind/manager.cc` line) | C++ | TS | Proposed facade | Breaks |
|---|---|---|---|---|---|
| C1 | `librarySave` :244, `libraryLoad` :246, `libraryAppend` :250, `libraryResolve` :253 | `saveLibrary`, `loadLibrary`, `appendLibrary`, `resolvePlaceholders` | the same as C++ (`S/bind/ts/library.ts:75-102`) | `saveLibrary`, `loadLibrary`, `appendLibrary`, `resolvePlaceholders`. `libraryResolve` also hides what it resolves | S 28, tests 28, docs 6 |
| C2 | `layoutJson` :176, `validateLayoutXml` :178 | `parseLayout` and `layoutToJson`, `validateLayout` | `parseLayout`, `validateLayout` (`S/bind/ts/layout.ts:85,91`) | `parseLayout`, `validateLayout` | S 10, tests 8, docs 3 |
| C3 | `toolSetActive`, `toolActive`, `toolEvent`, `toolDraw`, `toolCancel`, `toolSetStatus`, `toolSetOutcome`, `toolPushModal`, `toolAddGizmo`, `toolAddForeignGizmo`, `toolRemoveGizmo` (`S/bind/tool_facades.cc:603-624`, `S/bind/tool_facades.h`) | `ToolManager::setActive`, `active`, `dispatch`, `draw`, `cancel`, `setStatus`, `setOutcome`, `pushModal`, `addGizmo`, `removeGizmo` | `setActiveTool`, `activeTool`, `sendEvent`, `drawTools`, `cancelTool`, `setToolStatus`, `setToolOutcome`, `pushModal`, `addGizmo`, `removeGizmo` | the TS names: `setActiveTool`, `activeTool`, `sendToolEvent`, `drawTools`, `cancelTool`, `setToolStatus`, `setToolOutcome`, `pushModal`, `addGizmo`, `addForeignGizmo`, `removeGizmo`. Also rename TS `sendEvent` to `sendToolEvent`: in a flat namespace, `sendEvent` doesn't say what receives it. `toolStatus` stays (a noun query) | S about 80, tests 14, docs 11 |
| C4 | `toolEvent` / `ToolManager::dispatch` / TS `sendEvent`: three names for one call | as C3 | | as C3, `sendToolEvent` | |
| C5 | `graphCreate` :351 | `createGraph` (`S/graph/provider.h:21`) | `graphCreate` (`S/bind/ts/graph.ts:284`) | `newGraph` in all three, and the command id `graph.create` becomes `core.graph.new` (D1) | S 7, tests 22, docs 2 |
| C6 | `graphInstance` :380 (it constructs) | `GraphInstance` | `new GraphInstance` | `newGraphInstance` | S 5, tests 15 |
| C7 | `sceneRefs` :498 | — | `sceneObjects` (`S/bind/ts/scene.ts:410`) | `sceneObjects` | S 6, tests 5, docs 3 |
| C8 | `sceneChildren` :500 | `Scene::children` | `children` (`S/bind/ts/scene.ts:416`) | TS `sceneChildren`: the facade is right, and `children` is too generic for a flat namespace | TS and tests only |
| C9 | `registerForeignCommand` | | `registerCommand` (`S/bind/ts/index.ts:304`) | TS `defineCommand`, as `defineTool`, `defineNodeType`, `defineTransformType` and `defineStrokeBrush` are | tests 34, `apps/testapp/src` 3, docs 12 |
| C10 | TS `graphAddNode`, `graphRemoveNode`, `graphLink`, `graphUnlink`, `graphAddInterface` (`S/bind/ts/graph.ts:288-310`) | | | TS `addGraphNode`, `removeGraphNode`, `addGraphLink`, `removeGraphLink`, `addGraphInterface` (TS actions are verb first). The facades keep their names, since `graphPath` is their subject | tests 26+ |
| C11 | TS `dragUpdate`, `dragUpdateEach`, `elementsDragUpdate` (`S/bind/ts/tool.ts:200-216`), and the facades of the same names | `ApplyDelta::update` | | TS `updateDrag`, `updateDragEach`, `updateElementsDrag`. The facades, under Q1: the same | S 7, tests 7, docs 4 |

### D. Command ids (bound, typed by users, used in layouts and last-used state)
| # | Current | Sites | Proposed | Convention | Breaks |
|---|---|---|---|---|---|
| D1 | `graph.create`, `graph.node.add`, `graph.node.remove`, `graph.link`, `graph.unlink`, `graph.interface.{add,remove,move,setType}`, `graph.edit` | `S/graph/commands.h:27-35`, `S/graph/ops.cc:605-606`; `grep -rn "graph\.\(create\|node\.\|link\|unlink\|interface\.\|edit\)" source tests docs` | `core.graph.new`, `core.graph.node.add`, and so on: the `core.` namespace. Today `registerForeignCommand` reserves only `core.` (`S/bind/foreign_command.cc:438`), so a script may register a command named `graph.anything` | `core.` | the ids in tests and layouts; docs `plans/2026-11-24-p5-graph.md` |
| D2 | `core.elements.applyDelta`, `core.elements.dragDelta` | `S/tool/elements.h:102,104` | `core.element.applyDelta`, `core.element.dragDelta` | singular noun | low (Q4) |
| D3 | `core.drivers.addGraph`, `core.drivers.removeGraph` | `S/drive/commands.h:17-18`; TS `addDriverGraph` and `removeDriverGraph` (`S/bind/ts/scene.ts:693,697`) | `core.driver.add` and `core.driver.remove` (input `graph`), or keep the plural (Q4) | singular noun | low |
| D4 | the constants `kGraph*Command`, `kElements*Command`, `kAddGraphCommand`, `kRemoveGraphCommand`, `kApplyDeltaCommand`, `kDragDeltaCommand` | as above | `kNewGraphCommand`, `kAddGraphNodeCommand`, ..., `kAddDriverGraphCommand`, `kApplyObjectDeltaCommand`, ... | `k<Verb><Noun>Command` | C++ only |

### E. The DrawList's script methods (known item 4)
- **Bound now** (`S/draw/bindings.cc:119-131`): `linesXyz`, `pointsXyz`, `trianglesXyz`, `shapeOf`, `pushMatrix`,
  `popMatrix`, `circle2d`, `line2dXy`, `recordCount`, `depth`, `frozen`, `address` and `size`.
- **Four name forms are mixed:**
  - a suffix that names the flat array's layout (`Xyz`, `Xy`);
  - an `Of` suffix that means "takes the kind as an int" (`shapeOf`);
  - the C++ name, where no variant was needed (`circle2d`, `pushMatrix`);
  - `depth`, which hides what it measures (A2).
- **Proposed rule:** a script variant is the C++ name plus the layout of its flat array. So keep `linesXyz`,
  `pointsXyz`, `trianglesXyz` and `line2dXy`. Replace `shapeOf` with `shape` bound directly (B3) or, until
  then, `shapeKindValue`. Rename `depth` to `matrixDepth`. Rename `size` to `byteSize`, because next to
  `recordCount`, `size` reads as a record count. Sites: `S/draw/drawlist.h:161-172`,
  `S/bind/ts/drawlist.ts:92-117,146`, and `tests/draw` (1 `shapeOf`).
- **TS record kinds against C++:**
  - TS `RecordKind` `'push' | 'pop'` (`S/bind/ts/drawlist.ts:19`) are `DrawKind::PushMatrix/PopMatrix` in C++
    and `pushMatrix`/`popMatrix` on `Drawer`. Proposed: `'pushMatrix' | 'popMatrix'`. Testapp
    `src/overlay/tessellate.ts` switches on them.
  - TS `DrawRecord` and `RecordKind` are `DrawItem` and `DrawKind` in C++ (`S/draw/drawlist.h:26,226`).
    Proposed: C++ `DrawRecord` and `DrawRecordKind`, or TS `DrawItem`, so that the two match. Owner's choice; C++
    has fewer sites.

### F. Other outliers in bound names
| # | Current | Sites | Proposed | Convention | Breaks |
|---|---|---|---|---|---|
| F1 | `defUiName`, `defDescription`, `defIcon`, `defFlags`, `defInput`, `defOutput` (the CommandDef builder) | `S/bind/manager.cc:159-166`, `S/bind/facades.h` | `commandDefUiName`, and so on | `toolDef*`, `nodeType*` and `transformType*` all name their def | S 7, tests 11, docs 10 |
| F2 | `transformChannel`, `transformEnumChannel`, `transformChannelItem`, `transformChannelText`, `transformChannelNumbers` | `S/bind/manager.cc:462-482` | `transformTypeChannel`, `transformTypeEnumChannel`, ... | `transformTypeUiName` and `transformTypeVersion` sit beside them | S 8+, tests 13 |
| F3 | `newNodeType` returns a `NodeTypeDef` | `S/bind/manager.cc:259` | `newNodeTypeDef` (and `newTransformType` becomes `newTransformTypeDef` if Q7 renames the prefixes) | `newCommandDef` and `newToolDef` name the def | S 4, tests 8 |
| F4 | `inputGet`, `outputGet`, `outputSet` (the running command's bags), against `nodeInput` (get) and `nodeOutput` (set) in graph kernels | `S/bind/manager.cc:128-131,314,331`; `S/bind/facades.h`, `S/bind/graph_facades.h` | `commandInput`, `commandOutput`, `commandSetOutput`; `nodeSetOutput`, `nodeSetOutputTable` | subject + `Set` + noun for writes, as `graphSetInput` and `graphSetInputTable` already do. Today `nodeOutput` writes while `outputGet` reads | S 27, tests 28 |
| F5 | `tableRows` and `tableColumns` return counts | `S/bind/manager.cc:336-337`, `S/bind/script_table.h` | `tableRowCount`, `tableColumnCount` | `xCount()` for counts (`readerCount`, `recordCount`) | S 4, TS 2 |
| F6 | `SceneTable::places()` returns a count while its siblings `parents()`, `members()` and `changed()` return lists | `S/bind/scene_facades.h` (`places()`), bound `S/bind/scene_facades.cc:359`; TS `SceneResults.places` (`S/bind/ts/scene.ts:372,395`) | `placeCount`. The C++ member is already called that | `xCount()` | 3 lines |
| F7 | `ToolChecks`, `newToolChecks`, `toolChecksAdd`, `toolChecksRun` check a layout's `<tool>` invocations. They sit next to the `tool*` facades of the ToolManager, which are unrelated | `S/bind/run_facades.h`; `S/bind/manager.cc:196-202`; TS `class ToolChecks` (`S/bind/ts/run.ts:84`) | `InvocationChecks`, `newInvocationChecks`, `invocationChecksAdd`, `invocationChecksRun`. See Q6 for the layout tag itself | one word, one meaning | S 10, tests 7 |
| F8 | the `foreachRef` interface slot of `BlockProvider`, `AttachmentProvider` and `SnapshotProvider`; `Library::foreachRef` and `foreachDataRef`; `foreachCell` and `foreachCellOf` | `S/lib/provider.h:75`, `S/lib/attachment.h:76`, `S/command/undo_strategies.h:117`, `S/lib/library.h:230,232`, `S/graph/def.h:596`, `S/drive/driver_list.h:63`, `S/scene/object.h:137-157`, `S/scene/scene.h:186` | `forEachRef`, `forEachDataRef`, `forEachCell`, `forEachCellOf` | `forEach` (9 against 4) | 169 lines across `source`, `tests` and `apps`. Interface slots break every host provider (the testapp's `BoxProvider` and `ItemProvider`, the Unreal spike), so do this now or never |
| F9 | `EvalCtx` | `grep -rnw EvalCtx` (230 lines); bound; TS `TransformModel` slots (`S/bind/ts/scene.ts:190-240`) | `EvalContext` | `Context`, `NodeExecContext`, `RekeyContext` | large but mechanical; Q9 |
| F10 | ApiStruct names `ToolSettings`, `ToolSettings:<id>`, `ToolSettingsCheck:<id>#<hash>`, `CommandDefaults`, `CommandDefaults:<id>` | `S/tool/settings.cc:11`, `S/tool/registry.cc:103`, `S/command/last_used.cc:21` | `coredcc.ToolSettings`, `coredcc.CommandDefaults` (with the same suffixes) | `coredcc.<Pascal>` | small; tests that compare `pathMeta().struct` |
| F11 | TS `ToolEvent` and `ToolEventType` against C++ `InputEvent` and `EventType` | `S/bind/ts/tool.ts:36,64`; `S/tool/input.h:21,80` | TS `InputEvent` and `InputEventType` (TS has no other `InputEvent`) | the TS name equals the C++ name | tests that import the types |
| F12 | `ToolRegistry::validateDef`, `TransformRegistry::canAdd` | `S/tool/tool.h`, `S/scene/transform_registry.h` | `checkDef` | `checkDef` (2) | C++ only, a few lines |

---

## 3. Considered and recommended to leave alone
- **`remove` against `delete`.** `delete` destroys a block (`core.block.delete`, `core.object.delete`).
  `remove` takes something out of a container (`graph.node.remove`, `core.drivers.removeGraph`, `removeGizmo`,
  `removeSlot`). The split is applied consistently.
- **`core.object.add` against `core.scene.new`.** Adding an object also links it into the scene, so `add` is the
  accurate verb.
- **`Id`, `Handle`, `Ref` and `Key`.** Each has one meaning:
  - `Id`: persistent (`BlockId`, `NodeId`, `LinkId`).
  - `Handle`: a generation-checked slot (`BlockHandle`, `SocketHandle`).
  - `Ref`: a reference value (`PointerRef`, `PayloadRef`, `SocketRef`, TS `Ref`).
  - `Key`: a hash key (`SocketKey`, `OwnerKey`).
- **`XInfo` against `XDef`.** `BlockTypeInfo`, `AttachmentTypeInfo` and `TransformTypeInfo` are plain structs
  with fields. `CommandDef`, `ToolDef` and `NodeTypeDef` are fluent builders. Three of each, and the split is
  meaningful.
- **`size()` against `xCount()`.** Containers have `size()` (`RefList`, `PickList`, `bagSize`, `bytesSize`).
  Counts inside other objects are `xCount()`. This holds throughout, apart from F5 and F6.
- **Def builders without an `add` verb** (`defInput`, `nodeTypeParam`, `toolDefSetting`). They follow the C++
  builders (`CommandDef::input`), consistently.
- **TS `get`, `set`, `format`, `parse`, `write`, `meta` and `resolve` without `Value` or `Path`.** These are the
  core datapath verbs of the TS namespace; it is a deliberate short form.
- **Integer constants for keys, modifiers, drawflags and gizmo parts.** These are open sets or bit masks.
- **The `*Text` suffix for string forms, and `uiName(s)` / `uiNameText()` on defs.** Each is consistent.
- **`fps` (datapath, `S/api/format.cc:324`) against `frameRate` (the file, `S/scene/scene_io.cc:20`) against
  `rate` (C++).** The datapath value is a float and the stored one a Vec2 fraction, so different names are
  honest.
- **`Transformable::setParentId` and `setTransformTypeId`** against `core.object.setParent`. The slot stores
  only an id, and the doc comment says so.
- **`begin`, `start`, `end`, `finish` and `commit`.** `start*` appears only in nouns (`startFrame`,
  `startState`). `begin` and `end` bracket, `commit` and `cancel` close an interactive command, and `finish`
  ends a sampler.
- **Bound enum keys in Pascal (`EvalStatus` `ModelFailed`) against datapath keys in UPPER_SNAKE
  (`MODEL_FAILED`, `S/scene/evaluated.cc:161`; `CLOSEST`, `S/scene/object_commands.cc:298`).** Each surface is
  internally consistent, and the UPPER keys are stored (B below). The one real cost is that one enum has two
  spellings; it should be documented rather than renamed (Q5).
- **Gizmo's `draw` slot against Tool's and Modal's `drawOverlay`.** Gizmo `draw` takes hover, active and idBase,
  so it is a different call; a shared name would suggest otherwise.
- **`rerunLast` (facade, TS) against `CommandExecutor::rerunHead`.** This is C++ internal drift with no script
  cost. Optional.

---

## 4. Stored names (the most expensive to change)
`detail::typeNamed` (`S/lib/type_names.h:12`) is the single compatibility point for block, attachment and slot
type names. **Transform type names and node or socket type names do not pass through it**:
- `transformType` is read with `TypeId::of(name)` (`S/scene/object.cc:49-53`).
- Graph files resolve node and socket types with `TypeId::of("node:"/"socket:" + name)` (`S/graph/def_io.cc:207-214`).

Renaming either would need a second alias path, or routing them through `typeNamed`.

| Name | Where stored | State | Recommendation |
|---|---|---|---|
| Block types `coredcc.Object`, `coredcc.Scene`, `coredcc.DriverList`, `coredcc.GraphDef` (`S/scene/object.h:58`, `S/scene/scene.h:17`, `S/drive/driver_list.h:26`, `S/graph/provider.h:15`) | every block record | consistent `coredcc.<Pascal>`. `GraphDef` is the C++ class name where the others are user terms | leave it, or alias `coredcc.Graph` through `typeNamed` now, while there are no users' files |
| Transform types `core.transform.trs`, `core.transform.fbx`, `core.transform.matrix` (`S/scene/builtins.h:11-15`) | the object field `transformType` (`S/scene/object_io.cc:17`) | consistent with `core.` ids | leave |
| Node types `value.<type>`, `math.float`, `vec.combine`, `vec.separate`, `mat4.multiply` (`S/graph/builtin_nodes.cc:107-145`); `elements.range`, `elements.count`, `elements.rename`, `elements.combine`, ... (`S/graph/element_nodes.cc:168-201`); `graph.input`, `graph.output`, `graph.group`, `graph.foreach` (`S/graph/node_type.h:274-277`); `scene.time`, `scene.object.state`, `scene.drive.<kind>`, `scene.transform.<name>`, `scene.missingTransform` (`S/drive/node_types.h:32-39`) | graph files | five unreserved built-in namespaces: `registerForeignNodeType` reserves only `core.`, `graph.` and `scene.` (`S/bind/graph_facades.cc:186-191`), so a script may claim `math.*`, `vec.*`, `value.*` or `elements.*`. `vec.*` and the `vec3` socket type spell it differently | decide now (Q8): reserve those namespaces, or move the built-ins under one prefix. No compatibility path exists yet |
| Socket types `bool`, `int`, `float`, `vec2`-`vec4`, `mat4`, `quat`, `string`, `elements`; and `coredcc.TransformState` (`S/graph/socket_type.cc:105-113`, `S/graph/foreach.h:59`, `S/drive/node_types.h:19,31`) | graph files | lowercase short names; `coredcc.TransformState` is the one namespaced Pascal name (the doc example in `S/graph/payload.h:24` is `my.Mesh`) | leave the built-ins; decide whether payload types are `ns.Pascal` (then document it) |
| Scene fields `objects`, `selected`, `active`, `frame`, `startFrame`, `frameRate` (`S/scene/scene_io.cc:15-20`) | scene blocks | `selected` and `active` are the datapath's `selectedObjects` and `activeObject` (`S/scene/scene.cc:594-618`) | optional; low value. If renamed, read both spellings |
| Object fields `transformType`, `transformVersion`, `flags`, `locks`, `parent`, `data` (`S/scene/object_io.cc:17-22`) | object blocks | match the datapath | leave |
| Datapath enum keys stored by key, such as graph params (`S/graph/def_io.cc:55-59`: "enums by key"); `ADD`, `MULTIPLY`, ..., `INPUT`, `OUTPUT` | graph files, tool settings and last-used lines (`S/tool/settings.h`: `core.tool.transform(mode='ROTATE')`) | UPPER_SNAKE throughout | leave |
| Command ids, inside the tool settings and last-used state (the `CommandDefaults:<id>` and `ToolSettings:<id>` keys) | serialized runtime state, and layouts | D1 renames `graph.*` | if D1 goes ahead, do it before anything persists those lines |

---

## 5. Questions for the owner
- **Q1. Facade naming rule.** Adopt "verb first when the first argument is only the Context, subject first when
  it is a handle, def or path"?
  - Counted, it is about 25 against 19 among Context-only actions.
  - C++ and TS already follow it, so adopting it makes the facades equal the TS names (C1-C3, C5, C11).
  - The alternative is renaming TS to match the facades (`librarySave` in TS), which would put TS at odds with
    C++.
- **Q2. Enums for scripts.** Bind every closed C++ enum as a `types::Enum` (as `SetWorldMode` and `EvalStatus`
  are) and drop the int accessors (B3)?
  - If members of a bound struct can't be enums in litestl, choose one accessor shape. Each is used 3 times:
    `xValue()`/`setX()`, a raw int32 member, or bare getters.
  - Make `eventresult` and `modalresult` enum classes (B1, B2)? Slot return types would then be enums, so this
    needs checking in the foreign-call path.
- **Q3. Which meaning keeps the bare "depth"?** The proposal keeps NDC depth and renames the other five (A).
  The alternative keeps "depth" for viewDistance, since gizmo and pick hits already use it that way, and renames
  `ViewPoint::depth` to `ndcDepth` instead.
- **Q4. Command namespaces.** Move `graph.*` under `core.` (D1)? Make `core.elements` and `core.drivers`
  singular (D2, D3)? Give `core.setProperty` and `core.stroke` a noun (`core.property.set`, and
  `core.stroke.apply` beside `core.tool.stroke`), or accept them as top-level?
- **Q5. Enum key spelling.** Accept UPPER_SNAKE datapath keys beside Pascal binding enums for the same C++ enum
  (`EvalStatus`, `SetWorldMode`)? Recommended: yes, and document it in `docs/embedding.md`. TS string unions are
  lowercase when TS made them and Pascal when C++ JSON did; keep that, or make them all one casing?
- **Q6. The word "tool".** The layout element `<tool invocation=...>` and `LayoutBuilder::tool` and `toolOptions`
  (`S/layout/xml.h:10`, `S/layout/builder.h:36-37`, TS `LayoutKind 'tool' | 'toolOptions'`) predate the
  interactive Tools of P4, and now mean "a command button". Rename the element (to `<operator>`, `<button>` or
  `<command>`)? It is stored in layout XML that apps write. F7 renames only the checker.
- **Q7. Def-builder prefixes.** The minimal fix is F1-F3 (`commandDef*`, `transformType*`, `newNodeTypeDef`). The
  full fix makes every builder prefix `<thing>Def` (`nodeTypeDef*`, `transformTypeDef*`): about 20 more
  renames, for one uniform rule.
- **Q8. Built-in node type namespaces** (§4). Reserve `math.`, `vec.`, `value.` and `elements.` for coredcc, or
  move the built-ins under `core.`? Either way, decide before graph files exist outside the tests, since node
  type names have no alias path.
- **Q9. `EvalCtx` to `EvalContext`** (230 lines). It is bound and script-visible in every TS transform model
  signature. Worth it now, or accepted as an established abbreviation?
