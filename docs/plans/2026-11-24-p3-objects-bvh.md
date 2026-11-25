# P3: objects, transforms and BVH

- Status: pressure-tested twice (2026-11-24) and changed; the records are at the end. Steps 0 to 3
  are done.
- Branch: `p3-scene`, in the worktree `C:\dev\coredcc-p3-scene`.
- Architecture: §8 (objects and transforms), §9 (BVH), §10 (View), §2a (interfaces), §14.
- Feature reviews: **objects and transforms**, and **BVH**.

## Ownership between the parallel phases

| Phase | Owns |
|---|---|
| P2 | Value bytes; `BlockProvider`'s optional slots; **library lifetime** (pin kinds, deferred destruction of pinned blocks, `LibraryRef`, `LibraryPin`); attachments; `Library::rename`; snapshot undo and lz4; CreatedBlocks with BlockIds; `layout/`; TA2; `gen-paths`. Value bytes, slots, lifetime and the attachment registry are step A1, merged early. |
| P3 | `scene/`, `bvh/`, `draw/View`, `EvalCtx`, `CollectionAccessor::activeMember`, the Context scene slot, display units and frames. Its providers' `serialize` is its last large step. |
| P5 | `graph/`, the Runtime's global version counter, payloads, and **immediate change notification**: ChangeBus listeners, notify before the `update` hook, `notifyAll()`, EvaluationScope and its Busy rule (its step 1, merged early). GraphDef serialization is its last step. |
| Shared files | `source/coredcc/CMakeLists.txt`, `tests/CMakeLists.txt`, the task list, `docs/debugging.md`, `docs/embedding.md`: small appended changes only. Task list: each phase edits its own row and focus lines. No phase edits `CLAUDE.md`. |

- **`api/struct.h`** gets one optional field from P3 (`activeMember`) and two from P2.
- **`api/resolve.cc`'s `resolve()` isn't changed by P3** (see "Setting the active item").
- **`props/units.*`** are P3's. `props/def.cc` gets one check in `validate()`.

## Scope

### In
- **From architecture §14:** TransformModel, TrsTransform, FbxTransform, Object, its
  capabilities, Scene; BVH, its built-in sources, SceneBvh with two-phase queries; the
  leaf-size benchmark.
- **Moved here from P4:** `draw/View` only. Picking reports screen distances, so it needs one.
- **Deferred to P3 by earlier phases:** setting a collection's active item (lib and api review,
  D2); the Context's scene slot (P1d; its ToolManager slot goes to P4); display units, the
  subtype/unit agreement check and frames (props review, D12).
- **The litestl math PR ("PR D"),** planned for before P3 and never written.

### Moved out
- **Scripting interactive commands** and `runSub` of an Interactive def: the command review
  deferred them "with the tools", and the tools are P4.
- **Script capability overrides (`implementOn`)** go to P6's faberforge spike, which is what
  needs them. They will use P2's `AttachmentProvider` (`lib/attachment.h`), not a destroy
  function. P3's script surface is foreign transform types.
- **`AnyBvhSource::rayTest`, and a world-space `nearest`:** deferred until a client needs them.

### Out
- The scene evaluator as a graph client (P5c). DrawList, `OverlayDrawable`, gizmos,
  TransformableElements, Symmetry (P4). Attributes, Triangles, ActiveElement (§14).
- Incremental BVH insertion and removal. FBX's `SphericXYZ` order. IK and constraints.
- Fixing litestl's `Matrix * Vec`.

## What the code says
Scratch: the author's `p3\` and the test's `pt-p3\` folders in the session scratchpad.

- **litestl's math group never landed.** `math/quat.h` doesn't compile. There are no Euler
  conversions, no decompose and no bound matrix.
- **`Matrix * Vec` uses the transpose** of `Matrix * Matrix`'s convention. sculptcore depends
  on that: `brush_command.h:456` computes `renderMatrix * float4`, and `texture_eval.h` copies
  its indexing.
- **A singular `Matrix::inverse()` is NaN.** The default AABB isn't empty. `rayTriIsect` has an
  absolute epsilon and misses triangles with 1e-4 edges.
- **`matrix.h` includes Eigen.** It compiles next to Unreal-style macros and `<windows.h>`
  (tested).
- **Interfaces carry P3's shapes** (a prototype, native and through the fake host): structs by
  const reference and by value, interface values, `Vector<int32_t>` returns.
- **Foreign dispatch:** a `Vector&` argument reaches JS as a copy. A returned struct needs a
  bound copy constructor; `TypeId` and `BlockId` have none, and `foreignImplementable` doesn't
  check. A failed call returns `R()`, which for a matrix is all zeros, and sets the sticky
  error, which keeps only the first error.
- **`task::parallel_for`** gives each worker one contiguous band, without rebalancing.
  `parallel_for_chunks` and `parallel_reduce` have chunk boundaries independent of the worker
  count. wasm-st is serial. wasm-mt works under node.
- **`deletePreview` follows Strong refs**, so Strong membership would delete the Scene.
  `onWeakTargetLost` runs at the sweep, too late to keep a child's world transform.
- **`destroyBlocks` clears refs out of the garbage, not refs into it** (`lib/gc.cc:198`). A
  cell in a survivor keeps an id that has no record.
- **Nothing makes a Scene a GC root.**
- **`objects.active` with no active item is NotFound**, and SetProperty stores the *item's*
  canonical address.
- **`PropValue` has no list type,** and a Pointer has no literal until P2's B1.
- **A capability lookup costs.** A `Library::query` is a provider thunk plus a hash lookup.

### Measured (clang-cl /O2, 100,000 objects with one data block each, the real Library)
| | ms |
|---|---|
| The first draft's `evaluate` (per-object queries, 6 slots) | 39–43 |
| Of which the model calls | 2 |
| Cached members, one bulk slot, parent and data ids re-read and compared | 8.5–10 |
| Calling `Library::data` again for an object and its data block | + 5 |
| Rebuilding the cache after an epoch change | 25–28 |

SceneBvh isn't in these numbers.

### FBX inherit types, checked numerically
`GR` is the parent's carried global rotation; `GSM = GR⁻¹ · linear(parentWorld)`; `LR`, `LS` are
the node's rotation and scale; `pls` is the parent's local scale. The formulas are from
Autodesk's "Transformations" sample; the FBX SDK isn't installed.

| Type | Linear part of the world matrix |
|---|---|
| RrSs | `GR · LR · GSM · LS` |
| RSrs | `GR · GSM · LR · LS` (= `parentWorld · local`) |
| Rrs | `GR · LR · GSM · S(pls)⁻¹ · LS` |

- The translation is `parentWorld · localTranslation` for all three.
- Two independent implementations agree: RSrs is the TRS product, an all-RrSs chain is
  `R(all)·S(product)`, an all-Rrs chain is Maya's `P·T·S(pls)⁻¹·R·S`, with signed scales.
- **Those identities are weak tests.** `Rrs = GR·LR·LS` and `RrSs = GR·LR·LS·GSM` pass them too.
- **Outside all-Rrs chains, Rrs differs from Maya's segment scale compensation** (by 6 for an
  Rrs joint under a unit-scale RSrs joint under a non-uniform group). Documented, not changed.
- **`GR` is carried, not decomposed.** They agree to 6e-14 without shear and reflection, and
  differ by 16 with them. Carrying gives `R(all)·S(product)` for reflected chains.

### Inverting them, checked numerically
Solve `X = LR · A · LS` for `LR` and `LS` with `A` known. (`A` is the identity for RSrs and TRS,
after `X = (GR·GSM)⁻¹ · target`.)
- **Scale:** `|LS_i| = |X_i| / |A_i|` by columns. Try all 8 sign patterns.
- **Rotation:** from orthonormal frames of the two most independent columns of `A`, and the
  same columns of `X · LS⁻¹`. No inverse of `A` is needed.
- **Residual:** the largest relative error of a column of `LR·A` against `X·LS⁻¹`.
- **Choice:** among the patterns whose residual is within 1e-9 of the smallest, the fewest sign
  changes from the current channels, then the rotation nearest to the current one. The smallest
  residual has to come first: a sheared target has none below 1e-9, and sign changes alone then
  lose a mirror (2,000 of 2,000). With this rule none is lost for shears from 0 to 1, under
  sheared parents too (`p3\closest.mjs`).
- **Results** (all in `p3\e1.mjs`):
  - A node's own world, with random signs, depths 1 to 4 and mixed types: 0 of 40,000 refused,
    and its own channels come back to 1e-14. The first draft's method refused 29% to 76%.
  - Scales from 1/1000 to 1000 at every level: 0 of 8,000 refused, residual 1e-11. At 1e4,
    1 refused.
  - Arbitrary targets under a sheared parent: 0 of 8,000 accepted.
- **Zero columns** (under 1e-12 of the largest): the scale is 0. The rotation is completed from
  the other columns, and from the current rotation when fewer than two are left. 0 of 20,000
  fail, and a node's own flattened world keeps its rotation to 6e-16.
- **The residual equals the target's shear,** continuously. 1e-9 is a chosen threshold, not a gap
  in the data.
- **The limit:** for TRS and RSrs, `A` is the identity and `P⁻¹ · target` is projected onto `LR·LS`.
  With non-uniform scales at several levels that loses digits: at 0.01 to 100, 0, 20 and 457 of
  8,000 random targets are refused under 1, 2 and 3 parents; at 1/1000 to 1000, 6, 1,160 and
  4,051. (The 0 of 8,000 above is for RrSs and Rrs, whose `A` is known.) A child of one parent
  scaled by 1e-7 along an axis: 463 of 3,000. An object's own world is found another way, below.
- **Translation:** with two steps of iterative refinement, the residual is below
  1.0 · ε·(|P|·|t|) for parent conditions from 1e2 to 1e8. Without refinement it is 8,600 times
  that unit at 1e6.

## Key decisions
1. **A TransformModel is stateless.** Channels are a flat array of doubles on the object, and
   reach the model through EvalCtx. P5c's overrides are a second array that EvalCtx reads
   first. Serialization, duplication, undo and animation are generic over channels.
2. **Parents are passed as data** (`TransformState {world, rotation, localScale}`), not as
   interfaces. It is a socket value in P5c, and scripts can't test a null interface.
3. **`GR` is carried down the chain.**
4. **Objects are found through a capability** (`Transformable`), not a class.
5. **`Scene::evaluate()` recomputes every object's transform.** Only lookups are cached, and the
   cached lookups are checked each time. P5c replaces what is behind `evaluate()`.
6. **Picking's broad phase uses planes,** taken to object space by `Mᵀ`, without an inverse.
7. **BVH traversal computes in double;** nodes store the source's scalar, rounded outward.
8. **Scene membership uses OptStrong refs,** and a Scene is a Root block.
9. **Mutations never read the last evaluation.** They compute the parent's state themselves.
10. **litestl gets the math,** and P3's steps that don't need it come first.

## Step 0: litestl PR D (`coredcc/math`)
Written in this worktree's `extern/litestl`, following its CLAUDE.md (no standard containers)
and the pr-workflow skill: fresh base, the open PRs checked first (none on 2026-11-24).

- **`math/quat.h`**, rewritten (it never compiled, so it has no users): xyzw, product,
  conjugate, normalize (zero and non-finite give the identity), rotate, to and from a rotation
  matrix, axis-angle, slerp.
- **`math/euler.h`** (new): 6 orders, `eulerToQuat`, `quatToEuler(q, order, near)`.
- **`math/transform.h`** (new): `transformPoint`, `transformVector`, `transformPlane`,
  `composeTrs`, `decomposeTrs` (reports shear), `tryInverse(m, out) -> bool`, `perspective`,
  `orthographic`, `lookAt`.
- **`math/math_bindings.h`** (new): `Binder` for `Matrix` and `Quat` with default and copy
  constructors, `at(i)` and `put(i, v)`. No other repo defines one for them.
- Tests, and one CMake entry.
- **`matrix.h`, `aabb.h` and `vector.h` are not touched,** so nothing changes for sculptcore
  (`StrokeView::inverted`, `renderMatrix * float4`), faberforge or UberTopo. Taking Eigen out of
  `matrix.h`, a const `operator[]` and the dangling `operator*=` are reported to the user as
  possible later PRs.
- **P3 never uses `Matrix * Vec`, `operator*=` or `inverse()`.** A grep test added in step 5
  checks `scene/`, `draw/` and `bvh/`.

Steps that need it are built against the PR commit with the gitlink uncommitted, and merge to
master after the bump.

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
  Status build(const S &src, BvhOptions opts = {});
  Status refit(const S &src);          // same size and the same skipped set as the build
  double cost() const;  int size() const;  int skipped() const;
  Box3<Scalar> bounds() const;
  const Vector<int> &order() const;    // leaf position -> primitive
  template <class NodeTest, class Leaf> void traverse(NodeTest &&, Leaf &&) const;
};
```
- **`Box3<Scalar>`** is coredcc's: empty by default; an empty box stays empty under any
  transform; transforming rounds outward (each sum padded by its own error bound). `Box3d` is
  bound with default and copy constructors.
- **Finite checks test exponent bits,** so a host's fast-math can't remove them.
- **Build:** binned SAH over centroids computed as `0.5*min + 0.5*max` (no overflow). Areas are
  in double, over extents scaled by the node's largest one, so they don't overflow.
  - `leafSize >= 1` and `2 <= bins <= 64` are checked (InvalidArgument).
  - **A median split is used for:** no finite best split, equal centroids, an extent whose
    reciprocal isn't finite, or a depth over 48. Bin indices are clamped before the int cast.
    The depth is at most 48 + 31, so a fixed 96-entry stack is enough.
  - **Parallel:** the top is split serially (bins by `parallel_reduce`) until a subtree has 4096
    primitives or fewer, or there are at least 64 subtrees. Subtrees are handed out largest
    first, interleaved over the bands, by `parallel_for_chunks`; each builds its own node list,
    and the lists are appended in tree order. Bit-identical for any worker count.
  - Serial inside a pool worker, and on a browser's main thread
    (`emscripten_is_main_browser_thread`), where blocking could deadlock. coredcc never calls
    `task::run`.
- **Refit** goes through the node array backwards.
- **Failure:** over 2³¹−1 primitives is OutOfRange. Primitives with non-finite bounds are left
  out, counted by `skipped()`, and never hit. Refit with another skipped set is
  FailedPrecondition.

### Sources (leaf-ordered structure of arrays)
- `TriangleSource<Scalar>`, `CapsuleSource<Scalar>`, `SphereSource<Scalar>`, `BoxSource` (double,
  rounded outward).
- A zero-area triangle can't be hit by a ray, and is a segment or point to nearest queries. A
  zero-length capsule is a sphere. A negative or NaN radius skips the primitive. Radii are in
  object units.
- `GeometryBvh<Source>` owns a leaf-ordered copy, the BVH and the id map. `onInvalidate(kinds)`
  refits on Positions, and rebuilds on Topology or when the refit is refused.

### Queries (object space, double)
- **Ray** (closest, any, all), **nearest within a radius,** **sphere overlap,** **region** (up to
  8 planes; candidates, not exact overlaps).
- A ray with a non-finite component or a zero direction hits nothing.
- **Kernels** (`bvh/kernels.{h,cc}`):
  - **Ray-triangle is watertight** (Woop, Benthin and Wald): shear to ray space, edge functions
    with closed signs, so a ray through a shared edge hits at least one side. Measured in double:
    0 leaks of 100,000 each for random rays and rays aimed at edges, at vertices, and at edges
    along an axis. Möller-Trumbore leaks 8.8% of those aimed at edges. Interior hits are found at
    scales from 1e-30 to 1e15.
  - It needs `a*b - c*d` to change sign exactly when its operands swap. Contracted multiply-add
    can break that, so `kernels.cc` is compiled with `-ffp-contract=off` or `/fp:precise` (a
    source-file property, not a global flag) and the function isn't inline.
  - A hit on a shared edge may be found twice; "closest" keeps the lower primitive id.
  - Ray-capsule and ray-sphere (a ray starting inside hits at `tMin`, flagged); point-triangle,
    point-segment and point-box distances.
- **The slab test pads `tMax` by 4 ulp.** Without it, 3.3% of rays aimed at vertices are lost;
  with it, none of 16 million grazing rays.

### Foreign sources
```cpp
struct AnyBvhSource : IfaceRef {
  int size() const;
  Vector<double> bounds(int first, int count) const;      // 6 per primitive
};
```
- `AnyBvh::build` gathers all bounds first on the calling thread, in chunks of 4096, then builds.
  ThreadAffine native sources are treated the same. No slot is called from a worker.
- A wrong length, or a sticky error after a chunk, fails the build and keeps the previous tree.

### Leaf-size benchmark
- `node tools/bench-bvh.mjs [--quick] [--wasm]` builds the release target (outside `all` and
  ctest), runs it and writes `docs/bvh-leafsize.md`. The coordinator runs it on an idle machine.
- **What decides the leaf size:** triangles at 10k and 1M, capsules and spheres at 1M, all nine leaf
  sizes (4 to 1024), coherent and random closest-ray, and region queries. Build and refit time
  and node memory are reported. Fixed seeds, best of 5.
- **The rule:** the largest leaf size whose worst slowdown is at most 10%.
  - A slowdown is a size's query time over the best size's, for one query kind on one data set. The
    worst is taken over all of them. Nothing is averaged, so no data set or kind can hide
    another.
  - If no size is within 10% everywhere, it is the one with the smallest worst slowdown, and the
    larger on a tie.
- **Confirmation only:** triangles at 10M with the winner and its two neighbours. A worst slowdown
  over 10% there is reported as "not confirmed".
- `--quick` is 10k triangles and spheres, as a smoke test; `--wasm` runs in node without 10M.

## View (`draw/view.h`)
```cpp
struct View {
  double viewport[4];            // x, y, width, height: device pixels, y down
  mat4d view, proj;
  DepthRange depth;              // ZeroToOne, MinusOneToOne
  bool reversedZ, infiniteFar, ortho;
  Handedness handedness; UpAxis up;
  double dpiScale;               // device pixels per logical pixel
  Status validate() const;
};
```
- **`validate()`** checks that everything is finite and invertible, and **checks the flags
  against the matrices:** `ortho` against the projection's w row; `reversedZ` and `depth` against
  where two probe points land; handedness against the sign of the view's determinant and the
  projection's w row.
- **`ViewMath`** (from a valid View; bound): `project`, `unproject`, `rayFromPixel`,
  `regionFromPixel(px, radius)`, `pixelSizeAt(p)`, `viewDistance(p)`.
  - Pixel radii are **logical**; they are multiplied by `dpiScale`.
  - **The near plane comes from the projection:** `z >= 0`, `w + z >= 0`, or `w - z >= 0` when
    reversed, by depth range.
  - **Rays start on the near plane.** In orthographic views it may be behind the eye, and
    depths measured from the eye are then negative.
  - A point outside the near plane isn't visible. `behind` means `w <= 0` and exists only in
    perspective.
  - `unproject` at an infinite far plane's depth is FailedPrecondition.
- **The region** is 4 side planes plus the near plane, of a square at least 0.5 px in radius.
  Each plane is moved out by 1e-12 of the magnitude of its terms. It is conservative; scoring
  decides. In double this holds to scene offsets of about 1e9; at 1e12 double itself is wrong by
  up to 20 px.
- **Depth for ranking** is the distance along the view direction from the near plane.

## Transforms (`scene/`)

### Types
```cpp
struct TransformState { mat4d world; quatd rotation; double3 localScale; };   // bound, copyable
class EvalCtx {                      // bound; all that a pure evaluation may read
  double channel(int i) const;       // the override if one is set, else the object's value
  int channelCount() const;
  double time() const;               // seconds: the scene's current frame over its fps
};
class ChannelBlock { int size() const; double get(int) const; void set(int, double); };

struct TransformModel : IfaceRef {
  TransformState evaluate(const EvalCtx &ctx, const TransformState &parent) const;
  Status setWorld(const EvalCtx &current, const mat4d &target, const TransformState &parent,
                  SetWorldMode mode, ChannelBlock &out) const;
  Status applyDelta(const EvalCtx &base, const ChannelBlock &near, const TransformDelta &,
                    const TransformState &parent, ChannelBlock &out) const;     // optional
};
```
- **EvalCtx has no Library, Runtime or Context.** P5c fills `overrides` from its sockets.
- **Reading a channel as Bool, Int or Enum** rounds to nearest (Bool: >= 0.5), overrides
  included. An enum value with no item is the first item.
- **`TransformType`** (in `TransformRegistry`, a Runtime extension): name, StructId, channel defs
  (Bool, Float, Int, Vec2 to Vec4, Enum, Quat, flattened in order), defaults, one
  `Owned<TransformModel>`. A second registration of a name is AlreadyExists, so a second JS
  object can't inherit the first one's slot table.
  - Registering builds its ApiStruct, whose parent is `TransformChannels`. Members read and
    write the owning object's channel array.
  - Int and Enum channels outside ±2⁵³ are refused by their setter (OutOfRange). `coerce`
    clamps, so it can't do this.
- **`setWorldOf(lib, object, target, mode, out)`** is what callers use.
  - **The current channels come first:** if they already give the target (the check below), they
    are returned unchanged, without calling the model. So an object's own world is always found,
    with any scales, pivots, quaternion length or singular parent.
  - Otherwise it calls the model with the parent's state (below), and **checks the result** by
    evaluating it: the linear part by the column residual (1e-9), the translation within
    `16 ε · (|P|·L + |target t|)`. `L` is the sum of the magnitudes of the channels of subtype
    Distance (translation, offsets, pivots), times 1 plus the largest local scale. Without the
    pivots in `L`, 2,598 of 3,000 objects with pivots of 100 were refused; with them, none at
    pivots up to 1e6. Beyond the bounds it is FailedPrecondition, "not representable".
  - `Closest` drops shear and keeps the translation. It checks only that the result is finite
    and that the translation holds.
  - A parent that `tryInverse` refuses is FailedPrecondition in both modes. A zero-scale *target*
    is fine.
  - **Commands use `Closest`.** In the exact mode, unparenting a rotated child of a (1,2,1) parent
    (residual 0.56) would fail, and so would a delete of many objects because of one. Exact is
    for callers that need to know. Float32 matrices are refused by it (1,998 of 2,000): importers
    use `Closest`.
- **`TransformDelta {kind, basis (quatd), pivot, vector, axis, angle}`.** `basis` replaces a space
  enum: world, local, view and custom orientations are all bases.
  - **The delta is the total from `base`;** `near` is the previous result. Trs and Fbx pick the
    Euler solution nearest to `near`, so a 720° drag ends at 720°. A model without the slot goes
    through `setWorld`.
- **`computeState(lib, object, time)`** walks up from the object, uncached, with a cycle check.
  Every command uses it, so a command can parent to an object created a moment before, and
  parents changed earlier in the same command count. It is Busy in an EvaluationScope.

### Built-in models
- **`core.transform.trs`:** location, rotationMode (6 orders, QUATERNION), rotationEuler,
  rotationQuaternion, scale. A zero or non-finite quaternion is the identity. Setting
  `rotationMode` converts nothing; `core.object.setRotationMode` does.
- **`core.transform.fbx`:** lclTranslation, lclRotation, rotationOrder, pre- and postRotation,
  rotation and scaling offsets and pivots, lclScaling, inheritType. Local is
  `T·Roff·Rp·Rpre·R·Rpost⁻¹·Rp⁻¹·Soff·Sp·S·Sp⁻¹`; `LR = Rpre·R·Rpost⁻¹`.
- **`core.transform.matrix`:** 16 channels; any affine target. `rotation` and `localScale` come
  from `decomposeTrs` (reflections on x).

### Failure behaviour
- **Scale channels of 0** evaluate. Rrs uses 1 for a zero component of `pls` (documented).
- **A non-finite result:** the object gets its parent's state and `EvalStatus::Invalid`. Children
  follow it. Its bounds aren't in SceneBvh. Nothing aborts.
- **A foreign model that throws** is noticed right after its call: the sticky error is taken, the
  object gets its parent's state and `EvalStatus::ModelFailed`, and `evaluate()` returns the
  first such error after finishing. `evaluate()` refuses to start with an error pending, as
  `collect()` does.
- **P5c keeps this rule inside its kernel** (a fallback state plus a status output), so it
  doesn't use `lastGood()` for it.
- **Huge coordinates:** 1e12 works to about 1e-4; 1e200 overflows and ends as Invalid.

## Objects and Scene (`scene/`)

### Capabilities
- **`Transformable`:** `read(ObjectInfo &info, ChannelBlock &channels) const`, one bulk slot. An
  `ObjectInfo` holds the type name's id, parent id, data id and flags. Also
  `writeChannels(const ChannelBlock &) -> Status` and optional `setParentId(const BlockId &) ->
  Status`. No slot returns an id.
- **`Spatial`:** `localBounds() -> Box3d`, `domains()`, `closestElements(co, radius, domain) ->
  Vector<int32_t>` (object space), optional `pick(const PickQuery &, PickSink &)`. Without `pick`
  the object answers with its bounds.
- **`Invalidation`:** `invalidate(uint32 kinds)`.
- **`Elements`:** `topoStamp`, `elementCount(domain)`, `positions(handles, domain) ->
  Vector<double>`.
- `objectQuery<I>(lib, handle)` asks the data block first, then the object.
- **`foreignImplementable` is extended:** a struct return needs bound default and copy
  constructors, and a Vector return needs scalar elements. Otherwise it is refused when
  registered, not on the first call.

### Change notification
- **Through the datapath:** `setValue` notifies (P5's step 1 does so before the `update` hook).
- **Commands** write channels with `writeTransform(ctx, object, channels)`, which calls
  `writeChannels` and then `notify(owner, "transform")`, in exec, undo and redo.
- **Data blocks:** `invalidateData(ctx, block, kinds)` is what anyone calls after changing a
  block's own data. It calls the block's `Invalidation` if it has one, then
  `notify(ofBlock(id))`. Built-in tools and hosts use it; nothing else calls `Invalidation`.
- **P3's evaluator doesn't listen.** It recomputes.

### `coredcc.Object`
- Data: flags (Hidden, Unselectable), Weak parent, OptStrong data, the transform type's name, its
  channels.
- A missing transform type keeps its channels (as P2's `BlockEntry`s once loaded) and evaluates
  as its parent, flagged `MissingModel`.
- ApiStruct: `name` (read-only; renaming is P2's command), `flags`, `parent`, `data`, `transform`
  (a fixed nested pointer, whose struct is the transform type's).
  - `parent` and `data`, and Scene's `activeObject`, fill P2's `PointerAccessor::cell`, so undo
    keeps links to placeholders.
- **Storage:** declared `BlockStorage::Transient` in the step that adds the type, and Payload in
  the serialize step. A type with neither fails every save (P2's A4).

### `coredcc.Scene`
- Data: OptStrong cells in order, Weak active object and selection, unit settings, fps, current
  frame. A handle-to-index map beside the cells makes `contains` and `add` O(1). Same storage
  rule as Object.
- **Created with the Root flag,** by `core.scene.new`. (P2's append turns Root into FakeUser.)
- **Cells:** `objects()` skips cells whose target isn't Live; InHistory and Detached members come
  back by themselves. **Cells whose target has no record are pruned** by the membership commands
  (never by `evaluate`, which doesn't change data). Undo of `core.object.add` and `link` removes
  the cell itself. A save writes null for cells to deleted blocks (P2), and the load drops null
  cells.
- **Parents:** `setParent` refuses a parent that isn't in the scene, and cycles. A parent that is
  outside anyway (a host, undo, a file) is evaluated but not picked. Cycles found by `evaluate`
  (undo of two reparents, a file) get the identity parent and `EvalStatus::Cycle`.
- **`evaluate(ctx)`:**
  - holds `beginNoCollect`, and calls `SceneEvaluator::run`, which opens P5's EvaluationScope:
    setValue, executor entries, graph edits, `computeState` and another `evaluate` are Busy.
    `evaluate` itself doesn't open it, because P5c's evaluator must set graph inputs and call
    `GraphInstance::evaluate`, which are Busy inside one;
  - walks the hierarchy with an explicit stack, parents first; world boxes are the transform of
    the local box, rounded outward; then SceneBvh;
  - **writes to a second store and swaps it in at the end,** so a foreign slot that reads
    results sees the previous evaluation, complete.
- **Lookups are cached per member:** handle, interface tables, parent index, topological order.
  The cache is in the Scene's block data, so it goes with it, and a duplicate starts without one.
  - **The key** is `Library::epoch()`, the interface registry's `tableCount()`, and
    `ClientRegistry::generation()`: a later `registerImpl`, or an alias that makes a raw cell
    resolve, doesn't move the epoch.
  - **When the key changes, it is revalidated in place:** every member's lookups are made again
    and compared, with the cell list. If they are equal, the order, the store and SceneBvh stay.
    Selecting writes Weak cells, which moves the epoch, so this is the common case.
  - **Checked on every evaluate:** `Library::data()` is called again for every object and data
    block (hosts may move them), and `read`'s parent and data ids are compared with the cached
    ones. A different id, or data that is null and wasn't (or the opposite), rebuilds the cache and
    starts over. If that happens again in the same call, `evaluate` is FailedPrecondition and
    keeps the last results: a slot answers differently each time.
  - **Cached only when the table comes from the block's own type,** with non-null data. An
    override isn't cached. Where the type has no table and its provider has `queryInterface`,
    it is asked each time, also after a null answer.
- **The store has its own handle-to-index map,** swapped with it. The Scene's map changes between
  evaluations.
- **Results:** `evaluated(handle, ObjectEval &out) -> bool` copies. Nothing hands out pointers
  into the store.
- **By path:** a runtime owner `coredcc.evaluated` has a collection `objects` keyed by the
  object's BlockId, as P2's text (`objects["<client>,<32 hex>"]`), with `world`, `bounds` and `status`, for the Context's scene.
  `addEvaluated(ApiStruct &root)` puts it on a host's root. After the scope closes, `evaluate`
  calls `notify` on that owner if any result changed. Watchers then fire, no block is stamped as
  edited, and an object in several scenes has one answer. With no scene, or none evaluated, it
  is NotFound.
  - The key isn't the handle: these items' addresses do contain it, and a handle names another
    object after a reload.
  - Results are compared bitwise.
- **Seam for P5c:** `SceneEvaluator::run(Scene &, EvaluatedStore &)`, internal.
- **The Context slot** is `setScene(BlockHandle)` and `scene()`; `activeScene(ctx)` resolves it.
  `setScene` bumps the ChangeBus's structural epoch, so watchers of `evaluated` see the switch.

### Setting the active item
- **The active item is an ordinary settable Pointer member** of the collection's holder: Scene's
  `activeObject`, a Weak cell, with `PointerAccessor::cell`. Its setter refuses an object that
  isn't in the scene.
- **`CollectionAccessor::activeMember`** (optional) is that member's name. **Writes** to a path that
  ends in `.active` of such a collection go to the member: `resolveForWrite(ctx, path)` resolves
  everything before `.active`, then the member of the holder. Otherwise it is `resolve()`. The
  string forms of `setValue`, `setValueUndo` and SetProperty use it.
- So undo, folding, links to blocks that aren't Live, watchers (of `activeObject`), NoUndo,
  ReadOnly, Busy and text come from SetProperty. Nothing else is added.
- **Reads are unchanged:** `objects.active` resolves to the item and is NotFound without one;
  `getMeta` answers for the item.
- A collection whose items live inside their owner names a member with its own `address`.
- Files: `api/struct.h`, `api/resolve.{h,cc}` (one function), `command/set_property.cc`. P2's
  static resolver and `meta` see `.active` as today.
- **Keys:** `objects` uses the block handle, packed into an int64, as its key
  (`objects[#...]`), and `lookup` also takes a name (the first match). Items are roots of their
  own owner, so canonical addresses never contain the key.

### Commands (`scene/commands.cc`)
- **Which objects:** commands that take several work on the Scene's **selection**. An optional
  String input `objects` replaces it: space-separated `@block(<client>, <32 hex>)`, P2's text form
  (its B1), in order, duplicates ignored; a missing one is NotFound.
- Single objects are Pointer inputs. From invocations they need B1's literals.
- `core.scene.new`, `core.scene.link`, `core.scene.unlink`, `core.scene.setActive`,
  `core.scene.select(mode=SET|ADD|REMOVE|TOGGLE|ALL|NONE)`.
- `core.object.add(type, name, location, parent)`.
- `core.object.setParent(parent, keepWorld)`, `core.object.setTransformType(type, keepWorld)`,
  `core.object.setRotationMode(mode)`.
- `core.object.applyDelta(kind, basis, pivot, vector, axis, angle)`: TransformDelta's fields as
  inputs. It skips descendants of selected objects. Interactive use keeps `base` from the start.
- `core.object.delete(keepChildrenWorld)`: reparents Live children outside the set to their
  nearest surviving ancestor, parents first, then runs `core.block.delete`. Plain
  `core.block.delete` lets children jump; undo restores them.
- Each captures old channels, and old parents, from the objects themselves.

### SceneBvh
- `Bvh<BoxSource>` over world boxes. Refit needs the same objects, the same skipped set, and a
  cost below 2× the last build's; otherwise it rebuilds.
- **`pick(view, px, radius, filter) -> Result<Vector<PickHit>>` answers for the last `evaluate`,**
  which is what was drawn. Before one it is FailedPrecondition. Hosts evaluate once per frame.
  1. The region's planes against world boxes.
  2. Per object: planes to object space; `Spatial::pick`, or its bounds.
  3. **Clip to the near plane,** then score in screen space.
  4. Sort: **ray hits before misses;** hits by depth; misses by screen distance, then depth; ties
     in scene order.
- The object-space ray exists only if the object's matrix inverts. Otherwise the region's
  candidates are transformed to world and scored there.
- **The interfaces are queried again for each candidate,** and one that isn't Live, or whose data is
  null now, is skipped. Data evaluated earlier may have been freed or moved since.
- **Spheres and capsules** are scored by the exact distance, in pixels, from the cursor to the
  outline of the solid clipped by the near plane (`outline.{h,cc}`):
  - **GJK in 2D** on the projected, clipped solid, which is convex. It ends when the certified
    gap is under 0.01 px, or with 0 when the cursor is inside. 5 to 7 iterations.
  - **The support point** in a screen direction `n` maximizes `n·(x, y) / w`. With the value `h` of
    the last point, it is the point that maximizes the linear function `n·(X, Y) - h·W` (the rows
    of the object-to-pixel matrix), repeated until `h` stops growing (2 or 3 times). Every point
    has `w > 0` because of the clipping, so the eye plane needs no case.
  - **The linear maximum over a clipped capsule is closed form:** the best of each end's ball cut by
    the plane, and, where the plane crosses between them, the point where it cuts the line of
    the two ends' support points for `g + λm` with `(g + λm)·(b - a) = 0`.
  - An empty clipped solid is not a candidate.
  - **Measured** (`p3\outline.mjs`, `p3\outline2.mjs`): against an independent minimizer on 393
    solids, whole, crossing the near plane and crossing the eye plane, never more than 1e-4 px
    above it, and 0 in 5 cases where the minimizer found no point inside (one checked by hand);
    against GJK on the dual conics, on 36,000 whole capsules (the sets where the
    search failed, sheared, orthographic), within 1e-5 px. 2.5 to 6.4 µs per candidate in JS.

## Units and frames (own small step, last)
- **`UnitSettings`** (`props/unit_settings.h`): a display unit per family, fps as a rational, start
  frame. Scene has one at `units`; `Context::units()` gives the active scene's, or the defaults.
- `frame` is a Time unit of `1/fps`, in new overloads of `units::format` and `parse` that take
  UnitSettings. `formatValue` and `parseValue` in `api/format.h`.
- `PropertyDef::validate()` checks that subtype and unit agree. No def outside a doc comment sets
  either today, so nothing is refused; the check is for new defs.

## Bindings (`bind/`)
- **Types:** `Mat4d`, `Quatd`, `double3`, `Box3d`, `TransformState`, `EvalCtx`, `ChannelBlock`,
  `TransformDelta`, `View`, `ViewMath`, `ObjectEval`, and the TransformModel interface. Value
  types have `at` and `put`, so a script needs no array members.
- **`registerForeignTransformType(ctx, type, handle, slots)`**, with `newTransformType(name)` and
  `transformChannel(type, name, defaultValue)`. One JS object per type; ForeignBindings keeps its
  handle. Busy while evaluating. `core.` is reserved.
- **Facades:** `newScene`, `setScene`, `evaluateScene`, `worldMatrix(ctx, objectText)`, and `pick`
  (evaluates first), each Busy inside a foreign slot other than `exec`. P5's (interface, slot)
  gate is used once it is merged.
- **threadAffine:** every loop that calls interfaces runs on the calling thread.

## Files (each under 35 KB)
```
bvh/    box.h  ray.h  kernels.{h,cc}  bvh.h  build.h  sources.h  geometry_bvh.h
        any_source.{h,cc}  bindings.cc
draw/   view.{h,cc}  bindings.cc
scene/  eval_ctx.h  transform.{h,cc}  solve.{h,cc}  transform_registry.{h,cc}  trs.cc  fbx.cc
        matrix.cc  capabilities.h  object.{h,cc}  scene.{h,cc}  evaluator.{h,cc}  evaluated.{h,cc}
        scene_bvh.{h,cc}  pick.{h,cc}  outline.{h,cc}  api.cc  membership_commands.cc
        transform_commands.cc  commands.h  bindings.cc  builtins.{h,cc}
api/    format.{h,cc}
props/  unit_settings.h
bind/   foreign_transform.{h,cc}
tests/  bvh/  draw/  scene/  bench/
```
Layering: `base` ← `bvh`, `draw`; `command`, `bvh`, `draw` ← `scene` ← `bind`.

## Tests
- **BVH oracles:** every query against a brute-force loop, on random, clustered, coplanar,
  all-equal, long-thin and degenerate sets, leaf sizes 1 to 1024, float and double. Kernels
  against long-double references. Watertightness is tested with rays **aimed** at edges and
  vertices, from outside too.
- **Extremes:** centroids at 3e38f and 1e308, 1e160 boxes, denormal extents, NaN and infinity, 0
  and 1 primitives, `leafSize` 0.
- **Determinism:** node bytes for `ScopedBandCount` 1, 2, 5 and 12 (tests only), parallel and not.
- **Foreign source** through the fake host: thread ids, a throw mid-gather, a wrong length.
- **View:** every combination of depth range, reversed Z, infinite far, handedness and ortho, with
  near planes in front of and behind the eye; flags that contradict the matrices; the region is
  conservative, and scoring is exact to 1e-3 px up to offsets of 1e6; `dpiScale` 2.
- **FBX golden values:** `tests/scene/gen_fbx_golden.mjs` is a separate implementation of the
  sample's formulas. Its table (mixed types, sheared, signed and reflected chains, pivots) is
  committed. Mutation checks: dropping GSM, reordering it, and replacing `GR` with a decomposition
  must each fail.
- **Solving:** own worlds with pivots up to 1e6 and scales from 1/1000 to 1000 through three parents
  (TRS too), with non-unit quaternions and scales of 1e-13, come back unchanged; `Closest` keeps
  mirrors under shear; pivots in targets that aren't the object's own. A node's own world for every model, with signs, shear, zero scales and scales from
  1/1000 to 1000; unchanged channels; refusals; parent conditions to 1e8; translations at 1e10
  with a wrong rotation must be refused.
- **Deltas:** total-angle drags to 720° with `near`, custom bases, gimbal lock.
- **Scene:** cycles; a deleted and restored parent; Detached; delete, then history clear, then a
  membership command (the cell is gone); undo of add and its discard; purge keeps the Scene; a
  100,000-deep chain; an Invalid and a throwing model mid-chain; a foreign slot that reads and one
  that writes during `evaluate`; a host-owned Transformable whose data moves; a ref written
  without `writeRef`; several scenes; quarantine mode on.
- **Commands:** create a parent and parent to it in one step; `[A, B]` with A above B; delete of a
  parent and its child; `objects` text with bad, missing and repeated ids; undo and redo of each.
- **Scene cache, where the epoch doesn't move:** a provider's `queryInterface` from null to an
  answer; `registerImpl` after the first evaluate; data that becomes null and comes back, while
  Live; an alias registered later. Also: selection keeps the order and SceneBvh (counters);
  `evaluated` after an unlink; a `read` whose parent differs each time; a duplicate and a new
  Library; `setScene` and a watcher.
- **Active item:** from no item, clear, wrong type, not a member; every existing `.active` read as
  before; a member name that doesn't exist (`validate`). **Delete the active object, set another,
  undo both; a Detached active object, then undo.** Gestures fold. An inner collection
  (`tool.brushes`).
- **Picking, analytic:** spheres and capsules against a minimizer over the solid, with ends offscreen,
  at and across the near plane and the eye plane, near planes from 0.001, sheared and singular
  matrices, perspective and ortho, and scores at each pixel around them.
- **Pick after changes:** evaluate, delete, clear the history, pick; data moved by its host.
- **Picking, triangles:** against a screen-space loop over every triangle, clipped to the near plane, for
  non-uniform, reflected, sheared and singular objects, in perspective and in ortho with the near
  plane behind the eye. The pressure test's thin-curve case.
- **compile_embed** includes every new header. **wasm-st** runs all of it under node
  (`COREDCC_TEST_SCALE`). The TS spike gains a foreign TransformModel. **wasm-mt** BVH and scene
  tests run in node.

## Order of work
Each step ends with the gate (4 native presets, wasm-st-debug) and a commit.

| Step | What | Needs |
|---|---|---|
| 0 | litestl PR D, opened alongside step 1 | — |
| 1 | `bvh/`: box, kernels, serial build, sources, queries, oracles | — |
| 2 | Parallel build, refit, GeometryBvh, AnyBvhSource, bindings | 1 |
| 3 | The benchmark and its command. **Mergeable here.** | 2 |
| 4 | `activeMember`, the Context scene slot, `foreignImplementable`'s return check. **Mergeable here.** | P2 A2 (`PointerAccessor::cell`) |
| 5 | `draw/View` | PR D |
| 6 | EvalCtx, the registry, Trs, Fbx, Matrix, the solver, golden values | PR D |
| 7 | Capabilities, Object, Scene, evaluate, results by path, API structs | 4, 6; P5 step 1 (EvaluationScope, notify); P2 A1 (`BlockStorage`, epoch bumps) |
| 8 | Commands. *Feature review: objects and transforms.* | 7; P2 A2 (`PointerAccessor::cell`) |
| 9 | SceneBvh and picking. *Feature review: BVH.* | 3, 5, 7 |
| 10 | Scripts: foreign transform types, facades, the TS test; the `objects` text input | 8, 9; P2 B1 (pointer text) |
| 11 | `serialize`, `deserialize`, `duplicate`, `rekey` for Object and Scene; storage becomes Payload | 7; P2 A1 (A4 for the round-trip test) |
| 12 | Units and frames | 7 |

- **On master:** P5's step 1 (`537b3d2`), P2's A1 (`09fd77e`) and A2 (`f6dd789`).
- If B1 is late, step 10's text input moves after 12.
- Channels are serialized as `BlockEntry`s by name, beside the type's name; unread ones are kept.

## Implementation notes

### Step 0
- litestl PR #14 (`coredcc/math`). Deviations: `planeToLocal` instead of `transformPlane`; the
  bindings are in `matrix_bindings.h`, because `binding.h` includes the existing
  `math_bindings.h` everywhere and `matrix.h` brings in Eigen.

### Steps 1 to 3
- **Commits:** the parallel build and refit share the serial build's code, so they are in step
  1's commit. Step 2 is AnyBvhSource and the binding, step 3 the benchmark.
- **`Bvh<Scalar>`** is a template on the scalar, and `build` and `refit` on the source.
  `onInvalidate` takes `GeometryChange` flags. More than 2³¹−1 primitives can't happen:
  `size()` is an int.
- **A median split** sorts by centroid, and by primitive on ties, once per subtree; below it
  ranges are halved.
- **Equal SAH costs** go to the more balanced split. Without that, evenly spaced primitives in a
  line gave a deep tree.
- **No check of the bins' scale:** for denormal extents it is infinite, the clamp handles it, and
  no cost is then below the leaf's, so the median is used. The check was redundant (its mutant
  survived), and so was swapping axes to keep the winding in the ray shear: u, v and t don't
  depend on it.
- **`poolUsable()`:** false on a browser's main thread and in wasm without threads (a test that
  set a band count started the pool there and aborted).
- **Claim 4, contraction:** confirmed. With fused products in two or three of the edge
  functions, the aimed-ray test leaks 3 to 42 rays. wasm's clang doesn't have
  `#pragma float_control`; the compile flags cover it.
- **`region` is conservative at leaves too:** a primitive is dropped only if it is certainly
  outside one plane.
- **wasm-mt:** the four BVH tests pass in node.
- **The leaf-size rule was replaced** (coordinator, 2026-11-24). The first one took the best query
  time among the sizes within 10% of the fastest build, but the build gets faster with every
  larger size, so it kept only the largest ones, with queries 6 to 10 times slower than 16's.

### Step 4
- `resolveForWrite` is one flag in the path walk. **Resolving `.active` also touches the
  activeMember:** inside one runtime owner neither the structural epoch nor the Library moves
  when it is written, so watchers of `brushes.active.size` would not have fired.
- `ApiRegistry::validate()` checks the activeMember (a settable pointer of the same struct, to the
  items' struct), not `add()`, because the member may be a parent struct's.
- `Context::setScene` takes any block handle and doesn't check it; `activeScene(ctx)` (step 7) does.
- `foreignImplementable` has a reason per slot. It also refuses reference returns and owned
  pointers, which litestl's dispatch refuses on the call.

### Step 5
- **Needs litestl PR #14.**
- `draw` depends on `bvh`, for `Ray` and `BvhPlane`.
- **`validate()` checks handedness with one determinant:** normalized device coordinates (x right, y
  up, depth away from the eye) are left-handed, so the sign of det(proj · view), flipped for
  reversed depth, gives the world's. It also finds mirrored images and, in orthographic views, a
  wrong `reversedZ`.
- **Not decidable from the matrices,** and documented: a [0, 1] projection declared as [-1, 1]
  (another near plane), and a float-precision projection with a distant far plane declared as
  infinite or not.
- **Refused:** oblique near planes (the depth row depending on x or y), and an orthographic
  `infiniteFar`.
- Clip and view transforms are applied one after the other, not as one matrix, and unprojecting
  inverts them separately. Pixels are within 1e-3 px to offsets of 1e6 (tested).
- The region's radius also grows by 1e-9: with the eye at the origin, a plane has no constant to
  pad, and the mutant without it loses points.
- **The grep test** finds `.inverse(` and `*=` in `draw/` and `scene/`. `Matrix * Vec` can't be told
  from `Matrix * Matrix` by text; that stays with reviews.
- Survivor: unproject's zero test at infinity. The inverse's entries cancel exactly in every test.

### Step 6
- **Needs litestl PR #14.**
- **`solveWorld(type, current, target, parent, mode)` and `solveDelta`** are the model-level halves
  of `setWorldOf`. Step 7 adds the ones that take an object, and `computeState`.
- **The Matrix type has 12 channels** (three columns and a translation), not 16. A state's world is
  affine, so the last row would be four channels that nothing reads.
- **An FBX node's `inheritType` defaults to RSrs,** the ordinary hierarchy. `rotationOrder` has the
  library's numbers, not FBX's, which swap YXZ and YZX. Angles are in radians.
- **A delta keeps what it can of the node:**
  - Translate changes the location only, and Scale the location and the scales: one factor per
    own axis, the part of the stretched axis that stays along it. So it never turns the node, and
    goes through 0 to a mirror. It is exact for uniform factors and along the node's axes.
  - Rotate conjugates with the parent's carried rotation for RrSs and Rrs, which is exact under
    any parent. For TRS and RSrs it turns around `P⁻¹ · axis` (the other way under a mirror),
    so the axis stays where it is in the world. It is exact without non-uniform parents.
  - A pivot at the node's origin doesn't move it, to the bit: the location gets an increment.
- **The rotation pair** is the most independent in both A and X. With A alone, parallel columns of
  X have no frame.
- **The API struct's data is the ChannelBlock.** An object's `transform` pointer returns its own.
- **A result's channels aren't checked for NaN,** only the state: a NaN in the Euler angles
  of an object in quaternion mode mustn't make it immovable.
- **Mutations:** 88, each alone. Dropping GSM (RrSs and Rrs), reordering it, and a decomposed `GR`
  fail the golden values. The survivors:
  - one step of refinement instead of two: the bound tested is 8 roundings;
  - `1 + scale` in the translation bound: every construction that needs it also has a large
    Distance channel;
  - sign patterns of zero columns, which are only skipped to save time;
  - each model's own check of the parent, which `solveWorld` and `solveDelta` make first.

### Step 7
- **Needs litestl PR #14.**
- **Files:** `capabilities.h object.{h,cc} scene.{h,cc} evaluator.{h,cc} evaluated.{h,cc}`. The API
  structs are with their types, so there is no `api.cc`.
- **`Transformable` has one more optional slot,** `setTransformTypeId`, for
  `core.object.setTransformType`. `Spatial::pick` comes with step 9.
- **Two empty API structs:** `coredcc.ObjectData`, the parent of structs that can be an object's
  `data`, and `coredcc.SceneObject`, the parent of structs that can be a `parent` or
  `activeObject`. A host's own objects use them. `transformType` (read-only) and `fps` are members
  too, and the selection is the collection `selectedObjects`.
- **`active` and `selected` leave out what isn't a member,** so an unlink needn't touch them, and
  its undo brings them back.
- **`SceneEvaluator::run(scene, library, changed, foreign)`** keeps both stores itself. It compares
  each result with the last one, and starts to write the second store where the first
  difference is. An unchanged scene writes nothing and swaps nothing.
- **Revalidation asks only what `evaluateAll` doesn't check anyway** (the list, overrides,
  Live targets, `queryInterface`). `read` and `data` are compared while evaluating. Where they
  differ, everything is looked up again, and evaluated again.
- **Looking up again** uses the last parent and data handles as hints: checking one is several times
  faster than finding its id. The store's index is an array over handle indices, not a hash map.
- **`ChannelBlock` holds 16 channels inline,** and the evaluator prefetches eight objects ahead
  (addresses of the last evaluation). Each saved about 4 ms of 100,000 objects: nearly all of the
  rest is waiting for memory.
- **A model or `read` that throws** is returned by `evaluateScene` after everything else is
  computed. Errors of a pass that was thrown away aren't.
- **`computeState`** returns a foreign error, and is FailedPrecondition for a block that isn't
  Live and Transformable. A parent that isn't Transformable is the identity there too.
- **An object's type name is registered with the Runtime** when it is set, so undo can come
  back to a missing type by id.
- **`blockIdText` and `parseBlockIdText`** (`evaluated.h`) are the text of a block id. P2's B1 should
  use or replace them.
- **Asked-for ids are kept** in a Runtime extension for the life of the Runtime (24 bytes each):
  a PointerRef into the results mustn't point into the store.
- **Measured on a busy machine** (two other worktrees building; CPU load between 5 and 100 %;
  the same binary varied by 10 to 20 % from run to run). `bench_scene`, 41 repetitions, ms, best and
  median:

  | | without SceneBvh | a 100,000-box refit |
  |---|---|---|
  | Unchanged (target 20) | 13.3, 16.0 | 4.8, 5.3 |
  | Every root moved | 13.2, 15.7 | |
  | The epoch moved (target 40) | 19.7, 23.2 | |
  | Structural, a parent (target 80) | 56.7, 62.5 | build: 17.3, 20.3 |
  | Structural, unlink and link (target 80) | 34.9, 39.0 | |

  The sums meet the targets: 18 to 21, 25 to 28, and 62 to 83 (with a build, if a changed parent
  needs one). The numbers to record come from an idle machine, with step 9's SceneBvh.
- **Mutations:** 83, each alone; those that survived and weren't redundant code (removed) got tests.
  What survives: the places where `add` keeps the index instead of making it again (speed only),
  and the Live checks of revalidation, which null data shows too.

### Step 8
- **Needs litestl PR #14.**
- **Files:** `commands.h membership_commands.cc transform_commands.cc object_changes.cc`.
  `registerSceneCommands` registers the types, these commands and the block commands.
- **Until step 10's `objects` input,** `core.scene.select` takes one `object`. `ALL` leaves out
  hidden and unselectable objects, and naming an unselectable one fails. `core.scene.link` and
  `setActive` take one, too.
- **`keepWorld` solves exactly, then as well as it can** (shear is lost). The output
  `approximate` counts the objects that couldn't be kept exactly. A singular parent fails.
- **`core.object.add`'s `location` is in the world,** also under a parent.
- **`ObjectChanges`** holds what a command changed, by id, with the values before and after. Undo
  and redo write those, so nothing is computed again. `write` takes everything back when one
  fails. An undo that fails for one object still writes the rest, and the first error becomes
  the sticky one.
- **`core.object.dragDelta`** is `applyDelta` as an Interactive command: `ApplyDelta::update` takes the
  total delta since the start and works on the channels from then. Interactive commands can't
  be run directly, hence two names for one implementation.
- **An object on a cycle isn't skipped by `applyDelta`:** it has no parent.
- **`core.object.delete`** looks for children in the whole Library. A child whose Transformable has no
  `setParentId` is left alone and jumps. One that can't be placed (a singular parent, no solver)
  only gets the new parent, and counts as approximate.
  - **Deleting many is quadratic:** each `core.block.delete` asks `usersOf`, whose cache is made
    again whenever the epoch moved. 500 objects of 2,000 take 68 ms, so 10,000 of 100,000 about a
    minute. Deleting several blocks with one preview belongs in `core.block.delete`.
- **Mutations:** 64, each alone. The survivors:
  - the member that a change is posted for: the epoch moves as well, and makes the watchers read
    everything again.

## Exit criteria
- The gate passes; wasm-mt builds and its BVH and scene tests pass in node.
- **`evaluate` on 100,000 native objects with data blocks, Release, idle machine: at most 20 ms
  when nothing structural changed (SceneBvh refit included), 40 ms when the epoch moved and
  nothing differs (a selection), and 80 ms after a structural change.** A bench target measures
  all three.
- The benchmark has run on an idle machine, `docs/bvh-leafsize.md` is committed, and the default
  follows from it.
- FBX golden values and mutation checks; `setWorld` keeps every model's own world.
- A TransformModel written in TS works in the spike.
- Both feature reviews are resolved. `docs/embedding.md` says where coredcc uses the task pool,
  and `docs/debugging.md` says when to call `evaluate` and `invalidateData`.

## Claims this revision depends on
1. PR D changes nothing for another repo: it doesn't touch the three headers, and nobody includes
   `quat.h`.
2. Interleaved largest-first subtrees balance contiguous bands well enough.
3. The stack of 96 holds for any input with the checked options.
4. Compiling one file without contraction keeps the triangle test watertight in every preset,
   wasm included.
5. Padded planes are conservative for every affine matrix up to offsets of 1e9.
6. The near planes taken from the projection are right in every convention.
7. `TransformState` is enough for every FBX type and for P5c.
8. Current channels first keeps a node's own channels; the sign search finds representable targets
   within the stated limits.
9. The cache's key and per-evaluate checks cover every answer that can change.
10. Writing to a second store makes every re-entrant read safe.
11. `computeState` is enough for every command.
12. The `coredcc.evaluated` owner can't loop with P5's versions.
13. The outline distance is right for any clipped capsule, under any affine matrix.
14. A flat array of doubles holds every channel kind exactly.
15. `activeMember` changes no existing read, and only writes to `.active` that failed before.
16. The `objects` text covers what TS and P2's layouts need.
17. 20, 40 and 80 ms are met.

## Risks
- **PR D waits for the user.** Steps 5 to 12 can be written, not merged, without it.
- **Size:** 12 steps. P4 and P5c need 5 to 9; the rest can follow.
- **The FBX rules are checked against a published sample, not the SDK.**
- **Stateless models** can't keep caches; a spline rig would be a graph node writing overrides.
- **`evaluate` at 100,000 objects** is still 10 to 20 ms per frame in a drag. P5c is the answer.
- **Float geometry far from its own origin** loses precision in the source data.

## Deviations from the architecture
Recorded there when the phase is done:
- decisions 1, 2, 5, 6, 8 and 9; `components()` replaced by TransformState;
- `CollectionAccessor::setActive` is `activeMember`;
- View moves from P4; `OverlayDrawable` is declared in P4;
- §9: cone queries are region queries (no `coneFromPixel`); the concept has no `centroid(i)` and
  no per-query callbacks;
- §8: `AABB<double3>` is `Box3d`; Transformable and PickSink are added; Invalidation is called
  only by `invalidateData`, and doesn't drive the evaluator or SceneBvh;
- "gizmos skip descendants" is in `core.object.applyDelta`.

## Pressure test record (2026-11-24)
A fresh agent was given the first draft (`1f88d05`) and the code, without the author's reasoning.
The author re-ran its experiments (all reproduced) and checked its claims about the code. The new
parts of this revision have not had a second pressure test, except where a measurement is given.

### Survived
- The FBX formulas (re-derived), `TransformState` as the parent, planes by `Mᵀ`, outward-rounded
  float nodes with double traversal, the 4-ulp slab pad, the parallel primitives, the depth bound
  (with checked options), transform types registered after Object's struct, OptStrong not
  cascading, const arguments being read-only in JS, the existing facade gates, the units check.

### Broke, and what changed
- **B1, commands' inputs can't be expressed.** The selection, an `objects` text input in P2's
  form, Pointer inputs for single objects, TransformDelta flattened; new, link and unlink commands.
- **B2, ortho views couldn't pick behind the eye; perspective scored clipped geometry.** The
  projection's near plane is used for rays, clipping and the oracle; negative depth in ortho.
- **B3, 10 ms was really 40 ms** (reproduced: 39–43). One bulk slot and cached, checked lookups:
  8.5 to 10 ms measured, about 15 with `data()` again. Targets are 20 and 80 ms.
- **B4, setWorld refused an object's own matrix** (29% and 76% with signs, reproduced). Eight
  signs, nearest to the current channels; frames instead of an inverse.
- **B5, the tolerance was wrong both ways.** Separate linear and translation checks, a stated
  threshold, FailedPrecondition, a check for Closest, and refinement of the translation.
- **B6, zero-scale targets gave NaN.** Completed from the other columns and the current rotation.
- **B7, Möller-Trumbore leaks at edges** (8.8%, reproduced). A watertight test, aimed rays, and no
  contraction in that file.
- **B8, the largest singular value misranks thin curves.** Exact conic distance; hits before
  misses. Its proposed formula is rejected, see below.
- **B9, no source for parent states.** `computeState`, and a stated contract for pick.
- **B10, a throwing model gave a zero matrix.** Errors are taken after each call; `ModelFailed`.
- **B11, scripts can't return ids, and the check let it through.** No slot returns one; the check
  is extended.
- **B12, deleted objects stayed as cells** (confirmed in `destroyBlocks`). Undo removes the cell,
  commands prune, P2's save writes null.
- **B13, conflicts with P2 and P5.** `implementOn` is out; `invalidateData` is the only caller of
  Invalidation and of `notify` for data blocks.
- **B14, the setActive change was understated.** A separate `resolveActive`; `resolve()` isn't
  touched; the file list is corrected.
- **B15, gizmos lost winding; no custom basis.** Base and near channels; `quatd basis`.
- **B16, weak FBX tests** (reproduced). Golden values, mutation checks; the Maya difference is
  documented.
- **B17, planes aren't exact in floating point.** Padding, a minimum radius, a conservative test,
  outward rounding of world boxes.
- **B18, small ones:** overflow and denormals (scaled areas, median split); Int range (the
  setter); the skipped set (a refit condition); `unproject` at infinity; `contains` (a map).
- **Parallel "chunk 1" doesn't balance; "at most 64" should be "at least"; options unchecked.**
  Largest first, interleaved; reworded; checked.
- **Purge destroyed the Scene.** It is created with Root.
- **Several scenes, watchers of `worldMatrix`, `ObjectEval *` lifetimes, re-entry.** The evaluated
  owner, copies, the second store, EvaluationScope.
- **Out-of-scene parents.** `setParent` refuses them.
- **`implName` reuse.** Type names are unique.
- **Missing pieces:** scene commands, the time source, Matrix's rotation, `dpiScale`, flag checks,
  rounding of overrides, the fallback rule, unrecorded deviations. Each is defined above.
- **wasm-mt on a browser's main thread.** Serial there.
- **Size.** Cut or deferred: `implementOn`, `rayTest`, world nearest, the load monitor, most of
  the 10M matrix. Units are their own last step.
- **Step 4 (View) did need PR D.** It is now step 5.
- **The author's claim that PR D may rewrite `matrix.h`** (found broken while checking): the first
  draft assumed nobody depended on `Matrix * Vec`; sculptcore does. PR D leaves the file alone.

### Rejected
- **B8's formula** (the support function along the separation direction is exact). It is a lower
  bound: for an ellipse of radii 10 and 0.1 and a point at (8, 8) it gives 4.24; the distance is
  7.94.
- **B14's `Kind::Active` from `resolve()`.** It would reach every caller of `resolve()`
  (`setPrefix`, Pointer values, P2's path text). A separate entry point doesn't.
- **"TransformState's socket kind must be chosen here."** P5's plan leaves it to P5c, and either
  fits.

### Not verified
- The FBX SDK; sculptcore, faberforge or UberTopo compiled against PR D; wasm-mt in a browser; the
  TS side of B11 and of `at`/`put`; the near-plane rules (reasoned; tested in step 5); whether
  contraction really breaks the triangle test; refit and SceneBvh times in the 20 ms target.

## Second pressure test record (2026-11-24)
A fresh agent tested the four pieces that were new in the revision. Its measurements were re-run
(all reproduced), and its claims about the code were checked on `f6dd789`.

### Survived
- The dual conic's formula; RrSs and Rrs inversion; state changes and id comparison in the cache;
  notifying `coredcc.evaluated` (with bitwise comparison).

### Broke, and what changed
- **The outline search missed narrow separating arcs** (545 of 1,616, reproduced), **gave nonsense for
  spheres across the eye plane, and its near-plane fallback lost 23 of 31 near misses.** Replaced
  by GJK, as it proposed. Its support function is different: see "Rejected".
- **`Closest` lost mirrors** (2,000 of 2,000). Residual within 1e-9 of the smallest comes first.
- **The translation check refused pivots** (2,598 of 3,000). The bound counts every Distance channel.
- **TRS and RSrs under scaled parents were refused.** The current channels come first, and the claim
  is limited to what holds.
- **The commands' mode wasn't stated.** `Closest`.
- **Four answers change while the epoch doesn't.** The key has `tableCount` and the client
  generation; nullness of data is checked; `queryInterface` isn't cached, null or not.
- **P5c's evaluator would have been Busy inside `evaluate`'s scope.** `run()` opens it.
- **`pick` could use freed data.** It queries again.
- **A selection cost a rebuild.** Revalidated in place, with a target (40 ms).
- **Undo of the active object lost it, and couldn't name inner items; results disagreed.** An
  ordinary Pointer member, named by `activeMember`.
- **`setScene` didn't reach watchers; `evaluated` was keyed by a handle.** The epoch; BlockId text.
- **Small:** the store's own map, the second difference, where the cache lives, the singular
  threshold, non-unit quaternions, scales of 1e-13, float32 targets. Each is stated above.
- **Euler near gimbal lock** (43% at 1e-7 with independent `atan2`): PR D already used the stable
  form. Its tests now go from 1e-2 to 0 from lock, in all orders, and fail with the other form.
- **Tests:** the missing ones are listed.

### Rejected
- **"GJK on the support function, with eye-plane and near-plane rules on whole end spheres."** The
  dual conic's support only holds for solids wholly in front, so the others still need a second
  method, and the proposed ones (clip the axis and add the cap disc) approximate. The support of
  the clipped solid is closed form, so one method is exact for all of them. It costs about
  twice as much per candidate.

### Not verified
- The 40 ms target; the near-plane clip in pixel space at offsets of 1e9.
