// Writes fbx_golden.inc: FBX node chains and their world matrices.
//   node tests/scene/gen_fbx_golden.mjs > tests/scene/fbx_golden.inc
// It follows Autodesk's "Transformations" SDK sample step by step, with 4x4 matrices only, and
// shares nothing with source/coredcc/scene/fbx.cc. Its one difference from the sample: the
// parent's global rotation is carried down the chain, not decomposed from its matrix.

const identity = () => [0, 1, 2, 3].map((i) => [0, 1, 2, 3].map((j) => (i === j ? 1 : 0)))
const mul = (...ms) =>
  ms.reduce((a, b) => a.map((r) => [0, 1, 2, 3].map((j) => r[0] * b[0][j] + r[1] * b[1][j] + r[2] * b[2][j] + r[3] * b[3][j])))
const translate = (t) => { const m = identity(); for (let i = 0; i < 3; i++) m[i][3] = t[i]; return m }
const scale = (s) => { const m = identity(); for (let i = 0; i < 3; i++) m[i][i] = s[i]; return m }
const axisRotation = [
  (a) => [[1, 0, 0, 0], [0, Math.cos(a), -Math.sin(a), 0], [0, Math.sin(a), Math.cos(a), 0], [0, 0, 0, 1]],
  (a) => [[Math.cos(a), 0, Math.sin(a), 0], [0, 1, 0, 0], [-Math.sin(a), 0, Math.cos(a), 0], [0, 0, 0, 1]],
  (a) => [[Math.cos(a), -Math.sin(a), 0, 0], [Math.sin(a), Math.cos(a), 0, 0], [0, 0, 1, 0], [0, 0, 0, 1]],
]
const orders = ['XYZ', 'XZY', 'YXZ', 'YZX', 'ZXY', 'ZYX'].map((s) => [...s].map((c) => 'XYZ'.indexOf(c)))
const rotate = (e, order) => { // the first axis applies first
  const [i, j, k] = orders[order]
  return mul(axisRotation[k](e[k]), axisRotation[j](e[j]), axisRotation[i](e[i]))
}
const transpose = (m) => m.map((_, i) => m.map((r) => r[i]))
const linear = (m) => m.map((r, i) => (i < 3 ? [r[0], r[1], r[2], 0] : [0, 0, 0, 1]))

function world(n, parent) {
  const T = translate(n.t), Roff = translate(n.roff), Rp = translate(n.rp)
  const RpInv = translate(n.rp.map((x) => -x)), Soff = translate(n.soff), Sp = translate(n.sp)
  const SpInv = translate(n.sp.map((x) => -x)), S = scale(n.s)
  const LR = mul(rotate(n.pre, 0), rotate(n.r, n.order), transpose(rotate(n.post, 0)))
  const local = mul(T, Roff, Rp, LR, RpInv, Soff, Sp, S, SpInv)

  const GRM = parent.rotation // the parent's global rotation
  const GSM = mul(transpose(GRM), linear(parent.world)) // its global shear and scale
  let m
  if (n.inherit === 0) {
    m = mul(GRM, LR, GSM, S)
  }
  else if (n.inherit === 1) {
    m = mul(GRM, GSM, LR, S)
  }
  else {
    const noLocal = mul(GSM, scale(parent.s.map((x) => (x === 0 ? 1 : 1 / x))))
    m = mul(GRM, LR, noLocal, S)
  }
  const at = mul(parent.world, translate([local[0][3], local[1][3], local[2][3]]))
  for (let i = 0; i < 3; i++) m[i][3] = at[i][3]
  return { world: m, rotation: mul(GRM, LR), s: n.s }
}

let seed = 20261124
const random = () => { seed = (Math.imul(seed, 1664525) + 1013904223) >>> 0; return seed / 4294967296 }
const range = (a, b) => Math.round((a + (b - a) * random()) * 1000) / 1000
const vec = (a, b) => [range(a, b), range(a, b), range(a, b)]
const zero = () => [0, 0, 0]

// What each chain is made of: inherit types (null for any), signs, pivots, uniform scales.
const kinds = [
  { types: [0] }, { types: [1] }, { types: [2] },
  { types: [1, 0, 0] }, { types: [1, 2, 2] }, { types: [1, 1, 0, 2] }, { types: [2, 1, 2, 0] },
  { types: [1, 1, 2, 0], signs: true }, { types: [1, 0, 2, 1], signs: true, pivots: true },
  { types: null, signs: true, pivots: true }, { types: null, pivots: true }, { types: null, signs: true },
  { types: [1, 2, 0], zeroParent: true }, { types: [1, 2, 0], signs: true, pivots: true, zeroParent: true },
]
console.log('// Written by gen_fbx_golden.mjs. {root, channels, world (column-major)}')
for (let round = 0; round < 3; round++) {
  for (const kind of kinds) {
    const count = kind.types?.length > 1 ? kind.types.length : 4
    let parent = { world: identity(), rotation: identity(), s: [1, 1, 1] }
    for (let i = 0; i < count; i++) {
      const signed = (v) => (kind.signs ? v.map((x) => (random() < 0.5 ? -x : x)) : v)
      const piv = () => (kind.pivots ? vec(-3, 3) : zero())
      const turns = () => (kind.pivots ? vec(-3, 3) : zero())
      const n = {
        t: vec(-5, 5), r: vec(-3.2, 3.2), order: Math.floor(random() * 6), pre: turns(), post: turns(),
        roff: piv(), rp: piv(), soff: piv(), sp: piv(), s: signed(vec(0.3, 3)),
        inherit: kind.types ? kind.types[i % kind.types.length] : Math.floor(random() * 3),
      }
      if (kind.zeroParent && i === 0) n.s[1] = 0
      const state = world(n, parent)
      const channels = [...n.t, ...n.r, n.order, ...n.pre, ...n.post, ...n.roff, ...n.rp, ...n.soff, ...n.sp, ...n.s, n.inherit]
      const columns = [0, 1, 2, 3].flatMap((c) => [0, 1, 2, 3].map((r) => state.world[r][c]))
      console.log(`{${i === 0}, {${channels.join(', ')}},\n {${columns.join(', ')}}},`)
      parent = state
    }
  }
}
