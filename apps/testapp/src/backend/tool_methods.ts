/**
 * The testbus methods of tools and the overlay (TA3, docs/plans/2026-11-25-p4-tools.md):
 * scenes to test in, input that is dispatched at once (not through the DOM, so it doesn't
 * depend on focus or rAF), frames, the last DrawList, the camera and the active tool.
 */
import * as cdcc from 'coredcc-script'
import type {Context, DecodedDrawList, Ref, ToolEvent, ToolEventType, ViewSpec} from 'coredcc-script'
import type {Host} from 'coredcc-gen/testapp/Host'
import * as ta from 'coredcc-gen/testapp/functions'
import {OverlayRenderer, type GpuHooks} from '../overlay/renderer.ts'
import {mul, projectPoint, type Viewport} from '../overlay/tessellate.ts'

export type DefineMethod = (name: string, fn: (...args: any[]) => unknown) => void

/** A camera: where it is, what it looks at, z up, and a vertical field of view in radians. */
export interface Camera {
  eye: number[]
  target: number[]
  fov: number
}

const DEFAULT_CAMERA: Camera = {eye: [6, -8, 5], target: [0, 0, 0], fov: 0.8}
/** The canvas size when there is no adapter (and no canvas to measure). */
const FALLBACK_SIZE = {width: 800, height: 600}

const sub = (a: number[], b: number[]) => [a[0] - b[0], a[1] - b[1], a[2] - b[2]]
const cross = (a: number[], b: number[]) => [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]
const dot = (a: number[], b: number[]) => a[0] * b[0] + a[1] * b[1] + a[2] * b[2]
const unit = (a: number[]) => {
  const l = Math.hypot(a[0], a[1], a[2]) || 1
  return [a[0] / l, a[1] / l, a[2] / l]
}

/** World to view, column-major, looking down -z. */
function lookAt(c: Camera): number[] {
  const f = unit(sub(c.target, c.eye))
  const s = unit(cross(f, [0, 0, 1]))
  const u = cross(s, f)
  return [s[0], u[0], -f[0], 0, s[1], u[1], -f[1], 0, s[2], u[2], -f[2], 0, -dot(s, c.eye), -dot(u, c.eye), dot(f, c.eye), 1]
}

/** A right-handed perspective to depths 0..1. */
function perspective(fov: number, aspect: number, near: number, far: number): number[] {
  const f = 1 / Math.tan(fov / 2)
  return [f / aspect, 0, 0, 0, 0, f, 0, 0, 0, 0, far / (near - far), -1, 0, 0, (near * far) / (near - far), 0]
}

/** Registers the methods. Returns what ends a gesture and drops the scene, before the document
 * closes. */
export function installToolMethods(method: DefineMethod, ctx: () => Context, gpu?: GpuHooks): () => void {
  let camera = DEFAULT_CAMERA
  let last: DecodedDrawList | undefined
  let renderer: OverlayRenderer | undefined

  const size = () => {
    const g = gpu?.gpuState()
    const w = g?.canvas.clientWidth || FALLBACK_SIZE.width
    const h = g?.canvas.clientHeight || FALLBACK_SIZE.height
    return {width: w, height: h}
  }
  const viewport = (): Viewport => ({x: 0, y: 0, ...size(), dpiScale: 1})
  const view = (): ViewSpec => {
    const {width, height} = size()
    return {viewport: [0, 0, width, height], view: lookAt(camera), proj: perspective(camera.fov, width / height, 0.1, 1000)}
  }
  const viewProj = () => {
    const v = view()
    return mul(v.proj, v.view)
  }
  /* What a frame draws: the last list, if the backend made one. */
  const drawFrame = (_g: any, encoder: any, target: any) => {
    if (!renderer) renderer = new OverlayRenderer(gpu!.gpuState()!.device, gpu!.gpuState()!.format)
    renderer.render(encoder, target, last, viewProj(), viewport())
  }
  gpu?.setRenderer(drawFrame)

  /** `gizmo`: two boxes, `a` at the origin (selected and active) and `b` at (3, 0, 0); the
   * transform tool; the default camera. Returns the objects as Ref text. */
  method('scene', (name: string) => {
    if (name !== 'gizmo') throw new TypeError(`no scene '${name}' (there is 'gizmo')`)
    const c = ctx()
    const host = (c as unknown as {__host: Host}).__host
    cdcc.setScene(c, cdcc.newScene(c, 'Gizmo'))
    const box = (n: string, x: number) => {
      const data = ta.addBox(host, 0.5)
      if (!data) throw new Error('addBox failed')
      return cdcc.invoke(c, `core.object.add(name="${n}" data="${data}" location=(${x}, 0, 0))`).object as Ref
    }
    const a = box('a', 0)
    const b = box('b', 3)
    cdcc.invoke(c, `core.scene.select(objects="${cdcc.blockIdText(c, [a])}")`)
    cdcc.setActiveTool(c, 'core.tool.transform')
    camera = DEFAULT_CAMERA
    cdcc.evaluateScene(c)
    return {a: {$ref: a.text}, b: {$ref: b.text}}
  })
  /** Dispatches each event now, evaluating after each as a frame would. Whether each was used. */
  method('input', (events: (Partial<ToolEvent> & {type: ToolEventType})[]) => {
    const c = ctx()
    return events.map((e) => {
      const used = cdcc.sendToolEvent(c, view(), e)
      cdcc.evaluateScene(c)
      return used
    })
  })
  /** Evaluates, draws the tools' list and renders it; resolves when the GPU has. */
  method('frame', async () => {
    const c = ctx()
    cdcc.evaluateScene(c)
    const drawn = cdcc.drawTools(c, view())
    last = drawn.decoded /* what was drawn, even when a slot failed */
    const gpuError = await gpu?.renderNow()
    if (drawn.error) throw drawn.error
    if (gpuError) throw new Error(`frame: WebGPU: ${gpuError}`)
    return {records: last.records.length, rendered: !!gpu?.gpuState()}
  })
  /** The last frame's list, decoded; with `outPath`, written there as JSON too. */
  method('drawList', (opts: {outPath?: string} = {}) => {
    if (!last) throw new Error('no frame yet')
    if (opts.outPath) {
      const fs = (globalThis as any).require('fs') as typeof import('node:fs')
      fs.writeFileSync(opts.outPath, JSON.stringify(last))
    }
    return last
  })
  /** The camera; `canvas` is where the viewport is in the page, in CSS pixels (for shot()). */
  method('view', (set?: Partial<Camera>) => {
    if (set) camera = {...camera, ...set}
    const rect = gpu?.gpuState()?.canvas.getBoundingClientRect()
    return {camera, ...view(), canvas: rect ? {left: rect.left, top: rect.top} : {left: 0, top: 0}}
  })
  /** Makes `id` active, then sets its settings (by name, as tool settings paths take them). */
  method('tool', (id: string, settings: Record<string, unknown> = {}) => {
    const c = ctx()
    cdcc.setActiveTool(c, id)
    for (const [k, v] of Object.entries(settings)) cdcc.set(c, `toolSettings["${id}"].${k}`, v as cdcc.Writable)
    return cdcc.activeTool(c)
  })
  /** A world point's pixel, or null behind the eye. */
  method('project', (point: number[]) => projectPoint(viewProj(), viewport(), point) ?? null)
  /** Renders the last list and reads back canvas pixel (x, y) as RGBA, from the GPU; null without
   * an adapter. */
  method('pixel', async (x: number, y: number) => (await gpu?.readPixel(x, y)) ?? null)
  /** An object's world matrix, by `{$ref}`. */
  method('worldMatrix', (o: {$ref: string}) => cdcc.worldMatrix(ctx(), new cdcc.Ref(o.$ref)))
  method('status', () => ({tool: cdcc.activeTool(ctx()) ?? null, status: cdcc.toolStatus(ctx()), interactive: cdcc.interactiveId(ctx()) ?? null}))

  return () => {
    last = undefined
    try {
      cdcc.cancelTool(ctx())
    } catch {
      /* no document, or nothing to cancel */
    }
  }
}
