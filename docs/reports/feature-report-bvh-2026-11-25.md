# Feature report: BVH, View and picking (`bvh/`, `draw/`, `scene/pick`), 2026-11-25

- **Reviewed:** `bvh/`, `draw/View`, SceneBvh and picking at `44ebacd`, against architecture §9 and §10
  and the P3 plan (steps 1 to 5 and 9).
- **Reviewer:** a fresh agent given the code and the spec, not the author's reasoning. It built a
  scratch copy and confirmed most of its findings with programs. Its timings are from a busy
  machine.
- **This file:** a rewrite of its review. Each finding ends with its resolution.

## Summary
- **What held up:** the kernels (watertight triangles, 3 million near-parallel capsules), every query
  against brute force at offsets up to 1e9 except for finding 3, the build's determinism and depth
  bound, View with extreme planes and float matrices, picking precision at offsets of 1e8, and
  re-entry during a pick.
- **What didn't:**
  - answers that depend on rounding, for flat geometry along the axes and for view matrices from
    general inverses;
  - the tree's quality, which nobody noticed getting bad next to a large object;
  - the pick ranking, under which points and curves over a surface couldn't be picked;
  - everything a sculpting host or a script needs beyond the first query.

## Bugs
1. **SceneBvh never rebuilt with one huge object in the scene,** because `cost()` is relative to the
   root. 19,619 boxes were tested per region instead of 105.
   - *Fixed:* `Bvh::growth()`, the mean over the nodes of how many times larger each is than when the
     tree was built, counted as 1 to 8. A node is compared with itself, so an outlier changes a few
     terms. A node that was almost nothing next to its parent counts as a little larger. SceneBvh
     rebuilds above 2, and hosts have the same signal. `cost()` says what it is good for.
   - *Tested:* with an outlier, and with an object that flies away.
2. **`View::validate` refused view matrices of common JS code,** 29% of three.js's, whose last row is
   off by an ulp.
   - *Fixed:* what must be 0 may be up to 1e-6 of what is in its column, and then is 0. The same for the
     projection. Tested with general inverses, scaled and rounded matrices.
3. **Rays against flat boxes along an axis broke two contracts:** `tMax` isn't inclusive (22 to 34% of
   such hits missed), and ties went to another primitive (0.5 to 1%).
   - *Fixed:* nodes are made larger, by 1e-12 of how far from the ray's origin they reach, on both
     sides. A triangle's t is moved into its own box (made larger by a tenth of that), which also
     gives triangles without an area a t that means something. Boxes that are primitives keep an exact entry.
   - *Found while testing far from the origin:* spheres and capsules computed points in the world's
     coordinates, and lost precision there. They are computed relative to the ray's origin now, and
     give the same bits anywhere.
   - *Tested:* three new layouts (a lattice of overlapping flat triangles, rays along and through
     edges, coordinates near 1e7), and `tMin` and `tMax` at a hit's own t, for all queries.
4. **Points, segments and thin curves in front of a surface couldn't be picked,** since a covering
   surface has distance 0, and the sink kept one hit.
   - *Fixed:* a new ranking (the plan's notes have the reasons): thin shapes that aren't behind the surface
     under the cursor come first, by distance; then what covers the cursor, by depth; then the rest.
   - `PickOptions` has the domains that are wanted, whether surfaces hide thin shapes, and what counts as
     thin. `PickQuery` has `domains()`, `wants()` and `pixelSizeAt()`. Points and segments may have a
     pixel radius. The sink keeps the best hits of each domain.
   - Architecture §9 says now that "rank by (screenDist, depth)" is refined.
5. **No incremental update, and a refit cost a copy.**
   - *Fixed:* `refit(src, dirty primitives)`, which costs what is dirty and the nodes above it;
     refits write into the existing columns, in parallel; `build(Source &&)` takes a source without
     a copy.
   - *Deferred, tracked in the plan:* a source that borrows a host's indexed arrays, until a host needs
     it.
7. **Objects beyond a finite far plane were picked.** *Fixed:* a region has six planes, the cursor's ray
   ends at the far plane, and shapes are clipped to it; `ViewPoint::visible` means between the
   planes. The viewport's edge cuts nothing off; documented.
13. **`AnyBvh` had no refit and no `onInvalidate`.** *Fixed:* both, and a refit of a range of primitives, which
    gathers only those.
15. **An object whose bounds come and go forced a build per frame.** *Fixed:* a refit takes an empty box for a
    primitive of the tree, which is absent until a refit gives it bounds again. A primitive that had none
    when the tree was built still needs a build.

## API
6. **`GeometryChange` and `InvalidateKinds` had the same flags with swapped bits.** *Fixed:* one
   `InvalidateKinds` (`base/invalidate.h`). `None` and `Selection` do nothing.
8. **No region but a square around a pixel.**
   - *Fixed:* `ViewMath::regionFromRect`; `SceneBvh::objectsInRegion`, `objectsOnRay` and `objectsNear`;
     the optional `Spatial::elementsInRegion`; `pickRegion` for a scene.
   - *Argued:* there is nothing to implement `elementsInRegion` for in coredcc's own Object. It has no
     geometry: its data block is the host's. `GeometryBvh::region` answers it in one line for hosts.
9. **A pick asked every candidate and returned every hit.**
   - *Fixed:* `PickOptions`; candidates are asked front to back; with `maxHits` 1, the query of an object
     says how deep (`limitDepth`) and how far from the pixel (`surfaceRadiusDevicePixels`) anything is
     still wanted, and objects that can't be first aren't asked.
   - `firstOnly` would be the same as `maxHits` 1, so there is only `maxHits`.
   - *Deferred, tracked:* cancellation.
10. **Hit data and filters were missing.**
    - *Fixed:* every query takes a filter; `RayHit::backFacing`; `GeometryBvh::source()`; `PickHit` has
      the normal, `u` and `v`, `backFacing`, `thin` and the instance.
    - *Deferred, tracked:* k-nearest, and nearest to a segment.
11. **`rayAll` counts a hit on a shared edge twice.** *Documented,* with why a parity-safe count isn't
    cheap: which of the two faces counts needs the mesh's neighbours or an exact tie rule, and the
    closed edge signs that make the test watertight say "both".
12. **Scripts couldn't use the BVH.**
    - *Fixed:* `AnyBvh`, `RayHit`, `NearestHit`, the queries and sinks are bound, with bulk sink calls
      (`points`, `segments`, `triangles`, `capsules`, `addAll`), and so are View's conventions, regions
      and planes.
    - *Argued:* scripts build an `AnyBvh` from a vector of bounds, not from a source. A source's slot
      answers 4,096 primitives per call; a script can hand over all of them in one.
    - *Tested:* `test_bind_spatial`, a Spatial written by a fake foreign host, against a native one with
      the same triangles, for `pick`, `closestElements` and `elementsInRegion`; what it throws, and
      vectors of the wrong length.
14. **No nearest query for a scene.** *Fixed:* `nearestInScene`, in world space, with `maxCount`;
    `closestElements` gets a `NearQuery` and a `NearSink`, which takes positions and limits the search.
16. **Threading and pool use weren't in `embedding.md`,** and SceneBvh's options were fixed. *Fixed:* the
    section, and `SceneSettings`, a Runtime extension with the options.
17. **Mixed pixel units.** *Fixed:* what is in device pixels, and isn't a position, says so in its name.
18. **A `size()` of 2^32 + n wrapped.** *Fixed:* compared in 64 bits; OutOfRange.
19. **No concept for a geometry source.** *Fixed:* `GeometrySource`, which documents each function.
20. **Headers.** *Fixed:* `BvhPlane` is next to `Ray`, so `view.h` doesn't include the BVH.
21. **Three shapes of planes; queries that silently do nothing.** *Fixed:* `BvhPlane` everywhere in C++;
    `rayAll`, `sphereOverlap` and `region` return InvalidArgument for what isn't finite.
22. **`PickQuery`'s constructor took values that could contradict each other.** *Fixed:* it takes a `PickCursor`.
26. **`rayFromPixel` doesn't end at the far plane.** *Documented.*

## Tests, plan and benchmark
23. **The slack in `nearest` wasn't covered.** *Changed:* no input was found for which it matters, so it is
    gone. The slack that nodes have for rounded sources stays, and is tested with a source that rounds.
    The far, outlier, far-plane and general-inverse tests are added.
24. **The plan claimed long-double references.** *Corrected:* the kernels' oracles are geometric, and on
    Windows a long double is a double.
25. **The benchmark ignored `sphereOverlap` and `nearest`.** *Fixed:* both are measured and count for the
    leaf size. SceneBvh has its own (8; the plan's notes have the numbers).
27. **CLAUDE.md is out of date.** *Not changed:* it is the user's file.
