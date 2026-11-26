/**
 * Scenes and transform types for scripts (bind/scene_facades.h, bind/foreign_transform.h).
 * Exported by index.ts, whose rules hold here too.
 *
 * Matrices are 16 numbers, column by column. Vectors are 3 numbers, and quaternions 4: x, y, z, w.
 */
import {getBindingBackend} from '@litestl/typescript-runtime'
import type {AnyBvh} from 'coredcc-gen/coredcc/AnyBvh'
import type {ChannelEntries as BoundEntries} from 'coredcc-gen/coredcc/ChannelEntries'
import type {RefList} from 'coredcc-gen/coredcc/RefList'
import type {TransformBatch as BoundBatch} from 'coredcc-gen/coredcc/TransformBatch'
import type {TransformTypeInfo} from 'coredcc-gen/coredcc/TransformTypeInfo'
import type {ChannelBlock} from 'coredcc-gen/coredcc/ChannelBlock'
import type {Context} from 'coredcc-gen/coredcc/Context'
import type {EvalCtx} from 'coredcc-gen/coredcc/EvalCtx'
import {DeltaKind} from 'coredcc-gen/coredcc/DeltaKind'
import * as cd from 'coredcc-gen/coredcc/functions'
import {Handedness} from 'coredcc-gen/coredcc/Handedness'
import type {Status} from 'coredcc-gen/coredcc/Status'
import {StatusCode} from 'coredcc-gen/coredcc/StatusCode'
import type {TransformDelta as BoundDelta} from 'coredcc-gen/coredcc/TransformDelta'
import type {TransformState as BoundState} from 'coredcc-gen/coredcc/TransformState'
import type {PickOptions as BoundPickOptions} from 'coredcc-gen/coredcc/PickOptions'
import {UpAxis} from 'coredcc-gen/coredcc/UpAxis'
import type {View as BoundView} from 'coredcc-gen/coredcc/View'
import type {ViewMath} from 'coredcc-gen/coredcc/ViewMath'
import {run} from './run.ts'
import {
  bagToJs, check, checkCtx, inSlot, statusOf, sync, take, withValue, Ref, toJs, type Value, type Writable,
} from './core.ts'

/* ---- numbers ---- */

interface Elements {
  at(i: number): number
  put(i: number, value: number): void
}
type Vec3 = Parameters<typeof cd.vec3At>[0]

const read = (e: Elements, count: number) => Array.from({length: count}, (_, i) => e.at(i))
const readVec3 = (v: Vec3) => [0, 1, 2].map((i) => cd.vec3At(v, i))

function checkNumbers(what: string, values: ArrayLike<number>, count: number) {
  if (values.length !== count) throw new TypeError(`${what} has ${count} numbers, not ${values.length}`)
  for (let i = 0; i < count; i++) {
    if (typeof values[i] !== 'number') throw new TypeError(`${what}: ${String(values[i])} isn't a number`)
  }
}
function fill(what: string, e: Elements, values: ArrayLike<number>, count: number) {
  checkNumbers(what, values, count)
  for (let i = 0; i < count; i++) e.put(i, values[i])
}

/* ---- transform types ---- */

/** A channel with more than its default. The names are those of `meta`. */
export interface ChannelSpec {
  /** Boolean, number, vector, `quat(...)`, or with `items` a key. */
  default: Writable
  uiName?: string
  description?: string
  icon?: string
  subtype?: string
  unit?: string
  displayUnit?: string
  hardMin?: number
  hardMax?: number
  softMin?: number
  softMax?: number
  step?: number
  precision?: number
  /** Makes it an Enum. */
  items?: readonly {key: string; value: number; uiName?: string; description?: string}[]
}

/** What a file has of an object's channels, by the names of the version that wrote it. An enum is its
 * item's key. */
export interface ChannelEntries {
  get(name: string): Value
  set(name: string, value: Writable): void
  /** Whether there was one. */
  remove(name: string): boolean
  rename(from: string, to: string): boolean
}

/** Objects of one type, one after the other. */
export interface TransformBatch {
  readonly count: number
  /** In seconds. */
  readonly time: number
  /** As many numbers for each object as it has channels. */
  readonly channels: readonly number[]
  /** The states of their parents: 23 numbers each, the matrix, the rotation and the local scale. */
  readonly parents: readonly number[]
}

export interface TransformState {
  /** Object to world. */
  world: number[]
  /** The product of the local rotations along the chain of parents. */
  rotation: number[]
  /** The object's own scale, signed. */
  localScale: number[]
}

export interface TransformChannels {
  /** One number per channel; a vector has several. */
  readonly channels: readonly number[]
  /** In seconds. */
  readonly time: number
}

export interface TransformDelta {
  kind: 'translate' | 'rotate' | 'scale'
  /** What `vector` and `axis` are relative to, as a rotation to the world. */
  basis: number[]
  /** The fixed point of a rotation or a scale. */
  pivot: number[]
  /** The translation, or the factors of a scale. */
  vector: number[]
  axis: number[]
  /** In radians. */
  angle: number
}

export interface TransformTypeSpec {
  /** Dotted, e.g. "my.polar". The `core.` namespace is reserved. */
  id: string
  uiName?: string
  /** Moves when the channels are others. A type that raises it has `upgrade`. */
  version?: number
  /** Channels by name, with their defaults, or more: booleans, numbers, vectors and `quat(...)`. A whole
   * number declares an Int: use `float(1)` for a Float. */
  channels: Record<string, Writable | ChannelSpec>
  /** The names of those that are lengths that move the object. */
  distances?: readonly string[]
  /** The object's state. What is left out of it is the identity. One function for all objects of
   * the type: it keeps no state, and depends on its arguments alone. Inside it, as in the others,
   * everything that takes a Context fails (Busy). Synchronous; a throw leaves the object at its
   * parent. */
  evaluate(object: TransformChannels, parent: TransformState): Partial<TransformState>
  /** All the channels of an object whose matrix should be `target`. What the channels can't do may
   * be left out: the result is checked, with `closest` except for its translation. */
  setWorld(object: TransformChannels, target: number[], parent: TransformState, closest: boolean): ArrayLike<number>
  /** All the channels after `delta`, the total since `base`. Where several say the same, the ones
   * nearest to `nearTo` are wanted. Without it, `setWorld` is used. */
  applyDelta?(
    base: TransformChannels,
    nearTo: readonly number[],
    delta: TransformDelta,
    parent: TransformState
  ): ArrayLike<number>
  /** The same as `evaluate` for many: their states, 23 numbers each, as `parents` has them. evaluateScene
   * uses it, for the objects of a level of the hierarchy. */
  evaluateMany?(batch: TransformBatch): ArrayLike<number>
  /** The point in the world that the object turns about, where a gizmo is put. Without it, the translation
   * of `evaluate`'s matrix. */
  pivot?(object: TransformChannels, parent: TransformState): ArrayLike<number>
  /** Changes what a file of an older version has to what `channels` says now. */
  upgrade?(version: number, channels: ChannelEntries): void
}

const channelsOf = (ctx: EvalCtx): TransformChannels => ({
  channels: Array.from({length: ctx.channelCount()}, (_, i) => ctx.channel(i)),
  time: ctx.time(),
})
const stateOf = (s: BoundState): TransformState => ({
  world: read(s.world, 16),
  rotation: read(s.rotation, 4),
  localScale: readVec3(s.localScale),
})
const kinds = new Map<DeltaKind, TransformDelta['kind']>([
  [DeltaKind.Translate, 'translate'], [DeltaKind.Rotate, 'rotate'], [DeltaKind.Scale, 'scale'],
])
const deltaOf = (d: BoundDelta): TransformDelta => ({
  kind: kinds.get(d.kind)!,
  basis: read(d.basis, 4),
  pivot: readVec3(d.pivot),
  vector: readVec3(d.vector),
  axis: readVec3(d.axis),
  angle: d.angle,
})

function putChannels(what: string, out: ChannelBlock, values: ArrayLike<number>) {
  checkNumbers(what, values, out.size())
  for (let i = 0; i < values.length; i++) out.set(i, values[i])
}

/** What C++ calls: the spec, with plain values. */
class ModelAdapter {
  /* C++ copies it as soon as `evaluate` returns. */
  private result = getBindingBackend().construct('coredcc::TransformState') as BoundState
  constructor(private spec: TransformTypeSpec) {}

  evaluate(ctx: EvalCtx, parent: BoundState): BoundState {
    return inSlot(() => {
      const s = sync('evaluate', this.spec.evaluate(channelsOf(ctx), stateOf(parent)))
      if (typeof s !== 'object' || s === null) throw new TypeError('evaluate returned no state')
      const identity = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]
      fill('world', this.result.world, s.world ?? identity, 16)
      fill('rotation', this.result.rotation, s.rotation ?? [0, 0, 0, 1], 4)
      const scale = s.localScale ?? [1, 1, 1]
      checkNumbers('localScale', scale, 3)
      for (let i = 0; i < 3; i++) cd.vec3Put(this.result.localScale, i, scale[i])
      return this.result
    })
  }
  setWorld(current: EvalCtx, target: Elements, parent: BoundState, mode: number, out: ChannelBlock) {
    return this.solve('setWorld', out, () =>
      this.spec.setWorld(channelsOf(current), read(target, 16), stateOf(parent), mode === 1)
    )
  }
  applyDelta(base: EvalCtx, nearTo: ChannelBlock, delta: BoundDelta, parent: BoundState, out: ChannelBlock) {
    const near = Array.from({length: nearTo.size()}, (_, i) => nearTo.get(i))
    return this.solve('applyDelta', out, () =>
      this.spec.applyDelta!(channelsOf(base), near, deltaOf(delta), stateOf(parent))
    )
  }
  evaluateMany(batch: BoundBatch): Status | undefined {
    return inSlot(() => {
      try {
        const count = batch.count()
        const states = sync(
          'evaluateMany',
          this.spec.evaluateMany!({count, time: batch.time(), channels: batch.channels(), parents: batch.parents()})
        )
        if (typeof states !== 'object' || states === null) throw new TypeError('evaluateMany returned no states')
        checkNumbers('evaluateMany', states, count * 23)
        check(batch.setStates(Array.from(states)))
        return undefined
      } catch (e) {
        return statusOf(StatusCode.Internal, e)
      }
    })
  }
  pivot(ctx: EvalCtx, parent: BoundState): Vec3 {
    return inSlot(() => {
      const p = sync('pivot', this.spec.pivot!(channelsOf(ctx), stateOf(parent)))
      checkNumbers('pivot', p, 3)
      const out = this.result.localScale
      for (let i = 0; i < 3; i++) cd.vec3Put(out, i, p[i])
      return out
    })
  }
  upgrade(version: number, entries: BoundEntries): Status | undefined {
    return inSlot(() => {
      try {
        withEntries(entries, (e) => sync('upgrade', this.spec.upgrade!(version, e) as void))
        return undefined
      } catch (e) {
        return statusOf(StatusCode.Internal, e)
      }
    })
  }
  private solve(slot: string, out: ChannelBlock, fn: () => ArrayLike<number>): Status | undefined {
    return inSlot(() => {
      try {
        const values = sync(slot, fn())
        if (typeof values !== 'object' || values === null) throw new TypeError(`${slot} returned no channels`)
        putChannels(slot, out, values)
        return undefined
      } catch (e) {
        return statusOf(StatusCode.Internal, e)
      }
    })
  }
}

/** The entries are gone when `upgrade` returns. */
function withEntries(bound: BoundEntries, fn: (entries: ChannelEntries) => void) {
  let open = true
  const e = () => {
    if (!open) throw new Error('upgrade has returned')
    return bound
  }
  try {
    fn({
      get(name) {
        const v = e().get(name)
        try {
          return toJs(undefined, v)
        } finally {
          v[Symbol.dispose]()
        }
      },
      set: (name, value) => withValue(undefined, value, (v) => e().set(name, v)),
      remove: (name) => e().remove(name),
      rename: (from, to) => e().rename(from, to),
    })
  } finally {
    open = false
  }
}

const isChannelSpec = (v: Writable | ChannelSpec): v is ChannelSpec =>
  typeof v === 'object' && v !== null && Object.getPrototypeOf(v) === Object.prototype && Object.hasOwn(v, 'default')

const channelTexts = ['uiName', 'description', 'icon', 'subtype', 'unit', 'displayUnit'] as const

function addChannel(type: TransformTypeInfo, name: string, spec: ChannelSpec, distance: boolean) {
  if (spec.items) {
    if (typeof spec.default !== 'string') throw new TypeError(`channel ${name}: an enum's default is a key`)
    check(cd.transformEnumChannel(type, name))
    for (const i of spec.items) {
      check(cd.transformChannelItem(type, name, i.key, i.value, i.uiName ?? '', i.description ?? ''))
    }
    check(cd.transformChannelText(type, name, 'default', spec.default))
  } else {
    withValue(undefined, spec.default, (pv) => check(cd.transformChannel(type, name, pv, distance)))
  }
  for (const what of channelTexts) {
    const text = spec[what]
    if (text !== undefined) check(cd.transformChannelText(type, name, what, text))
  }
  const numbers = (what: string, a?: number, b = 0) => {
    if (a !== undefined) check(cd.transformChannelNumbers(type, name, what, a, b))
  }
  const range = (what: string, min?: number, max?: number) => {
    if (min !== undefined || max !== undefined) numbers(what, min ?? -Infinity, max ?? Infinity)
  }
  range('range', spec.hardMin, spec.hardMax)
  range('softRange', spec.softMin, spec.softMax)
  numbers('step', spec.step)
  numbers('precision', spec.precision)
}

/** Registers a transform type, for as long as the document is open. */
export function defineTransformType(ctx: Context, spec: TransformTypeSpec) {
  checkCtx(ctx)
  const optional = ['applyDelta', 'evaluateMany', 'pivot', 'upgrade'] as const
  for (const slot of ['evaluate', 'setWorld', ...optional] as const) {
    const left = optional.some((s) => s === slot) && spec[slot] === undefined
    if (typeof spec[slot] !== 'function' && !left) {
      throw new TypeError(`defineTransformType: ${spec.id}: ${slot} must be a function`)
    }
  }
  const odd = (spec.distances ?? []).find((name) => !Object.hasOwn(spec.channels, name))
  if (odd !== undefined) throw new TypeError(`defineTransformType: ${spec.id}: no channel is named '${odd}'`)
  const type = cd.newTransformType(spec.id)
  if (!type) throw new Error(`defineTransformType: no type for ${spec.id}`)
  try {
    if (spec.uiName !== undefined) check(cd.transformTypeUiName(type, spec.uiName))
    if (spec.version !== undefined) check(cd.transformTypeVersion(type, spec.version))
    for (const [name, v] of Object.entries(spec.channels)) {
      const distance = spec.distances?.includes(name) ?? false
      addChannel(type, name, isChannelSpec(v) ? v : {default: v}, distance)
    }
    const slots = ['evaluate', 'setWorld', ...optional.filter((s) => spec[s] !== undefined)].join(' ')
    const handle = getBindingBackend().foreignHandle(new ModelAdapter(spec))
    check(cd.registerForeignTransformType(ctx, type, handle, slots))
  } finally {
    type[Symbol.dispose]()
  }
}

/* ---- scenes ---- */

/** Makes a scene, undoably. It doesn't become the document's. */
export function newScene(ctx: Context, name: string): Ref {
  return take(cd.newScene(ctx, name), (v) => toJs(ctx, v) as Ref)
}
/** The scene that commands and the functions below work on; null for none. Not undoable. */
export function setScene(ctx: Context, scene: Ref | null) {
  check(cd.setScene(ctx, scene === null ? '' : scene.text))
}
/** For the `objects` input of commands. */
export function blockIds(ctx: Context, blocks: readonly Ref[]): string {
  return blocks.map((b) => take(cd.blockIdText(ctx, b.text), (s) => s)).join(';')
}
/** Computes the states of the scene's objects: `evaluated` has them. */
export function evaluateScene(ctx: Context) {
  check(cd.evaluateScene(ctx))
}
/** The last evaluation's results, one row for each place: an object that was evaluated. */
export interface SceneResults {
  /** How many places there are, also when only some were asked for. */
  places: number
  /** 16 numbers for each. */
  matrices: number[]
  /** 6 numbers for each: the minimum, then the maximum. An empty box has a minimum above its maximum. */
  bounds: number[]
  /** EvalStatus as a number: 0 is Ok. */
  status: number[]
  /** 1 for hidden, 2 for unselectable. */
  flags: number[]
  /** The parent's place, or -1. */
  parents: number[]
  /** 1 for a member of the scene, and 0 for a parent of one that isn't. */
  members: number[]
  /** Moves by one with each evaluation that changed something. Who has seen the one before this needs only
   * the places in `changed`, unless `allChanged`. */
  version: number
  changed: number[]
  allChanged: boolean
}
/** Of these places, or of all. Doesn't evaluate. */
export function sceneResults(ctx: Context, places?: ArrayLike<number>): SceneResults {
  const at = places === undefined ? [] : Array.from(places)
  return take(cd.sceneResults(ctx, at, places === undefined), (t) => ({
    places: t.places(),
    matrices: t.matrices(),
    bounds: t.bounds(),
    status: t.status(),
    flags: t.flags(),
    parents: t.parents(),
    members: t.members(),
    version: t.version(),
    changed: t.changed(),
    allChanged: t.allChanged(),
  }))
}
const refsOf = (list: RefList) =>
  Array.from({length: list.size()}, (_, i) => list.at(i)).map((text) => (text === '' ? null : new Ref(text)))
/** The objects at these places, or at all. null for one that is gone. */
export function sceneObjects(ctx: Context, places?: ArrayLike<number>): (Ref | null)[] {
  const at = places === undefined ? [] : Array.from(places)
  return take(cd.sceneRefs(ctx, at, places === undefined), refsOf)
}
/** The children of an object in the last evaluation, in the scene's order; for null the objects without a
 * parent. */
export function children(ctx: Context, object: Ref | null): Ref[] {
  return take(cd.sceneChildren(ctx, object?.text ?? ''), refsOf).filter((r) => r !== null)
}
/** Puts objects at matrices, 16 numbers for each, undoably. `exact` leaves out those whose transform can't
 * have it. Returns the outputs of core.object.setWorld. */
export function setWorld(ctx: Context, objects: readonly Ref[], matrices: ArrayLike<number>, exact = false) {
  checkNumbers('setWorld', matrices, objects.length * 16)
  const mode = exact ? 'EXACT' : 'CLOSEST'
  const numbers = Array.from(matrices, String).join(' ')
  const out = take(
    cd.invoke(ctx, `core.object.setWorld(objects="${blockIds(ctx, objects)}" matrices="${numbers}" mode="${mode}")`),
    (bag) => bagToJs(ctx, bag)
  )
  return out as {approximate: number; skipped: number}
}
/** Where an object is now. */
export function worldMatrix(ctx: Context, object: Ref): number[] {
  return take(cd.worldMatrix(ctx, object.text), (m) => read(m, 16))
}
/** A block changed other than through `set` or a command (a transform model of yours answers otherwise):
 * the scene evaluates it again. The incremental pass evaluates only what is notified. */
export function notifyChanged(ctx: Context, block: Ref) {
  check(cd.notifyChanged(ctx, block.text))
}

/* ---- picking ---- */

/** A viewport and its camera, as draw/view.h has it. */
export interface ViewSpec {
  /** x, y, width and height in device pixels, y down. */
  viewport: ArrayLike<number>
  /** World to view. */
  view: ArrayLike<number>
  /** View to clip space. */
  proj: ArrayLike<number>
  depthRange?: '0..1' | '-1..1'
  reversedZ?: boolean
  infiniteFar?: boolean
  ortho?: boolean
  handedness?: 'right' | 'left'
  up?: 'y' | 'z'
  dpiScale?: number
}

export interface PickedObject {
  object: Ref
  /** An element domain, and the element as the object names it. -1 for its bounds. */
  domain: number
  element: number
  /** Logical pixels from the cursor. 0 if it covers the cursor. */
  distance: number
  /** From the near plane, along the view direction, in world units. */
  viewDistance: number
  point: number[]
  /** -1 for none. */
  instance: number
  /** Of unit length, or 0 where there is none. */
  normal: number[]
  /** Where `point` is on a triangle or a segment. */
  u: number
  v: number
  backFacing: boolean
  /** A point, a segment, or a solid that is thin on the screen. */
  thin: boolean
}

/** scene/pick.h's PickOptions. Sizes are in logical pixels. */
export interface PickOptions {
  radius?: number
  /** A bit for each element domain that is wanted. All of them, if it is left out. */
  domains?: number
  shapeRadius?: number
  /** 0 for all. */
  maxHits?: number
  everyDomain?: boolean
  occlusion?: boolean
  thinRadius?: number
  slope?: number
}

function withView<R>(spec: ViewSpec, fn: (v: BoundView) => R): R {
  const v = getBindingBackend().construct('coredcc::View') as BoundView
  try {
    checkNumbers('viewport', spec.viewport, 4)
    for (let i = 0; i < 4; i++) v.putViewport(i, spec.viewport[i])
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

function withPickOptions<R>(spec: PickOptions, fn: (o: BoundPickOptions) => R): R {
  const o = getBindingBackend().construct('coredcc::PickOptions') as BoundPickOptions
  try {
    /* Not Object.assign: it would take any property the spec has. */
    o.radius = spec.radius ?? o.radius
    o.domains = spec.domains ?? o.domains
    o.shapeRadius = spec.shapeRadius ?? o.shapeRadius
    o.maxHits = spec.maxHits ?? o.maxHits
    o.everyDomain = spec.everyDomain ?? o.everyDomain
    o.occlusion = spec.occlusion ?? o.occlusion
    o.thinRadius = spec.thinRadius ?? o.thinRadius
    o.slope = spec.slope ?? o.slope
    return fn(o)
  } finally {
    o[Symbol.dispose]()
  }
}

/** The objects at or near a device pixel, the best first (scene/pick_scene.h has the order).
 * Evaluates the scene. Hidden and unselectable objects can't be picked. */
export function pick(ctx: Context, view: ViewSpec, x: number, y: number, options: PickOptions = {}): PickedObject[] {
  const picked = withView(view, (v) => withPickOptions(options, (o) => cd.pick(ctx, v, x, y, o)))
  return take(picked, (list) => {
    const out: PickedObject[] = []
    for (let i = 0; i < list.size(); i++) {
      const p = list.at(i)
      try {
        out.push({
          object: new Ref(p.object),
          domain: p.domain,
          element: p.element,
          distance: p.distance,
          viewDistance: p.viewDistance,
          point: readVec3(p.point),
          instance: p.instance,
          normal: readVec3(p.normal),
          u: p.u,
          v: p.v,
          backFacing: p.backFacing,
          thin: p.thin,
        })
      } finally {
        p[Symbol.dispose]()
      }
    }
    return out
  })
}

/* ---- views ---- */

/** Reads a bound value, and disposes of it. */
function use<T extends {[Symbol.dispose](): void}, R>(value: T, fn: (v: T) => R): R {
  try {
    return fn(value)
  } finally {
    value[Symbol.dispose]()
  }
}

function withViewMath<R>(spec: ViewSpec, fn: (m: ViewMath) => R): R {
  return withView(spec, (v) => {
    const m = getBindingBackend().construct('coredcc::ViewMath') as ViewMath
    try {
      check(m.set(v))
      return fn(m)
    } finally {
      m[Symbol.dispose]()
    }
  })
}

/** The ray through a device pixel: from the near plane, in the direction of increasing distance. */
export function rayFromPixel(view: ViewSpec, x: number, y: number): {origin: number[]; dir: number[]} {
  return withViewMath(view, (m) => ({
    origin: use(m.rayOrigin(x, y), readVec3),
    dir: use(m.rayDirection(x, y), readVec3),
  }))
}

/** The half-spaces around what is in a rectangle of the image, whose corners are in device pixels, for
 * Bvh.region: four numbers each, a normal and then `w`. */
export function regionFromRect(view: ViewSpec, x0: number, y0: number, x1: number, y1: number): number[] {
  return withViewMath(view, (m) =>
    use(m.regionFromRect(x0, y0, x1, y1), (r) =>
      Array.from({length: r.planeCount() * 4}, (_, i) => r.planeAt(i >> 2, i & 3))
    )
  )
}

/* ---- BVHs ---- */

export interface BvhHit {
  primitive: number
  /** How far along the ray the box is entered, in lengths of its direction; tMin from inside. */
  t: number
}

function withVec3<R>(v: ArrayLike<number>, what: string, fn: (p: Vec3) => R): R {
  checkNumbers(what, v, 3)
  const p = cd.vec3(v[0], v[1], v[2])
  try {
    return fn(p)
  } finally {
    p[Symbol.dispose]()
  }
}

/**
 * A bounding volume hierarchy over boxes, for finding the few primitives of a large set that a
 * query's answer depends on. Bounds are six numbers for each primitive: the minimum, then the maximum.
 * Primitives whose bounds are empty, or have a NaN, are left out. Dispose of it when it isn't needed any
 * more.
 */
export class Bvh {
  private bvh = getBindingBackend().construct('coredcc::AnyBvh') as AnyBvh

  build(bounds: ArrayLike<number>) {
    check(this.bvh.buildFrom(Array.from(bounds)))
  }
  /** New bounds: of all primitives, or of `primitives`, in that order. */
  refit(bounds: ArrayLike<number>, primitives?: ArrayLike<number>) {
    const b = Array.from(bounds)
    check(
      primitives === undefined ? this.bvh.refitFrom(b) : this.bvh.refitSome(Array.from(primitives), b)
    )
  }
  get size() {
    return this.bvh.size()
  }
  /** How much looser refits have made it, from 1. Build it again when it is about 2. */
  get growth() {
    return this.bvh.growth()
  }
  /** The box that the ray enters first, or null. */
  rayClosest(origin: ArrayLike<number>, dir: ArrayLike<number>, tMin = 0, tMax = Number.MAX_VALUE): BvhHit | null {
    return withVec3(origin, 'origin', (o) =>
      withVec3(dir, 'dir', (d) =>
        use(this.bvh.rayClosest(o, d, tMin, tMax), (h) =>
          h.primitive < 0 ? null : {primitive: h.primitive, t: h.t}
        )
      )
    )
  }
  /** The box that is nearest to a point, or null. */
  nearest(point: ArrayLike<number>, radius = Infinity): {primitive: number; distance: number} | null {
    return withVec3(point, 'point', (p) =>
      use(this.bvh.nearest(p, radius), (h) =>
        h.primitive < 0 ? null : {primitive: h.primitive, distance: h.distance}
      )
    )
  }
  /* What a ray, a sphere or half-spaces meet, in ascending order; nothing for input that isn't valid. */
  rayAll(origin: ArrayLike<number>, dir: ArrayLike<number>, tMin = 0, tMax = Number.MAX_VALUE): number[] {
    return withVec3(origin, 'origin', (o) => withVec3(dir, 'dir', (d) => this.bvh.rayAll(o, d, tMin, tMax)))
  }
  sphereOverlap(point: ArrayLike<number>, radius: number): number[] {
    return withVec3(point, 'point', (p) => this.bvh.sphereOverlap(p, radius))
  }
  /** Four numbers for each plane: a normal, then `w`. As `regionFromRect` and a query's `planeValues` give
   * them. */
  region(planes: ArrayLike<number>): number[] {
    return this.bvh.region(Array.from(planes))
  }
  [Symbol.dispose]() {
    this.bvh[Symbol.dispose]()
  }
}

/* ---- graphs that drive objects ---- */

/** Gives the document's Library drivers: the graphs its scenes list drive its objects. A Session takes them
 * away when it closes. */
export function enableDrivers(ctx: Context) {
  check(cd.enableDrivers(ctx))
}
export function disableDrivers(ctx: Context) {
  check(cd.disableDrivers(ctx))
}
/** The document's list of drivers names a graph block, undoably: it drives the document's objects. */
export function addDriverGraph(ctx: Context, graph: Ref) {
  run(ctx, 'core.drivers.addGraph', {graph})
}
/** No list of drivers names the graph any more. */
export function removeDriverGraph(ctx: Context, graph: Ref) {
  run(ctx, 'core.drivers.removeGraph', {graph})
}
/** Something in the listed graphs that can't work. `node` is a node path, "" for the graph. */
export interface DriverDiagnostic {
  graph: Ref | null
  node: string
  message: string
}
export function driverDiagnostics(ctx: Context): DriverDiagnostic[] {
  return take(cd.driverDiagnostics(ctx), (s) =>
    (JSON.parse(s) as {graph: string; node: string; message: string}[]).map((d) => ({
      graph: d.graph ? new Ref(d.graph) : null,
      node: d.node,
      message: d.message,
    }))
  )
}
