// Runs the P1f TS spike (spike.test.ts) against the test app's wasm-st module, node
// variant (source/coredcc/testapp):
//
//   node run.mjs --module <coredcc_testapp_node.mjs> [--out <scratch dir>]
//
// Exits 77 (a ctest skip) when the dev dependencies aren't installed (`npm ci` here).
//
// 1. generates the bindings' TS with the generator inside the module;
// 2. generates the test app's typed paths (gen-paths);
// 3. type-checks all of it with coredcc's TS layer (source/coredcc/bind/ts) and the suite;
// 4. bundles the suite with esbuild, and runs it under node:test.
import {spawnSync} from 'node:child_process'
import {existsSync, mkdirSync, rmSync, writeFileSync} from 'node:fs'
import {tmpdir} from 'node:os'
import {dirname, join, resolve} from 'node:path'
import {fileURLToPath} from 'node:url'
import {parseArgs} from 'node:util'

const here = dirname(fileURLToPath(import.meta.url))
const root = resolve(here, '../..')
const runtime = join(root, 'extern/litestl/binding/typescriptRuntime/index.ts')
const script = join(root, 'source/coredcc/bind/ts/index.ts')
const genPaths = join(root, 'source/coredcc/bind/ts/gen_paths.ts')
const genScript = join(root, 'source/coredcc/bind/ts/gen.ts')

const {values: opts} = parseArgs({options: {module: {type: 'string'}, out: {type: 'string'}}})
if (!opts.module) {
  console.error('usage: node run.mjs --module <coredcc_testapp_node.mjs> [--out <dir>]')
  process.exit(2)
}
const modulePath = resolve(opts.module)
const out = resolve(opts.out ?? join(tmpdir(), 'coredcc-spike'))
const gen = join(out, 'gen')
rmSync(gen, {recursive: true, force: true})
mkdirSync(gen, {recursive: true})

// The tools' JS entry points, run by node itself: no shell, so no quoting or %VAR%
// expansion of paths.
const tools = {
  tsc: join(here, 'node_modules', 'typescript', 'bin', 'tsc'),
  esbuild: join(here, 'node_modules', 'esbuild-wasm', 'bin', 'esbuild'),
}
const run = (args, env = {}) =>
  spawnSync(process.execPath, args, {stdio: 'inherit', env: {...process.env, ...env}}).status ?? 1
if (!existsSync(tools.tsc) || !existsSync(tools.esbuild)) {
  console.error(`skipped: missing dev dependencies; run "npm ci" in ${here}`)
  process.exit(77)
}
const aliases = [
  `--alias:@litestl/typescript-runtime=${runtime}`,
  `--alias:coredcc-script/gen_paths.ts=${genPaths}`,
  `--alias:coredcc-script=${script}`,
  `--alias:coredcc-gen=${gen}`,
]
const bundle = (entry, outfile) =>
  run([
    tools.esbuild,
    entry, '--bundle', '--platform=node', '--format=esm', '--target=node20',
    ...aliases, `--outfile=${outfile}`, '--log-level=warning',
  ])

console.log('== generating the bindings TS')
const genBundle = join(out, 'gen.mjs')
if (bundle(genScript, genBundle) !== 0 || run([genBundle, modulePath, gen, 'TESTAPP_GetManager']) !== 0) {
  process.exit(1)
}

console.log('== gen-paths')
const paths = join(gen, 'paths.ts')
const oddPaths = join(gen, 'odd_paths.ts')
const pathsBundle = join(out, 'gen_paths.mjs')
if (bundle(join(here, 'gen_paths.ts'), pathsBundle) !== 0 || run([pathsBundle, modulePath, paths, oddPaths]) !== 0) {
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
        allowImportingTsExtensions: true,
        lib: ['ES2022', 'DOM', 'ESNext.Disposable'],
        types: ['node'],
        typeRoots: [join(here, 'node_modules', '@types')],
        paths: {
          '@litestl/typescript-runtime': [runtime],
          'coredcc-script': [script],
          'coredcc-script/gen_paths.ts': [genPaths],
          'coredcc-paths': [paths],
          'coredcc-odd-paths': [oddPaths],
          'coredcc-gen/*': [join(gen, '*')],
        },
      },
      files: [join(here, 'spike.test.ts'), join(here, 'gen_paths.ts'), join(here, 'paths.types.ts'), genScript],
      include: [join(gen, '**', '*.ts')],
    },
    null,
    2
  )
)
if (run([tools.tsc, '-p', tsconfig]) !== 0) {
  process.exit(1)
}

console.log('== spike')
const suite = join(out, 'spike.test.mjs')
if (bundle(join(here, 'spike.test.ts'), suite) !== 0) {
  process.exit(1)
}
process.exit(run(['--test', '--test-reporter=spec', suite], {SPIKE_MODULE: modulePath}))
