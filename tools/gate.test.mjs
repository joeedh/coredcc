// node --test tools/gate.test.mjs
//
// What the gate makes of ctest's results. ctest itself writes them, in a project made for it.
// Needs cmake and ctest, through tools/devshell.cmd on Windows.

import assert from 'node:assert/strict'
import {spawnSync} from 'node:child_process'
import fs from 'node:fs'
import os from 'node:os'
import path from 'node:path'
import {after, before, test} from 'node:test'
import {root} from './checkout.mjs'
import {GATES, judge, matrix} from './gate.mjs'

const dir = fs.realpathSync.native(fs.mkdtempSync(path.join(os.tmpdir(), 'coredcc-gate-test-')))
let all = ''
let passes = ''

function sh(...args) {
  const devshell = path.join(root, 'tools', 'devshell.cmd')
  const [cmd, list] = process.platform === 'win32' ? ['cmd.exe', ['/c', devshell, ...args]] : [args[0], args.slice(1)]
  return spawnSync(cmd, list, {cwd: dir, encoding: 'utf8'})
}

const PROJECT = String.raw`cmake_minimum_required(VERSION 3.25)
project(g NONE)
enable_testing()
add_test(NAME good COMMAND "@CMAKE@" -E true)
add_test(NAME "a<b>&c" COMMAND "@CMAKE@" -E true)
add_test(NAME bad COMMAND "@CMAKE@" -E false)
add_test(NAME skips COMMAND "@CMAKE@" -E false)
set_tests_properties(skips PROPERTIES SKIP_RETURN_CODE 1)
add_test(NAME missing COMMAND no_such_program_here)
add_test(NAME off COMMAND "@CMAKE@" -E true)
set_tests_properties(off PROPERTIES DISABLED TRUE)
add_test(NAME slow COMMAND "@CMAKE@" -E sleep 20)
set_tests_properties(slow PROPERTIES TIMEOUT 1)
`

before(() => {
  fs.writeFileSync(path.join(dir, 'CMakeLists.txt'), PROJECT.replaceAll('@CMAKE@', '$' + '{CMAKE_COMMAND}'))
  const build = path.join(dir, 'b')
  const r = sh('cmake', '-S', dir, '-B', build, '-G', 'Ninja')
  assert.equal(r.status, 0, r.stdout + r.stderr)
  sh('ctest', '--test-dir', build, '--output-junit', path.join(dir, 'all.xml'))
  all = fs.readFileSync(path.join(dir, 'all.xml'), 'utf8')
  sh('ctest', '--test-dir', build, '-I', '1,2', '--output-junit', path.join(dir, 'passes.xml'))
  passes = fs.readFileSync(path.join(dir, 'passes.xml'), 'utf8')
})

after(() => fs.rmSync(dir, {recursive: true, force: true}))

test('what passes is no problem', () => {
  assert.deepEqual(judge(passes), {count: 2, problems: []})
  assert.deepEqual(judge(passes, ['good', 'a<b>&c']), {count: 2, problems: []})
})

test('failures, skips, missing programs, disabled tests and timeouts are problems', () => {
  const {count, problems} = judge(all)
  assert.equal(count, 7)
  assert.deepEqual(problems.map((p) => p.split(':')[0]).sort(), ['bad', 'missing', 'off', 'skips', 'slow'])
})

test('a required test that is not there is a problem', () => {
  assert.deepEqual(judge(passes, ['good', 'spike_ts']).problems, ['spike_ts: there is no such test'])
})

test('no tests are a problem', () => {
  assert.deepEqual(judge('<testsuite tests="0"></testsuite>').problems, ['there are no tests'])
})

test('the gates name presets that exist', () => {
  const {buildPresets, testPresets} = JSON.parse(fs.readFileSync(path.join(root, 'CMakePresets.json'), 'utf8'))
  for (const {preset} of Object.values(GATES).flat()) {
    assert.ok(buildPresets.some((p) => p.name === preset) && testPresets.some((p) => p.name === preset), preset)
  }
})

test("CI's matrix has every preset, with what it needs", () => {
  const {include} = matrix(GATES.ci)
  assert.deepEqual(include.map((job) => job.preset), GATES.ci.map((step) => step.preset))
  const having = (key) => include.filter((job) => job[key]).map((job) => job.preset)
  assert.deepEqual(having('wasm'), ['wasm-st-debug', 'wasm-mt-debug'])
  assert.deepEqual(having('scripts'), ['wasm-st-debug'])
})
