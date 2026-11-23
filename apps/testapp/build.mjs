#!/usr/bin/env node
/**
 * Builds the page (`pnpm run build`):
 *
 *   node build.mjs [--node-module <coredcc_testapp_node.mjs>]
 *
 * 1. `dist/app.js` from `src/main.ts`: the shell and the testbus, no coredcc.
 * 2. With the test app's wasm-st module built (source/coredcc/testapp), the coredcc
 *    backend:
 *    - generates the bindings' TS into `gen/` from the module's *node* variant (the web
 *      variant can't run here; both have the same bindings);
 *    - bundles `src/backend/wasm.ts` into `dist/backend-wasm.js`, with coredcc's TS
 *      layer and litestl's runtime.
 *    Without the module it says so and skips this: the page then runs with no backend,
 *    and TA1's tests skip.
 *
 * The page picks the *web* module at launch (`--apptest-module`, nwjs/launcher.mjs), so a
 * build doesn't depend on which build tree's module it was generated from.
 */
import {spawnSync} from 'node:child_process'
import {existsSync, mkdirSync, rmSync} from 'node:fs'
import {dirname, join, resolve} from 'node:path'
import {fileURLToPath} from 'node:url'
import {parseArgs} from 'node:util'
import * as esbuild from 'esbuild'

const here = dirname(fileURLToPath(import.meta.url))
const root = resolve(here, '../..')
const dist = join(here, 'dist')
const gen = join(here, 'gen')

/** The default node module: the wasm-st Debug build. */
export const DEFAULT_NODE_MODULE = join(
  root,
  'build/wasm-st/source/coredcc/testapp/Debug/coredcc_testapp_node.mjs'
)

const {values: opts} = parseArgs({options: {'node-module': {type: 'string'}}})
const nodeModule = resolve(opts['node-module'] ?? DEFAULT_NODE_MODULE)

const alias = {
  '@litestl/typescript-runtime': join(root, 'extern/litestl/binding/typescriptRuntime/index.ts'),
  'coredcc-script': join(root, 'source/coredcc/bind/ts/index.ts'),
  'coredcc-gen': gen,
}
const page = {bundle: true, format: 'esm', target: 'chrome140', sourcemap: true, logLevel: 'warning'}

await esbuild.build({...page, entryPoints: [join(here, 'src/main.ts')], outfile: join(dist, 'app.js')})

rmSync(join(dist, 'backend-wasm.js'), {force: true})
rmSync(join(dist, 'backend-wasm.js.map'), {force: true})
if (!existsSync(nodeModule)) {
  console.warn(`[testapp] no wasm-st module at ${nodeModule}: building without the coredcc backend`)
  console.warn('[testapp] build it with: tools\\devshell.cmd cmake --build --preset wasm-st-debug')
  process.exit(0)
}

rmSync(gen, {recursive: true, force: true})
mkdirSync(gen, {recursive: true})
const genBundle = join(dist, 'tools', 'gen.mjs')
await esbuild.build({
  entryPoints: [join(root, 'source/coredcc/bind/ts/gen.ts')],
  bundle: true,
  platform: 'node',
  format: 'esm',
  target: 'node20',
  alias,
  outfile: genBundle,
  logLevel: 'warning',
})
const r = spawnSync(process.execPath, [genBundle, nodeModule, gen, 'TESTAPP_GetManager'], {stdio: 'inherit'})
if (r.status !== 0) {
  console.error('[testapp] generating the bindings TS failed')
  process.exit(1)
}

await esbuild.build({
  ...page,
  entryPoints: [join(here, 'src/backend/wasm.ts')],
  alias,
  outfile: join(dist, 'backend-wasm.js'),
})
console.log(`[testapp] built with the coredcc backend (bindings from ${nodeModule})`)
