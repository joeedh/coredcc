# The naming pass (before P6)

P6 binds the whole API for N-API. After that, every rename breaks bound names, scripts and saved files. This pass
makes the names consistent first.

- **Input:** the audit `docs/reports/naming-audit-2026-11-26.md` (every site, with counts). Its section letters
  (A-F) and question numbers (Q1-Q9) are used below.
- **Also closes:** item L of the tools-and-overlay review (`docs/reports/feature-report-tools-overlay-2026-11-26.md`)
  and the naming line of the P4 plan's "After the review".
- **Decisions (user, 2026-11-26):** "go with recommendations", which means the first option of each of Q1-Q9 below.

## Rules this pass adopts
These go into `docs/naming.md` (new, listed in `docs/index.md`), so that later code follows them.
- **Facades (Q1):** verb first when the first argument is only the Context (`saveLibrary`); subject first when it is
  the thing acted on: a handle, a def or a path (`graphSetInput`). A constructor is `new<Type>`. A registration is
  `registerForeign<Thing>`. A string form is `<thing>Text`.
- **TS:** actions are verb first, queries are noun phrases, declarations are `define<Thing>`. A TS name equals the C++
  name of the same thing unless a flat namespace needs a qualifier (`sendToolEvent`, `sceneChildren`).
- **Enums (Q2):** a closed set of alternatives is an `enum class`, bound to scripts as an enum. Open sets and bit masks
  stay int32 constants (`keys::`, `modifiers::`, `drawflags::`, gizmo parts).
- **Enum keys (Q5):** datapath enum keys are UPPER_SNAKE (they are stored); bound script enums are Pascal. TS string
  unions keep their casing (lowercase where TS defines them, Pascal where C++ JSON produces them).
- **"depth" (Q3):** means NDC depth only. A distance along the view direction from the near plane (what
  `ViewMath::viewDistance` returns) is `viewDistance`.
- **Command ids (Q4):** `core.<noun>.<verb>`, singular noun, lowerCamel; a block is made with `new`. Constants are
  `k<Verb><Noun>Command`. Only coredcc registers under `core.`.
- **Def builders (Q7):** every builder facade is prefixed with its def: `commandDef*`, `toolDef*`, `nodeTypeDef*`,
  `transformTypeDef*`; constructors `newCommandDef`, `newToolDef`, `newNodeTypeDef`, `newTransformTypeDef`.
- **Counts** are `xCount()`; `size()` is for containers. **Iteration** is `forEach<X>`. **Context types** are
  `XContext`. **Registries** check a def with `checkDef`.
- **ApiStruct names** are `coredcc.<Pascal>` (with `:<id>` for per-type structs).
- **Reserved node type namespaces (Q8):** `core.`, `graph.`, `scene.`, `math.`, `vec.`, `mat4.`, `value.`, `elements.`.

## Steps
Each step is one or more commits that build and pass the pre-commit gate on their own. Every rename covers C++, the
facade, TS, the native and wasm tests, the test app, the Unreal spike and the docs in the same commit. Section letters
point at the audit's tables, which list the sites.

1. **Compatibility for stored type names (Q8, audit §4).**
   - `detail::typeNamed` is a stateless `TypeId::of(name)` with no alias table, and these sites keep the name string
     (`Object::setTransformType` stores `typeName_` and saves it; `def_io.cc` stores `typeName_`/`typeName` and
     registries look types up by name). An id-level alias would load an old file and write the old name back.
   - So: a Runtime extension holding a name-to-name alias table (`TypeAliases`, with `add(kind, old, current)`), and
     `canonicalTypeName(rt, kind, name)` returning the current **name**. Use it where transform type names are read
     (`object_io`, before `setTransformType`) and where node and socket type names are read (`def_io.cc`, before the
     hash-collision check), so the current name is stored and saved. Block, attachment and slot types go through it
     too, beside `typeNamed`.
   - Tests: an aliased node type, socket type and transform type load from an old file and save under the new name.
   - No stored name is renamed by this pass, so no alias is registered outside tests.
2. **Reserve the built-in node namespaces (Q8).** `registerForeignNodeType` refuses `math.`, `vec.`, `mat4.`,
   `value.` and `elements.` beside `core.`, `graph.` and `scene.`. Test: each prefix is refused.
3. **Bind the enums (Q2, audit B).**
   - Bind `DepthMode`, `ShapeKind`, `EventType`, `PointerType`, `DeltaKind`, `StrokeSpace`, `StrokeMethod`,
     `Handedness` and `UpAxis` as `types::Enum`, as `SetWorldMode` and `EvalStatus` are. litestl binds enums as
     members, parameters, returns and slot returns (its conformance fixture covers all four), so no fallback.
   - `DepthRange` is litestl's `math::ClipDepth`. Binding it in coredcc would specialize litestl's Binder outside
     litestl, so it is bound in litestl by PR (`coredcc/bind-clip-depth`) first; until that merges, View keeps
     `depthRange()` and this step leaves `setConventions` taking it.
   - Setters don't range-check, so the existing checks stay, with casts: `InputEvent::valid()`, `checkStyle`
     (`draw/drawlist.cc`), `View::validate`, `stroke.cc`. The DrawList reader casts the byte.
   - Make the members typed and drop the int accessors: `typeValue`/`setType`, `pointerValue`/`setPointer`,
     `kindValue`/`setKind`, `View::depthRange()`/`hand()`/`upAxis()`/`setConventions`; bind `DrawList::shape`
     directly in place of `shapeOf`.
   - `eventresult` and `modalresult` become `enum class EventResult` and `enum class ModalResult`, also returned by
     the `onEvent` slots. TS `'pass'` becomes `'passThrough'`.
   - TS sees a bound enum as a number, with a generated numeric `export enum`. The TS layer's lowercase tables
     (`depthModes`, `shapeKinds`, `eventTypes`, `pointerTypes`, `deltaKinds`) stay: their strings differ from the
     item names (`'pointerdown'`, `'xray'`, `'0..1'`) and they decode DrawList bytes. They are keyed by the generated
     enums instead of literal numbers.
   - The Unreal spike changes in the same commit (`DrawListPdi.cpp`'s `GroupOf(int32)` takes the enum).
4. **"depth" (Q3, audit A1-A8):** `DrawStyle::depth` → `depthMode`; `DrawList::depth()` → `matrixDepth()` (and
   `setFloor`'s parameter); `GizmoHit::depth`, `PickHit::depth`, `PickedObject::depth` and `ViewPoint::distance` →
   `viewDistance` (all measured from the near plane); `PickQuery::limitDepth` → `maxViewDistance`; `View::depth` and TS `ViewSpec.depth` → `depthRange`;
   `ForeignBindings::depth()` and the TS-internal `scriptSlots.depth` → `callDepth`. The DrawList byte format is
   unchanged. `DrawStyle.depth` and `ViewSpec.depth` are optional in TS, so a caller still passing `depth:` would be
   ignored silently: grep the tree for it after the rename.
5. **Facades and TS (Q1, audit C1-C11, F4-F6).**
   - Facades: `librarySave/Load/Append/Resolve` → `saveLibrary`, `loadLibrary`, `appendLibrary`,
     `resolvePlaceholders`; `layoutJson` → `parseLayout`, `validateLayoutXml` → `validateLayout`; the `tool*`
     ToolManager facades → `setActiveTool`, `activeTool`, `sendToolEvent`, `drawTools`, `cancelTool`, `setToolStatus`,
     `setToolOutcome`, `pushModal`, `addGizmo`, `addForeignGizmo`, `removeGizmo`; `graphCreate` → `newGraph` (and C++
     `createGraph`; first rename the test helper `fixture::newGraph` to `makeGraph`, or the renamed call inside it
     recurses into itself); `graphInstance` → `newGraphInstance`; `sceneRefs` → `sceneObjects`; `dragUpdate`,
     `dragUpdateEach`, `elementsDragUpdate` → `updateDrag`, `updateDragEach`, `updateElementsDrag`.
   - Bags: `inputGet`, `outputGet`, `outputSet` → `commandInput`, `commandOutput`, `commandSetOutput`; graph kernels'
     `nodeOutput` and `nodeOutputTable` (which write) → `nodeSetOutput`, `nodeSetOutputTable`.
   - The facades live in `coredcc::script`, so a facade named like the library function hides it or makes the call
     ambiguous (`resolvePlaceholders(ctx)`, `parseLayout(view(xml))`): qualify those calls with `coredcc::`.
   - Counts: `tableRows`, `tableColumns` → `tableRowCount`, `tableColumnCount`; `SceneTable::places` →
     `placeCount`.
   - TS only: `sendEvent` → `sendToolEvent`; `children` → `sceneChildren`; `registerCommand` → `defineCommand`;
     `graphAddNode`, `graphRemoveNode`, `graphLink`, `graphUnlink`, `graphAddInterface` → `addGraphNode`,
     `removeGraphNode`, `addGraphLink`, `removeGraphLink`, `addGraphInterface`. TS `ToolEvent` keeps its name (audit
     F11 dropped): DOM declares a global `InputEvent`, and page code handles both.
6. **The DrawList's script methods (audit E):** `size` → `byteSize`, in C++ too (methods are bound under their C++
   name), and in `docs/drawlist-format.md`; `depth` is already `matrixDepth` (step 4);
   `shapeOf` is gone (step 3). TS `RecordKind` `'push' | 'pop'` → `'pushMatrix' | 'popMatrix'`. C++ `DrawItem` and
   `DrawKind` → `DrawRecord` and `DrawRecordKind`, to match TS (fewer sites than the other way).
7. **Command ids (Q4, audit D1-D4):** `graph.create` → `core.graph.new`; the other `graph.*` → `core.graph.*`
   (`graph.link`/`graph.unlink` → `core.graph.link.add`/`core.graph.link.remove`, matching `node.add`/`node.remove`);
   `graph.edit` → `core.graph.edit`; `core.elements.*` → `core.element.*`; `core.drivers.addGraph`/`removeGraph` →
   `core.driver.add`/`core.driver.remove` (input `graph`). `core.setProperty` → `core.property.set`; `core.stroke` →
   `core.stroke.apply`. The constants become `k<Verb><Noun>Command`. No compatibility: layouts, tool settings and
   last-used lines name command ids, but no layout in the tree names a renamed id, and nothing persists the other two.
8. **The layout's `<tool>` element (Q6):** the elements `<tool>` → `<command>` and `<toolOptions>` → `<commandOptions>`
   (the JSON `kind` comes from the same name table, so they move together); C++ `LayoutKind::Tool/ToolOptions`, the
   `draft.cc` element and attribute table, `validate.cc`, `LayoutBuilder::tool/toolOptions`, TS `LayoutKind`, the test
   app's `panel.ts`, `tests/layout`, `ta2.test.ts` and `tests/wasm/ui.test.ts`. `ToolChecks` and its facades →
   `InvocationChecks`, `newInvocationChecks`, `invocationChecksAdd`, `invocationChecksRun`. `tool` and `toolOptions`
   join the reserved element names, so a file of any version gets an error naming the new element instead of a skip.
9. **Def builders (Q7, audit F1-F3):** `def*` → `commandDef*`; `nodeType*` builder facades → `nodeTypeDef*`;
   `transformType*` and `transformChannel*` → `transformTypeDef*` (`transformTypeDefChannel`, ...); `newNodeType` →
   `newNodeTypeDef`, `newTransformType` → `newTransformTypeDef`. `toolDef*` is already right.
10. **The rest (audit F8-F12, F10):** `foreachRef`, `foreachDataRef`, `foreachCell`, `foreachCellOf` → `forEach*`
    (interface slots: the test app's providers and the Unreal spike change too); `EvalCtx` → `EvalContext` (Q9);
    `ToolRegistry::validateDef` → `checkDef`; `TransformRegistry::canAdd` → `checkInfo` (it takes a `TransformTypeInfo`);
    the five ApiStructs → `coredcc.ToolSettings`, `coredcc.ToolSettingsCheck`, `coredcc.CommandDefaults` (with their
    suffixes; also `tests/wasm/paths.types.ts`); `View::hand` is gone (step 3).
11. **Docs:** `docs/naming.md`; update `docs/embedding.md`, `docs/modules.md`, `docs/drawlist-format.md` and any guide
    that names a renamed thing. Plans and reports keep the old names (they are history). The task list gets a row.

## Checks
- Each step: the pre-commit gate. Each step that touches TS: `pnpm run typecheck` in `apps/testapp` and the wasm
  tests (`node tools/gate.mjs ci --only wasm-st-debug`, 2 jobs).
- The Unreal spike (`node tools/unreal-spike.mjs`) after steps 3, 4, 6 and 10, which change names it uses.
- After the last step: a grep for every old name (the audit's patterns, word-qualified for `depth`, `size`, `tool`
  and `push`, excluding the spike's `Intermediate/` and `Binaries/`), which must find hits only in `docs/plans/`,
  `docs/reports/` and the compatibility tests.
- Feature review: a fresh agent reviews the renamed API against `docs/naming.md`, as for other features.

## Not in this pass
Left alone, with the audit's reasons (§3): remove against delete, Id/Handle/Ref/Key, XInfo against XDef, `size()`
against `xCount()`, `fps` against `frameRate`, the scene file's `selected`/`active`, `coredcc.GraphDef`, the socket
type names, and `rerunLast` against `rerunHead`.

## Pressure test (2026-11-26)
A fresh agent tried to falsify the first version, with the plan, the audit and the code.
- **Survived:**
  - litestl binds enums as struct members, parameters, returns and interface-slot returns, in wasm and N-API
    (`extern/litestl/tests/bindings/fixture.h`, `js/conformance.test.ts`). The fallback in step 3 is gone.
  - No test or app registers node types under the namespaces step 2 reserves (foreign types use `ts.*` and `js.*`).
  - No C++ JSON key is renamed except the layout `kind` strings; generated bindings are rebuilt from scratch.
  - No collision for `depthRange` (once step 3 removes the accessor), `DrawRecord`, `byteSize`, `core.property.set`,
    `commandInput`, `sceneObjects` or `checkDef`.
- **Broke, and how the plan changed:**
  - `typeNamed` has no alias table and the sites keep name strings: step 1 adds a Runtime alias table that returns
    the current name.
  - TS sees enums as numbers, so the lowercase tables stay (step 3). `DepthRange` is litestl's `ClipDepth` and is bound
    in litestl by PR.
  - `viewDistance` is measured from the near plane, not the eye: the rule says so.
  - The facades in `coredcc::script` would hide or make ambiguous `resolvePlaceholders` and `parseLayout`; and the test
    helper `fixture::newGraph` would recurse into itself after `createGraph` → `newGraph`. Step 5 handles both.
  - TS `InputEvent` would shadow DOM's global: `ToolEvent` keeps its name.
  - `DrawList::size` is bound under its C++ name, so the C++ method is renamed too.
  - Step 8 missed `<toolOptions>` (whose JSON kind moves with it), `LayoutKind`, `draft.cc`, `validate.cc` and the
    test app's panel, and a newer-version file would skip an unknown `<tool>` silently: both old names become
    reserved.
  - Layouts do name command ids (step 7's wording); none in the tree names a renamed one.
  - The Unreal spike uses names from steps 3, 4, 6 and 10, so it runs after each of them.