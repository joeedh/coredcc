/**
 * The wasm-st backend (TA1, docs/plans/2026-11-20-p1-application-spine.md "P1f step 6"):
 * loads the test app's web module, holds one document as a `Session`, and adds the
 * datapath and command methods to the testbus.
 *
 * Bundled on its own (`dist/backend-wasm.js`) with coredcc's TS layer and the generated
 * bindings; `main.ts` imports it at run time and passes it `registerMethod`, so a build
 * without the wasm module still runs TA0.
 */
import * as cdcc from 'coredcc-script'
import {ScriptError, Session, StatusCode, type Context, type ScriptCommand, type Value} from 'coredcc-script'
import * as cd from 'coredcc-gen/coredcc/functions'
import type {PropValue} from 'coredcc-gen/coredcc/PropValue'
import type {Host} from 'coredcc-gen/testapp/Host'
import * as ta from 'coredcc-gen/testapp/functions'
import type {IBindingBackend} from '@litestl/typescript-runtime'

type Register = (name: string, fn: (...args: any[]) => unknown) => void

/** `testapp.addCount(amount=1)`: adds `amount` to `count`, and returns `result`. The TS
 * command TA1 runs through the executor. */
class AddCount implements ScriptCommand {
  before = 0
  exec(ctx: Context) {
    this.before = cdcc.get(ctx, 'count') as number
    const after = this.before + (cdcc.input(ctx, 'amount') as number)
    cdcc.write(ctx, 'count', after)
    cdcc.output(ctx, 'result', after)
  }
  undo(ctx: Context) {
    cdcc.write(ctx, 'count', this.before)
  }
}

/**
 * Imports the Emscripten module at `url` and instantiates it.
 *
 * With ASSERTIONS (Debug), emscripten's startup check throws "not compiled for this
 * environment" whenever `process.versions.node` exists, as it does in NW.js. The check
 * runs synchronously inside the factory call, so `process` is hidden for that call only.
 */
async function instantiate(url: string): Promise<object> {
  const factory = (await import(url)).default as () => Promise<object>
  const g = globalThis as {process?: unknown}
  const proc = g.process
  let pending: Promise<object>
  delete g.process
  try {
    pending = factory()
  } finally {
    g.process = proc
  }
  return await pending
}

/** A document: the host and the Session over its context. */
interface Doc {
  host: Host
  session: Session
}

export interface BackendInfo {
  backend: 'wasm-st'
  module: string
}

/** Loads the module at `modulePath` and registers the TA1 testbus methods. */
export async function install(register: Register, modulePath: string): Promise<BackendInfo> {
  const {pathToFileURL} = (globalThis as any).require('url') as typeof import('node:url')
  const module = await instantiate(pathToFileURL(modulePath).href)
  /* Once per page: load() refuses a second module, so reset() only replaces the doc. */
  const backend: IBindingBackend = cdcc.load(module, 'TESTAPP_GetManager')

  const open = (): Doc => {
    const host = ta.create() as Host
    const session = new Session(host, ta.context(host) as Context)
    cdcc.registerCommand(session.ctx, {id: 'testapp.addCount', inputs: {amount: 1}, outputs: {result: 0}}, AddCount)
    return {host, session}
  }
  let doc = open()
  const ctx = () => doc.session.ctx

  /* ScriptErrors cross the wire as {name: 'ScriptError', code: 'NotFound', ...}. */
  const method = (name: string, fn: (...args: any[]) => unknown) =>
    register(name, (...args: any[]) => {
      try {
        return fn(...args)
      } catch (e) {
        if (!(e instanceof ScriptError)) throw e
        const out = new Error(e.message) as Error & {code?: string; detail?: string}
        out.name = 'ScriptError'
        out.code = StatusCode[e.code] ?? String(e.code)
        out.detail = e.detail
        throw out
      }
    })

  /* A value without a JS form (a pointer) crosses as its text; the copy is disposed of. */
  const wireValue = (v: Value): unknown => {
    if (typeof v !== 'object' || v === null || Array.isArray(v)) return v
    const pv = v as PropValue
    try {
      return {text: cd.valueToString(pv)}
    } finally {
      pv[Symbol.dispose]()
    }
  }

  method('resolve', (path: string) => {
    cdcc.resolve(ctx(), path)
    return true
  })
  method('get', (path: string) => wireValue(cdcc.get(ctx(), path)))
  method('set', (path: string, value: cdcc.Writable) => {
    cdcc.set(ctx(), path, value)
    return true
  })
  method('exec', (invocation: string) => {
    const out = cdcc.invoke(ctx(), invocation)
    const wired: Record<string, unknown> = {}
    for (const [k, v] of Object.entries(out)) {
      Object.defineProperty(wired, k, {value: wireValue(v), enumerable: true, writable: true, configurable: true})
    }
    return wired
  })
  method('undo', () => {
    cdcc.undo(ctx())
    return true
  })
  method('redo', () => {
    cdcc.redo(ctx())
    return true
  })
  method('history', () => ({
    size: ta.historySize(doc.host),
    canUndo: cdcc.canUndo(ctx()),
    canRedo: cdcc.canRedo(ctx()),
  }))
  /* Replaces the document; returns the foreign handles left between the two (0 unless a
   * script command leaked one). */
  method('reset', () => {
    doc.session[Symbol.dispose]()
    const leftOver = backend.foreignCount()
    doc = open()
    return {foreignCountBetween: leftOver}
  })
  method('check', () => {
    const st = ta.validate(doc.host)
    try {
      return {
        valid: st.ok(),
        message: st.ok() ? '' : cd.statusText(st),
        liveInstances: ta.liveInstances(doc.host),
        foreignCount: backend.foreignCount(),
      }
    } finally {
      st[Symbol.dispose]()
    }
  })
  /* TA1 tests register their own TS commands from source text; see ta1.test.ts. */
  method('defineCommand', (id: string, spec: Omit<cdcc.CommandSpec, 'id'>, classSource: string) => {
    const Cls = new Function('cdcc', `return (${classSource})`)(cdcc) as new () => ScriptCommand
    cdcc.registerCommand(ctx(), {...spec, id}, Cls)
    return true
  })

  return {backend: 'wasm-st', module: modulePath}
}
