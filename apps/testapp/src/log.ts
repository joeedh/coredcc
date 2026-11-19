/**
 * Mirrors console output into the launcher-provided log file (`--apptest-log=PATH`),
 * one synchronous append per line, so it survives a renderer crash. NW.js renderer
 * stdout/stderr never reach the launcher, so this and CDP are the only log channels.
 */
import {argValue, nodeRequire} from './node.ts'

let logPath: string | undefined
let fs: typeof import('node:fs') | undefined

function fmt(args: unknown[]): string {
  return args
    .map((a) => {
      if (typeof a === 'string') return a
      if (a instanceof Error) return a.stack ?? a.message
      try {
        return JSON.stringify(a, (_k, v) => (typeof v === 'bigint' ? `${v}n` : v))
      } catch {
        return String(a)
      }
    })
    .join(' ')
}

/** Append one line to the log file (no-op outside NW.js or without --apptest-log). */
export function logLine(level: string, text: string): void {
  if (!fs || !logPath) return
  try {
    fs.appendFileSync(logPath, `${new Date().toISOString()} [${level}] ${text}\n`)
  } catch {
    /* never let logging throw */
  }
}

export function installLogMirror(): void {
  logPath = argValue('apptest-log')
  fs = nodeRequire<typeof import('node:fs')>('fs')
  for (const level of ['log', 'info', 'warn', 'error', 'debug'] as const) {
    const orig = console[level].bind(console)
    console[level] = (...args: unknown[]) => {
      logLine(level, fmt(args))
      orig(...args)
    }
  }
  window.addEventListener('error', (e) => logLine('uncaught', fmt([e.error ?? e.message])))
  window.addEventListener('unhandledrejection', (e) => logLine('unhandled', fmt([e.reason])))
}
