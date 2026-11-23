#!/usr/bin/env node
/**
 * The wasm-st ctest `testapp` (tests/wasm/CMakeLists.txt): builds the page against the
 * given modules and runs every test file under `node --test`.
 *
 *   node ctest.mjs --node-module <coredcc_testapp_node.mjs> --module <coredcc_testapp.mjs>
 *
 * Exits 77 (a ctest skip) when the app can't run here: no `pnpm install`, no NW.js
 * binary, or a node too old to run `.ts` tests. Missing modules are a failure: ctest
 * built them.
 */
import {spawnSync} from 'node:child_process'
import {existsSync} from 'node:fs'
import {createRequire} from 'node:module'
import {dirname, join, resolve} from 'node:path'
import {fileURLToPath} from 'node:url'
import {parseArgs} from 'node:util'

const here = dirname(fileURLToPath(import.meta.url))
const skip = (why) => {
  console.error(`skipped: ${why}`)
  process.exit(77)
}

const {values: opts} = parseArgs({options: {'node-module': {type: 'string'}, module: {type: 'string'}}})
if (!opts['node-module'] || !opts.module) {
  console.error('usage: node ctest.mjs --node-module <coredcc_testapp_node.mjs> --module <coredcc_testapp.mjs>')
  process.exit(2)
}
for (const m of [opts['node-module'], opts.module]) {
  if (!existsSync(m)) {
    console.error(`missing module: ${m}`)
    process.exit(1)
  }
}

/* Type stripping, which runs the .ts tests, is on by default from 22.18. */
const [major, minor] = process.versions.node.split('.').map(Number)
if (major < 22 || (major === 22 && minor < 18)) skip(`node ${process.versions.node} can't run .ts tests (22.18+)`)
if (!existsSync(join(here, 'node_modules', 'esbuild'))) skip(`run "pnpm install" in ${here}`)
try {
  const nw = await createRequire(import.meta.url)('nw').findpath()
  if (!existsSync(nw)) skip(`no NW.js binary at ${nw} (pnpm install with nw's postinstall allowed)`)
} catch (err) {
  skip(`no NW.js: ${err.message}`)
}

const node = (args, env = {}) =>
  spawnSync(process.execPath, args, {cwd: here, stdio: 'inherit', env: {...process.env, ...env}}).status ?? 1

if (node([join(here, 'build.mjs'), '--node-module', resolve(opts['node-module'])]) !== 0) process.exit(1)
if (!existsSync(join(here, 'dist', 'backend-wasm.js'))) {
  console.error('the build produced no coredcc backend')
  process.exit(1)
}
process.exit(
  node(['--test', '--test-concurrency=4', '--test-reporter=spec', 'tests/**/*.test.ts'], {
    COREDCC_TESTAPP_MODULE: resolve(opts.module),
    /* TA1's tests fail rather than skip when the page has no backend. */
    COREDCC_TESTAPP_REQUIRE_BACKEND: '1',
  })
)
