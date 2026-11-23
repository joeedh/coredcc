/**
 * TA1 (P1 exit, docs/plans/2026-11-20-p1-application-spine.md "P1f step 6"): the page
 * loads the wasm-st module, and these tests drive the TS spike's four steps over CDP:
 * resolve a path, get and set a value, undo the set, and run a TS command through the
 * executor with undo. The rest covers the testbus's rules and the regressions from
 * docs/reports/feature-report-ta1-2026-11-23.md (named by finding).
 *
 * One launch for the main suite; `reset()` gives each test a fresh document, and
 * `check()` validates it afterwards. Tests skip only when the build has no backend
 * (`info().backendMissing`); any other load failure fails them.
 */
import assert from 'node:assert/strict'
import path from 'node:path'
import {after, afterEach, before, beforeEach, describe, test} from 'node:test'
import {DEBUG_MODULE} from '../nwjs/launcher.mjs'
import {type App, ok, startApp} from './harness.ts'

interface Reply {
  ok: boolean
  value?: any
  error?: {name: string; message: string; code?: string; detail?: string}
}

/** The backend the page should report: TA4 runs these against 'native' too. */
const EXPECTED_BACKEND = process.env.COREDCC_TESTAPP_BACKEND ?? 'wasm-st'
const MODULE = process.env.COREDCC_TESTAPP_MODULE ?? DEBUG_MODULE

/** Starts the app; throws (failing the suite) if its backend failed to load. Returns
 * undefined when the build has none, so the suite skips. */
async function startWithBackend(name: string, appArgs: string[] = []): Promise<{app: App; info: any}> {
  const app = await startApp({name, appArgs})
  const info = ok(await app.client.call('info'))
  return {app, info}
}

const call = async (app: App, method: string, ...args: unknown[]): Promise<Reply> =>
  (await app.client.call(method, args)) as Reply

describe('TA1: the wasm-st backend over CDP', () => {
  let app: App | undefined
  let missing: string | undefined
  let failed = false

  const value = async (method: string, ...args: unknown[]) => ok(await call(app!, method, ...args))
  /** The reply's error, which must be a ScriptError with `code`. */
  const scriptError = async (method: string, ...args: unknown[]) => {
    const r = await call(app!, method, ...args)
    assert.equal(r.ok, false, `${method} should fail`)
    assert.equal(r.error?.name, 'ScriptError', r.error?.message)
    return r.error!
  }
  /* A6: any failure, in a test or a hook, keeps the run's artifacts. */
  const tracked = (fn: () => Promise<void>) => async () => {
    try {
      await fn()
    } catch (err) {
      failed = true
      throw err
    }
  }
  const step = (name: string, fn: () => Promise<void>) =>
    test(name, async (t) => {
      if (missing !== undefined) {
        t.skip(`the build has no coredcc backend: ${missing}`)
        return
      }
      await tracked(fn)()
    })

  before(
    tracked(async () => {
      const started = await startWithBackend('ta1')
      app = started.app
      const info = started.info
      if (info.backendMissing && !process.env.COREDCC_TESTAPP_REQUIRE_BACKEND) {
        missing = info.backendError
        return
      }
      assert.equal(info.backend, EXPECTED_BACKEND, `the backend failed to load: ${info.backendError}`)
    })
  )
  after(async () => {
    await app?.close({failed})
  })
  beforeEach(
    tracked(async () => {
      if (missing !== undefined) return
      assert.deepEqual(await value('reset'), {foreignCountBetween: 0})
    })
  )
  afterEach(
    tracked(async () => {
      if (missing !== undefined) return
      const c = await value('check')
      assert.ok(c.valid, c.message)
    })
  )

  step('info() reports the backend and its module', async () => {
    const info = await value('info')
    assert.equal(info.backend, EXPECTED_BACKEND)
    assert.equal(path.resolve(info.module), path.resolve(MODULE))
    assert.equal(info.backendError, undefined)
  })

  step('1. resolves a path, with suggestions for a typo', async () => {
    assert.equal(await value('resolve', 'count'), true)
    const e = await scriptError('resolve', 'cuont')
    assert.equal(e.code, 'NotFound')
    assert.match(e.message, /did you mean.*count/)
  })

  step('2-3. gets, sets and undoes a value', async () => {
    assert.equal(await value('get', 'strength'), 0.5)
    await value('set', 'strength', 0.75)
    assert.equal(await value('get', 'strength'), 0.75)
    assert.deepEqual(await value('history'), {size: 1, canUndo: true, canRedo: false})
    await value('undo')
    assert.equal(await value('get', 'strength'), 0.5)
    await value('redo')
    assert.equal(await value('get', 'strength'), 0.75)
    assert.equal((await scriptError('set', 'strength', 'x')).code, 'TypeMismatch')
  })

  step('4. a TS command runs through the executor, with undo and redo', async () => {
    assert.deepEqual(await value('exec', 'testapp.addCount(amount=5)'), {result: 5})
    assert.equal(await value('get', 'count'), 5)
    assert.deepEqual(await value('exec', 'testapp.addCount'), {result: 10}, 'LastUsed: amount=5 again')
    await value('undo')
    await value('undo')
    assert.equal(await value('get', 'count'), 0)
    await value('redo') /* no redo slot: exec runs again */
    assert.equal(await value('get', 'count'), 5)
    const c = await value('check')
    assert.equal(c.liveInstances, 2)
    assert.equal(c.foreignCount, 3, "the page's factory and the two live instances")
  })

  step('undo and redo with nothing to do are FailedPrecondition', async () => {
    assert.equal((await scriptError('undo')).code, 'FailedPrecondition')
    assert.equal((await scriptError('redo')).code, 'FailedPrecondition')
  })

  step('values of each kind cross the wire', async () => {
    await value('set', 'offset', [1, 2.5, -3])
    assert.deepEqual(await value('get', 'offset'), [1, 2.5, -3])
    await value('set', 'mode', 'EDIT')
    assert.equal(await value('get', 'mode'), 1)
    await value('set', 'flags', 3)
    assert.equal(await value('get', 'flags'), 3)
    await value('set', 'label', 'hello')
    assert.equal(await value('get', 'label'), 'hello')
    await value('set', 'enabled', false)
    assert.equal(await value('get', 'enabled'), false)

    await value(
      'defineCommand',
      'ta1.kinds',
      {outputs: {v: [0, 0, 0], b: false, s: ''}},
      `class {
        exec(ctx) {
          cdcc.output(ctx, 'v', [1, 2, 3])
          cdcc.output(ctx, 'b', true)
          cdcc.output(ctx, 's', 'text')
        }
        undo() {}
      }`
    )
    assert.deepEqual(await value('exec', 'ta1.kinds'), {v: [1, 2, 3], b: true, s: 'text'})
  })

  step("errors: a command's code and detail, a refusing canRun, and a TypeError", async () => {
    await value(
      'defineCommand',
      'ta1.fail',
      {},
      `class {
        exec() { throw new cdcc.ScriptError(cdcc.StatusCode.NotFound, 'no brush') }
        undo() {}
      }`
    )
    const e = await scriptError('exec', 'ta1.fail')
    assert.equal(e.code, 'NotFound')
    assert.equal(e.detail, 'no brush')

    await value('defineCommand', 'ta1.refuse', {}, `class { canRun() { return 'nothing selected' } exec() {} undo() {} }`)
    const refused = await scriptError('exec', 'ta1.refuse')
    assert.equal(refused.code, 'FailedPrecondition')
    assert.match(refused.message, /nothing selected/)

    /* Misuse of the TS layer is a TypeError, without a code. */
    const r = await call(app!, 'defineCommand', 'ta1.notAClass', {}, '42')
    assert.equal(r.ok, false)
    assert.equal(r.error?.name, 'TypeError')
    assert.match(r.error!.message, /expected a class/)
    assert.equal(r.error?.code, undefined)
  })

  step('S4: {$float: n} declares a Float input', async () => {
    await value(
      'defineCommand',
      'ta1.scale',
      {inputs: {factor: {$float: 1}}},
      `class { exec(ctx) { cdcc.write(ctx, 'strength', cdcc.input(ctx, 'factor')) } undo() {} }`
    )
    await value('exec', 'ta1.scale(factor=0.25)')
    assert.equal(await value('get', 'strength'), 0.25)
  })

  step('an output named __proto__ crosses the wire', async () => {
    await value(
      'defineCommand',
      'ta1.proto',
      {outputs: JSON.parse('{"__proto__": 0}')},
      `class { exec(ctx) { cdcc.output(ctx, '__proto__', 3) } undo() {} }`
    )
    const out = await value('exec', 'ta1.proto')
    assert.equal(Object.getOwnPropertyDescriptor(out, '__proto__')?.value, 3)
  })

  step('A1: a command that runs itself fails at the nesting cap, and the document is fine', async () => {
    await value('defineCommand', 'ta1.recurse', {}, `class { exec(ctx) { cdcc.invoke(ctx, 'ta1.recurse') } undo() {} }`)
    const e = await scriptError('exec', 'ta1.recurse')
    assert.equal(e.code, 'OutOfRange')
    assert.match(e.message, /nested deeper than 32/)
    assert.deepEqual(await value('history'), {size: 0, canUndo: false, canRedo: false})
    assert.deepEqual(await value('exec', 'testapp.addCount(amount=2)'), {result: 2})
    assert.equal((await value('check')).liveInstances, 1)
  })

  step('reset() gives a fresh document', async () => {
    await value('exec', 'testapp.addCount(amount=3)')
    await value('defineCommand', 'ta1.extra', {}, 'class { exec() {} undo() {} }')
    assert.deepEqual(await value('reset'), {foreignCountBetween: 0})
    assert.equal(await value('get', 'count'), 0)
    assert.deepEqual(await value('history'), {size: 0, canUndo: false, canRedo: false})
    /* The page's own command is registered again; the test's is gone. */
    assert.deepEqual(await value('exec', 'testapp.addCount'), {result: 1})
    assert.equal((await scriptError('exec', 'ta1.extra')).code, 'NotFound')
  })
})

describe('TA1: startup and a dead module', () => {
  /** Runs `fn` on a fresh launch, skipping when the build has no backend. */
  const launched = (name: string, short: string, appArgs: string[], fn: (app: App, info: any) => Promise<void>) =>
    test(name, async (t) => {
      const {app, info} = await startWithBackend(short, appArgs)
      let failed = true
      try {
        if (info.backendMissing && !process.env.COREDCC_TESTAPP_REQUIRE_BACKEND) {
          t.skip(`the build has no coredcc backend: ${info.backendError}`)
        } else {
          await fn(app, info)
        }
        failed = false
      } finally {
        await app.close({failed})
      }
    })

  launched(
    'A4: a wrong module fails to load, and is reported, not skipped',
    'ta1-wrong-module',
    [`--apptest-module=${MODULE.replace(/coredcc_testapp\.mjs$/, 'coredcc_testapp_node.mjs')}`],
    async (_app, info) => {
      assert.equal(info.backend, 'none')
      assert.equal(info.backendMissing, undefined)
      assert.match(info.backendError, /node variant/)
    }
  )

  launched('A3: an aborted module is reported, and later calls say so', 'ta1-abort', [], async (app) => {
    const r = await call(app, 'abortForTest')
    assert.equal(r.ok, false)
    assert.match(r.error!.message, /module aborted/)
    assert.match(r.error!.message, /testapp::abortForTest/, "the C++ check's text, from stderr")
    const info = ok(await app.client.call('info'))
    assert.match(info.backendError, /aborted/)
    const later = await call(app, 'get', 'count')
    assert.equal(later.ok, false)
    assert.match(later.error!.message, /module aborted/)
  })
})
