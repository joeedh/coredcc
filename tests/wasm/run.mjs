// Runs the P1f TS spike (spike.test.ts) against the wasm-st module:
//
//   node run.mjs --module <coredcc_spike.mjs> [--out <scratch dir>]
//
// 1. generates the bindings' TS with the generator inside the module;
// 2. type-checks it with coredcc's TS layer (source/coredcc/bind/ts) and the suite;
// 3. bundles the suite with esbuild, and runs it under node:test.
import {spawnSync} from 'node:child_process'
import {existsSync, mkdirSync, rmSync, writeFileSync} from 'node:fs'
import {dirname, join, resolve} from 'node:path'
import {fileURLToPath} from 'node:url'
import {parseArgs} from 'node:util'

const here = dirname(fileURLToPath(import.meta.url))
const root = resolve(here, '../..')
const runtime = join(root, 'extern/litestl/binding/typescriptRuntime/index.ts')
const script = join(root, 'source/coredcc/bind/ts/index.ts')

const {values: opts} = parseArgs({options: {module: {type: 'string'}, out: {type: 'string'}}})
if (!opts.module) {
  console.error('usage: node run.mjs --module <coredcc_spike.mjs> [--out <dir>]')
  process.exit(2)
}
const modulePath = resolve(opts.module)
const out = resolve(opts.out ?? 'spike-ts')
const gen = join(out, 'gen')
rmSync(gen, {recursive: true, force: true})
mkdirSync(gen, {recursive: true})

const bin = (name) => join(here, 'node_modules', '.bin', process.platform === 'win32' ? `${name}.cmd` : name)
const run = (cmd, args, env = {}) => {
  const options = {stdio: 'inherit', env: {...process.env, ...env}}
  // Windows runs .cmd shims only through a shell, which needs the quoting.
  const r = cmd.endsWith('.cmd')
    ? spawnSync([cmd, ...args].map((a) => `"${a}"`).join(' '), {...options, shell: true})
    : spawnSync(cmd, args, options)
  return r.status ?? 1
}
if (!existsSync(bin('tsc'))) {
  console.error(`missing dev dependencies: run "npm ci" in ${here}`)
  process.exit(2)
}
const aliases = [
  `--alias:@litestl/typescript-runtime=${runtime}`,
  `--alias:coredcc-script=${script}`,
  `--alias:coredcc-gen=${gen}`,
]
const bundle = (entry, outfile) =>
  run(bin('esbuild'), [
    entry, '--bundle', '--platform=node', '--format=esm', '--target=node20',
    ...aliases, `--outfile=${outfile}`, '--log-level=warning',
  ])

console.log('== generating the bindings TS')
const genBundle = join(out, 'gen.mjs')
if (bundle(join(here, 'gen.ts'), genBundle) !== 0 || run(process.execPath, [genBundle, modulePath, gen]) !== 0) {
  process.exit(1)
}

console.log('== tsc: generated TS + coredcc TS layer + suite')
const tsconfig = join(out, 'tsconfig.json')
writeFileSync(
  tsconfig,
  JSON.stringify(
    {
      compilerOptions: {
        target: 'ES2022',
        module: 'ESNext',
        moduleResolution: 'bundler',
        strict: true,
        noEmit: true,
        skipLibCheck: true,
        isolatedModules: true,
        lib: ['ES2022', 'DOM', 'ESNext.Disposable'],
        types: ['node'],
        typeRoots: [join(here, 'node_modules', '@types')],
        paths: {
          '@litestl/typescript-runtime': [runtime],
          'coredcc-script': [script],
          'coredcc-gen/*': [join(gen, '*')],
        },
      },
      files: [join(here, 'spike.test.ts'), join(here, 'gen.ts')],
      include: [join(gen, '**', '*.ts')],
    },
    null,
    2
  )
)
if (run(bin('tsc'), ['-p', tsconfig]) !== 0) {
  process.exit(1)
}

console.log('== spike')
const suite = join(out, 'spike.test.mjs')
if (bundle(join(here, 'spike.test.ts'), suite) !== 0) {
  process.exit(1)
}
process.exit(run(process.execPath, ['--test', '--test-reporter=spec', suite], {SPIKE_MODULE: modulePath}))
