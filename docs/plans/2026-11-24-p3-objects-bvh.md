# P3: objects, transforms and BVH

- Status: draft, not pressure-tested yet.
- Branch: `p3-scene`, in the worktree `C:\dev\coredcc-p3-scene`.
- Architecture: §8 (objects and transforms), §9 (BVH), §10 (View), §2a (interfaces), §14.
- Feature reviews: **objects and transforms**, and **BVH**.

## Ownership between the parallel phases
The same text is in the P2, P3 and P5 plans.

| Phase | Owns |
|---|---|
| P2 | Value bytes (a bounded, versioned reader/writer for PropValue and PropertyBag); BlockProvider's optional `serialize`, `deserialize`, `duplicate` and `rekey`; the attachment type registry and the attach/detach commands; `Library::rename`; snapshot undo and lz4; CreatedBlocks with BlockIds; `layout/`; TA2. The shared pieces (value bytes, provider slots) are P2's **first step**, merged to master early so P3 and P5 can rebase onto them. |
| P3 | `scene/`, `bvh/`, `draw/View` if it needs it for screen distance, EvalCtx, `CollectionAccessor::setActive`, the Context scene slot, display units and frames. Its providers' `serialize` is its **last step**. |
| P5 | `graph/`, the Runtime's global version counter, payloads. GraphDef serialization is its **last step**. |
| Shared files | `source/coredcc/CMakeLists.txt`, `tests/CMakeLists.txt`, the task list, `debugging.md`, `docs/embedding.md`: small appended changes only. Task list: only the phase's own row and focus lines. No phase edits CLAUDE.md. |

## Scope

### In
- **From architecture §14:** TransformModel, TrsTransform, FbxTransform, Object, its
  capabilities, Scene; BVH, its built-in sources, SceneBvh with two-phase queries; the
  leaf-size benchmark.
- **Moved here from P4:** `draw/View` only (camera math and conventions). Picking has to
  report screen distances, so it needs one. The DrawList, events and tools stay in P4.
- **Deferred to P3 by earlier phases:**
  - `CollectionAccessor::setActive` (P1c/P1d review, D2);
  - the Context's scene slot (P1d; its ToolManager slot goes to P4 with the ToolManager);
  - display units and the subtype/unit agreement check (props review, D12);
  - frames as a time unit (props review, D12).
- **The litestl math PR ("PR D")**, planned for before P3 and never written. See step 0.

### Deferred to P3 by the command review, moved to P4
- Scripting interactive commands, and `runSub` of an Interactive def. The review deferred
  them "with the tools", and the tools are P4.

### Out
- The scene evaluator as a graph client: generated nodes, override inputs, adapter nodes,
  the apply phase (P5c).
- DrawList, OverlayDrawable drawing anything, gizmos, TransformableElements, Symmetry (P4).
- Attributes, Triangles and ActiveElement (deferred in §14).
- Files beyond the providers' `serialize` (P2 owns library IO).
- Incremental (dynamic) BVH insertion and removal. Rebuild and refit only.
- FBX's `SphericXYZ` rotation order, and IK or constraint evaluation.

## What the code says (checked 2026-11-24)
Scratch files are in the session scratchpad's `p3\` folder (`a.cc`, `q.cc`, `emb.cc`,
`fbx.mjs`, `iface.cc`, `hc.cc`, `mt.cc`).

- **litestl's math group never landed.** P0b merged A, B and C1 (#4–#6), and P1 merged C2.
  `math/quat.h` still doesn't compile (MSVC and clang-cl). There are no Euler conversions,
  no decompose and no bound matrix type.
- **`Matrix * Vec` is the transpose.** With a column-major translation of (5, 6, 7),
  `mat4d * double3(1, 2, 3)` gives (1, 2, 3), and the `double4` version adds 39 to w.
  `Matrix * Matrix` (through Eigen) is correct. `operator*=` returns a reference to a
  local, and `operator[]` has no const overload.
- **A singular `Matrix::inverse()` returns NaN**, with no error.
- **litestl's AABB:** the default constructor makes a box that isn't empty (both corners at
  the negative limit). An empty box's `size()` is −inf. `isEmpty` isn't const. Its binding
  has no copy constructor, so a script can't return one from a slot.
- **litestl's `rayTriIsect` has an absolute epsilon** (1e-6 on the determinant): a triangle
  with 1e-4 edges is never hit. `aabbRayIsects` skips axes whose direction is below 1e-4.
  P3 uses neither.
- **Nobody outside litestl uses `Matrix`'s Eigen conversions** in sculptcore or UberTopo
  (sculptcore includes Eigen's sparse headers itself).
- **Interfaces carry these shapes.** A prototype interface took a 184-byte struct by const
  reference and returned one by value, took an interface value by value (null included),
  and returned `Vector<int32_t>`:
  - native, and through the script dispatcher with the fake host, in both directions;
  - `foreignImplementable` accepts it;
  - a failed foreign call gave a default value plus the sticky error;
  - about 18 ns per native call through the table (clang-cl, /O2).
- **Foreign dispatch limits** (`typescriptRuntime/foreign.ts`): a `Vector&` argument reaches
  JS as a *copy*, so it can't be an output. A returned `Vector` may hold scalars only. A
  returned struct needs a bound copy constructor. A `void *` or `QueryAnswer` slot is
  refused, so a script can't implement BlockProvider.
- **Calling a forwarder on a null interface** asserts in Debug and crashes in Release, and
  `bool(iface)` isn't bound. So a script can't test a null interface argument.
- **`task::parallel_for` is unused in coredcc so far.** It blocks the caller, and nested
  waits can starve the pool. `parallel_for_chunks` and `parallel_reduce` have chunk
  boundaries independent of the worker count and run serially inside a worker.
  - wasm-st: `hardware_concurrency()` is 1, so it is serial.
  - wasm-mt under node: works with and without `PTHREAD_POOL_SIZE`. Blocking a browser's
    main thread is not tested.
- **A Strong ref from Scene to its objects would delete the Scene with any object**:
  `deletePreview` follows `RefKind::Strong`.
- **`onWeakTargetLost` runs at the sweep**, not at delete (`markDeleted`). It can't keep a
  child's world transform when its parent is deleted.
- **`refine` results and candidates accept subtypes** (P1d review), so new transform models
  don't have to be listed when Object's struct is registered.

### FBX inherit types, checked numerically
`fbx.mjs` evaluates the formulas of Autodesk's FBX SDK "Transformations" sample. Columns are
basis vectors; `GR` is the parent's global rotation; `GSM = GR⁻¹ · linear(parentWorld)`; `LR`
and `LS` are the node's rotation and scale; `pls` is the parent's *local* scale.

| Type | Linear part of the world matrix |
|---|---|
| RrSs | `GR · LR · GSM · LS` |
| RSrs | `GR · GSM · LR · LS` (= `parentWorld · local`) |
| Rrs | `GR · LR · GSM · S(pls)⁻¹ · LS` |

The translation is `parentWorld · localTranslation` for all three. Results over 2000 random
chains each:

- **RSrs equals the plain TRS product,** 4 deep with non-uniform scale: max error 6e-14.
- **With uniform scale, RrSs equals RSrs** (9e-14). With non-uniform scale they differ (32).
- **An all-RrSs chain equals `R(all) · S(product)`** (2e-14), so it never shears.
- **An all-Rrs chain equals Maya's segment scale compensation**, `P·T·S(pls)⁻¹·R·S`, 3 deep
  (9e-15).
- **Where `GR` comes from matters.** Decomposing it from the parent's matrix and carrying it
  down the chain (`GR · LR`) agree to 6e-14 with positive scales and no shear. They
  disagree by 16 when the parent is reflected in y or z, and by 17 when it is sheared.
  - Carrying it gives `R(all) · S(product)` for reflected RrSs chains. Decomposing doesn't.
- **Inverting RrSs and Rrs for setWorld.** Solve `X = LR · A · LS` for a known `A`:
  `LS[i] = |X col i| / |A col i|` (one sign flipped if the determinants' signs differ), then
  `LR = X · LS⁻¹ · A⁻¹`.
  - It recovers rotation and scale to 2e-15, even under a sheared parent.
  - The obvious QR method is wrong by 6 to 8 there.
  - For an arbitrary target under a sheared parent, `LR` isn't orthonormal (residual at
    least 6e-3), which is how "not representable" is detected.

Not checked against the FBX SDK itself, which isn't installed.

## Key decisions
1. **A TransformModel is stateless.** Channel values live with the object as a flat array of
   doubles, and reach the model through EvalCtx. This replaces §8's per-object model
   objects.
   - P5c's channel overrides become a second array that EvalCtx reads first. No model code
     changes.
   - Serialization, duplication, undo and animation are generic over channels.
   - A foreign model is one JS object per type, not one per scene object.
   - `evaluate` depends on its arguments only, which is what a graph node's `exec` requires.
2. **Parents are passed as data, not as interfaces.** `evaluate` takes the parent's
   `TransformState {world, rotation, localScale}` instead of §8's `TransformModel parent`.
   - It holds everything FBX's rules need (`GR` is `rotation`, `pls` is `localScale`).
   - In P5c the state is a socket value. A parent interface would make `exec` read another
     object.
   - Scripts can't test a null interface argument. A root gets the identity state.
   - §8's `components()` is replaced by the state, so a TRS parent works under an FBX child.
3. **`GR` is carried down the chain**, not decomposed, so reflections work. Where it differs
   from the SDK sample (a sheared or reflected parent) is documented and pinned by a test.
4. **Objects are found through a capability, not a class.** Scene members are blocks that
   implement `Transformable`. `coredcc.Object` is the built-in one; an Unreal actor block
   can be another.
5. **Until P5c, `Scene::evaluate()` recomputes every object.** There is no dirty tracking,
   so no stale cache and no need for P5's version counter. At about 20 ns per native model
   call, 100k objects take a few ms. Readers only use `Scene::evaluated(handle)`, and P5c
   replaces what is behind `evaluate()`.
6. **Picking's broad phase uses planes.** A pixel-radius region is the frustum of a screen
   square. World planes go to object space as `(Mᵀn, n·t + d)`, which is exact for any
   affine matrix, singular ones included, and needs no inverse. This replaces §9's bound by
   the largest singular value.
7. **BVH traversal computes in double.** Nodes store the source's scalar, rounded outward.
8. **Scene membership uses OptStrong refs**, not Strong.
9. **litestl gets the math** (architecture §2: general fixes go into litestl), and BVH, which
   doesn't need it, goes first so the PR doesn't block everything.

## Step 0: litestl PR D (`coredcc/math`)
Made in this worktree's `extern/litestl` on a branch, after checking the open PRs. Additive
for existing users, except that `matrix.h` stops including Eigen.

- **`math/quat.h`** rewritten: `Quat<Float>`, stored xyzw (as `PropValue::Quat`), trivially
  copyable. Identity, product, conjugate, inverse, normalize (a zero or non-finite
  quaternion gives the identity), rotate a vector, to and from a rotation matrix, axis-angle,
  slerp. `quatd` and `quatf`.
- **`math/euler.h`** (new): the 6 Tait-Bryan orders, `eulerToQuat`, `quatToEuler`, and
  `quatToEuler(q, order, near)`, which picks the solution closest to an earlier value.
- **`math/transform.h`** (new): `transformPoint`, `transformVector`, `transformNormal`,
  `transformPlane`; `translation`, `rotation`, `scaling`, `composeTrs`; `decomposeTrs(m) ->
  {t, q, s, shear, ok}` by Gram-Schmidt, reflections on x; `tryInverse(m, out) -> bool`;
  `largestSingularValue3`; `perspective` and `orthographic` (depth range and reversed-Z
  parameters, infinite far), `lookAt`.
- **`math/matrix.h`:** own multiply, determinant and inverse instead of Eigen; const
  `operator[]`; `operator*=` returns `*this`; trivially copyable. `operator*(Vec)` keeps its
  (transposed) behaviour, with a comment pointing to `transformPoint`; changing it could
  break another repo silently.
- **`math/aabb.h`:** const `isEmpty`.
- **`math/math_bindings.h`:** `Binder` for `Matrix` and `Quat` (default and copy
  constructors, `at(i)` and `put(i, v)`), and a copy constructor for `AABB`.
- Tests: round trips of each conversion in all orders, including gimbal lock; decompose and
  compose; inverse against the product; the old Matrix test.

Until it merges, the branch builds against the PR commit with the gitlink uncommitted, which
the task list allows. Steps 1 to 4 don't need it. Steps 5 on do, and they are merged to
master only after the gitlink bump.

## BVH (`bvh/`)

### Core
```cpp
template <class Scalar> struct BvhNode {   // 32 bytes for float
  Scalar min[3]; uint32_t rightOrFirst;    // inner: right child (left is next); leaf: first
  Scalar max[3]; uint32_t count;           // 0 for an inner node
};

template <class S> concept BvhSource = requires(const S &s, int i) {
  typename S::Scalar;
  { s.size() } -> std::convertible_to<int>;
  { s.bounds(i) } -> std::same_as<Box3<typename S::Scalar>>;
};

struct BvhOptions { int leafSize = 128; int bins = 16; bool parallel = true; };

template <BvhSource S> class Bvh {
 public:
  Status build(const S &src, BvhOptions opts = {});
  Status refit(const S &src);          // same size and the same skipped set as the build
  double cost() const;                 // SAH cost, for the refit-or-rebuild choice
  int size() const; int skipped() const;
  Box3<Scalar> bounds() const;
  const Vector<int> &order() const;    // leaf position -> primitive
  template <class NodeTest, class Leaf> void traverse(NodeTest &&, Leaf &&) const;
  ...
};
```
- **`Box3<Scalar>`** (`bvh/box.h`) is coredcc's: empty by default (min +max, max −max),
  `valid()` is "finite and min ≤ max", and an empty box stays empty under any transform.
  `Box3d` is bound with a copy constructor. Nothing uses litestl's AABB.
- **Finite checks test exponent bits**, so they still work if a host compiles the templates
  with fast-math (§2: coredcc doesn't depend on FP flags).
- **Build.** Binned SAH, 16 bins per axis over centroids (the bounds' centers), areas in
  double so huge float bounds don't overflow.
  - A split that puts everything on one side (equal centroids), or a depth above 48, uses a
    median split of the order array. So the depth is at most 48 + 31, and traversal uses a
    fixed 96-entry stack.
  - **Parallel:** the top of the tree is split serially, with bins computed by
    `parallel_reduce` over fixed chunks, until there are at most 64 subtrees or every
    subtree is below 4096 primitives. Subtrees are then built by one `parallel_for_chunks`
    (chunk 1), each into its own node list, and appended in order.
  - The result is bit-identical for any worker count, and serial inside a pool worker.
    coredcc never calls `task::run`.
- **Refit** is bottom-up over the node array in reverse, since children follow parents.
- **Failure:** more than 2³¹−1 primitives is OutOfRange. An empty source builds an empty BVH
  with no hits. Primitives whose bounds aren't finite are left out, counted by `skipped()`,
  and never hit.

### Built-in sources (leaf-ordered structure of arrays)
- **`TriangleSource<Scalar>`**: positions and index triples. Degenerate triangles (zero
  area) stay in the tree and can't be hit by a ray, but can be found by nearest queries,
  where they are a segment or a point.
- **`CapsuleSource<Scalar>`**: segments with a radius. A zero-length segment is a sphere.
- **`SphereSource<Scalar>`**: centers and radii. Radius 0 is a point.
- **`BoxSource`** (double): what SceneBvh and foreign sources use.
- Radii are in object units. A negative or NaN radius skips the primitive.

`GeometryBvh<Source>` (`bvh/geometry_bvh.h`) owns a leaf-ordered copy of the primitives, the
BVH and the id map. `onInvalidate(kinds)` refits on Positions and rebuilds on Topology.

### Queries (object space, double)
- **Ray:** closest, any, all. `t` in `[tMin, tMax]`, in units of `dir`, which needn't be unit
  length. Any non-finite component, or a zero direction, gives no hits.
- **Nearest within a radius,** and **sphere overlap**.
- **Region:** up to 6 planes (frustum or box). Returns candidates, not exact overlaps.
- **Kernels** (`bvh/kernels.h`), each with a brute-force reference in the tests:
  - ray-triangle: Möller-Trumbore with a *relative* determinant test (|det| against the
    edge lengths and the direction), double sided;
  - ray-capsule, ray-sphere (a ray starting inside hits at `t = tMin`, flagged `inside`);
  - point-triangle, point-segment and point-box distance.
- The ray-box slab test pads `tMax` by 4 ulp, so a ray through a shared edge can't miss
  both boxes.

### Foreign sources
```cpp
struct AnyBvhSource : IfaceRef {
  int size() const;
  Vector<double> bounds(int first, int count) const;      // 6 per primitive
  Vector<double> rayTest(const Vector<int> &prims, const Ray3d &ray) const;  // optional
};
```
- Bulk on purpose: `geometry-contract.md` §4 forbids per-element round trips. `rayTest`
  returns one `t` per primitive, NaN for a miss. Without it a box hit is the answer.
- **`AnyBvh::build`** gathers all bounds first, on the calling thread, in chunks of 4096.
  Only then does it build in parallel. That is what `threadAffine` needs, and native sources
  flagged ThreadAffine are treated the same. No slot is called from a worker.
- A wrong-length result, or a sticky error after a chunk, fails the build and keeps the
  previous tree.

### Leaf-size benchmark
- `tests/bench/bench_bvh_leafsize.cc`, a target outside `all` and ctest.
- One command, run by the coordinator on an idle machine:
  `node tools/bench-bvh.mjs [--quick] [--wasm]`. It builds the release target, runs it, and
  writes `docs/bvh-leafsize.md` (machine, compiler, commit, tables).
- Leaf sizes 16, 32, 64, 128, 256, 512, 1024; 10k, 1M and 10M triangles, capsules and
  spheres; closest-ray and region queries, coherent and random; build and refit time,
  node memory. Fixed seeds, best of 5. `--quick` stops at 1M. `--wasm` runs up to 1M in node.
- It warns when other processes use more than 10% of the CPU.
- The default becomes the best geometric mean of query time, among sizes whose build time is
  within 10% of the fastest. If the winner is within 5% of 128, 128 stays.

## View (`draw/view.h`)
```cpp
struct View {
  double viewport[4];            // x, y, width, height, pixels, y down
  mat4d view, proj;
  DepthRange depth;              // ZeroToOne, MinusOneToOne
  bool reversedZ, infiniteFar, ortho;
  Handedness handedness; UpAxis up;
  double dpiScale;
  Status validate() const;       // finite, invertible, positive viewport
};
```
- **`ViewMath`**, built from a valid View, caches the inverses: `project(p) -> {pixel, depth,
  behind}`, `unproject(pixel, ndcDepth)`, `rayFromPixel`, `regionFromPixel(px, radius) ->
  planes`, `pixelSizeAt(p)`, `viewDistance(p)`.
  - It inverts the view and the projection separately, and takes the ray's origin from the
    view matrix, so an infinite far plane and reversed Z never divide by zero.
  - A point at or behind the eye plane is `behind`, with an infinite screen distance.
- The region has 4 side planes, plus the near plane. It has no far plane, so an infinite far
  needs no special case.
- Depth for ranking is the distance along the view direction, never the NDC depth.
- P4 adds cone helpers, the DrawList and the event types on top.

## Transforms (`scene/transform.h`)

### Types
```cpp
struct TransformState {      // bound, copyable, 184 bytes
  mat4d world;  quatd rotation;  double3 localScale;
};
class EvalCtx {              // bound; only what a pure evaluation may read
 public:
  double channel(int i) const;       // the override if one is set, else the object's value
  int channelCount() const;
  double time() const;               // seconds
};
class ChannelBlock { int size(); double get(int); void set(int, double); };   // bound

struct TransformModel : IfaceRef {
  TransformState evaluate(const EvalCtx &ctx, const TransformState &parent) const;
  Status setWorld(const EvalCtx &, const mat4d &target, const TransformState &parent,
                  SetWorldMode mode, ChannelBlock &out) const;
  Status applyDelta(const EvalCtx &, const TransformDelta &, const TransformState &parent,
                    ChannelBlock &out) const;                    // optional
};
```
- **EvalCtx has no Library, Runtime or Context.** A model can't read another block, so
  decision 1's purity is checked by the type. P5c fills `overrides` from its sockets:
  `EvalCtx(values, overrideMask, overrideValues, time)`. P3 builds it with an empty mask,
  and tests set one.
- **`TransformType`** (registered with `TransformRegistry`, a Runtime extension): name,
  StructId, channel defs (`PropertyDef`s of kind Bool, Float, Int, Vec2-4, Enum or Quat,
  flattened to doubles in order), default values, and one `Owned<TransformModel>`.
  - Registration builds and registers the type's ApiStruct, with the parent struct
    `TransformChannels`. Its members read and write the owning object's channel array
    through `Transformable`, then call its update hook.
  - Int and Enum channels outside ±2⁵³ are rejected by coerce ranges.
- **`setWorld` and `applyDelta` are pure too:** they write `out`, not the object. A caller
  applies `out` as one undoable command.
  - `SetWorldMode::Strict` returns FailedPrecondition when the target isn't representable.
    `Closest` drops what can't be represented (shear), keeping translation exact.
  - **The caller verifies:** `setWorldOf(...)` re-evaluates the model with `out` and compares
    it with the target, relative to the target's size. A Strict result off by more than
    1e-9 is an Internal error, so a wrong foreign model can't corrupt an object silently.
- **`applyDelta`** takes `TransformDelta {kind, space, pivot, vector, axis, angle}`. Without
  the slot it goes through `setWorld`. Trs and Fbx rotate Euler channels directly and pick
  the solution nearest to the current one, so an accumulated rotation doesn't wrap.

### Built-in models
- **`core.transform.trs`:** location, rotationMode (6 orders, QUATERNION), rotationEuler,
  rotationQuaternion, scale. `world = parent.world · T·R·S`; `rotation = parent.rotation · R`.
  - Setting `rotationMode` doesn't convert the other channel. `core.object.setRotationMode`
    does, as one undoable command.
  - A zero or non-finite quaternion evaluates as the identity.
- **`core.transform.fbx`:** lclTranslation, lclRotation, rotationOrder, pre- and
  postRotation, rotation and scaling offsets and pivots, lclScaling, inheritType. The local
  matrix is `T·Roff·Rp·Rpre·R·Rpost⁻¹·Rp⁻¹·Soff·Sp·S·Sp⁻¹`. The world matrix follows the
  table above, with `LR = Rpre·R·Rpost⁻¹` and the translation column of the local matrix.
- **`core.transform.matrix`:** 16 channels and nothing else. It accepts any affine target
  (shear included), and keeps the interface honest.

### Failure behaviour
- **Singular matrices** (a scale channel of 0): `evaluate` is fine. Rrs divides by `pls`; a
  zero component uses 1 instead, which is documented. `setWorld` under a singular parent
  is FailedPrecondition in both modes.
- **Non-finite results** (NaN channels, overflow): the evaluator stores the parent's state
  instead, marks the object `EvalStatus::Invalid` and counts it. Children follow the
  fallback. Its bounds are skipped by SceneBvh. Nothing aborts.
- **Huge coordinates:** tested at 1e12 (double keeps about 1e-4) and at 1e200, which
  overflows in products and must end as Invalid.
- **A foreign model that throws:** its sticky error is returned by `evaluate()`, after every
  object is processed.

## Objects and Scene (`scene/`)

### Capabilities (interfaces; names from `geometry-contract.md`)
- **`Transformable`:** `transformType() -> TypeId`, `channelCount()`, `readChannels(ChannelBlock
  &out)`, `writeChannels(const ChannelBlock &) -> Status`, `parentId() -> BlockId`, `dataId()
  -> BlockId`, `flags() -> uint32`. Optional `setParentId(BlockId) -> Status`.
- **`Spatial`:** `localBounds() -> Box3d`, `domains() -> uint32`, `closestElements(co, radius,
  domain) -> Vector<int32_t>` (object space), optional `pick(const PickQuery &, PickSink &)`.
  - `PickQuery`: planes, object-space ray and whether it exists, the object's world matrix,
    the ViewMath, domain, mode. `PickSink` is a bound class; a script calls `add(...)`.
  - Without `pick`, the object answers with its bounds, as faberforge's base class does.
- **`Invalidation`:** `invalidate(uint32 kinds)`; Topology, Positions, Attributes, Selection,
  Materials.
- **`Elements`:** `topoStamp() -> uint64`, `elementCount(domain)`, `positions(handles, domain)
  -> Vector<double>`.
- **`OverlayDrawable`** is declared in P4 with the DrawList. Declaring a slot that takes a
  type that doesn't exist would have to change anyway.
- An object's geometry capabilities come from its **data block** first, then from the object.
  `objectQuery<I>(lib, handle)` does that lookup.

### `coredcc.Object`
- Data: flags (Hidden, Unselectable), `WeakRef` parent, `OptRef` data, the transform type
  (stored by name) and its channel array. The name is the block's.
- Its provider enumerates both refs. A missing transform type keeps its channels (as raw
  doubles) and evaluates as the identity, flagged `MissingModel`.
- ApiStruct members: `name` (read-only here; `Library::rename` is P2), `flags`, `parent`,
  `data`, `transform` (a fixed nested pointer whose struct is the transform type's), and
  read-only `worldMatrix` and `evalStatus`.

### `coredcc.Scene`
- Data: `OptStrong` cells in order, the active object and the selection as Weak cells, unit
  settings, the playback fps, and evaluated state (not saved, not undoable).
- `add`, `remove`, `contains`, `setParent` (checks), `objects()` (skips cells that resolve to
  null: deleted and Detached objects stay members, so undo and host restore bring them back).
- **Cycle checks:** `setParent` walks up from the new parent. A cycle that appears anyway
  (undo of two reparents, a host, a file) is found in `evaluate()`: every object on it gets
  the identity parent and `EvalStatus::Cycle`. A parent outside the scene still counts and is
  evaluated; it isn't drawn or picked.
- **`evaluate()`**: `beginNoCollect` for its duration; topological walk from the roots (an
  explicit stack, so a 100k-deep chain doesn't overflow); one `TransformState` and one world
  box (8 transformed corners) per object, keyed by BlockHandle; then SceneBvh.
  - Reads: `evaluated(handle) -> const ObjectEval *`, null before the first `evaluate()`.
  - Reading `worldMatrix` by path does not evaluate.
- **Seam for P5c:** `SceneEvaluator` is an internal virtual class with one method, `run(Scene
  &, EvaluatedStore &)`. P3's `HierarchyEvaluator` is one; P5c's graph evaluator is another.
- **The Context scene slot** is `Context::setScene(BlockHandle)` / `scene()`. `api/` can't name
  Scene, so it holds a handle. `activeScene(ctx)` in `scene/` resolves it.

### `CollectionAccessor::setActive`
- New optional callback `setActive(self, ctx, item) -> Status`; a null item clears it.
- A path ending in `.active` can then be read as a Pointer value and set with one.
  `ResolvedPath` gains `activeOf` (the collection's member and owner). Its struct behaviour
  and canonical address are unchanged: `objects.active.x` still canonicalizes to the item.
- `setValueUndo` on it records the collection's canonical key plus the pseudo-member
  `active`, so undo restores the old active item wherever the list has moved.
- Setting it bumps the ChangeBus's structural epoch.
- Scene's `objects` collection: keys are the block id as text (`"<client>:<local>"`, hex),
  which `lookup` tries first, then the first object of that name in scene order.

### Commands (`scene/commands.cc`; sub-commands where one exists)
- `core.object.add(type, name, location, parent)`, with CreatedBlocks.
- `core.object.setParent(objects, parent, keepWorld)`: verified `setWorldOf`, Closest.
- `core.object.setTransformType(object, type, keepWorld)`.
- `core.object.setRotationMode`, `core.object.applyDelta(objects, delta)` (skips descendants
  of selected objects).
- `core.object.delete(objects, keepChildrenWorld)`: reparents Live children to the deleted
  object's parent, then runs `core.block.delete`. Plain `core.block.delete` lets children
  jump to the root. Undo restores both.
- `core.scene.select(objects, mode)`, `core.scene.setActive`.

### SceneBvh
- A `Bvh<BoxSource>` over world boxes. `update()` refits when the object set is unchanged and
  the cost is below 2× the last build's, else rebuilds. Objects with empty or Invalid bounds
  are left out and can't be picked.
- **`pick(view, px, radius, filter) -> Vector<PickHit>`:**
  1. Broad phase in world space with the region's planes.
  2. Per object: planes to object space; `Spatial::pick`, or its bounds.
  3. Exact scoring in world and screen space: position, view distance, screen distance in
     pixels. Triangles and segments are projected; a primitive crossing the eye plane is
     clipped to it first.
  4. Sorted by `(screenDist, depth)`; a ray hit has screenDist 0. Ties keep scene order.
- **The object-space ray** exists only when the object's matrix inverts. For a singular
  (flattened) object the region's candidates are transformed to world and scored there.
- Radii under non-uniform scale: a ray hit is exact (`t` is affine invariant). For a miss, the
  screen distance to a capsule or sphere is the axis's distance less the radius times the
  largest singular value, which is an upper bound on the true radius. Documented.
- `nearest(co, radius)` in world space: planes of the sphere's box, then exact distances.

## Units and frames
- **`UnitSettings`** (`props/unit_settings.h`): a display unit symbol per family (length,
  angle, mass, time), fps as a rational, and the start frame. Scene holds one, exposed at
  `units`. `Context::units()` returns the active scene's through a callback the scene module
  installs, or the defaults.
- **Frames:** `frame` is a Time unit whose scale is `1/fps`. `units::format` and `parse` get
  overloads taking `const UnitSettings &`; the old ones are unchanged and don't know frames.
  Frame numbers count from the start frame; `12f` means 12 frames of duration.
- **`formatValue(ctx, def, value)` and `parseValue(ctx, def, text)`** in `api/`, for P2's
  widgets.
- **The agreement check** in `PropertyDef::validate()`: Angle and Euler need Angle or None;
  Distance and Xyz need Length or None; Time needs Time or None; Factor and Percentage need
  Factor or None.

## Bindings (`bind/`)
- **Types in the manager:** `Mat4d`, `Quatd`, `double3`, `Box3d`, `Ray3d`, `TransformState`,
  `EvalCtx`, `ChannelBlock`, `TransformDelta`, `PickQuery`, `PickSink`, `View`, and the
  interfaces above. Value types have `at` and `put` methods, so a script needs no array
  members.
- **`registerForeignTransformType(ctx, type, handle, slots)`**, with builder facades
  `newTransformType(name)` and `transformChannel(type, name, defaultValue)`. One JS object
  serves every object of the type. ForeignBindings keeps its handle, as it keeps factories.
- **`implementOn(ctx, blockId, ifaceName, implName, handle, slots)`**: a script capability on
  one block, through `registerForeign` (once per implName) and `Library::setOverride`. Its
  handle is released by an attachment's `destroy` when the block goes, or when it is cleared.
  It refuses interfaces that `foreignImplementable` refuses.
- **Facades:** `evaluateScene`, `worldMatrix(ctx, id)`, `pick(ctx, view, x, y, radius)` into a
  result struct, `setScene`.
- **threadAffine:** every P3 loop that calls interfaces (evaluate, gather, pick) runs on the
  calling thread. Only native, already gathered arrays are processed in parallel. Tests
  count foreign calls per thread id.
- Bound methods compile slowly (40–80 ms each, §2a), so they stay in `*/bindings.cc`.

## Files (each under 35 KB)
```
bvh/    box.h  ray.h  kernels.{h,cc}  bvh.h  build.h  sources.h  geometry_bvh.h
        any_source.{h,cc}  bindings.cc
draw/   view.{h,cc}  bindings.cc
scene/  eval_ctx.h  transform.{h,cc}  transform_registry.{h,cc}  trs.cc  fbx.cc  matrix.cc
        object.{h,cc}  capabilities.h  scene.{h,cc}  evaluator.{h,cc}  scene_bvh.{h,cc}
        pick.{h,cc}  api.cc  commands.{h,cc}  bindings.cc  builtins.{h,cc}
api/    setActive in struct.h, resolve.cc and context.h; format.{h,cc}
props/  unit_settings.h, units.cc
bind/   foreign_scene.{h,cc}
tests/  bvh/ draw/ scene/ bench/
```
Layering: `base` ← `bvh`; `base` ← `draw`; `command`, `bvh`, `draw` ← `scene` ← `bind`.

## Tests
- **Oracles.** Every BVH query is compared with a brute-force loop over the same kernels, on
  random, clustered, coplanar, all-equal, long-thin and degenerate sets, at leaf sizes 1 to
  1024, float and double. Kernels are compared with sampling or a long-double reference.
- **Determinism:** node bytes identical for `ScopedBandCount` 1, 2, 5 and 12 (tests only), and
  for serial and parallel builds.
- **Refit** equals a rebuild's query answers after random motion. `refit` after the skipped
  set changes is FailedPrecondition.
- **Robustness:** NaN and infinite vertices, negative radii, zero-length segments, 0 and 1
  primitives, rays along faces and through shared edges (a closed mesh is never leaked
  through, 1e6 rays), coordinates of 1e-6 and 1e30 in float.
- **Foreign source:** fake host, thread ids recorded, a throw mid-gather, a wrong length.
- **View:** all 2×2×2×2×2 combinations of depth range, reversed Z, infinite far, handedness
  and ortho: project against unproject, ray against project, region contains exactly the
  points within the radius square; points behind the eye.
- **Transforms:** the `fbx.mjs` identities, ported; each type under each other type; pivots
  against an explicit matrix product; `setWorld` round trips for all models (positive,
  negative and sheared parents) and Strict refusals; `applyDelta` over 720 degrees in 1
  degree steps without a jump; Euler gimbal lock; overrides through EvalCtx change only the
  overridden channel.
- **Scene:** cycles; a deleted and restored parent; a Detached object; a scene surviving an
  object's deletion; membership through undo, redo and discard; a collect during `evaluate`;
  a 100k-deep chain; an Invalid object in the middle of a chain; a host-owned Transformable
  type; a foreign model and a foreign override; quarantine mode on.
- **setActive:** set, undo after reordering, clear, watchers fire, `.active` in the middle of
  a path still resolves as before.
- **Picking:** non-uniform, reflected, sheared and singular objects against a brute-force
  screen-space loop over every triangle.
- **Units:** frames at 24, 30000/1001 and 25 fps; every subtype/unit pair.
- **compile_embed** includes every new public header.
- **wasm-st:** the whole native suite under node, with `COREDCC_TEST_SCALE` for the sizes; the
  TS spike gains a foreign TransformModel and a foreign Spatial override (`tests/wasm`).
  **wasm-mt** is built and its BVH tests run in node.

## Order of work
Each step ends with the full gate (4 native presets and wasm-st-debug) and a commit.

1. **`bvh/` core:** box, kernels, serial build, sources, queries, oracles.
2. **Parallel build, refit, GeometryBvh, AnyBvhSource,** and their bindings.
3. **The benchmark and its command.** Then the coordinator can merge steps 1–3 to master, and
   run the benchmark on an idle machine.
4. **`draw/View`.**
5. *(needs PR D)* **EvalCtx, the registry, Trs, Fbx, Matrix, setWorld.**
6. **Capabilities, Object, Scene, evaluate, the API structs, setActive, the Context slot.**
7. **Commands.** *Feature review: objects and transforms.*
8. **SceneBvh and picking.** *Feature review: BVH.*
9. **Scripts:** foreign models and overrides, facades, the TS spike's additions.
10. **Units and frames.**
11. *(needs P2's first step)* **The Object and Scene providers' serialize, deserialize,
    duplicate and rekey.** Channels are written by name with the type's name, so a missing
    or changed model keeps what it doesn't know.

PR D (step 0) starts alongside step 1.

## Exit criteria
- The gate passes; wasm-mt builds and its BVH and scene tests pass in node.
- The leaf-size benchmark has run on an idle machine, `docs/bvh-leafsize.md` is committed,
  and the default follows from it.
- The three FBX inherit types pass their identities, and a round trip through `setWorld`.
- A TransformModel and a Spatial written in TS work in the spike.
- Both feature reviews are resolved.
- `docs/embedding.md` says where coredcc uses the task pool.

## Claims to falsify
1. PR D doesn't break sculptcore, faberforge or ubertopo: nothing outside litestl uses
   `Matrix`'s Eigen conversions or relies on `matrix.h` including Eigen.
2. `parallel_for_chunks` and `parallel_reduce` suffice for the build, and it never nests.
3. The build is bit-identical for any worker count.
4. Depth is bounded by 79 for any input, so the 96-entry stack can't overflow.
5. Traversing float nodes in double with outward rounding never misses a hit brute force
   finds.
6. The padded slab test never leaks through shared edges of a closed mesh.
7. A relative determinant test handles edges from 1e-30 to 1e15 in double.
8. Transforming planes by `Mᵀ` is conservative for every affine matrix, including singular
   and reflected ones.
9. The screen square's 4 planes plus near contain every point within the pixel radius, in
   every convention and with an infinite far plane.
10. `TransformState` is enough for all three FBX types; nothing needs the parent's interface.
11. Carrying `GR` gives the same result as the SDK sample whenever the parent has no shear
    and no reflection.
12. The column-length inverse finds every representable target. (With reflections there are
    several sign choices; it tries one.)
13. EvalCtx is enough for P5c: no model needs more than channels, parent state and time.
14. A flat array of doubles can hold every channel kind we want, exactly.
15. A transform type registered after Object's struct works by path, by canonical StableRef
    and in undo, through the `TransformChannels` parent.
16. Recomputing everything in `evaluate()` takes under 10 ms for 100k native objects.
17. OptStrong membership never cascades, and an object stays a member through delete, undo,
    redo, discard, Detached and restore.
18. `evaluate()` can't be re-entered or made stale by a foreign slot that calls facades.
19. Adding `activeOf` changes no existing path behaviour.
20. A foreign override's handle is released exactly once on clear, on block destruction and
    at Runtime teardown.
21. The agreement check rejects no def that exists today.
22. Bound `at` and `put` value types are enough for a TS TransformModel, and a const argument
    can't be written from JS.
23. No slot is ever called off the calling thread.
24. `foreignImplementable` accepts every P3 interface except those the plan doesn't offer to
    scripts.
25. Nothing here needs P5's version counter.

## Risks
- **PR D waits for the user.** litestl #11 has been open since 2026-11-23. Steps 5 to 9 can
  be written on the branch, but not merged.
- **Size.** Eleven steps. If it slips, steps 9 and 10 can follow the merge, because P4 and
  P5c need only 4 to 8.
- **`api/resolve.cc` is changed by setActive,** which P2 may also touch. The change is small
  and comes in one commit.
- **The FBX rules are checked against a published sample's formulas, not the SDK.**
- **Stateless models** can't keep caches. A model that needs one (a spline rig) has to be a
  graph node writing channel overrides.
- **Benchmark noise** from parallel builds. Only the coordinator runs it.
- **Float geometry far from its own origin** loses precision in the source data itself. BVH
  can't help; DrawList's double origin (P4) is the documented answer.

## Deviations from the architecture
To be recorded there once the plan survives its pressure test:
- decisions 1, 2, 5, 6 and 8;
- View moves from P4 to P3;
- `OverlayDrawable` is declared in P4;
- `structId()` is the transform type's, not a model slot;
- the "gizmos skip descendants" rule is applied in `core.object.applyDelta`.

## Pressure test record
Not run yet.
