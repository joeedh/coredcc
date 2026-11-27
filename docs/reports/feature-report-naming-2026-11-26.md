# Feature report: the naming pass, 2026-11-26

- **Reviewed:** the naming pass at `d8bcb45`, the 11 commits from `4052d7c`. The reviewer checked them and the
  whole script- and host-facing surface (public headers, facades, bound names, command ids, ApiStructs, type
  names, the TS layer) against `naming.md`.
- **Reviewer:** a fresh agent given the code and the rules, not the author's reasoning. It only read the code;
  it built and ran nothing.
- **This file:** a rewrite of its review. Each finding ends with its resolution. The bugs come first, then the
  names that broke a rule, then the rules that were wrong, then what would be hard to change after P6.
- **Fixes:** in nine commits after `d8bcb45` (`5d11289` to `eb2837f`), named with each finding. What is deferred is tracked in the
  plan's Outcome (`plans/2026-11-26-naming-pass.md`).

## Summary
- **What held up:**
  - every TS `cd.<name>` call and every registered `coredcc::<name>` names a real facade;
  - no removed id, facade or TS export is left anywhere, the Unreal spike included;
  - no facade rename changed which overload a call picks;
  - the bound enums are complete, and the TS string tables are keyed by the generated enums.
- **What didn't:**
  - the type-alias table, which let a block load with two copies of one attachment or slot;
  - words the pass left behind, mostly "tool" for a command button;
  - about 60 names that broke the rules, from 5 facades to 20 command constants;
  - rules that contradicted each other or the code;
  - command and tool ids, which had no way to be renamed without dropping what users saved.

## Bugs
1. **An aliased name could give a block the same attachment or slot twice.** The parser refuses a block that
   holds two attachments of one type, or two slots with one (namespace, key). But it compared the names as
   stored. The alias table rewrites old names to current ones afterwards, so a block that stored both `old.att`
   and `new.att` ended up with two attachments of one type. That breaks a Library invariant, and a later lookup
   found whichever came first. *Fixed* (`5d11289`):
   - The load checks both again after the rewrite, and refuses such a block.
   - *Tested:* `test_lib_type_aliases` (`testAttachmentTwice`, `testSlotTwice`).
2. **The library kinds of alias had no test.** Block, attachment and slot aliases were wired up, but only node,
   socket and transform aliases were tested. *Fixed* (`5d11289`): `test_lib_type_aliases` covers load, save
   and append for all three, and the duplicate cases.
3. **The alias table's other checks.**
   - **An alias whose old name is still registered** made every stored block of that type load as the other
     type, silently. *Fixed* (`5d11289`): `checkTypeAliases` refuses to load while an old name is registered.
     The check is at load, not at registration, because the types are registered by other modules, in any
     order. *Tested:* `testOldNameRegistered`.
   - **The id-collision check didn't see current names.** It ran on the file's name table, which holds the old
     texts. *Fixed* (`5d11289`): it also checks the current names that aliases bring in. *Tested:*
     `testCurrentNameClash`.
   - **The file's name table keeps the old texts.** *Rejected:* saving writes only the names in use, so the old
     text is never written back.
   - The reviewer found no lifetime problem and no way to make a chain.
4. **The layout's `<tool>` to `<command>` change.**
   - **The JSON `kind` values changed, from `tool` to `command`, with `kLayoutVersion` still 1.** A frontend
     that switches on `kind` would stop drawing buttons without an error. *Fixed* (`f733c0b`), without a
     version bump: no frontend outside the tree exists. `embedding.md` says the kinds changed in the naming
     pass.
   - **`<tool>` and `<toolOptions>` are errors even inside a `custom` element,** so a host can't use those
     names for its own children. *Fixed* by documenting it, in `naming.md` and `embedding.md` (`f733c0b`).
     Limiting the error to the top level was rejected: an old layout that nests a `<tool>` in a custom
     element should fail as loudly as one that doesn't.
5. **Stale words and names.** *Fixed* (`f733c0b`):
   - `embedding.md` named `parseLayout` twice, and called a `<command>` element a tool. So did `bind/ts/run.ts`,
     the test app's panel and layout, and several tests.
   - The pick code's `detail::Occluder::depth` held a view distance. It is `viewDistance`.
   - `DrawListReader::items()` returned DrawRecords. It is `records()`.
   - Comments in `drive/driver_list.h` and its test named the drivers' old commands.
   - The errors of `runAt`, `canRunAt`, `canInvokeAt`, `invocationChecksRun` and `beginInteractiveAt` named the
     TS function. They now name the facade.
   - `script::invokeAt` called `invoke` unqualified, and got the facade only because it was the better
     overload. It now calls `script::invoke`.

## Names that broke a rule
6. **Five facades.** *Fixed* (`f1a3831`):
   - `transformEnumChannel` lacked its def's prefix: it is `transformTypeDefEnumChannel`.
   - `valueToString` is `valueText`.
   - The constructors `vec3` and `statusError` are `newVec3` and `newErrorStatus`. (`makeVec3` was taken by the
     PropValue maker.)
   - `commandSetOutput`'s only subject was the Context: it is `setCommandOutput`.
   - `bytesSize` returns a number of bytes: it is `bytesByteSize`, subject first.
7. **Seven TS functions.** *Fixed* (`f1a3831`): `input` and `output` are `commandInput` and `setCommandOutput`;
   `formatNumber` and `parseNumber` are `formatUnit` and `parseUnit`, like their facades; `canInvoke` is
   `canInvokeAt`; `interactive` is `interactiveId`; and `blockIds` is `blockIdText`. The node context's
   `output`, `progress` and `table` are `setOutput`, `reportProgress` and `inputTable`.
8. **Twenty command constants didn't follow `k<Verb><Noun>Command`.** Some dropped the noun (`kAttachCommand`,
   `kSetParentCommand`), some had the wrong one (`kAddDriverGraphCommand`), and some a plural one
   (`kDeleteObjectsCommand`). *Fixed* (`ce77ac0`): every constant now has its id's noun. `core.object.add`
   makes a block, so it is `core.object.new`.
9. **Counts, bytes and iteration.** *Fixed* (`cffe6a5`), except as noted:
   - Five classes that aren't containers had a bare `count()`: `Suggester`, `graph::Frame`, `ApplyDelta`,
     `Skips` and `PendingWrites`. They now say what they count (`candidateCount`, `objectCount`, ...).
     `ObjectChanges::size()` is `objectCount()` too (`eb2837f`).
   - `TransformBatch` and `TransformRegistry` are containers, so they have `size()`, as the other registries do.
   - `BytesReader::count` is kept: it reads a count from the bytes, so it is a verb. *Rejected.*
   - `memSize` is `byteSize` everywhere, the Command slot and `BlockProvider` included.
   - `LinearHistory::memoryUsed` and the `kMax*Length` limits are kept. *Rejected:* `memoryUsed` pairs with
     `setMemoryBudget`, and a length limit is on text.
   - `SyncKey::size`, `scannedSize` and `IoIssues::total` are kept. *Rejected:* they are fields of internal
     keys and tallies, and `total` reads clearly beside `first`.
   - `visit`, `visitRefs` and `each`/`eachIn` are `forEachRef`, `forEachRefWith` and `forEach`/`forEachIn`.
10. **`ElementDomain` reached scripts as a bare int, with -1 as a sentinel.** *Fixed* (`b07f673`): it is bound
    as an enum, with `None` for an object's bounds, and every script-facing use is typed. A mask of domains
    stays an integer.
11. **The per-transform-type channel structs were named by the bare type name** (`core.transform.trs`).
    *Fixed* (`b07f673`): they are `coredcc.TransformChannels:<type>`. No file stores the struct's name, only
    the transform type's, so no alias was needed.
12. **The reserved namespaces were compared literally.** `Core.x`, `SCENE.x`, and a bare `core` or `graph`
    all registered. The same was true of the `core.` check for commands, tools, brushes and transform types.
    *Fixed* (`b07f673`): the check ignores case and refuses a bare namespace.
    - *Tested:* `test_bind_graph`, `test_bind_scene`, `test_bind_tool` and `test_bind_review`.
13. **Smaller things the reviewer noted.**
    - `Object::solveCtx()` is `solveContext()` (`a6c690e`).
    - `CommandFlags` is a bit mask bound as an enum. *Rejected:* the rule was wrong (finding 17).
    - `coredcc.ToolSettingsCheck:<tool>#<hash>` had a suffix no rule described. *Fixed* by documenting it.
    - The test app's `TestBox`, `TestApp` and `TestItem` have no namespace. *Rejected:* they belong to a test
      app, not to the library.
    - `ClipDepth` crossed as an int. *Fixed* once litestl PR #17 bound it: `View::depthRange` is a bound member.
    - `SceneResults.status` crosses as a `number[]`, and `PropKind`, `PropSubtype`, `UnitFamily` and
      `LayoutSeverity` as strings. *Deferred* to P6, which binds the API.
    - The TS string unions `DepthMode`, `ShapeKind` and `ModalResult` share names with the generated enums,
      so code imports the latter under an alias. *Deferred* to P6.

## Rules that were wrong
All *Fixed* in `naming.md` (`a6c690e`), with the renames they then called for.

14. **"A TS name is the C++ name" contradicted "TS actions are verb first".** Six TS functions, such as
    `addGraphNode` over the facade `graphAddNode`, broke one rule or the other. The rules now say that TS
    types take the C++ name and TS functions are verb first. So `addGraphNode` and `readDrawList` are right,
    and TS's `NodeContext` is `NodeExecContext`.
15. **"Subject first" ignored the Context,** which many facades take before their subject. The Context is
    never the subject now: the subject is the first argument after it, and a path that names a block counts.
16. **Exceptions the audit accepted but the rules didn't list:** the `make*` and `read*` PropValue families,
    the TS short forms (`get`, `set`, `write`, `resolve`, `meta`, `format`, `parse`), and `define<Thing>` as
    the TS face of `registerForeign<Thing>`. They are listed now.
17. **"Bit masks stay int32 constants" contradicted the code,** where seven masks are flag `enum class`es.
    Flag enums are allowed now, and may be bound as bit-mask enums.
18. **"`depth` is NDC only" was stricter than the code,** which has many nesting depths. `depth` alone now
    means NDC depth in view, draw and pick code. A nesting depth is named for what nests, so:
    - `LayoutLimits::kMaxDepth` is `kMaxElementDepth`;
    - `GraphEvaluator`'s and the drivers' `kMaxDepth` is `kMaxGraphDepth`;
    - `SceneMember::depth` and `Ancestry::Place::depth` are `parentDepth`;
    - `graph::Frame::depth`, `emitDepth`, `callDepth` and `kMaxSahDepth` already said what nests.
19. **The `distance` rule named three hit types but not `NearestHit` and `NearHit`.** It is now limited to the
    three, and says what the others measure.
20. **Kinds of name no rule covered.** The rules now cover:
    - Def vs Info: a def is built by a client and kept by a registry; an info describes a registered type;
    - id vs name: the text is the id a user writes, stored as `name()`, and `id()` is its hash;
    - booleans (`has`, `can`, `is`, or a bare adjective);
    - removal verbs (`delete`, `remove`, `unlink`);
    - US spelling: `centre` is `center` throughout, and a few comments changed too;
    - tool ids (`core.tool.<name>`), the `#impl` and `#kernel` suffixes, and which name an error uses;
    - registration with `add`: `ClientRegistry::registerClient` is `add` (`eb2837f`).
    - *Rejected:* `getValue` and `getMeta` keep their verb, since `value(ctx, path)` would be unclear.
      `lookUp` is a verb and `lookup` a noun, so both are right. `validateAll` checks every registry, so it
      isn't `validate`.

## Hard to change after P6
21. **Command and tool ids had no aliases.** Last-used lines and tool settings store ids as text, and dropped an
    unknown one silently. Layouts and host keymaps hold them too. *Fixed* (`21053cf`):
    - `CommandRegistry::addAlias` and `ToolRegistry::addAlias` make `find` take an old id for the current one.
      Invocations, layouts, last-used lines, tool settings and `toolSettings[...]` paths all go through `find`,
      and saving writes the current id.
    - As with type aliases: no chains, an old id that is registered is refused, and an old id can't be
      registered later.
    - *Tested:* `test_command_dynamic` and `test_tool_settings` (`test_aliases`).
    - *Deferred* to P6: reporting a dropped last-used line instead of skipping it.
22. **The layout JSON's version.** See finding 4.
23. **The names P6 would freeze.** All fixed above, except `ToolEvent`, which stays because DOM has a global
    `InputEvent`. `naming.md` records the exception.
24. **The registered names carry `coredcc::`, while the functions live in `coredcc::script`.** *Deferred* to P6:
    how that prefix maps to N-API export names.
25. **The case-sensitive namespace check.** See finding 12.
26. **An alias of a name that is still registered.** See finding 3.

## Checks
- Each commit passed the pre-commit gate (169 tests). Each commit that touched TS or bindings also passed
  `wasm-st-debug` (170 tests) and the test app's typecheck.
- The Unreal spike passed, 11 of 11, at `eb2837f`, the last code commit.
- No old name is left outside plans and reports.
