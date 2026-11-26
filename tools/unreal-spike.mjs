#!/usr/bin/env node
// The Unreal spike (docs/plans/2026-11-25-p4-tools.md, "The Unreal spike"): builds coredcc with
// Unreal's flags, builds apps/unreal-spike's Editor module with UBT, runs its automation tests in
// a headless editor, and writes build/unreal-spike/report.txt. Outside the gates and CI.
//
//   node tools/unreal-spike.mjs [--engine <UE root>] [--jobs N] [--skip-build] [--filter Coredcc]
//
// Needs UE 5.9 (a launcher build is enough) and Visual Studio 18's MSVC.
import {spawnSync} from 'node:child_process'
import {existsSync, mkdirSync, readdirSync, readFileSync, writeFileSync} from 'node:fs'
import path from 'node:path'
import {fileURLToPath} from 'node:url'
import {parseArgs} from 'node:util'

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..')
const {values: opts} = parseArgs({
  options: {
    engine: {type: 'string', default: 'C:\\Program Files\\Epic Games\\UE_5.9'},
    jobs: {type: 'string', default: '2'},
    'skip-build': {type: 'boolean', default: false},
    filter: {type: 'string', default: 'Coredcc'},
  },
})
const out = path.join(root, 'build', 'unreal-spike')
const cmakeBuild = path.join(out, 'coredcc')
const project = path.join(root, 'apps', 'unreal-spike', 'CoredccSpike.uproject')
const engine = opts.engine
mkdirSync(out, {recursive: true})

const times = {}
function step(name, fn) {
  const t0 = Date.now()
  console.log(`[unreal-spike] ${name}`)
  const r = fn()
  times[name] = (Date.now() - t0) / 1000
  return r
}
function run(exe, args, {devshell = false, log} = {}) {
  const q = (a) => (/[\s"]/.test(a) ? `"${a.replace(/"/g, '\\"')}"` : a)
  /* A batch file runs through cmd, which needs the whole line quoted as it will see it. */
  const [cmd, list, verbatim] = devshell
    ? ['cmd.exe', ['/c', path.join(root, 'tools', 'devshell.cmd'), exe, ...args], false]
    : exe.endsWith('.bat')
      ? ['cmd.exe', ['/d', '/s', '/c', `"${[exe, ...args].map(q).join(' ')}"`], true]
      : [exe, args, false]
  const r = spawnSync(cmd, list, {
    cwd: root, encoding: 'utf8', maxBuffer: 1 << 28, windowsVerbatimArguments: verbatim,
    env: {...process.env, MSYS_NO_PATHCONV: '1'},
  })
  const text = (r.stdout ?? '') + (r.stderr ?? '')
  if (log) writeFileSync(path.join(out, log), text)
  if (r.status !== 0) {
    console.error(text.split('\n').slice(-40).join('\n'))
    throw new Error(`${exe} exited ${r.status}${log ? ` (log: build/unreal-spike/${log})` : ''}`)
  }
  return text
}
function libsUnder(dir) {
  const found = []
  for (const e of readdirSync(dir, {withFileTypes: true})) {
    const p = path.join(dir, e.name)
    if (e.isDirectory()) found.push(...libsUnder(p))
    else if (e.name.endsWith('.lib')) found.push(p)
  }
  return found
}

if (!opts['skip-build']) {
  step('configure coredcc with Unreal flags', () =>
    run('cmake', ['-S', path.join(root, 'apps', 'unreal-spike', 'coredcc'), '-B', cmakeBuild, '-G', 'Ninja',
      '-DCMAKE_BUILD_TYPE=RelWithDebInfo', '-DCMAKE_C_COMPILER=cl', '-DCMAKE_CXX_COMPILER=cl'], {devshell: true, log: 'configure.log'}))
  step('build coredcc', () =>
    run('cmake', ['--build', cmakeBuild, '--target', 'coredcc_tool', '-j', opts.jobs], {devshell: true, log: 'coredcc-build.log'}))
  const libs = libsUnder(cmakeBuild).filter((p) => !/[\\/]CMakeFiles[\\/]/.test(p))
  writeFileSync(path.join(out, 'libs.txt'), libs.join('\n') + '\n')
  step('build the Editor module (UBT)', () =>
    run(path.join(engine, 'Engine', 'Build', 'BatchFiles', 'Build.bat'),
      ['CoredccSpikeEditor', 'Win64', 'Development', `-Project=${project}`, '-WaitMutex', '-NoHotReload',
        `-MaxParallelActions=${opts.jobs}`], {log: 'ubt.log'}))
}

const editorLog = path.join(out, 'editor.log')
step('run the automation tests', () => {
  const exe = path.join(engine, 'Engine', 'Binaries', 'Win64', 'UnrealEditor-Cmd.exe')
  const r = spawnSync(exe, [project, `-ExecCmds=Automation RunTests ${opts.filter};Quit`, '-unattended', '-nullrhi',
    '-nosplash', '-nosound', '-NoLogTimes', `-abslog=${editorLog}`], {encoding: 'utf8', maxBuffer: 1 << 28})
  return r.status
})

const log = existsSync(editorLog) ? readFileSync(editorLog, 'utf8') : ''
const results = [...log.matchAll(/Test Completed\. Result=\{(\w+)\} Name=\{([^}]*)\} Path=\{([^}]*)\}/g)].map((m) => ({
  result: m[1], name: m[2], path: m[3],
}))
const errors = log.split('\n').filter((l) => /\b(Error|Warning):/.test(l) && /Coredcc|coredcc|Automation/.test(l)).slice(0, 60)
const ubtLog = existsSync(path.join(out, 'ubt.log')) ? readFileSync(path.join(out, 'ubt.log'), 'utf8') : ''
const toolchain = (ubtLog.match(/Using Visual Studio[^\n]*|toolchain[^\n]*version[^\n]*/gi) ?? []).slice(0, 3)
const report = [
  `Unreal spike report, ${new Date().toISOString()}`,
  `engine: ${engine}`,
  `coredcc flags: /EHsc /GR- /MD, RelWithDebInfo, _ITERATOR_DEBUG_LEVEL=0 (apps/unreal-spike/coredcc/CMakeLists.txt)`,
  `toolchain: ${toolchain.join(' | ') || 'see build/unreal-spike/ubt.log'}`,
  `not-preferred-toolchain warning: ${/not a preferred version/i.test(ubtLog) ? 'yes' : 'no'}`,
  `times (s): ${Object.entries(times).map(([k, v]) => `${k} ${v.toFixed(0)}`).join('; ')}`,
  '',
  ...results.map((r) => `${r.result.padEnd(8)} ${r.path}`),
  '',
  results.length ? `${results.filter((r) => r.result === 'Success').length} of ${results.length} passed` : 'no test results in the log',
  ...(errors.length ? ['', 'errors and warnings:', ...errors] : []),
].join('\n')
writeFileSync(path.join(out, 'report.txt'), report + '\n')
console.log(report)
process.exit(results.length > 0 && results.every((r) => r.result === 'Success') ? 0 : 1)
