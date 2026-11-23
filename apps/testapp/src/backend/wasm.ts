/**
 * The wasm-st backend's loader (TA1, docs/plans/2026-11-20-p1-application-spine.md
 * "P1f step 6"): loads the test app's web module and makes it the active binding
 * backend; `methods.ts` adds the testbus methods over it.
 *
 * Bundled on its own (`dist/backend-wasm.js`) with coredcc's TS layer and the generated
 * bindings; `main.ts` imports it at run time and passes it `registerMethod`, so a build
 * without the wasm module still runs TA0.
 */
import * as cdcc from 'coredcc-script'
import {isPoisoned} from '@litestl/typescript-runtime'
import {installMethods, type BackendState, type LoadedBackend, type Register} from './methods.ts'

/**
 * Imports the Emscripten module at `url` and instantiates it.
 *
 * With ASSERTIONS (Debug), emscripten's startup check throws "not compiled for this
 * environment" whenever `process.versions.node` exists, as it does in NW.js. The check
 * runs synchronously inside the factory call, so `process` is hidden for that call only.
 */
async function instantiate(url: string, printErr: (text: string) => void): Promise<object> {
  const factory = (await import(url)).default as (opts: object) => Promise<object>
  const g = globalThis as {process?: unknown}
  const proc = g.process
  let pending: Promise<object>
  delete g.process
  try {
    pending = factory({printErr})
  } finally {
    g.process = proc
  }
  return await pending
}

export interface BackendInfo extends BackendState {
  backend: string
  module: string
}

/** Loads the module at `modulePath` (absolute) and registers the testbus methods. */
export async function install(
  register: Register,
  modulePath: string,
  report: (info: BackendInfo) => void
): Promise<BackendInfo> {
  if (/_node\.mjs$/.test(modulePath)) {
    throw new Error(`${modulePath} is the node variant; the page needs coredcc_testapp.mjs (the web one)`)
  }
  const stderr: string[] = []
  const printErr = (text: string) => {
    stderr.push(text)
    if (stderr.length > 8) stderr.shift()
    console.error(text)
  }
  const {pathToFileURL} = (globalThis as any).require('url') as typeof import('node:url')
  const module = await instantiate(pathToFileURL(modulePath).href, printErr)
  /* Once per page: load() refuses a second module, so reset() only replaces the doc. */
  const backend = cdcc.load(module, 'TESTAPP_GetManager')
  const info: BackendInfo = {backend: 'wasm-st', module: modulePath}
  const loaded: LoadedBackend = {
    kind: 'wasm-st',
    module: modulePath,
    backend,
    isFatal: (e) =>
      e instanceof WebAssembly.RuntimeError ||
      (e as {name?: unknown} | null)?.name === 'ExitStatus' ||
      isPoisoned(module),
    recentStderr: () => [...stderr],
  }
  installMethods(register, loaded, (s) => report({...info, ...s}))
  return info
}
