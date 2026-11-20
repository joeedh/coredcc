# P0 and P0b: scaffold, base, interface system, litestl PRs

Status: pressure-tested 2026-11-19 and revised; the record is at the end. Decision
(user, 2026-11-20): the litestl PRs wait for litestl PR #3 to merge, then branch from
litestl master.

Covers rows 3 and 4 of `docs/implementation-tasklist.md`: architecture plan §14 P0 and
P0b, and the interface system of §2a.

## Toolchain (checked 2026-11-19)
- Visual Studio 18 Community, with its bundled CMake, Ninja and clang-cl 22.1.3. None of
  them is on `PATH`; builds run inside `vcvars64.bat`.
- emsdk 3.1.53 at `C:\dev\emsdk`. It can't compile any litestl translation unit that
  binds a method (a consteval `std::format` error in `binding_method.h:48`), and lacks the
  `emdawnwebgpu` port litestl's WASM link macro uses. The WASM toolchain is decided in the
  P1 plan (see "Open for P1").
- Node 24 and pnpm, for the test app only.

## Repository scaffold (on master)
- **Submodule at `extern/litestl`, not `source/litestl`.**
  - litestl adds its parent directory as an include root. Under `source/`, that made
    `source/` itself a system include directory once litestl was added `SYSTEM`, which
    silently disabled warnings in coredcc's own headers (reproduced on both compilers).
  - `extern/litestl` keeps `extern/` as litestl's include root and `source/` as
    coredcc's.
  - Pinned at litestl master `8a98264`.
- **Adding litestl:** `if(NOT TARGET util) add_subdirectory(extern/litestl SYSTEM)`, with
  `LITESTL_BUILD_TESTS` off. With tests off it does no network fetch.
  - When a parent project (ubertopo) supplies litestl non-`SYSTEM`, coredcc sets the
    `SYSTEM` property on the litestl targets it links, so litestl's warnings don't hit
    coredcc's `/WX`.
  - A feature macro from the interface-descriptor PR (`LITESTL_HAS_IFACE_DESCRIPTORS`) is
    checked with a `static_assert`, so building against a too-old parent pin fails with a
    clear message.
- **Flags:**
  - At directory scope, so litestl gets them too: `/GR-`, `/EHs-c-`,
    `_HAS_EXCEPTIONS=0`. Strip `/EHsc` and `/GR` from `CMAKE_CXX_FLAGS` first. Applying
    them only to coredcc's targets would leave `_HAS_EXCEPTIONS` different across
    translation units.
  - `/Zc:preprocessor` is MSVC-only; clang-cl rejects it as an unused argument.
  - `/W4` (or `-Wall -Wextra`) on coredcc targets. `/WX` is controlled by
    `COREDCC_WERROR`, which defaults to `PROJECT_IS_TOP_LEVEL`.
  - No FP flags. litestl's `attr` target has a PUBLIC `/fp:precise`, so coredcc doesn't
    link `attr` until that's dealt with.
- **Presets** (`CMakePresets.json`): `native-clang` and `native-msvc` configure presets,
  using Ninja Multi-Config in `build/<preset>`. Build and test presets
  `<preset>-debug` and `<preset>-release` (RelWithDebInfo).
- **Environment:** `tools/devshell.cmd` runs `vcvars64.bat >nul 2>&1`, then its arguments,
  and propagates the exit code. From Git Bash, set `MSYS_NO_PATHCONV=1` when passing
  `/flags` through it.
- **Tests:**
  - Use litestl's harness, `tests/test_harness.h` (`test_init`, `test_assert`). It
    includes `dtl/dtl.hpp`, which exists only when litestl's own tests are on, and doesn't
    use it. coredcc ships an empty stub at `tests/support/dtl/dtl.hpp` until PR A drops
    the include.
  - `coredcc_test(name src...)`: one executable per file, registered with CTest, with
    `LITESTL_TEST_NO_BREAK=1` in its environment. Without that, a failing assert calls
    `__debugbreak` and the test takes 6.5 s instead of 0.06 s.
  - `coredcc_compile_fail(name src regex)`: an `EXCLUDE_FROM_ALL` object target, plus a
    test that runs `cmake --build . --config $<CONFIG> --target <it>` and expects the
    regex in the output (`PASS_REGULAR_EXPRESSION`), with a `RESOURCE_LOCK`. litestl's
    own compile-fail pattern is skipped for MSVC-style compilers, which includes clang-cl.
    Verified for clang-cl and MSVC.
  - A **warning probe**: a compile-fail test whose source is a coredcc header with an
    unused parameter. It catches any change that makes `source/` a system directory
    again.
  - Run with `ctest --preset <preset>-debug`, or `-R <name>` for one test.
- **Docs:** replace CLAUDE.md's "Project status" section with the build and test
  commands. Start `docs/embedding.md`, listing litestl's process globals (architecture
  §2).

## The litestl gitlink rule
- coredcc's gitlink for `extern/litestl` only ever points at a commit on litestl's
  master. A rebase merge gives PR commits new hashes, so a gitlink to a PR branch would
  dangle after the merge.
- **Enforced by a pre-commit hook** (`tools/hooks/pre-commit`, installed with
  `git config core.hooksPath tools/hooks`). It rejects a staged `extern/litestl` gitlink
  that isn't an ancestor of the submodule's `origin/master`. The pressure test reproduced
  a plain `git commit -a` recording a PR commit.
- **Work that needs unmerged litestl PRs** happens on one local coredcc branch, `spine`,
  in the main checkout. It holds the interface system, and then P1, until the PRs merge.
  Because of the hook, `spine` never records a PR gitlink either.
- **In the submodule:** a local, never-pushed branch `coredcc/integration`, rebuilt by
  cherry-picking the open PR branches onto their base. That's what `spine` builds against.
- **Switching branches:** after checking out master, run `git submodule update`, so
  master builds against its own pin. Before fast-forwarding `spine` into master, build and
  test it in a temporary clean worktree at the bumped gitlink.
- **Merging:** once the PRs merge, the submodule moves to litestl master, the gitlink is
  bumped in a commit on `spine`, `spine` is verified, and it's fast-forwarded into master.

## base/ (on master, needs no litestl changes)
- **`Status`:** a code (enum), a message and optional suggestions. `[[nodiscard]]`, with
  a cheap `ok()` path. `Result<T>` holds a value or a Status.
- **Ids:** one constexpr hash, FNV-1a 64, over a domain tag plus the stable name:
  `"type:" + name`, `"iface:" + name`, `"client:" + name`. ClientId uses it too (the
  architecture said SipHash, but litestl's SipHash isn't constexpr).
  - The algorithm and tags are fixed forever.
  - A Runtime rejects a second name with an id already in use.
  - Collisions with names it never sees (placeholders, unregistered types in a file) can't
    be detected, so files always store names, and ids are rebuilt from them on load.
- **`HandlePool<T>`:**
  - A handle is a 32-bit index and a 32-bit generation over stable storage.
  - Generations start at 1, so `{0, 0}` is the null handle and never valid.
  - A slot whose generation reaches `UINT32_MAX` is retired and never reused.
  - Quarantine mode never reuses any slot, for tests (§8a).
- **`Signal<Args...>`:** multicast callbacks. litestl's `util::CallbackList` isn't used,
  because it isn't safe against re-entrancy. The contract:
  - `connect` returns a `Connection` token. Dropping the token doesn't disconnect; a
    `ScopedConnection` does.
  - A slot connected during an emit isn't called in that emit.
  - A slot disconnected during an emit, by itself or by another slot, isn't called again
    in that emit.
  - A re-entrant emit is allowed.
  - Destroying a Signal during its own emit is a bug, caught by a debug assert on the
    emit depth.
  - A token that outlives its Signal is harmless: it holds a weak reference to shared
    state, and disconnecting it does nothing.
- **`Runtime`:** owns every registry. In P0 that's only the id registry and the
  interface registry. It has no global instance.

## Interface system (`base/iface/`, on `spine`)
As specified in architecture §2a. P0 delivers:
- `IfaceRef`, `IfaceTable`, `CDCC_INTERFACE`, `CDCC_CALL`, `CDCC_HAS`.
- `registerImpl(rt, I, Impl, "name") -> Status`, matching slots by name, full type name,
  size and type key, and requiring exact const and return types.
- Adaptor registration (`BIND_ADAPTOR_METHOD`, or #3's free-function binding if that is
  where the PR lands).
- Single-base inheritance with concatenated slots. Other bases are reached by explicit
  upcast tables.
- The registry: `(TypeId, InterfaceId) -> IfaceTable`, plus `query<I>(TypeId, void*)`.
  Querying a block arrives in P1 with the library.
- `Owned<I>`, an owning interface value that destroys its implementation.
- `Runtime::validateAll()`.
- **Descriptor caches.** The `CDCC_INTERFACE` descriptor and each implementation's
  descriptor are built inside `alloc::PermanentGuard`. Without it, litestl's leak check
  in `test_end()` failed with 6 bound methods. Registration happens only on the main
  thread, so `PermanentGuard`'s missing thread safety doesn't bite in P0. The litestl
  fix is still in the PR list.
- **Foreign tables** with a native fake dispatcher. It records calls and returns canned
  results through the same marshalling entry point the real backends will use.

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
- Compile-fail tests: `CDCC_CALL` with a misspelled slot, and with wrong argument types.
- **Build-time measurement:** compile a `bindings.cc` with 50 bound methods under clang-cl
  and MSVC, and record the time here.
- **`compile_embed`:** lives on `spine`, because it needs PR A. It includes coredcc's
  public header closure with UE-style `check`/`verify`/`ensure`/`TEXT` macros and
  `<windows.h>` without `NOMINMAX`, with litestl headers *not* treated as system headers.
  `quat.h` is excluded until PR D.

## litestl PRs (P0b)
**litestl PR #3** (`litestl/napi-bindings`, from session `litestl-c2`) is open and large.
It rewrites the files these PRs touch (`binding_method.h`, `binding_struct.h`,
`typescript.cc`, the whole TS runtime), and it already does several planned items:
- `->template` in `BIND_STRUCT_METHOD`;
- free-function binding and noexcept traits for free functions, which can carry the
  adaptor methods;
- string, array and span marshalling in the TS runtime;
- `bigint` for int64;
- owned returns freed on dispose, which may cover the return-buffer leak (to be checked);
- the `function_ref` struct/class mangling fix, without which clang-cl fails to link
  litestl's own attr tests at `8a98264`;
- an N-API runtime behind an `IBindingBackend` interface shared with WASM, and WASM
  builds with plain clang and WASI instead of Emscripten.

**Decision (user, 2026-11-20): wait for #3 to merge**, then branch every PR from litestl
master. Before writing each PR, re-check its items against post-#3 master and drop what
#3 already did. Until then the interface system is blocked; P0 does the scaffold and
`base/`.

| PR | Contents | Needed by |
|---|---|---|
| A. Embedding hygiene | `#undef _` after its last use (it's currently undefined *before* the definition); `LSTL_`-prefix `force_inline`, `array_size`, `flatten_inline`; fix C4458 in `binding_constructor.h`; `<windows.h>`-safe min/max in `vector.h`, `string.h`, `task.h`, `boolvector.h`, `sort.h`, `math/vector.h`, `path/path.h`; MSVC C5030 on `[[gnu::flatten]]`; C4099 on `String`; drop the unused `dtl` include from `test_util.h` | `compile_embed` |
| B. Binding fixes not in #3 | `Struct::inherit` stray `;` (both loops); `.c_str()` in the `std::format` calls (blocks every emcc build); thread-safe `PermanentGuard`; the return-buffer leak if #3 doesn't cover it | interface system, any emcc build |
| C1. Interface descriptors | `isInterface`, `isOptional`, `findMethod`, per-type key; their `BindingInfo` offsets *and* the matching readers in `wasmInterface.ts` and #3's N-API runtime, with an ABI version bump; const-recording `Binder<const T&>`/`<const T*>` and its effect on full names in `typescript.cc` and `python.cc`; noexcept member-function `MethodTraits`; variadic `BIND_OPTIONAL_METHOD`; the `LITESTL_HAS_IFACE_DESCRIPTORS` macro | interface system |
| C2. Implementable interfaces | generator: implementable TS interfaces, optional methods, `Readonly<>`; a foreign-dispatch hook in #3's `IBindingBackend`, implemented for both the WASM and N-API backends | P1 foreign TS commands |
| D. Math | fix `quat.h`; Euler in 6 orders; decompose; translate/rotate/scale/perspective/lookAt; decide whether Eigen leaves the public `matrix.h` (an API break: it removes `Matrix`'s Eigen conversions) | P3 |

- The old PR E (N-API reverse marshalling) folds into C2, because #3 puts both backends
  behind one interface.
- **Dropped from A:** renaming `ensure`/`verify`. Only `attr` uses `ensure(`, coredcc
  doesn't include `attr`, and renaming `AttrGroup::ensure` would break ubertopo.
- **Moved from A to D:** taking Eigen out of `matrix.h`, because `quat.h` includes it and
  it's an API break.
- **Order:** A, B and C1 in parallel; then C2; D before P3.
- A, B and C1 touch some of the same files (`binding_struct.h`, `binding_method.h`,
  `binding_constructor.h`). They don't depend on each other logically, but expect
  rebase conflicts. The smallest one goes first.
- **Gates for each PR:** builds, and litestl's tests (including #3's binding conformance
  suite) pass, on MSVC and clang-cl locally. litestl CI after #3 adds linux gcc and clang.
- **Coordination:** before opening each PR, check litestl's open PRs for overlap, as the
  PR rules require.

## Order of work
1. Scaffold: submodule at `extern/litestl`, CMake, presets, devshell, the test helpers
   and warning probe, the pre-commit hook, the CLAUDE.md build section,
   `docs/embedding.md`. Commit on master.
2. `base/`: Status, ids, HandlePool, Signal, Runtime, with tests. Commit on master.
3. When #3 has merged: litestl PRs A, B and C1, from litestl master.
4. On `spine`: the interface system against `coredcc/integration`, then C2, then
   `compile_embed`.
5. P1 continues on `spine` while the PRs are in review.
6. Once A, B and C1 merge: bump the gitlink, verify, and fast-forward `spine` into master.
7. The feature review of the interface system, written up as
   `docs/reports/feature-report-interfaces-<date>.md`.

## Exit criteria
- Native builds and tests pass under `native-clang` and `native-msvc`, Debug and
  RelWithDebInfo, including the warning probe.
- `compile_embed` passes against merged PR A.
- The interface tests above pass, including the compile-fail ones.
- The binding build-time measurement is recorded here.
- The interfaces feature report exists, and its findings are resolved or tracked.

## Open for P1
- **WASM toolchain.** emsdk 3.1.53 can't build bound methods until PR B, and can't link
  litestl's WASM targets (no `emdawnwebgpu` port). #3 moves litestl's own WASM build to
  WASI and plain clang. sculptcore's `emsdkVersion.txt` asks for 5.0.6. The P1 plan
  decides between emcc (needed for `EM_JS` and to share a module with sculptcore) and
  #3's WASI path, and how `wasm-st`/`wasm-mt` map onto it. The test app needs
  `-sENVIRONMENT=web` if it's emcc.
- How coredcc consumes litestl's TS runtime after #3 (`backend.ts`, `dispatch.ts`), for
  the `wasm-st` TS spike.

## Pressure test record (2026-11-19)
A fresh agent was given this plan, the architecture plan and litestl, but none of the
author's reasoning. It built superprojects against a clone of litestl `8a98264` with
clang-cl, MSVC and emcc. Experiments are in `scratchpad\p0-pt\`.

### Survived
- litestl `8a98264` builds as a subdirectory with tests off, with no network fetch,
  under clang-cl and MSVC, Debug and RelWithDebInfo. It also builds at #3's head.
- litestl compiles with `/GR- /EHs-c- _HAS_EXCEPTIONS=0` applied globally, and its tests
  pass.
- A constexpr FNV-1a 64 hash compiles clean under `/W4 /WX` on both compilers, and works
  as a template argument.
- `tools/devshell.cmd` works from Git Bash and cmd, and propagates the exit code.
- An `EXCLUDE_FROM_ALL` target plus a ctest that builds it works as a compile-fail test
  on both compilers.

### Broke, and what changed
- **`/W4 /WX` with litestl headers:** about 20 warnings come from litestl headers. Making
  litestl `SYSTEM` under `source/` silenced coredcc's own headers too. Moved the
  submodule to `extern/litestl`, added a warning probe, and gated `/WX` for hosted builds.
- **`/Zc:preprocessor` under clang-cl** fails the build. It's MSVC-only now.
- **litestl's test harness from outside** fails on a missing `dtl/dtl.hpp`. A stub, plus
  a litestl fix in PR A; and `LITESTL_TEST_NO_BREAK=1` so failed asserts don't
  `__debugbreak`.
- **Cached binding descriptors fail litestl's leak check.** They're built under
  `PermanentGuard`.
- **litestl's compile-fail pattern** is skipped for clang-cl and MSVC. Replaced.
- **The gitlink scheme:** `git commit -a` on the local branch recorded a PR gitlink, and
  master built silently against PR code after a checkout. Added the pre-commit hook, the
  submodule integration branch, `git submodule update` after switching, and verification
  in a clean worktree.
- **The P0b split had hidden dependencies:** litestl PR #3 overlaps or already does much
  of B and C1; C1 also touches the TS runtime (positional `BindingInfo` reads) and the
  generators; A, B and C1 share files; A and D overlap through `matrix.h`. Re-split
  around #3, with the decision on stacking left to the user.
- **The PR gate "passes on clang-cl and emcc" is impossible at `8a98264`:** clang-cl
  fails to link attr tests (fixed in #3), and emcc fails on any bound method (PR B).
- **PR A's contents:** the min/max list was incomplete; the `ensure` rename would break
  ubertopo; the Eigen move is an API break that interacts with D. All adjusted.

### Still unverified
- HandlePool and Signal semantics: they're specified above, not yet built.
- Whether #3's owned-return handling covers the TS return-buffer leak.
- C4458 in `binding_struct.h`; it probably needs an instantiation to appear.
- The `EM_JS` dispatcher's fit with #3's backend abstraction.
