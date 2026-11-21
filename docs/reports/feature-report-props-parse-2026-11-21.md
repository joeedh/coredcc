# Feature report: properties and parsers (`props/`, `parse/`), 2026-11-21

- **Reviewed:** the API surface of `props/` and `parse/` and the base additions they use
  (`text`, `flags`, `block_id`, `Runtime::ext`), as committed at `be69f0d`. Reviewed
  against architecture §4–§5 and the P1 plan's P1a, P1b and implementation notes.
- **Reviewer:** a fresh agent given the code and the spec, not the author's reasoning.
  It proved its claims with scratch programs against the Debug libraries.
- **This file:** a rewrite of its review. Each finding ends with its resolution.

## Summary
- **What works:**
  - The parsers keep error offsets in `0..len`, always reject int64 overflow and NUL,
    and round-trip canonical forms; mutation fuzzing reaches valid inputs.
  - `PathPtr` stays valid after eviction, with correct refcounts.
  - Float→Int has the right 2⁶³ boundary.
  - Extension destruction order is correct.
- **What didn't:**
  - `coerce` let NaN through.
  - Several builder and validation edge cases.
  - Extension name collisions.
  - Hash collisions by construction.
  - `PropValue` was 544 bytes.

## Bugs
1. **NaN got past the hard-range clamp.** `coerce(floatProp.range(0,1), NaN)` returned NaN,
   and vectors kept NaN components.
   - *Fixed:* NaN is OutOfRange for every numeric kind. Infinity clamps to a finite bound.
2. **Two extension types sharing a `kExtName` aliased each other's memory.**
   - *Fixed:* each slot records a per-type tag. It is the address of a writable static,
     so identical-code folding can't merge two of them. A mismatch fails a `CDCC_CHECK`.
   - An extension constructor that asks for its own type now fails a check instead of
     recursing.
3. **An explicit Enum default was overwritten by the first `item()`.**
   - *Fixed:* `item()` sets the default only if none was given.
4. **`KeySegment::ofHandle` took negative handles**, which the path grammar can't express.
   - *Fixed:* handles are non-negative (checked). `canonicalize` (P1d) must produce keys
     in that range.
5. **NUL silently truncated names and keys** outside `ofString`.
   - *Fixed:* one helper, `lsString`, checks for NUL. Every string built from a view goes
     through it: keys, API names, item keys, path segments.
6. **Wrong-type defaults were reported as "out of range".**
   - *Fixed:* validate checks the type first ("the default value is a Int, not a
     Float").
7. **Int clamping compared in double**, so `range(0, 2^53)` let `2^53+1` through. An Int
   range holding no integer was accepted.
   - *Fixed:* clamping compares in int64, and validate rejects a range with no integer.
     validate also rejects `step < 0` and `maxLength < 0`.
8. **The null Pointer had several spellings that compared unequal.**
   - *Fixed:* a null `OwnerKey` includes a Block owner with a null id.
     `PropValue::ofPointer` stores every null ref as the one empty `StableRef`, and
     `StableRef`'s `==` and hash treat all nulls alike.
9. **Hashes collided by construction**: `a["b\x04c"]` and `a["b"][c]`.
   - *Fixed:* `Path` and `StableRef` hash each text's length before it.
10. **Suggester noise:** the empty Factor symbol was suggested, and every
    one-character candidate matched a one-character name.
    - *Fixed:* empty candidates are skipped, and names under three characters get no
      suggestions.
11. **Minor:**
    - `approxEqual(NaN, NaN)` was false while `==` was true. *Fixed:* it is now
      reflexive.
    - NaN printed as `-nan(ind)` under MSVC. *Fixed:* `nan`, `inf` and `-inf` on every
      platform.
    - `"1e-400 m"` is "out of range" rather than 0. *Kept:* a silent zero is worse.
    - `"1,5"` reads as an unknown unit. *Kept:* decimal commas belong to a locale-aware
      UI layer.

## Design
- **D1, object sizes.** `PropValue` was 544 bytes; `StableRef` 352; `PropertyDef` 1808.
  - *Fixed for `PropValue`:* String, Pointer and Mat4 payloads moved out of line.
    `sizeof(PropValue) <= 48` is a static assert. Scalars and vectors never allocate.
  - *Reduced:* `StableRef` stores two segments inline, and `EnumMeta` stores one item
    inline. litestl `Vector` can't have zero inline slots.
  - *Deferred to P1d:* `PropertyDef`'s size, and `resolved()` copying it on each read.
    `getMeta` should return the def itself when it isn't dynamic.
- **D2, `coerce` ignored dynamic hooks.**
  - *Fixed:* `coerce` refuses a dynamic def (FailedPrecondition: resolve it first).
  - `PropertyBag` resolves its dynamic defs with the bag as the owner, so one input's
    range can follow another's value.
  - `validate()` checks the static def and never calls a hook, so hooks never see a null
    owner.
- **D3, the hook has no `Context`.**
  - *Deferred to P1d:* `props` can't name `Context` (it lives in `api/`).
    `ApiMember` will carry a Context-aware hook that produces the patched
    `PropertyDef`.
  - TS-defined dynamic properties come with it.
- **D4, JS numbers.**
  - *Fixed:* Enum, Flags and Bool accept a Float holding an integer, since JS numbers
    arrive as Float. A Flags item may be bit 63: the bit test is done in uint64.
  - *Deferred to P1f:* BigInt for int64 and uint64.
- **D5, no value literal that round-trips.**
  - *Deferred to P1e:* `formatLiteral(def, value)` and its parse, using enum keys,
    `"X|Z"` flags and Pointers as quoted canonical paths. LastUsedStore, command
    logging and invoke-from-text need it.
  - `PropValue::toString` is documented as a debug form.
- **D6, path segments had no source offset.** *Fixed:* `PathSegment::offset` (uint32),
  the position of the segment's `.` or `[`. It is not part of equality.
- **D7, signatures the litestl binders can't express:** `std::string` returns,
  `std::string_view` parameters, `std::optional`, getter/setter overloads that share a
  name, and aborting misuse.
  - *Fixed:* the `size_t` member became `uint32_t` (`InvocationArg::offset`). Inputs are
    capped at `kMaxPathLength` (1 MiB) so 32-bit offsets hold.
  - *Deferred to P1f:* `bind/` gets binding-shaped facades that use litestl strings,
    Result-returning variants and distinct getter and setter names. The C++ API stays as
    it is.
- **D8, naming.**
  - *Fixed:*
    - `componentCount(PropValue::Type)` is now `valueComponentCount`, because it
      differs from `componentCount(PropKind)` for scalars;
    - `PropValue::hash()` is removed, leaving `computeHash()` as on every other type;
    - the kind name functions moved to `kind.cc`;
    - `access()` documents that it adds flags.
  - *Kept:*
    - `wasSet(i)` next to `isSet(name)`;
    - the metadata accessor names;
    - `HandleKey` next to `KeySegment::Handle`;
    - the small duplicated character helpers.
- **D9, `StableRef` had no type, and runtime owners are bare hashes.**
  - *Fixed:* `StableRef::type` (a `StructId`). It is part of `==` and the hash, and
    `coerce(Pointer)` checks it against `TargetMeta` when both are known.
  - *Deferred to P1d:* a registry of runtime owners, with collision checks and names.
- **D10, `PropertyBag`.**
  - *Fixed:*
    - `reset` returns NotFound for an unknown name;
    - `setAt(index)` exists;
    - setting None unsets an Optional;
    - pointer invalidation by `add()` is documented.
- **D11, PathCache eviction scanned every entry.**
  - *Fixed:* an intrusive LRU list makes eviction and touching O(1). `validate()` walks
    the list.
  - *Kept:* `rt.ext<T>()` is a short linear scan; callers that need speed keep the
    reference.
  - *Deferred:* a hook to test the hash-collision branch.
- **D12, units.**
  - *Fixed:* `μm` with the Greek letter is accepted.
  - *Deferred to P1d:* where the display unit lives (scene and preferences settings).
  - *Deferred to P1d:* a check that `subtype` and `unit` agree.
  - *Deferred to P3:* frames, which need the scene's frame rate.
  - *Kept:* case-sensitive symbols, because `M` and `m` (mega and milli) differ.
- **D13, smaller items.**
  - *Fixed:* `kIdDomainCount` is static-asserted; `uiName("")` re-derives the name.
  - *Kept:*
    - `findExt` is const and returns a mutable pointer, like a lookup;
    - `uiNameFromApiName("EDGE_LOOP")` gives `EDGE LOOP` (enum items can set a UI name);
    - a Pointer with no target struct is allowed, meaning "any struct".

## Test gaps
- *Added:*
  - `compile_embed` includes every new header and instantiates a Library, the
    PathCache and `coerce`;
  - NaN and infinity through ranged coerce;
  - builder order;
  - null-pointer normalization;
  - StableRef owner kinds, the type field, segment kinds and hash boundaries;
  - Int clamping above 2⁵³, empty Int ranges, step and maxLength;
  - integral Floats for Enum, Flags and Bool;
  - flag bit 63;
  - Pointer target types;
  - dynamic defs in coerce and in a bag;
  - Optional set to None, reset of an unknown name, `setAt`;
  - LRU order and `setCapacity(0)`;
  - segment offsets;
  - path hash boundaries;
  - short-name suggestions;
  - Mat4 and String copy, move and self-assignment;
  - NaN and infinity printing.
- *Not added (they abort, so they can't be tested in-process):* an extension name
  collision, and NUL in a key or name.
- *Deferred:*
  - fuzzing `units::parse`;
  - direct tests of `flags.h`'s operators and `BlockId` ordering;
  - the PathCache collision branch.

## Spec deviations
- **Recorded in the plan's implementation notes, and justified:**
  - `ParseError` became `Result` plus `errorOffset`;
  - `parse` is its own module;
  - there is no `VectorMeta`;
  - `TargetMeta` was added.
- **`dynamic` takes `(PropertyDef&, const void *owner)`, not `(PointerRef, Context&)`.**
  - It was not recorded before; it is now (D3).
- **NumericMeta has one unit family, not separate base and display units.**
  - Display units are deferred to P1d (D12).
- **Segments are `{kind, text, index, offset}`, not `{kind, name, index, key}`.**
  - The change is harmless: name and key share `text`.
- **`OwnerKey` has no cached `BlockHandle`.**
  - `props` sits below `lib`, so the resolver keeps handles in `PointerRef` (P1d).
