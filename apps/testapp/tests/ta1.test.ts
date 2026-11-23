/**
 * TA1 (P1 exit, docs/plans/2026-11-20-p1-application-spine.md "P1f step 6"): the page
 * loads the wasm-st module, and these tests drive the TS spike's four steps over CDP:
 * resolve a path, get and set a value, undo the set, and run a TS command through the
 * executor with undo.
 *
 * One launch for the file; `reset()` gives each test a fresh document, and `check()`
 * validates it afterwards. Without a wasm-st build the page has no backend and these
 * skip, except under the ctest (COREDCC_TESTAPP_REQUIRE_BACKEND), where that fails.
 */
import assert from 'node:assert/strict'
import {after, afterEach, before, beforeEach, describe, test} from 'node:test'
import {type App, ok, startApp} from './harness.ts'

interface Reply {
  ok: boolean
  value?: any
  error?: {name: string; message: string; code?: string}
}

let app: App | undefined
let backend = 'none'
let failed = false

const call = async (method: string, ...args: unknown[]): Promise<Reply> => {
  const r = (await app!.client.call(method, args)) as Reply
  return r
}
const value = async (method: string, ...args: unknown[]) => ok(await call(method, ...args))

/** The reply's error, which must be a ScriptError with `code`. */
const scriptError = async (method: string, ...args: unknown[]) => {
  const r = await call(method, ...args)
  assert.equal(r.ok, false, `${method} should fail`)
  assert.equal(r.error?.name, 'ScriptError', r.error?.message)
  return r.error!
}

/** A test that needs the backend; marks the run failed (keeping artifacts) on a throw. */
const step = (name: string, fn: () => Promise<void>) =>
  test(name, async (t) => {
    if (backend === 'none') {
      t.skip('no wasm-st build: the page has no coredcc backend')
      return
    }
    try {
      await fn()
    } catch (err) {
      failed = true
      throw err
    }
  })

describe('TA1: the wasm-st backend over CDP', () => {
  before(async () => {
    app = await startApp({name: 'ta1'})
    const info = ok(await app.client.call('info'))
    backend = info.backend
    if (backend === 'none' && process.env.COREDCC_TESTAPP_REQUIRE_BACKEND) {
      failed = true
      throw new Error(`the page has no backend: ${info.backendError}`)
    }
  })
  after(async () => {
    await app?.close({failed})
  })
  beforeEach(async () => {
    if (backend === 'none') return
    assert.deepEqual(await value('reset'), {foreignCountBetween: 0})
  })
  afterEach(async () => {
    if (backend === 'none') return
    const c = await value('check')
    if (!c.valid) failed = true
    assert.ok(c.valid, c.message)
  })

  step('info() reports the backend and its module', async () => {
    const info = await value('info')
    assert.equal(info.backend, 'wasm-st')
    assert.match(info.module, /coredcc_testapp\.mjs$/)
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
  })

  step('values of each kind cross the wire', async () => {
    await value('set', 'offset', [1, 2.5, -3])
    assert.deepEqual(await value('get', 'offset'), [1, 2.5, -3])
    await value('set', 'mode', 'EDIT')
    assert.equal(await value('get', 'mode'), 1)
    await value('set', 'label', 'hello')
    assert.equal(await value('get', 'label'), 'hello')
    await value('set', 'enabled', false)
    assert.equal(await value('get', 'enabled'), false)
  })

  step("a command's error code and its __proto__ output cross the wire", async () => {
    await value('defineCommand', 'ta1.fail', {}, `class {
      exec() { throw new cdcc.ScriptError(cdcc.StatusCode.NotFound, 'no brush') }
      undo() {}
    }`)
    const e = await scriptError('exec', 'ta1.fail')
    assert.equal(e.code, 'NotFound')
    assert.match(e.message, /no brush/)

    await value('defineCommand', 'ta1.proto', {outputs: JSON.parse('{"__proto__": 0}')}, `class {
      exec(ctx) { cdcc.output(ctx, '__proto__', 3) }
      undo() {}
    }`)
    const out = await value('exec', 'ta1.proto')
    assert.equal(Object.getOwnPropertyDescriptor(out, '__proto__')?.value, 3)
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
