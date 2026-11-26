# Naming

How coredcc names things that scripts and hosts see. New code follows these rules. The naming pass
(`plans/2026-11-26-naming-pass.md`) brought the existing names in line with them, and the
naming audit (`reports/naming-audit-2026-11-26.md`) explains each choice.

## Facades and TS functions
- **Verb first** when a facade's only subject is the Context: `saveLibrary`, `setActiveTool`, `updateDrag`.
- **Subject first** when the first argument is the thing acted on (a handle, a def or a path):
  `graphSetInput`, `commandDefInput`, `tableRowCount`.
- **A constructor** is `new<Type>`: `newGraph`, `newCommandDef`, `newInvocationChecks`.
- **A registration** is `registerForeign<Thing>`: `registerForeignCommand`, `registerForeignNodeType`.
- **A string form** is `<thing>Text`.
- **In TS,** actions are verb first, queries are noun phrases, and declarations are `define<Thing>`
  (`defineCommand`).
- **A TS name is the C++ name** of the same thing. The exception is when TS's flat namespace needs a
  qualifier: `sendToolEvent`, `sceneChildren`.
- **The facades live in `coredcc::script`.** A facade named like a library function hides that function there,
  or makes calls to it ambiguous. Qualify such calls with `coredcc::`, for example
  `coredcc::resolvePlaceholders(ctx)` inside the facade of the same name.

## Def builders
- Every builder facade starts with the name of its def: `commandDef*`, `toolDef*`, `nodeTypeDef*`,
  `transformTypeDef*`. Examples are `commandDefInput`, `nodeTypeDefParam` and `transformTypeDefChannel`.
- Their constructors are `newCommandDef`, `newToolDef`, `newNodeTypeDef` and `newTransformTypeDef`.

## Enums
- **A closed set of alternatives** is an `enum class` that is bound to scripts as an enum (`types::Enum`):
  `DepthMode`, `EventType`, `EventResult`, `ModalResult`, `DeltaKind`.
- **Open sets and bit masks** stay `int32` constants: `keys::`, `modifiers::`, `drawflags::` and the gizmo
  parts.
- **Datapath enum keys** are UPPER_SNAKE (`TRANSLATE`, `DRAG_DOT`), because they are stored in files.
- **Bound script enums** have Pascal items (`Exact`, `PassThrough`).
- **TS string unions keep their casing.** A union that TS defines is lowercase (`'xray'`, `'passThrough'`,
  `'pushMatrix'`). A union of strings that C++ JSON produces from enum item names is Pascal (`PathKind`,
  `ValueType`).
- **TS sees a bound enum as a number,** with a generated numeric `export enum`. Where the TS layer has its own
  string table (`depthModes`, `eventTypes`, ...), the table is keyed by the generated enum.

## Words with one meaning
- **`depth`** is NDC depth only (`ViewPoint::depth`, `ViewMath::unproject`'s argument). Every other kind of depth
  says what it is:
  - a depth-test mode is `depthMode`;
  - the matrix stack is `matrixDepth`;
  - the clip-space convention is `depthRange`;
  - the nesting of foreign calls is `callDepth`.
- **`viewDistance`** is a distance along the view direction, measured from the near plane: what
  `ViewMath::viewDistance` returns. Its limit is `maxViewDistance`.
- **`distance`** in a hit (`GizmoHit`, `PickHit`, `PickedObject`) is logical pixels from the cursor.
- **Counts** are `xCount()`: `recordCount`, `placeCount`, `tableRowCount`. **`size()`** is for containers. A
  number of bytes is `byteSize`.
- **Iteration** is `forEach<X>`: `forEachRef`, `forEachCell`.
- **Context types** are `XContext`: `EvalContext`, `NodeExecContext`.
- **Registries** check a def with `checkDef`. A registry that takes something else checks it with
  `check<Thing>` (`TransformRegistry::checkInfo`).
- **A DrawList record** is a record: `DrawRecord` and `DrawRecordKind` in C++, `DrawRecord` and `RecordKind`
  in TS.

## Command ids
- **The form is `core.<noun>.<verb>`,** with a singular, lowerCamel noun: `core.graph.node.add`,
  `core.element.applyDelta`, `core.property.set`, `core.stroke.apply`.
- **A block is made with `new`:** `core.graph.new`.
- **Only coredcc registers commands under `core.`.**
- **The constants** are `k<Verb><Noun>Command`: `kNewGraphCommand`, `kAddGraphLinkCommand`,
  `kApplyObjectDeltaCommand`.
- **In a layout, a button that runs a command is `<command>`.** A command's options are `<commandOptions>`.
  The old names `<tool>` and `<toolOptions>` are errors that name the new element.

## Registry and type names
- **ApiStruct names** are `coredcc.<Pascal>`: `coredcc.SceneObject`, `coredcc.ToolSettings`. A per-type struct
  adds `:<id>`, as in `coredcc.CommandDefaults:<command>`.
- **Reserved node type namespaces:** `core.`, `graph.`, `scene.`, `math.`, `vec.`, `mat4.`, `value.` and
  `elements.`. `registerForeignNodeType` refuses them.
- **To rename a stored type name** (a block, attachment, slot, transform, node or socket type), register the old
  name in the Runtime's `TypeAliases` (`lib/type_names.h`): `add(kind, old, current)`. A file that has the old
  name then loads under the current one and is saved with it.
