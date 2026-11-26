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

const add = (args: string) => cdcc.invoke(ctx, `core.object.add(${args})`).object as Ref
const where = (o: Ref) => cdcc.worldMatrix(ctx, o).slice(12, 15)
const select = (...objects: Ref[]) => cdcc.invoke(ctx, `core.scene.select(objects="${cdcc.blockIds(ctx, objects)}")`)
/** As a host's frame: the event, then an evaluation. */
const send = (event: Partial<cdcc.ToolEvent> & {type: cdcc.ToolEventType}) => {
  const used = cdcc.sendEvent(ctx, view, event)
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
        cdcc.dragUpdate(ctx, {vector: [(e.x - x0) * 0.2, 0, 0]})
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
  assert.equal(cdcc.interactive(ctx), undefined)
  assert.equal(cdcc.toolStatus(ctx), '')
  cdcc.undo(ctx)
  near(where(a), [1, 2, 0])

  send({type: 'pointerdown', x: 50, y: 50})
  send({type: 'pointermove', x: 55, y: 50})
  near(where(a), [2, 2, 0])
  send({type: 'keydown', key: cdcc.Keys.escape})
  assert.deepEqual(ended, [true, false])
  near(where(a), [1, 2, 0])
  assert.throws(() => cdcc.dragUpdate(ctx, {vector: [1, 0, 0]}), fails(StatusCode.FailedPrecondition, /no open drag/))
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
    hitTest: (_view, x, y) => (Math.hypot(x - 50, y - 50) < 5 ? {part: 0, distance: Math.hypot(x - 50, y - 50), depth: 1} : undefined),
    draw(draw, _view, hover, _active, idBase) {
      draw.circle2d(50, 50, 5, {color: hover === 0 ? 0xff00ffff : 0xff0000ff, id: idBase}, true)
    },
    beginDrag(ctx) {
      cdcc.beginInteractive(ctx, 'core.object.dragDelta', {}, {lastUsed: false})
      cdcc.pushModal(ctx, {
        onEvent: (ctx, e) => {
          if (e.type === 'pointerup') return 'finish'
          cdcc.dragUpdateEach(ctx, [{vector: [0, (50 - e.y) * 0.2, 0]}])
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
      draw.lines([0, 0, 0, 1, 0, 0], {width: 2, depth: 'always'})
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
  assert.ok(line && line.depth === 'always' && line.width === 2)
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
  const shapes = decoded.records.filter((r) => r.kind === 'shape' || r.kind === 'push')
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
  assert.throws(() => cdcc.decodeDrawList(bad), /past the end at byte 48/)
  dv.setUint32(8, 0, true)
  assert.deepEqual(cdcc.decodeDrawList(bad), {origin: [0, 0, 0], records: []})
})
