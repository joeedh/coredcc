/**
 * The DrawList's built-in shapes as triangle meshes, made from docs/drawlist-format.md's
 * definitions: each is three numbers per corner, three corners per triangle, in the shape's
 * own space.
 */
import type {ShapeKind} from 'coredcc-script'

const SEGMENTS = 24

type V = [number, number, number]
const tri = (out: number[], a: V, b: V, c: V) => out.push(...a, ...b, ...c)
const quad = (out: number[], a: V, b: V, c: V, d: V) => {
  tri(out, a, b, c)
  tri(out, a, c, d)
}
const around = (i: number) => (2 * Math.PI * (i % SEGMENTS)) / SEGMENTS

/* A surface of revolution about z: `profile` is (radius, z) pairs from one end to the other. */
function lathe(out: number[], profile: readonly [number, number][]) {
  for (let s = 0; s < SEGMENTS; s++) {
    const [c0, s0, c1, s1] = [Math.cos(around(s)), Math.sin(around(s)), Math.cos(around(s + 1)), Math.sin(around(s + 1))]
    for (let k = 0; k + 1 < profile.length; k++) {
      const [r0, z0] = profile[k]
      const [r1, z1] = profile[k + 1]
      quad(out, [r0 * c0, r0 * s0, z0], [r0 * c1, r0 * s1, z0], [r1 * c1, r1 * s1, z1], [r1 * c0, r1 * s0, z1])
    }
  }
}

function cube(out: number[]) {
  const h = 0.5
  const c = (x: number, y: number, z: number): V => [x * h, y * h, z * h]
  for (const axis of [0, 1, 2]) {
    for (const sign of [-1, 1]) {
      const p = (u: number, v: number): V => {
        const q = [0, 0, 0]
        q[axis] = sign
        q[(axis + 1) % 3] = u
        q[(axis + 2) % 3] = v
        return c(q[0], q[1], q[2])
      }
      quad(out, p(-1, -1), p(1, -1), p(1, 1), p(-1, 1))
    }
  }
}

function sphere(out: number[]) {
  const rings = SEGMENTS / 2
  const profile: [number, number][] = []
  for (let i = 0; i <= rings; i++) {
    const a = -Math.PI / 2 + (Math.PI * i) / rings
    profile.push([0.5 * Math.cos(a), 0.5 * Math.sin(a)])
  }
  lathe(out, profile)
}

function ring(out: number[]) {
  const minor = 0.02
  for (let s = 0; s < SEGMENTS * 2; s++) {
    const a0 = (Math.PI * s) / SEGMENTS
    const a1 = (Math.PI * (s + 1)) / SEGMENTS
    for (let t = 0; t < 8; t++) {
      const b0 = (2 * Math.PI * t) / 8
      const b1 = (2 * Math.PI * (t + 1)) / 8
      const p = (a: number, b: number): V => {
        const r = 1 + minor * Math.cos(b)
        return [r * Math.cos(a), r * Math.sin(a), minor * Math.sin(b)]
      }
      quad(out, p(a0, b0), p(a1, b0), p(a1, b1), p(a0, b1))
    }
  }
}

function make(kind: ShapeKind): Float64Array {
  const out: number[] = []
  switch (kind) {
    case 'arrow':
      lathe(out, [[0, 0], [0.02, 0], [0.02, 0.8], [0.07, 0.8], [0, 1]])
      break
    case 'cone':
      lathe(out, [[0, 0], [0.5, 0], [0, 1]])
      break
    case 'cube':
      cube(out)
      break
    case 'sphere':
      sphere(out)
      break
    case 'disc':
      lathe(out, [[0, 0], [1, 0]])
      break
    case 'ring':
      ring(out)
      break
    case 'quad':
      quad(out, [-0.5, -0.5, 0], [0.5, -0.5, 0], [0.5, 0.5, 0], [-0.5, 0.5, 0])
      break
  }
  return Float64Array.from(out)
}

const meshes = new Map<ShapeKind, Float64Array>()

/** The shape's triangles, made once. */
export function shapeMesh(kind: ShapeKind): Float64Array {
  let m = meshes.get(kind)
  if (!m) {
    m = make(kind)
    meshes.set(kind, m)
  }
  return m
}
