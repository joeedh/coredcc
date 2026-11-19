# P0 and P0b: scaffold, base, interface system, litestl PRs

Status: draft, not yet pressure-tested.

Covers rows 3 and 4 of `docs/implementation-tasklist.md`: architecture plan §14 P0 and
P0b, and the interface system of §2a. They are planned together because the interface
system can't be finished without the litestl changes.

## Toolchain (checked 2026-11-19)
- Visual Studio 18 Community, with its bundled CMake, Ninja and clang-cl 22.1.3. None of
  them is on `PATH`; builds run inside `vcvars64.bat`.
- emsdk 3.1.53 at `C:\dev\emsdk`. Not needed until P1.
- Node 24 and pnpm, for the test app only.
- sccache is not installed. It isn't needed until the parallel phase.

## Repository scaffold (on master)
- **Submodule:** `git submodule add https://github.com/joeedh/litestl.git source/litestl`,
  at litestl master `8a98264`. The directory must be named `litestl`.
- **CMake:**
  - The root `CMakeLists.txt` adds litestl only `if(NOT TARGET util)`, with
    `LITESTL_BUILD_TESTS` off.
  - Targets: `coredcc_base` now, more per module later. The include root is `source/`.
  - Warnings: `/W4` (clang-cl, MSVC) or `-Wall -Wextra`, as errors in coredcc's own
    targets only. Flags: `/GR-`, `/EHs-c-`, `_HAS_EXCEPTIONS=0`, `/Zc:preprocessor`. No FP
    flags.
- **Presets** (`CMakePresets.json`): `native-clang` and `native-msvc`, each with Debug and
  RelWithDebInfo. The build directory is `build/<preset>`.
- **Environment:** `tools/devshell.cmd` runs `vcvars64.bat`, then its arguments, so
  `tools\devshell.cmd cmake --build --preset native-clang-debug` works from any shell.
- **Tests:**
  - They use litestl's harness (`litestl/tests/test_harness.h`: `test_init`,
    `test_assert`). One executable per file, registered with CTest by a `coredcc_test()`
    macro.
  - Run with `ctest --preset <preset>`. A single test is `ctest --preset <preset> -R <name>`.
- **Docs:** replace CLAUDE.md's "Project status" section
  with the build and test commands.

## The litestl gitlink rule
- coredcc's gitlink for `source/litestl` only ever points at a commit on litestl's master.
  A rebase merge gives PR commits new hashes, so a gitlink to a PR branch would dangle
  after the merge.
- While litestl PRs are open, the submodule checkout sits locally on the PR branch.
- coredcc commits that compile only against unmerged litestl changes go on a local branch
  in the main checkout (`p0/iface`), not on master. Once the PRs merge, the submodule moves
  to litestl master, the gitlink is bumped in a commit on that branch, and the branch is
  fast-forwarded into master. That way master always builds.
- No worktree is used for this: it is one branch, and work on master pauses meanwhile.

## base/ (on master, needs no litestl changes)
- **`Status`:** a code (enum), a message and optional suggestions. `[[nodiscard]]`, with
  a cheap `ok()` path. `Result<T>` holds a value or a Status.
- **Type ids:** a 64-bit hash of a stable type name, from a `constexpr` function, so ids
  can be compile-time constants. The algorithm is fixed forever. The Runtime rejects a
  second name with an id already in use.
- **`HandlePool<T>`:** generational handles (a 32-bit index and a 32-bit generation) over
  stable storage. It has a quarantine mode that never reuses slots, for tests (§8a).
- **`Signal<Args...>`:** multicast callbacks with connection tokens. Firing while a slot
  disconnects itself is safe.
- **`Runtime`:** owns every registry. In P0 that's only the type-name registry and the
  interface registry. It has no global instance.

## Interface system (`base/iface/`, on `p0/iface` until the litestl PRs merge)
As specified in architecture §2a. P0 delivers:
- `IfaceRef`, `IfaceTable`, `CDCC_INTERFACE`, `CDCC_CALL`, `CDCC_HAS`.
- `registerImpl(rt, I, Impl, "name") -> Status`, with signature matching on name, full
  type name, size and type key, and exact const and return types.
- `BIND_ADAPTOR_METHOD` registration for adaptors.
- Single-base inheritance with concatenated slots. Other bases are reached by explicit
  upcast tables.
- The registry: `(TypeId, InterfaceId) -> IfaceTable`, plus `query<I>(TypeId, void*)`.
  Querying a block arrives in P1 with the library.
- `Owned<I>`, an owning interface value that destroys its implementation.
- `Runtime::validateAll()`.
- **Foreign tables** with a native fake dispatcher. It records calls and returns canned
  results through the same marshalling entry point the WASM and N-API dispatchers will use.

### Tests
- One interface implemented four ways, as listed in architecture §14 P0:
  1. a bound class;
  2. an adaptor over a type we don't own;
  3. a foreign table driven by the fake dispatcher;
  4. a mismatched class that `registerImpl` must reject, with the expected message.
- A self-referencing interface (`TransformModel`-shaped).
- Single-base inheritance, and an upcast to a second base.
- An optional slot, both present and absent.
- The prototype's failure cases as regression tests: const mismatch, int vs int64, a type
  bound under another type's name.
- A compile-fail test: `CDCC_CALL` with a misspelled slot or wrong argument types must not
  compile. litestl's `tests/compile_fail` shows the pattern.
- **Build-time measurement:** compile a `bindings.cc` with 50 bound methods under clang-cl
  and MSVC, and record the time in this plan.

## litestl PRs (P0b)
Branches are made in `source/litestl`, named `coredcc/<topic>` (the pr-workflow
convention), and opened against `joeedh/litestl` master. litestl runs a Claude review
workflow on PRs. Every PR builds and passes litestl's own tests on clang-cl and MSVC
first, and emcc for the binding PRs.

| PR | Contents | Needed by | Depends on |
|---|---|---|---|
| A. Embedding hygiene | `#undef _` after its last use; prefix `force_inline`, `array_size`, `flatten_inline` (`LSTL_`); rename `ensure`/`verify` identifiers; fix C4458 shadowing; `<windows.h>`-safe min/max; Eigen out of the public `matrix.h` | `compile_embed` test | — |
| B. Binding fixes | `Struct::inherit` stray `;`; the TS return-buffer leak; string and array marshalling in the TS `buildArgs`; `.c_str()` in `std::format` calls; thread-safe `PermanentGuard` | P1 spike | — |
| C1. Interface descriptors | `isInterface`, `isOptional`, `findMethod`, per-type key, `BindingInfo` offsets; const-recording `Binder<const T&>`/`<const T*>`; noexcept `MethodTraits`; `std::invoke` + free-function `MethodTraits` + `BIND_ADAPTOR_METHOD`; `->template`; variadic `BIND_OPTIONAL_METHOD` | interface system | — |
| C2. Interface TS support | generator: implementable interfaces, optional methods, `Readonly<>`, `bigint`; runtime: Enum case, `ptrType` reads, the return writer, the dispatcher hook | P1 foreign TS commands | C1; touches the same TS runtime as B |
| D. Math | fix `quat.h`; Euler in 6 orders; decompose; translate/rotate/scale/perspective/lookAt | P3 | — |
| E. N-API reverse marshalling | the dispatcher hook under N-API | P6 | the other session's N-API runtime PR |

- **Order:** A, B and C1 in parallel; then C2; D before P3; E after the N-API PR.
- **Stacking.** Per the PR rules, whether C2 stacks on B or C1, and E on the N-API PR, is
  the user's call. I'll ask when each one is ready, not decide it.
- Before opening each PR, check litestl's open PRs for overlap, especially the N-API PR,
  which touches `binding/`.

## Order of work
1. Scaffold, presets, devshell, the CTest wiring, and the CLAUDE.md build section.
   Commit on master.
2. `base/`: Status, type ids, HandlePool, Signal, Runtime, with tests. Commit on master.
3. TA0 of the test app, once its plan has been pressure-tested. It needs nothing from
   litestl.
4. litestl PRs A, B and C1.
5. On `p0/iface`: the interface system against those branches, then C2.
6. Once A, B and C1 merge: bump the gitlink and fast-forward `p0/iface` into master.
7. The feature review of the interface system, written up as
   `docs/reports/feature-report-interfaces-<date>.md`.

## Exit criteria
- Native builds and tests pass under `native-clang` and `native-msvc`, Debug and
  RelWithDebInfo.
- `compile_embed` passes against merged PR A.
- The interface tests above pass, including the compile-fail ones.
- The binding build-time measurement is recorded here.
- The interfaces feature report exists, and its findings are resolved or tracked.

## Claims to pressure-test
- litestl at `8a98264` builds as a subdirectory of another project with
  `LITESTL_BUILD_TESTS` off, with no other options, under clang-cl and MSVC.
- litestl's `tests/test_harness.h` can be used from outside litestl's own test directory.
- `/W4` as errors in coredcc targets is achievable while including litestl headers. If
  litestl headers warn, they need to be included as system headers.
- A `constexpr` 64-bit name hash is good enough for type ids, given that collisions are
  detected at registration.
- A gitlink is never pushed pointing at an unmerged PR commit under this branch scheme.
- The P0b PR split has no hidden dependencies beyond those listed.
