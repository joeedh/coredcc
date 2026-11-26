/**
 * Tools for scripts (bind/tool_facades.h): defining tools, modals and gizmos as plain
 * objects, the script half of drags, and driving the context's ToolManager as a viewport
 * host does. Exported by index.ts, whose rules hold here too.
 *
 * Events are plain objects (`ToolEvent`). A tool's `onEvent`, a modal's `onEvent` and a
 * gizmo's `beginDrag` may do what a script's top level may: run commands, begin a drag,
 * push a modal. Draw and hit-test slots, and a gizmo's `update`, may only read.
 *
 * During a slot the context's ToolManager is the one that called it, so `pushModal`,
 * `addGizmo` and the rest act on that viewport's. What a slot is handed (the view, the
 * `Drawer`) is valid during the call only: a Drawer kept for later throws.
 */
import {getBindingBackend} from '@litestl/typescript-runtime'
import type {Context} from 'coredcc-gen/coredcc/Context'
import type {DrawList} from 'coredcc-gen/coredcc/DrawList'
import {DeltaKind} from 'coredcc-gen/coredcc/DeltaKind'
import {EventResult} from 'coredcc-gen/coredcc/EventResult'
import {EventType} from 'coredcc-gen/coredcc/EventType'
import * as cd from 'coredcc-gen/coredcc/functions'
import type {GizmoHit as BoundHit} from 'coredcc-gen/coredcc/GizmoHit'
import {Handedness} from 'coredcc-gen/coredcc/Handedness'
import type {InputEvent as BoundEvent} from 'coredcc-gen/coredcc/InputEvent'
import {ModalResult as BoundModalResult} from 'coredcc-gen/coredcc/ModalResult'
import {PointerType} from 'coredcc-gen/coredcc/PointerType'
import type {Status} from 'coredcc-gen/coredcc/Status'
import type {StrokeSample as BoundSample} from 'coredcc-gen/coredcc/StrokeSample'
import type {StrokeTarget as BoundTarget} from 'coredcc-gen/coredcc/StrokeTarget'
import {StatusCode} from 'coredcc-gen/coredcc/StatusCode'
import type {TransformDelta as BoundDelta} from 'coredcc-gen/coredcc/TransformDelta'
import {UpAxis} from 'coredcc-gen/coredcc/UpAxis'
import type {View as BoundView} from 'coredcc-gen/coredcc/View'
import type {ViewMath} from 'coredcc-gen/coredcc/ViewMath'

import {check, checkCtx, inSlot, keyOf, isFatal, Ref, ScriptError, statusOf, sync, take, withValue, type Writable} from './core.ts'
import {decodeDrawList, Drawer, readDrawList, type DecodedDrawList} from './drawlist.ts'
import type {TransformDelta, ViewSpec} from './scene.ts'

export type {ViewMath}

/* ---- events ---- */

export type ToolEventType =
  | 'pointerdown' | 'pointermove' | 'pointerup' | 'wheel' | 'keydown' | 'keyup' | 'cancel' | 'focuslost'
/* DOM's names, not the C++ items. */
const eventTypes = new Map<EventType, ToolEventType>([
  [EventType.PointerDown, 'pointerdown'], [EventType.PointerMove, 'pointermove'], [EventType.PointerUp, 'pointerup'],
  [EventType.Wheel, 'wheel'], [EventType.KeyDown, 'keydown'], [EventType.KeyUp, 'keyup'],
  [EventType.Cancel, 'cancel'], [EventType.FocusLost, 'focuslost'],
])
const pointerTypes = new Map<PointerType, 'mouse' | 'pen' | 'touch'>([
  [PointerType.Mouse, 'mouse'], [PointerType.Pen, 'pen'], [PointerType.Touch, 'touch'],
])

/** Bits of `ToolEvent.modifiers`. */
export const Modifiers = {shift: 1, ctrl: 2, alt: 4, meta: 8} as const
/** Keys that aren't characters (a character is its code point without Shift: 'g'.codePointAt(0)). */
export const Keys = {
  escape: 0x110001, enter: 0x110002, tab: 0x110003, backspace: 0x110004, delete: 0x110005,
  left: 0x110006, right: 0x110007, up: 0x110008, down: 0x110009, home: 0x11000a, end: 0x11000b,
  pageUp: 0x11000c, pageDown: 0x11000d, shift: 0x11000e, ctrl: 0x11000f, alt: 0x110010, meta: 0x110011,
  f1: 0x110020,
} as const

export interface PointerSample {
  x: number
  y: number
  pressure: number
  tiltX: number
  tiltY: number
  twist: number
  time: number
}

/** What coredcc/tool/input.h's InputEvent says. Positions in device pixels, y down. */
export interface ToolEvent extends PointerSample {
  type: ToolEventType
  /** The button that went down or up: 0 left, 1 middle, 2 right. */
  button: number
  /** Held, as DOM's `buttons`: 1 left, 2 right, 4 middle. */
  buttons: number
  pointer: 'mouse' | 'pen' | 'touch'
  pointerId: number
  wheelX: number
  wheelY: number
  key: number
  repeat: boolean
  modifiers: number
  /** A move's coalesced positions, oldest first; the event's own is the newest. */
  samples: PointerSample[]
}

const sampleOf = (s: {x: number; y: number; pressure: number; tiltX: number; tiltY: number; twist: number; time: number}) => ({
  x: s.x, y: s.y, pressure: s.pressure, tiltX: s.tiltX, tiltY: s.tiltY, twist: s.twist, time: s.time,
})

function eventOf(e: BoundEvent): ToolEvent {
  const samples: PointerSample[] = []
  for (let i = 0; i < e.sampleCount(); i++) {
    const s = e.sampleAt(i)
    samples.push(sampleOf(s))
    ;(s as unknown as {[Symbol.dispose]?: () => void})[Symbol.dispose]?.()
  }
  return {
    ...sampleOf(e),
    type: eventTypes.get(e.type)!,
    button: e.button,
    buttons: e.buttons,
    pointer: pointerTypes.get(e.pointer)!,
    pointerId: e.pointerId,
    wheelX: e.wheelX,
    wheelY: e.wheelY,
    key: e.key,
    repeat: e.repeat,
    modifiers: e.modifiers,
    samples,
  }
}

function withEvent<R>(event: Partial<ToolEvent> & {type: ToolEventType}, fn: (e: BoundEvent) => R): R {
  const backend = getBindingBackend()
  const e = backend.construct('coredcc::InputEvent') as BoundEvent
  try {
    const type = keyOf(eventTypes, event.type)
    if (type === undefined) throw new TypeError(`ToolEvent: no type '${event.type}'`)
    e.type = type
    e.pointer = keyOf(pointerTypes, event.pointer ?? 'mouse') ?? PointerType.Mouse
    e.x = event.x ?? 0
    e.y = event.y ?? 0
    e.button = event.button ?? 0
    e.buttons = event.buttons ?? (event.type === 'pointerdown' ? 1 : 0)
    e.pointerId = event.pointerId ?? 0
    e.pressure = event.pressure ?? 0.5
    e.tiltX = event.tiltX ?? 0
    e.tiltY = event.tiltY ?? 0
    e.twist = event.twist ?? 0
    e.wheelX = event.wheelX ?? 0
    e.wheelY = event.wheelY ?? 0
    e.key = event.key ?? 0
    e.repeat = event.repeat ?? false
    e.modifiers = event.modifiers ?? 0
    e.time = event.time ?? 0
    for (const s of event.samples ?? []) {
      const b = backend.construct('coredcc::PointerSample') as Parameters<BoundEvent['addSample']>[0]
      try {
        Object.assign(b, sampleOf(s))
        e.addSample(b)
      } finally {
        ;(b as unknown as {[Symbol.dispose](): void})[Symbol.dispose]()
      }
    }
    return fn(e)
  } finally {
    e[Symbol.dispose]()
  }
}

/* ---- views (as scene.ts's, which keeps its own private) ---- */

function fill(what: string, e: {put(i: number, v: number): void}, values: ArrayLike<number>, count: number) {
  if (values.length !== count) throw new TypeError(`${what} has ${count} numbers, not ${values.length}`)
  for (let i = 0; i < count; i++) e.put(i, values[i])
}

function withView<R>(spec: ViewSpec, fn: (v: BoundView) => R): R {
  const v = getBindingBackend().construct('coredcc::View') as BoundView
  try {
    fill('viewport', {put: (i, x) => v.putViewport(i, x)}, spec.viewport, 4)
    fill('view', v.view, spec.view, 16)
    fill('proj', v.proj, spec.proj, 16)
    v.setConventions(spec.depthRange === '-1..1' ? 1 : 0)
    v.handedness = spec.handedness === 'left' ? Handedness.Left : Handedness.Right
    v.up = spec.up === 'y' ? UpAxis.Y : UpAxis.Z
    v.reversedZ = spec.reversedZ ?? false
    v.infiniteFar = spec.infiniteFar ?? false
    v.ortho = spec.ortho ?? false
    v.dpiScale = spec.dpiScale ?? 1
    return fn(v)
  } finally {
    v[Symbol.dispose]()
  }
}

/* ---- deltas ---- */

const deltaKinds = new Map<DeltaKind, TransformDelta['kind']>([
  [DeltaKind.Translate, 'translate'], [DeltaKind.Rotate, 'rotate'], [DeltaKind.Scale, 'scale'],
])

function putDelta(d: BoundDelta, delta: Partial<TransformDelta>) {
  const kind = keyOf(deltaKinds, delta.kind ?? 'translate')
  if (kind === undefined) throw new TypeError(`TransformDelta: no kind '${delta.kind}'`)
  d.kind = kind
  fill('basis', d.basis, delta.basis ?? [0, 0, 0, 1], 4)
  const vec = (what: string, v: Parameters<typeof cd.vec3Put>[0], values: ArrayLike<number>) => {
    if (values.length !== 3) throw new TypeError(`${what} has 3 numbers, not ${values.length}`)
    for (let i = 0; i < 3; i++) cd.vec3Put(v, i, values[i])
  }
  vec('pivot', d.pivot, delta.pivot ?? [0, 0, 0])
  vec('vector', d.vector, delta.vector ?? (kind === 2 ? [1, 1, 1] : [0, 0, 0]))
  vec('axis', d.axis, delta.axis ?? [0, 0, 1])
  d.angle = delta.angle ?? 0
}

function withDelta<R>(delta: Partial<TransformDelta>, fn: (d: BoundDelta) => R): R {
  const d = getBindingBackend().construct('coredcc::TransformDelta') as BoundDelta
  try {
    putDelta(d, delta)
    return fn(d)
  } finally {
    d[Symbol.dispose]()
  }
}

/** The open core.object.dragDelta's objects move by `delta`, the total since the drag began. */
export function updateDrag(ctx: Context, delta: Partial<TransformDelta>) {
  withDelta(delta, (d) => check(cd.updateDrag(ctx, d)))
}
/** The same, with one delta for each of the drag's objects (`dragCount`), in its order. */
export function updateDragEach(ctx: Context, deltas: readonly Partial<TransformDelta>[]) {
  const numbers: number[] = []
  for (const delta of deltas) {
    const kind = keyOf(deltaKinds, delta.kind ?? 'translate')
    if (kind === undefined) throw new TypeError(`TransformDelta: no kind '${delta.kind}'`)
    const vector = delta.vector ?? (kind === DeltaKind.Scale ? [1, 1, 1] : [0, 0, 0])
    numbers.push(kind, ...(delta.basis ?? [0, 0, 0, 1]), ...(delta.pivot ?? [0, 0, 0]), ...vector,
      ...(delta.axis ?? [0, 0, 1]), delta.angle ?? 0, 0)
  }
  check(cd.updateDragEach(ctx, numbers))
}
/** The open core.element.dragDelta's points move by `delta`. */
export function updateElementsDrag(ctx: Context, delta: Partial<TransformDelta>) {
  withDelta(delta, (d) => check(cd.updateElementsDrag(ctx, d)))
}
/** How many objects the open drag moves. */
export function dragCount(ctx: Context): number {
  return take(cd.dragCount(ctx), Number)
}

/* ---- tools ---- */

export interface ToolSpec {
  /** Dotted. The `core.` namespace is reserved. */
  id: string
  uiName?: string
  description?: string
  /** Settings by name, typed by their defaults as a command's inputs; `{keys}` for an enum. */
  settings?: Record<string, Writable | {keys: readonly string[]}>
  /** The settings panel, as layout XML. */
  layout?: string
  activate?(ctx: Context): void
  deactivate?(ctx: Context): void
  /** Return true when the event was used. */
  onEvent(ctx: Context, event: ToolEvent, view: ViewMath): boolean | void
  drawOverlay?(draw: Drawer, view: ViewMath): void
  settingsChanged?(ctx: Context): void
}

const isKeys = (v: unknown): v is {keys: readonly string[]} =>
  typeof v === 'object' && v !== null && Array.isArray((v as {keys?: unknown}).keys)

/* A throw from a draw slot is reported (the Runtime's sticky error); the manager keeps the drag.
 * The Drawer is closed when the slot returns. */
function guarded(what: string, out: DrawList, fn: (draw: Drawer) => void) {
  const draw = new Drawer(out)
  try {
    inSlot(() => sync(what, fn(draw)))
  } finally {
    draw.close()
  }
}

class ToolAdapter {
  constructor(private spec: ToolSpec) {}
  activate(ctx: Context): Status | undefined {
    return inSlot(() => {
      try {
        sync('activate', this.spec.activate?.(ctx))
        return undefined
      } catch (e) {
        return statusOf(StatusCode.Internal, e)
      }
    })
  }
  deactivate(ctx: Context) {
    inSlot(() => sync('deactivate', this.spec.deactivate?.(ctx)))
  }
  onEvent(ctx: Context, _tm: unknown, view: ViewMath, e: BoundEvent): EventResult {
    return inSlot(() =>
      sync('onEvent', this.spec.onEvent(ctx, eventOf(e), view)) ? EventResult.Handled : EventResult.Unhandled
    )
  }
  drawOverlay(view: ViewMath, out: DrawList) {
    guarded('drawOverlay', out, (draw) => this.spec.drawOverlay!(draw, view))
  }
  settingsChanged(ctx: Context) {
    inSlot(() => sync('settingsChanged', this.spec.settingsChanged?.(ctx)))
  }
}

/** Registers tool `spec.id`. Every activation calls the same `spec`: state kept on it outlives
 * an activation, so reset it in `activate`. */
export function defineTool(ctx: Context, spec: ToolSpec) {
  checkCtx(ctx)
  if (typeof spec.onEvent !== 'function') throw new TypeError(`defineTool: ${spec.id}: onEvent must be a function`)
  const def = cd.newToolDef(spec.id)
  if (!def) throw new Error(`defineTool: no def for ${spec.id}`)
  try {
    if (spec.uiName !== undefined) check(cd.toolDefUiName(def, spec.uiName))
    if (spec.description !== undefined) check(cd.toolDefDescription(def, spec.description))
    for (const [name, v] of Object.entries(spec.settings ?? {})) {
      if (isKeys(v)) check(cd.toolDefEnumSetting(def, name, v.keys.join(' ')))
      else withValue(ctx, v, (pv) => check(cd.toolDefSetting(def, name, pv)))
    }
    if (spec.layout !== undefined) check(cd.toolDefLayout(def, spec.layout))
    const slots = ['activate', 'onEvent']
    if (spec.deactivate) slots.push('deactivate')
    if (spec.drawOverlay) slots.push('drawOverlay')
    if (spec.settingsChanged) slots.push('settingsChanged')
    const backend = getBindingBackend()
    const factory = {make: () => backend.foreignHandle(new ToolAdapter(spec))}
    check(cd.registerForeignTool(ctx, def, backend.foreignHandle(factory), slots.join(' ')))
  } finally {
    def[Symbol.dispose]()
  }
}

/* ---- modals ---- */

export type ModalResult = 'continue' | 'passThrough' | 'finish' | 'cancel'
const modalResults = new Map<BoundModalResult, ModalResult>([
  [BoundModalResult.Continue, 'continue'], [BoundModalResult.PassThrough, 'passThrough'],
  [BoundModalResult.Finish, 'finish'], [BoundModalResult.Cancel, 'cancel'],
])

export interface ModalSpec {
  /** 'finish' commits the drag it owns, 'cancel' cancels it; 'passThrough' keeps the grab and lets the host
   * have the event. A throw cancels. */
  onEvent(ctx: Context, event: ToolEvent, view: ViewMath): ModalResult
  drawOverlay?(draw: Drawer, view: ViewMath): void
  /** It is gone; `committed` says whether its drag was committed. */
  end?(ctx: Context, committed: boolean): void
}

class ModalAdapter {
  constructor(private spec: ModalSpec) {}
  onEvent(ctx: Context, _tm: unknown, view: ViewMath, e: BoundEvent): BoundModalResult {
    return inSlot(() => {
      const r = sync('onEvent', this.spec.onEvent(ctx, eventOf(e), view))
      const n = keyOf(modalResults, r)
      if (n === undefined) {
        throw new TypeError(`a modal's onEvent returned '${String(r)}', not one of ${[...modalResults.values()].join(', ')}`)
      }
      return n
    })
  }
  drawOverlay(view: ViewMath, out: DrawList) {
    guarded('drawOverlay', out, (draw) => this.spec.drawOverlay!(draw, view))
  }
  end(ctx: Context, committed: boolean) {
    inSlot(() => sync('end', this.spec.end?.(ctx, committed)))
  }
}

/** From a tool's or a gizmo's event slot: `modal` gets every event until it finishes. With
 * `ownsInteractive` (the default), the open drag is its: committed or cancelled when it ends. */
export function pushModal(ctx: Context, modal: ModalSpec, opts: {ownsInteractive?: boolean} = {}) {
  const slots = ['onEvent']
  if (modal.drawOverlay) slots.push('drawOverlay')
  if (modal.end) slots.push('end')
  const handle = getBindingBackend().foreignHandle(new ModalAdapter(modal))
  check(cd.pushModal(ctx, handle, slots.join(' '), opts.ownsInteractive ?? true))
}

/* ---- gizmos ---- */

export interface GizmoHit {
  part: number
  /** Logical pixels from the cursor. */
  distance: number
  /** From the near plane, along the view direction, in world units. */
  viewDistance: number
}

export interface GizmoSpec {
  /** Places it from the document before each hit test and draw, outside gestures. Reads only. */
  update?(ctx: Context): void
  /** The part nearest to the pixel, or undefined for none. */
  hitTest(view: ViewMath, x: number, y: number): GizmoHit | undefined
  /** `hover` and `active` are parts or -1; a record's id is `idBase + part`. */
  draw(draw: Drawer, view: ViewMath, hover: number, active: number, idBase: number): void
  /** A press on `part`: begins the drag and pushes a modal. */
  beginDrag(ctx: Context, view: ViewMath, event: ToolEvent, part: number): void
}

class GizmoAdapter {
  private hit = getBindingBackend().construct('coredcc::GizmoHit') as BoundHit
  constructor(private spec: GizmoSpec) {}
  update(ctx: Context) {
    inSlot(() => sync('update', this.spec.update?.(ctx)))
  }
  hitTest(view: ViewMath, x: number, y: number): BoundHit {
    return inSlot(() => {
      const h = sync('hitTest', this.spec.hitTest(view, x, y))
      this.hit.part = h ? h.part : -1
      this.hit.distance = h?.distance ?? 0
      this.hit.viewDistance = h?.viewDistance ?? 0
      return this.hit
    })
  }
  draw(view: ViewMath, hover: number, active: number, idBase: number, out: DrawList) {
    guarded('draw', out, (draw) => this.spec.draw(draw, view, hover, active, idBase))
  }
  beginDrag(ctx: Context, _tm: unknown, view: ViewMath, e: BoundEvent, part: number): Status | undefined {
    return inSlot(() => {
      try {
        sync('beginDrag', this.spec.beginDrag(ctx, view, eventOf(e), part))
        return undefined
      } catch (e) {
        return statusOf(StatusCode.Internal, e)
      }
    })
  }
}

/** The built-in transform gizmo, following the settings of tool `settingsOf` (mode, pivot,
 * orientation, target and the rest, by core.tool.transform's names; a missing one is its default). */
export interface BuiltinGizmo {
  kind: 'transform'
  settingsOf?: string
}

/** From a tool's slot: a gizmo. `'transform'` is the built-in one with core.tool.transform's
 * settings. Its id. */
export function addGizmo(ctx: Context, gizmo: 'transform' | BuiltinGizmo | GizmoSpec): number {
  if (typeof gizmo === 'string') return take(cd.addGizmo(ctx, gizmo, ''), Number)
  if ('kind' in gizmo) return take(cd.addGizmo(ctx, gizmo.kind, gizmo.settingsOf ?? ''), Number)
  const handle = getBindingBackend().foreignHandle(new GizmoAdapter(gizmo))
  return take(cd.addForeignGizmo(ctx, handle, 'update hitTest draw beginDrag'), Number)
}
export function removeGizmo(ctx: Context, id: number) {
  check(cd.removeGizmo(ctx, id))
}

/* ---- stroke brushes ---- */

/** One sample of a stroke (coredcc/tool/stroke.h's StrokeSample): device pixels, world points. */
export interface StrokeSample {
  x: number
  y: number
  world: number[]
  normal: number[]
  hit: boolean
  interpolated: boolean
  pressure: number
  tiltX: number
  tiltY: number
  twist: number
  time: number
  radiusPixels: number
  radiusWorld: number
  distance: number
  step: number
  angle: number
  /** Anchored strokes: from the anchor to the cursor. */
  dragX: number
  dragY: number
  dragWorld: number[]
  /** The mirror axes of this copy (bits 1, 2, 4); 0 for the stroke itself. */
  mirror: number
}

/** The object a stroke is on, its world matrix (column-major) and its mirror axes. */
export interface StrokeTarget {
  object: Ref
  world: number[]
  mirrorAxes: number
}

/**
 * One stroke's brush. `begin` and `apply` may write values (`write`), not run commands;
 * `undo` puts back what they wrote. `begin` runs again before a redo replays the samples.
 * A block they change in another way must be notified (`notifyChanged`), or the scene's
 * incremental evaluation keeps its old result.
 */
export interface StrokeBrush {
  begin?(ctx: Context, target: StrokeTarget): void
  apply(ctx: Context, sample: StrokeSample): void
  undo(ctx: Context): void
  /** Bytes held for undo. */
  byteSize?(): number
}

const vec3Of = (v: Parameters<typeof cd.vec3At>[0]) => [0, 1, 2].map((i) => cd.vec3At(v, i))

function strokeSampleOf(s: BoundSample): StrokeSample {
  return {
    x: s.x, y: s.y, world: vec3Of(s.world), normal: vec3Of(s.normal), hit: s.hit,
    interpolated: s.interpolated, pressure: s.pressure, tiltX: s.tiltX, tiltY: s.tiltY, twist: s.twist,
    time: s.time, radiusPixels: s.radiusPixels, radiusWorld: s.radiusWorld, distance: s.distance,
    step: s.step, angle: s.angle, dragX: s.dragX, dragY: s.dragY, dragWorld: vec3Of(s.dragWorld),
    mirror: s.mirror,
  }
}

class BrushAdapter {
  constructor(private name: string, private brush: StrokeBrush) {}
  begin(ctx: Context, t: BoundTarget): Status | undefined {
    return this.slot('begin', () =>
      this.brush.begin?.(ctx, {
        object: new Ref(String(t.object)),
        world: Array.from({length: 16}, (_, i) => t.worldAt(i)),
        mirrorAxes: t.mirrorAxes,
      })
    )
  }
  apply(ctx: Context, s: BoundSample): Status | undefined {
    return this.slot('apply', () => this.brush.apply(ctx, strokeSampleOf(s)))
  }
  undo(ctx: Context) {
    inSlot(() => sync('undo', this.brush.undo(ctx)))
  }
  byteSize(): number {
    return inSlot(() => {
      try {
        const n = Number(sync('byteSize', this.brush.byteSize?.() ?? 0))
        return Number.isFinite(n) && n > 0 ? Math.min(Math.floor(n), Number.MAX_SAFE_INTEGER) : 0
      } catch (e) {
        if (isFatal(e)) throw e
        console.warn(`${this.name}: byteSize failed, counted as 0:`, e)
        return 0
      }
    })
  }
  private slot(what: string, fn: () => unknown): Status | undefined {
    return inSlot(() => {
      try {
        sync(what, fn())
        return undefined
      } catch (e) {
        return statusOf(StatusCode.Internal, e)
      }
    })
  }
}

/** Registers a brush for core.stroke.apply and core.tool.stroke's `brush` setting: `create` makes one
 * for each stroke, which its undo step keeps. */
export function defineStrokeBrush(ctx: Context, name: string, create: () => StrokeBrush) {
  checkCtx(ctx)
  if (typeof create !== 'function') throw new TypeError(`defineStrokeBrush: ${name}: create must be a function`)
  const backend = getBindingBackend()
  const factory = {
    make: () => {
      const brush = create()
      if (typeof brush?.apply !== 'function' || typeof brush.undo !== 'function') {
        throw new TypeError(`${name}: a stroke brush needs apply and undo`)
      }
      return backend.foreignHandle(new BrushAdapter(name, brush))
    },
  }
  check(cd.registerForeignStrokeBrush(ctx, name, backend.foreignHandle(factory), 'begin apply undo byteSize'))
}

/* ---- the viewport's side ---- */

export function setActiveTool(ctx: Context, id: string) {
  check(cd.setActiveTool(ctx, id))
}
/** The active tool's id; undefined for none. */
export function activeTool(ctx: Context): string | undefined {
  return take(cd.activeTool(ctx), (s) => s) || undefined
}
/** A ScriptError of `sendToolEvent` that came after the event was dispatched: `used` says whether it was. */
export type ToolEventError = ScriptError & {used?: boolean}

/** Hands the context's ToolManager an event in `view`. Whether it was used. A gizmo's update or
 * hit test that failed throws after the event went on: the error's `used` is then set. */
export function sendToolEvent(ctx: Context, view: ViewSpec, event: Partial<ToolEvent> & {type: ToolEventType}): boolean {
  return withView(view, (v) =>
    withEvent(event, (e) => {
      const r = cd.sendToolEvent(ctx, v, e)
      try {
        const used = Number(r.value) === EventResult.Handled
        if (!r.status.ok()) throw Object.assign(ScriptError.from(r.status), {used}) as ToolEventError
        return used
      } finally {
        r[Symbol.dispose]()
      }
    })
  )
}
/** What the tools draw over `view` this frame: the scene's overlays, the gizmos, the modal's. A slot
 * that failed doesn't lose the rest: `error` is set, and the list has what was drawn. */
export function drawTools(ctx: Context, view: ViewSpec): {bytes: Uint8Array; decoded: DecodedDrawList; error?: ScriptError} {
  const list = getBindingBackend().construct('coredcc::DrawList') as DrawList
  try {
    let error: ScriptError | undefined
    withView(view, (v) => {
      const s = cd.drawTools(ctx, v, list)
      try {
        if (!s.ok()) error = ScriptError.from(s)
      } finally {
        s[Symbol.dispose]()
      }
    })
    const bytes = readDrawList(list)
    return {bytes, decoded: decodeDrawList(bytes), ...(error ? {error} : {})}
  } finally {
    list[Symbol.dispose]()
  }
}
/** Ends the gesture in progress with a cancel. */
export function cancelTool(ctx: Context) {
  check(cd.cancelTool(ctx))
}
export function toolStatus(ctx: Context): string {
  return take(cd.toolStatus(ctx), (s) => s)
}
export function setToolStatus(ctx: Context, text: string) {
  check(cd.setToolStatus(ctx, text))
}
/** The status once the gesture in progress is over (why it did nothing, say), instead of empty. */
export function setToolOutcome(ctx: Context, text: string) {
  check(cd.setToolOutcome(ctx, text))
}
