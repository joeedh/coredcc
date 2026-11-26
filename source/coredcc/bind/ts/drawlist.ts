/**
 * DrawLists for scripts (docs/drawlist-format.md): drawing into one from a tool's or a
 * gizmo's slot, reading one's bytes, and decoding the bytes into plain records, which the
 * test app's renderer and the tests use. Exported by index.ts.
 */
import {DepthMode as BoundDepthMode} from 'coredcc-gen/coredcc/DepthMode'
import type {DrawList} from 'coredcc-gen/coredcc/DrawList'
import type {DrawStyle as BoundStyle} from 'coredcc-gen/coredcc/DrawStyle'
import * as cd from 'coredcc-gen/coredcc/functions'
import {ShapeKind as BoundShapeKind} from 'coredcc-gen/coredcc/ShapeKind'
import {getBindingBackend} from '@litestl/typescript-runtime'

import {check, keyOf, moduleHeap} from './core.ts'

export type {DrawList}

export const kDrawListMagic = 0x4c444443
export const kDrawListVersion = 1

export type DepthMode = 'test' | 'always' | 'xray'
export type RecordKind = 'lines' | 'points' | 'triangles' | 'shape' | 'circle2d' | 'line2d' | 'push' | 'pop'
export type ShapeKind = 'arrow' | 'cone' | 'cube' | 'sphere' | 'disc' | 'ring' | 'quad'

/* The names differ from the C++ items, and decode the list's bytes. */
const depthModes = new Map<BoundDepthMode, DepthMode>([
  [BoundDepthMode.Test, 'test'], [BoundDepthMode.Always, 'always'], [BoundDepthMode.XRay, 'xray'],
])
const recordKinds: readonly RecordKind[] = ['lines', 'points', 'triangles', 'shape', 'circle2d', 'line2d', 'push', 'pop']
const shapeKinds = new Map<BoundShapeKind, ShapeKind>([
  [BoundShapeKind.Arrow, 'arrow'], [BoundShapeKind.Cone, 'cone'], [BoundShapeKind.Cube, 'cube'],
  [BoundShapeKind.Sphere, 'sphere'], [BoundShapeKind.Disc, 'disc'], [BoundShapeKind.Ring, 'ring'],
  [BoundShapeKind.Quad, 'quad'],
])

/** How a record looks. `color` is RGBA8 with red in the lowest byte: 0xff0000ff is opaque red. */
export interface DrawStyle {
  color?: number
  /** Logical pixels: of lines and outlines; a point's diameter. */
  width?: number
  depth?: DepthMode
  /** What it draws: a gizmo's `idBase + part`. */
  id?: number
}

/** One record of a list, decoded. Positions are as stored: relative to the origin, or to the
 * pushed matrix. */
export interface DrawRecord {
  kind: RecordKind
  depth: DepthMode
  color: number
  width: number
  id: number
  /** Lines, points and triangles: x, y, z per vertex. Line2D: x, y per point. Circle2D: x, y, radius. */
  positions?: number[]
  /** Triangles with a color per corner. */
  colors?: number[]
  /** Circle2D. */
  filled?: boolean
  shape?: ShapeKind
  /** Shape and push: 16 numbers, column by column. */
  matrix?: number[]
}

export interface DecodedDrawList {
  origin: number[]
  records: DrawRecord[]
}

/* ---- drawing ---- */

function withStyle<R>(style: DrawStyle, fn: (s: BoundStyle) => R): R {
  const s = getBindingBackend().construct('coredcc::DrawStyle') as BoundStyle
  try {
    s.color = style.color ?? 0xffffffff
    s.width = style.width ?? 1
    const depth = keyOf(depthModes, style.depth ?? 'test')
    if (depth === undefined) throw new TypeError(`DrawStyle: no depth mode '${style.depth}'`)
    s.depth = depth
    s.id = style.id ?? 0
    return fn(s)
  } finally {
    s[Symbol.dispose]()
  }
}

/** Appends to a list that a slot was handed, during that call only. Each throws a ScriptError for
 * what the format refuses, and a TypeError once the slot has returned. */
export class Drawer {
  #list: DrawList | undefined
  constructor(list: DrawList) {
    this.#list = list
  }
  get list(): DrawList {
    if (!this.#list) throw new TypeError('a Drawer is valid during the slot it was handed to only')
    return this.#list
  }
  /** Ends it: later calls throw. */
  close() {
    this.#list = undefined
  }
  lines(xyz: ArrayLike<number>, style: DrawStyle = {}) {
    withStyle(style, (s) => check(this.list.linesXyz(s, Array.from(xyz))))
  }
  points(xyz: ArrayLike<number>, style: DrawStyle = {}) {
    withStyle(style, (s) => check(this.list.pointsXyz(s, Array.from(xyz))))
  }
  /** `colors`: one per corner, or none. */
  triangles(xyz: ArrayLike<number>, style: DrawStyle = {}, colors: ArrayLike<number> = []) {
    withStyle(style, (s) => check(this.list.trianglesXyz(s, Array.from(xyz), Array.from(colors))))
  }
  shape(kind: ShapeKind, matrix: ArrayLike<number>, style: DrawStyle = {}) {
    const k = keyOf(shapeKinds, kind)
    if (k === undefined) throw new TypeError(`shape: no shape '${kind}'`)
    withMatrix(matrix, (m) => withStyle(style, (s) => check(this.list.shape(s, k, m))))
  }
  pushMatrix(matrix: ArrayLike<number>) {
    withMatrix(matrix, (m) => check(this.list.pushMatrix(m)))
  }
  popMatrix() {
    check(this.list.popMatrix())
  }
  circle2d(x: number, y: number, radius: number, style: DrawStyle = {}, filled = false) {
    withStyle(style, (s) => check(this.list.circle2d(s, x, y, radius, filled)))
  }
  line2d(xy: ArrayLike<number>, style: DrawStyle = {}) {
    withStyle(style, (s) => check(this.list.line2dXy(s, Array.from(xy))))
  }
}

type BoundMatrix = Parameters<DrawList['pushMatrix']>[0]

function withMatrix<R>(values: ArrayLike<number>, fn: (m: BoundMatrix) => R): R {
  if (values.length !== 16) throw new TypeError(`a matrix has 16 numbers, not ${values.length}`)
  const m = getBindingBackend().construct('litestl::math::mat4d') as unknown as BoundMatrix & {
    put(i: number, v: number): void
    [Symbol.dispose](): void
  }
  try {
    for (let i = 0; i < 16; i++) m.put(i, values[i])
    return fn(m)
  } finally {
    m[Symbol.dispose]()
  }
}

/* ---- reading ---- */

const CHUNK = 1 << 16

/**
 * The list's bytes now. With `view`, a view of the module's memory where there is one: valid
 * until the next call into coredcc, so read it at once. Otherwise a copy.
 */
export function readDrawList(list: DrawList, opts: {view?: boolean} = {}): Uint8Array {
  const size = Number(list.size())
  const heap = opts.view ? moduleHeap() : undefined
  if (heap) {
    const at = Number(list.address())
    return heap.subarray(at, at + size)
  }
  const bytes = new Uint8Array(size)
  for (let at = 0; at < size; at += CHUNK) {
    check(cd.drawListRead(list, BigInt(at), bytes.subarray(at, Math.min(size, at + CHUNK))))
  }
  return bytes
}

/**
 * Plain records of a list's bytes. Throws a TypeError, naming the byte offset, for a list that
 * isn't valid version 1. It checks what C++'s DrawListReader checks (docs/drawlist-format.md,
 * "Errors"), so a list one takes the other takes.
 */
export function decodeDrawList(bytes: Uint8Array): DecodedDrawList {
  const dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength)
  const fail = (at: number, what: string): never => {
    throw new TypeError(`DrawList: ${what} at byte ${at}`)
  }
  const total = bytes.byteLength
  if (total < 48) fail(0, 'no header')
  if (dv.getUint32(0, true) !== kDrawListMagic) fail(0, 'not a DrawList')
  if (dv.getUint16(4, true) !== kDrawListVersion) fail(4, `version ${dv.getUint16(4, true)}`)
  if (dv.getUint16(6, true) !== 0) fail(6, "header flags that don't exist")
  const count = dv.getUint32(8, true)
  if (dv.getUint32(12, true) !== total) fail(12, `a size of ${dv.getUint32(12, true)} for ${total} bytes`)
  const origin = [0, 1, 2].map((i) => dv.getFloat64(16 + 8 * i, true))
  if (!origin.every(Number.isFinite)) fail(16, "an origin that isn't finite")
  if (dv.getBigUint64(40, true) !== 0n) fail(40, "a reserved word that isn't 0")
  const f32 = (at: number, n: number) => Array.from({length: n}, (_, i) => dv.getFloat32(at + 4 * i, true))
  const f64 = (at: number, n: number) => Array.from({length: n}, (_, i) => dv.getFloat64(at + 8 * i, true))
  const pad8 = (n: number) => (n + 7) & ~7
  const records: DrawRecord[] = []
  let at = 48
  let depth = 0
  while (at < total) {
    if (total - at < 24) fail(at, 'a record header past the end')
    const kindNo = dv.getUint16(at, true)
    const kind = recordKinds[kindNo - 1] ?? fail(at, `no such kind ${kindNo}`)
    const mode = depthModes.get(dv.getUint8(at + 2)) ?? fail(at, `no such depth mode ${dv.getUint8(at + 2)}`)
    const flags = dv.getUint8(at + 3)
    const allowed = kind === 'triangles' ? 1 : kind === 'circle2d' ? 2 : 0
    if (flags & ~allowed) fail(at, "flags that this kind doesn't have")
    const width = dv.getFloat32(at + 12, true)
    if (!Number.isFinite(width) || width < 0) fail(at, "a width that isn't a number of 0 or more")
    const size = dv.getUint32(at + 4, true)
    if (size % 8 !== 0 || size < 24 || size > total - at) fail(at, `a record size of ${size}`)
    const aux = dv.getUint32(at + 20, true)
    const rec: DrawRecord = {kind, depth: mode, color: dv.getUint32(at + 8, true), width, id: dv.getUint32(at + 16, true)}
    const multiple = kind === 'lines' ? 2 : kind === 'triangles' ? 3 : 1
    if ((kind === 'lines' || kind === 'triangles') && aux % multiple !== 0) {
      fail(at, `a vertex count that isn't a multiple of ${multiple}`)
    }
    if (kind === 'shape' && !shapeKinds.has(aux)) fail(at, `no such shape ${aux}`)
    const need =
      kind === 'lines' || kind === 'points' || kind === 'triangles' ? 12 * aux + (flags & 1 ? 4 * aux : 0)
      : kind === 'shape' || kind === 'push' ? 128
      : kind === 'circle2d' ? 12
      : kind === 'line2d' ? 8 * aux
      : 0
    if (pad8(need) !== size - 24) fail(at, "a record size that doesn't fit its count")
    const p = at + 24
    for (let i = p + need; i < at + size; i++) if (bytes[i] !== 0) fail(at, "padding that isn't zeros")
    const matrix = kind === 'shape' || kind === 'push'
    const numbers = matrix ? f64(p, 16) : f32(p, kind === 'line2d' ? 2 * aux : kind === 'circle2d' ? 3 : kind === 'pop' ? 0 : 3 * aux)
    if (!numbers.every(Number.isFinite)) fail(at, "a number that isn't finite")
    if (matrix && (numbers[3] !== 0 || numbers[7] !== 0 || numbers[11] !== 0 || numbers[15] !== 1)) {
      fail(at, "a matrix that isn't affine")
    }
    switch (kind) {
      case 'lines':
      case 'points':
      case 'triangles':
        rec.positions = numbers
        if (flags & 1) rec.colors = Array.from({length: aux}, (_, i) => dv.getUint32(p + 12 * aux + 4 * i, true))
        break
      case 'shape':
        rec.shape = shapeKinds.get(aux)
        rec.matrix = numbers
        break
      case 'push':
        rec.matrix = numbers
        depth++
        break
      case 'pop':
        if (--depth < 0) fail(at, 'a pop without a push')
        break
      case 'circle2d':
        if (numbers[2] < 0) fail(at, 'a negative radius')
        rec.positions = numbers
        rec.filled = (flags & 2) !== 0
        break
      case 'line2d':
        rec.positions = numbers
        break
    }
    records.push(rec)
    at += size
  }
  if (records.length !== count) fail(8, `a record count of ${count} for ${records.length} records`)
  if (depth !== 0) fail(total, `${depth} pushes without a pop`)
  return {origin, records}
}
