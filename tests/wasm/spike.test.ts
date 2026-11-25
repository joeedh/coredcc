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

import * as cdcc from 'coredcc-script'
import {Ref, CommandFlags, ScriptError, Session, StatusCode, type Context, type ScriptCommand} from 'coredcc-script'
import type {CommandDef} from 'coredcc-gen/coredcc/CommandDef'
import * as cd from 'coredcc-gen/coredcc/functions'
import type {NodeTypeDef} from 'coredcc-gen/coredcc/NodeTypeDef'
import type {Status} from 'coredcc-gen/coredcc/Status'
import type {Host} from 'coredcc-gen/testapp/Host'
import * as spike from 'coredcc-gen/testapp/functions'

import {backend, create, fails, gc, hostOf, module, open, validate} from './fixture.ts'
import {graphTests} from './graphs.ts'

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
    assert.throws(() => cdcc.cancelGesture(ctx, cancelled + 1), fails(StatusCode.InvalidArgument, /beginGesture/))
    for (const g of [0, -1, 1.5, NaN, 2 ** 60]) assert.throws(() => cdcc.set(ctx, 'strength', 1, g), TypeError)
  })

  test('a string with a NUL is refused wherever it comes in', () => {
    const nul = 'a\0b'
    const refused = fails(StatusCode.InvalidArgument, /NUL/)
    class Nothing {
      exec() {}
      undo() {}
    }
    const defineNul = (more: Partial<cdcc.NodeTypeSpec>) =>
      cdcc.defineNodeType(ctx, {id: 'ts.n', outputs: {}, evaluate() {}, ...more})
    cdcc.graphCreate(ctx, 'nul')[Symbol.dispose]()
    using graph = new cdcc.GraphInstance(ctx, 'graphs["nul"]')
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
      ['an item', () => defineNul({params: {p: cdcc.enumParam({[nul]: 1}, nul)}})],
      ['an item param', () => defineNul({params: {[nul]: cdcc.enumParam({A: 1}, 'A')}})],
      ['a pointer param', () => defineNul({params: {[nul]: cdcc.pointerParam('Item')}})],
      ['a target', () => defineNul({params: {p: cdcc.pointerParam(nul)}})],
      ['an input name', () => defineNul({inputs: {a: 1}, inputNames: {a: nul}})],
      ['an output name', () => defineNul({outputs: {a: 'float'}, outputNames: {[nul]: 'x'}})],
      ['a range', () => defineNul({params: {p: 1}, ranges: {[nul]: [0, 1]}})],
      ['a new id', () => cdcc.graphNewId(ctx, nul)],
      ['diagnostics', () => cdcc.graphDiagnostics(ctx, nul)],
      ['a table', () => graph.setInput('in', new cdcc.Table([1], {[nul]: [1]}))],
      ['a cell', () => graph.setInput('in', new cdcc.Table([1], {c: [nul]}))],
      ['a table input', () => graph.setInput(nul, new cdcc.Table())],
      ['a table output', () => graph.evaluateTable(nul)],
      ['a watched node', () => graph.watch('x', nul)],
      ['a watch', () => graph.watch(nul)],
      ['peek', () => graph.peek(nul, 'x')],
      ['peek at a socket', () => graph.peekTable(1, nul)],
      ['report', () => graph.report(nul)],
      ['drop', () => graph.drop(nul)],
    ]
    for (const [what, call] of calls) assert.throws(call, refused, what)
    /* So are slots, and the handle goes with them. */
    const handles = backend.foreignCount()
    const command = cd.newCommandDef('ts.nul') as CommandDef
    const node = cd.newNodeType('ts.n') as NodeTypeDef
    const entries: ((handle: number) => Status)[] = [
      (handle) => cd.registerForeignCommand(ctx, command, handle, nul),
      (handle) => cd.registerForeignNodeType(ctx, node, handle, nul),
      (handle) => cd.replaceForeignNodeType(ctx, node, handle, nul),
    ]
    for (const entry of entries) {
      const status = entry(backend.foreignHandle({}))
      assert.match(status.message(), /NUL/)
      status[Symbol.dispose]()
      assert.equal(backend.foreignCount(), handles)
    }
    command[Symbol.dispose]()
    node[Symbol.dispose]()
    /* An id is refused as one that isn't an identifier. */
    assert.throws(() => cdcc.registerCommand(ctx, {id: 'ts.' + nul}, Nothing), ScriptError)
    assert.equal(cdcc.get(ctx, 'label'), '')
    assert.equal(cdcc.get(ctx, 'items["Cube"].name'), 'Cube')
    /* The id wasn't taken by those. */
    cdcc.registerCommand(ctx, {id: 'ts.nul'}, Nothing)
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

  graphTests({
    get ctx() {
      return ctx
    },
    get session() {
      return session
    },
    open,
    hostOf,
    fails,
  })

  test('T4, T14: one module per realm', async () => {
    assert.equal(cdcc.load(module, 'TESTAPP_GetManager'), backend)
    const second = await create()
    assert.throws(() => cdcc.load(second, 'TESTAPP_GetManager'), /another coredcc module/)
    assert.throws(() => cdcc.load({}, 'TESTAPP_GetManager', {replace: true}), /Emscripten module/)
    assert.throws(() => cdcc.load(second, 'NoSuchExport', {replace: true}), /exports no NoSuchExport/)
  })
})
