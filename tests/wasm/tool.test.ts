/**
 * Tools from TS (source/coredcc/bind/ts/tool.ts and drawlist.ts): a script tool that drags
 * through a modal, a script gizmo, the built-in transform tool driven as a viewport host
 * drives it, and decoding DrawLists. run.mjs builds and runs it.
 */
import assert from 'node:assert/strict'
import {afterEach, beforeEach, test} from 'node:test'
import {pathToFileURL} from 'node:url'

import * as cdcc from 'coredcc-script'
import {Ref, ScriptError, Session, StatusCode, type Context} from 'coredcc-script'
import type {Host} from 'coredcc-gen/testapp/Host'
import * as app from 'coredcc-gen/testapp/functions'

const create = (await import(pathToFileURL(process.env.SPIKE_MODULE as string).href)).default
cdcc.load(await create(), 'TESTAPP_GetManager')

const fails = (code: StatusCode, re: RegExp) => (e: unknown) =>
  e instanceof ScriptError && e.code === code && re.test(e.message)
const near = (a: ArrayLike<number>, b: ArrayLike<number>) => {
  assert.equal(a.length, b.length)
  for (let i = 0; i < a.length; i++) assert.ok(Math.abs(a[i] - b[i]) < 1e-9, `${Array.from(a)} isn't ${Array.from(b)}`)
}
const at = (x: number, y: number, z: number) => [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, x, y, z, 1]
/** An orthographic view of x and y from -10 to 10, 100 pixels wide: a pixel is 0.2 units, y down. */
const view: cdcc.ViewSpec = {
  viewport: [0, 0, 100, 100],
  view: at(0, 0, 0),
  proj: [0.1, 0, 0, 0, 0, 0.1, 0, 0, 0, 0, -0.05, 0, 0, 0, 0.5, 1],
  ortho: true,
}

let session: Session
let ctx: Context
beforeEach(() => {
  const host = app.create() as Host
  session = new Session(host, app.context(host) as Context)
  ctx = session.ctx
  cdcc.setScene(ctx, cdcc.newScene(ctx, 'Scene'))
})
afterEach(() => {
  if (!session.disposed) session[Symbol.dispose]()
})

const add = (args: string) => cdcc.invoke(ctx, `core.object.new(${args})`).object as Ref
const where = (o: Ref) => cdcc.worldMatrix(ctx, o).slice(12, 15)
const select = (...objects: Ref[]) => cdcc.invoke(ctx, `core.scene.select(objects="${cdcc.blockIdText(ctx, objects)}")`)
/** As a host's frame: the event, then an evaluation. */
const send = (event: Partial<cdcc.ToolEvent> & {type: cdcc.ToolEventType}) => {
  const used = cdcc.sendToolEvent(ctx, view, event)
  cdcc.evaluateScene(ctx)
  return used
}

/** Drags the selection along x by the pointer's distance from the press, in units of 0.2. */
const nudge: cdcc.ToolSpec = {
  id: 'ts.nudge',
  uiName: 'Nudge',
  settings: {step: cdcc.float(0.2), mode: {keys: ['FREE', 'SNAP']}},
  onEvent(ctx, event) {
    if (event.type !== 'pointerdown') return false
    const x0 = event.x
    cdcc.beginInteractive(ctx, 'core.object.dragDelta', {}, {lastUsed: false})
    cdcc.pushModal(ctx, {
      onEvent(ctx, e) {
        if (e.type === 'pointerup') return 'finish'
        if (e.type === 'keydown' && e.key === cdcc.Keys.escape) return 'cancel'
        cdcc.updateDrag(ctx, {vector: [(e.x - x0) * 0.2, 0, 0]})
        cdcc.setToolStatus(ctx, `moving ${cdcc.dragCount(ctx)}`)
        return 'continue'
      },
      end(_ctx, committed) {
        ended.push(committed)
      },
    })
    return true
  },
}
let ended: boolean[] = []

test('a script tool drags through a modal: one undo step, and a cancel', () => {
  ended = []
  const a = add('name="a" location=(1, 2, 0)')
  const b = add('name="b" location=(-3, 0, 0)')
  select(a, b)
  cdcc.defineTool(ctx, nudge)
  assert.throws(() => cdcc.defineTool(ctx, nudge), fails(StatusCode.AlreadyExists, /ts.nudge/))
  assert.throws(() => cdcc.defineTool(ctx, {...nudge, id: 'core.mine'}), fails(StatusCode.InvalidArgument, /core\./))
  cdcc.setActiveTool(ctx, 'ts.nudge')
  assert.equal(cdcc.activeTool(ctx), 'ts.nudge')
  cdcc.evaluateScene(ctx)

  assert.ok(send({type: 'pointerdown', x: 50, y: 50}))
  send({type: 'pointermove', x: 60, y: 50})
  near(where(a), [3, 2, 0])
  near(where(b), [-1, 0, 0])
  assert.equal(cdcc.toolStatus(ctx), 'moving 2')
  send({type: 'pointerup', x: 60, y: 50})
  assert.deepEqual(ended, [true])
  assert.equal(cdcc.interactiveId(ctx), undefined)
  assert.equal(cdcc.toolStatus(ctx), '')
  cdcc.undo(ctx)
  near(where(a), [1, 2, 0])

  send({type: 'pointerdown', x: 50, y: 50})
  send({type: 'pointermove', x: 55, y: 50})
  near(where(a), [2, 2, 0])
  send({type: 'keydown', key: cdcc.Keys.escape})
  assert.deepEqual(ended, [true, false])
  near(where(a), [1, 2, 0])
  assert.throws(() => cdcc.updateDrag(ctx, {vector: [1, 0, 0]}), fails(StatusCode.FailedPrecondition, /no open drag/))
})

test('a script gizmo: reads in update, a drag from beginDrag, its draw in the list', () => {
  const a = add('name="a" location=(0, 0, 0)')
  select(a)
  const seen: {read?: number[]; write?: unknown} = {}
  const gizmo: cdcc.GizmoSpec = {
    update(ctx) {
      seen.read = cdcc.worldMatrix(ctx, a).slice(12, 15)
      try {
        cdcc.invoke(ctx, 'core.scene.new')
      } catch (e) {
        seen.write = e
      }
    },
    hitTest: (_view, x, y) => (Math.hypot(x - 50, y - 50) < 5 ? {part: 0, distance: Math.hypot(x - 50, y - 50), viewDistance: 1} : undefined),
    draw(draw, _view, hover, _active, idBase) {
      draw.circle2d(50, 50, 5, {color: hover === 0 ? 0xff00ffff : 0xff0000ff, id: idBase}, true)
    },
    beginDrag(ctx) {
      cdcc.beginInteractive(ctx, 'core.object.dragDelta', {}, {lastUsed: false})
      cdcc.pushModal(ctx, {
        onEvent: (ctx, e) => {
          if (e.type === 'pointerup') return 'finish'
          cdcc.updateDragEach(ctx, [{vector: [0, (50 - e.y) * 0.2, 0]}])
          return 'continue'
        },
      })
    },
  }
  cdcc.defineTool(ctx, {
    id: 'ts.handle',
    activate(ctx) {
      cdcc.addGizmo(ctx, gizmo)
    },
    onEvent: () => false,
    drawOverlay(draw) {
      draw.lines([0, 0, 0, 1, 0, 0], {width: 2, depthMode: 'always'})
    },
  })
  cdcc.setActiveTool(ctx, 'ts.handle')
  cdcc.evaluateScene(ctx)
  send({type: 'pointermove', x: 51, y: 50})
  near(seen.read!, [0, 0, 0])
  assert.ok(fails(StatusCode.Busy, /Gizmo\.update/)(seen.write), String(seen.write))

  const {bytes, decoded} = cdcc.drawTools(ctx, view)
  const circle = decoded.records.find((r) => r.kind === 'circle2d')
  assert.ok(circle && circle.filled && circle.color === 0xff00ffff && circle.id > 0)
  const line = decoded.records.find((r) => r.kind === 'lines')
  assert.ok(line && line.depthMode === 'always' && line.width === 2)
  assert.equal(new DataView(bytes.buffer, bytes.byteOffset).getUint32(0, true), cdcc.kDrawListMagic)

  send({type: 'pointerdown', x: 50, y: 50})
  send({type: 'pointermove', x: 50, y: 40})
  send({type: 'pointerup', x: 50, y: 40})
  near(where(a), [0, 2, 0])
  cdcc.undo(ctx)
  near(where(a), [0, 0, 0])
})

test('the built-in transform tool draws its gizmo, and decodes', () => {
  const a = add('name="a" location=(2, 0, 0)')
  select(a)
  cdcc.setActiveTool(ctx, 'core.tool.transform')
  cdcc.evaluateScene(ctx)
  const {decoded} = cdcc.drawTools(ctx, view)
  const shapes = decoded.records.filter((r) => r.kind === 'shape' || r.kind === 'pushMatrix')
  assert.ok(shapes.length > 0, 'no gizmo')
  assert.ok(decoded.records.every((r) => r.id === 0 || r.id >= 65536))
})

test('decodeDrawList refuses what is not a list', () => {
  assert.throws(() => cdcc.decodeDrawList(new Uint8Array(8)), /no header/)
  const bad = new Uint8Array(48)
  assert.throws(() => cdcc.decodeDrawList(bad), /not a DrawList/)
  const dv = new DataView(bad.buffer)
  dv.setUint32(0, cdcc.kDrawListMagic, true)
  dv.setUint16(4, 1, true)
  dv.setUint32(8, 1, true)
  dv.setUint32(12, 48, true)
  assert.throws(() => cdcc.decodeDrawList(bad), /a record count of 1 for 0 records at byte 8/)
  dv.setUint32(8, 0, true)
  assert.deepEqual(cdcc.decodeDrawList(bad), {origin: [0, 0, 0], records: []})
})

test('a TS stroke brush through core.tool.stroke: samples, one undo step, and a replay', () => {
  const a = add('name="a"')
  select(a)
  const calls: string[] = []
  const samples: cdcc.StrokeSample[] = []
  let target: cdcc.StrokeTarget | undefined
  cdcc.defineStrokeBrush(ctx, 'ts.brush', () => ({
    begin(_ctx, t) {
      calls.push('begin')
      target = t
    },
    apply(ctx, s) {
      calls.push('apply')
      samples.push(s)
      assert.throws(() => cdcc.invoke(ctx, 'core.scene.new'), fails(StatusCode.Busy, /StrokeBrush.apply/))
    },
    undo() {
      calls.push('undo')
    },
  }))
  assert.throws(() => cdcc.defineStrokeBrush(ctx, 'ts.brush', () => ({apply() {}, undo() {}})),
    fails(StatusCode.AlreadyExists, /ts.brush/))
  cdcc.set(ctx, 'toolSettings["core.tool.stroke"].brush', 'ts.brush')
  cdcc.setActiveTool(ctx, 'core.tool.stroke')
  send({type: 'pointerdown', x: 20, y: 50, buttons: 1})
  for (let x = 30; x <= 80; x += 10) send({type: 'pointermove', x, y: 50, buttons: 1})
  send({type: 'pointerup', x: 80, y: 50})
  assert.equal(calls[0], 'begin')
  assert.equal(String(target?.object), String(a))
  // 60 pixels at the default spacing: a sample every 0.1 × 2 × 25 = 5 pixels.
  assert.equal(samples.length, 13)
  assert.deepEqual([samples[0].x, samples[0].interpolated], [20, false])
  for (let i = 1; i < samples.length; i++) assert.ok(Math.abs(samples[i].x - samples[i - 1].x - 5) < 1e-9)
  assert.ok(samples.every((s) => s.y === 50 && !s.hit && s.mirror === 0))
  calls.length = 0
  cdcc.undo(ctx)
  assert.deepEqual(calls, ['undo'])
  calls.length = 0
  const before = samples.length
  cdcc.redo(ctx)
  assert.equal(calls.length, 1 + before)
  near(samples.slice(before).map((s) => s.x), samples.slice(0, before).map((s) => s.x))
})

test('decodeDrawList checks what DrawListReader checks', () => {
  // The same changes as tests/draw/test_drawlist.cc's testRules, and the C++ reader's other rules.
  cdcc.defineTool(ctx, {
    id: 'ts.records',
    onEvent: () => false,
    drawOverlay(draw) {
      draw.points([1, 2, 3])
      draw.pushMatrix(at(1, 0, 0))
      draw.shape('cube', at(0, 1, 0))
      draw.popMatrix()
      draw.circle2d(10, 10, 4)
    },
  })
  cdcc.setActiveTool(ctx, 'ts.records')
  const {bytes} = cdcc.drawTools(ctx, view)
  const dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength)
  const record: Record<number, number> = {} // the first record of each kind, by its number
  for (let o = 48; o < bytes.length; o += dv.getUint32(o + 4, true)) record[dv.getUint16(o, true)] ??= o
  const [points, shape, push, circle] = [record[2], record[4], record[7], record[5]]
  const refused = (set: (d: DataView) => void, re: RegExp) => {
    const b = bytes.slice()
    set(new DataView(b.buffer))
    assert.throws(() => cdcc.decodeDrawList(b), re)
  }
  refused((d) => d.setUint8(6, 1), /header flags/)
  refused((d) => d.setUint8(40, 1), /reserved/)
  refused((d) => d.setUint8(points + 24 + 12, 1), /padding/)
  refused((d) => d.setFloat64(shape + 24 + 8 * 3, 0.5, true), /affine/)
  refused((d) => d.setFloat64(push + 24 + 8 * 15, 2, true), /affine/)
  refused((d) => d.setFloat32(circle + 24 + 8, -1, true), /negative radius/)
  refused((d) => d.setFloat32(points + 24, NaN, true), /isn't finite/)
  refused((d) => d.setFloat32(points + 12, -1, true), /width/)
  refused((d) => d.setUint8(points + 3, 1), /flags that this kind/)
  refused((d) => d.setUint32(points + 20, 2, true), /doesn't fit its count/)
  assert.equal(cdcc.decodeDrawList(bytes).records.length, 5)
})

test('a slot that fails: the rest of the frame, its error alongside, and a Drawer that closes', () => {
  let kept: cdcc.Drawer | undefined
  cdcc.defineTool(ctx, {
    id: 'ts.throws',
    onEvent: () => false,
    drawOverlay(draw) {
      kept = draw
      draw.lines([0, 0, 0, 1, 0, 0])
      throw new Error('boom')
    },
  })
  cdcc.setActiveTool(ctx, 'ts.throws')
  const {decoded, error} = cdcc.drawTools(ctx, view)
  assert.ok(error && /boom/.test(error.message), String(error))
  assert.equal(decoded.records.filter((r) => r.kind === 'lines').length, 1)
  assert.throws(() => kept!.points([0, 0, 0]), /during the slot/)
})

test('sendToolEvent: a failed gizmo update after the event was used; an outcome text; a gizmo with its own settings', () => {
  const a = add('name="a"')
  select(a)
  let outcome = false
  cdcc.defineTool(ctx, {
    id: 'ts.late',
    settings: {mode: {keys: ['TRANSLATE', 'ROTATE', 'SCALE']}},
    activate(ctx) {
      cdcc.addGizmo(ctx, {
        update() {
          throw new Error('stale')
        },
        hitTest: () => undefined,
        draw() {},
        beginDrag() {},
      })
      assert.throws(() => cdcc.addGizmo(ctx, {kind: 'transform', settingsOf: 'ts.none'}), fails(StatusCode.NotFound, /ts.none/))
      cdcc.addGizmo(ctx, {kind: 'transform', settingsOf: 'ts.late'})
    },
    onEvent(ctx, e) {
      if (e.type !== 'keydown') return e.type === 'pointermove'
      cdcc.pushModal(ctx, {
        onEvent(ctx, e) {
          if (e.type !== 'keyup') return 'continue'
          if (outcome) cdcc.setToolOutcome(ctx, 'nothing to do')
          return 'finish'
        },
      }, {ownsInteractive: false})
      return true
    },
  })
  cdcc.setActiveTool(ctx, 'ts.late')
  cdcc.evaluateScene(ctx)
  assert.throws(() => cdcc.sendToolEvent(ctx, view, {type: 'pointermove', x: 1, y: 1}),
    (e: unknown) => e instanceof ScriptError && /stale/.test(e.message) && (e as cdcc.ToolEventError).used === true)
  outcome = true
  send({type: 'keydown', key: 'g'.codePointAt(0)})
  cdcc.setToolStatus(ctx, 'grabbing')
  send({type: 'keyup', key: 'g'.codePointAt(0)})
  assert.equal(cdcc.toolStatus(ctx), 'nothing to do')
})
