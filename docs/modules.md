# Modules

What is in `source/coredcc/`, where to start reading, and what depends on what. The design, and the reasons
for it, are in the [architecture plan](plans/2026-11-19-architecture.md) (§ below). The comments at the top of
the headers are the reference.

## Layers
A module uses those above it, and no others.

| Module | Target | Uses | What it is | § |
|---|---|---|---|---|
| `base/` | `coredcc_base` | litestl | `Status` and `Result`, ids, handles, signals, interfaces, `Runtime` | 2a |
| `props/` | `coredcc_props` | `base` | properties: definitions, values, bags, units | 4 |
| `parse/` | `coredcc_parse` | `base` | the parsers of datapaths and of command invocations | 5, 6 |
| `bvh/` | `coredcc_bvh` | `base` | BVHs: building, refitting, queries | 9 |
| `lib/` | `coredcc_lib` | `props` | the document: blocks, references, garbage collection, files | 8a |
| `draw/` | `coredcc_draw` | `bvh` | `View`: cameras and projections; the DrawList; `OverlayDrawable` | 10, 11 |
| `api/` | `coredcc_api` | `lib`, `props`, `parse` | datapaths: structs, resolving, change notifications | 5 |
| `command/` | `coredcc_command` | `api` | commands, the executor, undo | 6 |
| `layout/` | `coredcc_layout` | `command` | descriptions of UIs | 12 |
| `graph/` | `coredcc_graph` | `command` | the dependency graph | 7 |
| `scene/` | `coredcc_scene` | `command`, `bvh`, `draw` | objects, transforms, scenes, picking | 8, 9 |
| `drive/` | `coredcc_drive` | `graph`, `scene` | graphs that drive objects: `SceneDrivers` | 7, 8 |
| `tool/` | `coredcc_tool` | `scene`, `layout`, `draw` | input events, tools, gizmos, element transforms, strokes | 11 |
| `bind/` | `coredcc_bind` | `drive`, `graph`, `layout`, `scene`, `tool` | bindings for scripts | 13 |
| `testapp/` | `coredcc_testapp` | `bind` | the test app's backend. Not part of the library | — |

`scene/` and `graph/` don't know each other. P5c connects them.

## Terms
- **Runtime:** one instance of coredcc, with all of its registries. Several may be in one process.
- **Library:** a document's blocks. **Block:** a record with an id, a type and the data that the type's
  provider owns.
- **Context:** what paths are resolved in and commands run in: the Library, the active scene, the ChangeBus.
- **Interface:** a contract that a client implements, natively or in a script (`base/iface`).
- **Epoch:** a number of the Library (or the ChangeBus) that moves when its structure changes.

## Where to start

### `base/`
- `runtime.h`: `Runtime`, and extensions of it (`rt.ext<T>()`).
- `status.h`: there are no exceptions. Everything that can fail returns a `Status` or a `Result<T>`.
- `iface/iface.h`: how interfaces are declared, implemented and called.
- `invalidate.h`: the kinds of changes to geometry.

### `props/` and `parse/`
- `props/def.h`: `PropertyDef`, and `coerce`, the one place where values are converted.
- `props/units.h` and `unit_text.h`: values are stored in base units, and shown in others.
- `parse/path.h` and `parse/invocation.h`: the syntax of `a.b[0]["key"]` and `object.add(name="x")`.

### `lib/`
- `library.h`: `Library`. `block.h`: block records and their states.
- `provider.h`: `BlockProvider`, which a block type implements: its data, references and serialization.
- `attachment.h`: data that other modules hang on blocks.
- `type_names.h`: `TypeAliases`, so that a renamed block, attachment, slot, transform, node or socket type
  loads from files that have its old name.
- Saving and loading: `api/library_io.h`, and "Files" in [embedding.md](embedding.md#files).

### `api/`
- `context.h`: `Context`. `struct.h`: `ApiStruct`, the members that paths go through.
- `resolve.h`: reading, writing and canonicalizing paths.
- `change_bus.h`: who is told what changed.
- `format.h`: values as text and back, in the scene's units.

### `command/`
- `command.h`: `Command` and `CommandDef`. `executor.h`: running them, `UndoHistory` and `LinearHistory`.
- `set_property.h`: setting a path, undoably. `last_used.h`: the inputs of the last run.
- `undo_strategies.h`: what a command may use to undo, such as `SnapshotProvider`.

### `layout/`
- `xml.h` reads layouts, `builder.h` makes them in C++, and `validate.h` checks them against a Context's struct.

### `bvh/`
- `geometry_bvh.h`: a BVH of triangles, capsules or spheres. `bvh.h`: the generic one.
- `any_source.h`: primitives that a client describes at run time.
- `BvhOptions::leafSize` is 8: see [bvh-leafsize.md](bvh-leafsize.md).

### `scene/`
- `capabilities.h`: what any block may implement: `Transformable`, `Spatial`, `Invalidation`,
  `ElementAccess`. A host's own objects take part with these.
- `object.h` and `scene.h`: coredcc's own object and scene. `evaluateScene` computes world transforms and
  bounds.
- `transform.h`: `TransformModel`, for transform types. `models.h` has the built-in ones.
- `overrides.h`: `OverrideProvider`, for channels that something else drives.
- `pick.h`: picking. `commands.h`: `core.object.*` and `core.scene.*`.

### `draw/`
- `view.h`: `View`, what a host says of a viewport, and `ViewMath`, conversions between the world and pixels.
- `drawlist.h`: the DrawList, what tools and overlays draw, as bytes a renderer reads.
  [drawlist-format.md](drawlist-format.md) is the format. `overlay.h`: `OverlayDrawable`.

### `tool/`
- `input.h`: `InputEvent`. `manager.h`: `ToolManager`, which a host gives events and asks for a DrawList.
- `tool.h`: `Tool`, `Modal` and `Gizmo`, and `ToolDef`. `builtins.h`: `core.tool.select` and
  `core.tool.transform`; `gizmos/` has the transform gizmo.
- `elements.h`: `TransformableElements` and `Symmetry`, and the element commands.
- `stroke.h`: `StrokeSampler`, `StrokeBrush`, `core.stroke.apply` and `core.tool.stroke`.

### `graph/`
- `node_type.h`: node types and `GraphKernel`. `def.h`: `GraphDef`, a graph's nodes and links.
- `instance.h`: `GraphInstance`, evaluation. Only what a change reaches is evaluated again.
- `foreach.h`: a group for each element of a table. `interior.h`: links into groups, for programs that
  build graphs around a user's.

### `drive/`
- `drive.h`: `SceneDrivers`, the Library's `OverrideProvider` for the graphs of the document's list of
  drivers, and `registerDriveTypes`.
- `driver_list.h`: that list, a block type (`coredcc.DriverList`), and the root collection `driverGraphs`.
- `node_types.h`: the adapters of users' graphs (`scene.time`, `scene.object.state`, `scene.drive.<kind>`)
  and the transform node types. `sync.cc` makes the generated graph; `provider.cc` answers from it.

### `bind/`
- `facades.h` and the other `*_facades.h`: the functions that scripts call. `manager.h` registers all of it.
  Their names follow [naming.md](naming.md).
- `ts/`: the TypeScript layer, with plain values in and out and errors as exceptions. `ts/index.ts` has its
  rules.

## Examples
There is no tutorial yet. These tests are small and complete:
- `tests/command/`: commands and undo.
- `tests/scene/scene_fixture.h` and `pick_doc.h`: a document with a scene.
- `tests/graph/test_graph_scene.cc`: a graph that drives transforms.
- `tests/tool/tool_fixture.h`: a headless host of tools, driven by events.
- `tests/wasm/spike.test.ts`, `scene.test.ts`, `ui.test.ts` and `tool.test.ts`: all of it from TypeScript.
- `source/coredcc/testapp/`: a host. `apps/unreal-spike/`: an Unreal Editor host of the history and the tools.
