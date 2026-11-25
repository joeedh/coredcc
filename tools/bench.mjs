#!/usr/bin/env node
// Builds a benchmark (native-clang, RelWithDebInfo) and runs it. The targets aren't in `all`, so
// nothing else builds them: a benchmark that is run in another way may be an old one.
//
//   node tools/bench.mjs scene [-- --reps 41]        builds bench_scene, says what it is, runs it
//   node tools/bench.mjs scene --keep FILE           also copies the exe, as a baseline for later
//   node tools/bench.mjs scene --against FILE        runs the baseline and the new one in turns
//       [--rounds 3] [--cooldown 5]                  (seconds between runs), and compares best times
//
// `--against` waits until other programs leave the machine alone, and says how busy it was.
//
// Names: graph, scene, scene_commands. tools/bench-bvh.mjs is for the BVH. Run it on an idle machine.

import {spawnSync} from 'node:child_process'
import fs from 'node:fs'
import os from 'node:os'
import path from 'node:path'
import {parseArgs} from 'node:util'
import {fileURLToPath, pathToFileURL} from 'node:url'

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..')
const NAMES = ['graph', 'scene', 'scene_commands']

function sh(args) {
  const devshell = path.join(root, 'tools', 'devshell.cmd')
  const [cmd, list] = process.platform === 'win32' ? ['cmd.exe', ['/c', devshell, ...args]] : [args[0], args.slice(1)]
  const r = spawnSync(cmd, list, {cwd: root, encoding: 'utf8', maxBuffer: 1 << 26})
  if (r.status !== 0) {
    console.error(r.stdout, r.stderr)
    throw new Error(`failed: ${args.join(' ')}`)
  }
}

/** Builds `target` in the preset's release configuration. Returns the program's path. */
export function build(target, preset = 'native-clang') {
  if (!fs.existsSync(path.join(root, 'build', preset, 'CMakeCache.txt'))) {
    sh(['cmake', '--preset', preset])
  }
  const jobs = process.env.COREDCC_BENCH_JOBS
  sh(['cmake', '--build', '--preset', `${preset}-release`, '--target', target, ...(jobs ? ['-j', jobs] : [])])
  const dir = path.join(root, 'build', preset, 'tests', 'bench', 'RelWithDebInfo')
  const ext = preset.startsWith('wasm') ? '.js' : process.platform === 'win32' ? '.exe' : ''
  return path.join(dir, target + ext)
}

const git = (...args) => spawnSync('git', args, {cwd: root, encoding: 'utf8'}).stdout.trim()

/** The commit, whether the sources differ from it, and when the program was linked. */
export function describe(exe) {
  const dirty = git('status', '--porcelain', '--', 'source', 'tests', 'cmake', 'CMakeLists.txt', 'CMakePresets.json')
  const litestl = git('-C', 'extern/litestl', 'rev-parse', '--short', 'HEAD')
  return (
    `${path.basename(exe)}: ${git('rev-parse', '--short', 'HEAD')}${dirty ? ' with changes' : ''}, ` +
    `litestl ${litestl}, linked ${fs.statSync(exe).mtime.toISOString()}`
  )
}

function run(exe, args) {
  const r = spawnSync(exe, args, {cwd: root, encoding: 'utf8', maxBuffer: 1 << 26})
  if (r.status !== 0) throw r.error ?? new Error(`${exe} failed (${r.status}): ${r.stderr}`)
  return r.stdout
}

/** How many cores worked while nothing of this ran, in `seconds`. */
function busyCores(seconds) {
  const sum = () => {
    let idle = 0
    let all = 0
    for (const {times} of os.cpus()) {
      idle += times.idle
      all += times.idle + times.user + times.sys + times.nice + times.irq
    }
    return {idle, all}
  }
  const a = sum()
  Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0, seconds * 1000)
  const b = sum()
  return os.cpus().length * (1 - (b.idle - a.idle) / Math.max(b.all - a.all, 1))
}

/** Waits until other programs leave the machine alone: at most `QUIET` cores in all. */
const QUIET = 0.6
function quiet(seconds) {
  for (const from = Date.now(); Date.now() - from < 3600_000; ) {
    const busy = busyCores(seconds)
    if (busy <= QUIET) return busy
  }
  throw new Error('the machine was busy for an hour')
}

/** Best times by case, of bench_scene's lines. */
const bests = (text) =>
  new Map([...text.matchAll(/^(.+?)\s+best\s+([\d.]+)/gm)].map(([, name, ms]) => [name.trim(), Number(ms)]))

function compare(before, after, rounds, cooldown, args) {
  const seen = [new Map(), new Map()]
  let most = 0
  let dropped = 0
  for (let i = 0; i < rounds; i++) {
    for (const [k, exe] of [before, after].entries()) {
      let found, before, after
      do {
        before = quiet(cooldown)
        found = bests(run(exe, args))
        after = busyCores(1)
        dropped += after > QUIET ? 1 : 0
      } while (after > QUIET)
      most = Math.max(most, before, after)
      if (found.size === 0) throw new Error('--against needs a benchmark that prints "best" times')
      for (const [name, ms] of found) seen[k].set(name, [...(seen[k].get(name) ?? []), ms])
    }
  }
  console.log(`at most ${most.toFixed(1)} cores were busy just before and after the runs; ${dropped} others were dropped`)
  console.log(`best ms of each of ${rounds} rounds: before | after`)
  for (const [name, times] of seen[1]) {
    const old = (seen[0].get(name) ?? []).map((t) => t.toFixed(2)).join(' ')
    console.log(`${name.padEnd(28)} ${old} | ${times.map((t) => t.toFixed(2)).join(' ')}`)
  }
}

function main() {
  const {values, positionals} = parseArgs({
    allowPositionals: true,
    options: {
      keep: {type: 'string'},
      against: {type: 'string'},
      rounds: {type: 'string', default: '3'},
      cooldown: {type: 'string', default: '5'},
    },
  })
  const [name, ...args] = positionals
  if (!NAMES.includes(name)) throw new Error(`which benchmark: ${NAMES.join(', ')}`)
  const exe = build(`bench_${name}`)
  console.log(describe(exe))
  if (values.keep) fs.copyFileSync(exe, values.keep)
  if (values.against) {
    const old = path.resolve(values.against)
    console.log(`against ${old}, linked ${fs.statSync(old).mtime.toISOString()}`)
    compare(old, exe, Number(values.rounds), Number(values.cooldown), args)
  } else {
    process.stdout.write(run(exe, args))
  }
}

if (import.meta.url === pathToFileURL(process.argv[1]).href) main()
