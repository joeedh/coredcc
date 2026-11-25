#!/usr/bin/env node
// The BVH leaf-size benchmark. Builds the release bench_bvh target, runs it and writes
// docs/bvh-leafsize.md. Run it on an idle machine.
//
//   node tools/bench-bvh.mjs            triangles at 10k and 1M, capsules and spheres at 1M
//                                       decide; triangles at 10M confirm
//   node tools/bench-bvh.mjs --quick    10k triangles and spheres, few queries, report on
//                                       stdout: a smoke test, not a measurement
//   node tools/bench-bvh.mjs --wasm     the same on wasm-st in node, without 10M, report on
//                                       stdout
//   --out FILE                          write the report here instead
//
// The report says what the default (BvhOptions::leafSize) should be; it doesn't change it. A section
// "Notes" at the end of the file is kept when the file is written again.

import {spawnSync} from 'node:child_process'
import fs from 'node:fs'
import os from 'node:os'
import path from 'node:path'
import {parseArgs} from 'node:util'
import {fileURLToPath} from 'node:url'

import {build, describe} from './bench.mjs'

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..')
const {values: opts} = parseArgs({
  options: {
    quick: {type: 'boolean', default: false},
    wasm: {type: 'boolean', default: false},
    out: {type: 'string'},
  },
})

const LEAVES = [4, 8, 16, 32, 64, 128, 256, 512, 1024]
/* What it is now, in the source. */
const DEFAULT_LEAF = Number(
  /int leafSize = (\d+);/.exec(fs.readFileSync(path.join(root, 'source', 'coredcc', 'bvh', 'bvh.h'), 'utf8'))[1],
)
const preset = opts.wasm ? 'wasm-st' : 'native-clang'

function runBench(exe, source, count, leaves, extra = []) {
  const args = ['--source', source, '--count', String(count), '--leaf', leaves.join(','), ...extra]
  console.error(`  ${source} ${count}, leaf sizes ${leaves.join(' ')}`)
  const r = opts.wasm
    ? spawnSync(process.execPath, [exe, ...args], {encoding: 'utf8'})
    : spawnSync(exe, args, {encoding: 'utf8'})
  if (r.status !== 0) throw r.error ?? new Error(`bench_bvh failed (${r.status}): ${r.stderr}`)
  return r.stdout.trim().split('\n').map((line) => JSON.parse(line))
}

const KINDS = ['coherentNs', 'randomNs', 'regionNs', 'sphereNs', 'nearestNs']
const TOLERANCE = 1.1

/**
 * The rule (docs/plans/2026-11-24-p3-objects-bvh.md): for each data set and query kind, a leaf
 * size's slowdown is its time over the best size's. Its score is its worst slowdown. The winner is
 * the largest size with a score of at most 1.1, or, if there is none, the best score (the larger
 * size on a tie). No averaging, so a data set counts as much as any other.
 */
function decide(rows, leaves) {
  const sets = [...new Set(rows.map((r) => `${r.source} ${r.count}`))]
  const scores = leaves.map((leaf) => ({leaf, score: 0, where: ''}))
  for (const set of sets) {
    const mine = rows.filter((r) => `${r.source} ${r.count}` === set)
    for (const kind of KINDS) {
      const best = Math.min(...mine.map((r) => r[kind]))
      for (const s of scores) {
        const slowdown = mine.find((r) => r.leaf === s.leaf)[kind] / best
        if (slowdown > s.score) {
          s.score = slowdown
          s.where = `${kind.replace('Ns', '')}, ${set}`
        }
      }
    }
  }
  const good = scores.filter((s) => s.score <= TOLERANCE)
  const winner =
    good.length > 0
      ? good.reduce((a, b) => (b.leaf > a.leaf ? b : a))
      : scores.reduce((a, b) => (b.score < a.score || (b.score === a.score && b.leaf > a.leaf) ? b : a))
  return {scores, winner, within: good.length > 0}
}

const COLUMNS = [
  ['source', (r) => r.source],
  ['count', (r) => r.count],
  ['leaf', (r) => r.leaf],
  ['coherent ray ns', (r) => r.coherentNs.toFixed(0)],
  ['random ray ns', (r) => r.randomNs.toFixed(0)],
  ['region ns', (r) => r.regionNs.toFixed(0)],
  ['sphere ns', (r) => r.sphereNs.toFixed(0)],
  ['nearest ns', (r) => r.nearestNs.toFixed(0)],
  ['build ms', (r) => r.buildMs.toFixed(1)],
  ['serial build ms', (r) => r.serialBuildMs.toFixed(1)],
  ['refit ms', (r) => r.refitMs.toFixed(1)],
  ['node KB', (r) => (r.nodeBytes / 1024).toFixed(0)],
]
const table = (rows) =>
  [
    `| ${COLUMNS.map((c) => c[0]).join(' | ')} |`,
    `|${COLUMNS.map(() => '---').join('|')}|`,
    ...rows.map((r) => `| ${COLUMNS.map((c) => c[1](r)).join(' | ')} |`),
  ].join('\n')

function report(main, confirm, d) {
  const lines = [
    '# BVH leaf size',
    '',
    `Written by \`node tools/bench-bvh.mjs${opts.quick ? ' --quick' : ''}${opts.wasm ? ' --wasm' : ''}\`.`,
    'Best of several runs; queries are single-threaded.',
    '',
    `- Machine: ${os.cpus()[0].model.trim()}, ${os.cpus().length} threads, ${os.type()} ${os.release()}`,
    `- Program: ${describe(exe)}`,
    `- Build: ${preset}, RelWithDebInfo${opts.wasm ? `, node ${process.version}` : ''}`,
    `- Date: ${new Date().toISOString().slice(0, 10)}`,
    '',
    '## Result',
    '',
    `- **Leaf size ${d.winner.leaf}.** The default is ${DEFAULT_LEAF}.`,
    '- The rule: the largest size whose worst slowdown is at most 10%. A slowdown is against the best',
    '  size, for one query kind (coherent and random closest ray, region, sphere overlap, nearest)',
    '  on one data set; nothing is averaged.',
    ...(d.within ? [] : ['- **No size is within 10% everywhere.** This is the one with the smallest worst slowdown.']),
    ...(d.winner.leaf === LEAVES[0] ? ['- **It is the smallest size measured,** so a smaller one could be better.'] : []),
    ...(opts.quick ? ['- **A smoke test: too few queries to mean anything.**'] : []),
    '',
    '| leaf | worst slowdown | where |',
    '|---|---|---|',
    ...d.scores.map((s) => `| ${s.leaf} | ${s.score.toFixed(2)} | ${s.where} |`),
    '',
    '## Data sets that decide',
    '',
    table(main),
  ]
  if (confirm.length > 0) {
    const c = decide(confirm, [...new Set(confirm.map((r) => r.leaf))])
    const mine = c.scores.find((s) => s.leaf === d.winner.leaf)
    lines.push(
      '',
      '## Confirmation',
      '',
      `- At 10M triangles, against its neighbours, size ${mine.leaf}'s worst slowdown is ${mine.score.toFixed(2)}` +
        ` (${mine.where})${mine.score > TOLERANCE ? ': **the choice is not confirmed**' : ''}.`,
      '',
      table(confirm),
    )
  }
  return `${lines.join('\n')}\n`
}

const exe = build('bench_bvh', preset)
console.error('running')
const extra = opts.quick ? ['--queries', '1024', '--reps', '2'] : []
const SETS = opts.quick
  ? [['triangles', 10_000], ['spheres', 10_000]]
  : [['triangles', 10_000], ['triangles', 1_000_000], ['capsules', 1_000_000], ['spheres', 1_000_000]]
const main = SETS.flatMap(([source, n]) => runBench(exe, source, n, LEAVES, extra))
const d = decide(main, LEAVES)
const confirm = []
if (!opts.quick && !opts.wasm) {
  const at = LEAVES.indexOf(d.winner.leaf)
  confirm.push(...runBench(exe, 'triangles', 10_000_000, LEAVES.slice(Math.max(0, at - 1), at + 2)))
}
const text = report(main, confirm, d)
const out = opts.out ?? (opts.quick || opts.wasm ? null : path.join(root, 'docs', 'bvh-leafsize.md'))
if (out) {
  /* What was written by hand is below this line, and stays. */
  const notes = /^## Notes$[^]*/m.exec(fs.existsSync(out) ? fs.readFileSync(out, 'utf8') : '')
  fs.writeFileSync(out, notes ? `${text}\n${notes[0]}` : text)
  console.error(`wrote ${out}`)
} else {
  process.stdout.write(text)
}
