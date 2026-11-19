/**
 * TA0: the shell, launcher, profiles, CDP client, reply encoding, logs, screenshots and
 * crash handling. No coredcc yet.
 */
import assert from 'node:assert/strict'
import fs from 'node:fs'
import os from 'node:os'
import path from 'node:path'
import {test} from 'node:test'
import {launch} from '../nwjs/launcher.mjs'
import {deleteProfile} from '../nwjs/profile_dir.mjs'
import {ok, startApp} from './harness.ts'

test('two instances run side by side, and both profiles are deleted', async () => {
  const [a, b] = await Promise.all([startApp({name: 'pair-a'}), startApp({name: 'pair-b'})])
  try {
    const [ia, ib] = await Promise.all([a.client.call('info'), b.client.call('info')])
    const infoA = ok(ia)
    const infoB = ok(ib)
    assert.equal(infoA.protocol, 1)
    assert.notEqual(a.port, b.port)
    assert.equal(path.resolve(infoA.profile), path.resolve(a.profileDir))
    assert.equal(path.resolve(infoB.profile), path.resolve(b.profileDir))
    assert.equal(infoA.browserPid, a.pid)
    assert.equal(infoA.devicePixelRatio, 1)
  } finally {
    await Promise.all([a.close(), b.close()])
  }
  assert.ok(!fs.existsSync(a.profileDir), 'profile a deleted')
  assert.ok(!fs.existsSync(b.profileDir), 'profile b deleted')
})

test('a second launch on a profile in use fails loudly', async () => {
  const profileDir = fs.mkdtempSync(path.join(os.tmpdir(), 'coredcc-testapp-'))
  const first = await launch({profileDir, headless: true})
  try {
    await assert.rejects(launch({profileDir, headless: true}), /profile in use/)
  } finally {
    first.child.kill()
    await first.exited
    deleteProfile(profileDir)
  }
})

test('reply encoding round-trips values returnByValue would mangle', async () => {
  const app = await startApp({name: 'wire'})
  let failed = true
  try {
    const value = {
      big: 1n << 63n,
      neg: -42n,
      nan: Number.NaN,
      inf: [Infinity, -Infinity],
      negZero: -0,
      undef: undefined,
      holes: [1, undefined, 3],
      f32: new Float32Array([1.5, Number.NaN]),
      u64: new BigUint64Array([2n ** 64n - 1n]),
      map: new Map<unknown, unknown>([['k', 1n]]),
      set: new Set([1, 2]),
      forged: {$t: 'bigint', v: '5'},
      nested: {deep: {deeper: [{x: 1n}]}},
    }
    const back = ok(await app.client.call('echo', [value]))
    assert.deepStrictEqual(back, value)
    assert.ok(Object.is(back.negZero, -0))
    assert.ok(Object.hasOwn(back, 'undef'))

    const tooBig = await app.client.call('bigString', [17 * 1024 * 1024])
    assert.equal(tooBig.ok, false)
    assert.equal(tooBig.error.name, 'ReplyTooLarge')

    const unknown = await app.client.call('noSuchMethod')
    assert.equal(unknown.ok, false)
    assert.match(unknown.error.message, /unknown testbus method/)

    await assert.rejects(
      app.client.evaluate('new Promise(() => {})', {timeoutMs: 500}),
      /timed out/,
    )
    failed = false
  } finally {
    await app.close({failed})
  }
})

test('logs reach CDP and the app log file', async () => {
  const app = await startApp({name: 'logs'})
  let failed = true
  try {
    ok(await app.client.call('log', ['error', 'ta0-log-marker']))
    const deadline = Date.now() + 2000
    while (!app.client.logs.some((e) => e.text.includes('ta0-log-marker')) && Date.now() < deadline) {
      await new Promise((r) => setTimeout(r, 20))
    }
    assert.ok(app.client.logs.some((e) => e.level === 'error' && e.text.includes('ta0-log-marker')))
    assert.match(fs.readFileSync(app.logFile, 'utf8'), /\[error\] ta0-log-marker/)
    failed = false
  } finally {
    await app.close({failed})
  }
})

test('shot() captures a PNG from a hidden window', async () => {
  const app = await startApp({name: 'shot'})
  let failed = true
  try {
    const info = ok(await app.client.call('info'))
    const {png} = ok(await app.client.call('shot'))
    const bytes = Buffer.from(png, 'base64')
    assert.equal(bytes.subarray(1, 4).toString('latin1'), 'PNG')
    // IHDR width and height, big-endian, at bytes 16..24.
    assert.equal(bytes.readUInt32BE(16), info.viewport.width)
    assert.equal(bytes.readUInt32BE(20), info.viewport.height)

    const out = path.join(app.artifactsDir, 'shot.png')
    fs.mkdirSync(app.artifactsDir, {recursive: true})
    const written = ok(await app.client.call('shot', [{outPath: out}]))
    assert.equal(written.bytes, fs.statSync(out).size)
    failed = false
  } finally {
    await app.close({failed})
  }
})

test('a renderer crash is detected and its artifacts are kept', async () => {
  const app = await startApp({name: 'crash'})
  try {
    app.client.send('Page.crash').catch(() => {})
    const reply = await app.client.call('info', [], {timeoutMs: 5000})
    assert.equal(reply.ok, false)
    assert.ok(app.crashed(), 'socket closed without close()')
  } finally {
    await app.close()
  }
  assert.ok(fs.existsSync(path.join(app.artifactsDir, 'cdp-log.json')))
  assert.ok(!fs.existsSync(app.profileDir), 'profile deleted after copying artifacts')
  fs.rmSync(app.artifactsDir, {recursive: true, force: true})
})
