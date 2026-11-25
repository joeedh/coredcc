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
    for (const x of [0.6, 0.7, 0.8]) cdcc.set(ctx, 'strength', x, 5)
    assert.equal(spike.historySize(hostOf(ctx)), 1)
    cdcc.set(ctx, 'strength', 0.9, 6)
    assert.equal(spike.historySize(hostOf(ctx)), 2)
    cdcc.undo(ctx)
    cdcc.undo(ctx)
    assert.equal(cdcc.get(ctx, 'strength'), 0.5)
    for (const g of [0, -1, 1.5, NaN, 2 ** 60]) assert.throws(() => cdcc.set(ctx, 'strength', 1, g), TypeError)
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
