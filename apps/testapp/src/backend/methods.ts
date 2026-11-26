/**
 * The testbus methods over a coredcc backend (TA1, docs/plans/2026-11-20-p1-application-spine.md
 * "P1f step 6"), shared by every backend: a loader (`wasm.ts`, later `native.ts`) makes
 * its module the active binding backend and hands this a `LoadedBackend`.
 *
 * The page holds one document as a `Session`; methods see only its context.
 */
import * as cdcc from 'coredcc-script'
import {ScriptError, Session, StatusCode, type Context, type ScriptCommand} from 'coredcc-script'
import * as cd from 'coredcc-gen/coredcc/functions'
import type {Host} from 'coredcc-gen/testapp/Host'
import * as ta from 'coredcc-gen/testapp/functions'
import type {IBindingBackend} from '@litestl/typescript-runtime'
import type {GpuHooks} from '../overlay/renderer.ts'
import {installPanelMethods} from './panel_methods.ts'
import {installToolMethods} from './tool_methods.ts'

export type Register = (name: string, fn: (...args: any[]) => unknown) => void

/** What a loader hands the methods. */
export interface LoadedBackend {
  /** `info().backend`: 'wasm-st', later 'native'. */
  kind: string
  /** The module file it loaded. */
  module: string
  backend: IBindingBackend
  /** True for an error that killed the module (a trap, an abort, an exit). */
  isFatal(e: unknown): boolean
  /** The module's last few stderr lines, for a fatal error's report. */
  recentStderr(): string[]
}

/** What `info()` reports beyond the loader's kind and module. */
export interface BackendState {
  backendError?: string
}

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

/** `testapp.weigh(by=1)`: adds `by` to the `weight` under the path prefix, as a panel's tool. */
class Weigh implements ScriptCommand {
  before = 0
  exec(ctx: Context) {
    this.before = cdcc.get(ctx, 'weight') as number
    cdcc.write(ctx, 'weight', this.before + (cdcc.input(ctx, 'by') as number))
  }
  undo(ctx: Context) {
    cdcc.write(ctx, 'weight', this.before)
  }
}

/** A document: the host and the Session over its context. */
interface Doc {
  host: Host
  session: Session
}

/** Opens a document with the page's commands registered; nothing leaks if that fails. */
function openDoc(): Doc {
  const host = ta.create() as Host
  const session = new Session(host, ta.context(host) as Context)
  try {
    cdcc.defineCommand(session.ctx, {id: 'testapp.addCount', inputs: {amount: 1}, outputs: {result: 0}}, AddCount)
    cdcc.defineCommand(session.ctx, {id: 'testapp.weigh', inputs: {by: cdcc.float(1)}}, Weigh)
  } catch (e) {
    session[Symbol.dispose]()
    throw e
  }
  return {host, session}
}

/* A Ref crosses the wire as `{$ref: text}`, both ways. */
const wireValue = (v: cdcc.NamedValue): unknown => (v instanceof cdcc.Ref ? {$ref: v.text} : v)
function unwireValue(v: unknown): cdcc.Writable {
  if (v !== null && typeof v === 'object' && !Array.isArray(v) && '$ref' in v) {
    return new cdcc.Ref((v as {$ref: unknown}).$ref as string)
  }
  return v as cdcc.Writable
}

/* `{$float: 1}` in a spec crosses the wire as a Float default: `float()` itself can't. */
function specValues(values: Record<string, unknown> | undefined): Record<string, cdcc.Writable> | undefined {
  if (!values) return undefined
  const out: Record<string, cdcc.Writable> = {}
  for (const [k, v] of Object.entries(values)) {
    const f = v !== null && typeof v === 'object' && '$float' in v ? cdcc.float(Number((v as {$float: unknown}).$float)) : v
    Object.defineProperty(out, k, {value: f, enumerable: true, writable: true, configurable: true})
  }
  return out
}

/**
 * Registers the testbus methods over `loaded`. `report` updates what `info()` says about
 * the backend; it is called when the module dies.
 */
export function installMethods(
  register: Register,
  loaded: LoadedBackend,
  report: (s: BackendState) => void,
  gpu?: GpuHooks
) {
  let doc: Doc | undefined = openDoc()
  let docError = ''
  let dead: string | undefined

  const current = (): Doc => {
    if (!doc) throw new Error(`no document: ${docError}`)
    return doc
  }
  const ctx = () => current().session.ctx

  /*
   * Every method: a dead module refuses at once, with what killed it. A ScriptError
   * crosses as {name: 'ScriptError', code: 'NotFound', detail, ...}.
   */
  const method = (name: string, fn: (...args: any[]) => unknown) =>
    register(name, (...args: any[]) => {
      if (dead) throw new Error(`the ${loaded.kind} module aborted: ${dead}`)
      try {
        return fn(...args)
      } catch (e) {
        if (loaded.isFatal(e)) {
          const stderr = loaded.recentStderr().join('\n')
          dead = `${name}: ${e instanceof Error ? e.message : String(e)}${stderr ? `\n${stderr}` : ''}`
          report({backendError: `the module aborted: ${dead}`})
          throw new Error(`the ${loaded.kind} module aborted: ${dead}`)
        }
        if (!(e instanceof ScriptError)) throw e
        const out = new Error(e.message) as Error & {code?: string; detail?: string}
        out.name = 'ScriptError'
        out.code = StatusCode[e.code] ?? String(e.code)
        out.detail = e.detail
        out.stack = e.stack
        throw out
      }
    })

  method('resolve', (path: string) => cdcc.resolve(ctx(), path))
  method('meta', (path: string) => cdcc.meta(ctx(), path))
  method('get', (path: string, opts?: {names?: boolean}) => wireValue(cdcc.get(ctx(), path, opts ?? {})))
  method('set', (path: string, value: unknown, gesture?: number) => {
    cdcc.set(ctx(), path, unwireValue(value), gesture)
    return true
  })
  method('beginGesture', () => cdcc.beginGesture(ctx()))
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
    size: ta.historySize(current().host),
    canUndo: cdcc.canUndo(ctx()),
    canRedo: cdcc.canRedo(ctx()),
  }))
  /* Replaces the document. Returns the foreign handles left between the two: 0 unless a
   * script command leaked one. */
  method('reset', () => {
    closePanel()
    closeTools()
    if (doc) {
      doc.session[Symbol.dispose]()
      doc = undefined
    }
    const leftOver = loaded.backend.foreignCount()
    try {
      doc = openDoc()
    } catch (e) {
      docError = `reset failed: ${e instanceof Error ? e.message : String(e)}`
      throw e
    }
    return {foreignCountBetween: leftOver}
  })
  /* `foreignCount` includes the page's own command factories: 2 with nothing else live. */
  method('check', () => {
    const d = current()
    const st = ta.validate(d.host)
    try {
      return {
        valid: st.ok(),
        message: st.ok() ? '' : cd.statusText(st),
        liveInstances: ta.liveInstances(d.host),
        foreignCount: loaded.backend.foreignCount(),
      }
    } finally {
      st[Symbol.dispose]()
    }
  })
  /* Registers a TS command from source text: `classSource` evaluates to a class, with
   * `cdcc` (coredcc's TS layer) in scope. `{$float: n}` in inputs or outputs is a Float. */
  method('defineCommand', (id: string, spec: Omit<cdcc.CommandSpec, 'id'>, classSource: string) => {
    const Cls = new Function('cdcc', `return (${classSource})`)(cdcc) as new () => ScriptCommand
    cdcc.defineCommand(
      ctx(),
      {...spec, id, inputs: specValues(spec.inputs), outputs: specValues(spec.outputs)},
      Cls
    )
    return true
  })
  /* Changes `strength` in C++, behind the page's back. */
  method('nudge', () => {
    ta.nudge(current().host)
    return true
  })
  method('watched', () => ta.watched(current().host))
  const closePanel = installPanelMethods(method, ctx)
  const closeTools = installToolMethods(method, ctx, gpu)
  /* Kills the module, for tests of how a dead backend is reported. */
  method('abortForTest', () => {
    ta.abortForTest()
    return true
  })
}
