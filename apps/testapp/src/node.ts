/**
 * Access to NW.js's Node context from the bundled page. `require` is a global in an NW.js
 * window; going through globalThis keeps esbuild from rewriting it. In a plain browser
 * these return undefined.
 */

export const isNw: boolean = typeof (globalThis as any).nw !== 'undefined'

export function nodeRequire<T = any>(id: string): T | undefined {
  const req = (globalThis as any).require
  return typeof req === 'function' ? (req(id) as T) : undefined
}

/** App arguments (NW.js also leaks Chromium switches here; unknown ones are ignored). */
export function appArgv(): string[] {
  return isNw ? ((globalThis as any).nw.App.argv as string[]) : []
}

export function argValue(name: string): string | undefined {
  const prefix = `--${name}=`
  return appArgv().find((a) => a.startsWith(prefix))?.slice(prefix.length)
}

export function hasArg(name: string): boolean {
  return appArgv().some((a) => a === `--${name}` || a.startsWith(`--${name}=`))
}
