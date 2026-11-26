/**
 * A decoded DrawList as triangles in clip space, one batch per depth mode, for the WebGPU
 * renderer. Everything is computed in JS numbers (doubles) relative to the list's origin,
 * and only the clip-space results become floats, so nothing far from the origin is ever a
 * float (docs/drawlist-format.md, "Coordinates and precision").
 *
 * Wide lines and points are expanded into quads here, in pixels, rather than in a vertex
 * shader: one pipeline then draws everything.
 */
import type {DecodedDrawList, DrawRecord} from 'coredcc-script'
import {shapeMesh} from './shapes.ts'

/** Eight floats per vertex: clip x, y, z, w, then r, g, b, a. */
export const FLOATS_PER_VERTEX = 8

export interface Batches {
  test: number[]
  always: number[]
  /** Drawn tested, then again where hidden at a third of its alpha. */
  xray: number[]
}

export interface Viewport {
  x: number
  y: number
  width: number
  height: number
  dpiScale: number
}

type M4 = number[]

/** Column-major a·b. */
export function mul(a: ArrayLike<number>, b: ArrayLike<number>): M4 {
  const out = new Array<number>(16)
  for (let c = 0; c < 4; c++) {
    for (let r = 0; r < 4; r++) {
      let sum = 0
      for (let k = 0; k < 4; k++) sum += a[k * 4 + r] * b[c * 4 + k]
      out[c * 4 + r] = sum
    }
  }
  return out
}

export const translation = (x: number, y: number, z: number): M4 => [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, x, y, z, 1]

const apply = (m: ArrayLike<number>, x: number, y: number, z: number): [number, number, number, number] => [
  m[0] * x + m[4] * y + m[8] * z + m[12],
  m[1] * x + m[5] * y + m[9] * z + m[13],
  m[2] * x + m[6] * y + m[10] * z + m[14],
  m[3] * x + m[7] * y + m[11] * z + m[15],
]

const rgba = (c: number): [number, number, number, number] => [
  (c & 255) / 255,
  ((c >>> 8) & 255) / 255,
  ((c >>> 16) & 255) / 255,
  ((c >>> 24) & 255) / 255,
]

/**
 * `viewProj` is the projection times the view, both in the world; the origin is taken out
 * here, in double.
 */
export function tessellate(list: DecodedDrawList, viewProj: ArrayLike<number>, vp: Viewport): Batches {
  const out: Batches = {test: [], always: [], xray: []}
  const [ox, oy, oz] = list.origin
  const base = mul(viewProj, translation(ox, oy, oz))
  const stack: M4[] = [base]
  const px = 2 / vp.width
  const py = 2 / vp.height

  const vertex = (dst: number[], c: ArrayLike<number>, col: ArrayLike<number>) => dst.push(c[0], c[1], c[2], c[3], ...Array.from(col))
  /* A device pixel of the canvas, in NDC, on the near side of the depth range. */
  const screen = (x: number, y: number): [number, number, number, number] => [
    ((x - vp.x) / vp.width) * 2 - 1,
    1 - ((y - vp.y) / vp.height) * 2,
    0,
    1,
  ]
  /* A quad around the segment a-b (clip), `half` pixels to either side. */
  const wide = (dst: number[], a: number[], b: number[], half: number, col: ArrayLike<number>) => {
    if (a[3] <= 1e-9 || b[3] <= 1e-9) return
    const na = [a[0] / a[3], a[1] / a[3], a[2] / a[3]]
    const nb = [b[0] / b[3], b[1] / b[3], b[2] / b[3]]
    const dx = (nb[0] - na[0]) / px
    const dy = (nb[1] - na[1]) / py
    const len = Math.hypot(dx, dy) || 1
    const ox2 = (-dy / len) * half * px
    const oy2 = (dx / len) * half * py
    const c = (n: number[], s: number) => [n[0] + s * ox2, n[1] + s * oy2, n[2], 1]
    for (const v of [c(na, 1), c(na, -1), c(nb, -1), c(na, 1), c(nb, -1), c(nb, 1)]) vertex(dst, v, col)
  }

  for (const r of list.records) {
    const cur = stack[stack.length - 1]
    const dst = out[r.depthMode === 'always' ? 'always' : r.depthMode === 'xray' ? 'xray' : 'test']
    const col = rgba(r.color)
    const half = (Math.max(r.width, 1) * vp.dpiScale) / 2
    const p = r.positions ?? []
    switch (r.kind) {
      case 'push':
        stack.push(mul(cur, r.matrix!))
        break
      case 'pop':
        stack.pop()
        break
      case 'triangles':
        for (let i = 0; i + 2 < p.length; i += 3) {
          vertex(dst, apply(cur, p[i], p[i + 1], p[i + 2]), r.colors ? rgba(r.colors[i / 3]) : col)
        }
        break
      case 'lines':
        for (let i = 0; i + 5 < p.length; i += 6) {
          wide(dst, apply(cur, p[i], p[i + 1], p[i + 2]), apply(cur, p[i + 3], p[i + 4], p[i + 5]), half, col)
        }
        break
      case 'points':
        for (let i = 0; i + 2 < p.length; i += 3) {
          const c = apply(cur, p[i], p[i + 1], p[i + 2])
          if (c[3] <= 1e-9) continue
          const n = [c[0] / c[3], c[1] / c[3], c[2] / c[3]]
          const [hx, hy] = [half * px, half * py]
          const q = (sx: number, sy: number) => [n[0] + sx * hx, n[1] + sy * hy, n[2], 1]
          for (const v of [q(-1, -1), q(1, -1), q(1, 1), q(-1, -1), q(1, 1), q(-1, 1)]) vertex(dst, v, col)
        }
        break
      case 'shape': {
        const m = mul(cur, r.matrix!)
        const mesh = shapeMesh(r.shape!)
        for (let i = 0; i < mesh.length; i += 3) vertex(dst, apply(m, mesh[i], mesh[i + 1], mesh[i + 2]), col)
        break
      }
      case 'circle2d': {
        const [cx, cy, radius] = p
        const rad = radius * vp.dpiScale
        const at = (a: number, k: number) => screen(cx + Math.cos(a) * rad * k, cy + Math.sin(a) * rad * k)
        for (let s = 0; s < 32; s++) {
          const [a0, a1] = [(2 * Math.PI * s) / 32, (2 * Math.PI * (s + 1)) / 32]
          if (r.filled) {
            for (const v of [screen(cx, cy), at(a0, 1), at(a1, 1)]) vertex(dst, v, col)
          } else {
            wide(dst, at(a0, 1), at(a1, 1), half, col)
          }
        }
        break
      }
      case 'line2d':
        for (let i = 0; i + 3 < p.length; i += 2) wide(dst, screen(p[i], p[i + 1]), screen(p[i + 2], p[i + 3]), half, col)
        break
    }
  }
  return out
}

/** The pixel of a world point in `viewProj` (y down), or undefined behind the eye. */
export function projectPoint(viewProj: ArrayLike<number>, vp: Viewport, p: ArrayLike<number>): [number, number] | undefined {
  const c = apply(viewProj, p[0], p[1], p[2])
  if (c[3] <= 1e-9) return undefined
  return [vp.x + ((c[0] / c[3] + 1) / 2) * vp.width, vp.y + ((1 - c[1] / c[3]) / 2) * vp.height]
}

export type {DrawRecord}
