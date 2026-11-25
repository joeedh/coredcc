/**
 * The testbus: `globalThis.coredccTest.$call(method, encodedArgs) -> encodedReply`.
 *
 * Arguments and replies use the wire encoding in `../wire.mjs` (JSON strings with tags
 * for bigint, NaN, -0, undefined, typed arrays...), because CDP's returnByValue mangles
 * those. Every method's result or thrown error becomes `{ok, value}` / `{ok: false, error}`.
 */
import {PROTOCOL_VERSION, decodeReply, encodeReply, errorInfo} from '../wire.mjs'
import {adapterReport} from './gpu.ts'
import {argValue, isNw, nodeRequire} from './node.ts'

type Method = (...args: any[]) => unknown

const methods = new Map<string, Method>()

/**
 * What `info()` reports about the coredcc backend (src/main.ts sets it):
 * - `backend`: 'wasm-st', or 'none';
 * - `backendMissing`: the build has no backend (TA1's tests skip);
 * - `backendError`: why there is none, or what killed it.
 */
export interface BackendReport {
  backend: string
  backendMissing?: boolean
  backendError?: string
  module?: string
}
let backendReport: BackendReport = {backend: 'none'}

export function setBackendInfo(report: BackendReport): void {
  backendReport = report
}

export function registerMethod(name: string, fn: Method): void {
  if (methods.has(name)) throw new Error(`testbus method '${name}' is already registered`)
  methods.set(name, fn)
}

async function dispatch(name: string, encodedArgs: string): Promise<string> {
  try {
    const fn = methods.get(name)
    if (!fn) throw new Error(`unknown testbus method '${name}'`)
    const decoded = decodeReply(encodedArgs) as {value: unknown[]}
    const value = await fn(...decoded.value)
    return encodeReply({ok: true, value})
  } catch (err) {
    return encodeReply({ok: false, error: errorInfo(err)})
  }
}

function capturePng(): Promise<string> {
  return new Promise((resolve, reject) => {
    if (!isNw) {
      reject(new Error('shot() needs NW.js'))
      return
    }
    const win = (globalThis as any).nw.Window.get()
    win.capturePage(
      (dataUri: string) => {
        const comma = dataUri.indexOf(',')
        if (comma < 0) reject(new Error('capturePage returned no data'))
        else resolve(dataUri.slice(comma + 1))
      },
      {format: 'png', datatype: 'datauri'},
    )
  })
}

function registerCoreMethods(): void {
  registerMethod('info', () => {
    const proc = (globalThis as any).process
    return {
      protocol: PROTOCOL_VERSION,
      pid: proc?.pid,
      browserPid: proc?.ppid,
      profile: argValue('user-data-dir'),
      ...backendReport,
      build: 'ta2',
      devicePixelRatio: window.devicePixelRatio,
      viewport: {width: window.innerWidth, height: window.innerHeight},
      adapter: adapterReport(),
    }
  })

  // Uses NW.js capturePage: CDP Page.captureScreenshot hangs in a never-shown window.
  registerMethod('shot', async (opts: {outPath?: string} = {}) => {
    const base64 = await capturePng()
    if (!opts.outPath) return {png: base64}
    const fs = nodeRequire<typeof import('node:fs')>('fs')!
    const bytes = Uint8Array.from(atob(base64), (c) => c.charCodeAt(0))
    fs.writeFileSync(opts.outPath, bytes)
    return {path: opts.outPath, bytes: bytes.length}
  })

  registerMethod('echo', (value: unknown) => value)
  registerMethod('bigString', (n: number) => 'x'.repeat(n))
  registerMethod('log', (level: 'log' | 'warn' | 'error', text: string) => {
    console[level](text)
    return true
  })
  registerMethod('quit', () => {
    // Reply first, then quit, so the caller sees ok rather than a closed socket.
    setTimeout(() => (globalThis as any).nw?.App.quit(), 0)
    return true
  })
}

export function installTestbus(): void {
  registerCoreMethods()
  ;(globalThis as any).coredccTest = {$call: dispatch, methods: () => [...methods.keys()]}
}
