#!/usr/bin/env node
/**
 * Dependency-free CDP client for the test app (Node 22+: global fetch and WebSocket).
 *
 * Library:
 *   const c = await CdpClient.connect(port)
 *   await c.waitReady()
 *   const info = await c.call('info')      // {ok, value} | {ok: false, error}
 *
 * CLI (port from --port, else --profile's DevToolsActivePort, else this worktree's
 * default manual profile):
 *   node apps/testapp/cdp.mjs [--port N | --profile DIR] list
 *   node apps/testapp/cdp.mjs ... call <method> [jsonArg...]
 *   node apps/testapp/cdp.mjs ... eval "<js expression>"
 *   node apps/testapp/cdp.mjs ... shot out.png
 *   node apps/testapp/cdp.mjs ... logs            (stream console output until Ctrl+C)
 *
 * Every failure (CDP error, page exception, closed socket, timeout) becomes an
 * `{ok: false, error}` reply from `call`, never an exception.
 */
import fs from 'node:fs'
import path from 'node:path'
import {fileURLToPath} from 'node:url'
import {decodeReply, encodeReply} from './wire.mjs'
import {worktreeProfileDir} from './nwjs/profile_dir.mjs'

const DEFAULT_TIMEOUT_MS = 30000

function sleep(ms) {
  return new Promise((r) => setTimeout(r, ms))
}

export class CdpClient {
  /** @param {WebSocket} ws */
  constructor(ws) {
    this.ws = ws
    this.nextId = 1
    this.pending = new Map()
    this.listeners = new Map()
    /** Console, exception and log entries recorded after `recordLogs()`. */
    this.logs = []
    this.closed = false
    ws.addEventListener('message', (ev) => this.#onMessage(ev))
    ws.addEventListener('close', () => this.#onClose())
  }

  /** Connect to the app's page target on `port`, retrying until it appears. */
  static async connect(port, {timeoutMs = 20000} = {}) {
    const deadline = Date.now() + timeoutMs
    let lastErr
    while (Date.now() < deadline) {
      try {
        const res = await fetch(`http://127.0.0.1:${port}/json/list`)
        const targets = await res.json()
        const page = targets.find((t) => t.type === 'page' && t.url.includes('window.html'))
        if (page) {
          const ws = new WebSocket(page.webSocketDebuggerUrl)
          await new Promise((resolve, reject) => {
            ws.addEventListener('open', resolve, {once: true})
            ws.addEventListener('error', () => reject(new Error('CDP WebSocket failed to open')), {
              once: true,
            })
          })
          return new CdpClient(ws)
        }
        lastErr = new Error('no page target yet')
      } catch (err) {
        lastErr = err
      }
      await sleep(100)
    }
    throw new Error(`could not connect to CDP on port ${port}: ${lastErr?.message}`)
  }

  #onMessage(ev) {
    const msg = JSON.parse(ev.data)
    if (msg.id !== undefined) {
      const p = this.pending.get(msg.id)
      if (!p) return
      this.pending.delete(msg.id)
      clearTimeout(p.timer)
      if (msg.error) p.reject(new Error(`CDP ${msg.error.code}: ${msg.error.message}`))
      else p.resolve(msg.result)
      return
    }
    for (const cb of this.listeners.get(msg.method) ?? []) cb(msg.params)
  }

  #onClose() {
    this.closed = true
    for (const p of this.pending.values()) {
      clearTimeout(p.timer)
      p.reject(new Error('CDP WebSocket closed (the app exited or its renderer crashed)'))
    }
    this.pending.clear()
    for (const cb of this.listeners.get('$close') ?? []) cb()
  }

  /** Subscribe to a CDP event (or '$close'). */
  on(method, cb) {
    if (!this.listeners.has(method)) this.listeners.set(method, [])
    this.listeners.get(method).push(cb)
  }

  /** Send a raw CDP command. Rejects on CDP error, timeout or a closed socket. */
  send(method, params = {}, timeoutMs = DEFAULT_TIMEOUT_MS) {
    if (this.closed) return Promise.reject(new Error('CDP WebSocket closed'))
    const id = this.nextId++
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        this.pending.delete(id)
        reject(new Error(`CDP ${method} timed out after ${timeoutMs} ms`))
      }, timeoutMs)
      this.pending.set(id, {resolve, reject, timer})
      this.ws.send(JSON.stringify({id, method, params}))
    })
  }

  /** Evaluate an expression in the page; rejects on a page exception. */
  async evaluate(expression, {timeoutMs = DEFAULT_TIMEOUT_MS, returnByValue = true} = {}) {
    const res = await this.send(
      'Runtime.evaluate',
      {expression, awaitPromise: true, returnByValue},
      timeoutMs,
    )
    if (res.exceptionDetails) {
      const d = res.exceptionDetails
      throw new Error(d.exception?.description ?? d.text ?? 'page exception')
    }
    return res.result
  }

  /** Wait until the page has installed the testbus. */
  async waitReady({timeoutMs = 20000} = {}) {
    const deadline = Date.now() + timeoutMs
    while (Date.now() < deadline) {
      try {
        const r = await this.evaluate('typeof globalThis.coredccTest?.$call === "function"', {
          timeoutMs: 2000,
        })
        if (r.value === true) return
      } catch {
        /* page still loading */
      }
      await sleep(50)
    }
    throw new Error('testbus did not become ready')
  }

  /** Call a testbus method. Always resolves to `{ok, value}` or `{ok: false, error}`. */
  async call(method, args = [], {timeoutMs = DEFAULT_TIMEOUT_MS} = {}) {
    try {
      const encodedArgs = encodeReply({ok: true, value: args})
      const expr = `globalThis.coredccTest.$call(${JSON.stringify(method)}, ${JSON.stringify(encodedArgs)})`
      const r = await this.evaluate(expr, {timeoutMs})
      if (typeof r.value !== 'string') {
        return {ok: false, error: {name: 'ProtocolError', message: `non-string reply: ${r.type}`}}
      }
      return decodeReply(r.value)
    } catch (err) {
      return {ok: false, error: {name: 'CdpError', message: err.message}}
    }
  }

  /** Start recording console calls, exceptions and log entries into `this.logs`. */
  async recordLogs(sink) {
    const push = (entry) => {
      this.logs.push(entry)
      sink?.(entry)
    }
    this.on('Runtime.consoleAPICalled', (p) =>
      push({
        kind: 'console',
        level: p.type,
        text: p.args.map((a) => (a.value !== undefined ? String(a.value) : a.description)).join(' '),
      }),
    )
    this.on('Runtime.exceptionThrown', (p) =>
      push({
        kind: 'exception',
        level: 'error',
        text: p.exceptionDetails.exception?.description ?? p.exceptionDetails.text,
      }),
    )
    this.on('Log.entryAdded', (p) => push({kind: 'log', level: p.entry.level, text: p.entry.text}))
    await this.send('Runtime.enable')
    await this.send('Log.enable')
  }

  close() {
    if (!this.closed) this.ws.close()
  }
}

/** Read the CDP port from a profile's DevToolsActivePort. */
export function portFromProfile(profileDir) {
  const text = fs.readFileSync(path.join(profileDir, 'DevToolsActivePort'), 'utf8')
  return Number.parseInt(text.split('\n')[0], 10)
}

async function main(argv) {
  let port
  let profile
  while (argv[0]?.startsWith('--')) {
    const a = argv.shift()
    if (a === '--port') port = Number(argv.shift())
    else if (a === '--profile') profile = argv.shift()
    else throw new Error(`unknown option ${a}`)
  }
  if (!port) {
    const repoRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..', '..')
    port = portFromProfile(profile ?? worktreeProfileDir(repoRoot))
  }
  const cmd = argv.shift()
  if (cmd === 'list') {
    console.log(JSON.stringify(await (await fetch(`http://127.0.0.1:${port}/json/list`)).json(), null, 2))
    return
  }
  const c = await CdpClient.connect(port)
  try {
    if (cmd === 'eval') {
      console.log(JSON.stringify((await c.evaluate(argv.join(' '))).value, null, 2))
    } else if (cmd === 'call') {
      await c.waitReady()
      const method = argv.shift()
      const reply = await c.call(method, argv.map((a) => JSON.parse(a)))
      console.log(reply)
      process.exitCode = reply.ok ? 0 : 1
    } else if (cmd === 'shot') {
      await c.waitReady()
      const out = path.resolve(argv[0] ?? 'shot.png')
      const reply = await c.call('shot', [{outPath: out}])
      console.log(reply.ok ? `wrote ${out}` : reply.error)
      process.exitCode = reply.ok ? 0 : 1
    } else if (cmd === 'logs') {
      await c.recordLogs((e) => console.log(`[${e.level}] ${e.text}`))
      await new Promise((r) => c.on('$close', r))
    } else {
      throw new Error(`unknown command ${cmd}`)
    }
  } finally {
    c.close()
  }
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  main(process.argv.slice(2)).catch((err) => {
    console.error(err.message)
    process.exit(1)
  })
}
