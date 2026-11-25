#!/usr/bin/env node
// The gates: they configure, build and run every test, and say whether all of it passed.
//
//   node tools/gate.mjs pre-commit   native-clang-release (RelWithDebInfo). Run it by hand: it
//                                    isn't a git hook
//   node tools/gate.mjs ci           the four native presets, wasm-st-debug and wasm-mt-debug,
//                                    and the benchmarks are built, though not run.
//                                    .github/workflows/ci.yml runs it, a job per preset
//       [--only PRESET]...           some of the gate's presets. Doesn't count as the gate
//       [--jobs N]                   compile jobs, 5 by default: more run out of memory
//       [--fresh]                    configure from scratch
//       [--matrix]                   only print the presets, as GitHub's job matrix
//
// - A test that is skipped or couldn't run fails the gate, and so does a script test that isn't
//   there: both mean missing dependencies (docs/debugging.md, "spike_ts missing").
// - After a failure the other presets still run. Exits 0 if everything passed.
// - Logs: build/gate/<preset>.log.

import {spawnSync} from 'node:child_process'
import crypto from 'node:crypto'
import fs from 'node:fs'
import path from 'node:path'
import {pathToFileURL} from 'node:url'
import {parseArgs} from 'node:util'
import {root} from './checkout.mjs'

const LOG_TAIL = 40

/** `benchmarks`: also build them. `tests`: what has to be among the tests. */
export const GATES = {
  'pre-commit': [{preset: 'native-clang-release'}],
  ci: [
    {preset: 'native-clang-debug'},
    {preset: 'native-clang-release', benchmarks: true},
    {preset: 'native-msvc-debug'},
    {preset: 'native-msvc-release', benchmarks: true},
    {preset: 'wasm-st-debug', benchmarks: true, tests: ['spike_ts', 'testapp']},
    {preset: 'wasm-mt-debug'},
  ],
}

const ENTITIES = {lt: '<', gt: '>', quot: '"', apos: "'", amp: '&'}
const unescape = (s) => s.replace(/&(lt|gt|quot|apos|amp);/g, (_, name) => ENTITIES[name])

/** The gate's presets as GitHub's job matrix, and what each needs installed. */
export const matrix = (gate) => ({
  include: gate.map(({preset, tests}) => ({preset, wasm: preset.startsWith('wasm-'), scripts: tests !== undefined})),
})

/** What ctest's JUnit file says: how many tests, and everything in it that isn't a pass. */
export function judge(junit, required = []) {
  const problems = []
  const names = new Set()
  for (const [, attributes] of junit.matchAll(/<testcase\b([^>]*)>/g)) {
    const of = (key) => unescape(new RegExp(`\\b${key}="([^"]*)"`).exec(attributes)?.[1] ?? '')
    const name = of('name')
    names.add(name)
    // "run" is a pass; "notrun" a skip or a missing program; "fail" and "disabled" are what they say.
    const status = of('status')
    if (status !== 'run') {
      problems.push(`${name}: ${status === 'notrun' ? 'skipped, or could not run' : status || 'no status'}`)
    }
  }
  if (names.size === 0) problems.push('there are no tests')
  for (const name of required) {
    if (!names.has(name)) problems.push(`${name}: there is no such test`)
  }
  return {count: names.size, problems}
}

/** Runs it in the Visual Studio environment, its output appended to `log`. Null, or what failed. */
function sh(log, args) {
  const devshell = path.join(root, 'tools', 'devshell.cmd')
  const [cmd, list] = process.platform === 'win32' ? ['cmd.exe', ['/c', devshell, ...args]] : [args[0], args.slice(1)]
  // The build cache's launcher sets these itself; the caller's would be for another server.
  const env = Object.fromEntries(Object.entries(process.env).filter(([name]) => !/^SCCACHE_/i.test(name)))
  const fd = fs.openSync(log, 'a')
  fs.writeSync(fd, `\n> ${args.join(' ')}\n`)
  const r = spawnSync(cmd, list, {cwd: root, env, stdio: ['ignore', fd, fd]})
  fs.closeSync(fd)
  if (r.error) return r.error.message
  return r.status === 0 ? null : `exit code ${r.status ?? r.signal}`
}

/** Null, or what went wrong. `result.count` becomes the number of tests. */
function runPreset(step, log, opts, configured, result) {
  const configure = step.preset.replace(/-(debug|release)$/, '')
  if (!configured.has(configure)) {
    const failed = sh(log, ['cmake', ...(opts.fresh ? ['--fresh'] : []), '--preset', configure])
    if (failed) return `configuring failed (${failed})`
    configured.add(configure)
  }
  const build = ['cmake', '--build', '--preset', step.preset, '-j', String(opts.jobs)]
  let failed = sh(log, build)
  if (failed) return `the build failed (${failed})`
  failed = step.benchmarks ? sh(log, [...build, '--target', 'benchmarks']) : null
  if (failed) return `building the benchmarks failed (${failed})`

  const junit = path.join(path.dirname(log), `${step.preset}.xml`)
  fs.rmSync(junit, {force: true})
  failed = sh(log, ['ctest', '--preset', step.preset, '--output-junit', junit])
  if (!fs.existsSync(junit)) return `ctest wrote no results (${failed ?? 'exit code 0'})`
  const {count, problems} = judge(fs.readFileSync(junit, 'utf8'), step.tests)
  result.count = count
  if (failed && problems.length === 0) problems.push(`ctest failed (${failed})`)
  return problems.length ? problems.join('\n    ') : null
}

function git(...args) {
  const r = spawnSync('git', args, {cwd: root, encoding: 'utf8', maxBuffer: 1 << 28})
  if (r.status !== 0) throw new Error(`git ${args[0]}: ${r.error?.message ?? r.stderr}`)
  return r.stdout
}

/** The commit, and a digest of everything but documentation: what is committed, and the changes to it. */
function tree() {
  const code = ['--', '.', ':(exclude)docs', ':(exclude,glob)**/*.md']
  const status = git('status', '--porcelain', '--untracked-files=all', ...code)
  const committed = git('ls-files', '--stage', ...code)
  const digest = crypto.createHash('sha1').update(status).update(committed).update(git('diff', ...code)).digest('hex')
  return {head: git('rev-parse', '--short', 'HEAD').trim(), status, digest}
}

function main() {
  const {values, positionals} = parseArgs({
    allowPositionals: true,
    options: {
      only: {type: 'string', multiple: true, default: []},
      jobs: {type: 'string', default: '5'},
      fresh: {type: 'boolean', default: false},
      matrix: {type: 'boolean', default: false},
    },
  })
  const name = positionals[0]
  const gate = Object.hasOwn(GATES, name ?? '') ? GATES[name] : null
  if (!gate || positionals.length !== 1) throw new Error(`which gate: ${Object.keys(GATES).join(' or ')}`)
  if (values.matrix) {
    console.log(JSON.stringify(matrix(gate)))
    return
  }
  const jobs = Number(values.jobs)
  if (!Number.isInteger(jobs) || jobs < 1) throw new Error(`--jobs ${values.jobs}: not a number of jobs`)
  for (const preset of values.only) {
    if (!gate.some((s) => s.preset === preset)) throw new Error(`--only ${preset}: not in ${name}`)
  }
  const steps = values.only.length ? gate.filter((s) => values.only.includes(s.preset)) : gate

  const before = tree()
  console.log(`${name} gate of ${before.head}${before.status ? ' with uncommitted changes' : ''}`)
  const logs = path.join(root, 'build', 'gate')
  fs.mkdirSync(logs, {recursive: true})
  const configured = new Set()
  let bad = 0
  for (const step of steps) {
    const log = path.join(logs, `${step.preset}.log`)
    fs.writeFileSync(log, '')
    const start = Date.now()
    const result = {}
    const problem = runPreset(step, log, {fresh: values.fresh, jobs}, configured, result)
    const seconds = Math.round((Date.now() - start) / 1000)
    const tests = result.count === undefined ? '' : `, ${result.count} tests`
    console.log(`${problem ? 'FAILED' : 'passed'}  ${step.preset}${tests}, ${seconds} s`)
    if (problem) {
      bad++
      console.log(`    ${problem}\n    the end of ${log}:`)
      const lines = fs.readFileSync(log, 'utf8').trimEnd().split(/\r?\n/).slice(-LOG_TAIL)
      console.log(lines.map((l) => `    | ${l}`).join('\n'))
    }
  }
  const after = tree()
  if (after.digest !== before.digest) {
    bad++
    console.log('FAILED  the code changed while the gate ran')
  }
  const partly = steps.length < gate.length ? ` (${steps.length} of ${gate.length} presets: not the whole gate)` : ''
  console.log(`${name} gate: ${bad ? 'FAILED' : 'passed'}${partly}`)
  process.exitCode = bad ? 1 : 0
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  try {
    main()
  } catch (err) {
    console.error(`gate: ${err.message}`)
    process.exitCode = 2
  }
}
