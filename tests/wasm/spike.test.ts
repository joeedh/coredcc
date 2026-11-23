/**
 * The P1f TS spike (docs/plans/2026-11-20-p1-application-spine.md, exit criteria): the
 * wasm-st module, through litestl's TS runtime and coredcc's TS layer,
 * 1. resolves a path;
 * 2. gets and sets a value;
 * 3. undoes the set;
 * 4. runs a Command implemented in TS through the executor, including undo.
 * run.mjs builds and runs it.
 */
import assert from 'node:assert/strict'
import {afterEach, beforeEach, describe, test} from 'node:test'
import {pathToFileURL} from 'node:url'

import * as cdcc from 'coredcc-script'
import {ScriptError, StatusCode, type Context, type ScriptCommand} from 'coredcc-script'
import type {Host} from 'coredcc-gen/spike/Host'
import * as spike from 'coredcc-gen/spike/functions'

const create = (await import(pathToFileURL(process.env.SPIKE_MODULE as string).href)).default
const backend = cdcc.load(await create(), 'SPIKE_GetManager')

/** A ScriptError with `code` whose message matches `re`. */
const fails = (code: StatusCode, re: RegExp) => (e: unknown) =>
  e instanceof ScriptError && e.code === code && re.test(e.message)

const validate = (host: Host) => {
  const s = spike.validate(host)
  try {
    assert.ok(s.ok(), s.message())
  } finally {
    s[Symbol.dispose]()
  }
}

/* Adds `amount` to `count`, keeping what undo needs; writes `result`. */
class AddCount implements ScriptCommand {
  static made = 0
  static discarded = 0
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
  discard() {
    AddCount.discarded++
  }
}

describe('coredcc wasm-st spike', () => {
  let host: Host
  let ctx: Context
  const handlesBefore = backend.foreignCount()

  beforeEach(() => {
    host = spike.create() as Host
    ctx = spike.context(host) as Context
  })
  afterEach(() => {
    validate(host)
    host[Symbol.dispose]()
    /* The history released every instance, and the Runtime every factory. */
    assert.equal(backend.foreignCount(), handlesBefore)
  })

  test('1. resolves a path', () => {
    cdcc.resolve(ctx, 'count')
    cdcc.resolve(ctx, 'strength')
    assert.throws(() => cdcc.resolve(ctx, 'cuont'), fails(StatusCode.NotFound, /count/))
  })

  test('2. gets and sets a value', () => {
    assert.equal(cdcc.get(ctx, 'strength'), 0.5)
    cdcc.set(ctx, 'strength', 0.75)
    assert.equal(cdcc.get(ctx, 'strength'), 0.75)
    assert.equal(spike.historySize(host), 1)
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
    AddCount.made = AddCount.discarded = 0
    cdcc.registerCommand(ctx, {id: 'ts.add', inputs: {amount: 1}, outputs: {result: 0}}, AddCount)
    const out = cdcc.invoke(ctx, 'ts.add(amount=5)')
    assert.deepEqual(out, {result: 5})
    assert.equal(cdcc.get(ctx, 'count'), 5)
    assert.equal(cdcc.invoke(ctx, 'ts.add').result, 10, 'LastUsed: amount=5 again')
    cdcc.undo(ctx)
    cdcc.undo(ctx)
    assert.equal(cdcc.get(ctx, 'count'), 0)
    cdcc.redo(ctx) /* no redo slot: exec runs again */
    assert.equal(cdcc.get(ctx, 'count'), 5)
    assert.equal(spike.liveInstances(host), 2)
    assert.equal(AddCount.made, 2)
  })

  test('a TS command composes: set and invoke in exec are sub-commands', () => {
    cdcc.registerCommand(ctx, {id: 'ts.add', inputs: {amount: 1}, outputs: {result: 0}}, AddCount)
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
    assert.equal(spike.historySize(host), 1)
    cdcc.undo(ctx)
    assert.equal(cdcc.get(ctx, 'strength'), 0.5)
    assert.equal(cdcc.get(ctx, 'count'), 0)
  })

  test('failures: a throwing exec, a refusing canRun, Busy from undo', () => {
    cdcc.registerCommand(
      ctx,
      {id: 'ts.fail'},
      class implements ScriptCommand {
        exec() {
          throw new Error('boom')
        }
        undo() {}
      }
    )
    cdcc.registerCommand(
      ctx,
      {id: 'ts.refuse'},
      class implements ScriptCommand {
        canRun() {
          return 'nothing selected'
        }
        exec() {}
        undo() {}
      }
    )
    cdcc.registerCommand(
      ctx,
      {id: 'ts.badUndo'},
      class implements ScriptCommand {
        exec() {}
        undo(c: Context) {
          cdcc.set(c, 'count', 1) /* Busy: undo may only write */
        }
      }
    )
    assert.throws(() => cdcc.invoke(ctx, 'ts.fail'), fails(StatusCode.Internal, /boom/))
    assert.throws(() => cdcc.invoke(ctx, 'ts.refuse'), fails(StatusCode.FailedPrecondition, /nothing selected/))
    assert.equal(spike.historySize(host), 0)
    cdcc.invoke(ctx, 'ts.badUndo')
    /* The throw became the undo's error; the command was dropped, not left redoable. */
    assert.throws(() => cdcc.undo(ctx), fails(StatusCode.Internal, /Busy.*dropped/s))
    assert.ok(!cdcc.canRedo(ctx))
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
})
