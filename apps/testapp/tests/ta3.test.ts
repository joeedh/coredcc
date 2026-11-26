/**
 * TA3 (P4, docs/plans/2026-11-25-p4-tools.md): the transform tool's gizmo in the page. Find
 * the X arrow in the DrawList by its id, drag it 100 pixels with `input`, and check the
 * object against `worldMatrix` through undo and redo; hover highlights the arrow; Escape
 * cancels; and the rendered frame, read back from the GPU, shows the arrow where it projects.
 */
import assert from 'node:assert/strict'
import {after, afterEach, before, beforeEach, describe, test} from 'node:test'
import {type App, ok, startApp} from './harness.ts'

/** The transform gizmo's parts (tool/gizmos/gizmo_common.h). */
const PART_X = 0
const GIZMO_ID_UNIT = 65536

interface Rec {
  kind: string
  id: number
  color: number
  depth: string
  matrix?: number[]
}

describe('TA3: the transform gizmo in the page', () => {
  let app: App
  let failed = false
  let missing: string | undefined
  let objects: {a: {$ref: string}; b: {$ref: string}}

  const call = async (method: string, ...args: unknown[]) => ok(await app.client.call(method, args))
  /** Object a's place, from its world matrix. */
  const locationOfA = async (): Promise<number[]> =>
    ((await call('worldMatrix', objects.a)) as number[]).slice(12, 15).map((v) => Math.round(v * 1e9) / 1e9 + 0)
  const arrow = async (): Promise<Rec> => {
    await call('frame')
    const list = await call('drawList')
    const recs = (list.records as Rec[]).filter((r) => r.kind === 'shape' && r.id % GIZMO_ID_UNIT === PART_X && r.id >= GIZMO_ID_UNIT)
    assert.equal(recs.length, 1, 'one X arrow')
    return {...recs[0], matrix: recs[0].matrix!.map((v, i) => (i >= 12 && i < 15 ? v + list.origin[i - 12] : v))}
  }
  /** The pixel of the arrow's shaft at 70% of its length, which is on it, not on the tip. */
  const onArrow = async (r: Rec, along = 0.7): Promise<[number, number]> => {
    const m = r.matrix!
    const p = [m[12] + m[8] * along, m[13] + m[9] * along, m[14] + m[10] * along]
    const px = await call('project', p)
    assert.ok(px, 'the arrow is in front of the camera')
    return px as [number, number]
  }

  before(async () => {
    app = await startApp({name: 'ta3'})
    const info = ok(await app.client.call('info'))
    if (info.backendMissing && !process.env.COREDCC_TESTAPP_REQUIRE_BACKEND) missing = info.backendError
    else assert.equal(info.backend, 'wasm-st', `the backend failed to load: ${info.backendError}`)
  })
  after(async () => {
    await app?.close({failed})
  })
  beforeEach(async () => {
    if (missing !== undefined) return
    await call('reset')
    objects = await call('scene', 'gizmo')
  })
  afterEach(async () => {
    if (missing !== undefined) return
    const c = await call('check')
    if (!c.valid) failed = true
    assert.ok(c.valid, c.message)
  })
  const step = (name: string, fn: () => Promise<void>) =>
    test(name, async (t) => {
      if (missing !== undefined) return t.skip(`no backend: ${missing}`)
      try {
        await fn()
      } catch (e) {
        failed = true
        throw e
      }
    })

  step('a drag of the X arrow moves the object along x: one undo step, undone and redone', async () => {
    const before = (await call('history')).size
    const r = await arrow()
    const [x, y] = await onArrow(r)
    const tip = await call('project', [r.matrix![12] + r.matrix![8], r.matrix![13] + r.matrix![9], r.matrix![14] + r.matrix![10]])
    /* 100 pixels along the arrow's direction on the screen. */
    const [dx, dy] = [tip[0] - x, tip[1] - y]
    const len = Math.hypot(dx, dy)
    const [mx, my] = [x + (dx / len) * 100, y + (dy / len) * 100]
    const used = await call('input', [
      {type: 'pointerdown', x, y},
      {type: 'pointermove', x: (x + mx) / 2, y: (y + my) / 2},
      {type: 'pointermove', x: mx, y: my},
      {type: 'pointerup', x: mx, y: my},
    ])
    assert.deepEqual(used, [true, true, true, true])
    const moved = await locationOfA()
    assert.ok(moved[0] > 0.5, `moved along x: ${moved}`)
    assert.ok(Math.abs(moved[1]) < 1e-9 && Math.abs(moved[2]) < 1e-9, `only along x: ${moved}`)
    assert.equal((await call('history')).size, before + 1)
    await call('undo')
    assert.deepEqual(await locationOfA(), [0, 0, 0])
    await call('redo')
    assert.deepEqual(await locationOfA(), moved)
  })

  step('hover highlights the arrow, and Escape cancels a drag', async () => {
    const r = await arrow()
    const [x, y] = await onArrow(r)
    await call('input', [{type: 'pointermove', x, y, buttons: 0}])
    const hovered = await arrow()
    assert.ok(hovered.color >>> 24 > r.color >>> 24, `alpha ${hovered.color >>> 24} over ${r.color >>> 24}`)
    await call('input', [
      {type: 'pointerdown', x, y},
      {type: 'pointermove', x: x + 40, y: y + 10},
    ])
    assert.equal((await call('status')).interactive, 'core.object.dragDelta')
    assert.notDeepEqual(await locationOfA(), [0, 0, 0])
    await call('input', [{type: 'keydown', key: 0x110001}])
    assert.deepEqual(await locationOfA(), [0, 0, 0])
    assert.equal((await call('status')).interactive, null)
  })

  /* shot() can't be used: a hidden window doesn't composite frames drawn outside rAF, so it
   * shows an old one. `pixel` reads the rendered frame back from the GPU instead. */
  step('the rendered frame shows the arrow where it projects', async () => {
    const info = await call('info')
    if (!info.adapter.available) return /* no adapter here: the list is checked above */
    const r = await arrow()
    const [x, y] = await onArrow(r, 0.85)
    const [pr, pg, pb] = (await call('pixel', x, y)) as number[]
    const background = [0.16, 0.16, 0.18].map((c) => Math.round(c * 255))
    assert.ok(
      Math.abs(pr - background[0]) + Math.abs(pg - background[1]) + Math.abs(pb - background[2]) > 30,
      `the arrow's pixel (${pr}, ${pg}, ${pb}) is the background's (adapter: ${info.adapter.description || info.adapter.vendor}, fallback ${info.adapter.isFallbackAdapter})`
    )
    /* X is red: more red than green or blue. */
    assert.ok(pr > pg && pr > pb, `the arrow's pixel (${pr}, ${pg}, ${pb}) isn't red`)
  })
})
