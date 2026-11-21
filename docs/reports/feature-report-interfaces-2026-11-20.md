# Feature report: interfaces (`base/iface`), 2026-11-20

- **Reviewed:** the API surface of the interface system as committed on `spine` (`ce7b7da`),
  against architecture §2a and the P0 plan.
- **Reviewer:** a fresh agent given the code and the spec, not the author's reasoning.
- **Evidence:** it proved 11 findings with small programs (scratch `review/exp.cc`, plus a
  two-DLL experiment).
- **This file:** a rewrite of its review. Each finding ends with its resolution.

## Summary
- **What works:**
  - `CDCC_CALL`'s typing comes from the forwarder's own member pointer.
  - Exact type matching uses kind, const, size, typeKey and name.
  - The registration error messages are useful.
  - The self-reference cache works.
- **Where it fails:**
  - inheritance;
  - the implementation-descriptor cache;
  - unchecked object types in `own`/`query`;
  - type identity for non-struct kinds and across DLLs;
  - the foreign error path.
- Most of these failures compile cleanly and then corrupt memory or return wrong
  values.

## Bugs
1. **The concatenated base of an `_EXTENDS` interface was neither registered nor
   queryable.** `registerImpl<Volume, Box>` stored no `Spatial` table, so
   `query<Spatial>` missed every `Volume` implementation.
   - *Fixed:* see "Related interfaces" in the resolution notes.
2. **`CDCC_INTERFACE` on a derived interface (forgetting `_EXTENDS`) compiled and then
   called the wrong slot.** In the experiment, `count()` returned `volume`'s float bits,
   and `cost()` read past the thunk table.
   - *Fixed (debug):* `CDCC_SLOT` also caches the slot's `Method*`. `FwdCall` asserts
     that the table's slot is the same descriptor, and that the index is in bounds. A
     compile-time check isn't possible without reading class bases.
3. **A `T&&` implementation matched a `const T&` slot for value-like types (`string`,
   `span`)**, and moved out of the caller's const object. litestl binds both as a plain
   `T`.
   - *Fixed in litestl #6:* `MethodParam::isRvalueRef`. The matcher compares it.
4. **Enums (and other non-struct kinds) matched by name only**, so a `uint8_t` enum and
   an `int64_t` enum with the same name were accepted.
   - *Fixed:* `sameType` compares `getSize()` for every kind.
   - Same-size enums with different values still match. They are ABI-safe, but a typeKey
     on `Enum` would catch them. Tracked.
5. **The implementation-descriptor cache was keyed only by name.** As a result:
   - a second C++ type under the same name reused the first type's thunks;
   - a second adaptor `bindFn` was ignored;
   - a failed registration poisoned the name.
   - *Fixed:* a cache hit must have the same typeKey (otherwise AlreadyExists), and a
     failed registration isn't cached.
   - Adaptors: one `bindFn` per adaptor name, covering every interface it implements
     (documented).
6. **`own<I, Impl>` didn't check that `Impl` is the registered type.**
   - *Fixed:* `own` checks `as<Impl>()`.
   - The typed `query<I>(rt, type, T *self)` overload debug-asserts the same, for
     native tables.
7. **Calling an absent optional slot without `CDCC_HAS` called a null pointer.**
   - *Fixed:* `FwdCall` asserts `hasSlot`.
8. **A foreign call couldn't report failure.** The forwarder then moved from, and
   destroyed, an unconstructed return buffer.
   - *Fixed:* `ForeignDispatcher::call` returns a `Status`. On error the forwarder
     returns a value-initialized `R` and sets the Runtime's sticky error
     (`Runtime::takeStickyError()`). A slot with a non-default-constructible return
     can't be called on a foreign table.

## Design
- **Type identity is per-DLL.** `typeKeyOf<T>`, the `defineBindings()` statics and the
  `CDCC_INTERFACE` cache are per image. An Unreal plugin DLL implementing a coredcc
  interface would build a second descriptor.
  - *Documented* in `docs/embedding.md`: declarations and implementations live in the
    coredcc module image.
  - Stable, name-based keys are tracked for the Unreal spike (P4).
- **`Owned<I>` only gave const access**, so non-const slots such as `Command::exec`
  couldn't be called through it.
  - *Fixed:* non-const `get()` and `operator->`.
- **`addImpl`/`addForeign` didn't register type names.** Only the templates did, and
  N-API and WASM hosts will call `addForeign` directly.
  - *Fixed:* the registry registers the name.
- **Foreign tables ignored `CDCC_UPCASTS`. `upcast<>` didn't reach the concatenated
  base, and upcasts weren't transitive.**
  - *Fixed:* see "Related interfaces" in the resolution notes.
- **A failed registration left state behind.**
  - *Fixed* for the descriptor cache.
  - Interface and type-name registrations persist by design: they are valid on their own.
- **`validateAll()` couldn't fail.** It rebuilt tables from immutable descriptors.
  - *Fixed:* it now checks real invariants:
    - the key map agrees with the table list;
    - names are registered;
    - thunk counts match slot counts;
    - related tables are stored.
  - `buildNative` is a free function, so there's no `const_cast`.
- **Native tables couldn't be marked `ThreadAffine`.**
  - *Fixed:* `registerImpl`/`registerAdaptor` take `IfaceFlags`.
- **A forwarder whose slot was never bound aborts on its first call.**
  - *Documented.*
  - The tests call every forwarder they declare. A real fix needs the constexpr binding
    redesign.
- **Slot names `table`, `self` and `as` collided with `IfaceRef` members.**
  - *Fixed:* `CDCC_HAS` qualifies `IfaceRef::table()`, and registration rejects the three
    names.
- **Interface descriptors had no "copy" constructor**, which litestl's TS runtime needs
  to pass an interface by value.
  - *Fixed.*
- **Naming:**
  - `implementForeign` is renamed `registerForeign`.
  - The registry's `add*` methods stay as the low-level layer under the `register*`
    templates.
  - §3's `Iface<I>` was never built; `IfaceRef` plus derived interfaces cover it.
- **The `Runtime` doc comment had moved onto a forward declaration.**
  - *Fixed.*
- **The primitive out-parameter check flagged `const int*`, and missed `int**`.**
  - *Fixed:* only non-const references and pointers to primitives are rejected, and
    pointer-to-pointer is rejected.
- **By-value parameters were copied an extra time inside `FwdCall`.**
  - *Fixed:* by-value parameters are passed through as `const P&`.
- **`BIND_STRUCT_MEMBER` names `binding::Bind` unqualified.**
  - *Fixed in litestl #4.*

## Test gaps (all added unless noted)
- **Declaration-rule rejections:**
  - an overloaded slot;
  - a reference return;
  - a primitive out-parameter, and a `const int*`, which is accepted;
  - a reserved slot name;
  - two C++ interfaces with one name.
- **Non-trivial returns:** struct and `string` returns, natively and through the fake
  dispatcher, including a dispatcher failure.
- **Bases:** `query<Base>` on an `_EXTENDS` implementation, and three-level `_EXTENDS`.
- **Wrong types:**
  - the enum impostor;
  - an rvalue-reference implementation;
  - a second C++ type under an existing name;
  - `own` with a mismatched `Impl`.
- **Optional slots:** an optional slot present with the wrong signature, and an optional
  slot present on a foreign table.
- **Rollback and foreign tables:**
  - rollback when an upcast base isn't satisfied;
  - a foreign `Volume` with its bases and upcasts;
  - `addForeign` called directly.
- **`Owned` and Runtimes:**
  - `Owned` move-assign, reset and self-move, and a mutating slot through `Owned`;
  - two Runtimes registering the same implementation.
- **Compile-fail and embedding:**
  - a compile-fail test for a non-convertible argument type;
  - `compile_embed` defines `TEXT` as Unreal does, and instantiates `CDCC_INTERFACE`,
    `registerImpl` and `query`.
- **Not added:**
  - The forgotten-`_EXTENDS` assert aborts the process, so it isn't unit-testable
    in-process.
  - A negative `validateAll` test would need a corruption hook.
  - There is no pointer-to-pointer test: litestl can't bind `T**` (it has no
    `Binder<T*>`), so the rule only guards against a future binder.
  - All three are tracked.

## Spec deviations
- **`as<T>()` compares typeKey, not `implType`.** Justified: it works without a name for
  `T`. It shares the per-DLL limitation.
- **The syntax differs** (`registerImpl<I, Impl>(rt, name)`, `CDCC_INTERFACE_EXTENDS`).
  The forms are equivalent.
- **Declaration rules not checked:** function-pointer, `function_ref`, `Span` and template
  slots. Static methods in an interface are now rejected.
- **Marshal closures** (precompiled when a foreign table is built) belong to C2. The
  dispatcher gets the table, which can carry them.
- **`validateAll()` at startup** is the host's call. Every test that builds a Runtime
  calls it.
