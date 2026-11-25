/**
 * The P1f TS spike (docs/plans/2026-11-20-p1-application-spine.md, exit criteria): the
 * wasm-st module, through litestl's TS runtime and coredcc's TS layer,
 * 1. resolves a path;
 * 2. gets and sets a value;
 * 3. undoes the set;
 * 4. runs a Command implemented in TS through the executor, including undo.
 * The rest checks the TS layer's rules, including the regressions from
 * docs/reports/feature-report-ts-spike-2026-11-22.md (named by finding). run.mjs builds
 * and runs it.
 */
import assert from 'node:assert/strict'
import {afterEach, beforeEach, describe, test} from 'node:test'
import {pathToFileURL} from 'node:url'
import {setFlagsFromString} from 'node:v8'
import {runInNewContext} from 'node:vm'

import * as cdcc from 'coredcc-script'
import {Ref, CommandFlags, ScriptError, Session, StatusCode, type Context, type ScriptCommand} from 'coredcc-script'
import type {Host} from 'coredcc-gen/testapp/Host'
import * as spike from 'coredcc-gen/testapp/functions'

const create = (await import(pathToFileURL(process.env.SPIKE_MODULE as string).href)).default
const module = await create()
const backend = cdcc.load(module, 'TESTAPP_GetManager')

setFlagsFromString('--expose-gc')
const gc = runInNewContext('gc') as () => void

/** A ScriptError with `code` whose message matches `re`. */
const fails = (code: StatusCode, re: RegExp) => (e: unknown) =>
  e instanceof ScriptError && e.code === code && re.test(e.message)

const open = () => {
  const host = spike.create() as Host
  return new Session(host, spike.context(host) as Context)
}

const hostOf = (ctx: Context) => (ctx as unknown as {__host: Host}).__host

const validate = (ctx: Context) => {
  const st = spike.validate(hostOf(ctx))
  try {
    assert.ok(st.ok(), st.message())
  } finally {
    st[Symbol.dispose]()
  }
}

/* Adds `amount` to `count`, keeping what undo needs; writes `result`. */
class AddCount implements ScriptCommand {
  static made = 0
  before = 0
  constructor() {
    AddCount.made++
  }
  exec(ctx: Context) {
    this.before = cdcc.get(ctx, 'count') as number
    const after = this.before + (cdcc.input(ctx, 'amount') as number)
    cdcc.write(ctx, 'count', after)
    cdcc.output(ctx, 'result', after)
  }
  undo(ctx: Context) {
    cdcc.write(ctx, 'count', this.before)
  }
  memSize() {
    return 8
  }
}
const addSpec = {id: 'ts.add', inputs: {amount: 1}, outputs: {result: 0}}

describe('coredcc wasm-st spike', () => {
  let session: Session
  let ctx: Context
  const handlesBefore = backend.foreignCount()

  beforeEach(() => {
    session = open()
    ctx = session.ctx
  })
  afterEach(() => {
    validate(ctx)
    session[Symbol.dispose]()
    /* The history released every instance, and the Runtime every factory. */
    assert.equal(backend.foreignCount(), handlesBefore)
  })

  test('1. resolves a path', () => {
    cdcc.resolve(ctx, 'count')
    cdcc.resolve(ctx, 'strength')
    assert.throws(() => cdcc.resolve(ctx, 'cuont'), fails(StatusCode.NotFound, /did you mean.*count/))
  })

  test('2. gets and sets a value', () => {
    assert.equal(cdcc.get(ctx, 'strength'), 0.5)
    cdcc.set(ctx, 'strength', 0.75)
    assert.equal(cdcc.get(ctx, 'strength'), 0.75)
    assert.equal(spike.historySize(hostOf(ctx)), 1)
    /* Coerced and clamped to the member's def (range 0-1). */
    cdcc.set(ctx, 'strength', 4)
    assert.equal(cdcc.get(ctx, 'strength'), 1)
    assert.throws(() => cdcc.set(ctx, 'strength', 'x'), ScriptError)
  })

  test('resolve gives the kind of a path', () => {
    const kinds = {
      count: 'Int', strength: 'Float', label: 'String', mode: 'Enum', 'mode[EDIT]': 'EnumKey',
      flags: 'Flags', offset: 'Vec3', 'offset[1]': 'Component', items: 'Collection',
      'items.length': 'Length', 'items[0]': 'Struct', activeItem: 'Pointer',
    }
    for (const [path, kind] of Object.entries(kinds)) assert.equal(cdcc.resolve(ctx, path), kind, path)
  })

  test('pointers are frozen Refs, or null, and can be written', () => {
    assert.equal(cdcc.get(ctx, 'activeItem'), null)
    const cube = cdcc.get(ctx, 'items["Cube"]') as Ref
    assert.ok(cube instanceof Ref && Object.isFrozen(cube))
    assert.equal(cube.text, `@block(testapp, ${'0'.repeat(31)}1) : TestItem`)
    assert.equal(String(cube), cube.text)
    cdcc.set(ctx, 'activeItem', cube)
    assert.deepEqual(cdcc.get(ctx, 'activeItem'), cube)
    assert.equal(cdcc.get(ctx, 'activeItem.name'), 'Cube')
    cdcc.set(ctx, 'activeItem.other', new Ref(`@block(testapp, ${'0'.repeat(31)}2)`))
    assert.equal(cdcc.get(ctx, 'items["Cube"].other.name'), 'Ball')
    cdcc.set(ctx, 'activeItem', null)
    assert.equal(cdcc.get(ctx, 'activeItem'), null)
    cdcc.undo(ctx)
    assert.deepEqual(cdcc.get(ctx, 'activeItem'), cube)
    /* What a Ref names is checked when it is written. */
    assert.throws(() => cdcc.set(ctx, 'activeItem', new Ref('nope')), fails(StatusCode.InvalidArgument, /reference:/))
    assert.throws(
      () => cdcc.set(ctx, 'activeItem', new Ref(`@block(testapp, ${'0'.repeat(31)}9)`)),
      fails(StatusCode.NotFound, /./)
    )
    assert.throws(() => cdcc.set(ctx, 'activeItem', 3), ScriptError)
    assert.throws(() => cdcc.set(ctx, 'count', null), ScriptError)
    assert.throws(() => new Ref(3 as never), TypeError)
    /* Graph values have no context, and are never pointers. */
    assert.throws(() => cdcc.fromJs(undefined, new Ref('@')), TypeError)
  })

  test('pointers pass through a command', () => {
    const seen: unknown[] = []
    cdcc.registerCommand(
      ctx,
      {id: 'ts.pick', outputs: {label: ''}},
      class implements cdcc.ScriptCommand {
        exec(c: Context) {
          const ball = cdcc.get(c, 'items["Ball"]') as Ref
          cdcc.set(c, 'activeItem', ball)
          seen.push(ball)
          cdcc.output(c, 'label', ball.text)
        }
        undo() {}
      }
    )
    const out = cdcc.invoke(ctx, 'ts.pick')
    assert.deepEqual(cdcc.get(ctx, 'activeItem'), seen[0])
    assert.equal(out.label, (seen[0] as Ref).text)
    cdcc.undo(ctx)
    assert.equal(cdcc.get(ctx, 'activeItem'), null)
  })

  test('enums and flags by name, both ways', () => {
    assert.equal(cdcc.get(ctx, 'mode', {names: true}), 'OBJECT')
    cdcc.set(ctx, 'mode', 'EDIT')
    assert.equal(cdcc.get(ctx, 'mode', {names: true}), 'EDIT')
    assert.equal(cdcc.get(ctx, 'mode'), 1)
    assert.deepEqual(cdcc.get(ctx, 'flags', {names: true}), [])
    cdcc.set(ctx, 'flags', ['SELECT', 'HIDE'])
    assert.deepEqual(cdcc.get(ctx, 'flags', {names: true}), ['SELECT', 'HIDE'])
    cdcc.set(ctx, 'flags', ['HIDE'])
    assert.equal(cdcc.get(ctx, 'flags'), 2)
    cdcc.set(ctx, 'flags', [])
    assert.equal(cdcc.get(ctx, 'flags'), 0)
    assert.throws(() => cdcc.set(ctx, 'flags', ['HIDDEN']), ScriptError)
    assert.throws(() => cdcc.set(ctx, 'flags', ['SELECT|HIDE']), TypeError)
    assert.throws(() => cdcc.set(ctx, 'flags', ['']), TypeError)
    assert.throws(() => cdcc.set(ctx, 'label', []), ScriptError)
    /* Other values are as without names. */
    assert.equal(cdcc.get(ctx, 'strength', {names: true}), 0.5)
    assert.equal(cdcc.get(ctx, 'mode[EDIT]', {names: true}), true)
    assert.equal(cdcc.get(ctx, 'activeItem', {names: true}), null)
  })

  test('an Int past 2^53 is a bigint', () => {
    const big = 2n ** 60n + 1n
    cdcc.set(ctx, 'big', big)
    assert.equal(cdcc.get(ctx, 'big'), big)
    cdcc.set(ctx, 'big', -5)
    assert.equal(cdcc.get(ctx, 'big'), -5)
  })

  test('meta describes a path', () => {
    const m = cdcc.meta(ctx, 'strength')
    assert.equal(m.kind, 'Float')
    assert.equal(m.hardMax, 1)
    assert.equal(m.canonical, '@owner(testapp).strength')
    assert.deepEqual(
      cdcc.meta(ctx, 'mode').items?.map((i) => i.key),
      ['OBJECT', 'EDIT']
    )
    assert.equal(cdcc.meta(ctx, 'activeItem').target, 'TestItem')
    assert.deepEqual(cdcc.meta(ctx, 'items[0]').members, ['name', 'weight', 'other'])
    assert.throws(() => cdcc.meta(ctx, 'nope'), fails(StatusCode.NotFound, /nope/))
  })

  test('sets of one gesture are one undo step', () => {
    const drag = cdcc.beginGesture(ctx)
    for (const x of [0.6, 0.7, 0.8]) cdcc.set(ctx, 'strength', x, drag)
    assert.equal(spike.historySize(hostOf(ctx)), 1)
    cdcc.set(ctx, 'strength', 0.9, cdcc.beginGesture(ctx))
    assert.equal(spike.historySize(hostOf(ctx)), 2)
    cdcc.undo(ctx)
    cdcc.undo(ctx)
    assert.equal(cdcc.get(ctx, 'strength'), 0.5)
    assert.throws(() => cdcc.set(ctx, 'strength', 1, drag + 2), fails(StatusCode.InvalidArgument, /beginGesture/))
    const cancelled = cdcc.beginGesture(ctx)
    for (const x of [0.6, 0.7]) cdcc.set(ctx, 'strength', x, cancelled)
    cdcc.set(ctx, 'count', 4, cancelled)
    cdcc.cancelGesture(ctx, cancelled)
    assert.deepEqual([cdcc.get(ctx, 'strength'), cdcc.get(ctx, 'count')], [0.5, 0])
    assert.equal(cdcc.canRedo(ctx), false)
    for (const g of [0, -1, 1.5, NaN, 2 ** 60]) assert.throws(() => cdcc.set(ctx, 'strength', 1, g), TypeError)
  })

  test('layouts: nodes, issues, and a tool under a prefix', () => {
    const xml = `<layout path="items['Cube']">
  <prop path="weight" enabledIf="!/enabled"/>
  <tool invocation="ts.weigh(by=2)">Weigh</tool>
</layout>`
    const root = cdcc.parseLayout(xml)
    assert.equal(root.kind, 'layout')
    const [prop, tool] = root.children!
    assert.equal(prop.effectivePath, '/items["Cube"].weight')
    assert.equal(cdcc.get(ctx, prop.effectivePath), 1)
    assert.deepEqual(prop.enabledIf, {path: '/enabled', not: true})
    assert.equal(tool.label, 'Weigh')
    assert.equal(tool.prefix, '/items["Cube"]')
    assert.throws(() => cdcc.parseLayout(`<layout>
<x/></layout>`), fails(StatusCode.InvalidArgument, /layout 2:1/))

    const issues = cdcc.validateLayout(ctx, xml)
    assert.deepEqual(issues.map((i) => [i.severity, i.line, i.node, i.code]), [['Error', 3, 2, 'NotFound']])

    /* The command's own paths are under the prefix too, in its undo as well. */
    class Weigh implements ScriptCommand {
      before = 0
      exec(c: Context) {
        this.before = cdcc.get(c, 'weight') as number
        cdcc.write(c, 'weight', this.before + (cdcc.input(c, 'by') as number))
      }
      undo(c: Context) {
        cdcc.write(c, 'weight', this.before)
      }
    }
    cdcc.registerCommand(ctx, {id: 'ts.weigh', inputs: {by: 1}}, Weigh)
    assert.deepEqual(cdcc.validateLayout(ctx, xml), [])
    cdcc.invokeAt(ctx, tool.prefix!, tool.invocation!)
    assert.equal(cdcc.get(ctx, 'items["Cube"].weight'), 3)
    cdcc.undo(ctx)
    assert.equal(cdcc.get(ctx, 'items["Cube"].weight'), 1)
    /* Undo is where the run was, whatever the prefix names by then. */
    cdcc.write(ctx, 'activeItem', cdcc.get(ctx, 'items["Ball"]') as cdcc.Ref)
    cdcc.invokeAt(ctx, 'activeItem', 'ts.weigh(by=2)')
    cdcc.write(ctx, 'activeItem', cdcc.get(ctx, 'items["Cube"]') as cdcc.Ref)
    cdcc.write(ctx, 'items["Cube"].weight', 50)
    cdcc.undo(ctx)
    cdcc.redo(ctx)
    assert.equal(cdcc.get(ctx, 'items["Ball"].weight'), 3)
    cdcc.write(ctx, 'items["Ball"].name', 'Sphere')
    cdcc.undo(ctx)
    assert.equal(cdcc.get(ctx, 'items["Sphere"].weight'), 1)
    assert.equal(cdcc.get(ctx, 'items["Cube"].weight'), 50)
    cdcc.write(ctx, 'items["Sphere"].name', 'Ball')
    assert.throws(() => cdcc.invokeAt(ctx, 'items["Cone"]', tool.invocation!), ScriptError)
    assert.throws(() => cdcc.invokeAt(ctx, '', tool.invocation!), fails(StatusCode.NotFound, /weight/))
  })

  test('a string with a NUL is refused wherever it comes in', () => {
    const nul = 'a\0b'
    const refused = fails(StatusCode.InvalidArgument, /NUL/)
    class Nothing {
      exec() {}
      undo() {}
    }
    const calls: [string, () => unknown][] = [
      ['set', () => cdcc.set(ctx, 'label', nul)],
      ['an enum', () => cdcc.set(ctx, 'mode', nul)],
      ['flags', () => cdcc.set(ctx, 'flags', [nul])],
      ['write', () => cdcc.write(ctx, 'label', nul)],
      ['rename', () => cdcc.set(ctx, 'items["Cube"].name', nul)],
      ['a path', () => cdcc.get(ctx, nul)],
      ['a path to write', () => cdcc.set(ctx, nul, 1)],
      ['resolve', () => cdcc.resolve(ctx, nul)],
      ['meta', () => cdcc.meta(ctx, nul)],
      ['a reference', () => cdcc.set(ctx, 'activeItem', new cdcc.Ref(nul))],
      ['invoke', () => cdcc.invoke(ctx, nul)],
      ['invokeAt', () => cdcc.invokeAt(ctx, nul, 'ts.weigh()')],
      ['a layout', () => cdcc.parseLayout(nul)],
      ['a layout to check', () => cdcc.validateLayout(ctx, nul)],
      ['a reader', () => new cdcc.PathReader(ctx, [nul])],
      ['uiName', () => cdcc.registerCommand(ctx, {id: 'ts.nul', uiName: nul}, Nothing)],
      ['description', () => cdcc.registerCommand(ctx, {id: 'ts.nul', description: nul}, Nothing)],
      ['icon', () => cdcc.registerCommand(ctx, {id: 'ts.nul', icon: nul}, Nothing)],
      ['an input', () => cdcc.registerCommand(ctx, {id: 'ts.nul', inputs: {[nul]: 1}}, Nothing)],
      ['a default', () => cdcc.registerCommand(ctx, {id: 'ts.nul', inputs: {a: nul}}, Nothing)],
      ['an output', () => cdcc.registerCommand(ctx, {id: 'ts.nul', outputs: {[nul]: 1}}, Nothing)],
      ['a graph', () => cdcc.graphCreate(ctx, nul)],
      ['a node type', () => cdcc.defineNodeType(ctx, {id: 'ts.n', uiName: nul, outputs: {}, evaluate() {}})],
      ['a socket', () => cdcc.defineNodeType(ctx, {id: 'ts.n', outputs: {a: nul}, evaluate() {}})],
      ['a param', () => cdcc.defineNodeType(ctx, {id: 'ts.n', params: {[nul]: 1}, outputs: {}, evaluate() {}})],
    ]
    for (const [what, call] of calls) assert.throws(call, refused, what)
    /* An id is refused as one that isn't an identifier. */
    assert.throws(() => cdcc.registerCommand(ctx, {id: 'ts.' + nul}, Nothing), ScriptError)
    assert.equal(cdcc.get(ctx, 'label'), '')
    assert.equal(cdcc.get(ctx, 'items["Cube"].name'), 'Cube')
    /* The id wasn't taken by those. */
    cdcc.registerCommand(ctx, {id: 'ts.nul'}, Nothing)
  })

  test('a dynamic def: its keys, and watching what meta gives', () => {
    assert.equal(cdcc.meta(ctx, 'strength').dynamic, undefined)
    const focus = cdcc.meta(ctx, 'focus')
    assert.equal(focus.dynamic, true)
    assert.deepEqual(focus.items!.map((i) => i.key), ['NONE', 'Cube', 'Ball'])
    cdcc.set(ctx, 'focus[Ball]', true)
    assert.equal(cdcc.get(ctx, 'focus', {names: true}), 'Ball')
    assert.throws(() => cdcc.get(ctx, 'focus[Cone]'), fails(StatusCode.NotFound, /no key 'Cone'/))
    /* It can't be known without an instance. */
    const issues = cdcc.validateLayout(ctx, '<layout><prop path="focus[Cone]" visibleIf="focus[Cube]"/></layout>')
    assert.deepEqual(issues.map((i) => i.severity), ['Warning', 'Warning'])

    const reader = new cdcc.PathReader(ctx, ['level', 'focus', 'strength', 'focus[Ball]'])
    reader.watch()
    cdcc.set(ctx, 'count', 5)
    cdcc.set(ctx, 'items["Ball"].name', 'Sphere')
    cdcc.flushChanges(ctx)
    assert.deepEqual(reader.takeChangedMeta(), [0, 1, 3])
    assert.deepEqual(reader.takeChangedMeta(), [])
    assert.deepEqual(reader.takeChanged(), [3])
    assert.equal(cdcc.meta(ctx, 'level').hardMax, 5)
    assert.equal(reader.value(1, {names: true}), 'Sphere')
    reader[Symbol.dispose]()
  })

  test('PathReader reads, watches, and goes with its session', () => {
    const reader = new cdcc.PathReader(ctx, ['strength', 'mode', 'flags', 'items["Cone"].weight'])
    assert.equal(reader.count, 4)
    assert.throws(() => reader.value(0), fails(StatusCode.FailedPrecondition, /wasn't read/))
    reader.read()
    assert.equal(reader.value(0), 0.5)
    assert.equal(reader.value(1), 0)
    assert.equal(reader.value(1, {names: true}), 'OBJECT')
    assert.deepEqual(reader.value(2, {names: true}), [])
    assert.throws(() => reader.value(3), ScriptError)
    assert.throws(() => reader.value(4), fails(StatusCode.OutOfRange, /path 4/))
    assert.throws(() => reader.add('a..b'), ScriptError)
    assert.throws(() => new cdcc.PathReader(ctx, ['count', 'a..b']), ScriptError)

    reader.watch()
    assert.equal(spike.watched(hostOf(ctx)), 4)
    assert.equal(cdcc.flushChanges(ctx), 0)
    cdcc.set(ctx, 'strength', 0.25)
    cdcc.set(ctx, 'flags', ['HIDE'])
    assert.equal(reader.value(0), 0.5)
    assert.equal(cdcc.flushChanges(ctx), 2)
    assert.deepEqual(reader.takeChanged(), [0, 2])
    assert.deepEqual(reader.takeChanged(), [])
    assert.deepEqual(reader.value(2, {names: true}), ['HIDE'])
    /* A change made in C++. */
    spike.nudge(hostOf(ctx))
    assert.equal(cdcc.flushChanges(ctx), 1)
    assert.equal(reader.value(0), 0.375)
    reader.unwatch()
    assert.equal(spike.watched(hostOf(ctx)), 0)

    /* In a slot: reading, but not watching; a slot's context isn't a session's. */
    let inSlot: unknown[] = []
    class Slot implements ScriptCommand {
      exec(c: Context) {
        const tried = (fn: () => unknown) => {
          try {
            return fn()
          } catch (e) {
            return e instanceof ScriptError ? e.code : (e as Error).message
          }
        }
        inSlot = [
          tried(() => reader.read()),
          tried(() => reader.watch()),
          tried(() => cdcc.flushChanges(c)),
          tried(() => new cdcc.PathReader(c, ['count'])),
        ]
      }
      undo() {}
    }
    cdcc.registerCommand(ctx, {id: 'ts.slot'}, Slot)
    cdcc.invoke(ctx, 'ts.slot()')
    assert.deepEqual(inSlot, [undefined, StatusCode.Busy, StatusCode.Busy, "expected a session's context"])

    /* Disposed of by hand, or with the session, before its host. */
    const second = open()
    const other = new cdcc.PathReader(second.ctx, ['count'])
    other.watch()
    other[Symbol.dispose]()
    other[Symbol.dispose]()
    assert.equal(spike.watched(hostOf(second.ctx)), 0)
    assert.throws(() => other.read(), /disposed/)
    const late = new cdcc.PathReader(second.ctx, ['count'])
    late.watch()
    second[Symbol.dispose]()
    assert.throws(() => late.value(0), /disposed/)
    assert.throws(() => new cdcc.PathReader(second.ctx, ['count']), /disposed/)
  })

  test('saves, loads, appends, with reports', () => {
    cdcc.set(ctx, 'items["Cube"].weight', 7)
    cdcc.set(ctx, 'items["Cube"].other', cdcc.get(ctx, 'items["Ball"]') as Ref)
    const {bytes, report} = cdcc.saveLibrary(ctx)
    assert.ok(bytes instanceof Uint8Array && bytes.length > 100)
    assert.equal(report.blocks, 2)
    assert.equal(report.issues.total, 0)

    const host = spike.createEmpty() as Host
    const empty = new Session(host, spike.context(host) as Context)
    assert.equal(cdcc.get(empty.ctx, 'items.length'), 0)
    assert.throws(() => cdcc.loadLibrary(empty.ctx, bytes.subarray(0, 40)), ScriptError)
    assert.throws(() => cdcc.loadLibrary(empty.ctx, bytes, {maxBlocks: 1}), fails(StatusCode.OutOfRange, /./))
    assert.throws(() => cdcc.loadLibrary(empty.ctx, bytes, {maxBytes: -1}), TypeError)
    const loaded = cdcc.loadLibrary(empty.ctx, bytes, {sourceName: 'file', maxBytes: 1 << 20})
    assert.equal(loaded.records, 2)
    assert.deepEqual(loaded.blocks.first.map((b) => b.name), ['Cube', 'Ball'])
    assert.equal(cdcc.get(empty.ctx, 'items["Cube"].other.name'), 'Ball')
    assert.equal(cdcc.get(empty.ctx, 'items["Cube"].weight'), 7)
    assert.deepEqual(cdcc.saveLibrary(empty.ctx).bytes, bytes)
    assert.throws(() => cdcc.loadLibrary(empty.ctx, bytes), fails(StatusCode.FailedPrecondition, /records/))

    /* Under new ids, as one step. */
    const added = cdcc.appendLibrary(empty.ctx, bytes)
    assert.deepEqual(added.ids.first.map((i) => [i.from.at(-1), i.to.at(-1)]), [['1', '3'], ['2', '4']])
    assert.equal(cdcc.get(empty.ctx, 'items.length'), 4)
    assert.equal((cdcc.get(empty.ctx, 'items[2].other') as Ref).text.includes(added.ids.first[1].to.split(',')[1]), true)
    cdcc.undo(empty.ctx)
    assert.equal(cdcc.get(empty.ctx, 'items.length'), 2)
    cdcc.set(empty.ctx, 'count', 3)
    assert.throws(() => cdcc.resolvePlaceholders(empty.ctx), fails(StatusCode.FailedPrecondition, /history/))
    cdcc.clearHistory(empty.ctx)
    assert.equal(cdcc.canUndo(empty.ctx), false)
    assert.equal(cdcc.resolvePlaceholders(empty.ctx).blocks.total, 0)
    validate(empty.ctx)
    empty[Symbol.dispose]()
  })

  test('3. undoes the set', () => {
    cdcc.set(ctx, 'strength', 0.75)
    assert.ok(cdcc.canUndo(ctx) && !cdcc.canRedo(ctx))
    cdcc.undo(ctx)
    assert.equal(cdcc.get(ctx, 'strength'), 0.5)
    assert.ok(cdcc.canRedo(ctx))
    cdcc.redo(ctx)
    assert.equal(cdcc.get(ctx, 'strength'), 0.75)
  })

  test('4. a TS command runs through the executor, with undo and redo', () => {
    AddCount.made = 0
    cdcc.registerCommand(ctx, addSpec, AddCount)
    const out = cdcc.invoke(ctx, 'ts.add(amount=5)')
    assert.deepEqual(out, {result: 5})
    assert.equal(cdcc.get(ctx, 'count'), 5)
    assert.equal(cdcc.invoke(ctx, 'ts.add').result, 10, 'LastUsed: amount=5 again')
    cdcc.undo(ctx)
    cdcc.undo(ctx)
    assert.equal(cdcc.get(ctx, 'count'), 0)
    cdcc.redo(ctx) /* no redo slot: exec runs again */
    assert.equal(cdcc.get(ctx, 'count'), 5)
    assert.equal(spike.liveInstances(hostOf(ctx)), 2)
    assert.equal(AddCount.made, 2)
  })

  test('a class with its own redo: redo calls it', () => {
    let redos = 0
    cdcc.registerCommand(
      ctx,
      addSpec,
      class extends AddCount {
        redo(c: Context) {
          redos++
          this.exec(c)
        }
      }
    )
    cdcc.invoke(ctx, 'ts.add(amount=2)')
    cdcc.undo(ctx)
    cdcc.redo(ctx)
    assert.equal(redos, 1)
    assert.equal(cdcc.get(ctx, 'count'), 2)
  })

  test('a TS command composes: set and invoke in exec are sub-commands', () => {
    cdcc.registerCommand(ctx, addSpec, AddCount)
    cdcc.registerCommand(
      ctx,
      {id: 'ts.macro'},
      class implements ScriptCommand {
        exec(c: Context) {
          cdcc.set(c, 'strength', 0.25)
          cdcc.invoke(c, 'ts.add(amount=3)')
        }
        undo() {} /* the sub-commands are undone by the executor */
      }
    )
    cdcc.invoke(ctx, 'ts.macro')
    assert.equal(cdcc.get(ctx, 'strength'), 0.25)
    assert.equal(cdcc.get(ctx, 'count'), 3)
    assert.equal(spike.historySize(hostOf(ctx)), 1)
    cdcc.undo(ctx)
    assert.equal(cdcc.get(ctx, 'strength'), 0.5)
    assert.equal(cdcc.get(ctx, 'count'), 0)
  })

  test('slot failures: exec, canRun, and undo', () => {
    /* A command whose slots come from `body` (as methods, on the prototype). */
    const cmd = (id: string, body: Partial<ScriptCommand>) => {
      class C implements ScriptCommand {
        exec() {}
        undo() {}
      }
      Object.assign(C.prototype, body)
      cdcc.registerCommand(ctx, {id}, C)
    }
    cmd('ts.fail', {
      exec: () => {
        throw new Error('boom')
      },
    })
    cmd('ts.code', {
      exec: () => {
        throw new ScriptError(StatusCode.NotFound, 'no brush')
      },
    })
    cmd('ts.refuse', {canRun: () => 'nothing selected'})
    cmd('ts.false', {canRun: () => false})
    cmd('ts.throwRefuse', {
      canRun: () => {
        throw new Error('locked')
      },
    })
    cmd('ts.badUndo', {undo: (c: Context) => cdcc.set(c, 'count', 1) /* Busy: undo may only write */})
    assert.throws(() => cdcc.invoke(ctx, 'ts.fail'), fails(StatusCode.Internal, /boom/))
    assert.throws(() => cdcc.invoke(ctx, 'ts.code'), fails(StatusCode.NotFound, /no brush/))
    assert.throws(() => cdcc.invoke(ctx, 'ts.refuse'), fails(StatusCode.FailedPrecondition, /nothing selected/))
    assert.throws(() => cdcc.invoke(ctx, 'ts.false'), fails(StatusCode.FailedPrecondition, /refused/))
    assert.throws(() => cdcc.invoke(ctx, 'ts.throwRefuse'), fails(StatusCode.FailedPrecondition, /locked/))
    assert.equal(spike.historySize(hostOf(ctx)), 0)
    cdcc.invoke(ctx, 'ts.badUndo')
    /* The throw became the undo's error; the command was dropped, not left redoable. */
    assert.throws(() => cdcc.undo(ctx), fails(StatusCode.Internal, /Busy.*dropped/s))
    assert.ok(!cdcc.canRedo(ctx))
  })

  test('T2: a slot returning a Promise fails instead of passing', () => {
    cdcc.registerCommand(
      ctx,
      {id: 'ts.async'},
      class implements ScriptCommand {
        async exec(c: Context) {
          cdcc.write(c, 'count', 7)
        }
        undo() {}
      }
    )
    class AsyncCanRun {
      async canRun() {
        return 'nope'
      }
      exec() {}
      undo() {}
    }
    cdcc.registerCommand(ctx, {id: 'ts.asyncCanRun'}, AsyncCanRun as unknown as new () => ScriptCommand)
    assert.throws(() => cdcc.invoke(ctx, 'ts.async'), fails(StatusCode.Internal, /exec returned a Promise/))
    assert.throws(() => cdcc.invoke(ctx, 'ts.asyncCanRun'), fails(StatusCode.FailedPrecondition, /Promise/))
    assert.equal(spike.historySize(hostOf(ctx)), 0)
  })

  test('T3: a throwing memSize or discard is ignored, with a warning', (t) => {
    const warn = t.mock.method(console, 'warn', () => {})
    cdcc.registerCommand(
      ctx,
      addSpec,
      class extends AddCount {
        memSize(): number {
          throw new Error('no size')
        }
        discard() {
          throw new Error('no discard')
        }
      }
    )
    cdcc.invoke(ctx, 'ts.add(amount=1)') /* recorded, memSize counted as 0 */
    cdcc.undo(ctx) /* memSize is read again */
    assert.equal(warn.mock.callCount(), 2)
    cdcc.set(ctx, 'strength', 0.1) /* truncates the redo side: discard throws */
    assert.equal(cdcc.get(ctx, 'strength'), 0.1)
    assert.equal(spike.historySize(hostOf(ctx)), 1)
    assert.equal(warn.mock.callCount(), 3)
  })

  test('T5, T6: Float defaults, and numbers past int64 and 2^53', () => {
    cdcc.registerCommand(
      ctx,
      {id: 'ts.scale', inputs: {factor: cdcc.float(1)}},
      class implements ScriptCommand {
        exec(c: Context) {
          cdcc.write(c, 'strength', cdcc.input(c, 'factor') as number)
        }
        undo() {}
      }
    )
    cdcc.invoke(ctx, 'ts.scale(factor=0.25)')
    assert.equal(cdcc.get(ctx, 'strength'), 0.25)
    cdcc.set(ctx, 'strength', 1e19) /* a Float, clamped; not wrapped to int64 */
    assert.equal(cdcc.get(ctx, 'strength'), 1)
    assert.throws(() => cdcc.fromJs(ctx, 2n ** 64n), RangeError)
    assert.throws(() => cdcc.registerCommand(ctx, {id: 'ts.flags', flags: 2 ** 40}, AddCount), TypeError)
  })

  test('T7: redo as an arrow-function field is refused', () => {
    cdcc.registerCommand(
      ctx,
      {id: 'ts.arrow'},
      class implements ScriptCommand {
        exec = () => {}
        undo = () => {}
        redo = () => {}
      }
    )
    assert.throws(() => cdcc.invoke(ctx, 'ts.arrow'), fails(StatusCode.Internal, /redo must be a method/))
  })

  test('T12: an output named __proto__ is data', () => {
    const outputs: Record<string, number> = {}
    Object.defineProperty(outputs, '__proto__', {value: 0, enumerable: true})
    cdcc.registerCommand(
      ctx,
      {id: 'ts.proto', outputs},
      class implements ScriptCommand {
        exec(c: Context) {
          cdcc.output(c, '__proto__', 3)
        }
        undo() {}
      }
    )
    const out = cdcc.invoke(ctx, 'ts.proto')
    assert.equal(Object.getOwnPropertyDescriptor(out, '__proto__')?.value, 3)
  })

  test('T15: CommandFlags', () => {
    cdcc.registerCommand(ctx, {...addSpec, flags: CommandFlags.NoUndo}, AddCount)
    cdcc.invoke(ctx, 'ts.add(amount=4)')
    assert.equal(cdcc.get(ctx, 'count'), 4)
    assert.equal(spike.historySize(hostOf(ctx)), 0)
  })

  test('values of each kind round-trip', () => {
    cdcc.set(ctx, 'enabled', false)
    assert.equal(cdcc.get(ctx, 'enabled'), false)
    cdcc.set(ctx, 'label', 'hello')
    assert.equal(cdcc.get(ctx, 'label'), 'hello')
    cdcc.set(ctx, 'offset', [1, 2.5, -3])
    assert.deepEqual(cdcc.get(ctx, 'offset'), [1, 2.5, -3])
    cdcc.set(ctx, 'offset', new Float64Array([4, 5, 6]))
    assert.deepEqual(cdcc.get(ctx, 'offset'), [4, 5, 6])
    cdcc.set(ctx, 'mode', 'EDIT')
    assert.equal(cdcc.get(ctx, 'mode'), 1)
    cdcc.set(ctx, 'flags', 3)
    assert.equal(cdcc.get(ctx, 'flags'), 3)
    cdcc.undo(ctx)
    assert.equal(cdcc.get(ctx, 'flags'), 0)
    assert.throws(() => cdcc.set(ctx, 'offset', [1]), TypeError)
  })

  test('registration errors are ScriptErrors', () => {
    assert.throws(
      () => cdcc.registerCommand(ctx, {id: 'core.mine'}, AddCount),
      fails(StatusCode.InvalidArgument, /reserved/)
    )
    assert.throws(() => cdcc.registerCommand(ctx, {id: 'not an id'}, AddCount), ScriptError)
    cdcc.registerCommand(ctx, {id: 'ts.once'}, AddCount)
    assert.throws(() => cdcc.registerCommand(ctx, {id: 'ts.once'}, AddCount), fails(StatusCode.AlreadyExists, /once/))
  })

  test('T1, T8, T11: a disposed or stale context throws; a document closes only outside slots', () => {
    const other = open()
    other[Symbol.dispose]()
    assert.ok(other.disposed)
    assert.throws(() => cdcc.get(other.ctx, 'count'), /disposed/)
    assert.throws(() => cdcc.registerCommand(other.ctx, addSpec, AddCount), /disposed/)

    const inner = open()
    let kept: Context | undefined
    cdcc.registerCommand(
      ctx,
      {id: 'ts.close'},
      class implements ScriptCommand {
        exec(c: Context) {
          kept = c
          inner[Symbol.dispose]()
        }
        undo() {}
      }
    )
    assert.throws(() => cdcc.invoke(ctx, 'ts.close'), fails(StatusCode.Internal, /from inside a command/))
    assert.ok(!inner.disposed)
    inner[Symbol.dispose]()
    assert.throws(() => cdcc.get(kept as Context, 'count'), /disposed/)
  })

  test('T1: a context keeps its document alive through GC', async () => {
    const ctxOnly = open().ctx /* the Session is dropped at once */
    gc()
    await new Promise((r) => setImmediate(r))
    gc()
    const second = open()
    try {
      cdcc.set(second.ctx, 'count', 7)
      assert.equal(cdcc.get(ctxOnly, 'count'), 0)
      cdcc.set(ctxOnly, 'count', 2)
      assert.equal(cdcc.get(second.ctx, 'count'), 7)
    } finally {
      second[Symbol.dispose]()
      hostOf(ctxOnly)[Symbol.dispose]()
    }
  })

  /* graphs["g"] with a node of `type`, its `value` the output `out`, its `a` the input `in`. */
  const makeGraph = (type: string) => {
    const path = 'graphs["g"]'
    cdcc.graphCreate(ctx, 'g')[Symbol.dispose]()
    const out = cdcc.graphAddInterface(ctx, path, 'output', 'out', 'float')
    const in_ = cdcc.graphAddInterface(ctx, path, 'input', 'in', 'float')
    const node = cdcc.graphAddNode(ctx, path, type, 10, 20)
    const output = cdcc.graphAddNode(ctx, path, 'graph.output')
    const input = cdcc.graphAddNode(ctx, path, 'graph.input')
    const link = cdcc.graphLink(ctx, path, node, 'value', output, `#${out}`)
    cdcc.graphLink(ctx, path, input, `#${in_}`, node, 'a')
    return {path, node, link, at: `${path}.nodes[#${node}]`}
  }

  test('a TS node type: evaluated, cached, edited, undone', () => {
    let runs = 0
    cdcc.defineNodeType(ctx, {
      id: 'ts.scale',
      uiName: 'Scale',
      inputs: {a: cdcc.float(1), offset: cdcc.float(0.5)},
      params: {factor: 2, label: 'x'},
      outputs: {value: 'float', text: 'string', copy: 'float'},
      evaluate(node) {
        runs++
        const a = node.input('a') as number
        node.output('value', a * (node.param('factor') as number) + (node.input('offset') as number))
        node.output('text', `${node.param('label')}${a}`)
        node.pass('a', 'copy')
      },
    })
    const {path, node, link, at} = makeGraph('ts.scale')
    assert.deepEqual(cdcc.get(ctx, `${at}.position`), [10, 20])
    using graph = new cdcc.GraphInstance(ctx, path)
    assert.equal(spike.graphInstances(hostOf(ctx)), 1)
    assert.equal(graph.evaluate('out'), 0.5)
    graph.setInput('in', 3)
    assert.equal(graph.evaluate('out'), 6.5)
    assert.equal(runs, 2)
    assert.equal(graph.evaluate('out'), 6.5)
    assert.equal(runs, 2)
    cdcc.set(ctx, `${at}.params.factor`, 10)
    assert.equal(graph.evaluate('out'), 30.5)
    cdcc.undo(ctx)
    assert.equal(graph.evaluate('out'), 6.5)
    graph.setInput('in', undefined)
    assert.equal(graph.evaluate('out'), 0.5)
    const steps = spike.historySize(hostOf(ctx))
    cdcc.graphRemoveNode(ctx, path, node)
    assert.equal(graph.evaluate('out'), 0)
    cdcc.undo(ctx)
    assert.equal(graph.evaluate('out'), 0.5)
    assert.equal(spike.historySize(hostOf(ctx)), steps) /* instead of the one undone */
    cdcc.graphUnlink(ctx, path, link) /* back with its id */
    assert.equal(graph.evaluate('out'), 0)
    assert.throws(() => graph.evaluate('no'), fails(StatusCode.NotFound, /no/))
    assert.throws(() => cdcc.graphAddNode(ctx, path, 'ts.sclae'), fails(StatusCode.NotFound, /ts\.scale/))
  })

  test('a TS kernel that throws, or misuses what it has', () => {
    let body: (node: cdcc.NodeContext) => unknown = () => {}
    let kept: cdcc.NodeContext | undefined
    cdcc.defineNodeType(ctx, {
      id: 'ts.odd',
      inputs: {a: cdcc.float(1)},
      outputs: {value: 'float'},
      evaluate(node) {
        kept = node
        return body(node) as void
      },
    })
    const {path, node} = makeGraph('ts.odd')
    using graph = new cdcc.GraphInstance(ctx, path)
    let value = 0
    const run = (f: typeof body) => {
      body = f
      graph.setInput('in', ++value) /* so that it runs */
      return graph.evaluate('out')
    }
    const boom = () => {
      throw new Error('boom')
    }
    assert.throws(() => run(boom), fails(StatusCode.Internal, /boom/))
    assert.match(graph.errorText('out'), new RegExp(`boom.*node ${node}`))
    const coded = () => {
      throw new ScriptError(StatusCode.OutOfRange, 'too far')
    }
    assert.throws(() => run(coded), fails(StatusCode.OutOfRange, /too far/))
    assert.throws(() => run(() => {}), fails(StatusCode.Internal, /value/)) /* no output */
    assert.throws(() => run(async () => {}), fails(StatusCode.Internal, /Promise/))
    assert.throws(() => run((n) => n.input('b')), fails(StatusCode.NotFound, /'b'/))
    assert.throws(() => run((n) => n.output('value', 'x')), fails(StatusCode.TypeMismatch, /./))
    /* The document is out of reach, and it can't be closed. */
    assert.throws(() => run(() => cdcc.get(ctx, 'count')), fails(StatusCode.Busy, /kernel/))
    assert.throws(() => run(() => cdcc.set(ctx, 'count', 1)), fails(StatusCode.Busy, /kernel/))
    assert.throws(() => run(() => cdcc.graphCreate(ctx, 'x')), fails(StatusCode.Busy, /kernel/))
    assert.throws(() => run(() => session[Symbol.dispose]()), fails(StatusCode.Internal, /from inside/))
    assert.equal(cdcc.get(ctx, 'count'), 0)
    /* And it works again. */
    assert.equal(run((n) => n.output('value', (n.input('a') as number) + 0.5)), value + 0.5)
    assert.equal(graph.errorText('out'), '')
    assert.throws(() => kept!.input('a'), /already evaluated/)
  })

  test('a TS kernel that disposes of its own graph instance', () => {
    let graph: cdcc.GraphInstance | undefined
    cdcc.defineNodeType(ctx, {
      id: 'ts.dispose',
      inputs: {a: cdcc.float(1)},
      outputs: {value: 'float'},
      evaluate(node) {
        graph?.[Symbol.dispose]()
        node.output('value', 7)
      },
    })
    const {path} = makeGraph('ts.dispose')
    graph = new cdcc.GraphInstance(ctx, path)
    assert.equal(graph.evaluate('out'), 7)
    assert.equal(spike.graphInstances(hostOf(ctx)), 0)
    graph[Symbol.dispose]()
    assert.throws(() => graph!.evaluate('out'), /graph instance was disposed/)
  })

  test('node type registration errors; an instance outlives its document', () => {
    const spec = {id: 'ts.n', outputs: {value: 'float'}, evaluate() {}}
    assert.throws(() => cdcc.defineNodeType(ctx, {...spec, id: 'graph.n'}), fails(StatusCode.InvalidArgument, /reserved/))
    assert.throws(() => cdcc.defineNodeType(ctx, {...spec, outputs: {}}), fails(StatusCode.InvalidArgument, /output/))
    assert.throws(
      () => cdcc.defineNodeType(ctx, {...spec, inputs: {m: cdcc.payload('no.Mesh')}}),
      fails(StatusCode.NotFound, /no\.Mesh/)
    )
    assert.throws(() => cdcc.defineNodeType(ctx, {...spec, evaluate: 1 as never}), TypeError)
    cdcc.defineNodeType(ctx, spec)
    assert.throws(() => cdcc.defineNodeType(ctx, spec), fails(StatusCode.AlreadyExists, /ts\.n/))
    assert.throws(() => new cdcc.GraphInstance(ctx, 'count'), fails(StatusCode.InvalidArgument, /graph/))

    const other = open()
    cdcc.graphCreate(other.ctx, 'g')[Symbol.dispose]()
    using late = new cdcc.GraphInstance(other.ctx, 'graphs["g"]')
    other[Symbol.dispose]()
    assert.throws(() => late.evaluate('out'), fails(StatusCode.FailedPrecondition, /closed/))
  })

  test('T4, T14: one module per realm', async () => {
    assert.equal(cdcc.load(module, 'TESTAPP_GetManager'), backend)
    const second = await create()
    assert.throws(() => cdcc.load(second, 'TESTAPP_GetManager'), /another coredcc module/)
    assert.throws(() => cdcc.load({}, 'TESTAPP_GetManager', {replace: true}), /Emscripten module/)
    assert.throws(() => cdcc.load(second, 'NoSuchExport', {replace: true}), /exports no NoSuchExport/)
  })
})
