#!/usr/bin/env node
// The BVH leaf-size benchmark. Builds the release bench_bvh target, runs it and writes
// docs/bvh-leafsize.md. Run it on an idle machine.
//
//   node tools/bench-bvh.mjs            triangles at 10k and 1M decide; capsules and spheres
//                                       at 1M and triangles at 10M confirm
//   node tools/bench-bvh.mjs --quick    10k triangles only, few queries, report on stdout: a
//                                       smoke test, not a measurement
//   node tools/bench-bvh.mjs --wasm     the same on wasm-st in node, up to 1M, report on stdout
//   --out FILE                          write the report here instead
//
// The report says what the default (BvhOptions::leafSize) should be; it doesn't change it.

import {spawnSync} from 'node:child_process'
import fs from 'node:fs'
import os from 'node:os'
import path from 'node:path'
import {parseArgs} from 'node:util'
import {fileURLToPath} from 'node:url'

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..')
const {values: opts} = parseArgs({
  options: {
    quick: {type: 'boolean', default: false},
    wasm: {type: 'boolean', default: false},
    out: {type: 'string'},
  },
})

const LEAVES = [16, 32, 64, 128, 256, 512, 1024]
const DEFAULT_LEAF = 128
const preset = opts.wasm ? 'wasm-st' : 'native-clang'

function sh(args) {
  const devshell = path.join(root, 'tools', 'devshell.cmd')
  const [cmd, list] = process.platform === 'win32' ? ['cmd.exe', ['/c', devshell, ...args]] : [args[0], args.slice(1)]
  const r = spawnSync(cmd, list, {cwd: root, encoding: 'utf8', maxBuffer: 1 << 26})
  if (r.status !== 0) {
    console.error(r.stdout, r.stderr)
    throw new Error(`failed: ${args.join(' ')}`)
  }
}

function buildBench() {
  if (!fs.existsSync(path.join(root, 'build', preset, 'CMakeCache.txt'))) {
    sh(['cmake', '--preset', preset])
  }
  sh(['cmake', '--build', '--preset', `${preset}-release`, '--target', 'bench_bvh'])
  const dir = path.join(root, 'build', preset, 'tests', 'bench', 'RelWithDebInfo')
  return opts.wasm ? path.join(dir, 'bench_bvh.js') : path.join(dir, process.platform === 'win32' ? 'bench_bvh.exe' : 'bench_bvh')
}

function runBench(exe, source, count, leaves, extra = []) {
  const args = ['--source', source, '--count', String(count), '--leaf', leaves.join(','), ...extra]
  console.error(`  ${source} ${count}, leaf sizes ${leaves.join(' ')}`)
  const r = opts.wasm
    ? spawnSync(process.execPath, [exe, ...args], {encoding: 'utf8'})
    : spawnSync(exe, args, {encoding: 'utf8'})
  if (r.status !== 0) throw r.error ?? new Error(`bench_bvh failed (${r.status}): ${r.stderr}`)
  return r.stdout.trim().split('\n').map((line) => JSON.parse(line))
}

const geomean = (v) => Math.exp(v.reduce((s, x) => s + Math.log(x), 0) / v.length)

/** The rule of the plan. Returns the scores too, so the report can show its working. */
function decide(rows) {
  const leaves = [...new Set(rows.map((r) => r.leaf))]
  const large = Math.max(...rows.map((r) => r.count))
  const scores = leaves.map((leaf) => {
    const mine = rows.filter((r) => r.leaf === leaf)
    return {
      leaf,
      query: geomean(mine.flatMap((r) => [r.coherentNs, r.randomNs, r.regionNs])),
      build: mine.find((r) => r.count === large).buildMs,
    }
  })
  const fastest = Math.min(...scores.map((s) => s.build))
  const eligible = scores.filter((s) => s.build <= fastest * 1.1)
  const best = eligible.reduce((a, b) => (b.query < a.query ? b : a))
  const bestAnyBuild = scores.reduce((a, b) => (b.query < a.query ? b : a))
  const old = eligible.find((s) => s.leaf === DEFAULT_LEAF)
  const winner = old && old.query <= best.query * 1.05 ? old : best
  const largest = Math.max(...scores.filter((s) => s.query <= bestAnyBuild.query * 1.1).map((s) => s.leaf))
  return {largest, scores, eligible: eligible.map((s) => s.leaf), best, bestAnyBuild, winner}
}

const COLUMNS = [
  ['source', (r) => r.source],
  ['count', (r) => r.count],
  ['leaf', (r) => r.leaf],
  ['coherent ray ns', (r) => r.coherentNs.toFixed(0)],
  ['random ray ns', (r) => r.randomNs.toFixed(0)],
  ['region ns', (r) => r.regionNs.toFixed(0)],
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
    `- Build: ${preset}, RelWithDebInfo${opts.wasm ? `, node ${process.version}` : ''}`,
    `- Date: ${new Date().toISOString().slice(0, 10)}`,
    '',
    '## Result',
    '',
    `- **Leaf size ${d.winner.leaf}.** The default is ${DEFAULT_LEAF}.`,
    '- The rule: the best geometric mean of query times (coherent and random closest ray, region;',
    '  triangles at every count) among the sizes whose build, at the largest count, is within 10%',
    `  of the fastest; ${DEFAULT_LEAF} stays if it is within 5% of that.`,
    `- Sizes within 10% of the fastest build: ${d.eligible.join(', ')}. Best among them: ${d.best.leaf}.`,
    `- Best whatever the build time: ${d.bestAnyBuild.leaf}; the largest within 10% of its query time: ${d.largest}.`,
    ...(opts.quick ? ['- **A smoke test: too few queries to mean anything.**'] : []),
    '',
    '| leaf | query geomean ns | build ms |',
    '|---|---|---|',
    ...d.scores.map((s) => `| ${s.leaf} | ${s.query.toFixed(0)} | ${s.build.toFixed(1)} |`),
    '',
    '## Triangles',
    '',
    table(main),
  ]
  if (confirm.length > 0) {
    lines.push('', '## Confirmation', '', table(confirm))
  }
  return `${lines.join('\n')}\n`
}

const exe = buildBench()
console.error('running')
const extra = opts.quick ? ['--queries', '1024', '--reps', '2'] : []
const main = (opts.quick ? [10_000] : [10_000, 1_000_000]).flatMap((n) => runBench(exe, 'triangles', n, LEAVES, extra))
const d = decide(main)
const confirm = []
if (!opts.quick) {
  const at = LEAVES.indexOf(d.winner.leaf)
  const around = LEAVES.slice(Math.max(0, at - 1), at + 2)
  confirm.push(...runBench(exe, 'capsules', 1_000_000, around))
  confirm.push(...runBench(exe, 'spheres', 1_000_000, around))
  if (!opts.wasm) confirm.push(...runBench(exe, 'triangles', 10_000_000, around))
}
const text = report(main, confirm, d)
const out = opts.out ?? (opts.quick || opts.wasm ? null : path.join(root, 'docs', 'bvh-leafsize.md'))
if (out) {
  fs.writeFileSync(out, text)
  console.error(`wrote ${out}`)
} else {
  process.stdout.write(text)
}
