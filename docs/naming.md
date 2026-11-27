# Naming

How coredcc names things that scripts and hosts see. New code follows these rules. The naming pass
(`plans/2026-11-26-naming-pass.md`) brought the existing names in line with them, the naming audit
(`reports/naming-audit-2026-11-26.md`) explains each choice, and the pass's review
(`reports/feature-report-naming-2026-11-26.md`) settled the rules that conflicted.

## Facades
- **The Context is never the subject.** A facade's subject is its first argument after the Context, if that
  argument is the thing acted on: a handle, a def, or a path that names a block.
- **Subject first** when there is a subject: `graphSetInput(graph, …)`, `graphAddNode(ctx, graphPath, …)`,
  `commandDefInput(def, …)`, `tableRowCount(table)`.
- **Otherwise verb first for an action** (`saveLibrary(ctx)`, `setActiveTool(ctx, id)`,
  `setCommandOutput(ctx, name, value)`), and **a noun phrase for a query** (`activeTool(ctx)`,
  `commandInput(ctx, name)`).
- **A constructor** is `new<Type>`: `newGraph`, `newCommandDef`, `newInvocationChecks`, `newVec3`,
  `newErrorStatus`.
- **A PropValue** is made with `make<Kind>` (`makeFloat`, `makeVec3`, `makePointer`) and read with `read<Kind>`
  (`readFloat`, `readEnum`). These two families put the verb first although the value is the subject.
- **A registration** is `registerForeign<Thing>`: `registerForeignCommand`, `registerForeignNodeType`.
- **A string form** is `<thing>Text`: `valueText`, `blockIdText`.
- **The facades live in `coredcc::script`.** A facade named like a library function hides that function there,
  or makes calls to it ambiguous. Qualify such calls: `coredcc::resolvePlaceholders(ctx)` inside the facade of
  the same name, and `script::invoke` where a facade means the facade.
- **Error messages name the facade** that failed (`runAt: …`), not the TS function that called it.

## Def builders
- Every builder facade starts with the name of what it defines: `commandDef*`, `toolDef*`, `nodeTypeDef*`,
  `transformTypeDef*`. Examples are `commandDefInput`, `nodeTypeDefParam` and `transformTypeDefEnumChannel`.
- Their constructors are `newCommandDef`, `newToolDef`, `newNodeTypeDef` and `newTransformTypeDef`.
- **`XDef` vs `XInfo`:** a def is what a client builds and a registry keeps (`CommandDef`, `ToolDef`,
  `NodeTypeDef`). An info describes a registered type of the library or the scene (`BlockTypeInfo`,
  `AttachmentTypeInfo`, `TransformTypeInfo`). The builder facades are named for what they define, so
  `transformTypeDef*` fills a `TransformTypeInfo`.
- **id vs name:** a def's id is the dotted text a user writes (`core.graph.new`). The def stores that text as
  `name()`, and `id()` is its hash (`CommandId`, `TypeId`). Facade parameters call the text `id`.

## TS
- **TS types take the C++ name** of the same thing: `NodeExecContext`, `DrawRecord`, `InvocationChecks`. The
  exception is `ToolEvent`, which wraps `InputEvent`, because DOM declares a global `InputEvent`.
- **TS functions are verb first for actions** and noun phrases for queries, even where the facade is subject
  first: `addGraphNode` calls `graphAddNode`, and `readDrawList` calls `drawListRead`.
- **A TS function needs a qualifier** where TS's flat namespace would make the name unclear:
  `sendToolEvent`, `sceneChildren`.
- **Declarations are `define<Thing>`** (`defineCommand`, `defineNodeType`), the TS face of
  `registerForeign<Thing>`.
- **Short forms** for the datapath functions on a Context: `get`, `set`, `write`, `resolve`, `meta`, `format`
  and `parse`.
- **Methods follow the same rule:** `NodeExecContext`'s `setOutput`, `reportProgress` and `inputTable`.

## Enums and flags
- **A closed set of alternatives** is an `enum class` bound to scripts as an enum (`types::Enum`): `DepthMode`,
  `EventType`, `EventResult`, `ModalResult`, `DeltaKind`, `ElementDomain`.
- **Open sets** stay `int32` constants: `keys::`, `modifiers::`, `drawflags::` and the gizmo parts.
- **Bit masks** are `int32` constants, or a flag `enum class` (`CommandFlags`, `BlockFlags`, `ObjectFlags`,
  `InvalidateKinds` and the others). A flag enum may be bound to scripts as a bit-mask enum, as `CommandFlags` is.
- **Datapath enum keys** are UPPER_SNAKE (`TRANSLATE`, `DRAG_DOT`), because they are stored in files.
- **Bound script enums** have Pascal items (`Exact`, `PassThrough`).
- **TS string unions keep their casing.** A union that TS defines is lowercase (`'xray'`, `'passThrough'`,
  `'pushMatrix'`). A union of strings that C++ JSON produces from enum item names is Pascal (`PathKind`,
  `ValueType`).
- **TS sees a bound enum as a number,** with a generated numeric `export enum`. Where the TS layer has its own
  string table (`depthModes`, `eventTypes`, ...), the table is keyed by the generated enum.

## Words with one meaning
- **`depth` alone means NDC depth in view, draw and pick code** (`ViewPoint::depth`, `ViewMath::unproject`'s
  argument). Other depths there say what they are:
  - a depth-test mode is `depthMode`;
  - the matrix stack is `matrixDepth`;
  - the clip-space convention is `depthRange`.
- **A nesting depth is named for what nests,** unless its type already says it: `callDepth`, `parentDepth`,
  `emitDepth`, `LayoutLimits::kMaxElementDepth`, `GraphEvaluator::kMaxGraphDepth`, `graph::Frame::depth`.
  A local loop counter may be `depth`.
- **`viewDistance`** is a distance along the view direction, measured from the near plane: what
  `ViewMath::viewDistance` returns. Its limit is `maxViewDistance`.
- **`distance` in `GizmoHit`, `PickHit` and `PickedObject`** is logical pixels from the cursor. Other structs
  say in their docs what their `distance` measures (world units in `NearestHit` and `NearHit`, device pixels in
  `ScreenPoint`).
- **Counts** are `xCount()`: `recordCount`, `placeCount`, `tableRowCount`, `objectCount`. **`size()`** is for
  containers, registries included. A number of bytes is `byteSize`, also as a Command slot.
- **Iteration** is `forEach<X>`: `forEachRef`, `forEachCell`. A plain `forEach` is for a container's items.
- **Context types** are `XContext`: `EvalContext`, `NodeExecContext`.
- **Registries** check a def with `checkDef`. A registry that takes something else checks it with
  `check<Thing>` (`TransformRegistry::checkInfo`). A registry registers with `add`, or
  `add<Thing>` for a typed form (`SocketTypeRegistry::addPayloadType`).
- **Removing:** `delete` makes a block go away, `remove` takes something out of a container or a list, and
  `unlink` drops a link or a reference.
- **Booleans:** `has<Thing>` for presence (`hasPick`), `can<Verb>` for permission (`canRun`), and `is<State>`
  where the bare word would read as a noun or a verb (`isLinked`). A bare adjective is fine otherwise
  (`valid`, `frozen`, `alive`, `busy`).
- **Spelling is US English:** `center`, `color`.
- **A DrawList record** is a record: `DrawRecord` and `DrawRecordKind` in C++ (`DrawListReader::records`),
  `DrawRecord` and `RecordKind` in TS.

## Command and tool ids
- **The form is `core.<noun>.<verb>`,** with a singular, lowerCamel noun: `core.graph.node.add`,
  `core.element.applyDelta`, `core.property.set`, `core.stroke.apply`.
- **A block is made with `new`:** `core.graph.new`, `core.object.new`.
- **Tool ids** are `core.tool.<name>`, lowerCamel: `core.tool.transform`, `core.tool.stroke`.
- **Only coredcc registers under `core.`,** for commands, tools, stroke brushes, transform types and node types.
  The check ignores case, and refuses a bare `core` too.
- **The constants** are `k<Verb><Noun>Command`, with the id's noun: `kNewGraphCommand`, `kAddGraphLinkCommand`,
  `kSetObjectParentCommand`, `kApplyObjectDeltaCommand`.
- **To rename an id,** register the old one: `CommandRegistry::addAlias(old, current)` or
  `ToolRegistry::addAlias`. Invocations, layouts, last-used lines and tool settings then take the old id for the
  current one, and save the current one.
- **In a layout, a button that runs a command is `<command>`.** A command's options are `<commandOptions>`.
  The old names `<tool>` and `<toolOptions>` are errors that name the new element, in a file of any version and
  also inside a `custom` element.

## Registry and type names
- **ApiStruct names** are `coredcc.<Pascal>`: `coredcc.SceneObject`, `coredcc.ToolSettings`. A per-type struct
  adds `:<id>`, as in `coredcc.CommandDefaults:<command>` and `coredcc.TransformChannels:<type>`.
  `coredcc.ToolSettingsCheck:<tool>#<hash>` also has the hash of the settings' names and kinds.
- **Implementations** are registered as `<id>#impl` (commands, tools) or `<id>#kernel` (node types).
- **Reserved node type namespaces:** `core.`, `graph.`, `scene.`, `math.`, `vec.`, `mat4.`, `value.` and
  `elements.`, in any case. `registerForeignNodeType` refuses them, and refuses a bare `graph` too.
- **To rename a stored type name** (a block, attachment, slot, transform, node or socket type), register the old
  name in the Runtime's `TypeAliases` (`lib/type_names.h`): `add(kind, old, current)`. A file that has the old
  name then loads under the current one and is saved with it.
  - The old name must no longer be registered: `loadLibrary` and `appendLibrary` refuse to load while it is.
  - A block that has a slot or an attachment under both names is refused.
