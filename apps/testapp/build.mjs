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
 *
 * Outputs are written to temporary names and renamed into place, so a page launched
 * during a build sees the old files or the new ones, never none.
 */
import {spawnSync} from 'node:child_process'
import {existsSync, mkdirSync, renameSync, rmSync, writeFileSync} from 'node:fs'
import {dirname, join, resolve} from 'node:path'
import {fileURLToPath} from 'node:url'
import {parseArgs} from 'node:util'
import * as esbuild from 'esbuild'

const here = dirname(fileURLToPath(import.meta.url))
const root = resolve(here, '../..')
const dist = join(here, 'dist')
const gen = join(here, 'gen')
const tmpSuffix = `.${process.pid}.tmp`

/** The default node module: the wasm-st Debug build. */
const DEFAULT_NODE_MODULE = join(root, 'build/wasm-st/source/coredcc/testapp/Debug/coredcc_testapp_node.mjs')

const {values: opts} = parseArgs({options: {'node-module': {type: 'string'}}})
if (opts['node-module'] && !existsSync(opts['node-module'])) {
  console.error(`[testapp] no module at --node-module ${opts['node-module']}`)
  process.exit(1)
}
const nodeModule = resolve(opts['node-module'] ?? DEFAULT_NODE_MODULE)

/* Windows refuses a rename over a file another process has open, briefly. */
function renameRetrying(from, to) {
  for (let i = 0; ; i++) {
    try {
      renameSync(from, to)
      return
    } catch (err) {
      if (i >= 40 || (err.code !== 'EPERM' && err.code !== 'EBUSY' && err.code !== 'EACCES')) throw err
      Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0, 50)
    }
  }
}

/** Bundles `entry` to `outfile` (and its map) through temporary files. */
async function bundle(entry, outfile, options) {
  const result = await esbuild.build({...options, entryPoints: [entry], outfile, write: false})
  mkdirSync(dirname(outfile), {recursive: true})
  for (const f of result.outputFiles) {
    writeFileSync(f.path + tmpSuffix, f.contents)
    renameRetrying(f.path + tmpSuffix, f.path)
  }
}

const page = {bundle: true, format: 'esm', target: 'chrome140', sourcemap: true, logLevel: 'warning'}
await bundle(join(here, 'src/main.ts'), join(dist, 'app.js'), page)

if (!existsSync(nodeModule)) {
  for (const f of ['backend-wasm.js', 'backend-wasm.js.map']) rmSync(join(dist, f), {force: true})
  rmSync(gen, {recursive: true, force: true}) /* stale bindings would type-check as current */
  console.warn(`[testapp] no wasm-st module at ${nodeModule}: building without the coredcc backend`)
  console.warn(String.raw`[testapp] build it with: tools\devshell.cmd cmake --build --preset wasm-st-debug`)
  process.exit(0)
}

/* Generate into a private directory, bundle against it, then swap it in. */
const genTmp = gen + tmpSuffix
rmSync(genTmp, {recursive: true, force: true})
mkdirSync(genTmp, {recursive: true})
const alias = (genDir) => ({
  '@litestl/typescript-runtime': join(root, 'extern/litestl/binding/typescriptRuntime/index.ts'),
  'coredcc-script': join(root, 'source/coredcc/bind/ts/index.ts'),
  'coredcc-gen': genDir,
})
const genBundle = join(dist, 'tools', 'gen.mjs')
await bundle(join(root, 'source/coredcc/bind/ts/gen.ts'), genBundle, {
  bundle: true,
  platform: 'node',
  format: 'esm',
  target: 'node20',
  alias: alias(genTmp),
  logLevel: 'warning',
})
const r = spawnSync(process.execPath, [genBundle, nodeModule, genTmp, 'TESTAPP_GetManager'], {stdio: 'inherit'})
if (r.status !== 0) {
  rmSync(genTmp, {recursive: true, force: true})
  console.error('[testapp] generating the bindings TS failed')
  process.exit(1)
}
await bundle(join(here, 'src/backend/wasm.ts'), join(dist, 'backend-wasm.js'), {...page, alias: alias(genTmp)})
rmSync(gen, {recursive: true, force: true})
renameRetrying(genTmp, gen)
console.log(`[testapp] built with the coredcc backend (bindings from ${nodeModule})`)
