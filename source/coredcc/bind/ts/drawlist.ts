/**
 * DrawLists for scripts (docs/drawlist-format.md): drawing into one from a tool's or a
 * gizmo's slot, reading one's bytes, and decoding the bytes into plain records, which the
 * test app's renderer and the tests use. Exported by index.ts.
 */
import type {DrawList} from 'coredcc-gen/coredcc/DrawList'
import type {DrawStyle as BoundStyle} from 'coredcc-gen/coredcc/DrawStyle'
import * as cd from 'coredcc-gen/coredcc/functions'
import {getBindingBackend} from '@litestl/typescript-runtime'

import {check, moduleHeap} from './core.ts'

export type {DrawList}

export const kDrawListMagic = 0x4c444443
export const kDrawListVersion = 1

export type DepthMode = 'test' | 'always' | 'xray'
export type RecordKind = 'lines' | 'points' | 'triangles' | 'shape' | 'circle2d' | 'line2d' | 'push' | 'pop'
export type ShapeKind = 'arrow' | 'cone' | 'cube' | 'sphere' | 'disc' | 'ring' | 'quad'

const depthModes: readonly DepthMode[] = ['test', 'always', 'xray']
const recordKinds: readonly RecordKind[] = ['lines', 'points', 'triangles', 'shape', 'circle2d', 'line2d', 'push', 'pop']
const shapeKinds: readonly ShapeKind[] = ['arrow', 'cone', 'cube', 'sphere', 'disc', 'ring', 'quad']

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
    const depth = depthModes.indexOf(style.depth ?? 'test')
    if (depth < 0) throw new TypeError(`DrawStyle: no depth mode '${style.depth}'`)
    s.depth = depth
    s.id = style.id ?? 0
    return fn(s)
  } finally {
    s[Symbol.dispose]()
  }
}

/** Appends to a list that a slot was handed. Each throws a ScriptError for what the format refuses. */
export class Drawer {
  constructor(readonly list: DrawList) {}
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
    const k = shapeKinds.indexOf(kind)
    if (k < 0) throw new TypeError(`shape: no shape '${kind}'`)
    withMatrix(matrix, (m) => withStyle(style, (s) => check(this.list.shapeOf(s, k + 1, m))))
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

/** Plain records of a list's bytes. Throws a TypeError, naming the byte offset, for a list that
 * isn't valid version 1 (a C++ writer never makes one). */
export function decodeDrawList(bytes: Uint8Array): DecodedDrawList {
  const dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength)
  const fail = (at: number, what: string): never => {
    throw new TypeError(`DrawList: ${what} at byte ${at}`)
  }
  if (bytes.byteLength < 48) fail(0, 'no header')
  if (dv.getUint32(0, true) !== kDrawListMagic) fail(0, 'not a DrawList')
  if (dv.getUint16(4, true) !== kDrawListVersion) fail(4, `version ${dv.getUint16(4, true)}`)
  const count = dv.getUint32(8, true)
  const total = dv.getUint32(12, true)
  if (total !== bytes.byteLength) fail(12, `a size of ${total} for ${bytes.byteLength} bytes`)
  const origin = [0, 1, 2].map((i) => dv.getFloat64(16 + 8 * i, true))
  const f32 = (at: number, n: number) => Array.from({length: n}, (_, i) => dv.getFloat32(at + 4 * i, true))
  const f64 = (at: number, n: number) => Array.from({length: n}, (_, i) => dv.getFloat64(at + 8 * i, true))
  const records: DrawRecord[] = []
  let at = 48
  let depth = 0
  for (let r = 0; r < count; r++) {
    if (at + 24 > total) fail(at, 'a record past the end')
    const kind = recordKinds[dv.getUint16(at, true) - 1] ?? fail(at, `kind ${dv.getUint16(at, true)}`)
    const mode = depthModes[dv.getUint8(at + 2)] ?? fail(at + 2, `depth mode ${dv.getUint8(at + 2)}`)
    const flags = dv.getUint8(at + 3)
    const size = dv.getUint32(at + 4, true)
    if (size < 24 || size % 8 !== 0 || at + size > total) fail(at + 4, `a record size of ${size}`)
    const aux = dv.getUint32(at + 20, true)
    const rec: DrawRecord = {
      kind,
      depth: mode,
      color: dv.getUint32(at + 8, true),
      width: dv.getFloat32(at + 12, true),
      id: dv.getUint32(at + 16, true),
    }
    const p = at + 24
    const need = (n: number) => {
      if (p + n > at + size) fail(at, `${kind} needs ${n} payload bytes`)
    }
    switch (kind) {
      case 'lines':
      case 'points':
      case 'triangles':
        need(12 * aux)
        rec.positions = f32(p, 3 * aux)
        if (kind === 'triangles' && flags & 1) {
          need(16 * aux)
          rec.colors = Array.from({length: aux}, (_, i) => dv.getUint32(p + 12 * aux + 4 * i, true))
        }
        break
      case 'shape':
        need(128)
        rec.shape = shapeKinds[aux - 1] ?? fail(at + 20, `shape ${aux}`)
        rec.matrix = f64(p, 16)
        break
      case 'push':
        need(128)
        rec.matrix = f64(p, 16)
        depth++
        break
      case 'pop':
        if (--depth < 0) fail(at, 'a pop without a push')
        break
      case 'circle2d':
        need(12)
        rec.positions = f32(p, 3)
        rec.filled = (flags & 2) !== 0
        break
      case 'line2d':
        need(8 * aux)
        rec.positions = f32(p, 2 * aux)
        break
    }
    records.push(rec)
    at += size
  }
  if (at !== total) fail(at, `${total - at} bytes after the last record`)
  if (depth !== 0) fail(at, `${depth} pushes without a pop`)
  return {origin, records}
}
