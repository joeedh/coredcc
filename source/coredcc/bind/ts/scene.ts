/**
 * Scenes and transform types for scripts (bind/scene_facades.h, bind/foreign_transform.h).
 * Exported by index.ts, whose rules hold here too.
 *
 * Matrices are 16 numbers, column by column. Vectors are 3 numbers, and quaternions 4: x, y, z, w.
 */
import {getBindingBackend} from '@litestl/typescript-runtime'
import type {ChannelBlock} from 'coredcc-gen/coredcc/ChannelBlock'
import type {Context} from 'coredcc-gen/coredcc/Context'
import type {EvalCtx} from 'coredcc-gen/coredcc/EvalCtx'
import * as cd from 'coredcc-gen/coredcc/functions'
import type {Status} from 'coredcc-gen/coredcc/Status'
import {StatusCode} from 'coredcc-gen/coredcc/StatusCode'
import type {TransformDelta as BoundDelta} from 'coredcc-gen/coredcc/TransformDelta'
import type {TransformState as BoundState} from 'coredcc-gen/coredcc/TransformState'
import type {View as BoundView} from 'coredcc-gen/coredcc/View'
import {internal, Ref, toJs, type Writable} from './index'

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
  /** Channels by name, with their defaults: booleans, numbers and vectors. A whole number declares
   * an Int: use `float(1)` for a Float. */
  channels: Record<string, Writable>
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
const kinds = ['translate', 'rotate', 'scale'] as const
const deltaOf = (d: BoundDelta): TransformDelta => ({
  kind: kinds[d.kindValue()],
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
    return internal.inSlot(() => {
      const s = internal.sync('evaluate', this.spec.evaluate(channelsOf(ctx), stateOf(parent)))
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
  private solve(slot: string, out: ChannelBlock, fn: () => ArrayLike<number>): Status | undefined {
    return internal.inSlot(() => {
      try {
        const values = internal.sync(slot, fn())
        if (typeof values !== 'object' || values === null) throw new TypeError(`${slot} returned no channels`)
        putChannels(slot, out, values)
        return undefined
      } catch (e) {
        return internal.statusOf(StatusCode.Internal, e)
      }
    })
  }
}

/** Registers a transform type, for as long as the document is open. */
export function defineTransformType(ctx: Context, spec: TransformTypeSpec) {
  internal.checkCtx(ctx)
  for (const slot of ['evaluate', 'setWorld', 'applyDelta'] as const) {
    if (typeof spec[slot] !== 'function' && !(slot === 'applyDelta' && spec[slot] === undefined)) {
      throw new TypeError(`defineTransformType: ${spec.id}: ${slot} must be a function`)
    }
  }
  const odd = (spec.distances ?? []).find((name) => !Object.hasOwn(spec.channels, name))
  if (odd !== undefined) throw new TypeError(`defineTransformType: ${spec.id}: no channel is named '${odd}'`)
  const type = cd.newTransformType(spec.id)
  if (!type) throw new Error(`defineTransformType: no type for ${spec.id}`)
  try {
    if (spec.uiName !== undefined) internal.check(cd.transformTypeUiName(type, spec.uiName))
    for (const [name, v] of Object.entries(spec.channels)) {
      const distance = spec.distances?.includes(name) ?? false
      internal.withValue(undefined, v, (pv) => internal.check(cd.transformChannel(type, name, pv, distance)))
    }
    const slots = spec.applyDelta ? 'evaluate setWorld applyDelta' : 'evaluate setWorld'
    const handle = getBindingBackend().foreignHandle(new ModelAdapter(spec))
    internal.check(cd.registerForeignTransformType(ctx, type, handle, slots))
  } finally {
    type[Symbol.dispose]()
  }
}

/* ---- scenes ---- */

/** Makes a scene, undoably. It doesn't become the document's. */
export function newScene(ctx: Context, name: string): Ref {
  return internal.take(cd.newScene(ctx, name), (v) => toJs(ctx, v) as Ref)
}
/** The scene that commands and the functions below work on; null for none. Not undoable. */
export function setScene(ctx: Context, scene: Ref | null) {
  internal.check(cd.setScene(ctx, scene === null ? '' : scene.text))
}
/** For the `objects` input of commands. */
export function blockIds(ctx: Context, blocks: readonly Ref[]): string {
  return blocks.map((b) => internal.take(cd.blockIdText(ctx, b.text), (s) => s)).join(';')
}
/** Computes the states of the scene's objects: `evaluated` has them. */
export function evaluateScene(ctx: Context) {
  internal.check(cd.evaluateScene(ctx))
}
/** Where an object is now. */
export function worldMatrix(ctx: Context, object: Ref): number[] {
  return internal.take(cd.worldMatrix(ctx, object.text), (m) => read(m, 16))
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
  depth?: '0..1' | '-1..1'
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
  depth: number
  point: number[]
}

function withView<R>(spec: ViewSpec, fn: (v: BoundView) => R): R {
  const v = getBindingBackend().construct('coredcc::View') as BoundView
  try {
    checkNumbers('viewport', spec.viewport, 4)
    for (let i = 0; i < 4; i++) v.putViewport(i, spec.viewport[i])
    fill('view', v.view, spec.view, 16)
    fill('proj', v.proj, spec.proj, 16)
    v.setConventions(spec.depth === '-1..1' ? 1 : 0, spec.handedness === 'left' ? 1 : 0, spec.up === 'y' ? 0 : 1)
    v.reversedZ = spec.reversedZ ?? false
    v.infiniteFar = spec.infiniteFar ?? false
    v.ortho = spec.ortho ?? false
    v.dpiScale = spec.dpiScale ?? 1
    return fn(v)
  } finally {
    v[Symbol.dispose]()
  }
}

/** The objects within `radius` logical pixels of a device pixel, the best first. Evaluates the
 * scene. Hidden and unselectable objects can't be picked. */
export function pick(ctx: Context, view: ViewSpec, x: number, y: number, radius: number): PickedObject[] {
  return withView(view, (v) =>
    internal.take(cd.pick(ctx, v, x, y, radius), (list) => {
      const out: PickedObject[] = []
      for (let i = 0; i < list.size(); i++) {
        const p = list.at(i)
        try {
          out.push({
            object: new Ref(p.object),
            domain: p.domain,
            element: p.element,
            distance: p.distance,
            depth: p.depth,
            point: readVec3(p.point),
          })
        } finally {
          p[Symbol.dispose]()
        }
      }
      return out
    })
  )
}
