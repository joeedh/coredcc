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
import {CommandFlags, ScriptError, Session, StatusCode, type Context, type ScriptCommand} from 'coredcc-script'
import type {Host} from 'coredcc-gen/spike/Host'
import * as spike from 'coredcc-gen/spike/functions'

const create = (await import(pathToFileURL(process.env.SPIKE_MODULE as string).href)).default
const module = await create()
const backend = cdcc.load(module, 'SPIKE_GetManager')

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
    cdcc.undo(ctx)
    cdcc.set(ctx, 'strength', 0.1) /* truncates the redo side: discard throws */
    assert.equal(cdcc.get(ctx, 'strength'), 0.1)
    assert.equal(spike.historySize(hostOf(ctx)), 1)
    assert.equal(warn.mock.callCount(), 2)
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
    assert.throws(() => cdcc.fromJs(2n ** 64n), RangeError)
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

  test('T4, T14: one module per realm', async () => {
    assert.equal(cdcc.load(module, 'SPIKE_GetManager'), backend)
    const second = await create()
    assert.throws(() => cdcc.load(second, 'SPIKE_GetManager'), /another coredcc module/)
    assert.throws(() => cdcc.load({}, 'SPIKE_GetManager', {replace: true}), /Emscripten module/)
    assert.throws(() => cdcc.load(second, 'NoSuchExport', {replace: true}), /exports no NoSuchExport/)
  })
})
